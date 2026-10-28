#include "ads/model.h"

#include <algorithm>
#include <format>
#include <random>

namespace ads {

std::string_view to_string(CampaignStatus s)
{
    switch (s) {
    case CampaignStatus::Enabled: return "ENABLED";
    case CampaignStatus::Paused: return "PAUSED";
    case CampaignStatus::Archived: return "ARCHIVED";
    }
    return "?";
}

std::string_view to_string(Pacing p) { return p == Pacing::Standard ? "STANDARD" : "ACCELERATED"; }

std::optional<CampaignStatus> parse_status(std::string_view s)
{
    if (s == "ENABLED") return CampaignStatus::Enabled;
    if (s == "PAUSED") return CampaignStatus::Paused;
    if (s == "ARCHIVED") return CampaignStatus::Archived;
    return std::nullopt;
}

std::optional<Pacing> parse_pacing(std::string_view s)
{
    if (s == "STANDARD") return Pacing::Standard;
    if (s == "ACCELERATED") return Pacing::Accelerated;
    return std::nullopt;
}

nlohmann::json to_json(const Campaign& c)
{
    return {{"campaign_id", c.campaign_id},   {"name", c.name},
            {"daily_budget_micros", c.daily_budget}, {"status", to_string(c.status)},
            {"pacing", to_string(c.pacing)},  {"version", c.version},
            {"created_at", c.created_at}};
}

nlohmann::json to_json(const Keyword& k)
{
    return {{"campaign_id", k.campaign_id}, {"text", k.text}, {"bid_micros", k.bid}};
}

std::string validate_id(std::string_view id, std::string_view what)
{
    const bool ok = !id.empty() && id.size() <= 64 &&
                    std::ranges::all_of(id, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
    // 只允许这些字符: id 会被拼进 DynamoDB 的 key ("ADV#xxx") 和 SQL 数组字面量，白名单比转义安全
    return ok ? "" : std::format("{} must match [A-Za-z0-9_-]{{1,64}}", what);
}

std::string validate_budget(Micros m)
{
    if (m < kMicrosPerDollar || m > 10'000'000 * kMicrosPerDollar) return "daily_budget_micros must be between $1 and $10M";
    return "";
}

std::string validate_name(std::string_view name)
{
    return name.empty() || name.size() > 128 ? "name must be 1..128 characters" : "";
}

std::string new_id()
{
    thread_local std::mt19937_64 rng{std::random_device{}()};
    return std::format("{:016x}", rng());
}

std::string utc_date(std::chrono::system_clock::time_point t)
{
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(t)};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                       static_cast<unsigned>(ymd.day()));
}

double fraction_of_day(std::chrono::system_clock::time_point t)
{
    const auto since_midnight = t - std::chrono::floor<std::chrono::days>(t);
    return std::chrono::duration<double>(since_midnight).count() / 86400.0;
}

}  // namespace ads
