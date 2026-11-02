# 端到端流程：一个请求经过哪些组件

想**亲眼看**的话，直接跑：

```bash
cd ads-api   # 仓库根目录
ads-api/demo/e2e_demo.sh          # 每一步停下来，按回车继续
ads-api/demo/e2e_demo.sh --no-pause
```

脚本每发一个请求，就把四个地方的变化打印出来：① HTTP 请求和响应、② 哪个 ads_api 实例处理了、③ DynamoDB 里的 item、④ PostgreSQL 里的行。

---

## 0. 组件全景

```
                 本地 (docker compose)                         AWS (CDK 部署后)
               ─────────────────────────                    ──────────────────────────────────
  调用方        curl / integration_test.py                   客户端 (SigV4 签名) / VPC 内的广告服务器
                     │                                              │
  入口                │  直接打到容器端口                              ▼
                     │                                   API Gateway HTTP API
                     │                                     · IAM 鉴权 (验签)
                     │                                     · 限流 (超出返回 429)
                     │                                     · 用调用者的账号覆盖 X-Advertiser-Id
                     │                                              │ VPC Link
                     │                                              ▼
  负载均衡             │                                   内部 ALB (健康检查 /health)
                     ▼                                              ▼
  服务         api1 :8080  /  api2 :8081                  Fargate 任务 × 2 (同一个镜像)
               ads_api (C++)                              ads_api (C++)
                 │        │                                  │        │
  数据         DynamoDB   PostgreSQL                       DynamoDB   RDS PostgreSQL
               Local      16 (容器)                        (走 Gateway Endpoint)  (私有子网)
```

服务内部分三层（[src/](../src/)）：

```
api.cpp (路由、校验、JSON)  ──▶  BudgetService (租约、匀速、去重)  ──▶  存储接口
                                                                     ├─ DynamoStore (aws-sdk-cpp)
                                                                     └─ PgLedger    (libpq)
```

---

> **下面第 1–8 节的图从 "请求到达 ads_api 进程" 开始画。** 图里的 `curl ──▶ api.cpp` 是**本地**的路径。
> 线上在它前面还有一段：`客户端 ─▶ API Gateway (验签、限流、写入 X-Advertiser-Id) ─▶ VPC Link ─▶ 内部 ALB ─▶ 某个 Fargate 任务`。
> 进入进程之后本地和线上走的是同一份代码，只有环境变量不同：
>
> | | 本地 | 线上 |
> |---|---|---|
> | DynamoDB | `DDB_ENDPOINT=http://dynamodb:8000` (DynamoDB Local) | 不设置 → 真实 DynamoDB (走 Gateway Endpoint) |
> | PostgreSQL | `compose.yaml` 里写死的账号密码 | RDS，`PGSSLMODE=require`，账号密码由 Secrets Manager 注入 |
> | `X-Advertiser-Id` | 调用方自己带 (能伪造) | API Gateway 写入 (伪造不了) |
>
> 例外: `/charges` 在线上不经过 API Gateway，由 VPC 内的广告服务器直接调内部 ALB。

## 1. 创建 campaign：`POST /v1/campaigns`

```
curl ──▶ api.cpp:77 路由
          │ 1. 检查 X-Advertiser-Id (缺了返回 401)、Idempotency-Key (缺了返回 400)
          │ 2. 解析 JSON、校验 name / budget，生成 campaign_id
          │ 3. 请求指纹 = hash(规范化后的 JSON)
          ▼
        DynamoStore::create  (dynamo_store.cpp:187)
          │  TransactWriteItems [
          │    Put  PK=ADV#{adv}  SK=CMP#{id}      IF attribute_not_exists(PK)
          │    Put  PK=IDEM#{adv}#{key}  SK=IDEM   IF attribute_not_exists(PK)   (TTL 24h)
          │  ]
          ├─ 成功            ──▶ 201 Created + ETag: "1"
          └─ 事务被取消        ──▶ 读幂等记录 (强一致读)
                ├─ 指纹相同  ──▶ 200 + Idempotent-Replayed: true (返回第一次创建的 campaign)
                └─ 指纹不同  ──▶ 422 (同一个 key 不能用于不同的请求)
```
**涉及**：DynamoDB（campaign item 和幂等记录）。不涉及 PostgreSQL。

