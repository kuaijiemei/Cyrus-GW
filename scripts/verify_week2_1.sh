#!/usr/bin/env bash
# Week 2.1 里程碑：一键可视化验收入口（SSE 正常/总超时/非流式回归）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/gateway-cpp/scripts/verify_2_1.sh"
