// REST API (cpp-httplib)。路由和 HTTP 语义:
//
//   GET    /health
//   POST   /v1/campaigns                         Idempotency-Key 头必填；201 新建 / 200 重放 / 422 同 key 不同内容
//   GET    /v1/campaigns                         当前广告主的全部 campaign
//   GET    /v1/campaigns/:id                     ETag: 版本号
//   PATCH  /v1/campaigns/:id                     If-Match: 版本号 必填 (428)；版本不对 409
//   PUT    /v1/campaigns/:id/keywords/:text      设置关键词出价
//   GET    /v1/campaigns/:id/keywords
//   POST   /v1/campaigns/:id/charges             广告服务器上报点击扣费 (内部接口: 在 AWS 上只开放给 VPC 内部)
//   GET    /v1/campaigns/:id/budget              当天预算对账视图
//   GET    /v1/reports/daily-spend?from=&to=     日花费报表 (PostgreSQL 窗口函数)
//
// 身份: X-Advertiser-Id 头。本地由调用方直接传；在 AWS 上由 API Gateway (IAM 鉴权) 根据调用者身份改写，
//       客户端自己传的值会被覆盖，伪造不了。所有 key 都带 advertiser_id → 天然的租户隔离 (别人的 campaign 一律 404)
#pragma once
#include "ads/budget_service.h"
#include "ads/stores.h"

namespace httplib {
class Server;
}

namespace ads {

struct ApiDeps {
    CampaignStore& campaigns;
    LedgerStore& ledger;
    BudgetService& budget;
    const Clock& clock;
};

void register_routes(httplib::Server& server, ApiDeps deps);

}  // namespace ads
