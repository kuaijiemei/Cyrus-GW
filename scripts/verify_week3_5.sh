#!/usr/bin/env bash
# =============================================================================
# 模块职责：Week 3 / TODO 3.5 里程碑一键验证入口。
#
# 对外行为：在仓库根目录串行执行
#   scripts/verify_week3_1.sh（3.1 决策机制）
#   scripts/verify_week3_2.sh（3.2 Tool 框架）
#   scripts/verify_week3_3.sh（3.3 Tool + LLM 回填闭环）
# 任一段失败则立即以非 0 退出，避免误报「整周通过」。
#
# 关键分支原因：
# - 使用 set -e：子脚本失败必须中断里程碑，符合「失败即停、便于定位」
# - 使用绝对路径调用子脚本：避免从错误 cwd 启动时找不到 scripts/*.sh
#
# 易踩坑点：
# - 必须在仓库根执行 `bash scripts/verify_week3_5.sh`；若只 cd 到 scripts/
#   再执行，ROOT 解析仍正确，但习惯上统一从根目录跑，与 README 一致
# - 子脚本各自占用不同 Mock/Agent 端口，必须串行；并行跑两份里程碑会端口冲突
# - request_id 由各子脚本自行构造；里程碑层不负责透传，仅编排顺序
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo ""
echo -e "${CYAN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║     Week 3.5 里程碑验证（聚合 3.1 / 3.2 / 3.3）              ║${NC}"
echo -e "${CYAN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo -e "${YELLOW}映射说明（与 TODO 3.5 子项对应）：${NC}"
echo "  • 直接回答路径：verify_week3_1 中 LLM 决策 → direct_answer；verify_week3_3 中降级 direct"
echo "  • tool 路径：verify_week3_1（规则/LLM→tool）、verify_week3_2、verify_week3_3 成功分支"
echo "  • 异常路径：各子脚本均含异常/兜底用例（无效 JSON、坏参数、未知 tool 等）"
echo ""

run_segment() {
    local title="$1"
    local script_path="$2"
    echo -e "${CYAN}────────── ${title} ──────────${NC}"
    bash "${script_path}"
    echo -e "${GREEN}✔ 段完成：${title}${NC}"
    echo ""
}

run_segment "3.1 Agent 决策机制" "${ROOT}/scripts/verify_week3_1.sh"
run_segment "3.2 Tool 框架" "${ROOT}/scripts/verify_week3_2.sh"
run_segment "3.3 Tool + LLM 回填闭环" "${ROOT}/scripts/verify_week3_3.sh"

echo -e "${GREEN}════════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}DONE: Week 3.5 milestone verify finished（3.1 + 3.2 + 3.3 全部通过）${NC}"
echo -e "${GREEN}════════════════════════════════════════════════════════════════${NC}"
exit 0
