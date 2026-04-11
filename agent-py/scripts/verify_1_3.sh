#!/usr/bin/env bash
# TODO 1.3 可视化验收：需先启动 uvicorn app.main:app --port 8001
set -euo pipefail
BASE="${BASE:-http://127.0.0.1:8001}"

echo "========== 1.3 Agent /health（期望 HTTP 200）=========="
curl -sS -w '\n' -D - "${BASE}/health" | sed -n '1,20p'

echo ""
echo "========== 1.3 Agent /agent/chat（无上游，期望可读错误）=========="
curl -sS -w '\n' -D - -X POST "${BASE}/agent/chat" \
  -H "Content-Type: application/json" \
  -d '{"request_id":"req-demo-1","message":"hello from verify","stream":false}' \
  | sed -n '1,40p'

echo ""
echo "========== 1.3 参数校验（message 为空，期望 422）=========="
curl -sS -w '\n' -D - -X POST "${BASE}/agent/chat" \
  -H "Content-Type: application/json" \
  -d '{"request_id":"req-demo-2","message":"   ","stream":false}' \
  | sed -n '1,40p'

echo ""
echo "提示：若你已配置 LLM_BASE_URL + LLM_API_KEY，再跑一次上面的 /agent/chat 应返回 200。"
