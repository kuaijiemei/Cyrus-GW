#!/usr/bin/env bash
# Week 3 / TODO 3.2 验收脚本：Tool 框架
# 覆盖：
# - 统一 tool 协议（输入/输出/错误码）可用
# - time_tool / echo_tool 可执行
# - tool_router 路由可执行
# - tool 执行错误可控返回（不崩溃）
# - 至少 1 条请求可复现 tool 成功调用
# - tool 异常请求不会导致 Agent 进程崩溃
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AGENT_DIR="${ROOT}/agent-py"

MOCK_LLM_PORT="${MOCK_LLM_PORT:-19421}"
AGENT_PORT="${AGENT_PORT:-19422}"
MOCK_LLM_PID=""
AGENT_PID=""
MOCK_LLM_SCRIPT=""
AGENT_LOG="/tmp/agent_3_2.log"

GREEN='\033[0;32m'
RED='\033[0;31m'
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
echo "Week 3.2 Tool 框架验收（可视化）"
echo "===================================================="

info "Step 1/4 - 协议层自检（ToolInvokeRequest/Response + ToolRouter.execute）"
protocol_output="$(
    cd "${AGENT_DIR}" && "${AGENT_DIR}/.venv/bin/python" - <<'PY'
from app.tool_router import ToolRouter
from app.tools.contracts import ToolInvokeRequest, ToolErrorCode

router = ToolRouter()

resp_ok = router.execute(
    ToolInvokeRequest(request_id="req-protocol-1", tool_name="time_tool", tool_args={})
)
print(f"PROTO_OK={resp_ok.ok}")
print(f"PROTO_TOOL={resp_ok.tool_name}")
print(f"PROTO_ERR={resp_ok.error_code}")

resp_unknown = router.execute(
    ToolInvokeRequest(request_id="req-protocol-2", tool_name="missing_tool", tool_args={})
)
print(f"UNKNOWN_OK={resp_unknown.ok}")
print(f"UNKNOWN_ERR={resp_unknown.error_code}")

resp_bad_args = router.execute(
    ToolInvokeRequest(request_id="req-protocol-3", tool_name="echo_tool", tool_args={"content": "x"})
)
print(f"BADARGS_OK={resp_bad_args.ok}")
print(f"BADARGS_ERR={resp_bad_args.error_code}")

assert resp_ok.ok is True
assert resp_ok.error_code == ToolErrorCode.NONE
assert resp_unknown.ok is False
assert resp_unknown.error_code == ToolErrorCode.TOOL_NOT_FOUND
assert resp_bad_args.ok is False
assert resp_bad_args.error_code == ToolErrorCode.TOOL_BAD_ARGS
PY
)"
echo "${protocol_output}"
ok "统一 tool 协议输入/输出/错误码可用"

info "Step 2/4 - 启动 Mock LLM（用于触发 LLM 辅助 tool 决策）"
MOCK_LLM_SCRIPT="$(mktemp /tmp/cyrus_mock_llm_3_2_XXXXXX.py)"
cat > "${MOCK_LLM_SCRIPT}" <<'PY'
import json
from http.server import BaseHTTPRequestHandler, HTTPServer


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
        user = ""
        for msg in messages:
            if msg.get("role") == "system":
                system = str(msg.get("content") or "")
            if msg.get("role") == "user":
                user = str(msg.get("content") or "")

        # 决策调用：system prompt 包含 routing agent
        if "routing agent" in system.lower():
            if "route-time" in user:
                content = '{"action":"tool_call","tool_name":"time_tool","tool_args":{}}'
            elif "route-echo-badargs" in user:
                content = '{"action":"tool_call","tool_name":"echo_tool","tool_args":{"content":"bad"}}'
            elif "route-unknown-tool" in user:
                content = '{"action":"tool_call","tool_name":"unknown_tool","tool_args":{}}'
            else:
                content = '{"action":"direct_answer","tool_name":null,"tool_args":{}}'
        else:
            content = "mock final answer"

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

"${AGENT_DIR}/.venv/bin/python" "${MOCK_LLM_SCRIPT}" "${MOCK_LLM_PORT}" >/tmp/mock_llm_3_2.log 2>&1 &
MOCK_LLM_PID=$!
sleep 0.5
if kill -0 "${MOCK_LLM_PID}" >/dev/null 2>&1; then
    ok "Mock LLM 启动成功"
else
    fail "Mock LLM 启动失败"
fi

info "Step 3/4 - 启动 Agent"
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

info "Step 4/4 - HTTP 请求验收（成功路径 + 异常路径）"

resp_success="$(
    curl -sS -X POST "http://127.0.0.1:${AGENT_PORT}/agent/chat" \
    -H "Content-Type: application/json" \
    --data '{"request_id":"req-3-2-ok","message":"route-time","stream":false}'
)"
echo "SUCCESS RESP: ${resp_success}"
if [[ "${resp_success}" == *'"tool_used":"time_tool"'* ]]; then
    ok "至少 1 条请求可复现 tool 成功调用（time_tool）"
else
    fail "未复现成功 tool 调用（预期 tool_used=time_tool）"
fi

resp_bad_args="$(
    curl -sS -X POST "http://127.0.0.1:${AGENT_PORT}/agent/chat" \
    -H "Content-Type: application/json" \
    --data '{"request_id":"req-3-2-badargs","message":"route-echo-badargs","stream":false}'
)"
echo "BADARGS RESP: ${resp_bad_args}"
if [[ "${resp_bad_args}" == *'"tool_used":""'* ]]; then
    ok "tool 参数异常可控返回（降级 direct_answer，tool_used 为空）"
else
    fail "tool 参数异常未按预期降级"
fi

resp_unknown="$(
    curl -sS -X POST "http://127.0.0.1:${AGENT_PORT}/agent/chat" \
    -H "Content-Type: application/json" \
    --data '{"request_id":"req-3-2-unknown","message":"route-unknown-tool","stream":false}'
)"
echo "UNKNOWN RESP: ${resp_unknown}"
if [[ "${resp_unknown}" == *'"tool_used":""'* ]]; then
    ok "未知 tool 可控返回（降级 direct_answer，tool_used 为空）"
else
    fail "未知 tool 未按预期降级"
fi

if kill -0 "${AGENT_PID}" >/dev/null 2>&1; then
    ok "tool 异常不会导致 Agent 进程崩溃"
else
    fail "Agent 进程异常退出"
fi

echo "----------------------------------------------------"
echo "PASS=${pass_count} FAIL=${fail_count}"
if [[ "${fail_count}" -eq 0 ]]; then
    echo -e "${GREEN}DONE Week 3.2 verify passed${NC}"
    exit 0
fi
echo -e "${RED}DONE Week 3.2 verify failed${NC}"
exit 1
