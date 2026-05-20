#!/bin/bash
# agent_query.sh — AI-friendly structured query interface for KV Cache
#
# Usage:
#     ./agent_query.sh <query> [--repo <namespace>] [--type <type>]
#
# Examples:
#     ./agent_query.sh /code/nginx/symbols/ngx_palloc
#     ./agent_query.sh ngx_palloc --repo /code/nginx --type context
#     ./agent_query.sh "memory pool" --repo /code/nginx --type search

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_SCRIPT="${SCRIPT_DIR}/agent_query.py"

if [[ ! -f "$PYTHON_SCRIPT" ]]; then
    echo "ERROR: agent_query.py not found at $PYTHON_SCRIPT"
    exit 1
fi

# Run Python script and filter out cache internal log messages
python3 "$PYTHON_SCRIPT" "$@" 2>/dev/null | grep -v '^\[CACHE\]'
