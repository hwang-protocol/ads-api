#!/usr/bin/env bash
# 在本机跑和 CI 一样的安全扫描 (只需要 Docker，不用安装任何扫描器)
#   scripts/scan-local.sh            # 全部
#   scripts/scan-local.sh secrets    # 只跑某一项: secrets | sast | sca | iac | image
# 扫描器版本从 .github/workflows/devsecops.yml 读取，保证本地和 CI 一致
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
APP=ads-api
WF=.github/workflows/devsecops.yml
img() { sed -n "s/^  $1: //p" "$WF"; }
GITLEAKS=$(img GITLEAKS_IMAGE); SEMGREP=$(img SEMGREP_IMAGE); TRIVY=$(img TRIVY_IMAGE)
HADOLINT=$(img HADOLINT_IMAGE); CHECKOV=$(img CHECKOV_IMAGE)

failed=()
run() {   # run <名字> <命令...>: 失败不中断，最后汇总
  local name=$1; shift
  echo -e "\n=== $name"
  "$@" || failed+=("$name")
}
want() { [[ $# -eq 0 || " $* " == *" $STEP "* ]]; }

secrets() {
  # 本地扫工作目录 (还没提交的文件也算，提交前就拦住)；CI 扫完整 git 历史
  run gitleaks docker run --rm -v "$ROOT":/repo -w /repo "$GITLEAKS" dir /repo --redact --no-banner
}
sast() {
  run semgrep docker run --rm -v "$ROOT":/src -w /src "$SEMGREP" semgrep scan \
    --config p/c --config p/dockerfile --config p/typescript --config p/github-actions --metrics=off --error
}
sca() {
  run trivy-fs docker run --rm -v "$ROOT":/repo -v trivy-cache:/root/.cache -w /repo "$TRIVY" fs \
    --scanners vuln --skip-dirs '**/node_modules' --skip-dirs '**/cdk.out' \
            --severity HIGH,CRITICAL --ignore-unfixed --exit-code 1 .
}
iac() {
  run hadolint sh -c "docker run --rm -i -v '$ROOT/.hadolint.yaml':/.config/hadolint.yaml '$HADOLINT' < '$APP/docker/Dockerfile'"
  run cdk-synth sh -c "cd '$APP/infra' && npm ci --no-audit --no-fund --silent && npx cdk synth -q"
  run checkov docker run --rm -v "$ROOT":/repo -w /repo "$CHECKOV" \
    --config-file .checkov.yaml -f "$APP/infra/cdk.out/AdsCampaignBudget.template.json"
  run trivy-config docker run --rm -v "$ROOT":/repo -v trivy-cache:/root/.cache -w /repo "$TRIVY" config \
    --skip-dirs '**/node_modules' --skip-dirs '**/cdk.out/asset.*' \
            --severity MEDIUM,HIGH,CRITICAL --exit-code 1 .
}
image() {
  run docker-build docker build -t ads-api:local -f "$APP/docker/Dockerfile" .
  run trivy-image docker run --rm -v /var/run/docker.sock:/var/run/docker.sock -v trivy-cache:/root/.cache "$TRIVY" image \
    --severity HIGH,CRITICAL --ignore-unfixed --exit-code 1 ads-api:local
}

for STEP in secrets sast sca iac image; do
  want "$@" && $STEP
done

echo
if ((${#failed[@]})); then echo "FAILED: ${failed[*]}"; exit 1; fi
echo "ALL PASSED"
