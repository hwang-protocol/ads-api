// 存储接口 (ports & adapters / 六边形架构):
//   业务逻辑 (budget_service、api) 只依赖这些接口
//   MemoryStore   → 单元测试、本机 --memory 模式
//   DynamoStore   → DynamoDB (Campaign / Keyword / 当日预算 / 幂等记录 都在同一张表，单表设计)
//   PgLedger      → PostgreSQL 记账流水 (计费、报表、对账需要 SQL 的聚合和窗口函数)
// 好处: 核心逻辑在 macOS 上不装 AWS SDK 也能编译、测试；换存储不改业务代码
#pragma once
#include "ads/model.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace ads {

enum class StoreError { NotFound, VersionConflict, IdempotencyMismatch };

// 后端暂时不可用 (DynamoDB 限流、PostgreSQL 断线)：API 返回 503，调用方稍后重试
struct Unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct CreateResult {
    Campaign campaign;
    bool created;   // false = 同一个幂等键之前已经创建过，这次返回的是当时的结果
};

class CampaignStore {
public:
    virtual ~CampaignStore() = default;

    // 幂等创建: 同一个 (advertiser, key) 第二次调用返回第一次的结果；
    // 同一个 key 但请求内容不同 (request_hash 不一样) → IdempotencyMismatch
    virtual std::expected<CreateResult, StoreError> create(const Campaign& c, const std::string& idempotency_key,
                                                           const std::string& request_hash) = 0;
    virtual std::optional<Campaign> get(const std::string& advertiser_id, const std::string& campaign_id) = 0;
    virtual std::vector<Campaign> list(const std::string& advertiser_id) = 0;

    // 乐观锁更新: 只有当前 version == expected_version 才写入
    // 如果改了日预算，同一个原子操作里把当天剩余预算也调整 (差额)，today = 当天日期
    virtual std::expected<Campaign, StoreError> update(const std::string& advertiser_id, const std::string& campaign_id,
                                                       std::int64_t expected_version, const CampaignPatch& patch,
                                                       const std::string& today) = 0;

    virtual void put_keyword(const Keyword& k) = 0;
    virtual std::vector<Keyword> list_keywords(const std::string& campaign_id) = 0;
};

// 每个 campaign 每天一个 "剩余预算" 计数器
class BudgetStore {
public:
    virtual ~BudgetStore() = default;

    // 从当天剩余预算里拿 want (不存在就先用 daily_budget 初始化)。
    // 剩余不够时拿走全部剩余；返回实际拿到的金额 (0 ~ want) 和拿完之后的剩余
    struct Grant {
        Micros granted;
        Micros remaining_after;
    };
    virtual Grant lease(const std::string& campaign_id, const std::string& date, Micros daily_budget, Micros want) = 0;
    virtual void give_back(const std::string& campaign_id, const std::string& date, Micros amount) = 0;
    virtual std::optional<Micros> remaining(const std::string& campaign_id, const std::string& date) = 0;
    virtual std::int64_t calls() const = 0;   // 调用次数 (benchmark 用)
};

struct DailySpendRow {
    std::string date;
    std::string campaign_id;
    std::int64_t events{0};
    Micros spend{0};
    double share_of_advertiser{0};   // 当天占该广告主总花费的比例
    Micros running_total{0};         // 该 campaign 从区间开始累计到这一天
};

// 记账流水: 只追加、按 event_id 去重
class LedgerStore {
public:
    virtual ~LedgerStore() = default;
    // 返回值与 events 一一对应: true = 新记录，false = event_id 已存在 (重复)
    virtual std::vector<bool> record(const std::string& advertiser_id, const std::string& campaign_id,
                                     const std::string& date, std::span<const ChargeEvent> events) = 0;
    virtual Micros spent(const std::string& campaign_id, const std::string& date) = 0;
    virtual std::vector<DailySpendRow> report(const std::string& advertiser_id, const std::string& from,
                                              const std::string& to) = 0;
};

}  // namespace ads
