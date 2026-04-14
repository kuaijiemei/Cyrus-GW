#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# 模块职责：Week 4.2 压测命令固化入口。
# 对外暴露：
#   1) 固定压测参数（并发档位、预热时长、采样时长）；
#   2) 非流式压测命令（wrk/hey）；
#   3) 流式 TTFT 命令（python 脚本）；
#   4) 一键打印命令或直接执行压测。
# 注意：该脚本只负责“命令固化与执行”，不做 4.3 的结果结论整理。
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WRK_LUA="${ROOT}/scripts/wrk_chat_non_stream.lua"
TTFT_PY="${ROOT}/scripts/stream_ttft_bench.py"

RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[0;33m'
CYN='\033[0;36m'
RST='\033[0m'

log_info() { echo -e "${CYN}[信息]${RST} $1"; }
log_warn() { echo -e "${YEL}[警告]${RST} $1"; }
log_ok() { echo -e "${GRN}[通过]${RST} $1"; }
log_err() { echo -e "${RED}[失败]${RST} $1"; }

MODE="epoll"
HOST="127.0.0.1"
PORT="8080"
TOOL="auto"                  # auto / wrk / hey
WARMUP_SEC="10"
DURATION_SEC="30"
LEVELS_CSV="100,300,500"
STREAM_REQUESTS_MULTIPLIER="2"
RUN_TARGET="all"             # all / non-stream / stream
PRINT_ONLY=false

usage() {
    cat <<'EOF'
用法：
  bash scripts/bench_week4_2.sh [参数]

参数：
  --mode <iouring|epoll|blocking>   仅用于提示当前目标模式（默认 epoll）
  --host <ip>                        压测目标主机（默认 127.0.0.1）
  --port <port>                      压测目标端口（默认 8080）
  --tool <auto|wrk|hey>              非流式压测工具（默认 auto，优先 wrk）
  --warmup-sec <n>                   预热时长秒（默认 10）
  --duration-sec <n>                 正式采样时长秒（默认 30）
  --levels <csv>                     并发档位（默认 100,300,500）
  --run <all|non-stream|stream>      执行全部/仅非流式/仅流式（默认 all）
  --print-only                       仅打印固化命令，不实际执行
  --help                             查看帮助

示例：
  bash scripts/bench_week4_2.sh --mode=epoll --print-only
  bash scripts/bench_week4_2.sh --mode=iouring --tool=wrk
EOF
}

for arg in "$@"; do
    case "$arg" in
        --mode=*) MODE="${arg#*=}" ;;
        --host=*) HOST="${arg#*=}" ;;
        --port=*) PORT="${arg#*=}" ;;
        --tool=*) TOOL="${arg#*=}" ;;
        --warmup-sec=*) WARMUP_SEC="${arg#*=}" ;;
        --duration-sec=*) DURATION_SEC="${arg#*=}" ;;
        --levels=*) LEVELS_CSV="${arg#*=}" ;;
        --run=*) RUN_TARGET="${arg#*=}" ;;
        --print-only) PRINT_ONLY=true ;;
        --help) usage; exit 0 ;;
        *)
            log_err "未知参数：${arg}"
            usage
            exit 1
            ;;
    esac
done

case "${MODE}" in
    iouring|epoll|blocking) ;;
    *)
        log_err "--mode 仅支持 iouring/epoll/blocking"
        exit 1
        ;;
esac

case "${RUN_TARGET}" in
    all|non-stream|stream) ;;
    *)
        log_err "--run 仅支持 all/non-stream/stream"
        exit 1
        ;;
esac

GW_DIR="${ROOT}/gateway-cpp"
GW_BIN="${GW_DIR}/build/cyrus-gateway"
MOCK_LLM_PORT="19943"
AGENT_PORT="8001"
BENCH_PIDS=()

bench_cleanup() {
    for p in "${BENCH_PIDS[@]}"; do
        kill "$p" >/dev/null 2>&1 || true
    done
    for p in "${BENCH_PIDS[@]}"; do
        wait "$p" 2>/dev/null || true
    done
    rm -f /tmp/bench_mock_llm_*.py 2>/dev/null || true
}
trap bench_cleanup EXIT

