// 单元测试 + HTTP 层测试 (全部用内存存储，主机和 Docker 里都能跑)
#include "ads/api.h"
#include "ads/budget_service.h"
#include "ads/memory_store.h"
#include "common/check.h"

#include <httplib.h>

#include <thread>

using namespace ads;
using namespace std::chrono;

namespace {
system_clock::time_point at_utc(int hour)
{
    return sys_days{year{2026} / 10 / 24} + hours{hour};
}

Campaign make_campaign(const std::string& adv, Micros budget, Pacing p = Pacing::Accelerated)
{
    Campaign c;
    c.advertiser_id = adv;
    c.campaign_id = new_id();
    c.name = "test";
    c.daily_budget = budget;
    c.pacing = p;
    return c;
}

std::vector<ChargeEvent> events(const std::string& prefix, int n, Micros cost)
{
    std::vector<ChargeEvent> out;
    for (int i = 0; i < n; ++i) out.push_back({prefix + std::to_string(i), cost});
    return out;
}
}  // namespace

static void test_idempotent_create_and_tenant_isolation()
{
    MemoryStore s;
    const auto c = make_campaign("adv1", 100 * kMicrosPerDollar);
    auto r1 = s.create(c, "k1", "hashA");
    CHECK(r1 && r1->created);
    auto other = c;
    other.campaign_id = new_id();
    auto r2 = s.create(other, "k1", "hashA");             // 重试: 同 key 同内容
    CHECK(r2 && !r2->created);
    CHECK_EQ(r2->campaign.campaign_id, c.campaign_id);     // 返回的是第一次创建的那个
    CHECK(!s.create(other, "k1", "hashB"));                // 同 key 不同内容
    CHECK(!s.get("adv2", c.campaign_id).has_value());      // 别的广告主看不到
}

static void test_optimistic_lock_and_budget_adjust()
{
    MemoryStore s;
    FixedClock clock{at_utc(12)};
    const auto c = make_campaign("adv", 100 * kMicrosPerDollar);
    (void)s.create(c, "k", "h");
    const auto today = utc_date(clock.now());
    (void)s.lease(c.campaign_id, today, c.daily_budget, 30 * kMicrosPerDollar);   // 当天计数器: 70 剩余
    CampaignPatch p;
    p.daily_budget = 150 * kMicrosPerDollar;
    auto u = s.update("adv", c.campaign_id, 1, p, today);
    CHECK(u && u->version == 2);
    CHECK_EQ(s.remaining(c.campaign_id, today).value_or(-1), 120 * kMicrosPerDollar);   // 70 + 50 (value_or 按值返回，避免引用悬空)
    CHECK(s.update("adv", c.campaign_id, 1, p, today).error() == StoreError::VersionConflict);   // 拿旧版本号改
    CHECK(s.update("adv", "nope", 2, p, today).error() == StoreError::NotFound);
}

