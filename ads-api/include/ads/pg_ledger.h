// PostgreSQL 记账流水 (libpq)
//   spend_ledger: 每次扣费一行，主键 (campaign_id, event_id) → INSERT ... ON CONFLICT DO NOTHING RETURNING 天然去重
//   去重范围是 campaign 内: 不同 campaign 的 event_id 相同不算重复 (只用 event_id 做主键会把别的 campaign 的点击误判成重复，漏收钱)
//   为什么花费明细放 PostgreSQL 而不是 DynamoDB:
//     计费、报表、对账要按天 / 按广告主聚合、做窗口函数、和别的表 JOIN —— 这是关系数据库的强项；
//     DynamoDB 适合按 key 的高频读写 (预算计数器)，不适合即席查询
// 连接池: libpq 的连接不能多个线程同时用，所以每个请求从池里借一个，用完还回去
// 连接参数: 空字符串 → libpq 自动读 PGHOST / PGUSER / PGPASSWORD / PGDATABASE 环境变量
//           (在 ECS 上由 Secrets Manager 注入成环境变量，代码里不出现密码)
#pragma once
#include "ads/stores.h"

#include <memory>

namespace ads {

class PgLedger final : public LedgerStore {
public:
    explicit PgLedger(const std::string& conninfo = "", int pool_size = 8);
    ~PgLedger() override;

    void migrate();   // 建表、建索引 (advisory lock 保证多个实例同时启动不会互相冲突)

    std::vector<bool> record(const std::string& advertiser_id, const std::string& campaign_id, const std::string& date,
                             std::span<const ChargeEvent> events) override;
    Micros spent(const std::string& campaign_id, const std::string& date) override;
    std::vector<DailySpendRow> report(const std::string& advertiser_id, const std::string& from,
                                      const std::string& to) override;

private:
    struct Pool;
    std::unique_ptr<Pool> pool_;
};

}  // namespace ads
