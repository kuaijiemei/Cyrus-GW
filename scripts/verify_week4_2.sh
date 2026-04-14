#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Week 4.2 验证脚本 —— 压测脚本与命令固化
# 使用方式：bash scripts/verify_week4_2.sh
# ---------------------------------------------------------------------------
set -uo pipefail

RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
CYN='\033[0;36m'
RST='\033[0m'
PASS=0
FAIL=0
SKIP=0

pass() { PASS=$((PASS + 1)); echo -e "  ${GRN}✔ PASS${RST}: $1"; }
fail() { FAIL=$((FAIL + 1)); echo -e "  ${RED}✘ FAIL${RST}: $1"; }
skip() { SKIP=$((SKIP + 1)); echo -e "  ${YEL}⊘ SKIP${RST}: $1"; }
info() { echo -e "  ${CYN}ℹ${RST}  $1"; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GW_DIR="${ROOT}/gateway-cpp"
GW_BIN="${GW_DIR}/build/cyrus-gateway"
BENCH_SH="${ROOT}/scripts/bench_week4_2.sh"
TTFT_PY="${ROOT}/scripts/stream_ttft_bench.py"
WRK_LUA="${ROOT}/scripts/wrk_chat_non_stream.lua"

PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do
        kill "$p" >/dev/null 2>&1 || true
    done
    for p in "${PIDS[@]}"; do
        wait "$p" 2>/dev/null || true
    done
    rm -f /tmp/mock_llm_w42_*.py /tmp/w42_*.log /tmp/w42_print_only.log 2>/dev/null || true
}
trap cleanup EXIT

wait_http() {
    local url="$1"
    local retries="${2:-20}"
    for ((i = 0; i < retries; i++)); do
        if curl -sS --connect-timeout 1 "${url}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

has_bench_tool=false
if command -v wrk >/dev/null 2>&1 || command -v hey >/dev/null 2>&1; then
    has_bench_tool=true
fi

echo -e "\n${CYN}[4.2-1] 脚本文件存在性检查${RST}"
if [[ -f "${BENCH_SH}" && -f "${TTFT_PY}" && -f "${WRK_LUA}" ]]; then
    pass "bench/ttft/wrk Lua 脚本均已存在"
else
    fail "缺少 4.2 关键脚本文件"
fi

echo -e "\n${CYN}[4.2-2] 命令固化参数检查（100/300/500 + 预热 + 时长）${RST}"
bash "${BENCH_SH}" --mode=epoll --print-only >/tmp/w42_print_only.log 2>&1
if [[ $? -ne 0 ]]; then
    if ${has_bench_tool}; then
        fail "print-only 执行失败，详见 /tmp/w42_print_only.log"
    else
        skip "当前环境未安装 wrk/hey，print-only 无法通过工具选择"
    fi
else
    out="$(python3 - <<'PY'
from pathlib import Path
txt = Path("/tmp/w42_print_only.log").read_text(encoding="utf-8", errors="ignore")
ok = all(k in txt for k in ["C=100", "C=300", "C=500", "预热时长(秒)   : 10", "采样时长(秒)   : 30"])
print("ok" if ok else "bad")
PY
)"
    if [[ "${out}" == "ok" ]]; then
        pass "并发档位与预热/采样时长已固化"
    else
        fail "固定参数检查未通过（请检查 print-only 输出）"
    fi
fi

echo -e "\n${CYN}[4.2-3] 启动 mock LLM + Agent + Gateway（epoll）${RST}"
if [[ ! -x "${GW_BIN}" ]]; then
    fail "缺少 ${GW_BIN}，请先执行 Week 4.1 编译步骤"
else
    MOCK_PY="$(mktemp /tmp/mock_llm_w42_XXXX.py)"
    cat >"${MOCK_PY}" <<'PYEOF'
from http.server import HTTPServer, BaseHTTPRequestHandler
import json

class H(BaseHTTPRequestHandler):
    def do_POST(self):
        cl = int(self.headers.get("Content-Length", 0))
        if cl:
            self.rfile.read(cl)
        body = json.dumps({"choices":[{"message":{"content":"week4.2 mock reply"}}],"model":"mock"})
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body.encode())

    def log_message(self, *_args):
        return

HTTPServer(("127.0.0.1", 19942), H).serve_forever()
PYEOF
    python3 "${MOCK_PY}" >/tmp/w42_mock.log 2>&1 &
    PIDS+=($!)

    if [[ ! -f "${ROOT}/agent-py/.venv/bin/python" ]]; then
        fail "agent-py/.venv 缺失，无法启动 Agent"
    else
        (
            cd "${ROOT}/agent-py"
            source .venv/bin/activate
            LLM_BASE_URL="http://127.0.0.1:19942" \
            LLM_API_KEY="test" \
            LLM_MODEL="mock" \
            LLM_TIMEOUT_MS=5000 \
            exec python3 -m uvicorn app.main:app --host 127.0.0.1 --port 8001 >/tmp/w42_agent.log 2>&1
        ) &
        PIDS+=($!)

        if wait_http "http://127.0.0.1:8001/health" 20; then
            (
                cd "${GW_DIR}"
                ./build/cyrus-gateway --mode=epoll >/tmp/w42_gateway.log 2>&1
            ) &
            PIDS+=($!)
            if wait_http "http://127.0.0.1:8080/health" 20; then
                pass "mock LLM + Agent + epoll Gateway 启动成功"
            else
                fail "Gateway 启动失败"
            fi
        else
            fail "Agent 启动失败"
        fi
    fi
fi

echo -e "\n${CYN}[4.2-4] 正常路径：流式 TTFT 命令可执行${RST}"
python3 "${TTFT_PY}" --url "http://127.0.0.1:8080/chat" --concurrency 1 --requests 1 --timeout-ms 15000 --request-id-prefix "verify-w42" >/tmp/w42_ttft.log 2>&1
if [[ $? -eq 0 ]]; then
    pass "TTFT 采样命令执行成功（正常路径）"
else
    fail "TTFT 采样命令执行失败，详见 /tmp/w42_ttft.log"
fi

echo -e "\n${CYN}[4.2-5] 异常路径：非法参数应立即失败${RST}"
bash "${BENCH_SH}" --mode=invalid --print-only >/tmp/w42_invalid.log 2>&1
if [[ $? -ne 0 ]]; then
    pass "非法 mode 参数可被拦截（异常路径）"
else
    fail "非法 mode 参数未被拦截"
fi

echo -e "\n${CYN}[4.2-6] 非流式压测工具可用性${RST}"
if ${has_bench_tool}; then
    pass "检测到 wrk/hey，满足 Week 4.2 工具选型执行条件"
else
    skip "当前环境未安装 wrk/hey；已通过脚本提供安装提示与命令固化"
fi

echo
echo "======================================================"
echo -e "  ${GRN}PASS=${PASS}${RST}  ${RED}FAIL=${FAIL}${RST}  ${YEL}SKIP=${SKIP}${RST}"
echo "======================================================"
echo

if [[ "${FAIL}" -eq 0 ]]; then
    echo -e "${GRN}DONE: Week 4.2 压测脚本与命令固化 verify passed${RST}"
else
    echo -e "${RED}DONE: Week 4.2 verify finished with ${FAIL} failure(s)${RST}"
    exit 1
fi
