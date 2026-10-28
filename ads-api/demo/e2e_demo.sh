#!/usr/bin/env bash
# 端到端演示: 一步一步发请求，每一步之后把每个组件里发生了什么打印出来
#   ① HTTP 请求和响应
#   ② ads_api 的访问日志 (哪个实例处理的)
#   ③ DynamoDB 里相关的 item
#   ④ PostgreSQL 里相关的行
#
# 用法 (在仓库根目录或任意目录):
#   ads-api/demo/e2e_demo.sh            每一步停下来等你按回车
#   ads-api/demo/e2e_demo.sh --no-pause 一口气跑完
# 依赖: Docker、curl、jq、aws CLI (只用来读本地 DynamoDB Local，不连真实 AWS)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
COMPOSE=(docker compose -f "$HERE/../docker/compose.yaml")
A=http://localhost:8080   # api1
B=http://localhost:8081   # api2
ADV="demo-$(date +%H%M%S)"
PAUSE=1
[[ "${1:-}" == "--no-pause" ]] && PAUSE=0

bold() { printf '\n\033[1;36m%s\033[0m\n' "$*"; }
note() { printf '\033[2m%s\033[0m\n' "$*"; }
hdr()  { printf '\033[1;33m  %s\033[0m\n' "$*"; }
pause() { [[ $PAUSE == 1 ]] && { read -r -p $'\n按回车进入下一步 ...' _; } || true; }
now_utc() { python3 -c 'import datetime; print(datetime.datetime.now(datetime.timezone.utc).isoformat())'; }

# ---------- 每个组件的 "观察窗口" ----------
ddb() {   # ddb <PK> [SK]   读 DynamoDB Local 里的 item，把 {"S": ".."} / {"N": ".."} 拍平
    local pk=$1 sk=${2:-}
    local args=(--table-name ads --key-condition-expression "PK = :pk" --expression-attribute-values "{\":pk\":{\"S\":\"$pk\"}}")
    [[ -n "$sk" ]] && args=(--table-name ads --key-condition-expression "PK = :pk AND SK = :sk"
                            --expression-attribute-values "{\":pk\":{\"S\":\"$pk\"},\":sk\":{\"S\":\"$sk\"}}")
    AWS_ACCESS_KEY_ID=local AWS_SECRET_ACCESS_KEY=local AWS_PAGER="" \
        aws dynamodb query --endpoint-url http://localhost:8000 --region us-east-1 --consistent-read "${args[@]}" \
        | jq -c '.Items[] | with_entries(.value |= (.S // .N))' | sed 's/^/    /'
}
pg() {    # pg "<SQL>"
    "${COMPOSE[@]}" exec -T postgres psql -U ads -d ads -P pager=off -c "$1" | sed 's/^/    /'
}
logs_since() {   # 这一步期间两个 ads_api 实例打出的日志
    "${COMPOSE[@]}" logs --since "$1" api1 api2 2>/dev/null | grep -v '^\s*$' | sed 's/^/    /' || true
}
call() {  # call <METHOD> <BASE> <PATH> [BODY] [额外的 -H ...]
    local method=$1 base=$2 path=$3 body=${4:-}; shift 4 || shift $#
    local args=(-s -D /tmp/e2e_headers -o /tmp/e2e_body -X "$method" "$base$path" -H "X-Advertiser-Id: $ADV" -H "Content-Type: application/json" "$@")
    [[ -n "$body" ]] && args+=(-d "$body")
    hdr "① 请求"
    printf '    %s %s%s\n' "$method" "$base" "$path"
    for h in "$@"; do [[ "$h" != "-H" ]] && printf '    %s\n' "$h"; done
    [[ -n "$body" ]] && printf '    %s\n' "$body"
    curl "${args[@]}" || true
    hdr "   响应"
    printf '    %s\n' "$(head -1 /tmp/e2e_headers | tr -d '\r')"
    grep -iE '^(etag|idempotent-replayed|retry-after):' /tmp/e2e_headers | tr -d '\r' | sed 's/^/    /' || true
    jq -c . /tmp/e2e_body 2>/dev/null | cut -c1-400 | sed 's/^/    /' || cat /tmp/e2e_body
}
field() { jq -r "$1" /tmp/e2e_body; }