wait_http() {
    local url="$1" max="${2:-30}"
    for ((i = 0; i < max; i++)); do
        if curl -sS --connect-timeout 1 "${url}" >/dev/null 2>&1; then return 0; fi
        sleep 0.5
    done
    return 1
}

ensure_services() {
    if curl -sS --connect-timeout 1 "http://${HOST}:${PORT}/health" >/dev/null 2>&1; then
        if curl -sS --connect-timeout 1 "http://127.0.0.1:${AGENT_PORT}/health" >/dev/null 2>&1; then
            log_ok "Gateway(:${PORT}) + Agent(:${AGENT_PORT}) 已在运行，跳过自动拉起"
            return 0
        fi
    fi

    log_info "自动拉起 mock LLM + Agent + Gateway（${MODE}）..."

    # mock LLM
    local mock_py
    mock_py="$(mktemp /tmp/bench_mock_llm_XXXX.py)"
    # 易踩坑：单线程 HTTPServer 在 100+ 并发下成为瓶颈，导致 wrk 全部超时。
    # 必须使用 ThreadingHTTPServer 使 mock 能并行处理请求。
    cat >"${mock_py}" <<'PYEOF'
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
import json, sys
class H(BaseHTTPRequestHandler):
    def do_POST(self):
        cl = int(self.headers.get("Content-Length", 0))
        if cl: self.rfile.read(cl)
        body = json.dumps({"choices":[{"message":{"content":"bench mock reply"}}],"model":"mock"})
        self.send_response(200)
        self.send_header("Content-Type","application/json")
        self.send_header("Content-Length",str(len(body)))
        self.end_headers()
        self.wfile.write(body.encode())
    def log_message(self, *a): pass
class ThreadedHTTPServer(ThreadingMixIn, HTTPServer): daemon_threads = True
ThreadedHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
PYEOF
    python3 "${mock_py}" "${MOCK_LLM_PORT}" >/dev/null 2>&1 &
    BENCH_PIDS+=($!)

    # Agent
    if [[ ! -f "${ROOT}/agent-py/.venv/bin/python" ]]; then
        log_err "agent-py/.venv 缺失，无法启动 Agent"
        exit 1
    fi
    (
        cd "${ROOT}/agent-py"
        source .venv/bin/activate
        LLM_BASE_URL="http://127.0.0.1:${MOCK_LLM_PORT}" \
        LLM_API_KEY="bench" \
        LLM_MODEL="mock" \
        LLM_TIMEOUT_MS=5000 \
        exec python3 -m uvicorn app.main:app --host 127.0.0.1 --port "${AGENT_PORT}" --log-level warning >/dev/null 2>&1
    ) &
    BENCH_PIDS+=($!)

    if ! wait_http "http://127.0.0.1:${AGENT_PORT}/health" 20; then
        log_err "Agent 启动失败"
        exit 1
    fi
    log_ok "mock LLM(:${MOCK_LLM_PORT}) + Agent(:${AGENT_PORT}) 已就绪"

    # Gateway
    if [[ ! -x "${GW_BIN}" ]]; then
        log_err "缺少 ${GW_BIN}，请先编译 Gateway"
        exit 1
    fi
    (
        cd "${GW_DIR}"
        exec ./build/cyrus-gateway --mode="${MODE}" >/dev/null 2>&1
    ) &
    BENCH_PIDS+=($!)

    if ! wait_http "http://${HOST}:${PORT}/health" 20; then
        log_err "Gateway 启动失败"
        exit 1
    fi
    log_ok "Gateway(:${PORT}, mode=${MODE}) 已就绪"
}

resolve_tool() {
    if [[ "${TOOL}" == "wrk" || "${TOOL}" == "hey" ]]; then
        echo "${TOOL}"
        return 0
    fi
    # 决策原因：优先选择 wrk（吞吐压测更常用），缺失时降级 hey，保证最小环境可执行。
    if command -v wrk >/dev/null 2>&1; then
        echo "wrk"
    elif command -v hey >/dev/null 2>&1; then
        echo "hey"
    else
        echo "none"
    fi
}

