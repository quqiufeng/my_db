#!/bin/bash
#
# =============================================================================
# explore_repo.sh — AI Agent 代码库探索脚本（通用版）
# =============================================================================
#
# 设计目标：
#   这个脚本是专门为 AI Agent 设计的代码探索工具。之前的探索流程需要手动设置
#   LD_LIBRARY_PATH 环境变量、记忆复杂的 cache_query 参数、处理 JSON 输出，
#   这个脚本将所有这些封装起来，提供标准化的探索接口。
#
# 核心特性：
#   1. 自动设置 GPU 环境变量（TensorRT + cuDNN + CUDA），无需手动 export
#   2. 不依赖任何硬编码的项目知识，完全基于语义搜索和符号查询
#   3. 支持任意 C/C++ 项目（Redis、Nginx、PostgreSQL、Linux 内核等）
#   4. 输出格式化为人类/AI 可读的格式，无需手动解析 JSON
#
# 前置条件：
#   1. 项目已导入 KV Cache 记忆系统（通过 ./analyze_repo.sh）
#   2. 工具已编译：make tools/cache_query
#   3. GPU 驱动已安装（语义搜索需要）
#
# =============================================================================
# 快速开始（AI 使用指南）
# =============================================================================
#
# 基本用法：
#   ./explore_repo.sh <namespace> <command> [options]
#
# namespace 是什么？
#   这是项目在 KV Cache 中的命名空间路径。导入时由 analyze_repo.sh 自动生成：
#   - GitHub 项目：https://github.com/redis/redis → /code/redis/redis
#   - 本地项目：/opt/redis → /code/local/redis
#   - 查看当前有哪些项目：ls /opt/ai_code_memory/ 或直接查询 /code 前缀
#
# =============================================================================
# 命令详解
# =============================================================================
#
# 1. overview — 项目概览
#    查看项目的基本统计信息：总条目数、导入时间、chunks 数量等
#    用法：./explore_repo.sh /code/local/redis overview
#    适用：第一次接触新项目，了解规模
#
# 2. symbol <name> — 符号深度分析（核心利器）
#    查询任意函数/结构体的完整上下文，包括 caller/callee/call_paths
#    用法：./explore_repo.sh /code/local/redis symbol zmalloc --depth 2
#    适用：已知函数名时，直接深度分析其实现和调用关系
#    特点：最稳定可靠的命令，返回结构化 caller/callee/call_paths
#    参数：--depth N  展开调用链深度（默认1，最大5）
#    提示：如果已知函数名，优先用 symbol 而非 search，结果更精准
#
# 3. search "<query>" — 语义搜索
#    用自然语言搜索代码（如 "memory allocation"、"event loop handler"）
#    用法：./explore_repo.sh /code/local/redis search "event loop" --max 10
#    适用：不知道具体函数名，只知道功能描述时探索未知代码库
#    参数：--max N  返回结果数量（默认10）
#    提示：适合探索阶段，从搜索结果中发现关键函数名，然后用 symbol 深入分析
#
# 4. explore "<query>" — 子系统探索（推荐）
#    语义搜索 + 自动深度分析 Top 结果，一站式了解某个子系统
#    用法：./explore_repo.sh /code/local/redis explore "memory allocation"
#    适用：快速了解某个子系统的全貌（如内存管理、网络 I/O、配置解析）
#    特点：自动调用语义搜索，然后对 Top-1 结果做深度符号分析
#
# 5. compare <ns2> "<query>" — 跨项目对比
#    对比两个项目对同一主题的实现差异
#    用法：./explore_repo.sh /code/local/redis compare /code/nginx "memory pool"
#    适用：学习不同项目如何解决同一问题（如 Redis zmalloc vs Nginx ngx_palloc）
#
# 6. report — 生成架构报告
#    输出 JSON 格式的完整项目分析报告
#    用法：./explore_repo.sh /code/local/redis report > report.json
#    适用：生成可持久化的分析结果，供后续参考
#
# 7. top [N] — 热点函数排行
#    查看被调用次数最多的 N 个函数（基于调用图统计）
#    用法：./explore_repo.sh /code/local/redis top 20
#    适用：识别项目的核心基础设施函数
#
# =============================================================================
# 典型工作流（AI 探索新项目）
# =============================================================================
#
# Step 1: 了解项目规模
#   ./explore_repo.sh /code/local/redis overview
#
# Step 2: 探索核心子系统（选择感兴趣的主题）
#   ./explore_repo.sh /code/local/redis explore "memory management"
#   ./explore_repo.sh /code/local/redis explore "event loop"
#   ./explore_repo.sh /code/local/redis explore "network connection"
#
# Step 3: 深入关键函数（从 explore 结果中选择一个函数）
#   ./explore_repo.sh /code/local/redis symbol zmalloc --depth 2
#
# Step 4: 跨项目对比（如果已有其他项目的记忆）
#   ./explore_repo.sh /code/local/redis compare /code/nginx "memory pool"
#
# Step 5: 生成报告
#   ./explore_repo.sh /code/local/redis report > redis_analysis.json
#
# =============================================================================
# 使用示例
# =============================================================================
#
# 示例 1: 探索 Redis 内存管理
#   ./explore_repo.sh /code/local/redis explore "memory allocation"
#   → 返回：zmalloc/zfree 的语义搜索结果 + 深度调用链分析
#
# 示例 2: 探索 Nginx 事件循环
#   ./explore_repo.sh /code/local/nginx explore "event loop"
#   → 返回：ngx_process_events_and_timers 的调用关系
#
# 示例 3: 对比两个项目的内存管理
#   ./explore_repo.sh /code/local/redis compare /code/nginx "memory pool"
#   → 返回：Redis zmalloc vs Nginx ngx_palloc 的对比
#
# 示例 4: 查找配置解析相关代码
#   ./explore_repo.sh /code/local/redis explore "configuration parser"
#   → 返回：配置解析相关的函数列表和实现
#
# 示例 5: 查看项目热点函数
#   ./explore_repo.sh /code/local/redis top 10
#   → 返回：被调用次数最多的 10 个函数
#
# =============================================================================
# 注意事项
# =============================================================================
#
# 1. 命名空间获取：
#    如果不确定 namespace，可以用以下命令查看已导入的项目：
#    strings /opt/ai_code_memory/cache.bin | grep "^/code/" | sort -u | head -20
#
# 2. 向量缓存：
#    explore/search/compare 命令需要向量缓存（/opt/code_caches/{project}_cache/vectors/）
#    如果缺失，脚本会提示：需要先运行 ./analyze_repo.sh 导入项目
#
# 3. GPU 环境：
#    脚本内部自动设置 LD_LIBRARY_PATH，无需手动 export
#    如果语义搜索报错 "libnvinfer.so.10 not found"，检查 TensorRT 安装
#
# 4. 完全通用：
#    这个脚本对 Redis、Nginx、PostgreSQL、Linux 内核、SQLite 等都有效
#    不需要修改脚本，只需更换 namespace 和 query
#
# =============================================================================

