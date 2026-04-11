#!/usr/bin/env bash
# TODO 1.2 可视化验收：需已启动 ./build/cyrus-gateway（工作目录为 gateway-cpp）。
set -euo pipefail
BASE="${BASE:-http://127.0.0.1:8080}"

echo "========== 1.2 POST /chat（期望 HTTP 200 + JSON）=========="
curl -sS -w '\n' -D - -X POST "${BASE}/chat" \
  -H "Content-Type: application/json" \
  -d '{"message":"hello","stream":false}' \
  | sed -n '1,20p'

echo ""
echo "========== 1.2 参数错误：空 message（期望 HTTP 400）=========="
curl -sS -w '\n' -D - -X POST "${BASE}/chat" \
  -H "Content-Type: application/json" \
  -d '{"message":"","stream":false}' \
  | sed -n '1,25p'

echo ""
echo "========== 1.2 参数错误：缺 message（期望 HTTP 400）=========="
curl -sS -w '\n' -D - -X POST "${BASE}/chat" \
  -H "Content-Type: application/json" \
  -d '{"stream":false}' \
  | sed -n '1,25p'

echo ""
echo "========== request_id 透传：X-Request-Id + body.request_id（期望响应 request_id 为 body 值）=========="
curl -sS -w '\n' -X POST "${BASE}/chat" \
  -H "Content-Type: application/json" \
  -H "X-Request-Id: from-header-should-be-overridden" \
  -d '{"request_id":"client-req-001","message":"ping","stream":false}'

echo ""
echo "========== GET /health（骨架保留）=========="
curl -sS -w '\n' -D - "${BASE}/health" | sed -n '1,15p'

echo ""
echo "========== 网关 stderr 应出现含 request_id / latency_ms / status_code / tool_used 的访问日志行 =========="
echo "(若在本机前台运行 cyrus-gateway，请查看启动该进程的终端输出)"
