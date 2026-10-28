#include "ads/api.h"

#include <httplib.h>

#include <algorithm>
#include <charconv>
#include <map>
#include <format>
#include <print>

namespace ads {

namespace {

using json = nlohmann::json;

void reply(httplib::Response& res, int status, const json& body)
{
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void error(httplib::Response& res, int status, std::string_view code, std::string_view message)
{
    reply(res, status, {{"error", code}, {"message", message}});
}

// 取广告主身份；没有或格式不对 → 401，返回空
std::optional<std::string> advertiser(const httplib::Request& req, httplib::Response& res)
{
    auto id = req.get_header_value("X-Advertiser-Id");
    if (id.empty() || !validate_id(id, "advertiser").empty()) {
        error(res, 401, "UNAUTHENTICATED", "missing or invalid X-Advertiser-Id");
        return std::nullopt;
    }
    return id;
}

std::optional<json> parse_body(const httplib::Request& req, httplib::Response& res)
{
    auto j = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        error(res, 400, "BAD_REQUEST", "body must be a JSON object");
        return std::nullopt;
    }
    return j;
}

// 取 JSON 里的整数字段；类型不对返回 nullopt
std::optional<std::int64_t> get_int(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<std::int64_t>();
}

std::optional<std::string> get_str(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

void with_etag(httplib::Response& res, const Campaign& c, int status = 200)
{
    res.set_header("ETag", std::format("\"{}\"", c.version));
    reply(res, status, to_json(c));
}

}  // namespace

void register_routes(httplib::Server& svr, ApiDeps d)
{
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) { res.set_content("ok", "text/plain"); });

