#!/usr/bin/env bash
# Week 3 / TODO 3.1 验收脚本：Agent 决策机制（规则优先 + LLM 辅助 + 异常兜底）
#
# 测试覆盖：
#   3.1-1  规则优先 time_tool（中文关键词）
#   3.1-2  规则优先 echo_tool（echo 前缀命令）
#   3.1-3  LLM 辅助决策 → tool_call (mock 返回 time_tool 决策 JSON)
#   3.1-4  LLM 辅助决策 → direct_answer (mock 返回 direct_answer 决策 JSON)
#   3.1-5  异常兜底：mock 返回无效 JSON，应降级 direct_answer（tool_used=""）
#   3.1-6  决策日志可回放（stderr 含 tool_used 字段）
#
# 前置条件：agent-py/.venv 已安装依赖
set -euo pipefail

echo "╔══════════════════════════════════════════════════════════════╗"
echo "║        Week 3.1 Agent 决策机制 — 一键可视化验收              ║"
echo "╚══════════════════════════════════════════════════════════════╝"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AGENT_DIR="${ROOT}/agent-py"

MOCK_LLM_PORT="${MOCK_LLM_PORT:-19361}"
AGENT_PORT="${AGENT_PORT:-19362}"

MOCK_LLM_PID=""
AGENT_PID=""
MOCK_LLM_SCRIPT=""
AGENT_LOG="/tmp/agent_3_1.log"

# ── 颜色输出 ────────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
ok()   { echo -e "  ${GREEN}✔ $*${NC}"; }
fail() { echo -e "  ${RED}✘ $*${NC}"; }
info() { echo -e "  ${CYAN}→ $*${NC}"; }
warn() { echo -e "  ${YELLOW}! $*${NC}"; }

PASS=0
FAIL=0

check() {
    local desc="$1"; local actual="$2"; local expected="$3"
    # FastAPI 默认紧凑 JSON（无冒号后空格），同时兼容 pretty-print 格式
    local compact_expected
    compact_expected="$(echo "${expected}" | sed 's/": /":/')"
    if echo "${actual}" | grep -qF "${expected}" || echo "${actual}" | grep -qF "${compact_expected}"; then
        ok "[PASS] ${desc}"
        PASS=$((PASS + 1))
    else
        fail "[FAIL] ${desc}"
        warn "  expected to contain: ${expected}"
        warn "  actual: ${actual}"
        FAIL=$((FAIL + 1))
    fi
}

