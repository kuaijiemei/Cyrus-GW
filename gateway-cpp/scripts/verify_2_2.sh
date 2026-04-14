#!/usr/bin/env bash
# Week 2 / TODO 2.2 验收脚本：Token Bucket 限流可视化验证。
# 模块职责：
#   1) 启动 mock LLM + Agent + Gateway
#   2) 用严格配置稳定触发 429
#   3) 用宽松配置验证行为随参数变化
set -euo pipefail

echo "========== Week 2.2 限流（Token Bucket）验收 =========="

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATEWAY_DIR="${ROOT}/gateway-cpp"
AGENT_DIR="${ROOT}/agent-py"

MOCK_PORT="${MOCK_PORT:-19220}"
AGENT_PORT="${AGENT_PORT:-18220}"
GW_STRICT_PORT="${GW_STRICT_PORT:-18221}"
GW_RELAXED_PORT="${GW_RELAXED_PORT:-18222}"

MOCK_SCRIPT=""
MOCK_PID=""
AGENT_PID=""
GW_PID=""
TMP_CFG_STRICT="/tmp/gateway_2_2_strict.yaml"
TMP_CFG_RELAXED="/tmp/gateway_2_2_relaxed.yaml"

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
    rm -f "${TMP_CFG_STRICT}" "${TMP_CFG_RELAXED}"
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

MOCK_SCRIPT="$(mktemp /tmp/cyrus_mock_week2_2_XXXXXX.py)"
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
            "choices": [{"message": {"role": "assistant", "content": "rate-limit-ok"}}],
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
"${AGENT_DIR}/.venv/bin/python" "${MOCK_SCRIPT}" >/tmp/mock_week2_2.log 2>&1 &
MOCK_PID=$!

(
    cd "${AGENT_DIR}"
    export LLM_BASE_URL="http://127.0.0.1:${MOCK_PORT}"
    export LLM_API_KEY="dummy"
    export LLM_MODEL="mock-gpt"
    exec "${AGENT_DIR}/.venv/bin/python" -m uvicorn app.main:app --host 127.0.0.1 --port "${AGENT_PORT}"
) >/tmp/agent_week2_2.log 2>&1 &
AGENT_PID=$!

sleep 1.0

cat > "${TMP_CFG_STRICT}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_STRICT_PORT}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 3000
agent_retry_max: 1
rate_limit:
  capacity: 2
  refill_per_sec: 0
CFG

cat > "${TMP_CFG_RELAXED}" <<CFG
listen_host: "127.0.0.1"
listen_port: ${GW_RELAXED_PORT}
agent_base_url: "http://127.0.0.1:${AGENT_PORT}"
agent_timeout_ms: 3000
agent_retry_max: 1
rate_limit:
  capacity: 10
  refill_per_sec: 100
CFG

run_burst() {
    local port="$1"
    local tag="$2"
    local out_file="$3"
    : > "${out_file}"
    for i in 1 2 3 4 5 6; do
        code="$(curl -sS -o /tmp/resp_week2_2_${tag}_${i}.json -w '%{http_code}' \
            -X POST "http://127.0.0.1:${port}/chat" \
            -H "Content-Type: application/json" \
            --data-binary "{\"request_id\":\"req-${tag}-${i}\",\"message\":\"hello ${i}\",\"stream\":false}")"
        echo "${code}" >> "${out_file}"
    done
}

echo ""
echo "========== 2.2-1 严格限流（期望稳定触发 429）=========="
"${GATEWAY_DIR}/build/cyrus-gateway" "${TMP_CFG_STRICT}" >/tmp/gateway_week2_2_strict.log 2>&1 &
GW_PID=$!
sleep 1.0
run_burst "${GW_STRICT_PORT}" "strict" "/tmp/codes_week2_2_strict.txt"
STRICT_OK="$(awk '$1=="200"{c++} END{print c+0}' /tmp/codes_week2_2_strict.txt)"
STRICT_429="$(awk '$1=="429"{c++} END{print c+0}' /tmp/codes_week2_2_strict.txt)"
echo "strict codes: $(tr '\n' ' ' < /tmp/codes_week2_2_strict.txt)"
echo "strict summary: 200=${STRICT_OK}, 429=${STRICT_429}"
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

echo ""
echo "========== 2.2-2 宽松限流（期望同样请求下 429 显著减少）=========="
"${GATEWAY_DIR}/build/cyrus-gateway" "${TMP_CFG_RELAXED}" >/tmp/gateway_week2_2_relaxed.log 2>&1 &
GW_PID=$!
sleep 1.0
run_burst "${GW_RELAXED_PORT}" "relaxed" "/tmp/codes_week2_2_relaxed.txt"
RELAXED_OK="$(awk '$1=="200"{c++} END{print c+0}' /tmp/codes_week2_2_relaxed.txt)"
RELAXED_429="$(awk '$1=="429"{c++} END{print c+0}' /tmp/codes_week2_2_relaxed.txt)"
echo "relaxed codes: $(tr '\n' ' ' < /tmp/codes_week2_2_relaxed.txt)"
echo "relaxed summary: 200=${RELAXED_OK}, 429=${RELAXED_429}"
kill "${GW_PID}" >/dev/null 2>&1 || true
wait "${GW_PID}" 2>/dev/null || true
GW_PID=""

echo ""
echo "========== 2.2-3 限流命中日志（tool_used=rate_limited）=========="
tail -n 12 /tmp/gateway_week2_2_strict.log || true

echo ""
echo "done: Week 2.2 verify finished"
