// 架构:
//
//   客户端 (SigV4 签名) ──▶ API Gateway HTTP API (IAM 鉴权、限流，把调用者账号写进 X-Advertiser-Id)
//                               │ VPC Link
//   ┌──────────────── VPC (只有私有隔离子网，没有 NAT、没有公网出口) ────────────────┐
//   │  内部 ALB ──▶ ECS Fargate (ARM64, 2 个任务, C++ ads_api) ──▶ RDS PostgreSQL     │
//   │                     │                                                         │
//   │                     ├── Gateway Endpoint ──▶ DynamoDB (KMS CMK 加密, PITR, TTL) │
//   │                     └── Interface Endpoints ──▶ ECR / CloudWatch Logs / Secrets │
//   └─────────────────────────────────────────────────────────────────────────────────┘
//   广告服务器 (在 VPC 内) 直接调 ALB 的 /charges；这个接口不经过 API Gateway，不对公网开放
import * as path from 'path';
import * as cdk from 'aws-cdk-lib';
import { Construct } from 'constructs';
import * as apigw from 'aws-cdk-lib/aws-apigateway';
import * as apigwv2 from 'aws-cdk-lib/aws-apigatewayv2';
import { HttpIamAuthorizer } from 'aws-cdk-lib/aws-apigatewayv2-authorizers';
import { HttpAlbIntegration } from 'aws-cdk-lib/aws-apigatewayv2-integrations';
import * as cloudwatch from 'aws-cdk-lib/aws-cloudwatch';
import * as dynamodb from 'aws-cdk-lib/aws-dynamodb';
import * as ec2 from 'aws-cdk-lib/aws-ec2';
import * as ecrAssets from 'aws-cdk-lib/aws-ecr-assets';
import * as ecs from 'aws-cdk-lib/aws-ecs';
import * as elbv2 from 'aws-cdk-lib/aws-elasticloadbalancingv2';
import * as iam from 'aws-cdk-lib/aws-iam';
import * as kms from 'aws-cdk-lib/aws-kms';
import * as logs from 'aws-cdk-lib/aws-logs';
import * as rds from 'aws-cdk-lib/aws-rds';

// Checkov 豁免写在资源自己的 CloudFormation Metadata 里：理由和代码放在一起，只对这一个资源生效
type Skip = [id: string, reason: string];
function checkovSkip(c: Construct, ...checks: Skip[]) {
  const cfn = (c.node.defaultChild ?? c) as cdk.CfnResource;
  cfn.addMetadata('checkov', { skip: checks.map(([id, comment]) => ({ id, comment })) });
}

