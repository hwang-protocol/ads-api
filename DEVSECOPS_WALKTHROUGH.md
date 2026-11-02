# DevSecOps 项目导读（写给刚学 DevSecOps 的人）

这份文档讲清楚：这个项目做了什么、每一步是什么意思、为什么要这么做。
读完之后，你应该能自己跑一遍，也能在面试里讲出来。

---

## 0. 先建立概念

### DevSecOps 是什么

- **DevOps**：开发 (Dev) 和运维 (Ops) 合在一起，用自动化流水线把代码快速、稳定地发到线上
- **DevSecOps**：在这条流水线的**每一步都加上安全检查** (Sec)

以前的做法是：开发完 → 上线前交给安全团队审查 → 发现问题 → 返工。
又慢又贵，而且越晚发现问题，修起来越贵。

DevSecOps 的做法是**左移 (Shift Left)**：
把安全检查往前挪，挪到每次提交代码的时候自动执行。

```
写代码 ──▶ 提交 ──▶ 构建 ──▶ 测试 ──▶ 发布 ──▶ 运行
 ◀──────────── 安全检查越靠左，修复成本越低 ────────────
```

### 这个仓库里有什么

| 部分 | 是什么 |
|---|---|
| `ads-api/` | **被保护的应用**：一个 C++ 写的广告预算服务，部署在 AWS 上 |
| `common/` | 应用用到的公共 C++ 库 |
| `.github/`、`scripts/`、各种 `.xxx` 配置文件 | **DevSecOps 这一层**：本项目新做的部分 |

打个比方：`ads-api` 是工厂的产品，DevSecOps 是生产线上的质检工序。

---

## 1. 整体流程

每次 push 代码或开 PR，GitHub Actions 自动运行 [devsecops.yml](.github/workflows/devsecops.yml)：

```
                ┌─ ① secrets  密钥泄露扫描
                ├─ ② sast     代码扫描 (Semgrep)
 push / PR ─────┼─ ③ codeql   代码深度扫描 (CodeQL)
                ├─ ④ test     单元测试 + 内存错误检测
                ├─ ⑤ sca      依赖漏洞扫描
                └─ ⑥ iac      基础设施配置扫描
                        │
                        │ 全部通过才继续
                        ▼
                  ⑦ image   构建镜像 → 扫描镜像 → 生成 SBOM
                        │
                        │ 只在 main 分支
                        ▼
                  推送到 GHCR → cosign 签名
```

- ①–⑥ **并行**运行，互不依赖，节省时间
- 任何一个失败，⑦ 就不会执行。这就叫**安全闸门 (Security Gate)**
- 每周一凌晨还会自动重扫一次（后面解释为什么）

下面一步一步讲。

---

## 2. 每一步是什么意思

### ① 密钥泄露扫描（gitleaks）

**解决什么问题**
开发者不小心把 AWS 密钥、数据库密码、API token 写进代码并提交了。
一旦推到 GitHub，几分钟内就会被自动化机器人扫到并盗用。

**怎么做**
gitleaks 用几百条正则规则匹配常见的密钥格式，比如 AWS key 以 `AKIA` 开头。

**关键细节**
- CI 里扫的是**完整 git 历史**（`fetch-depth: 0`）
  因为就算你在下一个提交里删掉了密钥，它仍然留在 git 历史里
- 本地脚本扫的是**工作目录**，在提交之前就拦住

**我们遇到的情况**
- 第一次扫出 2 个"泄露"，查下来是误报：
  `cdk.out/` 里 S3 文件名是哈希值，长得像密钥
- 处理：在 [.gitleaks.toml](.gitleaks.toml) 里排除生成目录
- 然后放一个**假的** AWS key 进去，确认 gitleaks 还能抓到。
  这叫**金丝雀测试**：改完规则一定要验证规则仍然有效，
  否则可能不小心把所有东西都放过了

### ② SAST 静态代码扫描（Semgrep）

**SAST** = Static Application Security Testing，静态应用安全测试。
"静态"的意思是**不运行程序，只读源代码**。

**解决什么问题**
代码里常见的危险写法，比如：
- C/C++：用 `strcpy`、`sprintf` 这类容易缓冲区溢出的函数
- Dockerfile：用 root 运行、`curl | sh`
- GitHub Actions：把用户可控的输入直接拼进 shell 命令（脚本注入）

**怎么做**
Semgrep 按规则匹配代码模式，速度快，几秒钟就能跑完。
我们用了 4 套规则：C、Dockerfile、TypeScript、GitHub Actions。

