#!/usr/bin/env bash
# =================================================================
# verify_week4_4.sh — Week 4.4 演示与讲解材料 一键验证
#
# 职责：校验 §4.4 全部交付物是否完整且内容符合验收标准。
# 对外暴露：PASS/FAIL 计数 + 中文彩色输出。
# =================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

PASS=0
FAIL=0

green()  { printf "\033[0;32m%s\033[0m\n" "$1"; }
red()    { printf "\033[0;31m%s\033[0m\n" "$1"; }
cyan()   { printf "\033[0;36m%s\033[0m\n" "$1"; }

check() {
    local desc="$1"
    local result="$2"
    if [[ "$result" == "ok" ]]; then
        green "[通过] $desc"
        PASS=$((PASS + 1))
    else
        red "[失败] $desc — $result"
        FAIL=$((FAIL + 1))
    fi
}

file_exists() {
    [[ -f "$1" ]] && echo "ok" || echo "文件不存在: $1"
}

file_contains() {
    local file="$1"
    local pattern="$2"
    if [[ -f "$file" ]] && grep -q "$pattern" "$file" 2>/dev/null; then
        echo "ok"
    else
        echo "文件 $file 中未找到: $pattern"
    fi
}

echo "======================================================"
cyan "Week 4.4 演示与讲解材料 验证"
echo "======================================================"

# ----------------------------------------------------------
# 1. DEMO_SCRIPT.md 文件存在
# ----------------------------------------------------------
cyan "[检查] DEMO_SCRIPT.md 存在性"
check "DEMO_SCRIPT.md 文件存在" "$(file_exists "${ROOT}/DEMO_SCRIPT.md")"

# ----------------------------------------------------------
# 2. 架构图（双服务）
# ----------------------------------------------------------
cyan "[检查] 架构图内容"
check "包含架构图（C++ Gateway Service）" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "C++ Gateway Service")"
check "包含架构图（Python Agent Service）" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "Python Agent Service")"
check "包含 Rate Limiter" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "Rate Limiter")"
check "包含 Tool Router" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "Tool")"

# ----------------------------------------------------------
# 3. 时序图（普通对话 / tool 对话）
# ----------------------------------------------------------
cyan "[检查] 时序图内容"
check "包含普通对话时序图" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "普通对话")"
check "包含工具调用时序图" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "工具调用对话")"
check "包含流式 SSE 时序图" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "流式 SSE")"
check "时序图含 request_id 透传说明" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "request_id")"

# ----------------------------------------------------------
# 4. 5 分钟 demo 流程
# ----------------------------------------------------------
cyan "[检查] 5 分钟演示流程"
check "包含演示步骤（非流式）" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "非流式对话")"
check "包含演示步骤（流式）" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "流式 SSE 对话")"
check "包含演示步骤（Tool 调用）" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "Tool 调用路径")"
check "包含演示步骤（限流）" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "限流演示")"
check "包含演示步骤（压测结果）" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "压测结果展示")"
check "包含 curl 命令示例" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "curl")"

# ----------------------------------------------------------
# 5. 常见追问答辩点
# ----------------------------------------------------------
cyan "[检查] 答辩追问点"
check "包含架构取舍问答" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "为什么用两个服务")"
check "包含 io_uring vs epoll 问答" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "为什么主线选 io_uring")"
check "包含后续演进方向" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "后续演进方向")"
check "包含 tool 安全性讨论" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "shell_tool")"

# ----------------------------------------------------------
# 6. TIL Top 问题提炼
# ----------------------------------------------------------
cyan "[检查] TIL Top 问题提炼"
check "包含 Top 问题汇总章节" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "TIL Top 问题提炼")"
check "至少包含 Top 1" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "Top 1")"
check "至少包含 Top 5" \
    "$(file_contains "${ROOT}/DEMO_SCRIPT.md" "Top 5")"

# ----------------------------------------------------------
# 7. TIL.md 本身覆盖度检查
# ----------------------------------------------------------
cyan "[检查] TIL.md 模块覆盖度"
TIL_FILE="${ROOT}/TIL.md"
TIL_ENTRIES=0
if [[ -f "$TIL_FILE" ]]; then
    TIL_ENTRIES=$(grep -c '^\#\#\# \[' "$TIL_FILE" 2>/dev/null || true)
fi
if (( TIL_ENTRIES >= 5 )); then
    check "TIL.md 至少 5 条问题记录（实际 ${TIL_ENTRIES} 条）" "ok"
else
    check "TIL.md 至少 5 条问题记录（实际 ${TIL_ENTRIES} 条）" "不足 5 条"
fi

# ----------------------------------------------------------
# 8. BENCHMARK_RESULTS.md 存在
# ----------------------------------------------------------
cyan "[检查] 压测结果文档"
check "BENCHMARK_RESULTS.md 存在" \
    "$(file_exists "${ROOT}/BENCHMARK_RESULTS.md")"
check "包含并发对比结论" \
    "$(file_contains "${ROOT}/BENCHMARK_RESULTS.md" "为什么主线选择")"

# ----------------------------------------------------------
# 9. 异常路径：文件内容不含禁止项
# ----------------------------------------------------------
cyan "[检查] 禁止项合规（异常路径）"
DEMO="${ROOT}/DEMO_SCRIPT.md"
PROHIBITED_FOUND="ok"
for keyword in "gRPC" "分布式架构" "多租户" "cost 计费"; do
    if grep -q "必须.*${keyword}\|引入.*${keyword}\|实现.*${keyword}" "$DEMO" 2>/dev/null; then
        PROHIBITED_FOUND="DEMO_SCRIPT.md 中发现引入禁止项: ${keyword}"
        break
    fi
done
check "DEMO_SCRIPT.md 未引入 §0.3 禁止项" "$PROHIBITED_FOUND"

echo ""
echo "======================================================"
if (( FAIL == 0 )); then
    green "DONE: Week 4.4 演示与讲解材料 verify passed（PASS=${PASS} FAIL=${FAIL}）"
else
    red "DONE: Week 4.4 verify FAILED（PASS=${PASS} FAIL=${FAIL}）"
fi
echo "======================================================"
exit "$FAIL"
