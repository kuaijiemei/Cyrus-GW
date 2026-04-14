#!/usr/bin/env bash
# Week 2.5 里程碑：一键可视化验收入口（聚合 2.1~2.4）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/gateway-cpp/scripts/verify_2_5.sh"
