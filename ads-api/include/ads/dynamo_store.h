// DynamoDB 实现 (单表设计，一张表放所有实体):
//
//   实体         PK                      SK                   说明
//   Campaign     ADV#{advertiser}        CMP#{campaign}       按广告主 Query 列出全部 campaign
//   Keyword      CMP#{campaign}          KW#{text}            按 campaign Query 列出关键词
//   当日预算     CMP#{campaign}          BUDGET#{yyyy-mm-dd}  remaining 计数器，TTL 7 天后自动删除
//   幂等记录     IDEM#{advertiser}#{key} IDEM                 TTL 24 小时
//
// 用到的 DynamoDB 特性:
//   条件写 (ConditionExpression)       → 乐观锁、不超支、幂等
//   TransactWriteItems                 → 创建 campaign + 写幂等记录、改预算 + 调整当天计数器，两两原子
//   UpdateItem + if_not_exists + 返回新值 → 一次调用完成 "不存在就初始化，然后扣减"
//   强一致读 (ConsistentRead)          → 对账、冲突后重读
//
// pimpl: 头文件里不出现任何 AWS SDK 类型 → main.cpp 等不需要包含 SDK 头文件，编译快，SDK 升级也只重编一个 .cpp
#pragma once
#include "ads/stores.h"

#include <atomic>
#include <memory>

namespace ads {

// Aws::InitAPI / ShutdownAPI 的 RAII 封装: 必须比所有 SDK client 活得久 → 在 main 里最先构造
class AwsApiGuard {
public:
    AwsApiGuard();
    ~AwsApiGuard();
    AwsApiGuard(const AwsApiGuard&) = delete;
    AwsApiGuard& operator=(const AwsApiGuard&) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct DynamoConfig {
    std::string table = "ads";
    std::string region = "us-east-1";
    std::string endpoint;   // 空 = AWS 默认终端节点；本地 = http://dynamodb:8000
};

class DynamoStore final : public CampaignStore, public BudgetStore {
public:
    explicit DynamoStore(const DynamoConfig& cfg);
    ~DynamoStore() override;

    void create_table_if_missing();   // 只在本地用；AWS 上表由 CDK 创建 (含 KMS 加密、PITR、TTL)

    std::expected<CreateResult, StoreError> create(const Campaign& c, const std::string& idempotency_key,
                                                   const std::string& request_hash) override;
    std::optional<Campaign> get(const std::string& advertiser_id, const std::string& campaign_id) override;
    std::vector<Campaign> list(const std::string& advertiser_id) override;
    std::expected<Campaign, StoreError> update(const std::string& advertiser_id, const std::string& campaign_id,
                                               std::int64_t expected_version, const CampaignPatch& patch,
                                               const std::string& today) override;
    void put_keyword(const Keyword& k) override;
    std::vector<Keyword> list_keywords(const std::string& campaign_id) override;

    Grant lease(const std::string& campaign_id, const std::string& date, Micros daily_budget, Micros want) override;
    void give_back(const std::string& campaign_id, const std::string& date, Micros amount) override;
    std::optional<Micros> remaining(const std::string& campaign_id, const std::string& date) override;
    std::int64_t calls() const override { return calls_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<std::int64_t> calls_{0};
};

}  // namespace ads