set -euo pipefail

# 脚本目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 工具路径
CACHE_QUERY="${SCRIPT_DIR}/tools/cache_query"

# GPU 环境（兼容本地和远程）
GPU_LIBS=""
[ -d "${SCRIPT_DIR}" ] && GPU_LIBS="${GPU_LIBS}:${SCRIPT_DIR}"
[ -d "/opt/TensorRT-10/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
[ -d "/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib"
[ -d "/opt/cuda/lib64" ] && GPU_LIBS="${GPU_LIBS}:/opt/cuda/lib64"
[ -d "/home/dministrator/anaconda3/envs/dl/lib" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib"
[ -d "/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/tensorrt_libs" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/tensorrt_libs"
[ -d "/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/nvidia/cudnn/lib" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/nvidia/cudnn/lib"
GPU_LIBS="${GPU_LIBS#:}"
export LD_LIBRARY_PATH="${GPU_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${BLUE}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }
ok() { echo -e "${GREEN}[OK]${NC} $1"; }

# 检查工具
check_tool() {
    if [[ ! -f "$CACHE_QUERY" ]]; then
        error "cache_query not found at $CACHE_QUERY"
        error "Please build: make tools/cache_query"
        exit 1
    fi
}

# =============================================================================
# JSON 提取辅助函数
# =============================================================================
# cache_query 的输出包含日志信息（如 [CACHE]、TensorRT 加载信息等），
# 这些日志会干扰 JSON 解析。此函数从混合输出中提取纯净的 JSON。
# 原理：找到第一个 '{' 和最后一个 '}'，提取中间的内容。
# =============================================================================
extract_json() {
    python3 -c '
import sys
content = sys.stdin.read()
start = content.find("{")
end = content.rfind("}")
if start != -1 and end != -1 and end > start:
    print(content[start:end+1])
else:
    print("{}")
'
}

# 解析参数
NAMESPACE=""
COMMAND=""

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <namespace> <command> [options]"
    echo ""
    echo "Commands:"
    echo "  overview                          项目概览"
    echo "  symbol <name> [--depth N]        符号上下文查询"
    echo "  search \"<query>\" [--max N]       语义搜索"
    echo "  explore \"<query>\"                 子系统探索（语义搜索+符号分析）"
    echo "  compare <ns2> \"<query>\"          跨项目对比"
    echo "  report                            生成 JSON 架构报告"
    echo "  top [N]                           热点函数排行（默认10）"
    echo ""
    echo "Examples:"
    echo "  $0 /code/local/redis overview"
    echo "  $0 /code/local/redis symbol zmalloc --depth 2"
    echo "  $0 /code/local/redis search \"event loop\""
    echo "  $0 /code/local/redis explore \"memory allocation\""
    echo "  $0 /code/local/redis compare /code/nginx \"memory pool\""
    echo "  $0 /code/local/redis report > report.json"
    exit 1
fi

NAMESPACE="$1"
COMMAND="$2"
shift 2

# 提取额外参数（如 --depth 2）
EXTRA_ARGS="$@"

# =============================================================================
# 命令实现
# =============================================================================

cmd_overview() {
    log "Project Overview: $NAMESPACE"
    echo ""
    
    local meta_key="${NAMESPACE}/_meta/info"
    "$CACHE_QUERY" "$meta_key" --type exact --pretty 2>&1 | extract_json
    
    echo ""
    log "To explore further, try:"
    echo "  $0 $NAMESPACE explore \"memory allocation\""
    echo "  $0 $NAMESPACE explore \"event loop\""
    echo "  $0 $NAMESPACE top 10"
}

cmd_symbol() {
    local symbol="${1:-}"
    if [[ -z "$symbol" ]]; then
        error "Usage: $0 $NAMESPACE symbol <name> [--depth N]"
        exit 1
    fi
    
    local depth=1
    if [[ "$EXTRA_ARGS" == *"--depth"* ]]; then
        # Extract depth value from arguments (e.g., "--depth 2")
        depth=$(echo "$EXTRA_ARGS" | sed -n 's/.*--depth *\([0-9]*\).*/\1/p')
        [[ -z "$depth" ]] && depth=1
    fi
    
    log "Exploring symbol: $symbol"
    echo ""
    
    "$CACHE_QUERY" "$symbol" --repo "$NAMESPACE" --type context --depth "$depth" --pretty 2>&1 | extract_json | python3 -c "
import sys, json
data = json.load(sys.stdin)
ctx = data.get('context', {})
sym = ctx.get('symbol', {})

if sym:
    print(f'=== Symbol: {sym.get(\"name\", \"unknown\")} ===')
    print(f'File: {sym.get(\"file\", \"N/A\")}:{sym.get(\"line\", \"N/A\")}')
    print(f'Kind: {sym.get(\"kind\", \"N/A\")}')
    print('')

callers = ctx.get('callers', [])
if callers:
    print(f'Callers ({len(callers)}):')
    for c in callers[:20]:
        print(f'  {c[\"name\"]} ({c[\"file\"]}:{c[\"line\"]})')
    if len(callers) > 20:
        print(f'  ... and {len(callers) - 20} more')
    print('')

callees = ctx.get('callees', [])
if callees:
    print(f'Callees ({len(callees)}):')
    for c in callees[:10]:
        print(f'  {c[\"name\"]}')
    print('')

paths = ctx.get('call_paths', [])
if paths:
    print(f'Call paths ({len(paths)}):')
    for p in paths[:8]:
        print('  -> '.join(p))
"
}

cmd_search() {
    local query="${1:-}"
    if [[ -z "$query" ]]; then
        error "Usage: $0 $NAMESPACE search \"<query>\""
        exit 1
    fi
    
    local max_results=10
    if [[ "$EXTRA_ARGS" == *"--max"* ]]; then
        max_results=$(echo "$EXTRA_ARGS" | sed -n 's/.*--max *\([0-9]*\).*/\1/p')
        [[ -z "$max_results" ]] && max_results=10
    fi
    
    log "Semantic search: \"$query\""
    echo ""
    
    local project_name=$(basename "$NAMESPACE")
    local cache_dir="/opt/code_caches/${project_name}_cache"
    
    if [[ ! -d "$cache_dir/vectors" ]]; then
        warn "Vector cache not found at $cache_dir/vectors"
        warn "Semantic search requires vectors. Run: ./analyze_repo.sh \u003csource\u003e"
        exit 1
    fi
    
    "$CACHE_QUERY" "$query" --repo "$NAMESPACE" --type search --analysis-dir "$cache_dir" --max-results "$max_results" --pretty 2>&1 | extract_json | python3 -c "
import sys, json
data = json.load(sys.stdin)
results = data.get('results', [])

print(f'Found {len(results)} results:\n')
for i, r in enumerate(results, 1):
    print(f'{i}. {r[\"name\"]} [{r.get(\"kind\", \"unknown\")}]')
    print(f'   File: {r[\"file\"]}:{r.get(\"line_start\", \"\")}')
    print(f'   Score: {r.get(\"score\", 0):.3f}')
    if r.get('content'):
        content = r['content'][:200].replace('\\n', ' ')
        print(f'   Content: {content}...')
    print('')
"
}

cmd_explore() {
    local query="${1:-}"
    if [[ -z "$query" ]]; then
        error "Usage: $0 $NAMESPACE explore \"<query>\""
        error "Example: $0 $NAMESPACE explore \"memory allocation\""
        exit 1
    fi
    
    log "Exploring subsystem: \"$query\""
    echo ""
    
    local project_name=$(basename "$NAMESPACE")
    local cache_dir="/opt/code_caches/${project_name}_cache"
    
    if [[ ! -d "$cache_dir/vectors" ]]; then
        warn "Vector cache not found. Using symbol search only."
        # Fallback: search for symbols matching the query
        echo "=== Symbols matching '$query' ==="
        echo ""
        
        # Try to find symbols by searching the cache keys
        strings /opt/ai_code_memory/cache.bin 2>/dev/null | \
            grep "^${NAMESPACE}/symbols/" | \
            sed "s|^${NAMESPACE}/symbols/||" | \
            grep -i "$(echo "$query" | tr ' ' '|')" | head -20 | \
            while read sym; do
                echo "  $sym"
            done || true
        
        echo ""
        echo "To enable semantic search, run: ./analyze_repo.sh \u003csource\u003e"
        return
    fi
    
    # Step 1: 语义搜索发现相关函数
    log "Step 1: Semantic search..."
    echo ""
    
    local search_result
    search_result=$("$CACHE_QUERY" "$query" --repo "$NAMESPACE" --type search --analysis-dir "$cache_dir" --max-results 5 --pretty 2>&1 | extract_json)
    
    echo "$search_result" | python3 -c "
import sys, json
data = json.load(sys.stdin)
results = data.get('results', [])

print('=== Top related functions ===')
for i, r in enumerate(results[:5], 1):
    print(f'{i}. {r[\"name\"]} [{r.get(\"kind\", \"unknown\")}]')
    print(f'   File: {r[\"file\"]}:{r.get(\"line_start\", \"\")}')
    print(f'   Score: {r.get(\"score\", 0):.3f}')
    print('')
"
    
    # Step 2: 深度分析 Top-1 函数
    log "Step 2: Deep analysis of top function..."
    echo ""
    
    local top_function
    top_function=$(echo "$search_result" | python3 -c "
import sys, json
data = json.load(sys.stdin)
results = data.get('results', [])
if results:
    print(results[0]['name'])
" 2>/dev/null)
    
    if [[ -n "$top_function" ]]; then
        log "Analyzing: $top_function"
        echo ""
        
        "$CACHE_QUERY" "$top_function" --repo "$NAMESPACE" --type context --depth 2 --pretty 2>&1 | extract_json | python3 -c "
import sys, json
data = json.load(sys.stdin)
ctx = data.get('context', {})
sym = ctx.get('symbol', {})
callers = ctx.get('callers', [])

if sym:
    print(f'Function: {sym.get(\"name\", \"N/A\")}')
    print(f'File: {sym.get(\"file\", \"N/A\")}:{sym.get(\"line\", \"N/A\")}')
    print(f'Callers: {len(callers)}')
    if callers:
        print('Top callers:')
        for c in callers[:10]:
            print(f'  {c[\"name\"]}')
    print('')
"
    fi
    
    echo ""
    log "Explore complete. Try:"
    echo "  $0 $NAMESPACE symbol $top_function --depth 2"
    echo "  $0 $NAMESPACE search \"$query\" --max 20"
}

cmd_compare() {
    local ns2="${1:-}"
    local query="${2:-}"
    
    if [[ -z "$ns2" || -z "$query" ]]; then
        error "Usage: $0 $NAMESPACE compare <namespace2> \"<query>\""
        error "Example: $0 $NAMESPACE compare /code/nginx \"memory pool\""
        exit 1
    fi
    
    log "Comparing: $NAMESPACE vs $ns2 on topic: \"$query\""
    echo ""
    
    local proj1=$(basename "$NAMESPACE")
    local proj2=$(basename "$ns2")
    local cache_dir1="/opt/code_caches/${proj1}_cache"
    local cache_dir2="/opt/code_caches/${proj2}_cache"
    
    # Project 1 semantic search
    echo "=== $proj1: Semantic Search ==="
    echo ""
    
    if [[ -d "$cache_dir1/vectors" ]]; then
        "$CACHE_QUERY" "$query" --repo "$NAMESPACE" --type search --analysis-dir "$cache_dir1" --max-results 3 --pretty 2>&1 | extract_json | python3 -c "
import sys, json
data = json.load(sys.stdin)
results = data.get('results', [])
for i, r in enumerate(results, 1):
    print(f'{i}. {r[\"name\"]} [{r.get(\"kind\", \"\")}] score={r.get(\"score\", 0):.3f}')
    print(f'   {r[\"file\"]}')
"
    else
        warn "No vector cache for $proj1"
    fi
    
    echo ""
    echo "=== $proj2: Semantic Search ==="
    echo ""
    
    if [[ -d "$cache_dir2/vectors" ]]; then
        "$CACHE_QUERY" "$query" --repo "$ns2" --type search --analysis-dir "$cache_dir2" --max-results 3 --pretty 2>&1 | extract_json | python3 -c "
import sys, json
data = json.load(sys.stdin)
results = data.get('results', [])
for i, r in enumerate(results, 1):
    print(f'{i}. {r[\"name\"]} [{r.get(\"kind\", \"\")}] score={r.get(\"score\", 0):.3f}')
    print(f'   {r[\"file\"]}')
"
    else
        warn "No vector cache for $proj2"
    fi
    
    echo ""
    log "Comparison complete"
}

cmd_report() {
    log "Generating architecture report for $NAMESPACE"
    echo ""
    
    local project_name=$(basename "$NAMESPACE")
    
    echo "{"
    echo '  "project": "'$project_name'",'
    echo '  "namespace": "'$NAMESPACE'",'
    echo '  "analysis_date": "'$(date -Iseconds)'",'
    
    # 元数据
    "$CACHE_QUERY" "${NAMESPACE}/_meta/info" --type exact --pretty 2>&1 | extract_json | python3 -c "
import sys, json
d = json.load(sys.stdin)
r = d.get('results', [{}])[0]
print(f'  \"total_keys\": {r.get(\"total_keys\", 0)},')
print(f'  \"imported_at\": \"{r.get(\"imported_at\", \"\")}\",')
"
    
    # 发现热点符号（通过语义搜索）
    echo '  "discovered_symbols": ['
    
    # 尝试几个通用查询来发现项目特色
    local queries=("main function" "init" "create" "process" "handle")
    local first=1
    for q in "${queries[@]}"; do
        local result
        result=$("$CACHE_QUERY" "$q" --repo "$NAMESPACE" --type search --analysis-dir "./${project_name}_cache" --max-results 1 --pretty 2>&1 | extract_json | python3 -c "
import sys, json
d = json.load(sys.stdin)
results = d.get('results', [])
if results:
    r = results[0]
    if $first:
        prefix = ''
    else:
        prefix = ','
    print(f'{prefix}    {{')
    print(f'      \"query\": \"{q}\",')
    print(f'      \"name\": \"{r.get(\"name\", \"\")}\",')
    print(f'      \"file\": \"{r.get(\"file\", \"\")}\",')
    print(f'      \"kind\": \"{r.get(\"kind\", \"\")}\",')
    print(f'      \"score\": {r.get(\"score\", 0):.3f}')
    print(f'    }}')
" 2>/dev/null)
        if [[ -n "$result" && "$result" != *"N/A"* ]]; then
            echo "$result"
            first=0
        fi
    done
    
    echo '  ]'
    echo "}"
}

cmd_top() {
    local n="${1:-10}"
    
    log "Top $n most referenced symbols in $NAMESPACE"
    echo ""
    
    # 从 cache.bin 中提取 callers/callees 统计
    strings /opt/ai_code_memory/cache.bin 2>/dev/null | \
        grep -E "^${NAMESPACE}/(callers|callees)/" | \
        sed "s|^${NAMESPACE}/callers/||; s|^${NAMESPACE}/callees/||" | \
        sort | uniq -c | sort -rn | head -n "$n" | \
        while read count name; do
            printf "  %5d  %s\n" "$count" "$name"
        done || warn "Could not extract caller statistics"
}

# =============================================================================
# 主程序
# =============================================================================

main() {
    check_tool
    
    case "$COMMAND" in
        overview|o)
            cmd_overview
            ;;
        symbol|sym|s)
            cmd_symbol "$1"
            ;;
        search|q)
            cmd_search "$1"
            ;;
        explore|e)
            cmd_explore "$1"
            ;;
        compare|cmp|vs)
            cmd_compare "$1" "$2"
            ;;
        report|r)
            cmd_report
            ;;
        top|t)
            cmd_top "${1:-10}"
            ;;
        help|--help|-h)
            echo "Usage: $0 <namespace> <command> [options]"
            echo ""
            echo "Commands:"
            echo "  overview                  项目概览"
            echo "  symbol <name>             符号上下文（含 caller/callee）"
            echo "  search \"<query>\"         语义搜索（自然语言）"
            echo "  explore \"<query>\"         子系统探索（语义搜索+自动分析）"
            echo "  compare <ns2> \"<query>\"   跨项目对比"
            echo "  report                    生成 JSON 架构报告"
            echo "  top [N]                   热点函数排行（默认10）"
            echo ""
            echo "Examples:"
            echo "  $0 /code/local/redis overview"
            echo "  $0 /code/local/redis symbol zmalloc --depth 2"
            echo "  $0 /code/local/redis search \"event loop\""
            echo "  $0 /code/local/redis explore \"memory allocation\""
            echo "  $0 /code/local/redis compare /code/nginx \"memory pool\""
            echo "  $0 /code/local/redis report > report.json"
            echo "  $0 /code/local/nginx explore \"configuration parser\""
            echo "  $0 /code/local/postgres explore \"query planner\""
            ;;
        *)
            error "Unknown command: $COMMAND"
            error "Run '$0 <namespace> help' for usage"
            exit 1
            ;;
    esac
}

main "$@"
