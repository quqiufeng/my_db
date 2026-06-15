#!/bin/bash
#
# =============================================================================
# analyze_nodejs_repo.sh — Node.js / TypeScript 项目一键分析包装脚本
# =============================================================================
#
# 设计目标：
#   为 Node.js / TypeScript 项目（React/Vue/SolidJS/Bun/Effect-TS 等）提供
#   零记忆成本的 analyze_repo.sh 调用方式，自动启用 TypeScript AST 插件，
#   并过滤常见的构建产物目录。
#
# 依赖：
#   - /opt/my_db/analyze_repo.sh
#   - /opt/my_db/plugins/typescript-indexer/bin/typescript-indexer
#
# 用法：
#   ./analyze_nodejs_repo.sh <source> [namespace] [options]
#
# 示例：
#   ./analyze_nodejs_repo.sh /opt/opencode
#   ./analyze_nodejs_repo.sh /opt/opencode /code/opencode
#   ./analyze_nodejs_repo.sh https://github.com/owner/repo --jobs 4
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLUGIN_DIR="${SCRIPT_DIR}/plugins/typescript-indexer"
PLUGIN_BIN="${PLUGIN_DIR}/bin/typescript-indexer"
PLUGIN_JSON="${PLUGIN_DIR}/plugin.json"
ANALYZE_REPO="${SCRIPT_DIR}/analyze_repo.sh"

# 默认排除目录：node_modules 和常见构建产物
DEFAULT_EXCLUDES="node_modules,dist,build,out,.next,.nuxt,.output,coverage"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log()  { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error(){ echo -e "${RED}[ERROR]${NC} $1" >&2; }
ok()   { echo -e "${GREEN}[OK]${NC} $1"; }

usage() {
    echo "Usage: $0 <source> [namespace] [options]"
    echo ""
    echo "Arguments:"
    echo "  source     GitHub URL or local path (required)"
    echo "  namespace  Target namespace (default: auto-detect)"
    echo ""
    echo "Options:"
    echo "  --jobs <n>         Parallel workers (default: CPU cores)"
    echo "  --name <name>      Project name"
    echo "  --cache-dir <dir>  KV Cache directory (default: /memory)"
    echo "  --skip-vectors     Skip vector generation"
    echo "  --skip-callgraph   Skip call graph analysis"
    echo "  --skip-dataflow    Skip dataflow analysis"
    echo "  --exclude-dir <dirs>  Extra dirs to skip (comma-separated)"
    echo "  -h, --help         Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 /opt/opencode"
    echo "  $0 /opt/opencode /code/opencode"
    echo "  $0 https://github.com/owner/repo --jobs 4"
    exit 0
}

# 解析参数
SOURCE=""
NAMESPACE=""
EXTRA_ARGS=()
EXTRA_EXCLUDES=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            ;;
        --jobs|--name|--cache-dir)
            EXTRA_ARGS+=("$1" "$2")
            shift 2
            ;;
        --skip-vectors|--skip-callgraph|--skip-dataflow)
            EXTRA_ARGS+=("$1")
            shift
            ;;
        --exclude-dir)
            EXTRA_EXCLUDES="$2"
            shift 2
            ;;
        -*)
            error "Unknown option: $1"
            exit 1
            ;;
        *)
            if [[ -z "$SOURCE" ]]; then
                SOURCE="$1"
            elif [[ -z "$NAMESPACE" ]]; then
                NAMESPACE="$1"
            else
                error "Too many positional arguments"
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "$SOURCE" ]]; then
    error "No source specified"
    usage
fi

# 合并排除目录
EXCLUDES="$DEFAULT_EXCLUDES"
if [[ -n "$EXTRA_EXCLUDES" ]]; then
    EXCLUDES="${EXCLUDES},${EXTRA_EXCLUDES}"
fi

# 确保 analyze_repo.sh 存在
if [[ ! -x "$ANALYZE_REPO" ]]; then
    error "analyze_repo.sh not found or not executable: $ANALYZE_REPO"
    exit 1
fi

# 确保插件已编译
if [[ ! -x "$PLUGIN_BIN" ]]; then
    log "TypeScript indexer plugin not built yet. Building..."
    if [[ -f "${PLUGIN_DIR}/Makefile" ]]; then
        (cd "$PLUGIN_DIR" && make) || {
            error "Failed to build TypeScript indexer plugin"
            exit 1
        }
    else
        error "Plugin Makefile not found: ${PLUGIN_DIR}/Makefile"
        exit 1
    fi
fi

# 验证插件 JSON
if [[ ! -f "$PLUGIN_JSON" ]]; then
    error "Plugin registry not found: $PLUGIN_JSON"
    exit 1
fi

log "================================================================"
log "Node.js / TypeScript analysis wrapper"
log "Source: $SOURCE"
log "Namespace: ${NAMESPACE:-auto-detect}"
log "Plugin: $PLUGIN_JSON"
log "Excludes: $EXCLUDES"
log "================================================================"

# 构建调用参数
CMD=("$ANALYZE_REPO" "$SOURCE")
[[ -n "$NAMESPACE" ]] && CMD+=("$NAMESPACE")

# 自动设置项目名（如果未提供）
if [[ ! " ${EXTRA_ARGS[*]} " =~ " --name " ]]; then
    if [[ "$SOURCE" =~ ^https?://github.com/([^/]+)/([^/]+) ]]; then
        CMD+=("--name" "${BASH_REMATCH[2]}")
    elif [[ -d "$SOURCE" ]]; then
        CMD+=("--name" "$(basename "$SOURCE")")
    fi
fi

CMD+=("--plugins" "$PLUGIN_JSON")
CMD+=("--exclude-dir" "$EXCLUDES")
CMD+=("${EXTRA_ARGS[@]}")

log "Running: ${CMD[*]}"
"${CMD[@]}"
