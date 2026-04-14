#!/usr/bin/env bash
# Week 2.2 里程碑：一键可视化验收入口（Token Bucket 限流）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/gateway-cpp/scripts/verify_2_2.sh"
