#!/usr/bin/env bash
# Week 2.3 里程碑：一键可视化验收入口（超时与重试）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/gateway-cpp/scripts/verify_2_3.sh"