split_levels() {
    IFS=',' read -r -a LEVELS <<< "${LEVELS_CSV}"
}

build_non_stream_cmd() {
    local selected_tool="$1"
    local concurrency="$2"
    local seconds="$3"
    local url="http://${HOST}:${PORT}/chat"

    if [[ "${selected_tool}" == "wrk" ]]; then
        # 易踩坑：wrk Lua 脚本中 body 已固定 JSON，避免 shell 层 JSON 转义错误。
        echo "wrk -t4 -c${concurrency} -d${seconds}s --latency --timeout 10s -s \"${WRK_LUA}\" \"${url}\""
    else
        # 易踩坑：通过 --disable-compression 降低不同主机压缩差异，便于复现实验。
        echo "hey -m POST -H 'Content-Type: application/json' --disable-compression -d '{\"message\":\"bench non-stream\",\"stream\":false}' -c ${concurrency} -z ${seconds}s \"${url}\""
    fi
}

build_stream_cmd() {
    local concurrency="$1"
    local requests="$2"
    local url="http://${HOST}:${PORT}/chat"
    # 易踩坑：显式传 request_id 前缀，便于在网关/Agent 日志里串联 TTFT 样本。
    echo "python3 \"${TTFT_PY}\" --url \"${url}\" --concurrency ${concurrency} --requests ${requests} --timeout-ms 30000 --request-id-prefix ttft-${MODE}"
}

run_cmd() {
    local cmd="$1"
    log_info "执行：${cmd}"
    eval "${cmd}" || log_warn "命令返回非零（压测中部分失败属正常现象）"
}

main() {
    split_levels
    local selected_tool
    selected_tool="$(resolve_tool)"

    echo "======================================================"
    echo "Week 4.2 压测命令固化"
    echo "目标模式       : ${MODE}"
    echo "目标地址       : http://${HOST}:${PORT}"
    echo "非流式工具     : ${selected_tool}"
    echo "并发档位       : ${LEVELS_CSV}"
    echo "预热时长(秒)   : ${WARMUP_SEC}"
    echo "采样时长(秒)   : ${DURATION_SEC}"
    echo "======================================================"

    if [[ "${selected_tool}" == "none" ]]; then
        log_err "未检测到 wrk/hey，请先安装其中一个压测工具。"
        exit 1
    fi
    log_ok "压测工具选择完成（${selected_tool}）"

    echo
    echo "[固化命令] 非流式压测"
    for c in "${LEVELS[@]}"; do
        echo "  C=${c}: $(build_non_stream_cmd "${selected_tool}" "${c}" "${DURATION_SEC}")"
    done

    echo
    echo "[固化命令] 流式 TTFT 采样"
    for c in "${LEVELS[@]}"; do
        local_requests=$((c * STREAM_REQUESTS_MULTIPLIER))
        echo "  C=${c}: $(build_stream_cmd "${c}" "${local_requests}")"
    done

    if ${PRINT_ONLY}; then
        log_ok "print-only 模式：已输出全部固化命令。"
        exit 0
    fi

    echo
    ensure_services

    if [[ "${RUN_TARGET}" == "all" || "${RUN_TARGET}" == "non-stream" ]]; then
        echo
        echo "[执行阶段] 非流式压测（含预热）"
        for c in "${LEVELS[@]}"; do
            run_cmd "$(build_non_stream_cmd "${selected_tool}" "${c}" "${WARMUP_SEC}")"
            run_cmd "$(build_non_stream_cmd "${selected_tool}" "${c}" "${DURATION_SEC}")"
        done
    fi

    if [[ "${RUN_TARGET}" == "all" || "${RUN_TARGET}" == "stream" ]]; then
        echo
        echo "[执行阶段] 流式 TTFT 采样（含预热）"
        for c in "${LEVELS[@]}"; do
            local_warmup_requests=$((c))
            local_run_requests=$((c * STREAM_REQUESTS_MULTIPLIER))
            run_cmd "$(build_stream_cmd "${c}" "${local_warmup_requests}")"
            run_cmd "$(build_stream_cmd "${c}" "${local_run_requests}")"
        done
    fi

    log_ok "Week 4.2 压测命令执行完成。"
}

main "$@"