export class AdsStack extends cdk.Stack {
  constructor(scope: Construct, id: string, props?: cdk.StackProps) {
    super(scope, id, props);
    // side project: 删除栈时一起删数据，避免忘记清理继续扣费。生产环境要改成 RETAIN
    const removal = cdk.RemovalPolicy.DESTROY;

    // ---------- 加密 ----------
    const key = new kms.Key(this, 'DataKey', {
      description: 'ads-campaign-budget data at rest (DynamoDB, RDS)',
      enableKeyRotation: true,
      pendingWindow: cdk.Duration.days(7),
      removalPolicy: removal,
    });
    // CloudWatch Logs 要用这把 key 加密日志，必须在 key policy 里授权给 logs 服务 (限定本账号的日志组)
    key.addToResourcePolicy(new iam.PolicyStatement({
      principals: [new iam.ServicePrincipal(`logs.${this.region}.amazonaws.com`)],
      actions: ['kms:Encrypt*', 'kms:Decrypt*', 'kms:ReEncrypt*', 'kms:GenerateDataKey*', 'kms:Describe*'],
      resources: ['*'],
      conditions: { ArnLike: { 'kms:EncryptionContext:aws:logs:arn': `arn:${this.partition}:logs:${this.region}:${this.account}:*` } },
    }));

    // ---------- 网络 ----------
    // 没有 NAT Gateway: 任务访问 AWS 服务全部走 VPC Endpoint，流量不出 AWS 网络，也没有公网出口可被利用
    const vpc = new ec2.Vpc(this, 'Vpc', {
      maxAzs: 2,
      natGateways: 0,
      subnetConfiguration: [{ name: 'app', subnetType: ec2.SubnetType.PRIVATE_ISOLATED, cidrMask: 24 }],
    });
    const isolated = { subnetType: ec2.SubnetType.PRIVATE_ISOLATED };
    // Gateway Endpoint 免费；Interface Endpoint 按 AZ × 小时收费
    vpc.addGatewayEndpoint('DynamoDbEndpoint', { service: ec2.GatewayVpcEndpointAwsService.DYNAMODB });
    vpc.addGatewayEndpoint('S3Endpoint', { service: ec2.GatewayVpcEndpointAwsService.S3 });   // ECR 镜像层存在 S3
    const interfaceEndpoints: Record<string, ec2.InterfaceVpcEndpointAwsService> = {
      EcrApiEndpoint: ec2.InterfaceVpcEndpointAwsService.ECR,
      EcrDockerEndpoint: ec2.InterfaceVpcEndpointAwsService.ECR_DOCKER,
      LogsEndpoint: ec2.InterfaceVpcEndpointAwsService.CLOUDWATCH_LOGS,
      SecretsEndpoint: ec2.InterfaceVpcEndpointAwsService.SECRETS_MANAGER,
    };
    // 安全组: 每一层只允许上一层进来 (VPC Link → ALB → 任务 → 数据库)，出站也只开需要的端口
    const vpcLinkSg = new ec2.SecurityGroup(this, 'VpcLinkSg', { vpc, allowAllOutbound: false, description: 'API Gateway VPC link' });
    const albSg = new ec2.SecurityGroup(this, 'AlbSg', { vpc, allowAllOutbound: false, description: 'internal ALB' });
    const taskSg = new ec2.SecurityGroup(this, 'TaskSg', { vpc, allowAllOutbound: false, description: 'ads_api tasks' });
    const dbSg = new ec2.SecurityGroup(this, 'DbSg', { vpc, allowAllOutbound: false, description: 'ledger database' });
    const endpointSg = new ec2.SecurityGroup(this, 'EndpointSg', { vpc, allowAllOutbound: false, description: 'interface VPC endpoints' });

    // Interface Endpoint 只接受任务的 HTTPS (默认是整个 VPC CIDR 都能连、出站全开)
    for (const [name, service] of Object.entries(interfaceEndpoints)) {
      vpc.addInterfaceEndpoint(name, { service, subnets: isolated, privateDnsEnabled: true, securityGroups: [endpointSg], open: false });
    }
    endpointSg.addIngressRule(taskSg, ec2.Port.tcp(443), 'from tasks only');
    // 任务出站: 443 去 AWS API (Interface Endpoint 在 VPC 内；DynamoDB/S3 走 Gateway Endpoint，目的地址是 AWS 公网段，
    // 所以目标只能写 0.0.0.0/0 —— 但 VPC 没有 NAT/IGW，实际只能到达 Endpoint)；5432 只去数据库
    taskSg.addEgressRule(ec2.Peer.anyIpv4(), ec2.Port.tcp(443), 'AWS APIs via VPC endpoints (no NAT/IGW route)');
    taskSg.addEgressRule(dbSg, ec2.Port.tcp(5432), 'ledger database');
    albSg.addIngressRule(vpcLinkSg, ec2.Port.tcp(80), 'from API Gateway');
    albSg.addIngressRule(ec2.Peer.ipv4(vpc.vpcCidrBlock), ec2.Port.tcp(80), 'ad servers inside the VPC');
    vpcLinkSg.addEgressRule(albSg, ec2.Port.tcp(80));
    albSg.addEgressRule(taskSg, ec2.Port.tcp(8080));
    taskSg.addIngressRule(albSg, ec2.Port.tcp(8080), 'from ALB only');
    dbSg.addIngressRule(taskSg, ec2.Port.tcp(5432), 'from tasks only');

    // ---------- DynamoDB (单表) ----------
    const table = new dynamodb.TableV2(this, 'Table', {
      partitionKey: { name: 'PK', type: dynamodb.AttributeType.STRING },
      sortKey: { name: 'SK', type: dynamodb.AttributeType.STRING },
      billing: dynamodb.Billing.onDemand(),   // 按请求计费: 流量小的时候几乎不花钱，突发流量也不用预估容量
      encryption: dynamodb.TableEncryptionV2.customerManagedKey(key),
      pointInTimeRecoverySpecification: { pointInTimeRecoveryEnabled: true },
      timeToLiveAttribute: 'expires_at',      // 幂等记录 24h、每日预算计数器 7 天后自动删除
      removalPolicy: removal,
    });

    // ---------- RDS PostgreSQL (记账流水) ----------
    const db = new rds.DatabaseInstance(this, 'LedgerDb', {
      engine: rds.DatabaseInstanceEngine.postgres({ version: rds.PostgresEngineVersion.VER_16 }),
      instanceType: ec2.InstanceType.of(ec2.InstanceClass.T4G, ec2.InstanceSize.MICRO),
      vpc,
      vpcSubnets: isolated,
      securityGroups: [dbSg],
      credentials: rds.Credentials.fromGeneratedSecret('ads', { encryptionKey: key }),   // 密码自动生成，存 Secrets Manager (CMK 加密)
      databaseName: 'ads',
      allocatedStorage: 20,
      storageEncrypted: true,
      storageEncryptionKey: key,
      multiAz: false,                         // 生产要开；side project 省钱
      backupRetention: cdk.Duration.days(1),
      deletionProtection: false,
      publiclyAccessible: false,
      removalPolicy: removal,
    });
    checkovSkip(db,
      ['CKV_AWS_157', 'Multi-AZ doubles cost; side project accepts single-AZ (enable in prod)'],
      ['CKV_AWS_118', 'enhanced monitoring adds cost; CloudWatch basic metrics + Container Insights are enough here'],
      ['CKV_AWS_161', 'app authenticates with the Secrets Manager password; IAM auth needs token refresh in libpq (not implemented)']);

    // ---------- ECS Fargate ----------
    const cluster = new ecs.Cluster(this, 'Cluster', { vpc, containerInsightsV2: ecs.ContainerInsights.ENABLED });
    const image = new ecrAssets.DockerImageAsset(this, 'ApiImage', {
      directory: path.join(__dirname, '../../..'),   // 仓库根目录 (Dockerfile 需要 common/)
      file: 'ads-api/docker/Dockerfile',
      platform: ecrAssets.Platform.LINUX_ARM64,       // Graviton: 同样性能比 x86 便宜约 20%
    });
    const logGroup = new logs.LogGroup(this, 'ApiLogs', { retention: logs.RetentionDays.TWO_WEEKS, encryptionKey: key, removalPolicy: removal });

    const task = new ecs.FargateTaskDefinition(this, 'Task', {
      cpu: 512,
      memoryLimitMiB: 1024,
      runtimePlatform: { cpuArchitecture: ecs.CpuArchitecture.ARM64, operatingSystemFamily: ecs.OperatingSystemFamily.LINUX },
    });
    const dbSecret = db.secret!;
    task.addContainer('api', {
      image: ecs.ContainerImage.fromDockerImageAsset(image),
      portMappings: [{ containerPort: 8080 }],
      environment: { TABLE_NAME: table.tableName, AWS_REGION: this.region, PGDATABASE: 'ads', PGSSLMODE: 'require' },
      // 数据库凭证由 ECS 从 Secrets Manager 取出来注入成环境变量；libpq 直接读 PG* 变量，代码里没有密码
      secrets: {
        PGHOST: ecs.Secret.fromSecretsManager(dbSecret, 'host'),
        PGUSER: ecs.Secret.fromSecretsManager(dbSecret, 'username'),
        PGPASSWORD: ecs.Secret.fromSecretsManager(dbSecret, 'password'),
      },
      logging: ecs.LogDrivers.awsLogs({ streamPrefix: 'api', logGroup }),
      stopTimeout: cdk.Duration.seconds(30),   // SIGTERM 后 30 秒内把预算租约还回去
    });

    // 最小权限: 只给代码真正用到的 5 个动作，只针对这一张表 (没有 Scan、DeleteItem、BatchWrite)
    // TransactWriteItems 没有单独的 IAM 动作，按事务里每一项的 Put / Update / ConditionCheck 分别鉴权
    task.taskRole.addToPrincipalPolicy(new iam.PolicyStatement({
      actions: ['dynamodb:GetItem', 'dynamodb:PutItem', 'dynamodb:UpdateItem', 'dynamodb:Query', 'dynamodb:ConditionCheckItem'],
      resources: [table.tableArn],
    }));
    key.grantEncryptDecrypt(task.taskRole);   // 表用 CMK 加密，调用方需要用这把 key

    const service = new ecs.FargateService(this, 'Service', {
      cluster,
      taskDefinition: task,
      desiredCount: 2,                         // 两个实例: 预算租约就是为多实例设计的
      securityGroups: [taskSg],
      vpcSubnets: isolated,
      assignPublicIp: false,
      minHealthyPercent: 100,                  // 滚动发布时先起新任务再停旧任务
      maxHealthyPercent: 200,
      circuitBreaker: { enable: true, rollback: true },   // 新版本起不来 → 自动回滚到上一个版本
    });

    // ---------- 内部 ALB ----------
    const alb = new elbv2.ApplicationLoadBalancer(this, 'Alb', {
      vpc, internetFacing: false, securityGroup: albSg, vpcSubnets: isolated,
      dropInvalidHeaderFields: true,   // 丢弃不合法的 HTTP 头，防 HTTP 请求走私
    });
    checkovSkip(alb, ['CKV_AWS_91', 'ALB access logs need an S3 bucket; added in stage 6 (logging). External traffic is logged at API Gateway']);
    const listener = alb.addListener('Http', { port: 80, open: false });
    // 内部 ALB 只在隔离子网里，TLS 在 API Gateway 终止；VPC 内东西向加密放到阶段 7 (Istio mTLS)
    checkovSkip(listener,
      ['CKV_AWS_2', 'internal ALB in isolated subnets; TLS terminates at API Gateway, no ACM cert/domain for internal hop'],
      ['CKV_AWS_103', 'same as CKV_AWS_2: plain HTTP listener inside the VPC only']);
    listener.addTargets('Api', {
      port: 8080,
      protocol: elbv2.ApplicationProtocol.HTTP,
      targets: [service],
      healthCheck: { path: '/health', healthyHttpCodes: '200', interval: cdk.Duration.seconds(10) },
      deregistrationDelay: cdk.Duration.seconds(15),
    });

    // ---------- API Gateway (对外) ----------
    const vpcLink = new apigwv2.VpcLink(this, 'VpcLink', { vpc, subnets: isolated, securityGroups: [vpcLinkSg] });
    const integration = new HttpAlbIntegration('AlbIntegration', listener, {
      vpcLink,
      // 身份由网关写入，覆盖客户端自己传的值 → 无法冒充其它广告主
      // 这里用 SigV4 调用者的 AWS 账号当广告主 id；真实的 Ads API 用 OAuth (Login with Amazon)，
      // 对应换成 JWT authorizer + $context.authorizer.claims.sub
      parameterMapping: new apigwv2.ParameterMapping().overwriteHeader(
        'X-Advertiser-Id', apigwv2.MappingValue.contextVariable('identity.accountId')),
    });
    const api = new apigwv2.HttpApi(this, 'Api', {
      apiName: 'ads-campaign-budget',
      defaultAuthorizer: new HttpIamAuthorizer(),
      createDefaultStage: false,
    });
    new apigwv2.HttpStage(this, 'DefaultStage', {
      httpApi: api,
      stageName: '$default',
      autoDeploy: true,
      throttle: { rateLimit: 50, burstLimit: 100 },   // 防止单个客户端把后端打爆 (超出返回 429)
      // 访问日志：谁 (调用者账号) 在什么时候调了什么，出安全事件时用来追查
      accessLogSettings: {
        destination: new apigwv2.LogGroupLogDestination(new logs.LogGroup(this, 'ApiAccessLogs', {
          retention: logs.RetentionDays.ONE_MONTH, encryptionKey: key, removalPolicy: removal,
        })),
        format: apigw.AccessLogFormat.custom(JSON.stringify({
          requestId: '$context.requestId', ip: '$context.identity.sourceIp', caller: '$context.identity.accountId',
          time: '$context.requestTime', method: '$context.httpMethod', path: '$context.path',
          status: '$context.status', latencyMs: '$context.responseLatency',
        })),
      },
    });
    const routes: Array<[apigwv2.HttpMethod, string]> = [
      [apigwv2.HttpMethod.POST, '/v1/campaigns'],
      [apigwv2.HttpMethod.GET, '/v1/campaigns'],
      [apigwv2.HttpMethod.GET, '/v1/campaigns/{id}'],
      [apigwv2.HttpMethod.PATCH, '/v1/campaigns/{id}'],
      [apigwv2.HttpMethod.PUT, '/v1/campaigns/{id}/keywords/{text}'],
      [apigwv2.HttpMethod.GET, '/v1/campaigns/{id}/keywords'],
      [apigwv2.HttpMethod.GET, '/v1/campaigns/{id}/budget'],
      [apigwv2.HttpMethod.GET, '/v1/reports/daily-spend'],
      // 故意没有 /charges: 只有 VPC 内的广告服务器能调
    ];
    for (const [method, routePath] of routes) {
      api.addRoutes({ path: routePath, methods: [method], integration });
    }

    // ---------- 告警 ----------
    new cloudwatch.Alarm(this, 'Target5xxAlarm', {
      alarmDescription: 'ads_api returned 5xx',
      metric: alb.metrics.httpCodeTarget(elbv2.HttpCodeTarget.TARGET_5XX_COUNT, { period: cdk.Duration.minutes(1) }),
      threshold: 5,
      evaluationPeriods: 3,
      datapointsToAlarm: 2,   // 3 分钟里有 2 分钟超标才告警，避免一次抖动就报
      treatMissingData: cloudwatch.TreatMissingData.NOT_BREACHING,
    });
    new cloudwatch.Alarm(this, 'LatencyP99Alarm', {
      alarmDescription: 'ads_api p99 latency above 200ms',
      metric: alb.metrics.targetResponseTime({ statistic: 'p99', period: cdk.Duration.minutes(1) }),
      threshold: 0.2,
      evaluationPeriods: 5,
      datapointsToAlarm: 3,
    });

    // CDK 自己生成的 Lambda (restrictDefaultSecurityGroup 的 custom resource)，只在部署时跑一次
    cdk.Aspects.of(this).add({
      visit(node) {
        if (node instanceof cdk.CfnResource && node.cfnResourceType === 'AWS::Lambda::Function') {
          checkovSkip(node,
            ['CKV_AWS_115', 'CDK-managed deploy-time custom resource, not a workload'],
            ['CKV_AWS_116', 'CDK-managed deploy-time custom resource; CloudFormation surfaces failures'],
            ['CKV_AWS_117', 'CDK-managed custom resource only calls the EC2 API; VPC has no NAT so it cannot run inside it']);
        }
      },
    });

    new cdk.CfnOutput(this, 'ApiUrl', { value: api.apiEndpoint });
    new cdk.CfnOutput(this, 'InternalAlbDns', { value: alb.loadBalancerDnsName });
    new cdk.CfnOutput(this, 'TableName', { value: table.tableName });
    new cdk.CfnOutput(this, 'DbEndpoint', { value: db.dbInstanceEndpointAddress });
  }
}
