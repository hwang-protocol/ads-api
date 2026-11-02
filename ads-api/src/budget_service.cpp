#include "ads/budget_service.h"

#include <algorithm>
#include <unordered_set>

namespace ads {

std::string_view to_string(ChargeOutcome o)
{
    switch (o) {
    case ChargeOutcome::Accepted: return "ACCEPTED";
    case ChargeOutcome::Duplicate: return "DUPLICATE";
    case ChargeOutcome::BudgetExhausted: return "BUDGET_EXHAUSTED";
    case ChargeOutcome::PacingLimited: return "PACING_LIMITED";
    case ChargeOutcome::CampaignNotActive: return "CAMPAIGN_NOT_ACTIVE";
    case ChargeOutcome::Invalid: return "INVALID";
    }
    return "?";
}

BudgetService::BudgetService(CampaignStore& campaigns, BudgetStore& budgets, LedgerStore& ledger, const Clock& clock,
                             BudgetConfig cfg)
    : campaigns_{campaigns}, budgets_{budgets}, ledger_{ledger}, clock_{clock}, cfg_{cfg}
{
}

BudgetService::~BudgetService()
{
    try {
        release_all();
    } catch (...) {   // 析构函数不能抛异常；还不回去的部分当天少花，不会超支
    }
}

BudgetService::Lease& BudgetService::lease_for(const std::string& campaign_id)
{
    {
        std::shared_lock lock{leases_m_};
        if (auto it = leases_.find(campaign_id); it != leases_.end()) return *it->second;
    }
    std::unique_lock lock{leases_m_};
    auto [it, inserted] = leases_.try_emplace(campaign_id, nullptr);
    if (inserted) it->second = std::make_unique<Lease>();   // unique_ptr: map 重排时 Lease (含 mutex) 地址不变
    return *it->second;
}

std::optional<Campaign> BudgetService::campaign(const std::string& advertiser_id, const std::string& campaign_id)
{
    const auto now = std::chrono::steady_clock::now();
    const auto key = std::pair{advertiser_id, campaign_id};
    {
        std::lock_guard lock{cache_m_};
        if (auto it = cache_.find(key); it != cache_.end() && now - it->second.fetched < cfg_.campaign_cache_ttl)
            return it->second.c;
    }
    auto c = campaigns_.get(advertiser_id, campaign_id);   // 在锁外做 I/O
    if (c) {
        std::lock_guard lock{cache_m_};
        cache_[key] = {*c, now};
    }
    return c;
}

void BudgetService::invalidate(const std::string& campaign_id)
{
    std::lock_guard lock{cache_m_};
    std::erase_if(cache_, [&](const auto& kv) { return kv.first.second == campaign_id; });
}

void BudgetService::roll_over(Lease& l, const std::string& campaign_id, const std::string& today)
{
    if (l.date == today) return;
    if (!l.date.empty() && l.local > 0) budgets_.give_back(campaign_id, l.date, l.local);   // 昨天没花完的还给昨天
    l.date = today;
    l.local = 0;
    l.last_remaining = -1;
    l.exhausted_until = {};
}

std::expected<ChargeResult, StoreError> BudgetService::charge(const std::string& advertiser_id, const std::string& campaign_id,
                                                              std::span<const ChargeEvent> events)
{
    const auto c = campaign(advertiser_id, campaign_id);
    if (!c) return std::unexpected(StoreError::NotFound);

    ChargeResult res;
    res.outcomes.assign(events.size(), ChargeOutcome::Accepted);
    if (c->status != CampaignStatus::Enabled) {
        std::ranges::fill(res.outcomes, ChargeOutcome::CampaignNotActive);
        return res;
    }

    // 同一批里重复的 event_id、金额不合法的先标出来
    std::unordered_set<std::string_view> seen;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].cost <= 0 || !validate_id(events[i].event_id, "event_id").empty()) res.outcomes[i] = ChargeOutcome::Invalid;
        else if (!seen.insert(events[i].event_id).second) res.outcomes[i] = ChargeOutcome::Duplicate;
    }

    const auto wall = clock_.now();
    const auto today = utc_date(wall);
    const Micros budget = c->daily_budget;
    const Micros chunk = static_cast<Micros>(static_cast<double>(budget) * cfg_.chunk_fraction);
    const double pace_limit = std::min(1.0, fraction_of_day(wall) * (1.0 + cfg_.pacing_slack) + cfg_.pacing_floor);
    const auto allowed_by_now = static_cast<Micros>(static_cast<double>(budget) * pace_limit);

    Lease& l = lease_for(campaign_id);
    std::vector<ChargeEvent> accepted;
    std::vector<std::size_t> accepted_idx;
    {
        // 同一个 campaign 在本实例上串行扣费；批发 (DynamoDB 调用) 也在锁里 → 同一时刻只有一个线程去批发 (single flight)
        std::lock_guard lock{l.m};
        roll_over(l, campaign_id, today);
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (res.outcomes[i] != ChargeOutcome::Accepted) continue;
            const Micros cost = events[i].cost;
            if (c->pacing == Pacing::Standard && l.last_remaining >= 0) {
                // 全局已消耗 ≈ 日预算 − 计数器余额 − 我手里还没花的 (其它实例手里没花的也算进去了 → 偏保守)
                const Micros consumed = budget - l.last_remaining - l.local;
                if (consumed + cost > allowed_by_now) {
                    res.outcomes[i] = ChargeOutcome::PacingLimited;
                    continue;
                }
            }
            if (l.local < cost) {
                if (std::chrono::steady_clock::now() < l.exhausted_until) {
                    res.outcomes[i] = ChargeOutcome::BudgetExhausted;
                    continue;
                }
                const Micros want = std::min(budget, std::max(chunk, cost - l.local));
                const auto g = budgets_.lease(campaign_id, today, budget, want);
                l.local += g.granted;
                l.last_remaining = g.remaining_after;
                if (l.local < cost) {
                    l.exhausted_until = std::chrono::steady_clock::now() + cfg_.exhausted_backoff;
                    res.outcomes[i] = ChargeOutcome::BudgetExhausted;
                    continue;
                }
            }
            l.local -= cost;   // 先在本地 "预扣"，记账成功才算真的花了
            accepted.push_back(events[i]);
            accepted_idx.push_back(i);
        }
    }

    // 记账 (按 event_id 去重)。失败 → 预扣全部退回本地余额，告诉调用方稍后重试
    std::vector<bool> inserted;
    try {
        inserted = accepted.empty() ? std::vector<bool>{} : ledger_.record(advertiser_id, campaign_id, today, accepted);
    } catch (...) {
        std::lock_guard lock{l.m};
        for (const auto& e : accepted) l.local += e.cost;
        throw;
    }
    Micros refund = 0;
    for (std::size_t k = 0; k < accepted.size(); ++k) {
        if (inserted[k]) {
            res.accepted_total += accepted[k].cost;
        } else {   // 之前的批次已经记过这个 event_id (调用方重试了) → 不重复扣钱
            res.outcomes[accepted_idx[k]] = ChargeOutcome::Duplicate;
            refund += accepted[k].cost;
        }
    }
    if (refund > 0) {
        std::lock_guard lock{l.m};
        if (l.date == today) l.local += refund;
        else budgets_.give_back(campaign_id, today, refund);   // 极端情况: 这期间跨天了
    }
    return res;
}

