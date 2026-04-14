#!/usr/bin/env bash
# Week 2 / TODO 2.5 里程碑验收：聚合执行 2.1~2.4 可视化脚本。
# 模块职责：提供单入口演示「SSE + 限流 + 重试 + 错误统一」完整链路。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "========== Week 2 里程碑总验收（TODO 2.5）=========="

run_step() {
    local title="$1"
    local cmd="$2"
    echo ""
    echo "----- ${title} -----"
    bash -lc "${cmd}"
}

run_step "2.1 SSE 端到端" "bash \"${ROOT}/scripts/verify_week2_1.sh\""
run_step "2.2 限流 Token Bucket" "bash \"${ROOT}/scripts/verify_week2_2.sh\""
run_step "2.3 超时与重试" "bash \"${ROOT}/scripts/verify_week2_3.sh\""
run_step "2.4 错误处理与状态码统一" "bash \"${ROOT}/scripts/verify_week2_4.sh\""

echo ""
echo "done: Week 2 milestone verify finished"