**结果**：94 条规则，0 个发现。

### ③ CodeQL 深度代码扫描

和 Semgrep 一样是 SAST，但方法不同：

| | Semgrep | CodeQL |
|---|---|---|
| 原理 | 模式匹配 | 把代码编译成数据库，做**数据流分析** |
| 能发现 | "这里用了危险函数" | "用户输入从 A 一路流到了 B 处的 SQL 查询" |
| 速度 | 秒级 | 分钟级 |

两个一起用，一快一深，互相补充。
C++ 需要**真正编译一遍**，CodeQL 才能看到代码（`build-mode: manual`）。

### ④ 单元测试 + ASan/UBSan

**为什么测试也算安全**
C++ 最大的安全问题是**内存错误**：越界读写、释放后再用、整数溢出。
很多严重漏洞都来自这类错误。

**怎么做**
编译时加上 sanitizer 参数：
- **ASan** (AddressSanitizer)：检测内存越界、use-after-free
- **UBSan** (UndefinedBehaviorSanitizer)：检测未定义行为，比如整数溢出

然后跑单元测试。只要测试过程中出现一次内存错误，程序立刻报错退出，CI 失败。

### ⑤ SCA 依赖漏洞扫描（Trivy）

**SCA** = Software Composition Analysis，软件成分分析。

**解决什么问题**
你的代码没问题，但你用的**第三方库**有已知漏洞。
现代项目里 80% 以上的代码都来自第三方库。

**两个概念**
- **CVE**：公开漏洞的编号，比如 `CVE-2026-102276`。全世界统一编号
- **严重等级**：LOW / MEDIUM / HIGH / CRITICAL

**怎么做**
Trivy 读 `package-lock.json`（npm 依赖清单），和漏洞数据库对比。

**我们的闸门规则**：`HIGH` 或 `CRITICAL` 级别，**并且已经有修复版本** → 失败。
- 为什么要"有修复版本"？如果上游还没修，你阻塞流水线也没用，只会让大家想办法绕过闸门

**我们遇到的真实漏洞**
- `brace-expansion@5.0.9` 有 2 个 HIGH 级 DoS 漏洞
  （构造很深的嵌套花括号 → 递归太深 → 进程崩溃）
- 它不是我们直接装的，是 `aws-cdk-lib` 自己打包带进来的（**传递依赖**）
- 处理：把 `aws-cdk-lib` 从 2.272.0 升到 2.274.0
- 为什么不选更新的 2.275.0？它才发布 2 天。
  供应链安全的习惯是**新版本观察一周再用**，
  因为被投毒的包通常几天之内就会被发现并下架

**已知局限**
Trivy 看不懂 CMake 下载的 C++ 库。我们的处理办法：
在 CMake 里用 `URL_HASH SHA256=...` 固定每个库的哈希。
下载内容被篡改的话，哈希对不上，构建直接失败。

### ⑥ IaC 基础设施扫描（hadolint、Checkov、Trivy config）

**IaC** = Infrastructure as Code，基础设施即代码。
我们的 AWS 资源（VPC、数据库、负载均衡、权限）都写在 [ads-stack.ts](ads-api/infra/lib/ads-stack.ts) 里，
用 CDK 生成 CloudFormation 模板再部署。

**解决什么问题**
**配置错误**是云上安全事故的头号原因：
数据库忘了加密、安全组对全网开放、日志没开……
在部署之前扫一遍配置，就能把这些问题拦在上线前。

**三个工具，各管一块**

| 工具 | 扫什么 |
|---|---|
| hadolint | Dockerfile 的写法 |
| Checkov | CloudFormation 模板（数据库、负载均衡、IAM 等） |
| Trivy config | Dockerfile + CloudFormation，第二意见 |

**怎么做**
1. `cdk synth`：把 TypeScript 转成 CloudFormation JSON（不需要 AWS 账号）
2. 用 Checkov 和 Trivy 扫描这个 JSON

**这一步发现的问题最多，见第 3 节。**

### ⑦ 镜像扫描 + SBOM + 签名

这一步只有前面 6 项全部通过才会执行。

**7a. 镜像扫描**
Docker 镜像里不只有你的程序，还有整个 Ubuntu 系统的包（openssl、curl……）。
Trivy 扫描镜像里的系统包。
**结果**：109 个系统包，0 个 HIGH/CRITICAL。