# ---------- 0. 启动环境 ----------
bold "0. 启动本地环境: DynamoDB Local + PostgreSQL + 两个 ads_api 实例 (api1 :8080, api2 :8081)"
"${COMPOSE[@]}" up --build -d --quiet-pull 2>&1 | grep -E "Started|Running|Healthy" | sed 's/^/    /' || true
for i in $(seq 1 60); do
    curl -sf "$A/health" >/dev/null && curl -sf "$B/health" >/dev/null && break
    sleep 1
done
note "    两个实例都健康。广告主 id = $ADV"
note "    本地: curl 直接打到容器。AWS 上: 客户端 → API Gateway (SigV4 鉴权、限流、写入 X-Advertiser-Id) → VPC Link → 内部 ALB → 某个 Fargate 任务"
pause

# ---------- 1. 创建 campaign ----------
bold "1. POST /v1/campaigns —— 创建 campaign (带幂等键)"
note "   路径: api.cpp 路由 → 校验 → DynamoStore::create → DynamoDB TransactWriteItems [Put campaign, Put 幂等记录]"
KEY="create-$ADV"
T=$(now_utc)
call POST "$A" /v1/campaigns '{"name":"Holiday Sale","daily_budget_micros":50000000,"pacing":"ACCELERATED"}' -H "Idempotency-Key: $KEY"
CID=$(field .campaign_id)
hdr "② ads_api 日志";  logs_since "$T"
hdr "③ DynamoDB: PK=ADV#$ADV (campaign item)"; ddb "ADV#$ADV"
hdr "③ DynamoDB: PK=IDEM#$ADV#$KEY (幂等记录，TTL 24h)"; ddb "IDEM#$ADV#$KEY"
hdr "④ PostgreSQL: 这一步不涉及"
pause

# ---------- 2. 重试 ----------
bold "2. 同一个请求重试，打到另一个实例 api2 —— 幂等"
note "   路径: TransactWriteItems 因为幂等记录已存在被取消 → 读幂等记录 → hash 相同 → 返回第一次创建的 campaign"
T=$(now_utc)
call POST "$B" /v1/campaigns '{"name":"Holiday Sale","daily_budget_micros":50000000,"pacing":"ACCELERATED"}' -H "Idempotency-Key: $KEY"
hdr "② ads_api 日志 (注意是 api2)"; logs_since "$T"
hdr "③ DynamoDB: 仍然只有 1 个 campaign"; ddb "ADV#$ADV"
pause

# ---------- 3. 修改 (乐观锁) ----------
bold "3. PATCH /v1/campaigns/:id —— 乐观锁: 先用正确的版本号，再用过期的版本号"
note "   路径: DynamoStore::update → TransactWriteItems [Update campaign IF version = :expected]"
T=$(now_utc)
call PATCH "$A" "/v1/campaigns/$CID" '{"name":"Holiday Sale v2"}' -H 'If-Match: "1"'
call PATCH "$B" "/v1/campaigns/$CID" '{"status":"PAUSED"}' -H 'If-Match: "1"'
hdr "② ads_api 日志 (200 然后 409)"; logs_since "$T"
hdr "③ DynamoDB: version 变成 2，status 没被改成 PAUSED"; ddb "ADV#$ADV"
pause

# ---------- 4. 关键词 ----------
bold "4. PUT /v1/campaigns/:id/keywords/:text —— 设置关键词出价"
note "   路径: 先 GetItem 校验 campaign 归属 (防止给别人的 campaign 加词) → PutItem PK=CMP#id SK=KW#text"
T=$(now_utc)
call PUT "$A" "/v1/campaigns/$CID/keywords/winter-boots" '{"bid_micros":1200000}'
hdr "② ads_api 日志"; logs_since "$T"
hdr "③ DynamoDB: PK=CMP#$CID (关键词)"; ddb "CMP#$CID"
pause

# ---------- 5. 扣费 ----------
bold "5. POST /v1/campaigns/:id/charges —— 广告服务器上报 3 次点击 (打到 api1)"
note "   路径: BudgetService::charge"
note "     a. 读 campaign (本地缓存 1 秒)"
note "     b. 本地没有租约 → DynamoStore::lease: UpdateItem SET remaining = if_not_exists(remaining, 日预算) - 2% IF remaining >= want"
note "     c. 在本地租约里预扣 3 次点击"
note "     d. PgLedger::record: INSERT ... ON CONFLICT (event_id) DO NOTHING RETURNING event_id"
DATE=$(date -u +%Y-%m-%d)
T=$(now_utc)
call POST "$A" "/v1/campaigns/$CID/charges" \
    '{"events":[{"event_id":"click-001","cost_micros":250000},{"event_id":"click-002","cost_micros":250000},{"event_id":"click-003","cost_micros":500000}]}'
