#!/usr/bin/env bash
# Week 1 非流式端到端可视化验收（Mock LLM + Agent + Gateway，隔离端口）。
# 正常应在数秒内结束；若卡住，多为后台进程未退出，见 TIL「repo/README-week1」。
set -euo pipefail

echo "========== Week 1 里程碑（TODO 1.5 / 复用 1.4 链路）=========="

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATEWAY_DIR="${ROOT}/gateway-cpp"
AGENT_DIR="${ROOT}/agent-py"

MOCK_PORT="${MOCK_PORT:-19000}"
AGENT_PORT="${AGENT_PORT:-18001}"
GW_PORT="${GW_PORT:-18080}"

MOCK_SCRIPT=""
MOCK_PID=""
AGENT_PID=""
GW_PID=""

cleanup() {
    if [[ -n "${GW_PID}" ]]; then
        kill "${GW_PID}" >/dev/null 2>&1 || true
        wait "${GW_PID}" 2>/dev/null || true
        GW_PID=""
    fi
    pkill -f "uvicorn app.main:app --host 127.0.0.1 --port ${AGENT_PORT}" >/dev/null 2>&1 || true
    if [[ -n "${AGENT_PID}" ]]; then
        kill "${AGENT_PID}" >/dev/null 2>&1 || true
        wait "${AGENT_PID}" 2>/dev/null || true
        AGENT_PID=""
    fi
    if [[ -n "${MOCK_PID}" ]]; then
        kill "${MOCK_PID}" >/dev/null 2>&1 || true
        wait "${MOCK_PID}" 2>/dev/null || true
        MOCK_PID=""
    fi
    if [[ -n "${MOCK_SCRIPT}" && -f "${MOCK_SCRIPT}" ]]; then
        rm -f "${MOCK_SCRIPT}"
    fi
}

trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT TERM

if [[ ! -x "${AGENT_DIR}/.venv/bin/python" ]]; then
    echo "ERROR: 缺少 agent-py/.venv。请先执行：" >&2
    echo "  cd agent-py && python3 -m venv .venv && source .venv/bin/activate && pip install -r requirements.txt" >&2
    exit 1
fi

if [[ ! -x "${GATEWAY_DIR}/build/cyrus-gateway" ]]; then
    echo "INFO: 未找到 build/cyrus-gateway，正在用 g++ 编译…" >&2
    (cd "${GATEWAY_DIR}" && mkdir -p build && g++ -std=c++20 -Wall -Wextra -Wpedantic -O2 \
        src/main.cpp src/common/config_loader.cpp src/common/logger.cpp \
        src/net/http_server.cpp src/net/event_loop.cpp \
        src/api/chat_handler.cpp src/scheduler/task_queue.cpp src/scheduler/worker_pool.cpp \
        src/limiter/token_bucket.cpp src/upstream/agent_client.cpp \
        -Isrc -o build/cyrus-gateway)
fi

cat > /tmp/gateway_1_4_test.yaml <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_PORT}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 2500
agent_retry_max: 1
CFG

MOCK_SCRIPT="$(mktemp /tmp/cyrus_mock_llm_XXXXXX.py)"
cat > "${MOCK_SCRIPT}" <<PY
import json
import os
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(os.environ.get("MOCK_PORT", "${MOCK_PORT}"))


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/chat/completions":
            self.send_response(404)
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(n)
        body = {
            "model": "mock-gpt",
            "choices": [{"message": {"role": "assistant", "content": "mock llm says hi"}}],
        }
        data = json.dumps(body).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_args):
        return


if __name__ == "__main__":
    HTTPServer.allow_reuse_address = True
    HTTPServer(("127.0.0.1", PORT), H).serve_forever()
PY

export MOCK_PORT
"${AGENT_DIR}/.venv/bin/python" "${MOCK_SCRIPT}" &
MOCK_PID=$!

(
    cd "${AGENT_DIR}"
    export LLM_BASE_URL="http://127.0.0.1:${MOCK_PORT}"
    export LLM_API_KEY="dummy"
    export LLM_MODEL="mock-gpt"
    exec "${AGENT_DIR}/.venv/bin/python" -m uvicorn app.main:app --host 127.0.0.1 --port "${AGENT_PORT}"
) >/tmp/agent_1_4.log 2>&1 &
AGENT_PID=$!

"${GATEWAY_DIR}/build/cyrus-gateway" /tmp/gateway_1_4_test.yaml >/tmp/gateway_1_4.log 2>&1 &
GW_PID=$!

sleep 1.2

echo "========== 1.4 端到端成功（Gateway -> Agent -> LLM）=========="
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT}/chat" \
    -H "Content-Type: application/json" \
    -d '{"request_id":"req-e2e-1","message":"hello","stream":false}' | sed -n '1,25p'

echo ""
echo "========== 1.4 网关错误（参数非法，期望 400 + error_layer=gateway）=========="
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT}/chat" \
    -H "Content-Type: application/json" \
    -d '{"message":"","stream":false}' | sed -n '1,25p'

echo ""
echo "========== 1.4 上游错误（停掉 Agent，期望 502 + error_layer=agent）=========="
kill "${AGENT_PID}" >/dev/null 2>&1 || true
pkill -f "uvicorn app.main:app --host 127.0.0.1 --port ${AGENT_PORT}" >/dev/null 2>&1 || true
sleep 0.5
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT}/chat" \
    -H "Content-Type: application/json" \
    -d '{"request_id":"req-upstream-down","message":"hello again","stream":false}' | sed -n '1,25p'

echo ""
echo "========== Gateway 访问日志（末尾）=========="
tail -n 8 /tmp/gateway_1_4.log || true

trap - EXIT
cleanup
trap - INT TERM
