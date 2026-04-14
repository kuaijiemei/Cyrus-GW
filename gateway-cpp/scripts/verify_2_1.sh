#!/usr/bin/env bash
# Week 2 / TODO 2.1 验收脚本：SSE 端到端 + 首包超时 + 总超时中断 + 非流式回归。
# 模块职责：
#   1) 启动 mock LLM（含流式与非流式路径）
#   2) 启动 Agent 与 Gateway（隔离端口）
#   3) 可视化打印 4 组结果，便于按 TODO 2.1 勾选
set -euo pipefail

echo "========== Week 2.1 SSE 端到端验收 =========="

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATEWAY_DIR="${ROOT}/gateway-cpp"
AGENT_DIR="${ROOT}/agent-py"

MOCK_PORT="${MOCK_PORT:-19110}"
AGENT_PORT="${AGENT_PORT:-18110}"
GW_STREAM_OK_PORT="${GW_STREAM_OK_PORT:-18181}"
GW_FIRST_TIMEOUT_PORT="${GW_FIRST_TIMEOUT_PORT:-18182}"
GW_STREAM_TIMEOUT_PORT="${GW_STREAM_TIMEOUT_PORT:-18183}"
GW_NONSTREAM_PORT="${GW_NONSTREAM_PORT:-18184}"

MOCK_SCRIPT=""
MOCK_PID=""
AGENT_PID=""
GW_PID=""
TMP_CFG_STREAM_OK="/tmp/gateway_2_1_stream_ok.yaml"
TMP_CFG_FIRST_TIMEOUT="/tmp/gateway_2_1_first_timeout.yaml"
TMP_CFG_STREAM_TIMEOUT="/tmp/gateway_2_1_stream_timeout.yaml"
TMP_CFG_NONSTREAM="/tmp/gateway_2_1_nonstream.yaml"

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
    rm -f "${TMP_CFG_STREAM_OK}" "${TMP_CFG_FIRST_TIMEOUT}" "${TMP_CFG_STREAM_TIMEOUT}" "${TMP_CFG_NONSTREAM}"
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

MOCK_SCRIPT="$(mktemp /tmp/cyrus_mock_week2_1_XXXXXX.py)"
cat > "${MOCK_SCRIPT}" <<PY
import json
import os
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(os.environ.get("MOCK_PORT", "${MOCK_PORT}"))