hdr "② ads_api 日志"; logs_since "$T"
hdr "③ DynamoDB: 当日预算计数器 (日预算 \$50，api1 预支了 2% = \$1 → 剩 \$49)"; ddb "CMP#$CID" "BUDGET#$DATE"
hdr "④ PostgreSQL: spend_ledger"
pg "SELECT event_id, campaign_id, spend_date, cost_micros FROM spend_ledger WHERE advertiser_id = '$ADV' ORDER BY event_id"
pause

# ---------- 6. 重复事件 ----------
bold "6. 同一个点击 click-001 再报一次，这次打到 api2 —— 跨实例去重"
note "   路径: api2 本地也没有租约 → 也去 DynamoDB 预支 \$1 → 预扣 → PostgreSQL 插入时冲突 (RETURNING 里没有它) → 判定重复 → 退回 api2 的本地租约"
T=$(now_utc)
call POST "$B" "/v1/campaigns/$CID/charges" '{"events":[{"event_id":"click-001","cost_micros":250000}]}'
hdr "② ads_api 日志"; logs_since "$T"
hdr "③ DynamoDB: 计数器又少了 \$1 (被 api2 预支走了，但没花)"; ddb "CMP#$CID" "BUDGET#$DATE"
hdr "④ PostgreSQL: 还是 3 行 —— 没有重复扣钱"
pg "SELECT COUNT(*) AS rows, SUM(cost_micros) AS spent_micros FROM spend_ledger WHERE advertiser_id = '$ADV'"
pause

# ---------- 7. 对账 ----------
bold "7. GET /v1/campaigns/:id/budget —— 对账 (分别问两个实例)"
note "   守恒式: 记账 + 计数器余额 + 每个实例手里没花完的租约 = 日预算"
call GET "$A" "/v1/campaigns/$CID/budget"
L1=$(field .local_unspent_micros); SPENT=$(field .ledger_spent_micros); REM=$(field .store_remaining_micros); BUD=$(field .daily_budget_micros)
call GET "$B" "/v1/campaigns/$CID/budget"
L2=$(field .local_unspent_micros)
hdr "   计算"
printf '    记账 %s + 计数器 %s + api1 租约 %s + api2 租约 %s = %s   (日预算 %s)\n' "$SPENT" "$REM" "$L1" "$L2" $((SPENT + REM + L1 + L2)) "$BUD"
note "    每个实例的 consistent 字段只算了它自己手里的租约: api1 看不到 api2 手里的 \$1，所以 api1 会显示 false。"
note "    把所有实例加起来才守恒 —— 生产上的对账任务要汇总所有实例 (或者等它们都把租约还回去之后再对)。"
pause

# ---------- 8. 报表 ----------
bold "8. GET /v1/reports/daily-spend —— 报表 (PostgreSQL 窗口函数)"
note "   路径: PgLedger::report → GROUP BY + SUM() OVER (PARTITION BY spend_date) + SUM() OVER (PARTITION BY campaign_id ORDER BY spend_date)"
call GET "$A" "/v1/reports/daily-spend?from=$DATE&to=$DATE"
pause

# ---------- 9. 优雅关闭 ----------
bold "9. docker compose stop api2 —— 发 SIGTERM，api2 把没花完的 \$1 租约还回 DynamoDB"
note "   路径: 信号处理函数置标志位 → watcher 线程调用 svr.stop() → listen() 返回 → BudgetService::release_all → UpdateItem ADD remaining :amt"
note "   (ECS 滚动发布 / 缩容时也是这样: 先发 SIGTERM，等 stopTimeout 秒后才 SIGKILL)"
T=$(now_utc)
"${COMPOSE[@]}" stop api2 2>&1 | sed 's/^/    /'
hdr "② ads_api 日志"; logs_since "$T"
hdr "③ DynamoDB: 计数器加回来了"; ddb "CMP#$CID" "BUDGET#$DATE"
"${COMPOSE[@]}" start api2 2>&1 | sed 's/^/    /'
for i in $(seq 1 30); do curl -sf "$B/health" >/dev/null && break; sleep 1; done   # 等 api2 重新就绪再退出

bold "完成。环境还在运行，可以自己继续用 curl 试；关闭: docker compose -f ads-api/docker/compose.yaml down"
