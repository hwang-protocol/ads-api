#!/usr/bin/env python3
"""端到端测试: 对着一个或多个正在运行的 ads_api 实例发 HTTP 请求 (只用标准库)。

  本机内存版:  ./ads_api --memory --port 8080 &  python3 tests/integration_test.py http://localhost:8080
  Docker 两个实例 (DynamoDB Local + PostgreSQL):
               python3 tests/integration_test.py http://localhost:8080 http://localhost:8081

验证:
  1. 幂等创建 —— 第二次请求发到另一个实例，也返回同一个 campaign
  2. 租户隔离、乐观锁 (409)、If-Match 必填 (428)
  3. 多个实例 × 多线程同时扣费: 不超支、重复 event 不重复扣钱
  4. 守恒: 记账 + 计数器余额 + 各实例手里没花完的租约 = 日预算
  5. 报表 (PostgreSQL 窗口函数) 的合计和记账一致
"""
import json
import sys
import threading
import urllib.error
import urllib.request
import uuid

BASES = sys.argv[1:] or ["http://localhost:8080"]
ADV = "adv-" + uuid.uuid4().hex[:8]
failures = 0


def call(base, method, path, body=None, headers=None, adv=ADV):
    h = {"Content-Type": "application/json", **({"X-Advertiser-Id": adv} if adv else {}), **(headers or {})}
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"{}"), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}"), dict(e.headers)


def check(cond, msg):
    global failures
    print(("  ok   " if cond else "  FAIL ") + msg)
    failures += not cond


def main():
    a, b = BASES[0], BASES[-1]
    print(f"advertiser={ADV} instances={BASES}")

    print("1. idempotent create")
    key = {"Idempotency-Key": uuid.uuid4().hex}
    body = {"name": "Holiday sale", "daily_budget_micros": 20_000_000, "pacing": "ACCELERATED"}
    s1, c1, _ = call(a, "POST", "/v1/campaigns", body, key)
    s2, c2, h2 = call(b, "POST", "/v1/campaigns", body, key)   # 重试打到另一个实例
    check(s1 == 201 and s2 == 200, f"201 then 200 (got {s1}, {s2})")
    check(c1["campaign_id"] == c2["campaign_id"], "same campaign returned on replay")
    check(h2.get("Idempotent-Replayed") == "true", "Idempotent-Replayed header")
    s3, _, _ = call(a, "POST", "/v1/campaigns", {**body, "name": "changed"}, key)
    check(s3 == 422, f"same key, different body -> 422 (got {s3})")
    cid = c1["campaign_id"]

    print("2. isolation / locking")
    check(call(a, "GET", f"/v1/campaigns/{cid}", adv="someone-else")[0] == 404, "other advertiser gets 404")
    check(call(a, "GET", f"/v1/campaigns/{cid}", adv=None)[0] == 401, "no identity -> 401")
    check(call(a, "PATCH", f"/v1/campaigns/{cid}", {"name": "x"})[0] == 428, "PATCH without If-Match -> 428")
    s, upd, h = call(a, "PATCH", f"/v1/campaigns/{cid}", {"name": "Holiday sale v2"}, {"If-Match": '"1"'})
    check(s == 200 and upd["version"] == 2 and h.get("ETag") == '"2"', "PATCH with correct version -> v2")
    s, err, _ = call(b, "PATCH", f"/v1/campaigns/{cid}", {"status": "PAUSED"}, {"If-Match": '"1"'})
    check(s == 409 and err.get("current_version") == 2, "stale If-Match -> 409 with current_version")
    s, _, _ = call(a, "PUT", f"/v1/campaigns/{cid}/keywords/winter-boots", {"bid_micros": 1_200_000})
    s2, kws, _ = call(b, "GET", f"/v1/campaigns/{cid}/keywords")
    check(s == 200 and [k["text"] for k in kws["keywords"]] == ["winter-boots"], "keyword written on A, read on B")

    print("3. charges")
    first = [{"event_id": "click-0001", "cost_micros": 20_000}]
    s, r1, _ = call(a, "POST", f"/v1/campaigns/{cid}/charges", {"events": first})
    s, r2, _ = call(b, "POST", f"/v1/campaigns/{cid}/charges", {"events": first})   # 广告服务器重试，打到另一个实例
    check(r1["accepted_micros"] == 20_000 and r2["results"][0]["outcome"] == "DUPLICATE" and r2["accepted_micros"] == 0,
          "same event on another instance -> DUPLICATE, charged once (PostgreSQL ON CONFLICT)")

    other_key = {"Idempotency-Key": uuid.uuid4().hex}
    _, other, _ = call(a, "POST", "/v1/campaigns", {"name": "Other", "daily_budget_micros": 5_000_000, "pacing": "ACCELERATED"}, other_key)
    s, r3, _ = call(b, "POST", f"/v1/campaigns/{other['campaign_id']}/charges", {"events": first})
    check(r3["accepted_micros"] == 20_000, "same event_id in a different campaign is NOT a duplicate")

    print("   concurrent charges across instances (demand = 3x budget)")
    accepted = [20_000]
    lock = threading.Lock()

    def worker(t):
        base = BASES[t % len(BASES)]
        for n in range(15):
            events = [{"event_id": f"t{t}-n{n}-e{i}", "cost_micros": 20_000} for i in range(10)]
            s, r, _ = call(base, "POST", f"/v1/campaigns/{cid}/charges", {"events": events})
            if s == 200:
                with lock:
                    accepted[0] += r["accepted_micros"]

    threads = [threading.Thread(target=worker, args=(t,)) for t in range(20)]   # 需求 20*15*10*0.02 = $60
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check(accepted[0] <= 20_000_000, f"no overspend: accepted ${accepted[0] / 1e6:.2f} <= $20.00")
    check(accepted[0] >= 19_000_000, "and nearly all budget used")

    dup = [{"event_id": "t0-n0-e0", "cost_micros": 20_000}]   # 第一批里已经记过账的 event
    s, r, _ = call(b, "POST", f"/v1/campaigns/{cid}/charges", {"events": dup})
    check(r["results"][0]["outcome"] in ("DUPLICATE", "BUDGET_EXHAUSTED") and r["accepted_micros"] == 0,
          f"replayed event not charged again ({r['results'][0]['outcome']})")

    print("4. conservation across instances")
    views = [call(base, "GET", f"/v1/campaigns/{cid}/budget")[1] for base in dict.fromkeys(BASES)]
    v = views[0]
    total = v["ledger_spent_micros"] + v["store_remaining_micros"] + sum(x["local_unspent_micros"] for x in views)
    check(total == v["daily_budget_micros"],
          f"ledger {v['ledger_spent_micros']} + remaining {v['store_remaining_micros']} + leases "
          f"{sum(x['local_unspent_micros'] for x in views)} = {total} == budget {v['daily_budget_micros']}")
    check(v["ledger_spent_micros"] == accepted[0], "ledger == sum of accepted")

    print("5. report")
    s, rep, _ = call(a, "GET", f"/v1/reports/daily-spend?from={v['date']}&to={v['date']}")
    row = next((x for x in rep["rows"] if x["campaign_id"] == cid), None)
    day_total = sum(x["spend_micros"] for x in rep["rows"])   # 同一个广告主还有一个 "Other" campaign
    check(row is not None and row["spend_micros"] == accepted[0], "report spend == ledger")
    check(row is not None and abs(row["share_of_advertiser"] - accepted[0] / day_total) < 1e-9,
          f"share_of_advertiser = {row['share_of_advertiser']:.4f} (window function matches spend / day total)")

    print("FAILED" if failures else "ALL PASSED")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
