#include "ads/memory_store.h"

#include <algorithm>
#include <thread>

namespace ads {

void MemoryStore::simulate_latency()
{
    ++calls_;
    if (latency_.count() > 0) std::this_thread::sleep_for(latency_);   // 在锁外睡，和真实网络调用一样不阻塞别人
}

std::expected<CreateResult, StoreError> MemoryStore::create(const Campaign& c, const std::string& idempotency_key,
                                                            const std::string& request_hash)
{
    std::lock_guard lock{m_};
    const auto ik = std::pair{c.advertiser_id, idempotency_key};
    if (auto it = idem_.find(ik); it != idem_.end()) {
        if (it->second.first != request_hash) return std::unexpected(StoreError::IdempotencyMismatch);
        return CreateResult{campaigns_.at({c.advertiser_id, it->second.second}), false};
    }
    campaigns_[{c.advertiser_id, c.campaign_id}] = c;
    idem_[ik] = {request_hash, c.campaign_id};
    return CreateResult{c, true};
}

std::optional<Campaign> MemoryStore::get(const std::string& advertiser_id, const std::string& campaign_id)
{
    std::lock_guard lock{m_};
    auto it = campaigns_.find({advertiser_id, campaign_id});
    if (it == campaigns_.end()) return std::nullopt;
    return it->second;
}

std::vector<Campaign> MemoryStore::list(const std::string& advertiser_id)
{
    std::lock_guard lock{m_};
    std::vector<Campaign> out;
    for (const auto& [k, c] : campaigns_)
        if (k.first == advertiser_id) out.push_back(c);
    return out;
}

std::expected<Campaign, StoreError> MemoryStore::update(const std::string& advertiser_id, const std::string& campaign_id,
                                                        std::int64_t expected_version, const CampaignPatch& patch,
                                                        const std::string& today)
{
    std::lock_guard lock{m_};   // 一把锁 = 和 DynamoDB TransactWriteItems 一样的原子性
    auto it = campaigns_.find({advertiser_id, campaign_id});
    if (it == campaigns_.end()) return std::unexpected(StoreError::NotFound);
    Campaign& c = it->second;
    if (c.version != expected_version) return std::unexpected(StoreError::VersionConflict);
    if (patch.daily_budget) {
        // 当天预算计数器已存在 → 加上差额；不存在 → 以后第一次 lease 时用新预算初始化
        if (auto b = budgets_.find({campaign_id, today}); b != budgets_.end()) b->second += *patch.daily_budget - c.daily_budget;
        c.daily_budget = *patch.daily_budget;
    }
    if (patch.name) c.name = *patch.name;
    if (patch.status) c.status = *patch.status;
    if (patch.pacing) c.pacing = *patch.pacing;
    ++c.version;
    return c;
}

void MemoryStore::put_keyword(const Keyword& k)
{
    std::lock_guard lock{m_};
    keywords_[{k.campaign_id, k.text}] = k;
}

std::vector<Keyword> MemoryStore::list_keywords(const std::string& campaign_id)
{
    std::lock_guard lock{m_};
    std::vector<Keyword> out;
    for (const auto& [key, k] : keywords_)
        if (key.first == campaign_id) out.push_back(k);
    return out;
}

BudgetStore::Grant MemoryStore::lease(const std::string& campaign_id, const std::string& date, Micros daily_budget, Micros want)
{
    simulate_latency();
    std::lock_guard lock{m_};
    auto [it, inserted] = budgets_.try_emplace({campaign_id, date}, daily_budget);
    const Micros granted = std::clamp<Micros>(it->second, 0, want);
    it->second -= granted;
    return {granted, it->second};
}

void MemoryStore::give_back(const std::string& campaign_id, const std::string& date, Micros amount)
{
    simulate_latency();
    std::lock_guard lock{m_};
    budgets_[{campaign_id, date}] += amount;
}

std::optional<Micros> MemoryStore::remaining(const std::string& campaign_id, const std::string& date)
{
    std::lock_guard lock{m_};
    auto it = budgets_.find({campaign_id, date});
    if (it == budgets_.end()) return std::nullopt;
    return it->second;
}

std::vector<bool> MemoryStore::record(const std::string& advertiser_id, const std::string& campaign_id,
                                      const std::string& date, std::span<const ChargeEvent> events)
{
    std::lock_guard lock{m_};
    std::vector<bool> inserted;
    inserted.reserve(events.size());
    for (const auto& e : events)
        inserted.push_back(ledger_.try_emplace({campaign_id, e.event_id}, LedgerRow{advertiser_id, campaign_id, date, e.cost}).second);
    return inserted;
}

Micros MemoryStore::spent(const std::string& campaign_id, const std::string& date)
{
    std::lock_guard lock{m_};
    Micros total = 0;
    for (const auto& [id, r] : ledger_)
        if (r.campaign_id == campaign_id && r.date == date) total += r.cost;
    return total;
}

std::vector<DailySpendRow> MemoryStore::report(const std::string& advertiser_id, const std::string& from, const std::string& to)
{
    // 和 PgLedger 的 SQL (GROUP BY + 两个窗口函数) 算同样的东西
    std::map<std::pair<std::string, std::string>, DailySpendRow> agg;   // (date, campaign)
    {
        std::lock_guard lock{m_};
        for (const auto& [id, r] : ledger_) {
            if (r.advertiser_id != advertiser_id || r.date < from || r.date > to) continue;
            auto& row = agg[{r.date, r.campaign_id}];
            row.date = r.date;
            row.campaign_id = r.campaign_id;
            ++row.events;
            row.spend += r.cost;
        }
    }
    std::map<std::string, Micros> day_total, running;
    for (const auto& [k, row] : agg) day_total[row.date] += row.spend;
    std::vector<DailySpendRow> out;
    for (auto& [k, row] : agg) {   // map 按 (date, campaign) 排序 → 按日期累计
        row.share_of_advertiser = static_cast<double>(row.spend) / static_cast<double>(day_total[row.date]);
        row.running_total = (running[row.campaign_id] += row.spend);
        out.push_back(row);
    }
    std::ranges::stable_sort(out, [](const auto& a, const auto& b) { return a.date != b.date ? a.date < b.date : a.spend > b.spend; });
    return out;
}

}  // namespace ads
