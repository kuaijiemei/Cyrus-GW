#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Week 4.1 验证脚本 —— 并发模型准备
# 使用方式：bash scripts/verify_week4_1.sh
# ---------------------------------------------------------------------------
set -uo pipefail

RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[0;33m'; CYN='\033[0;36m'; RST='\033[0m'
PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo -e "  ${GRN}✔ PASS${RST}: $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "  ${RED}✘ FAIL${RST}: $1"; }
skip() { SKIP=$((SKIP+1)); echo -e "  ${YEL}⊘ SKIP${RST}: $1"; }
info() { echo -e "  ${CYN}ℹ${RST}  $1"; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GW_DIR="${ROOT}/gateway-cpp"
BIN="${GW_DIR}/build/cyrus-gateway"
PIDS=()

cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
    for p in "${PIDS[@]}"; do wait "$p" 2>/dev/null || true; done
    rm -f /tmp/mock_llm_w41_*.py 2>/dev/null || true
}
trap cleanup EXIT

wait_port() {
    local port=$1 max=${2:-30}
    for ((i=0;i<max;i++)); do
        if curl -sS --connect-timeout 1 "http://127.0.0.1:${port}/health" >/dev/null 2>&1; then return 0; fi
        sleep 0.5
    done
    return 1
}

# ========================= §1 编译检查 =========================
echo -e "\n${CYN}[4.1-1] 编译检查${RST}"

if [[ ! -x "$BIN" ]]; then
    info "binary not found, building..."
    cd "$GW_DIR"
    if command -v cmake &>/dev/null && command -v ninja &>/dev/null; then
        cmake -S . -B build -G Ninja >/dev/null 2>&1
        cmake --build build -j >/dev/null 2>&1
    else
        mkdir -p build
        g++ -std=c++20 -Wall -Wextra -Wpedantic -fcoroutines -O2 -pthread \
            src/main.cpp src/common/config_loader.cpp src/common/logger.cpp \
            src/net/http_server.cpp src/net/event_loop.cpp \
            src/net/iouring_server.cpp src/net/epoll_server.cpp \
            src/api/chat_handler.cpp src/scheduler/task_queue.cpp \
            src/scheduler/worker_pool.cpp src/limiter/token_bucket.cpp \
            src/upstream/agent_client.cpp \
            -Isrc -o build/cyrus-gateway 2>&1
    fi
    cd "$ROOT"
fi

if [[ -x "$BIN" ]]; then
    pass "cyrus-gateway binary exists and is executable"
else
    fail "cyrus-gateway binary not found"; exit 1
fi

# ========================= §2 协程符号检查 =========================
echo -e "\n${CYN}[4.1-2] C++20 协程符号检查（io_uring 代码已链入）${RST}"

if strings "$BIN" 2>/dev/null | grep -q 'iouring_disabled_by_kernel\|iouring listening'; then
    pass "binary contains io_uring startup strings (code linked)"
elif nm "$BIN" 2>/dev/null | grep -q 'coroutine\|UringAwaiter\|accept_loop'; then
    pass "binary contains coroutine / io_uring symbols"
else
    skip "could not confirm coroutine symbols (may still be present)"
fi

# ========================= §3 io_uring 内核可用性 =========================
echo -e "\n${CYN}[4.1-3] io_uring 内核可用性${RST}"

IOURING_OK=false
IOURING_DISABLED=$(cat /proc/sys/kernel/io_uring_disabled 2>/dev/null || echo "unknown")
if [[ "$IOURING_DISABLED" == "0" ]]; then
    IOURING_OK=true
    pass "kernel.io_uring_disabled=0 (io_uring available)"
else
    info "kernel.io_uring_disabled=${IOURING_DISABLED}"
    skip "io_uring runtime tests — kernel has disabled io_uring"
    info "To enable:  sudo sysctl -w kernel.io_uring_disabled=0"
fi

# ========================= §4 启动 mock LLM + Agent =========================
echo -e "\n${CYN}[4.1-4] 启动 mock LLM + Agent${RST}"

MOCK_PY=$(mktemp /tmp/mock_llm_w41_XXXX.py)
cat > "$MOCK_PY" <<'PYEOF'
from http.server import HTTPServer, BaseHTTPRequestHandler
import json
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        cl = int(self.headers.get("Content-Length", 0))
        if cl: self.rfile.read(cl)
        r = json.dumps({"choices":[{"message":{"content":"w41 mock reply"}}],"model":"mock"})
        self.send_response(200)
        self.send_header("Content-Type","application/json")
        self.send_header("Content-Length",str(len(r)))
        self.end_headers()
        self.wfile.write(r.encode())
    def log_message(self, *a): pass
HTTPServer(("127.0.0.1", 19941), H).serve_forever()
PYEOF
python3 "$MOCK_PY" &
PIDS+=($!)
sleep 0.5

if [[ ! -f "${ROOT}/agent-py/.venv/bin/python" ]]; then
    fail "agent-py .venv not found"; exit 1
fi

(
    cd "${ROOT}/agent-py"
    source .venv/bin/activate
    LLM_BASE_URL="http://127.0.0.1:19941" LLM_API_KEY="test" LLM_MODEL="mock" LLM_TIMEOUT_MS=5000 \
        exec python3 -m uvicorn app.main:app --host 127.0.0.1 --port 8001 >/dev/null 2>&1
) &
PIDS+=($!)

if wait_port 8001 20; then
    pass "mock LLM + Agent started on :8001"
else
    fail "Agent failed to start on :8001"; exit 1
fi

# ---------- helper: test a gateway mode ----------
test_mode() {
    local mode=$1 label=$2
    echo -e "\n${CYN}[4.1-${label}] ${mode} 模式 e2e${RST}"

    cd "$GW_DIR"
    ./build/cyrus-gateway --mode="${mode}" 2>"/tmp/gw_${mode}_w41.log" &
    local gw_pid=$!
    PIDS+=("$gw_pid")

    if ! wait_port 8080 10; then
        fail "${mode} gateway did not start"
        kill "$gw_pid" 2>/dev/null || true
        wait "$gw_pid" 2>/dev/null || true
        return
    fi

    # health
    local resp
    resp=$(curl -sS http://127.0.0.1:8080/health 2>&1)
    if [[ "$resp" == *'"status":"ok"'* ]]; then
        pass "${mode} /health returns ok"
    else
        fail "${mode} /health: $resp"
    fi

    # non-stream /chat
    resp=$(curl -sS -m 15 -X POST http://127.0.0.1:8080/chat \
        -H "Content-Type: application/json" \
        -d '{"message":"hello from '"${mode}"' test","stream":false}' 2>&1)
    if [[ "$resp" == *'"answer"'* ]]; then
        pass "${mode} /chat non-stream returns answer"
    else
        fail "${mode} /chat non-stream: $resp"
    fi

    # 400 bad request
    resp=$(curl -sS -m 5 -X POST http://127.0.0.1:8080/chat \
        -H "Content-Type: application/json" \
        -d '{}' 2>&1)
    if [[ "$resp" == *'"error"'* ]]; then
        pass "${mode} /chat bad request returns error"
    else
        fail "${mode} /chat bad request: $resp"
    fi

    kill "$gw_pid" 2>/dev/null || true
    wait "$gw_pid" 2>/dev/null || true
    sleep 0.5
}

# ========================= §5 epoll e2e =========================
test_mode epoll 5

# ========================= §6 blocking 回归 =========================
test_mode blocking 6

# ========================= §7 io_uring（条件） =========================
echo -e "\n${CYN}[4.1-7] io_uring + coroutines 模式${RST}"

if $IOURING_OK; then
    test_mode iouring 7
else
    cd "$GW_DIR"
    output=$(timeout 3 ./build/cyrus-gateway --mode=iouring 2>&1 || true)
    if echo "$output" | grep -q 'iouring_disabled_by_kernel\|io_uring is DISABLED'; then
        pass "iouring mode detects disabled kernel and exits gracefully"
    else
        fail "iouring mode did not produce expected diagnostic"
    fi
fi

# ========================= §8 --mode 参数完整性 =========================
echo -e "\n${CYN}[4.1-8] --mode 参数切换正确${RST}"

cd "$GW_DIR"
for m in iouring epoll blocking; do
    output=$(timeout 3 ./build/cyrus-gateway --mode="${m}" 2>&1 || true)
    if echo "$output" | grep -q "\"mode\":\"${m}\""; then
        pass "--mode=${m} parsed and logged"
    else
        fail "--mode=${m} not recognized in log"
    fi
done

# ========================= 汇总 =========================
echo ""
echo "======================================================"
echo -e "  ${GRN}PASS=${PASS}${RST}  ${RED}FAIL=${FAIL}${RST}  ${YEL}SKIP=${SKIP}${RST}"
echo "======================================================"
echo ""

if [[ "$FAIL" -eq 0 ]]; then
    echo -e "${GRN}DONE: Week 4.1 并发模型准备 verify passed${RST}"
else
    echo -e "${RED}DONE: Week 4.1 verify finished with ${FAIL} failure(s)${RST}"
    exit 1
fi
