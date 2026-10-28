# DevSecOps：Ads Campaign & Budget Service

把一个 AWS 原生的 C++ 服务 ([ads-api](ads-api/README.md)) 放进完整的 DevSecOps 流程：
每次提交都要经过密钥、代码、依赖、镜像、基础设施五层安全检查，全部通过的镜像才会签名发布。

第一次看？先读 [DEVSECOPS_WALKTHROUGH.md](DEVSECOPS_WALKTHROUGH.md)：每一步是什么意思、为什么这么做。

```
common/                     共用头文件库 (线程池、队列等)
ads-api/     服务本身：C++23 + DynamoDB + PostgreSQL，CDK 部署到 ECS Fargate
.github/workflows/          CI 流水线 (devsecops.yml)
scripts/scan-local.sh       本机跑和 CI 一样的扫描，只需要 Docker
```

## 流水线 (阶段 1–4)

| 层 | 工具 | 闸门 (失败条件) |
|---|---|---|
| 密钥泄露 | gitleaks (CI 扫完整 git 历史) | 任何发现 |
| SAST | Semgrep (C/C++、Dockerfile、TypeScript、Actions 规则)、CodeQL `security-extended` (C++ / TS / Actions) | 任何发现 |
| 内存安全 | 单元测试 + ASan/UBSan | 任何报错 |
| 依赖 (SCA) | Trivy fs (npm)；C++ 依赖见下文 | 有修复版本的 HIGH/CRITICAL |
| IaC | hadolint (Dockerfile)、Checkov + Trivy config (`cdk synth` 出的 CloudFormation) | 任何未豁免的发现 |
| 镜像 | Trivy image + CycloneDX SBOM | 有修复版本的 HIGH/CRITICAL |
| 发布 (仅 main) | 推到 GHCR，cosign 无密钥签名 + SBOM 证明 (attestation) | — |

扫描结果以 SARIF 格式上传到 GitHub **Security → Code scanning**。每周一定时重扫一次，因为代码没变也可能有新公布的 CVE。

### 供应链
- 所有 GitHub Action 都固定到 **commit SHA**，tag 写在注释里。tag 可以被改指向，2026-03 trivy-action 被劫持就是这种情况
- 扫描器镜像和基础镜像都固定版本，基础镜像固定到 **digest**
- C++ 第三方库 (nlohmann/json、cpp-httplib) 在 CMake 里用 `URL_HASH SHA256` 固定，下载内容被篡改会直接构建失败
- Dependabot 每周提 PR 统一升级上面这些，PR 同样要跑完整条流水线
- workflow 默认 `contents: read`，需要更多权限的 job 单独声明 (最小权限)

### 已知局限
- Trivy 不认识 CMake 的 FetchContent，也认不出从源码编译的 aws-sdk-cpp，所以 SCA 看不到这三个 C++ 依赖。目前靠版本和哈希固定，再加 Dependabot 之外的人工跟进
- CI 扫描的是 amd64 镜像，生产环境跑的是 ARM64 (Graviton)。两者包列表相同，但严格来说扫描的不是同一个产物；要扫同一个产物，可以改用 `ubuntu-24.04-arm` runner
- 公开仓库可以免费使用 Code scanning；私有仓库需要 GitHub Advanced Security，否则上传 SARIF 这一步会失败

## 风险接受记录

发现的问题要么修掉，要么写明理由后豁免。豁免写在离代码最近的地方：
- **Checkov**：写在 [ads-stack.ts](ads-api/infra/lib/ads-stack.ts) 里，通过 `checkovSkip()` 写进具体资源的 CloudFormation Metadata，只对那一个资源生效
- **Trivy**：写在 [.trivyignore](.trivyignore)
- **hadolint**：写在 [.hadolint.yaml](.hadolint.yaml)
- **gitleaks**：写在 [.gitleaks.toml](.gitleaks.toml)

第一次扫描后的处理：

| 发现 | 处理 |
|---|---|
| RDS 密钥未用 CMK 加密 (CKV_AWS_149) | **已修**：`fromGeneratedSecret(..., { encryptionKey })` |
| 日志组未加密 (CKV_AWS_158) | **已修**：KMS 加密，key policy 只授权本账号的 logs |
| ALB 不丢弃非法头 (CKV_AWS_131) | **已修**：`dropInvalidHeaderFields`，防 HTTP 请求走私 |
| API Gateway 没有访问日志 (CKV_AWS_95) | **已修**：JSON 访问日志，记录调用者账号、IP、状态码 |
| `brace-expansion@5.0.9` 有 2 个 DoS 漏洞 (CVE-2026-102276/102278，HIGH)，由 aws-cdk-lib 打包带入 | **已修**：aws-cdk-lib 2.272.0 → 2.274.0 (打包的依赖 npm overrides 改不了，只能升级上游)，CLI 跟着升到 2.1146.1 |
| 安全组出站全开 (AVD-AWS-0104) | **已修**：Endpoint 安全组只允许任务的 443 进入、出站关闭；任务出站只开 443 和到数据库的 5432 |
| RDS 单 AZ、无增强监控、备份 1 天、无删除保护 | 豁免：side project 的成本取舍，生产环境要改 |
| RDS IAM 认证 | 豁免：应用用 Secrets Manager 里的密码，只开开关不改代码没有意义 |
| ALB 用 HTTP | 豁免：内部 ALB 在隔离子网里，TLS 在 API Gateway 终止 → 阶段 7 用 Istio mTLS |
| ALB 访问日志 | 推迟：需要 S3 桶 → 阶段 6 |
| CDK 自动生成的 Lambda (DLQ / VPC / 并发) | 豁免：只在部署时运行一次的 custom resource |
| apt 包未固定版本 (DL3008) | 豁免：改为固定基础镜像 digest，再用 Trivy 扫镜像兜底 |
| 没有 HEALTHCHECK (DS026) | 豁免：运行时镜像故意不带 curl，健康检查由 ALB 做 |

## 本机运行

```bash
scripts/scan-local.sh              # 全部 (镜像要从源码编译 aws-sdk-cpp，第一次大约 15 分钟)
scripts/scan-local.sh secrets sast # 只跑其中几项: secrets | sast | sca | iac | image
```

## 路线图

- [x] 1–4 代码 / 依赖 / 镜像 / IaC 扫描、SBOM、签名
- [ ] 5 部署：GitHub OIDC → AWS 短期凭证 (不存 AK/SK)，`cdk deploy` 后对 API 跑 OWASP ZAP (DAST)
- [ ] 6 日志与监控：CloudTrail、ALB 访问日志、GuardDuty、AWS Budgets 告警
- [ ] 7 Kubernetes：EKS + ArgoCD (GitOps)、OPA Gatekeeper、External Secrets、Istio mTLS
- [ ] 8 合规即代码：AWS Config 规则、kube-bench (CIS)
