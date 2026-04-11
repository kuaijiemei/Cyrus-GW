#!/usr/bin/env bash
# Week 1 里程碑：一键可视化验收（Mock LLM + Agent + Gateway，隔离端口）。
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/gateway-cpp/scripts/verify_1_4.sh"
