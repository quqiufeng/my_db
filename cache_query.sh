#!/bin/bash
# cache_query.sh — AI-friendly structured query interface for KV Cache (C implementation)
#
# Usage:
#     ./cache_query.sh <query> [--repo <namespace>] [--type <type>]
#
# Example:
#     ./cache_query.sh ngx_palloc --repo /code/nginx --type context
#     ./cache_query.sh "memory pool" --repo /code/nginx --type search

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_QUERY="${SCRIPT_DIR}/../tools/cache_query"

if [[ ! -f "$CACHE_QUERY" ]]; then
    echo "ERROR: cache_query not found at $CACHE_QUERY"
    echo "Please build it first: make tools/cache_query"
    exit 1
fi

exec "$CACHE_QUERY" "$@"