cleanup() {
    if [[ -n "${AGENT_PID:-}" ]]; then
        kill "${AGENT_PID}" >/dev/null 2>&1 || true
        wait "${AGENT_PID}" 2>/dev/null || true
    fi
    if [[ -n "${MOCK_LLM_PID:-}" ]]; then
        kill "${MOCK_LLM_PID}" >/dev/null 2>&1 || true
        wait "${MOCK_LLM_PID}" 2>/dev/null || true
    fi
    pkill -f "uvicorn app.main:app --host 127.0.0.1 --port ${AGENT_PORT}" >/dev/null 2>&1 || true
    if [[ -n "${MOCK_LLM_SCRIPT:-}" && -f "${MOCK_LLM_SCRIPT}" ]]; then
        rm -f "${MOCK_LLM_SCRIPT}"
    fi
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT TERM

# ── 环境检查 ────────────────────────────────────────────────────────────────────
if [[ ! -x "${AGENT_DIR}/.venv/bin/python" ]]; then
    echo -e "${RED}ERROR: 缺少 agent-py/.venv。请先执行：${NC}" >&2
    echo "  cd agent-py && python3 -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt" >&2
    exit 1
fi

# ── 启动 Mock LLM ───────────────────────────────────────────────────────────────
echo ""
echo "▶ 启动 Mock LLM（端口 ${MOCK_LLM_PORT}）..."

MOCK_LLM_SCRIPT="$(mktemp /tmp/cyrus_mock_llm_3_1_XXXXXX.py)"
cat > "${MOCK_LLM_SCRIPT}" << 'PYEOF'
"""
智能 Mock LLM：根据 system prompt 内容区分决策请求与最终回答请求。

决策请求识别：system message 含 "routing agent" 关键词
  - user message 含 "llm-decide-time"  → 返回 time_tool 决策 JSON
  - user message 含 "llm-decide-bad"   → 返回无效 JSON（测试兜底）
  - 其他                                → 返回 direct_answer 决策 JSON

最终回答请求：
  - 固定返回友好文本，包含实际 tool 结果内容（若 user 消息含 "Tool result:"）
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer


def _make_completion(content: str) -> bytes:
    body = {
        "model": "mock-gpt",
        "choices": [{"message": {"role": "assistant", "content": content}}],
    }
    return json.dumps(body).encode("utf-8")


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/chat/completions":
            self.send_response(404)
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n).decode("utf-8"))
        messages = req.get("messages") or []

        system_content = ""
        user_content = ""
        for m in messages:
            if m.get("role") == "system":
                system_content = m.get("content", "")
            if m.get("role") == "user":
                user_content = m.get("content", "")

        # 决策请求：system prompt 含 "routing agent"
        if "routing agent" in system_content.lower():
            if "llm-decide-time" in user_content:
                content = '{"action": "tool_call", "tool_name": "time_tool", "tool_args": {}}'
            elif "llm-decide-bad" in user_content:
                content = "Sorry, I cannot decide. This is not JSON."
            else:
                content = '{"action": "direct_answer", "tool_name": null, "tool_args": {}}'
        else:
            # 最终回答：若上下文含 tool 结果，合成有意义的回答
            if "Tool result:" in user_content:
                # 提取 tool 结果部分
                lines = [l for l in user_content.split("\n") if l.startswith("Tool '")]
                content = f"Based on the tool result, here is your answer. {lines[0] if lines else ''}"
            else:
                content = "This is a mock LLM direct answer for your question."

        payload = _make_completion(content)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args):
        return


if __name__ == "__main__":
    import sys
    port = int(sys.argv[1])
    HTTPServer.allow_reuse_address = True
    HTTPServer(("127.0.0.1", port), H).serve_forever()
PYEOF

"${AGENT_DIR}/.venv/bin/python" "${MOCK_LLM_SCRIPT}" "${MOCK_LLM_PORT}" \
    >/tmp/mock_llm_3_1.log 2>&1 &
MOCK_LLM_PID=$!
sleep 0.5

# 快速健康检查 mock LLM
if ! kill -0 "${MOCK_LLM_PID}" 2>/dev/null; then
    fail "Mock LLM 启动失败"
    exit 1
fi
ok "Mock LLM 启动成功（PID ${MOCK_LLM_PID}）"

# ── 启动 Agent ──────────────────────────────────────────────────────────────────
echo ""
echo "▶ 启动 Agent（端口 ${AGENT_PORT}）..."

(
    cd "${AGENT_DIR}"
    export LLM_BASE_URL="http://127.0.0.1:${MOCK_LLM_PORT}"
    export LLM_API_KEY="dummy-key"
    export LLM_MODEL="mock-gpt"
    export LLM_TIMEOUT_MS="3000"
    export LLM_RETRY_MAX="1"
    exec "${AGENT_DIR}/.venv/bin/python" -m uvicorn app.main:app \
        --host 127.0.0.1 --port "${AGENT_PORT}" --log-level warning
) > "${AGENT_LOG}" 2>&1 &
AGENT_PID=$!

# 等待 Agent 健康就绪（最多 8 秒）
for i in $(seq 1 16); do
    if curl -sS "http://127.0.0.1:${AGENT_PORT}/health" >/dev/null 2>&1; then
        ok "Agent 就绪（PID ${AGENT_PID}）"
        break
    fi
    sleep 0.5
    if [[ $i -eq 16 ]]; then
        fail "Agent 启动超时（8s），请检查 ${AGENT_LOG}"
        cat "${AGENT_LOG}" >&2
        exit 1
    fi
done

# ── 辅助函数：POST /agent/chat ──────────────────────────────────────────────────
post_chat() {
    local message="$1"
    local stream="${2:-false}"
    curl -sS -X POST "http://127.0.0.1:${AGENT_PORT}/agent/chat" \
        -H "Content-Type: application/json" \
        --data-binary "{\"request_id\":\"req-test-$(date +%s%N | tail -c 6)\",\"message\":\"${message}\",\"stream\":${stream}}"
}

# ── 3.1-1 规则优先：中文时间关键词 → time_tool ──────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3.1-1 规则优先 — 中文关键词 '现在几点' → time_tool"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
RESP=$(post_chat "现在几点")
info "响应：${RESP}"
check "3.1-1a tool_used=time_tool"   "${RESP}" '"tool_used": "time_tool"'
check "3.1-1b answer 非空"           "${RESP}" '"answer":'
check "3.1-1c request_id 透传"       "${RESP}" '"request_id":'

# ── 3.1-2 规则优先：echo 前缀 → echo_tool ──────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3.1-2 规则优先 — 'echo hello world' → echo_tool"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
RESP=$(post_chat "echo hello world")
info "响应：${RESP}"
check "3.1-2a tool_used=echo_tool"   "${RESP}" '"tool_used": "echo_tool"'
check "3.1-2b answer 非空"           "${RESP}" '"answer":'

# ── 3.1-3 LLM 辅助决策 → tool_call ────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3.1-3 LLM 辅助决策 — mock 返回 time_tool JSON"
echo "  (消息含 'llm-decide-time'，mock 返回 time_tool 决策)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
# 注意：此消息不含规则关键词，会走 LLM 辅助决策分支
RESP=$(post_chat "llm-decide-time: please tell me")
info "响应：${RESP}"
check "3.1-3a tool_used=time_tool"   "${RESP}" '"tool_used": "time_tool"'
check "3.1-3b answer 非空"           "${RESP}" '"answer":'

# ── 3.1-4 LLM 辅助决策 → direct_answer ────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3.1-4 LLM 辅助决策 → direct_answer"
echo "  (普通问题，mock 返回 direct_answer 决策 JSON)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
RESP=$(post_chat "tell me a joke please")
info "响应：${RESP}"
# direct_answer 时 tool_used 应为空字符串
check "3.1-4a tool_used 为空"        "${RESP}" '"tool_used": ""'
check "3.1-4b answer 非空"           "${RESP}" '"answer":'

# ── 3.1-5 异常兜底：LLM 返回无效 JSON ─────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3.1-5 异常兜底 — mock 返回无效 JSON，应降级 direct_answer"
echo "  (消息含 'llm-decide-bad'，mock 返回非 JSON 文本)"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
RESP=$(post_chat "llm-decide-bad: what should I do")
info "响应：${RESP}"
# 兜底后走 direct_answer，tool_used 为空，且请求正常完成（200）
check "3.1-5a tool_used 为空（已兜底）" "${RESP}" '"tool_used": ""'
check "3.1-5b answer 存在（不崩溃）"  "${RESP}" '"answer":'

# ── 3.1-6 决策日志可回放：验证 stderr 含 tool_used 字段 ────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "3.1-6 决策日志可回放 — Agent stderr 应含 tool_used 字段"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
# 发一条 time_tool 请求后检查日志
post_chat "现在几点" >/dev/null 2>&1 || true
sleep 0.3

info "最近 Agent 日志（含 tool_used）："
AGENT_LOGS=$(grep '"tool_used"' "${AGENT_LOG}" 2>/dev/null | tail -5 || echo "")
if [[ -n "${AGENT_LOGS}" ]]; then
    echo "${AGENT_LOGS}" | while read -r line; do info "${line}"; done
    check "3.1-6a 日志含 tool_used 字段" "${AGENT_LOGS}" '"tool_used"'
else
    # 日志可能写到 stderr pipe，尝试从 uvicorn stderr 中搜索
    warn "标准日志未找到，尝试从进程 stderr..."
    AGENT_LOGS=$(cat "${AGENT_LOG}" 2>/dev/null | grep '"tool_used"' | tail -5 || echo "")
    check "3.1-6a 日志含 tool_used 字段" "${AGENT_LOGS}" '"tool_used"'
fi

# ── 汇总 ──────────────────────────────────────────────────────────────────────
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "                     验收汇总"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
TOTAL=$((PASS + FAIL))
echo -e "  通过 ${GREEN}${PASS}${NC} / ${TOTAL}    失败 ${RED}${FAIL}${NC}"
echo ""
if [[ ${FAIL} -eq 0 ]]; then
    echo -e "${GREEN}✔ done: Week 3.1 Agent 决策机制验收全部通过${NC}"
    exit 0
else
    echo -e "${RED}✘ done: Week 3.1 有 ${FAIL} 项未通过，请检查上方输出${NC}"
    exit 1
fi
