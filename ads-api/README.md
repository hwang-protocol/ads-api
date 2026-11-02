# Ads Campaign & Budget Service（AWS 原生，C++）

广告主通过 REST API 管理 campaign、关键词和出价；广告服务器上报点击扣费。服务保证**多实例并发扣费也不超过日预算**，并能匀速花费。

**对应简历**：AWS / Amazon Ads 的整体技术栈 —— API、DynamoDB 幂等和条件写、PostgreSQL、IAM 最小权限、VPC endpoint、KMS、CDK、优雅关闭和自动回滚。

> 领域概念 (advertiser → campaign → keyword、日预算、bid、micros) 来自**公开的 Amazon Ads API 文档**。代码、表设计和架构都是自己设计的，不包含任何 Amazon 内部的系统、代码或命名。

## 架构

```
客户端 (SigV4) ─▶ API Gateway HTTP API ── IAM 鉴权、限流、把调用者账号写进 X-Advertiser-Id
                      │ VPC Link
┌──────────── VPC：只有私有隔离子网，没有 NAT ─────────────────────────────────────────┐
│ 内部 ALB ─▶ ECS Fargate × 2 (ARM64，C++ ads_api) ─▶ RDS PostgreSQL (记账流水，KMS)  │
│    ▲                │                                                              │
│ 广告服务器          ├─ Gateway Endpoint ─▶ DynamoDB (campaign / 关键词 / 预算计数器) │
│ (/charges 只对内)   └─ Interface Endpoints ─▶ ECR、CloudWatch Logs、Secrets Manager  │
└────────────────────────────────────────────────────────────────────────────────────┘
```

代码分层（ports & adapters）：

```
api.cpp (HTTP + JSON) ─▶ BudgetService (租约、匀速、去重) ─▶ 存储接口 stores.h
                                                              ├─ MemoryStore   (测试 / 本机)
                                                              ├─ DynamoStore   (aws-sdk-cpp)
                                                              └─ PgLedger      (libpq)
```
核心逻辑只依赖接口，所以在 macOS 上不装 AWS SDK 也能编译和测试；AWS 版本在 Docker 里编译。

## API

| 方法 | 路径 | 要点 |
|---|---|---|
| POST | `/v1/campaigns` | 必须带 `Idempotency-Key`。新建返回 201；重放返回 200 并带 `Idempotent-Replayed: true`；同一个 key 配不同内容返回 422 |
| GET | `/v1/campaigns`、`/v1/campaigns/:id` | 返回 `ETag: "版本号"`；别人的 campaign 一律返回 404 |
| PATCH | `/v1/campaigns/:id` | 必须带 `If-Match`（缺了返回 428）；版本不对返回 409 和 `current_version`；改预算时同一个事务里调整当天的计数器 |
| PUT / GET | `/v1/campaigns/:id/keywords[/:text]` | 关键词出价 |
| POST | `/v1/campaigns/:id/charges` | 每批 1–500 个点击事件，逐条返回 ACCEPTED / DUPLICATE / BUDGET_EXHAUSTED / PACING_LIMITED / … |
| GET | `/v1/campaigns/:id/budget` | 对账：记账 + 计数器余额 + 本实例未花的租约 = 日预算 |
| GET | `/v1/reports/daily-spend?from=&to=` | PostgreSQL 窗口函数：当天占比、按日累计 |

## DynamoDB 单表设计

| 实体 | PK | SK | 访问模式 |
|---|---|---|---|
| Campaign | `ADV#{advertiser}` | `CMP#{id}` | GetItem；按广告主 Query 列表 |
| Keyword | `CMP#{id}` | `KW#{text}` | 按 campaign Query |
| 当日预算 | `CMP#{id}` | `BUDGET#{date}` | 原子扣减 (条件写)，TTL 7 天 |
| 幂等记录 | `IDEM#{advertiser}#{key}` | `IDEM` | 创建时放在事务里一起写，TTL 24 小时 |

用到的 DynamoDB 特性：
- 条件写：`attribute_not_exists`、`version = :expected`、`remaining >= :want`
- `TransactWriteItems`：创建时 campaign 和幂等记录原子写入；改预算时 campaign 和当天计数器原子更新
- `if_not_exists`：一次调用完成 "不存在就初始化，然后扣减"
- 强一致读

