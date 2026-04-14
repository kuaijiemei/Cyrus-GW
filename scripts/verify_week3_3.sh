#!/usr/bin/env bash
# Week 3 / TODO 3.3 验收脚本：Tool + LLM 回填闭环
#
# 覆盖目标：
# 1) Agent 将 tool 结果拼接回上下文（验证最终 LLM 请求中含 Tool 返回）
# 2) 再次调用 LLM 组织最终回答（验证 tool 路径触发“决策调用 + 最终回答调用”两次）
# 3) 输出中包含 tool_used（验证响应字段）
# 4) 能演示“为何调用 tool + 最终回答如何生成”（打印决策 JSON 与最终 prompt 片段）
#
# 可视化输出：彩色 PASS / FAIL
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AGENT_DIR="${ROOT}/agent-py"

MOCK_LLM_PORT="${MOCK_LLM_PORT:-19431}"
AGENT_PORT="${AGENT_PORT:-19432}"

MOCK_LLM_PID=""
AGENT_PID=""
MOCK_LLM_SCRIPT=""
TRACE_FILE="/tmp/mock_llm_3_3_trace.jsonl"
AGENT_LOG="/tmp/agent_3_3.log"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

pass_count=0
fail_count=0

ok() {
    echo -e "${GREEN}PASS${NC} $1"
    pass_count=$((pass_count + 1))
}

fail() {
    echo -e "${RED}FAIL${NC} $1"
    fail_count=$((fail_count + 1))
}

info() {
    echo -e "${CYAN}INFO${NC} $1"
}

warn() {
    echo -e "${YELLOW}WARN${NC} $1"
}

cleanup() {
    if [[ -n "${AGENT_PID}" ]]; then
        kill "${AGENT_PID}" >/dev/null 2>&1 || true
        wait "${AGENT_PID}" 2>/dev/null || true
    fi
    if [[ -n "${MOCK_LLM_PID}" ]]; then
        kill "${MOCK_LLM_PID}" >/dev/null 2>&1 || true
        wait "${MOCK_LLM_PID}" 2>/dev/null || true
    fi
    if [[ -n "${MOCK_LLM_SCRIPT}" && -f "${MOCK_LLM_SCRIPT}" ]]; then
        rm -f "${MOCK_LLM_SCRIPT}"
    fi
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT TERM

if [[ ! -x "${AGENT_DIR}/.venv/bin/python" ]]; then
    echo "ERROR: 缺少 ${AGENT_DIR}/.venv/bin/python，请先安装依赖" >&2
    exit 1
fi

echo "===================================================="
echo "Week 3.3 Tool + LLM 回填闭环验收（可视化）"
echo "===================================================="

rm -f "${TRACE_FILE}"

info "Step 1/5 - 启动可追踪 Mock LLM"
MOCK_LLM_SCRIPT="$(mktemp /tmp/cyrus_mock_llm_3_3_XXXXXX.py)"
cat > "${MOCK_LLM_SCRIPT}" <<'PY'
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer

TRACE_FILE = os.environ.get("TRACE_FILE", "/tmp/mock_llm_3_3_trace.jsonl")


def write_trace(obj: dict) -> None:
    with open(TRACE_FILE, "a", encoding="utf-8") as f:
        f.write(json.dumps(obj, ensure_ascii=True) + "\n")


def completion(content: str) -> bytes:
    payload = {
        "model": "mock-gpt",
        "choices": [{"message": {"role": "assistant", "content": content}}],
    }
    return json.dumps(payload).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/chat/completions":
            self.send_response(404)
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(length).decode("utf-8"))
        messages = req.get("messages") or []

        system = ""
        user_msgs = []
        for msg in messages:
            role = str(msg.get("role", ""))
            content = str(msg.get("content", ""))
            if role == "system":
                system = content
            if role == "user":
                user_msgs.append(content)

        # 关键分支：决策调用（routing agent） vs 最终回答调用
        if "routing agent" in system.lower():
            query = user_msgs[-1] if user_msgs else ""
            if "force-tool-route" in query:
                content = '{"action":"tool_call","tool_name":"time_tool","tool_args":{}}'
            elif "force-bad-tool" in query:
                content = '{"action":"tool_call","tool_name":"unknown_tool","tool_args":{}}'
            else:
                content = '{"action":"direct_answer","tool_name":null,"tool_args":{}}'
            write_trace(
                {
                    "phase": "decision",
                    "query": query,
                    "decision_json": content,
                }
            )
        else:
            # 最终回答：若消息中含 Tool 返回，说明 Agent 完成了回填
            user_join = "\n".join(user_msgs)
            if "Tool 'time_tool' returned:" in user_join:
                content = "final-answer-from-tool-context"
            else:
                content = "final-answer-direct"
            write_trace(
                {
                    "phase": "final_answer",
                    "user_context": user_join,
                    "answer": content,
                }
            )

        body = completion(content)
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args):
        return


if __name__ == "__main__":
    import sys

    port = int(sys.argv[1])
    HTTPServer.allow_reuse_address = True
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
PY

TRACE_FILE="${TRACE_FILE}" "${AGENT_DIR}/.venv/bin/python" "${MOCK_LLM_SCRIPT}" "${MOCK_LLM_PORT}" >/tmp/mock_llm_3_3.log 2>&1 &
MOCK_LLM_PID=$!
sleep 0.5
if kill -0 "${MOCK_LLM_PID}" >/dev/null 2>&1; then
    ok "Mock LLM 启动成功"