class H(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/chat/completions":
            self.send_response(404)
            self.end_headers()
            return

        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n)
        req = json.loads(raw.decode("utf-8")) if raw else {}
        stream = bool(req.get("stream"))
        message = ""
        msgs = req.get("messages") or []
        if msgs and isinstance(msgs, list):
            message = str((msgs[-1] or {}).get("content") or "")

        if stream:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()

            if message == "slow-second-chunk":
                # 关键分支原因：先发首包，确保不是 first_chunk_timeout；再延迟触发 total_timeout。
                first = {"choices": [{"delta": {"content": "start "}}]}
                self.wfile.write(f"data: {json.dumps(first)}\\n\\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(1.6)
                second = {"choices": [{"delta": {"content": "late"}}]}
                self.wfile.write(f"data: {json.dumps(second)}\\n\\n".encode("utf-8"))
                self.wfile.write(b"data: [DONE]\\n\\n")
                self.wfile.flush()
                return
            if message == "first-chunk-timeout":
                # 关键分支原因：首包前延迟，触发 gateway first_chunk_timeout(504)。
                time.sleep(1.2)
                payload = {"choices": [{"delta": {"content": "too-late-first"}}]}
                self.wfile.write(f"data: {json.dumps(payload)}\\n\\n".encode("utf-8"))
                self.wfile.write(b"data: [DONE]\\n\\n")
                self.wfile.flush()
                return

            for c in ["mock ", "stream ", "works"]:
                payload = {"choices": [{"delta": {"content": c}}]}
                self.wfile.write(f"data: {json.dumps(payload)}\\n\\n".encode("utf-8"))
                self.wfile.flush()
                time.sleep(0.15)
            self.wfile.write(b"data: [DONE]\\n\\n")
            self.wfile.flush()
            return

        body = {
            "model": "mock-gpt",
            "choices": [{"message": {"role": "assistant", "content": "nonstream-ok"}}],
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
"${AGENT_DIR}/.venv/bin/python" "${MOCK_SCRIPT}" >/tmp/mock_week2_1.log 2>&1 &
MOCK_PID=$!

(
    cd "${AGENT_DIR}"
    export LLM_BASE_URL="http://127.0.0.1:${MOCK_PORT}"
    export LLM_API_KEY="dummy"
    export LLM_MODEL="mock-gpt"
    exec "${AGENT_DIR}/.venv/bin/python" -m uvicorn app.main:app --host 127.0.0.1 --port "${AGENT_PORT}"
) >/tmp/agent_week2_1.log 2>&1 &
AGENT_PID=$!

sleep 1.0

cat > "${TMP_CFG_STREAM_OK}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_STREAM_OK_PORT}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 4000
agent_retry_max: 1
sse:
  first_chunk_timeout_ms: 3000
  total_timeout_ms: 15000
CFG

cat > "${TMP_CFG_STREAM_TIMEOUT}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_STREAM_TIMEOUT_PORT}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 4000
agent_retry_max: 1
sse:
  first_chunk_timeout_ms: 3000
  total_timeout_ms: 1000
CFG

cat > "${TMP_CFG_FIRST_TIMEOUT}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_FIRST_TIMEOUT_PORT}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 4000
agent_retry_max: 1
sse:
  first_chunk_timeout_ms: 500
  total_timeout_ms: 5000
CFG

cat > "${TMP_CFG_NONSTREAM}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_NONSTREAM_PORT}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 4000
agent_retry_max: 1
CFG

echo ""
echo "========== 2.1-1 正常流式（期望 delta...done）=========="
"${GATEWAY_DIR}/build/cyrus-gateway" "${TMP_CFG_STREAM_OK}" >/tmp/gateway_week2_1_ok.log 2>&1 &
GW_PID=$!
sleep 1.0
curl -sS -N -X POST "http://127.0.0.1:${GW_STREAM_OK_PORT}/chat" \
    -H "Content-Type: application/json" \
    --data-binary '{"request_id":"req-week2-1-ok","message":"stream test","stream":true}'
echo ""
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

echo ""
echo "========== 2.1-2 首包超时（期望 HTTP 504 + detail:first_chunk_timeout）=========="
"${GATEWAY_DIR}/build/cyrus-gateway" "${TMP_CFG_FIRST_TIMEOUT}" >/tmp/gateway_week2_1_first_timeout.log 2>&1 &
GW_PID=$!
sleep 1.0
curl -sS -w '\n' -D - -X POST "http://127.0.0.1:${GW_FIRST_TIMEOUT_PORT}/chat" \
    -H "Content-Type: application/json" \
    --data-binary '{"request_id":"req-week2-1-first-timeout","message":"first-chunk-timeout","stream":true}' | sed -n '1,25p'
echo ""
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

echo ""
echo "========== 2.1-3 总超时中断（期望 delta 后出现 error: stream_total_timeout）=========="
"${GATEWAY_DIR}/build/cyrus-gateway" "${TMP_CFG_STREAM_TIMEOUT}" >/tmp/gateway_week2_1_timeout.log 2>&1 &
GW_PID=$!
sleep 1.0
curl -sS -N -X POST "http://127.0.0.1:${GW_STREAM_TIMEOUT_PORT}/chat" \
    -H "Content-Type: application/json" \
    --data-binary '{"request_id":"req-week2-1-timeout","message":"slow-second-chunk","stream":true}'
echo ""
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

echo ""
echo "========== 2.1-4 非流式回归（期望标准 JSON）=========="
"${GATEWAY_DIR}/build/cyrus-gateway" "${TMP_CFG_NONSTREAM}" >/tmp/gateway_week2_1_nonstream.log 2>&1 &
GW_PID=$!
sleep 1.0
curl -sS -w '\n' -X POST "http://127.0.0.1:${GW_NONSTREAM_PORT}/chat" \
    -H "Content-Type: application/json" \
    --data-binary '{"request_id":"req-week2-1-nonstream","message":"hello","stream":false}'
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

echo ""
echo "========== Gateway 日志片段（含 ttft_ms）=========="
{ tail -n 6 /tmp/gateway_week2_1_ok.log; tail -n 6 /tmp/gateway_week2_1_first_timeout.log; tail -n 6 /tmp/gateway_week2_1_timeout.log; } 2>/dev/null || true

echo ""
echo "done: Week 2.1 verify finished"