## 2. 修改：`PATCH /v1/campaigns/:id`

```
curl (If-Match: "1") ──▶ api.cpp:122
          │ 缺 If-Match → 428；校验要改的字段
          ▼
        DynamoStore::update  (dynamo_store.cpp:237)
          │ 1. GetItem 读当前版本 (不对就直接返回 409，省一次写)
          │ 2. TransactWriteItems [
          │      Update campaign SET ..., version = version + 1   IF version = :expected
          │      (改了日预算的话) Update 当日计数器 SET remaining = if_not_exists(remaining, :旧预算) + :差额
          │    ]
          ├─ 成功 ──▶ 200 + ETag: "2"，然后清掉本实例的 campaign 缓存
          └─ 失败 ──▶ 409 + current_version (别人刚改过，客户端应该重读再改)
```

## 3. 关键词：`PUT /v1/campaigns/:id/keywords/:text`

```
curl ──▶ api.cpp:166 ──▶ DynamoStore::get (先确认这个 campaign 是你的，否则返回 404)
                    ──▶ DynamoStore::put_keyword (dynamo_store.cpp:285): PutItem PK=CMP#{id} SK=KW#{text}
```

## 4. 扣费（最核心的流程）：`POST /v1/campaigns/:id/charges`

在 AWS 上这个接口**不经过 API Gateway**，只有 VPC 内的广告服务器能直接调 ALB。

```
广告服务器 ──▶ api.cpp:193 (每批 1..500 个事件)
                │
                ▼
          BudgetService::charge  (budget_service.cpp:80)
                │ a. 读 campaign：本地缓存 1 秒，过期才 GetItem
                │    campaign 不是 ENABLED → 全部返回 CAMPAIGN_NOT_ACTIVE
                │ b. 同一批里重复的 event_id、金额不合法的先标出来
                │ c. 加这个 campaign 的本地锁，逐个事件:
                │      · 匀速检查 (STANDARD 模式): 已消耗 + 本次 > 日预算 × 时间进度 → PACING_LIMITED
                │      · 本地租约不够 → DynamoStore::lease (dynamo_store.cpp:307)
                │            UpdateItem PK=CMP#{id} SK=BUDGET#{date}
                │              SET remaining = if_not_exists(remaining, :日预算) - :want
                │              IF attribute_not_exists(remaining) OR remaining >= :want
                │            (want = 日预算的 2%；剩余不够就把剩下的全部拿走；真的没了 → BUDGET_EXHAUSTED)
                │      · 在本地租约里预扣
                │ d. PgLedger::record  (pg_ledger.cpp:111)
                │      INSERT INTO spend_ledger ... SELECT FROM unnest($ids, $costs)
                │      ON CONFLICT (event_id) DO NOTHING RETURNING event_id
                │ e. RETURNING 里没有的 event → DUPLICATE，把预扣退回本地租约
                │    PostgreSQL 写失败 → 预扣全部退回，返回 503 + Retry-After
                ▼
          200 {"accepted_micros":..., "summary":{...}, "results":[...]}
```
**涉及**：DynamoDB（campaign 配置、当日预算计数器）和 PostgreSQL（记账流水）。

**为什么 DynamoDB 调用很少**：只有在本地租约用完时才去 DynamoDB 预支一次。在 benchmark 里，5,008 次扣费调用降到了 58 次。

## 5. 对账：`GET /v1/campaigns/:id/budget`

```
api.cpp:222 ──▶ BudgetService::view (budget_service.cpp:172)
                  ├─ DynamoStore::get        日预算
                  ├─ DynamoStore::remaining  计数器余额 (强一致读)
                  ├─ 本实例手里的租约          (内存)
                  └─ PgLedger::spent          SELECT SUM(cost_micros) ... WHERE campaign_id = $1 AND spend_date = $2
```
守恒式：**记账 + 计数器余额 + 所有实例手里的租约 = 日预算**。