## 预算租约：核心设计

问题：多个实例同时给同一个热门 campaign 扣费，如何**不超支**，又不把 DynamoDB 的单个分区打爆？

- 每个实例一次从 DynamoDB 计数器里 "批发" 日预算的 2%，用条件写 `remaining >= :want` 保证不会拿超。
- 之后的扣费只在本地内存里做，用完了再去批发；剩余不够一整块时，把剩下的全部拿走。
- 收到 SIGTERM 时，把手里没花完的还回去（用 `ADD remaining`，原子操作）。
- **不变式**：记账总额 + 计数器余额 + 各实例手里的租约 = 日预算。测试、集成测试和 benchmark 都检查这一条。
- **代价**：实例崩溃时，手里的租约就丢了，当天会少花最多 2% 的预算。超支要赔钱，少花一点可以接受，这个取舍是故意的。

扣费流程（跨两个存储，没有分布式事务）：
1. 先从本地租约里预扣。
2. 写 PostgreSQL 记账，用 `ON CONFLICT (event_id) DO NOTHING RETURNING`。
3. 返回结果里没有的 event 就是重复的，把预扣退回本地。
4. 如果 PostgreSQL 写失败，把全部预扣退回本地，返回 503。

## 运行

想看一个请求经过哪些组件: [demo/E2E_FLOW.md](demo/E2E_FLOW.md)；一步一步亲眼看: `demo/e2e_demo.sh`

```bash
# 1) 本机，内存存储 (不需要 AWS SDK)；首次 cmake 会下载 nlohmann/json 和 cpp-httplib
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
build/ads_tests
build/ads_api --memory --port 8080 &
python3 tests/integration_test.py http://localhost:8080

# 2) Docker：DynamoDB Local + PostgreSQL + 两个 ads_api 实例 (首次构建要编译 aws-sdk-cpp，约 10 分钟)
cd ..   # 仓库根目录
docker compose -f ads-api/docker/compose.yaml up --build -d
python3 ads-api/tests/integration_test.py http://localhost:8080 http://localhost:8081
docker compose -f ads-api/docker/compose.yaml run --rm bench --chunk-fraction 0      # 对照组
docker compose -f ads-api/docker/compose.yaml run --rm bench --chunk-fraction 0.02   # 租约
docker compose -f ads-api/docker/compose.yaml down

# 3) AWS (可选，会产生费用)
cd ads-api/infra
npm install
npx cdk synth                 # 只生成 CloudFormation 模板，不花钱
npx cdk bootstrap             # 每个账号 / 区域只需要一次
npx cdk deploy                # 在本机用 Docker 构建 ARM64 镜像，推到 ECR
awscurl --service execute-api --region us-east-1 "$API_URL/v1/campaigns"   # SigV4 签名调用 (pip install awscurl)
npx cdk destroy               # 用完一定要删
```

**AWS 费用估算**（us-east-1 公开价格，以 AWS Pricing Calculator 为准）：4 个 Interface Endpoint × 2 个 AZ 约 $58/月，ALB 约 $16/月，Fargate ARM 2 个任务约 $29/月，RDS t4g.micro 加 20GB 存储约 $15/月，KMS 和 Secrets Manager 约 $1.5/月，**合计约 $120/月，即每小时约 $0.17**。部署、测试、删除在两小时内完成，花费不到 $1。

## 实测

**预算 benchmark**：4 个实例 × 4 个线程，一个热门 campaign，需求是预算的 1.6 倍。

| 存储 | 模式 | 预算存储调用次数 | 每批 10 次点击 p50 | 超支 | 守恒 |
|---|---|---|---|---|---|
| 内存 + 模拟 800µs 往返 (4 万次点击) | 每次点击扣一次 | 25,004 | 12.6ms | $0 | ✅ |
| 同上 | 租约 2% | **54** | **6.9µs** | $0 | ✅ |
| DynamoDB Local (8000 次点击，预算 $100) | 每次点击扣一次 | 5,008 | 6.6ms | $0 | ✅ |
| 同上 | 租约 2% | **58** | **3.7µs** | $0 | ✅ |

