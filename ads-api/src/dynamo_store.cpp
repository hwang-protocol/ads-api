#include "ads/dynamo_store.h"

#include <aws/core/Aws.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/DynamoDBErrors.h>
#include <aws/dynamodb/model/AttributeDefinition.h>
#include <aws/dynamodb/model/CreateTableRequest.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/KeySchemaElement.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/TransactWriteItemsRequest.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>

#include <chrono>
#include <format>
#include <thread>

namespace ads {

namespace ddb = Aws::DynamoDB;
namespace dm = Aws::DynamoDB::Model;
using Item = Aws::Map<Aws::String, dm::AttributeValue>;

// ---------------- AwsApiGuard ----------------

struct AwsApiGuard::Impl {
    Aws::SDKOptions options;
};

AwsApiGuard::AwsApiGuard() : impl_{std::make_unique<Impl>()} { Aws::InitAPI(impl_->options); }
AwsApiGuard::~AwsApiGuard() { Aws::ShutdownAPI(impl_->options); }

namespace {

Aws::String aws(const std::string& s) { return Aws::String(s.c_str(), s.size()); }
std::string str(const Aws::String& s) { return std::string(s.c_str(), s.size()); }

dm::AttributeValue S(const std::string& s)
{
    dm::AttributeValue v;
    v.SetS(aws(s));
    return v;
}
dm::AttributeValue N(std::int64_t n)
{
    dm::AttributeValue v;
    v.SetN(aws(std::to_string(n)));
    return v;
}

std::string get_s(const Item& it, const char* k)
{
    auto f = it.find(k);
    return f == it.end() ? std::string{} : str(f->second.GetS());
}
std::int64_t get_n(const Item& it, const char* k)
{
    auto f = it.find(k);
    return f == it.end() ? 0 : std::stoll(str(f->second.GetN()));
}

std::string pk_adv(const std::string& a) { return "ADV#" + a; }
std::string pk_cmp(const std::string& c) { return "CMP#" + c; }
std::string sk_budget(const std::string& d) { return "BUDGET#" + d; }

std::int64_t epoch_now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

Item key(const std::string& pk, const std::string& sk) { return {{"PK", S(pk)}, {"SK", S(sk)}}; }

Item campaign_item(const Campaign& c)
{
    return {{"PK", S(pk_adv(c.advertiser_id))},
            {"SK", S("CMP#" + c.campaign_id)},
            {"type", S("CAMPAIGN")},
            {"advertiser_id", S(c.advertiser_id)},
            {"campaign_id", S(c.campaign_id)},
            {"name", S(c.name)},
            {"daily_budget", N(c.daily_budget)},
            {"status", S(std::string(to_string(c.status)))},
            {"pacing", S(std::string(to_string(c.pacing)))},
            {"version", N(c.version)},
            {"created_at", N(c.created_at)}};
}

Campaign parse_campaign(const Item& it)
{
    Campaign c;
    c.advertiser_id = get_s(it, "advertiser_id");
    c.campaign_id = get_s(it, "campaign_id");
    c.name = get_s(it, "name");
    c.daily_budget = get_n(it, "daily_budget");
    c.status = parse_status(get_s(it, "status")).value_or(CampaignStatus::Paused);
    c.pacing = parse_pacing(get_s(it, "pacing")).value_or(Pacing::Standard);
    c.version = get_n(it, "version");
    c.created_at = get_n(it, "created_at");
    return c;
}

// SDK 自己已经按指数退避重试过了；还失败的: 限流 / 5xx → Unavailable (API 返回 503)，其它 → 程序错误
template <typename Outcome>
[[noreturn]] void fail(const Outcome& out, const char* op)
{
    const auto& e = out.GetError();
    const auto msg = std::format("{}: {} {}", op, str(e.GetExceptionName()), str(e.GetMessage()));
    if (e.ShouldRetry()) throw Unavailable(msg);
    throw std::runtime_error(msg);
}

}  // namespace

// ---------------- DynamoStore ----------------

struct DynamoStore::Impl {
    Aws::String table;
    std::unique_ptr<ddb::DynamoDBClient> client;