static void test_no_overspend_under_concurrency()
{
    MemoryStore s{microseconds(50)};
    FixedClock clock{at_utc(12)};
    const Micros budget = 100 * kMicrosPerDollar;
    const auto c = make_campaign("adv", budget);
    (void)s.create(c, "k", "h");
    // 3 个 "实例" × 4 线程，需求是预算的 3 倍
    std::vector<std::unique_ptr<BudgetService>> svc;
    for (int i = 0; i < 3; ++i) svc.push_back(std::make_unique<BudgetService>(s, s, s, clock));
    std::atomic<Micros> accepted{0};
    {
        std::vector<std::jthread> pool;
        for (int i = 0; i < 12; ++i)
            pool.emplace_back([&, i] {
                for (int b = 0; b < 50; ++b) {
                    auto r = svc[static_cast<std::size_t>(i % 3)]->charge("adv", c.campaign_id,
                                                                          events(std::format("t{}b{}_", i, b), 10, 50'000));
                    if (r) accepted += r->accepted_total;
                }
            });
    }
    CHECK(accepted.load() <= budget);                      // 不超支
    CHECK(accepted.load() > budget * 9 / 10);              // 也没有少花太多 (没花完的都在各实例手里)
    for (auto& x : svc) x->release_all();
    const auto today = utc_date(clock.now());
    CHECK_EQ(*s.remaining(c.campaign_id, today) + s.spent(c.campaign_id, today), budget);   // 守恒
}

static void test_duplicates_are_not_double_charged()
{
    MemoryStore s;
    FixedClock clock{at_utc(12)};
    const auto c = make_campaign("adv", 100 * kMicrosPerDollar);
    (void)s.create(c, "k", "h");
    BudgetService svc{s, s, s, clock};
    std::vector<ChargeEvent> b = {{"a", 1000}, {"b", 2000}, {"a", 1000}};   // 同一批里 a 出现两次
    auto r1 = svc.charge("adv", c.campaign_id, b);
    CHECK_EQ(r1->accepted_total, 3000);
    CHECK(r1->outcomes[2] == ChargeOutcome::Duplicate);
    auto r2 = svc.charge("adv", c.campaign_id, b);                          // 整批重试
    CHECK_EQ(r2->accepted_total, 0);
    CHECK(r2->outcomes[0] == ChargeOutcome::Duplicate);
    auto v = svc.view("adv", c.campaign_id);
    CHECK(v && v->consistent);
    CHECK_EQ(v->ledger_spent, 3000);
}

// 回归测试: 去重范围是 campaign 内。之前用 event_id 单独做主键，别的 campaign 的同名事件会被误判成重复 → 漏收钱
static void test_same_event_id_in_different_campaigns()
{
    MemoryStore s;
    FixedClock clock{at_utc(12)};
    const auto c1 = make_campaign("adv", 100 * kMicrosPerDollar);
    const auto c2 = make_campaign("other-adv", 100 * kMicrosPerDollar);
    (void)s.create(c1, "k1", "h");
    (void)s.create(c2, "k2", "h");
    BudgetService svc{s, s, s, clock};
    const std::vector<ChargeEvent> b = {{"click-001", 1000}};
    CHECK_EQ(svc.charge("adv", c1.campaign_id, b)->accepted_total, 1000);
    CHECK_EQ(svc.charge("other-adv", c2.campaign_id, b)->accepted_total, 1000);   // 不是重复
    CHECK_EQ(svc.charge("adv", c1.campaign_id, b)->accepted_total, 0);            // 同一个 campaign 里才是重复
}

static void test_pacing()
{
    MemoryStore s;
    FixedClock clock{at_utc(6)};   // 一天过去 25% → 最多花 25% × 1.1 + 2% = 29.5%
    const Micros budget = 100 * kMicrosPerDollar;
    const auto c = make_campaign("adv", budget, Pacing::Standard);
    (void)s.create(c, "k", "h");
    BudgetService svc{s, s, s, clock};
    Micros total = 0;
    int pacing_limited = 0;
    for (int b = 0; b < 100; ++b) {
        auto r = svc.charge("adv", c.campaign_id, events(std::format("p{}_", b), 10, 100'000));   // 每批 $1
        total += r->accepted_total;
        for (auto o : r->outcomes) pacing_limited += o == ChargeOutcome::PacingLimited;
    }
    CHECK(total <= budget * 295 / 1000);
    CHECK(total >= budget * 250 / 1000);
    CHECK(pacing_limited > 0);
    clock.set(at_utc(23));         // 到了晚上，又可以花了
    auto r = svc.charge("adv", c.campaign_id, events("late_", 10, 100'000));
    CHECK_EQ(r->accepted_total, 1'000'000);
}

static void test_paused_campaign()
{
    MemoryStore s;
    FixedClock clock{at_utc(12)};
    auto c = make_campaign("adv", 100 * kMicrosPerDollar);
    c.status = CampaignStatus::Paused;
    (void)s.create(c, "k", "h");
    BudgetService svc{s, s, s, clock};
    auto r = svc.charge("adv", c.campaign_id, events("x", 3, 1000));
    CHECK(r->outcomes[0] == ChargeOutcome::CampaignNotActive);
    CHECK(!svc.charge("other-adv", c.campaign_id, events("y", 1, 1000)));   // 别人的 campaign → NotFound
}

// HTTP 层: 起一个真实的 server (随机端口)，用 httplib::Client 调
static void test_http_api()
{
    MemoryStore s;
    FixedClock clock{at_utc(12)};
    BudgetService budget{s, s, s, clock};
    httplib::Server svr;
    register_routes(svr, {s, s, budget, clock});
    const int port = svr.bind_to_any_port("127.0.0.1");
    std::jthread t([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    httplib::Client cli("127.0.0.1", port);
    const httplib::Headers h{{"X-Advertiser-Id", "adv1"}, {"Idempotency-Key", "create-1"}};
    const std::string body = R"({"name":"Fall sale","daily_budget_micros":50000000})";

    CHECK_EQ(cli.Post("/v1/campaigns", body, "application/json")->status, 401);   // 没有身份
    auto r1 = cli.Post("/v1/campaigns", h, body, "application/json");
    CHECK_EQ(r1->status, 201);
    const auto id = nlohmann::json::parse(r1->body)["campaign_id"].get<std::string>();
    auto r2 = cli.Post("/v1/campaigns", h, body, "application/json");
    CHECK_EQ(r2->status, 200);
    CHECK_EQ(r2->get_header_value("Idempotent-Replayed"), std::string("true"));
    CHECK_EQ(cli.Post("/v1/campaigns", h, R"({"name":"other","daily_budget_micros":50000000})", "application/json")->status, 422);

    CHECK_EQ(cli.Get("/v1/campaigns/" + id, httplib::Headers{{"X-Advertiser-Id", "adv2"}})->status, 404);   // 租户隔离

    const httplib::Headers adv{{"X-Advertiser-Id", "adv1"}};
    CHECK_EQ(cli.Patch("/v1/campaigns/" + id, adv, R"({"status":"PAUSED"})", "application/json")->status, 428);
    httplib::Headers m1 = adv;
    m1.emplace("If-Match", "\"1\"");
    CHECK_EQ(cli.Patch("/v1/campaigns/" + id, m1, R"({"daily_budget_micros":80000000})", "application/json")->status, 200);
    auto stale = cli.Patch("/v1/campaigns/" + id, m1, R"({"status":"PAUSED"})", "application/json");
    CHECK_EQ(stale->status, 409);
    CHECK_EQ(nlohmann::json::parse(stale->body)["current_version"].get<int>(), 2);

    CHECK_EQ(cli.Put("/v1/campaigns/" + id + "/keywords/running-shoes", adv, R"({"bid_micros":1500000})", "application/json")->status, 200);
    CHECK_EQ(nlohmann::json::parse(cli.Get("/v1/campaigns/" + id + "/keywords", adv)->body)["keywords"].size(), 1u);

    auto ch = cli.Post("/v1/campaigns/" + id + "/charges", adv,
                       R"({"events":[{"event_id":"c1","cost_micros":250000},{"event_id":"c1","cost_micros":250000},{"event_id":"bad id","cost_micros":1}]})",
                       "application/json");
    CHECK_EQ(ch->status, 200);
    const auto cj = nlohmann::json::parse(ch->body);
    CHECK_EQ(cj["accepted_micros"].get<std::int64_t>(), 250000);
    CHECK_EQ(cj["summary"]["DUPLICATE"].get<int>(), 1);
    CHECK_EQ(cj["summary"]["INVALID"].get<int>(), 1);

    const auto bj = nlohmann::json::parse(cli.Get("/v1/campaigns/" + id + "/budget", adv)->body);
    CHECK(bj["consistent"].get<bool>());
    CHECK_EQ(bj["daily_budget_micros"].get<std::int64_t>(), 80'000'000);

    const auto rep = nlohmann::json::parse(cli.Get("/v1/reports/daily-spend?from=2026-10-24&to=2026-10-24", adv)->body);
    CHECK_EQ(rep["rows"].size(), 1u);
    CHECK_EQ(rep["rows"][0]["spend_micros"].get<std::int64_t>(), 250000);
    svr.stop();
}

int main()
{
    test_idempotent_create_and_tenant_isolation();
    test_optimistic_lock_and_budget_adjust();
    test_no_overspend_under_concurrency();
    test_duplicates_are_not_double_charged();
    test_same_event_id_in_different_campaigns();
    test_pacing();
    test_paused_campaign();
    test_http_api();
    return TEST_MAIN_RESULT();
}
