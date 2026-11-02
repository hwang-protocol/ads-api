// 领域模型 (概念来自公开的 Amazon Ads API: advertiser → campaign → keyword、日预算、竞价)
// 金额一律用 int64 的 micros (1 美元 = 1,000,000)，不用 double:
//   0.1 + 0.2 != 0.3，钱算错一分都是事故；Amazon Ads / Google Ads 的 API 也都用 micros
#pragma once
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace ads {

using Micros = std::int64_t;
constexpr Micros kMicrosPerDollar = 1'000'000;

enum class CampaignStatus { Enabled, Paused, Archived };
enum class Pacing { Standard, Accelerated };   // Standard: 按一天的时间进度平滑花费；Accelerated: 尽快花

struct Campaign {
    std::string advertiser_id;
    std::string campaign_id;
    std::string name;
    Micros daily_budget{0};
    CampaignStatus status{CampaignStatus::Enabled};
    Pacing pacing{Pacing::Standard};
    std::int64_t version{1};   // 乐观锁: 每次修改 +1
    std::int64_t created_at{0};
};

struct Keyword {
    std::string campaign_id;
    std::string text;
    Micros bid{0};
};

struct ChargeEvent {
    std::string event_id;   // 广告服务器生成的唯一 id，用来去重 (at-least-once 投递下同一次点击可能报两次)
    Micros cost{0};
};

// PATCH 请求里出现的字段才修改
struct CampaignPatch {
    std::optional<std::string> name;
    std::optional<Micros> daily_budget;
    std::optional<CampaignStatus> status;
    std::optional<Pacing> pacing;
};

std::string_view to_string(CampaignStatus s);
std::string_view to_string(Pacing p);
std::optional<CampaignStatus> parse_status(std::string_view s);
std::optional<Pacing> parse_pacing(std::string_view s);

nlohmann::json to_json(const Campaign& c);
nlohmann::json to_json(const Keyword& k);

// 校验: 返回错误信息，空表示通过
std::string validate_id(std::string_view id, std::string_view what);   // [A-Za-z0-9_-]{1,64}
std::string validate_budget(Micros m);
std::string validate_name(std::string_view name);

std::string new_id();   // 16 位十六进制随机 id

// 日期按 UTC 计算 (Amazon Ads 实际按广告主所在时区，这里简化)
std::string utc_date(std::chrono::system_clock::time_point t);
double fraction_of_day(std::chrono::system_clock::time_point t);   // 0.0 ~ 1.0

}  // namespace ads