**7b. SBOM（软件物料清单）**
**SBOM** = Software Bill of Materials。
就像食品包装上的配料表：列出镜像里**每一个**组件和版本。

有什么用？
下次再出现 Log4Shell 那种大漏洞，你不需要逐个检查服务，
只要搜一下所有 SBOM，就知道哪些服务受影响。
我们用 **CycloneDX** 格式（行业标准之一）。

**7c. 签名（cosign）**
**解决什么问题**：怎么证明线上跑的镜像，确实是我们 CI 构建的那一个，没有被人替换？

- cosign 给镜像签名，部署时可以验签
- 我们用**无密钥签名 (keyless)**：
  用 GitHub 给 CI 颁发的临时身份 (OIDC) 来签，不需要自己保管私钥
- 按 **digest** 签名，不按 tag：
  - tag（比如 `:latest`）可以被改指向另一个镜像
  - digest（`sha256:...`）是内容的哈希，内容变了 digest 一定会变
- 同时把 SBOM 作为**证明 (attestation)** 附在镜像上

**只在 main 分支发布**：PR 只扫描，不发布。

---

## 3. 发现问题后怎么处理：修复 or 豁免

扫描器报出问题后，只有两条路：

1. **修复**
2. **豁免**：确认是误报，或者是有意识接受的风险，**写下理由**

绝不能悄悄关掉检查。写下来的豁免记录叫**风险接受记录 (Risk Acceptance)**。
半年后有人问"为什么数据库没开 Multi-AZ"，答案就在代码旁边。

### 我们修复的 5 个问题

| 问题 | 风险 | 修复 |
|---|---|---|
| 数据库密码没用自己的 KMS key 加密 | 无法单独控制谁能解密 | 用项目自己的 KMS key 加密 |
| 日志没加密 | 日志里可能有敏感信息 | 日志组用 KMS 加密 |
| ALB 接受不合法的 HTTP 头 | **HTTP 请求走私**攻击 | 开启 `dropInvalidHeaderFields` |
| API 没有访问日志 | 出了安全事件无法追查是谁干的 | 记录调用者、IP、路径、状态码 |
| 安全组出站全开 | 被入侵后数据可以随便外传 | 只开 443（AWS API）和 5432（数据库） |
| `brace-expansion` 漏洞 | DoS | 升级 aws-cdk-lib |

### 豁免的问题（每条都有理由）

| 问题 | 理由 |
|---|---|
| 数据库单可用区、备份只留 1 天 | 个人项目的成本取舍，生产环境要改 |
| 内部 ALB 用 HTTP 不用 HTTPS | 在隔离子网里，HTTPS 在 API Gateway 终止；阶段 7 用 Istio mTLS 补 |
| ALB 没有访问日志 | 需要建 S3 桶，放到阶段 6 |
| CDK 自动生成的 Lambda | 只在部署时运行一次，不是业务代码 |
| Dockerfile 没写 HEALTHCHECK | 镜像里故意不装 curl（减少攻击面），健康检查由 ALB 做 |

**豁免写在哪里**：尽量离代码近、范围尽量小
- Checkov 的豁免写在 `ads-stack.ts` 里，只对某一个资源生效（`checkovSkip()` 函数）
- 其它写在 [.trivyignore](.trivyignore)、[.hadolint.yaml](.hadolint.yaml)

---

## 4. 贯穿全程的几个原则

### 供应链安全：一切都固定版本

**2026 年 3 月的真实事件**：攻击者篡改了 `trivy-action` 的 tag，
让它指向恶意代码。所有写 `uses: aquasecurity/trivy-action@v0.x` 的仓库，
下次运行时就执行了恶意代码，CI 里的密钥被偷走。

我们的做法：

| 对象 | 怎么固定 |
|---|---|
| GitHub Action | 固定到 **commit SHA**，tag 写在注释里 |
| 扫描器镜像 | 固定版本号 |
| Ubuntu 基础镜像 | 固定到 **digest** |
| C++ 第三方库 | 固定 URL + SHA256 哈希 |
| npm 包 | `package-lock.json` + 精确版本 |

```yaml
# 不安全：tag 可以被改
- uses: actions/checkout@v4

# 安全：SHA 改不了
- uses: actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683 # v4.2.2
```

**固定之后怎么升级？** 交给 **Dependabot**（[dependabot.yml](.github/dependabot.yml)）：
每周自动提 PR 升级，PR 一样要跑完整条安全流水线。

### 最小权限

