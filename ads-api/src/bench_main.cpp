// 预算租约 benchmark: 一个热门 campaign，多个服务实例 × 多个线程同时扣费
//   --chunk-fraction 0     对照组: 每次点击都去计数器扣一次 (= 每次一个 DynamoDB 写)
//   --chunk-fraction 0.02  租约: 每次批发 2% 日预算
// 两种模式都检查: 不超支 (接受的总额 <= 日预算) 和 守恒 (还回租约后: 计数器余额 + 已记账 = 日预算)
//
// 本机:   ./budget_bench --store memory --latency-us 800        (用 sleep 模拟 DynamoDB 往返)
// Docker: budget_bench --store dynamo                           (连 DynamoDB Local)
#include "ads/budget_service.h"
#include "ads/memory_store.h"
#include "common/cli.h"
#include "common/latency_histogram.h"

#ifdef ADS_WITH_AWS
#include "ads/dynamo_store.h"
#endif

#include <cstdlib>
#include <print>
#include <thread>

int main(int argc, char** argv)
{
    const common::Args args{argc, argv};
    const auto store_kind = args.str("--store", "memory");
    const int instances = args.num("--instances", 4);
    const int threads = args.num("--threads", 4);
    const int events = args.num("--events", 40'000);
    const int batch = args.num("--batch", 10);
    const ads::Micros budget_micros = args.num<ads::Micros>("--budget-dollars", 500) * ads::kMicrosPerDollar;
    const ads::Micros cost = args.num<ads::Micros>("--cost-micros", 20'000);   // 每次点击 2 美分
    ads::BudgetConfig cfg;
    cfg.chunk_fraction = args.num("--chunk-fraction", 0.02);

    ads::MemoryStore ledger;   // 记账固定用内存，只比较预算计数器这一层
#ifdef ADS_WITH_AWS
    std::optional<ads::AwsApiGuard> aws_api;
    std::unique_ptr<ads::DynamoStore> dynamo;
#endif
    std::unique_ptr<ads::MemoryStore> mem;
    ads::CampaignStore* campaigns = nullptr;
    ads::BudgetStore* budgets = nullptr;
    if (store_kind == "memory") {
        mem = std::make_unique<ads::MemoryStore>(std::chrono::microseconds(args.num("--latency-us", 800)));
        campaigns = mem.get();
        budgets = mem.get();
    }
#ifdef ADS_WITH_AWS
    else if (store_kind == "dynamo") {
        aws_api.emplace();
        const char* ep = std::getenv("DDB_ENDPOINT");
        dynamo = std::make_unique<ads::DynamoStore>(ads::DynamoConfig{"ads", "us-east-1", ep ? ep : ""});
        if (ep) dynamo->create_table_if_missing();
        campaigns = dynamo.get();
        budgets = dynamo.get();
    }
#endif
    else {
        std::println(stderr, "unknown --store {} (memory | dynamo; dynamo needs the Docker build)", store_kind);
        return 1;
    }

    ads::Campaign c;
    c.advertiser_id = "bench";
    c.campaign_id = ads::new_id();
    c.name = "hot campaign";
    c.daily_budget = budget_micros;
    c.pacing = ads::Pacing::Accelerated;   // 只测预算上限，不测匀速
    (void)campaigns->create(c, "bench-" + c.campaign_id, "h");

    ads::SystemClock clock;
    std::vector<std::unique_ptr<ads::BudgetService>> svc;   // 每个对象 = 一个服务实例 (各自的本地租约)
    for (int i = 0; i < instances; ++i) svc.push_back(std::make_unique<ads::BudgetService>(*campaigns, *budgets, ledger, clock, cfg));

    std::atomic<ads::Micros> accepted{0};
    std::vector<common::LatencyHistogram> hist(static_cast<std::size_t>(instances * threads));
    const int per_thread = events / (instances * threads);
    const auto t0 = common::now_ns();
    {
        std::vector<std::jthread> pool;
        for (int i = 0; i < instances; ++i)
            for (int t = 0; t < threads; ++t)
                pool.emplace_back([&, i, t] {
                    auto& h = hist[static_cast<std::size_t>(i * threads + t)];
                    std::vector<ads::ChargeEvent> b;
                    for (int k = 0; k < per_thread; k += batch) {
                        b.clear();
                        for (int j = k; j < std::min(per_thread, k + batch); ++j)
                            b.push_back({std::format("e{}_{}_{}", i, t, j), cost});
                        const auto s = common::now_ns();
                        auto r = svc[static_cast<std::size_t>(i)]->charge(c.advertiser_id, c.campaign_id, b);
                        h.record(common::now_ns() - s);
                        if (r) accepted += r->accepted_total;
                    }
                });
    }
    const double secs = static_cast<double>(common::now_ns() - t0) / 1e9;
    const auto calls_during = budgets->calls();
    for (auto& s : svc) s->release_all();

    common::LatencyHistogram all;
    for (auto& h : hist) all.merge(h);
    const auto today = ads::utc_date(clock.now());
    const auto remaining = budgets->remaining(c.campaign_id, today).value_or(budget_micros);
    const auto spent = ledger.spent(c.campaign_id, today);
    const auto demand = static_cast<ads::Micros>(per_thread) * instances * threads * cost;

    std::println("store={} instances={} threads/instance={} chunk_fraction={}", store_kind, instances, threads, cfg.chunk_fraction);
    std::println("demand=${:.2f}  budget=${:.2f}  accepted=${:.2f}", demand / 1e6, budget_micros / 1e6, accepted.load() / 1e6);
    std::println("budget-store calls={}  wall={:.2f}s  charge calls/s={:.0f}", calls_during, secs,
                 static_cast<double>(all.count()) / secs);
    std::println("charge() latency per batch of {}: {}", batch, all.summary("us"));
    std::println("overspend=${:.2f} (must be 0)", std::max<ads::Micros>(0, accepted.load() - budget_micros) / 1e6);
    std::println("conservation: remaining ${:.2f} + ledger ${:.2f} = ${:.2f} (must equal budget)", remaining / 1e6, spent / 1e6,
                 (remaining + spent) / 1e6);
    return accepted.load() <= budget_micros && remaining + spent == budget_micros ? 0 : 1;
}