每个实例的 `consistent` 字段只算了它自己的租约。实例多于一个时，需要把所有实例的租约加起来才能对上（演示脚本第 7 步专门演示了这一点）。

## 6. 报表：`GET /v1/reports/daily-spend`

```
api.cpp:235 ──▶ PgLedger::report (pg_ledger.cpp:152)
                  WITH daily AS (SELECT ... GROUP BY spend_date, campaign_id)
                  SELECT ..., spend / SUM(spend) OVER (PARTITION BY spend_date)          -- 当天占比
                            , SUM(spend) OVER (PARTITION BY campaign_id ORDER BY spend_date) -- 按日累计
```
**涉及**：只有 PostgreSQL。报表需要聚合和窗口函数，这是把记账放在关系数据库里的原因。

## 7. 关机：SIGTERM（`docker compose stop`、ECS 滚动发布、缩容）

```
SIGTERM ──▶ 信号处理函数只设置一个 atomic<bool> (main.cpp)
        ──▶ watcher 线程看到后调用 svr.stop() (main.cpp:90)
        ──▶ svr.listen() 返回：不再接新请求，正在处理的请求做完
        ──▶ BudgetService::release_all (budget_service.cpp:190)
              UpdateItem SET ... ADD remaining :本地没花完的租约   (原子加，多个实例同时还也不会丢)
        ──▶ 打印 "shutdown: returned N micros of unspent budget leases"，进程退出
```
ECS 发出 SIGTERM 后会等 `stopTimeout`（CDK 里设的是 30 秒），时间到了还没退出才发 SIGKILL。如果进程被 SIGKILL 或者直接崩溃，手里的租约就丢了：当天会少花这部分，但不会超支。

## 8. 启动

```
main.cpp ──▶ AwsApiGuard (初始化 aws-sdk-cpp)
         ──▶ DynamoStore；如果设置了 DDB_ENDPOINT (说明是本地) 就自己建表；AWS 上的表由 CDK 创建
         ──▶ PgLedger：建连接池 (8 个连接)，从 PG* 环境变量读连接参数
              migrate()：拿 advisory lock 再建表。两个实例同时启动不会冲突，日志里第二个会显示 "already exists, skipping"
         ──▶ 注册路由，开始监听 :8080
```

---

## 怎么单独触发每个组件

| 组件 | 怎么触发 / 怎么观察 |
|---|---|
| ads_api（某个实例） | `curl localhost:8080/...` 是 api1，`:8081` 是 api2；日志用 `docker compose -f docker/compose.yaml logs -f api1 api2` |
| DynamoDB | 读：`AWS_ACCESS_KEY_ID=local AWS_SECRET_ACCESS_KEY=local aws dynamodb scan --endpoint-url http://localhost:8000 --region us-east-1 --table-name ads` |
| PostgreSQL | `docker compose -f docker/compose.yaml exec postgres psql -U ads -d ads -c "SELECT * FROM spend_ledger LIMIT 10"` |
| 预算租约的并发行为 | `docker compose -f docker/compose.yaml run --rm bench --chunk-fraction 0.02`（改成 0 就是对照组） |
| 多实例 + 全部业务规则 | `python3 tests/integration_test.py http://localhost:8080 http://localhost:8081` |
| 优雅关机 | `docker compose -f docker/compose.yaml stop api2`，看日志里的 "returned ... micros" |
| PostgreSQL 挂掉时的 503 | `docker compose -f docker/compose.yaml stop postgres`，再发一次 charges 请求 |
| 不依赖 Docker 的核心逻辑 | `build/ads_tests`，或者 `ads_api --memory` |
| AWS 上的资源定义 | `cd infra && npx cdk synth`，生成的 CloudFormation 模板在 `cdk.out/` 里 |