    // ---------- campaign CRUD ----------
    svr.Post("/v1/campaigns", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        const auto key = req.get_header_value("Idempotency-Key");
        if (!validate_id(key, "Idempotency-Key").empty())
            return error(res, 400, "BAD_REQUEST", "Idempotency-Key header is required ([A-Za-z0-9_-]{1,64})");
        auto body = parse_body(req, res);
        if (!body) return;

        Campaign c;
        c.advertiser_id = *adv;
        c.campaign_id = new_id();
        c.name = get_str(*body, "name").value_or("");
        c.daily_budget = get_int(*body, "daily_budget_micros").value_or(0);
        c.pacing = parse_pacing(get_str(*body, "pacing").value_or("STANDARD")).value_or(Pacing::Standard);
        c.status = parse_status(get_str(*body, "status").value_or("ENABLED")).value_or(CampaignStatus::Enabled);
        c.created_at = std::chrono::duration_cast<std::chrono::seconds>(d.clock.now().time_since_epoch()).count();
        if (auto e = validate_name(c.name); !e.empty()) return error(res, 400, "VALIDATION", e);
        if (auto e = validate_budget(c.daily_budget); !e.empty()) return error(res, 400, "VALIDATION", e);

        // 请求指纹: nlohmann::json 的 object 按 key 排序，dump() 是规范形式 → 字段顺序不同也算同一个请求
        // (生产里用 SHA-256；std::hash 不是加密哈希，这里只用来比较)
        const auto hash = std::format("{:016x}", std::hash<std::string>{}(body->dump()));
        auto r = d.campaigns.create(c, key, hash);
        if (!r) return error(res, 422, "IDEMPOTENCY_KEY_REUSED", "this Idempotency-Key was used with a different request");
        if (!r->created) res.set_header("Idempotent-Replayed", "true");
        with_etag(res, r->campaign, r->created ? 201 : 200);
    });

    svr.Get("/v1/campaigns", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        json arr = json::array();
        for (const auto& c : d.campaigns.list(*adv)) arr.push_back(to_json(c));
        reply(res, 200, {{"campaigns", arr}});
    });

    svr.Get("/v1/campaigns/:id", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        const auto c = d.campaigns.get(*adv, req.path_params.at("id"));
        if (!c) return error(res, 404, "NOT_FOUND", "campaign not found");   // 别人的 campaign 也是 404，不暴露是否存在
        with_etag(res, *c);
    });

    svr.Patch("/v1/campaigns/:id", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        auto if_match = req.get_header_value("If-Match");
        std::erase(if_match, '"');
        std::int64_t version = 0;
        auto [p, ec] = std::from_chars(if_match.data(), if_match.data() + if_match.size(), version);
        if (if_match.empty() || ec != std::errc{} || p != if_match.data() + if_match.size())
            return error(res, 428, "PRECONDITION_REQUIRED", "If-Match: <version> header is required");
        auto body = parse_body(req, res);
        if (!body) return;

        CampaignPatch patch;
        if (body->contains("name")) {
            patch.name = get_str(*body, "name");
            if (!patch.name || !validate_name(*patch.name).empty()) return error(res, 400, "VALIDATION", "bad name");
        }
        if (body->contains("daily_budget_micros")) {
            patch.daily_budget = get_int(*body, "daily_budget_micros");
            if (!patch.daily_budget) return error(res, 400, "VALIDATION", "daily_budget_micros must be an integer");
            if (auto e = validate_budget(*patch.daily_budget); !e.empty()) return error(res, 400, "VALIDATION", e);
        }
        if (body->contains("status")) {
            patch.status = parse_status(get_str(*body, "status").value_or(""));
            if (!patch.status) return error(res, 400, "VALIDATION", "status must be ENABLED | PAUSED | ARCHIVED");
        }
        if (body->contains("pacing")) {
            patch.pacing = parse_pacing(get_str(*body, "pacing").value_or(""));
            if (!patch.pacing) return error(res, 400, "VALIDATION", "pacing must be STANDARD | ACCELERATED");
        }
        const auto& id = req.path_params.at("id");
        auto r = d.campaigns.update(*adv, id, version, patch, utc_date(d.clock.now()));
        if (!r && r.error() == StoreError::NotFound) return error(res, 404, "NOT_FOUND", "campaign not found");
        if (!r) {
            const auto cur = d.campaigns.get(*adv, id);
            return reply(res, 409, {{"error", "VERSION_CONFLICT"},
                                    {"message", "campaign was modified by someone else; re-read and retry"},
                                    {"current_version", cur ? cur->version : 0}});
        }
        d.budget.invalidate(id);
        with_etag(res, *r);
    });

    // ---------- keywords ----------
    svr.Put("/v1/campaigns/:id/keywords/:text", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        const auto& id = req.path_params.at("id");
        const auto& text = req.path_params.at("text");
        if (!d.campaigns.get(*adv, id)) return error(res, 404, "NOT_FOUND", "campaign not found");   // 先校验归属
        if (auto e = validate_id(text, "keyword"); !e.empty()) return error(res, 400, "VALIDATION", e);
        auto body = parse_body(req, res);
        if (!body) return;
        const auto bid = get_int(*body, "bid_micros");
        if (!bid || *bid <= 0 || *bid > 1000 * kMicrosPerDollar) return error(res, 400, "VALIDATION", "bid_micros must be 1..1e9");
        const Keyword k{id, text, *bid};
        d.campaigns.put_keyword(k);
        reply(res, 200, to_json(k));
    });

    svr.Get("/v1/campaigns/:id/keywords", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        const auto& id = req.path_params.at("id");
        if (!d.campaigns.get(*adv, id)) return error(res, 404, "NOT_FOUND", "campaign not found");
        json arr = json::array();
        for (const auto& k : d.campaigns.list_keywords(id)) arr.push_back(to_json(k));
        reply(res, 200, {{"keywords", arr}});
    });

    // ---------- budget ----------
    svr.Post("/v1/campaigns/:id/charges", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        auto body = parse_body(req, res);
        if (!body) return;
        auto it = body->find("events");
        if (it == body->end() || !it->is_array() || it->empty() || it->size() > 500)
            return error(res, 400, "VALIDATION", "events must be an array of 1..500 items");
        std::vector<ChargeEvent> events;
        events.reserve(it->size());
        for (const auto& e : *it) {
            if (!e.is_object()) return error(res, 400, "VALIDATION", "each event must be an object");
            events.push_back({get_str(e, "event_id").value_or(""), get_int(e, "cost_micros").value_or(0)});
        }
        auto r = d.budget.charge(*adv, req.path_params.at("id"), events);
        if (!r) return error(res, 404, "NOT_FOUND", "campaign not found");

        json results = json::array();
        std::map<std::string_view, int> counts;
        for (std::size_t i = 0; i < events.size(); ++i) {
            const auto o = to_string(r->outcomes[i]);
            ++counts[o];
            results.push_back({{"event_id", events[i].event_id}, {"outcome", o}});
        }
        json summary = json::object();
        for (const auto& [k, v] : counts) summary[std::string(k)] = v;
        reply(res, 200, {{"accepted_micros", r->accepted_total}, {"summary", summary}, {"results", results}});
    });

    svr.Get("/v1/campaigns/:id/budget", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        auto v = d.budget.view(*adv, req.path_params.at("id"));
        if (!v) return error(res, 404, "NOT_FOUND", "campaign not found");
        reply(res, 200, {{"date", v->date},
                         {"daily_budget_micros", v->daily_budget},
                         {"store_remaining_micros", v->store_remaining},
                         {"local_unspent_micros", v->local_unspent},
                         {"ledger_spent_micros", v->ledger_spent},
                         {"consistent", v->consistent}});
    });

    svr.Get("/v1/reports/daily-spend", [d](const httplib::Request& req, httplib::Response& res) {
        auto adv = advertiser(req, res);
        if (!adv) return;
        const auto today = utc_date(d.clock.now());
        const auto from = req.has_param("from") ? req.get_param_value("from") : today;
        const auto to = req.has_param("to") ? req.get_param_value("to") : today;
        auto is_date = [](const std::string& s) {
            return s.size() == 10 && s[4] == '-' && s[7] == '-' &&
                   std::ranges::all_of(s, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '-'; });
        };
        if (!is_date(from) || !is_date(to)) return error(res, 400, "VALIDATION", "from/to must be YYYY-MM-DD");
        json rows = json::array();
        for (const auto& r : d.ledger.report(*adv, from, to))
            rows.push_back({{"date", r.date},
                            {"campaign_id", r.campaign_id},
                            {"events", r.events},
                            {"spend_micros", r.spend},
                            {"share_of_advertiser", r.share_of_advertiser},
                            {"running_total_micros", r.running_total}});
        reply(res, 200, {{"from", from}, {"to", to}, {"rows", rows}});
    });

    // 后端暂时不可用 → 503 + Retry-After；其它未捕获异常 → 500，不把内部错误细节返回给调用方
    svr.set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const Unavailable& e) {
            res.set_header("Retry-After", "1");
            error(res, 503, "UNAVAILABLE", "backend temporarily unavailable, retry later");
            std::println(stderr, R"({{"level":"warn","path":"{}","error":"{}"}})", req.path, e.what());
        } catch (const std::exception& e) {
            error(res, 500, "INTERNAL", "internal error");
            std::println(stderr, R"({{"level":"error","path":"{}","error":"{}"}})", req.path, e.what());
        }
    });
}

}  // namespace ads
