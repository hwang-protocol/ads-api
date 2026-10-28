#!/usr/bin/env node
// 部署: npx cdk deploy  (用当前 AWS CLI 凭证对应的账号和区域)
import * as cdk from 'aws-cdk-lib';
import { AdsStack } from '../lib/ads-stack';

const app = new cdk.App();
new AdsStack(app, 'AdsCampaignBudget', {
  env: { account: process.env.CDK_DEFAULT_ACCOUNT, region: process.env.CDK_DEFAULT_REGION },
  description: 'Ads Campaign & Budget Service (side project)',
});
cdk.Tags.of(app).add('project', 'ads-campaign-budget');   // 所有资源打标签，方便在账单里按项目看花费
