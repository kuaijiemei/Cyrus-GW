#!/usr/bin/env bash
# Week 2 / TODO 2.3 验收脚本：超时与最多 1 次重试（Gateway->Agent + Agent->LLM）。
set -euo pipefail

echo "========== Week 2.3 超时与重试验收 =========="

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATEWAY_DIR="${ROOT}/gateway-cpp"
AGENT_DIR="${ROOT}/agent-py"

GW_PORT_A="${GW_PORT_A:-18331}"
GW_PORT_B="${GW_PORT_B:-18332}"
MOCK_AGENT_PORT="${MOCK_AGENT_PORT:-18341}"
MOCK_LLM_PORT="${MOCK_LLM_PORT:-18342}"
AGENT_PORT="${AGENT_PORT:-18343}"

GW_PID=""
AGENT_PID=""
MOCK_AGENT_PID=""
MOCK_LLM_PID=""
MOCK_AGENT_SCRIPT=""
MOCK_LLM_SCRIPT=""
CFG_A="/tmp/gateway_2_3_a.yaml"
CFG_B="/tmp/gateway_2_3_b.yaml"

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
    rm -f "${CFG_A}" "${CFG_B}"
    if [[ -n "${MOCK_AGENT_SCRIPT}" && -f "${MOCK_AGENT_SCRIPT}" ]]; then
        rm -f "${MOCK_AGENT_SCRIPT}"
    fi
    if [[ -n "${MOCK_LLM_SCRIPT}" && -f "${MOCK_LLM_SCRIPT}" ]]; then
        rm -f "${MOCK_LLM_SCRIPT}"
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

MOCK_AGENT_SCRIPT="$(mktemp /tmp/cyrus_mock_agent_2_3_XXXXXX.py)"
cat > "${MOCK_AGENT_SCRIPT}" <<PY
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from collections import defaultdict

counts = defaultdict(int)


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/stats":
            self.send_response(404)
            self.end_headers()
            return
        data = json.dumps(counts).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.path != "/agent/chat":
            self.send_response(404)
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n).decode("utf-8"))
        message = str(req.get("message") or "")
        req_id = str(req.get("request_id") or "req_unknown")
        counts[message] += 1
        if message == "gw-retry-ok" and counts[message] == 1:
            time.sleep(0.35)
        if message == "gw-retry-timeout":
            time.sleep(0.8)
        body = {
            "request_id": req_id,
            "answer": "mock-agent-ok",
            "tool_used": "",
            "model": "mock-agent",
            "retry_count": 0,
        }
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
agent_timeout_ms: 300
agent_retry_max: 1
rate_limit:
  capacity: 100
  refill_per_sec: 100
CFG

"${AGENT_DIR}/.venv/bin/python" "${MOCK_AGENT_SCRIPT}" >/tmp/mock_agent_2_3.log 2>&1 &
MOCK_AGENT_PID=$!
"${GATEWAY_DIR}/build/cyrus-gateway" "${CFG_A}" >/tmp/gateway_2_3_a.log 2>&1 &
GW_PID=$!
sleep 1.0

echo ""
echo "========== 2.3-1 Gateway->Agent 超时重试（首次超时，重试成功）=========="
curl -sS -w '\n' -X POST "http://127.0.0.1:${GW_PORT_A}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-gw-retry-ok","message":"gw-retry-ok","stream":false}'
echo "mock-agent stats:"
curl -sS -w '\n' "http://127.0.0.1:${MOCK_AGENT_PORT}/stats"
echo "gateway log:"
awk 'NR>=1{line=$0} END{print line}' /tmp/gateway_2_3_a.log

echo ""
echo "========== 2.3-2 Gateway->Agent 超时重试（两次超时，返回504，不无限重试）=========="
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT_A}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-gw-retry-timeout","message":"gw-retry-timeout","stream":false}' | sed -n '1,25p'
echo "mock-agent stats:"
curl -sS -w '\n' "http://127.0.0.1:${MOCK_AGENT_PORT}/stats"
echo "gateway log:"
awk 'NR>=1{line=$0} END{print line}' /tmp/gateway_2_3_a.log

kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""
kill "${MOCK_AGENT_PID}" >/dev/null 2>&1 || true
wait "${MOCK_AGENT_PID}" 2>/dev/null || true
MOCK_AGENT_PID=""

MOCK_LLM_SCRIPT="$(mktemp /tmp/cyrus_mock_llm_2_3_XXXXXX.py)"
cat > "${MOCK_LLM_SCRIPT}" <<PY
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from collections import defaultdict

counts = defaultdict(int)


class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/stats":
            self.send_response(404)
            self.end_headers()
            return
        data = json.dumps(counts).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.path != "/chat/completions":
            self.send_response(404)
            self.end_headers()
            return
        n = int(self.headers.get("Content-Length", "0"))
        req = json.loads(self.rfile.read(n).decode("utf-8"))
        messages = req.get("messages") or []
        user_msg = str((messages[-1] if messages else {}).get("content") or "")
        counts[user_msg] += 1
        if user_msg == "llm-retry-ok" and counts[user_msg] == 1:
            time.sleep(0.35)
        if user_msg == "llm-always-timeout":
            time.sleep(0.7)
        body = {
            "model": "mock-gpt",
            "choices": [{"message": {"role": "assistant", "content": "llm-ok"}}],
        }
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

cat > "${CFG_B}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_PORT_B}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 2500
agent_retry_max: 1
rate_limit:
  capacity: 100
  refill_per_sec: 100
CFG

"${AGENT_DIR}/.venv/bin/python" "${MOCK_LLM_SCRIPT}" >/tmp/mock_llm_2_3.log 2>&1 &
MOCK_LLM_PID=$!
(
    cd "${AGENT_DIR}"
    export LLM_BASE_URL="http://127.0.0.1:${MOCK_LLM_PORT}"
    export LLM_API_KEY="dummy"
    export LLM_MODEL="mock-gpt"
    export LLM_TIMEOUT_MS="300"
    export LLM_RETRY_MAX="1"
    exec "${AGENT_DIR}/.venv/bin/python" -m uvicorn app.main:app --host 127.0.0.1 --port "${AGENT_PORT}"
) >/tmp/agent_2_3.log 2>&1 &
AGENT_PID=$!
"${GATEWAY_DIR}/build/cyrus-gateway" "${CFG_B}" >/tmp/gateway_2_3_b.log 2>&1 &
GW_PID=$!
sleep 1.0

echo ""
echo "========== 2.3-3 LLM 超时重试（首次超时，重试成功）=========="
curl -sS -w '\n' -X POST "http://127.0.0.1:${GW_PORT_B}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-llm-retry-ok","message":"llm-retry-ok","stream":false}'
echo "mock-llm stats:"
curl -sS -w '\n' "http://127.0.0.1:${MOCK_LLM_PORT}/stats"
echo "agent log:"
awk 'NR>=1{line=$0} END{print line}' /tmp/agent_2_3.log

echo ""
echo "========== 2.3-4 LLM 超时重试（两次超时，返回504，不无限重试）=========="
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_PORT_B}/chat" \
  -H "Content-Type: application/json" \
  --data-binary '{"request_id":"req-llm-retry-timeout","message":"llm-always-timeout","stream":false}' | sed -n '1,25p'
echo "mock-llm stats:"
curl -sS -w '\n' "http://127.0.0.1:${MOCK_LLM_PORT}/stats"
echo "agent log:"
awk 'NR>=1{line=$0} END{print line}' /tmp/agent_2_3.log

echo ""
echo "done: Week 2.3 verify finished"
