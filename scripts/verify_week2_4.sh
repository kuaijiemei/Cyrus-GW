#!/usr/bin/env bash
# Week 2.4 里程碑：一键可视化验收入口（错误处理与状态码统一）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/gateway-cpp/scripts/verify_2_4.sh"