    std::optional<Item> get_item(const std::string& pk, const std::string& sk)
    {
        dm::GetItemRequest req;
        req.SetTableName(table);
        req.SetKey(key(pk, sk));
        req.SetConsistentRead(true);   // 强一致: 冲突后重读、对账都要读到最新值
        auto out = client->GetItem(req);
        if (!out.IsSuccess()) fail(out, "GetItem");
        if (out.GetResult().GetItem().empty()) return std::nullopt;
        return out.GetResult().GetItem();
    }

    std::vector<Item> query_prefix(const std::string& pk, const std::string& sk_prefix)
    {
        std::vector<Item> items;
        dm::QueryRequest req;
        req.SetTableName(table);
        req.SetKeyConditionExpression("PK = :pk AND begins_with(SK, :p)");
        req.SetExpressionAttributeValues({{":pk", S(pk)}, {":p", S(sk_prefix)}});
        for (;;) {   // 分页: 一次 Query 最多返回 1MB
            auto out = client->Query(req);
            if (!out.IsSuccess()) fail(out, "Query");
            for (const auto& it : out.GetResult().GetItems()) items.push_back(it);
            const auto& last = out.GetResult().GetLastEvaluatedKey();
            if (last.empty()) break;
            req.SetExclusiveStartKey(last);
        }
        return items;
    }
};

DynamoStore::DynamoStore(const DynamoConfig& cfg) : impl_{std::make_unique<Impl>()}
{
    Aws::Client::ClientConfiguration cc;
    cc.region = aws(cfg.region);
    if (!cfg.endpoint.empty()) {
        cc.endpointOverride = aws(cfg.endpoint);
        if (cfg.endpoint.starts_with("http://")) cc.scheme = Aws::Http::Scheme::HTTP;
    }
    cc.connectTimeoutMs = 1000;
    cc.requestTimeoutMs = 3000;
    cc.maxConnections = 64;   // 默认 25；HTTP 线程池比这个大时，多出来的线程会排队等连接
    impl_->table = aws(cfg.table);
    impl_->client = std::make_unique<ddb::DynamoDBClient>(cc);
}

DynamoStore::~DynamoStore() = default;   // Impl 在 .cpp 里才是完整类型，析构必须定义在这里 (pimpl 的固定写法)

void DynamoStore::create_table_if_missing()
{
    dm::CreateTableRequest req;
    req.SetTableName(impl_->table);
    dm::AttributeDefinition pk, sk;
    pk.SetAttributeName("PK");
    pk.SetAttributeType(dm::ScalarAttributeType::S);
    sk.SetAttributeName("SK");
    sk.SetAttributeType(dm::ScalarAttributeType::S);
    req.SetAttributeDefinitions({pk, sk});
    dm::KeySchemaElement hk, rk;
    hk.SetAttributeName("PK");
    hk.SetKeyType(dm::KeyType::HASH);
    rk.SetAttributeName("SK");
    rk.SetKeyType(dm::KeyType::RANGE);
    req.SetKeySchema({hk, rk});
    req.SetBillingMode(dm::BillingMode::PAY_PER_REQUEST);
    auto out = impl_->client->CreateTable(req);
    if (!out.IsSuccess() && out.GetError().GetErrorType() != ddb::DynamoDBErrors::RESOURCE_IN_USE) fail(out, "CreateTable");
}

std::expected<CreateResult, StoreError> DynamoStore::create(const Campaign& c, const std::string& idempotency_key,
                                                            const std::string& request_hash)
{
    const auto idem_pk = "IDEM#" + c.advertiser_id + "#" + idempotency_key;
    for (int attempt = 0; attempt < 3; ++attempt) {
        // 一个事务里: 写 campaign + 写幂等记录，两个都要求 "之前不存在"
        dm::Put put_c;
        put_c.SetTableName(impl_->table);
        put_c.SetItem(campaign_item(c));
        put_c.SetConditionExpression("attribute_not_exists(PK)");
        dm::Put put_i;
        put_i.SetTableName(impl_->table);
        put_i.SetItem({{"PK", S(idem_pk)},
                       {"SK", S("IDEM")},
                       {"request_hash", S(request_hash)},
                       {"campaign_id", S(c.campaign_id)},
                       {"expires_at", N(epoch_now() + 24 * 3600)}});
        put_i.SetConditionExpression("attribute_not_exists(PK)");
        dm::TransactWriteItemsRequest req;
        req.AddTransactItems(dm::TransactWriteItem().WithPut(put_c));
        req.AddTransactItems(dm::TransactWriteItem().WithPut(put_i));
        auto out = impl_->client->TransactWriteItems(req);
        if (out.IsSuccess()) return CreateResult{c, true};
        if (out.GetError().GetErrorType() != ddb::DynamoDBErrors::TRANSACTION_CANCELED) fail(out, "TransactWriteItems");

        // 事务被取消: 最常见的原因是这个幂等键已经用过 → 读出来看
        if (auto idem = impl_->get_item(idem_pk, "IDEM")) {
            if (get_s(*idem, "request_hash") != request_hash) return std::unexpected(StoreError::IdempotencyMismatch);
            if (auto prev = get(c.advertiser_id, get_s(*idem, "campaign_id"))) return CreateResult{*prev, false};
        }
        // 其它原因 (和另一个同 key 的请求同时提交导致事务冲突) → 稍等重试
        std::this_thread::sleep_for(std::chrono::milliseconds(20 * (attempt + 1)));
    }
    throw Unavailable("create campaign: transaction kept conflicting");
}

std::optional<Campaign> DynamoStore::get(const std::string& advertiser_id, const std::string& campaign_id)
{
    auto it = impl_->get_item(pk_adv(advertiser_id), "CMP#" + campaign_id);
    if (!it) return std::nullopt;
    return parse_campaign(*it);
}

std::vector<Campaign> DynamoStore::list(const std::string& advertiser_id)
{
    std::vector<Campaign> out;
    for (const auto& it : impl_->query_prefix(pk_adv(advertiser_id), "CMP#")) out.push_back(parse_campaign(it));
    return out;
}

std::expected<Campaign, StoreError> DynamoStore::update(const std::string& advertiser_id, const std::string& campaign_id,
                                                        std::int64_t expected_version, const CampaignPatch& patch,
                                                        const std::string& today)
{
    const auto current = get(advertiser_id, campaign_id);
    if (!current) return std::unexpected(StoreError::NotFound);
    if (current->version != expected_version) return std::unexpected(StoreError::VersionConflict);   // 快速失败

    // name / status 是 DynamoDB 保留字，必须用 #别名
    std::string expr = "SET #v = #v + :one";
    Item values{{":one", N(1)}, {":expected", N(expected_version)}};
    Aws::Map<Aws::String, Aws::String> names{{"#v", "version"}};
    if (patch.name) { expr += ", #n = :n"; names["#n"] = "name"; values[":n"] = S(*patch.name); }
    if (patch.daily_budget) { expr += ", daily_budget = :b"; values[":b"] = N(*patch.daily_budget); }
    if (patch.status) { expr += ", #s = :s"; names["#s"] = "status"; values[":s"] = S(std::string(to_string(*patch.status))); }
    if (patch.pacing) { expr += ", pacing = :p"; values[":p"] = S(std::string(to_string(*patch.pacing))); }

    dm::Update upd;
    upd.SetTableName(impl_->table);
    upd.SetKey(key(pk_adv(advertiser_id), "CMP#" + campaign_id));
    upd.SetUpdateExpression(aws(expr));
    upd.SetConditionExpression("attribute_exists(PK) AND #v = :expected");   // 乐观锁
    upd.SetExpressionAttributeNames(names);
    upd.SetExpressionAttributeValues(values);

    dm::TransactWriteItemsRequest req;
    req.AddTransactItems(dm::TransactWriteItem().WithUpdate(upd));
    if (patch.daily_budget && *patch.daily_budget != current->daily_budget) {
        // 当天计数器: 存在 → 加上差额；不存在 → 旧预算 + 差额 = 新预算。一个表达式搞定，不用先读
        dm::Update b;
        b.SetTableName(impl_->table);
        b.SetKey(key(pk_cmp(campaign_id), sk_budget(today)));
        b.SetUpdateExpression("SET remaining = if_not_exists(remaining, :old) + :delta, expires_at = if_not_exists(expires_at, :exp)");
        b.SetExpressionAttributeValues({{":old", N(current->daily_budget)},
                                        {":delta", N(*patch.daily_budget - current->daily_budget)},
                                        {":exp", N(epoch_now() + 7 * 24 * 3600)}});
        req.AddTransactItems(dm::TransactWriteItem().WithUpdate(b));
    }
    auto out = impl_->client->TransactWriteItems(req);
    if (!out.IsSuccess()) {
        if (out.GetError().GetErrorType() == ddb::DynamoDBErrors::TRANSACTION_CANCELED)
            return std::unexpected(get(advertiser_id, campaign_id) ? StoreError::VersionConflict : StoreError::NotFound);
        fail(out, "TransactWriteItems");
    }
    if (auto c = get(advertiser_id, campaign_id)) return *c;
    return std::unexpected(StoreError::NotFound);
}

void DynamoStore::put_keyword(const Keyword& k)
{
    dm::PutItemRequest req;
    req.SetTableName(impl_->table);
    req.SetItem({{"PK", S(pk_cmp(k.campaign_id))},
                 {"SK", S("KW#" + k.text)},
                 {"type", S("KEYWORD")},
                 {"campaign_id", S(k.campaign_id)},
                 {"text", S(k.text)},
                 {"bid", N(k.bid)}});
    auto out = impl_->client->PutItem(req);
    if (!out.IsSuccess()) fail(out, "PutItem");
}

std::vector<Keyword> DynamoStore::list_keywords(const std::string& campaign_id)
{
    std::vector<Keyword> out;
    for (const auto& it : impl_->query_prefix(pk_cmp(campaign_id), "KW#"))
        out.push_back({get_s(it, "campaign_id"), get_s(it, "text"), get_n(it, "bid")});
    return out;
}

BudgetStore::Grant DynamoStore::lease(const std::string& campaign_id, const std::string& date, Micros daily_budget, Micros want)
{
    want = std::min(want, daily_budget);   // 保证 "不存在时初始化为 daily_budget 再减 want" 不会变成负数
    for (int attempt = 0; attempt < 5 && want > 0; ++attempt) {
        // 一次调用: 不存在就初始化为日预算，然后扣 want；条件保证扣完不为负 → 永不超支
        dm::UpdateItemRequest req;
        req.SetTableName(impl_->table);
        req.SetKey(key(pk_cmp(campaign_id), sk_budget(date)));
        req.SetUpdateExpression("SET remaining = if_not_exists(remaining, :budget) - :want, expires_at = if_not_exists(expires_at, :exp)");
        req.SetConditionExpression("attribute_not_exists(remaining) OR remaining >= :want");
        req.SetExpressionAttributeValues({{":budget", N(daily_budget)}, {":want", N(want)}, {":exp", N(epoch_now() + 7 * 24 * 3600)}});
        req.SetReturnValues(dm::ReturnValue::UPDATED_NEW);
        ++calls_;
        auto out = impl_->client->UpdateItem(req);
        if (out.IsSuccess()) return {want, get_n(out.GetResult().GetAttributes(), "remaining")};
        if (out.GetError().GetErrorType() != ddb::DynamoDBErrors::CONDITIONAL_CHECK_FAILED) fail(out, "UpdateItem");

        // 剩余不够 want → 读出剩余，把剩下的全部拿走 (条件同样保证不会拿多)
        ++calls_;
        const auto r = remaining(campaign_id, date).value_or(0);
        if (r <= 0) return {0, r};
        want = std::min(want, r);
    }
    return {0, remaining(campaign_id, date).value_or(0)};
}

void DynamoStore::give_back(const std::string& campaign_id, const std::string& date, Micros amount)
{
    dm::UpdateItemRequest req;
    req.SetTableName(impl_->table);
    req.SetKey(key(pk_cmp(campaign_id), sk_budget(date)));
    req.SetUpdateExpression("ADD remaining :amt");   // ADD 是原子加，多个实例同时还也不会丢
    req.SetExpressionAttributeValues({{":amt", N(amount)}});
    ++calls_;
    auto out = impl_->client->UpdateItem(req);
    if (!out.IsSuccess()) fail(out, "UpdateItem(give_back)");
}

std::optional<Micros> DynamoStore::remaining(const std::string& campaign_id, const std::string& date)
{
    auto it = impl_->get_item(pk_cmp(campaign_id), sk_budget(date));
    if (!it || !it->contains("remaining")) return std::nullopt;
    return get_n(*it, "remaining");
}

}  // namespace ads
