#!/usr/bin/env bash
# Week 2 / TODO 2.4 验收脚本：错误处理与状态码统一（400/429/502/504 + request_id + error_layer）。
set -euo pipefail

echo "========== Week 2.4 错误处理与状态码统一验收 =========="

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATEWAY_DIR="${ROOT}/gateway-cpp"
AGENT_DIR="${ROOT}/agent-py"

GW_PORT_A="${GW_PORT_A:-18441}"
GW_PORT_B="${GW_PORT_B:-18442}"
GW_PORT_C="${GW_PORT_C:-18443}"
MOCK_AGENT_PORT="${MOCK_AGENT_PORT:-18451}"
MOCK_LLM_PORT="${MOCK_LLM_PORT:-18452}"
AGENT_PORT="${AGENT_PORT:-18453}"

GW_PID=""
MOCK_AGENT_PID=""
MOCK_LLM_PID=""
AGENT_PID=""
MOCK_AGENT_SCRIPT=""
MOCK_LLM_SCRIPT=""
CFG_A="/tmp/gateway_2_4_a.yaml"
CFG_B="/tmp/gateway_2_4_b.yaml"
CFG_C="/tmp/gateway_2_4_c.yaml"

cleanup() {
    if [[ -n "${GW_PID}" ]]; then
        kill "${GW_PID}" >/dev/null 2>&1 || true
        wait "${GW_PID}" 2>/dev/null || true
        GW_PID=""
    fi
    if [[ -n "${MOCK_AGENT_PID}" ]]; then
        kill "${MOCK_AGENT_PID}" >/dev/null 2>&1 || true
        wait "${MOCK_AGENT_PID}" 2>/dev/null || true
        MOCK_AGENT_PID=""
    fi
    if [[ -n "${MOCK_LLM_PID}" ]]; then
        kill "${MOCK_LLM_PID}" >/dev/null 2>&1 || true
        wait "${MOCK_LLM_PID}" 2>/dev/null || true
        MOCK_LLM_PID=""
    fi
    pkill -f "uvicorn app.main:app --host 127.0.0.1 --port ${AGENT_PORT}" >/dev/null 2>&1 || true
    if [[ -n "${AGENT_PID}" ]]; then
        kill "${AGENT_PID}" >/dev/null 2>&1 || true
        wait "${AGENT_PID}" 2>/dev/null || true
        AGENT_PID=""
    fi
    rm -f "${CFG_A}" "${CFG_B}" "${CFG_C}"
    [[ -n "${MOCK_AGENT_SCRIPT}" && -f "${MOCK_AGENT_SCRIPT}" ]] && rm -f "${MOCK_AGENT_SCRIPT}"
    [[ -n "${MOCK_LLM_SCRIPT}" && -f "${MOCK_LLM_SCRIPT}" ]] && rm -f "${MOCK_LLM_SCRIPT}"
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

MOCK_AGENT_SCRIPT="$(mktemp /tmp/cyrus_mock_agent_2_4_XXXXXX.py)"
cat > "${MOCK_AGENT_SCRIPT}" <<PY
import json
from http.server import BaseHTTPRequestHandler, HTTPServer


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/agent/chat":
            self.send_response(404)
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n).decode("utf-8"))
        req_id = str(req.get("request_id") or "req_unknown")
        body = {"request_id": req_id, "answer": "ok", "tool_used": "", "model": "mock-agent", "retry_count": 0}
        payload = json.dumps(body).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args):
        return


if __name__ == "__main__":
    HTTPServer.allow_reuse_address = True
    HTTPServer(("127.0.0.1", ${MOCK_AGENT_PORT}), H).serve_forever()
PY

cat > "${CFG_A}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_PORT_A}
agent_base_url: "http://127.0.0.1:${MOCK_AGENT_PORT}"
agent_timeout_ms: 800
agent_retry_max: 1
rate_limit:
  capacity: 1
  refill_per_sec: 0
CFG

"${AGENT_DIR}/.venv/bin/python" "${MOCK_AGENT_SCRIPT}" >/tmp/mock_agent_2_4.log 2>&1 &
MOCK_AGENT_PID=$!
"${GATEWAY_DIR}/build/cyrus-gateway" "${CFG_A}" >/tmp/gateway_2_4_a.log 2>&1 &
GW_PID=$!
sleep 1.0

