#!/bin/bash
# code_to_memory.sh — Bridge: Import analysis results into KV Cache
# 
# Usage:
#     ./code_to_memory.sh <analysis_dir> [namespace] [--cache-dir <dir>]
# 
# Examples:
#     ./code_to_memory.sh ./linux_subsystems/mm_cache /code/linux/mm
#     ./code_to_memory.sh ./nginx_cache /code/nginx
#
# This script bridges the analysis pipeline output into the KV Cache memory system.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_SCRIPT="${SCRIPT_DIR}/code_to_memory.py"

# Check Python script exists
if [[ ! -f "$PYTHON_SCRIPT" ]]; then
    echo "ERROR: code_to_memory.py not found at $PYTHON_SCRIPT"
    exit 1
fi

# Run Python script with all arguments
exec python3 "$PYTHON_SCRIPT" "$@"
