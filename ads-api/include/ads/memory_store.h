// 三个存储接口的内存实现 (一把锁)，用于单元测试和本机 --memory 模式
// latency 参数: 每次 BudgetStore 调用前睡一下，模拟 DynamoDB 的网络往返 (benchmark 用)
#pragma once
#include "ads/stores.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <unordered_map>

namespace ads {

class MemoryStore final : public CampaignStore, public BudgetStore, public LedgerStore {
public:
    explicit MemoryStore(std::chrono::microseconds budget_latency = std::chrono::microseconds{0})
        : latency_{budget_latency} {}

    // CampaignStore
    std::expected<CreateResult, StoreError> create(const Campaign& c, const std::string& idempotency_key,
                                                   const std::string& request_hash) override;
    std::optional<Campaign> get(const std::string& advertiser_id, const std::string& campaign_id) override;
    std::vector<Campaign> list(const std::string& advertiser_id) override;
    std::expected<Campaign, StoreError> update(const std::string& advertiser_id, const std::string& campaign_id,
                                               std::int64_t expected_version, const CampaignPatch& patch,
                                               const std::string& today) override;
    void put_keyword(const Keyword& k) override;
    std::vector<Keyword> list_keywords(const std::string& campaign_id) override;

    // BudgetStore
    Grant lease(const std::string& campaign_id, const std::string& date, Micros daily_budget, Micros want) override;
    void give_back(const std::string& campaign_id, const std::string& date, Micros amount) override;
    std::optional<Micros> remaining(const std::string& campaign_id, const std::string& date) override;
    std::int64_t calls() const override { return calls_.load(); }

    // LedgerStore
    std::vector<bool> record(const std::string& advertiser_id, const std::string& campaign_id, const std::string& date,
                             std::span<const ChargeEvent> events) override;
    Micros spent(const std::string& campaign_id, const std::string& date) override;
    std::vector<DailySpendRow> report(const std::string& advertiser_id, const std::string& from,
                                      const std::string& to) override;

private:
    struct LedgerRow {
        std::string advertiser_id, campaign_id, date;
        Micros cost;
    };
    void simulate_latency();

    std::mutex m_;
    std::map<std::pair<std::string, std::string>, Campaign> campaigns_;                       // (adv, id)
    std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>> idem_;  // (adv, key) → (hash, id)
    std::map<std::pair<std::string, std::string>, Keyword> keywords_;                         // (campaign, text)
    std::map<std::pair<std::string, std::string>, Micros> budgets_;                           // (campaign, date) → remaining
    std::map<std::pair<std::string, std::string>, LedgerRow> ledger_;                          // (campaign, event_id) → row
    std::chrono::microseconds latency_;
    std::atomic<std::int64_t> calls_{0};
};

}  // namespace ads
