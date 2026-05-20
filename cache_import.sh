#!/bin/bash
# cache_import.sh — Import analysis results into KV Cache (C implementation)
#
# Usage:
#     ./cache_import.sh <analysis_dir> <namespace> [--cache-dir <dir>]
#
# Example:
#     ./cache_import.sh ./nginx_cache /code/nginx
#     ./cache_import.sh ./linux_subsystems/mm_cache /code/linux/mm --cache-dir ./ai_memory

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_IMPORT="${SCRIPT_DIR}/../tools/cache_import"

if [[ ! -f "$CACHE_IMPORT" ]]; then
    echo "ERROR: cache_import not found at $CACHE_IMPORT"
    echo "Please build it first: make tools/cache_import"
    exit 1
fi

exec "$CACHE_IMPORT" "$@"