workflow 默认只给 `contents: read`，哪个 job 需要更多权限，就单独声明：
- 上传扫描结果 → `security-events: write`
- 推镜像 → `packages: write`
- 签名 → `id-token: write`

就算某个步骤被攻破，能造成的破坏也有限。

### 每周定时重扫

代码一行没改，也可能明天就公布了一个新 CVE，影响你正在用的某个库。
所以每周一自动重扫一次。

### 本地和 CI 用同一套

[scan-local.sh](scripts/scan-local.sh) 直接从 workflow 文件读取扫描器版本，
保证本地扫描结果和 CI 一致。push 之前先在本地跑，不用等 CI。

---

## 5. 自己动手跑

只需要 Docker，不用安装任何扫描器。

```bash
cd DecSecOpsRepo

# 全部跑一遍
# (第一次要编译 aws-sdk-cpp，大约 15 分钟)
scripts/scan-local.sh

# 只跑其中几项
scripts/scan-local.sh secrets
scripts/scan-local.sh sast
scripts/scan-local.sh sca iac
```

可选的项目：`secrets` `sast` `sca` `iac` `image`

**练习：亲手触发一次闸门**

```bash
# 1. 放一个假密钥
#    分两段拼起来：文档里不出现完整的 key，
#    否则这份文档自己就会被 gitleaks 拦下
fake="AKIA""QWERTYUIOPASDFGH"
echo "aws_secret = \"$fake\"" \
  > ads-api/test.env

# 2. 扫描，应该看到 FAILED: gitleaks
scripts/scan-local.sh secrets

# 3. 删掉
rm ads-api/test.env
```

---

## 6. 术语表

| 术语 | 全称 | 一句话解释 |
|---|---|---|
| SAST | Static Application Security Testing | 不运行程序，读源码找漏洞 |
| DAST | Dynamic Application Security Testing | 运行程序，从外面攻击它找漏洞（阶段 5） |
| SCA | Software Composition Analysis | 找第三方依赖里的已知漏洞 |
| IaC | Infrastructure as Code | 用代码定义云资源 |
| CVE | Common Vulnerabilities and Exposures | 公开漏洞的统一编号 |
| SBOM | Software Bill of Materials | 软件的"配料表" |
| SARIF | Static Analysis Results Interchange Format | 扫描结果的标准格式，GitHub 能直接展示 |
| OIDC | OpenID Connect | CI 用临时身份登录，不用存长期密钥 |
| digest | — | 镜像内容的 SHA256 哈希，不可篡改 |
| DoS | Denial of Service | 让服务瘫痪的攻击 |
| KMS / CMK | Key Management Service / Customer Managed Key | AWS 的密钥管理服务 / 自己管理的密钥 |
| Shift Left | — | 把安全检查往开发流程前面挪 |

---

## 7. 面试时怎么讲（30 秒版本）

> 我给自己写的一个 C++ AWS 服务搭了一条 DevSecOps 流水线。
> 每次提交会并行跑密钥扫描、Semgrep 和 CodeQL、带 ASan 的测试、
> 依赖扫描和 IaC 扫描，全部通过后才构建镜像，
> 扫描镜像、生成 SBOM，在 main 分支用 cosign 无密钥签名后发布。
>
> 第一次扫描发现了 6 个真实问题，比如安全组出站全开、
> 依赖里有 HIGH 级漏洞，我都修了；
> 其余发现逐条评估，豁免理由写在资源旁边。
> 另外所有 Action 都固定到 commit SHA，防止 tag 劫持这类供应链攻击。

可能的追问：
- **为什么 Semgrep 和 CodeQL 都要？** → 一快一深，见 ③
- **为什么按 digest 签名？** → tag 可变，见 ⑦
- **误报怎么处理？** → 修复或带理由豁免，用金丝雀测试验证规则，见 ① 和第 3 节
- **为什么不升级到最新版本？** → 供应链冷静期，见 ⑤

---

## 8. 下一步

- [x] 阶段 1–4：代码 / 依赖 / 镜像 / IaC 扫描、SBOM、签名
- [ ] 阶段 5：用 GitHub OIDC 部署到 AWS（不存长期密钥），部署后用 OWASP ZAP 做 DAST
- [ ] 阶段 6：CloudTrail、GuardDuty、ALB 访问日志、AWS Budgets 费用告警
- [ ] 阶段 7：EKS + ArgoCD (GitOps) + OPA (策略即代码) + Vault + Istio mTLS
- [ ] 阶段 8：AWS Config、kube-bench (CIS 合规)
