#!/bin/bash
#
# tools/cross_search.sh — 跨项目语义搜索
# ==========================================
#
# 功能: 一次搜索所有已索引项目，聚合结果，按语义相似度排序去重
#
# 用法:
#   ./tools/cross_search.sh "<query>" [max_results] [project1 project2 ...]
#
# 如果不指定项目，自动扫描 /opt/code_caches/ 下所有已索引项目
#
# 示例:
#   ./tools/cross_search.sh "memory pool" 10
#   ./tools/cross_search.sh "event loop" 5 nginx redis libuv
#   ./tools/cross_search.sh "slab allocator" 20 linux
#
# 依赖:
#   tools/cache_query — 每个项目的语义搜索
#   /opt/code_caches/{project}_cache/vectors/ — 向量文件

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_QUERY="${SCRIPT_DIR}/cache_query"
CACHE_DIR_BASE="/opt/code_caches"

# GPU 环境（同 explore_repo.sh / ai_code_search.sh）
GPU_LIBS=""
[ -d "${SCRIPT_DIR}" ] && GPU_LIBS="${GPU_LIBS}:${SCRIPT_DIR}"
[ -d "${SCRIPT_DIR}/.." ] && GPU_LIBS="${GPU_LIBS}:${SCRIPT_DIR}/.."
[ -d "/opt/TensorRT-10/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
[ -d "/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib"
[ -d "/opt/cuda/lib64" ] && GPU_LIBS="${GPU_LIBS}:/opt/cuda/lib64"
[ -d "/home/dministrator/anaconda3/envs/dl/lib" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib"
[ -d "/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/tensorrt_libs" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/tensorrt_libs"
[ -d "/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/nvidia/cudnn/lib" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/nvidia/cudnn/lib"
GPU_LIBS="${GPU_LIBS#:}"
export LD_LIBRARY_PATH="${GPU_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# 颜色
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
CYAN='\033[0;36m'
NC='\033[0m'
BOLD='\033[1m'

log()  { echo -e "${BLUE}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
error(){ echo -e "${RED}[ERROR]${NC} $1" >&2; }

# ============================================
# 自动发现已索引项目
# ============================================
discover_projects() {
    local projects=()
    for dir in "$CACHE_DIR_BASE"/*_cache/; do
        [[ -d "$dir/vectors" ]] || continue
        local name
        name=$(basename "$dir" | sed 's/_cache$//')
        projects+=("$name")
    done
    echo "${projects[@]}"
}

# ============================================
# 对单个项目执行搜索，返回 JSON
# ============================================
search_project() {
    local project="$1"
    local query="$2"
    local max_per_project="$3"

    local cache_dir="${CACHE_DIR_BASE}/${project}_cache"
    local namespace="/code/${project}"

    # 检查向量是否存在
    if [[ ! -d "$cache_dir/vectors" ]]; then
        return 0
    fi

    if [[ ! -x "$CACHE_QUERY" ]]; then
        warn "cache_query 未找到，跳过项目: $project"
        return 0
    fi

    local output
    output=$("$CACHE_QUERY" "$query" \
        --repo "$namespace" \
        --type search \
        --analysis-dir "$cache_dir" \
        --max-results "$max_per_project" \
        --pretty 2>/dev/null) || true

    if [[ -z "$output" ]]; then
        return 0
    fi

    echo "$output" | python3 -c "
import sys, json
try:
    data = json.load(sys.stdin)
except:
    sys.exit(0)
results = data.get('results', [])
project_name = '$project'
for r in results:
    r['_project'] = project_name
    print(json.dumps(r))
" 2>/dev/null || true
}

# ============================================
# 主程序
# ============================================
main() {
    local query="${1:-}"
    local max_results="${2:-15}"
    local -a projects=()

    if [[ -z "$query" ]]; then
        echo "用法: $0 \"<query>\" [max_results] [project1 project2 ...]"
        echo ""
        echo "如果不指定项目，自动扫描 /opt/code_caches/ 下所有已索引项目"
        echo ""
        echo "示例:"
        echo "  $0 \"memory pool\" 10"
        echo "  $0 \"event loop\" 5 nginx redis libuv"
        echo "  $0 \"slab allocator\" 20 linux"
        exit 1
    fi

    # 解析参数: 第三个及之后参数为项目名
    if [[ $# -ge 3 ]]; then
        shift 2
        projects=("$@")
    else
        IFS=' ' read -r -a projects <<< "$(discover_projects)"
    fi

    if [[ ${#projects[@]} -eq 0 ]]; then
        error "没有找到已索引的项目。请先运行 ./analyze_repo.sh 导入项目。"
        exit 1
    fi

    local max_per_project=$(( max_results > 5 ? max_results : 5 ))

    log "跨项目语义搜索: \"$query\""
    log "搜索范围: ${#projects[@]} 个项目"
    log ""

    local tmpfile
    tmpfile=$(mktemp /tmp/cross_search_XXXXXX.jsonl)
    trap "rm -f '$tmpfile'" EXIT

    # 并行搜索每个项目
    local pids=()
    for project in "${projects[@]}"; do
        search_project "$project" "$query" "$max_per_project" >> "$tmpfile" &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done

    echo ""
    echo -e "${BOLD}══════════════════════════════════════════════════════════════${NC}"
    echo -e "${BOLD}  搜索结果聚合${NC}"
    echo -e "${BOLD}══════════════════════════════════════════════════════════════${NC}"

    python3 -c "
import sys, json

B = chr(27) + '[1m'
N = chr(27) + '[0m'

results = []
seen = set()

with open('$tmpfile') as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
            project = r.get('_project', '?')
            name = r.get('name', '')
            score = r.get('score', 0)
            dedup_key = (project, name)
            if dedup_key in seen:
                continue
            seen.add(dedup_key)
            results.append(r)
        except:
            pass

results.sort(key=lambda x: -x.get('score', 0))
results = results[:$max_results]

if not results:
    print()
    print('  (无结果)')
    print()
    sys.exit(0)

from collections import Counter
proj_counter = Counter(r.get('_project', '?') for r in results)

print()
print(f'  {B}共 {len(results)} 条结果, 来自 {len(proj_counter)} 个项目{N}')
print()
proj_summary = ', '.join(f'{p}({c})' for p, c in proj_counter.most_common())
print(f'  📊 项目分布: {proj_summary}')
print()

for i, r in enumerate(results, 1):
    p = r.get('_project', '?')
    name = r.get('name', '?')
    kind = r.get('kind', '?')
    score = r.get('score', 0)
    sig = r.get('signature', '')
    f = r.get('file', '?')
    line = r.get('line_start', 0)
    content = r.get('content', '')

    print(f'  {B}#{i:2d}  {p}/{name}{N}  [{kind}]  score={score:.4f}')
    if sig:
        print(f'     Signature: {sig}')
    print(f'     File: {f}:{line}')
    if content:
        c = content[:250].replace(chr(92)+'n', '\n     | ')
        print(f'     Code:')
        print(f'     | {c}')
        if len(content) > 250:
            print(f'     | ...')
    print()
"
    echo ""
    log "搜索完成"
}

main "$@"
