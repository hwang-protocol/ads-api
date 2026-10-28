// ads_api 服务入口
//   本机:   ./ads_api --memory --port 8080
//   Docker: 读环境变量 TABLE_NAME / AWS_REGION / DDB_ENDPOINT (本地 DynamoDB Local) / PG* (PostgreSQL)
// 收到 SIGTERM (ECS 停任务、滚动发布) → 停止接新请求 → 把没花完的预算租约还回 DynamoDB → 退出
#include "ads/api.h"
#include "ads/memory_store.h"
#include "common/cli.h"

#ifdef ADS_WITH_AWS
#include "ads/dynamo_store.h"
#include "ads/pg_ledger.h"
#endif

#include <httplib.h>

#include <csignal>
#include <cstdlib>
#include <print>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
extern "C" void on_signal(int) { g_stop.store(true); }

std::string env(const char* name, std::string def)
{
    const char* v = std::getenv(name);
    return v && *v ? v : def;
}
}  // namespace

int main(int argc, char** argv)
{
    const common::Args args{argc, argv};
    const int port = args.num("--port", std::stoi(env("PORT", "8080")));

#ifdef ADS_WITH_AWS
    const bool memory = args.has("--memory");
    std::optional<ads::AwsApiGuard> aws_api;   // 最先构造、最后析构: SDK client 必须在 ShutdownAPI 之前销毁
    std::unique_ptr<ads::DynamoStore> dynamo;
    std::unique_ptr<ads::PgLedger> pg;
#else
    const bool memory = true;
#endif
    std::unique_ptr<ads::MemoryStore> mem;
    ads::CampaignStore* campaigns = nullptr;
    ads::BudgetStore* budgets = nullptr;
    ads::LedgerStore* ledger = nullptr;

    if (memory) {
        mem = std::make_unique<ads::MemoryStore>();
        campaigns = mem.get();
        budgets = mem.get();
        ledger = mem.get();
        std::println("storage: in-memory");
    }
#ifdef ADS_WITH_AWS
    else {
        aws_api.emplace();
        const ads::DynamoConfig dc{env("TABLE_NAME", "ads"), env("AWS_REGION", "us-east-1"), env("DDB_ENDPOINT", "")};
        dynamo = std::make_unique<ads::DynamoStore>(dc);
        if (!dc.endpoint.empty()) dynamo->create_table_if_missing();   // 只有本地才自己建表
        pg = std::make_unique<ads::PgLedger>("", std::stoi(env("PG_POOL_SIZE", "8")));
        pg->migrate();
        campaigns = dynamo.get();
        budgets = dynamo.get();
        ledger = pg.get();
        std::println("storage: dynamodb table={} endpoint={} | postgres host={}", dc.table,
                     dc.endpoint.empty() ? "aws" : dc.endpoint, env("PGHOST", "?"));
    }
#endif

    ads::SystemClock clock;
    ads::BudgetConfig cfg;
    cfg.chunk_fraction = args.num("--chunk-fraction", 0.02);
    ads::BudgetService budget{*campaigns, *budgets, *ledger, clock, cfg};

    httplib::Server svr;
    ads::register_routes(svr, {*campaigns, *ledger, budget, clock});
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        // 一行一个 JSON: CloudWatch Logs Insights 可以直接按字段查询
        std::println(R"({{"method":"{}","path":"{}","status":{},"advertiser":"{}"}})", req.method, req.path, res.status,
                     req.get_header_value("X-Advertiser-Id"));
        // 容器里 stdout 不是终端 → 默认全缓冲，不 flush 的话日志要攒满 4KB 才出现在 docker logs / CloudWatch
        std::fflush(stdout);
    });

    std::signal(SIGTERM, on_signal);
    std::signal(SIGINT, on_signal);
    std::jthread watcher([&](std::stop_token st) {
        while (!g_stop.load() && !st.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        svr.stop();   // 信号处理函数里不能做这种事，所以由普通线程来做
    });

    std::println("ads_api listening on :{}", port);
    std::fflush(stdout);
    if (!svr.listen("0.0.0.0", port)) {
        std::println(stderr, "failed to listen on port {}", port);
        return 1;
    }
    const auto unspent = budget.local_unspent_total();
    budget.release_all();
    std::println("shutdown: returned {} micros of unspent budget leases", unspent);
    watcher.request_stop();
}
