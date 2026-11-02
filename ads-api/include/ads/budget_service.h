// 预算控制 (budget pacing) —— 广告系统最经典的并发问题: 多台机器同时扣同一个预算，怎么保证不超支？
//
// 方案: 预算租约 (lease)
//   DynamoDB 里每个 campaign 每天一个 "剩余预算" 计数器 (全局真相)
//   每个服务实例从计数器里一次性 "批发" 一块 (比如日预算的 2%)，放在本地内存里，之后的扣费只在本地做
//   本地用完再去批发；关机时把没花完的还回去
//   → 永不超支 (所有实例本地余额之和 + 计数器余额 = 日预算 − 已花)
//   → DynamoDB 调用次数从 "每次点击一次" 降到 "每 2% 预算一次"，热点 campaign 不会把单个分区打爆
//   代价: 某个实例崩溃时，它手里没花完的租约 "丢了"，当天少花这部分 (少花可以接受，超支不行)
//
// 和 Millburn 的风控限额是同一个问题: 多个策略 / 分片共享一个总限额
//
// 匀速投放 (Standard pacing): 截至现在最多花 日预算 × min(1, 时间进度 × 1.1 + 2%)
//   否则早上 9 点就把一天的预算花光，下午用户最多的时候没钱了
#pragma once
#include "ads/stores.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace ads {

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::chrono::system_clock::time_point now() const = 0;
};

class SystemClock final : public Clock {
public:
    std::chrono::system_clock::time_point now() const override { return std::chrono::system_clock::now(); }
};

// 测试用: 时间由测试控制
class FixedClock final : public Clock {
public:
    explicit FixedClock(std::chrono::system_clock::time_point t) : t_{t} {}
    std::chrono::system_clock::time_point now() const override
    {
        std::lock_guard lock{m_};
        return t_;
    }
    void set(std::chrono::system_clock::time_point t)
    {
        std::lock_guard lock{m_};
        t_ = t;
    }

private:
    mutable std::mutex m_;
    std::chrono::system_clock::time_point t_;
};

struct BudgetConfig {
    double chunk_fraction = 0.02;   // 每次批发日预算的多少；0 = 不批发，每次只拿正好够的 (对照组)
    double pacing_slack = 0.10;
    double pacing_floor = 0.02;
    std::chrono::milliseconds campaign_cache_ttl{1000};   // campaign 配置本地缓存多久 (暂停后最多晚这么久生效)
    std::chrono::milliseconds exhausted_backoff{1000};    // 预算花完后多久再去问一次 (避免一直打 DynamoDB)
};

enum class ChargeOutcome { Accepted, Duplicate, BudgetExhausted, PacingLimited, CampaignNotActive, Invalid };
std::string_view to_string(ChargeOutcome o);

struct ChargeResult {
    std::vector<ChargeOutcome> outcomes;   // 和输入一一对应
    Micros accepted_total{0};
};

struct BudgetView {
    std::string date;
    Micros daily_budget{0};
    Micros store_remaining{0};   // 计数器里还剩多少 (还没被任何实例批发走)
    Micros local_unspent{0};     // 本实例手里批发了但还没花的
    Micros ledger_spent{0};      // 记账流水里今天实际花了多少
    bool consistent{false};      // 单实例时: 三者之和 == 日预算
};

class BudgetService {
public:
    BudgetService(CampaignStore& campaigns, BudgetStore& budgets, LedgerStore& ledger, const Clock& clock,
                  BudgetConfig cfg = {});
    ~BudgetService();

    std::expected<ChargeResult, StoreError> charge(const std::string& advertiser_id, const std::string& campaign_id,
                                                   std::span<const ChargeEvent> events);
    std::expected<BudgetView, StoreError> view(const std::string& advertiser_id, const std::string& campaign_id);

    void invalidate(const std::string& campaign_id);   // campaign 被修改后丢掉本地缓存
    void release_all();                                 // 优雅关机: 把本地没花完的租约还回去
    Micros local_unspent_total();

private:
    struct Lease {
        std::mutex m;
        std::string date;
        Micros local{0};             // 本地可花余额
        Micros last_remaining{-1};   // 上次批发后计数器的余额 (算全局花费进度用)；-1 = 未知
        std::chrono::steady_clock::time_point exhausted_until{};
    };
    struct CachedCampaign {
        Campaign c;
        std::chrono::steady_clock::time_point fetched;
    };

    Lease& lease_for(const std::string& campaign_id);
    std::optional<Campaign> campaign(const std::string& advertiser_id, const std::string& campaign_id);
    void roll_over(Lease& l, const std::string& campaign_id, const std::string& today);   // 需要持有 l.m

    CampaignStore& campaigns_;
    BudgetStore& budgets_;
    LedgerStore& ledger_;
    const Clock& clock_;
    BudgetConfig cfg_;

    std::shared_mutex leases_m_;   // 读多写少: 查找用共享锁，新建 campaign 的租约才用独占锁
    std::map<std::string, std::unique_ptr<Lease>> leases_;
    std::mutex cache_m_;
    std::map<std::pair<std::string, std::string>, CachedCampaign> cache_;
};

}  // namespace ads