**集成测试**：对着两个 Docker 实例 (DynamoDB Local + PostgreSQL) 全部通过。覆盖了：
- 幂等创建：重试打到另一个实例也返回同一个 campaign
- 租户隔离、409 / 428 状态码
- 同一个点击事件发到另一个实例会被识别为 DUPLICATE
- 20 个线程跨两个实例并发扣费：不超支，并且守恒

**优雅关闭**：`docker compose stop api2` 之后日志打印 `returned 1500000 micros of unspent budget leases`，计数器余额从 $98.00 回到 $99.50。

**CDK**：`cdk synth` 生成 61 个资源。task role 只有 5 个 DynamoDB 动作加上这把 KMS key 的权限；数据库安全组没有出站规则。

单元测试在 ASan 和 TSan 下都通过。

## 面试讲点

1. **幂等键（Stripe 风格）**：客户端重试不会重复创建。幂等记录和 campaign 在同一个 DynamoDB 事务里写入；同一个 key 配不同的请求体返回 422；记录有 24 小时 TTL。
2. **乐观锁 + ETag / If-Match**：并发修改不会互相覆盖（lost update）。不用加锁，冲突时返回 409，让客户端重读后重试。
3. **预算租约**：用 "不超支 vs 少花" 的取舍，换来 DynamoDB 调用量下降两个数量级，同时解决热点分区问题。这和 Millburn 的风控限额是同一个问题：多个策略分片共享一个总限额。
4. **为什么预算计数器用 DynamoDB、记账流水用 PostgreSQL**：前者是按 key 的高频原子操作，后者需要聚合、窗口函数、JOIN 和对账。两者之间没有分布式事务，靠写入顺序、event_id 去重和退款来保证一致性。
5. **金额用 int64 micros**，不用 double。
6. **去重的范围要想清楚**（端到端演示时发现的真实 bug）：记账表一开始只用 `event_id` 做主键，结果不同 campaign 里同名的点击被当成重复，**悄悄漏收了钱**，单元测试没覆盖到这个情况。修复方法是把主键改成 `(campaign_id, event_id)`，并写了一个幂等迁移，能直接升级已经有数据的旧表（在本地有数据的 PostgreSQL 上验证过）。面试时可以说：幂等键的作用域和唯一约束的列，是 API 契约的一部分，要和调用方约定清楚。
7. **网络**：没有 NAT，全部走 VPC Endpoint。安全性更好，但 Interface Endpoint 要收钱：4 个 × 2 个 AZ 已经比一个 NAT Gateway 贵了，这是需要讲清楚的成本取舍。
8. **IAM**：任务只有 5 个动作、只针对一张表；数据库密码由 ECS 从 Secrets Manager 注入；身份由 API Gateway 写入请求头，客户端无法伪造。
9. **发布安全**：ECS circuit breaker 自动回滚；minHealthy 100% 保证先起新任务再停旧任务；SIGTERM 后归还租约；CloudWatch 告警用 "3 个周期里有 2 个超标" 来避免误报。
10. **C++ 细节**：
   - pimpl：`dynamo_store.h` 里不出现 AWS SDK 类型。
   - `AwsApiGuard` 用 RAII 管理 SDK 的初始化和关闭顺序。
   - libpq 连接池：借出的连接用 RAII 自动归还。
   - 测试宏 `CHECK_EQ` 原来用 `auto&&` 绑定参数，遇到 `cli.Get(...)->status` 这种临时对象的成员时会悬空。编译器的 `-Wdangling-gsl` 警告和失败的测试暴露了这个问题，已改为按值拷贝。

## 没验证到的地方（如实说明）

- **没有真正部署到 AWS**：IAM 策略是否刚好够用、VPC Endpoint 是否齐全、API Gateway 的 `$context.identity.accountId` 映射是否生效，都只验证到 `cdk synth` 这一步。
- DynamoDB Local 不模拟限流和分区吞吐上限，所以 "租约能避免热点分区被打爆" 这个结论来自 DynamoDB 的文档 (单分区每秒 1000 WCU)，不是实测。
- 本地的身份认证只是一个请求头；真实的 Ads API 用 OAuth (Login with Amazon)。
- 匀速投放在多实例下是近似的：每个实例用 "计数器余额" 估算全局已花多少，其它实例手里没花的租约也算作已花，所以偏保守。