std::expected<BudgetView, StoreError> BudgetService::view(const std::string& advertiser_id, const std::string& campaign_id)
{
    const auto c = campaigns_.get(advertiser_id, campaign_id);   // 不走缓存: 对账要最新值
    if (!c) return std::unexpected(StoreError::NotFound);
    BudgetView v;
    v.date = utc_date(clock_.now());
    v.daily_budget = c->daily_budget;
    v.store_remaining = budgets_.remaining(campaign_id, v.date).value_or(c->daily_budget);
    {
        Lease& l = lease_for(campaign_id);
        std::lock_guard lock{l.m};
        v.local_unspent = l.date == v.date ? l.local : 0;
    }
    v.ledger_spent = ledger_.spent(campaign_id, v.date);
    v.consistent = v.ledger_spent + v.store_remaining + v.local_unspent == v.daily_budget;
    return v;
}

void BudgetService::release_all()
{
    std::shared_lock lock{leases_m_};
    for (auto& [cid, l] : leases_) {
        std::lock_guard ll{l->m};
        if (l->local > 0) {
            budgets_.give_back(cid, l->date, l->local);
            l->local = 0;
        }
    }
}

Micros BudgetService::local_unspent_total()
{
    std::shared_lock lock{leases_m_};
    Micros total = 0;
    for (auto& [cid, l] : leases_) {
        std::lock_guard ll{l->m};
        total += l->local;
    }
    return total;
}

}  // namespace ads