else
    fail "Mock LLM 启动失败"
    exit 1
fi

info "Step 2/5 - 启动 Agent"
(
    cd "${AGENT_DIR}"
    export LLM_BASE_URL="http://127.0.0.1:${MOCK_LLM_PORT}"
    export LLM_API_KEY="dummy"
    export LLM_MODEL="mock-gpt"
    export LLM_TIMEOUT_MS="3000"
    export LLM_RETRY_MAX="1"
    exec "${AGENT_DIR}/.venv/bin/python" -m uvicorn app.main:app --host 127.0.0.1 --port "${AGENT_PORT}" --log-level warning
) > "${AGENT_LOG}" 2>&1 &
AGENT_PID=$!

ready=0
for _ in $(seq 1 16); do
    if curl -sS "http://127.0.0.1:${AGENT_PORT}/health" >/dev/null 2>&1; then
        ready=1
        break
    fi
    sleep 0.5
done
if [[ "${ready}" -eq 1 ]]; then
    ok "Agent 启动成功"
else
    fail "Agent 启动失败"
    exit 1
fi

info "Step 3/5 - 正常路径：tool 决策 + 回填 + 最终回答"
resp_ok="$(
    curl -sS -X POST "http://127.0.0.1:${AGENT_PORT}/agent/chat" \
    -H "Content-Type: application/json" \
    --data '{"request_id":"req-3-3-ok","message":"force-tool-route","stream":false}'
)"
echo "NORMAL RESP: ${resp_ok}"
if [[ "${resp_ok}" == *'"tool_used":"time_tool"'* ]]; then
    ok "响应中包含 tool_used=time_tool"
else
    fail "响应未包含正确 tool_used"
fi

if [[ "${resp_ok}" == *'"answer":"final-answer-from-tool-context"'* ]]; then
    ok "最终回答来自 tool 回填上下文"
else
    fail "最终回答未体现 tool 回填上下文"
fi

info "Step 4/5 - 异常路径：tool 决策命中未知工具，不崩溃并降级"
resp_bad="$(
    curl -sS -X POST "http://127.0.0.1:${AGENT_PORT}/agent/chat" \
    -H "Content-Type: application/json" \
    --data '{"request_id":"req-3-3-bad","message":"force-bad-tool","stream":false}'
)"
echo "ERROR RESP: ${resp_bad}"
if [[ "${resp_bad}" == *'"tool_used":""'* ]]; then
    ok "异常工具调用降级成功（tool_used 为空）"
else
    fail "异常工具调用未按预期降级"
fi
if kill -0 "${AGENT_PID}" >/dev/null 2>&1; then
    ok "异常路径后 Agent 进程存活"
else
    fail "异常路径导致 Agent 崩溃"
fi

info "Step 5/5 - 可视化演示闭环（为什么调用 tool + 最终回答如何生成）"
trace_report="$(
    "${AGENT_DIR}/.venv/bin/python" - <<PY
import json
from collections import Counter

trace_file = "${TRACE_FILE}"
rows = []
with open(trace_file, "r", encoding="utf-8") as f:
    for line in f:
        line = line.strip()
        if line:
            rows.append(json.loads(line))

phase_counter = Counter(r.get("phase", "") for r in rows)
print(f"TRACE_TOTAL={len(rows)}")
print(f"TRACE_DECISION={phase_counter.get('decision', 0)}")
print(f"TRACE_FINAL={phase_counter.get('final_answer', 0)}")

tool_route_decision = next((r for r in rows if r.get("phase") == "decision" and "force-tool-route" in r.get("query", "")), None)
tool_route_final = next((r for r in rows if r.get("phase") == "final_answer" and "Tool 'time_tool' returned:" in r.get("user_context", "")), None)

if tool_route_decision:
    print("SHOW_DECISION_JSON=" + tool_route_decision.get("decision_json", ""))
else:
    print("SHOW_DECISION_JSON=<missing>")

if tool_route_final:
    context = tool_route_final.get("user_context", "").replace("\n", "\\n")
    print("SHOW_FINAL_CONTEXT=" + context[:220])
else:
    print("SHOW_FINAL_CONTEXT=<missing>")
PY
)"
echo "${trace_report}"

if [[ "${trace_report}" == *'SHOW_DECISION_JSON={"action":"tool_call","tool_name":"time_tool","tool_args":{}}'* ]]; then
    ok "可演示：为何调用 tool（决策 JSON 可见）"
else
    fail "不可演示：未捕获到决策 JSON"
fi

if [[ "${trace_report}" == *"SHOW_FINAL_CONTEXT="*"Tool 'time_tool' returned:"* ]]; then
    ok "可演示：最终回答如何生成（最终上下文含 tool 返回）"
else
    fail "不可演示：未捕获到最终上下文中的 tool 返回"
fi

echo "----------------------------------------------------"
echo "PASS=${pass_count} FAIL=${fail_count}"
if [[ "${fail_count}" -eq 0 ]]; then
    echo -e "${GREEN}DONE Week 3.3 verify passed${NC}"
    exit 0
fi
warn "如需排障，请查看：${TRACE_FILE} 与 ${AGENT_LOG}"
echo -e "${RED}DONE Week 3.3 verify failed${NC}"
exit 1