echo ""
echo "========== 2.4-1 400（非法 JSON，响应体应含 request_id + error_layer=gateway）=========="
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT_A}/chat" \
  -H "Content-Type: application/json" \
  -H "X-Request-Id: req-2-4-400" \
  --data-binary '{"message":"x","stream":false' | sed -n '1,25p'

echo ""
echo "========== 2.4-2 429（限流，响应体应含 request_id + error_layer=gateway）=========="
curl -sS -w '\n' -X POST "http://127.0.0.1:${GW_PORT_A}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-2-4-ok","message":"ok","stream":false}'
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT_A}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-2-4-429","message":"hit limit","stream":false}' | sed -n '1,25p'
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""
kill "${MOCK_AGENT_PID}" >/dev/null 2>&1 || true
wait "${MOCK_AGENT_PID}" 2>/dev/null || true
MOCK_AGENT_PID=""

cat > "${CFG_B}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_PORT_B}
agent_base_url: "http://127.0.0.1:19999"
agent_timeout_ms: 400
agent_retry_max: 1
rate_limit:
  capacity: 10
  refill_per_sec: 10
CFG

"${GATEWAY_DIR}/build/cyrus-gateway" "${CFG_B}" >/tmp/gateway_2_4_b.log 2>&1 &
GW_PID=$!
sleep 1.0
echo ""
echo "========== 2.4-3 502（Agent 不可用，error_layer=agent）=========="
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT_B}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-2-4-502","message":"downstream down","stream":false}' | sed -n '1,25p'
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

MOCK_LLM_SCRIPT="$(mktemp /tmp/cyrus_mock_llm_2_4_XXXXXX.py)"
cat > "${MOCK_LLM_SCRIPT}" <<PY
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/chat/completions":
            self.send_response(404)
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(n)
        time.sleep(0.7)
        body = {"model": "mock-gpt", "choices": [{"message": {"role": "assistant", "content": "late"}}]}
        payload = json.dumps(body).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args):
        return


if __name__ == "__main__":
    HTTPServer.allow_reuse_address = True
    HTTPServer(("127.0.0.1", ${MOCK_LLM_PORT}), H).serve_forever()
PY

cat > "${CFG_C}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_PORT_C}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 2500
agent_retry_max: 1
rate_limit:
  capacity: 10
  refill_per_sec: 10
CFG

"${AGENT_DIR}/.venv/bin/python" "${MOCK_LLM_SCRIPT}" >/tmp/mock_llm_2_4.log 2>&1 &
MOCK_LLM_PID=$!
(
  cd "${AGENT_DIR}"
  export LLM_BASE_URL="http://127.0.0.1:${MOCK_LLM_PORT}"
  export LLM_API_KEY="dummy"
  export LLM_MODEL="mock-gpt"
  export LLM_TIMEOUT_MS="300"
  export LLM_RETRY_MAX="1"
  exec "${AGENT_DIR}/.venv/bin/python" -m uvicorn app.main:app --host 127.0.0.1 --port "${AGENT_PORT}"
) >/tmp/agent_2_4.log 2>&1 &
AGENT_PID=$!
"${GATEWAY_DIR}/build/cyrus-gateway" "${CFG_C}" >/tmp/gateway_2_4_c.log 2>&1 &
GW_PID=$!
sleep 1.0

echo ""
echo "========== 2.4-4 504（上游超时，error_layer=agent；Agent 日志含 llm 层）=========="
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT_C}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-2-4-504","message":"slow llm","stream":false}' | sed -n '1,25p'

echo ""
echo "========== 2.4-5 错误日志层级（gateway/agent/llm）=========="
echo "[gateway log - 400/429]"
tail -n 8 /tmp/gateway_2_4_a.log || true
echo "[gateway log - 502]"
tail -n 6 /tmp/gateway_2_4_b.log || true
echo "[gateway log - 504]"
tail -n 6 /tmp/gateway_2_4_c.log || true
echo "[agent log - llm timeout layer]"
tail -n 8 /tmp/agent_2_4.log || true

echo ""
echo "done: Week 2.4 verify finished"
