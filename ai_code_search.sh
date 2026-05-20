#!/bin/bash
#
# AI Agent 代码语义搜索系统 - 统一脚本
# ======================================
#
# 功能：对代码库进行索引、生成向量、构建调用图、语义搜索
# 技术栈：C 多进程索引 + Jina v2 语义向量 + TensorRT GPU 推理
#
# 使用方式:
#   ./ai_code_search.sh index <repo_path> [cache_dir] [workers]  # 索引代码
#   ./ai_code_search.sh vector <cache_dir> [project_name]        # 生成向量
#   ./ai_code_search.sh callgraph <cache_dir>                    # 构建调用图
#   ./ai_code_search.sh search <cache_dir> <query> [max_results] # 语义搜索
#   ./ai_code_search.sh snippet <cache_dir> <code_file>          # 代码片段搜索
#   ./ai_code_search.sh dataflow <cache_dir> <var_name>          # 变量数据流追踪
#   ./ai_code_search.sh analyze <repo_path> [cache_dir]          # 一键分析
#
# 示例:
#   ./ai_code_search.sh index /opt/stable-diffusion.cpp ./sd_cache 4
#   ./ai_code_search.sh vector ./sd_cache stable-diffusion.cpp
#   ./ai_code_search.sh search ./sd_cache "upscale image" 10
#

set -euo pipefail

# ============================================
# 环境变量配置（TensorRT + cuDNN + CUDA）
# ============================================
# 注意：根据你的实际路径修改以下配置
export LD_LIBRARY_PATH="/home/dministrator/my_db:/opt/TensorRT-10/lib:/home/dministrator/anaconda3/envs/dl/lib:${LD_LIBRARY_PATH:-}"

# 验证 GPU 可用性
if command -v nvidia-smi &>/dev/null; then
    echo "✅ GPU 环境检测: $(nvidia-smi --query-gpu=name --format=csv,noheader | head -1)"
else
    echo "⚠️  警告: nvidia-smi 不可用，GPU 加速可能无法使用"
fi

# ============================================
# 工具路径
# ============================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INDEXER="${SCRIPT_DIR}/tools/code_indexer"
BATCH_EMBEDDER="${SCRIPT_DIR}/tools/batch_embedder"
VECTOR_SEARCH="${SCRIPT_DIR}/tools/vector_search"
CALL_GRAPH="${SCRIPT_DIR}/tools/call_graph"
WORD_FREQ="${SCRIPT_DIR}/tools/word_freq"
DATAFLOW="${SCRIPT_DIR}/tools/dataflow"

# ============================================
# 颜色输出
# ============================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ============================================
# 辅助函数：检查工具是否存在
# ============================================
check_tools() {
    local missing=0
    for tool in "$INDEXER" "$BATCH_EMBEDDER" "$VECTOR_SEARCH" "$CALL_GRAPH" "$DATAFLOW"; do
        if [[ ! -x "$tool" ]]; then
            error "工具不存在: $tool"
            missing=1
        fi
    done
    if [[ $missing -eq 1 ]]; then
        error "请先运行: make clean && make"
        exit 1
    fi
}

# ============================================
# 子命令: index - 索引代码库
# ============================================
cmd_index() {
    local repo_path="${1:-}"
    local cache_dir="${2:-./ai_code_memory}"
    local workers="${3:-$(nproc)}"

    if [[ -z "$repo_path" ]]; then
        echo "用法: $0 index <repo_path> [cache_dir] [workers]"
        echo "  repo_path:  源码目录路径"
        echo "  cache_dir:  输出缓存目录 (默认: ./ai_code_memory)"
        echo "  workers:    并行工作进程数 (默认: CPU 核心数)"
        echo ""
        echo "示例: $0 index /opt/stable-diffusion.cpp ./sd_cache 4"
        exit 1
    fi

    if [[ ! -d "$repo_path" ]]; then
        error "目录不存在: $repo_path"
        exit 1
    fi

    info "开始索引代码库..."
    info "  源码路径: $repo_path"
    info "  缓存目录: $cache_dir"
    info "  工作进程: $workers"
    echo ""

    "$INDEXER" "$repo_path" "$cache_dir" "$workers"

    ok "索引完成!"
    info "输出文件:"
    info "  ${cache_dir}/chunks_text.txt   - 代码文本（用于向量生成）"
    info "  ${cache_dir}/chunks_meta.jsonl - 元数据（用于搜索展示）"
}

# ============================================
# 子命令: vector - 生成语义向量
# ============================================
cmd_vector() {
    local cache_dir="${1:-}"
    local project_name="${2:-}"

    if [[ -z "$cache_dir" ]]; then
        echo "用法: $0 vector <cache_dir> [project_name]"
        echo "  cache_dir:     缓存目录路径"
        echo "  project_name:  项目名称（用于向量文件命名）"
        echo ""
        echo "示例: $0 vector ./sd_cache stable-diffusion.cpp"
        exit 1
    fi

    if [[ ! -f "${cache_dir}/chunks_text.txt" ]]; then
        error "未找到 chunks_text.txt，请先运行: $0 index <repo_path> ${cache_dir}"
        exit 1
    fi

    # 自动推导项目名称
    if [[ -z "$project_name" ]]; then
        project_name=$(basename "$cache_dir" | sed 's/_cache$//')
        warn "未指定项目名，使用: $project_name"
    fi

    info "生成语义向量..."
    info "  缓存目录: $cache_dir"
    info "  模型: Jina v2 (代码专用)"
    info "  项目名: $project_name"
    echo ""

    # Step 1: 生成词频统计（用于 TF-IDF 排序优化）
    info "Step 1/3: 统计词频..."
    "$WORD_FREQ" "$cache_dir" || warn "词频统计失败（非致命）"

    # Step 2: 生成语义向量
    info "Step 2/4: 生成语义向量（TensorRT GPU）..."
    "$BATCH_EMBEDDER" "$cache_dir" --model jina --name "$project_name"

    # Step 3: 构建调用关系图
    info "Step 3/4: 构建调用关系图..."
    "$CALL_GRAPH" "$cache_dir" || warn "调用图构建失败（非致命）"

    # Step 4: 生成变量数据流分析
    info "Step 4/4: 生成变量数据流分析..."
    "$DATAFLOW" analyze "$cache_dir" || warn "数据流分析失败（非致命）"

    ok "向量生成完成!"
    info "输出文件:"
    info "  ${cache_dir}/vectors/code_local_${project_name}.jina.bin - 向量数据"
    info "  ${cache_dir}/vectors/code_local_${project_name}.jina.idx - 索引文件"
    info "  ${cache_dir}/call_graph.json - 调用关系图"
    info "  ${cache_dir}/dataflow.json - 变量数据流"
    info "  ${cache_dir}/word_freq.json - 词频统计"
}

# ============================================
# 子命令: callgraph - 构建调用关系图
# ============================================
cmd_callgraph() {
    local cache_dir="${1:-}"

    if [[ -z "$cache_dir" ]]; then
        echo "用法: $0 callgraph <cache_dir>"
        echo "  cache_dir: 缓存目录路径"
        exit 1
    fi

    info "构建调用关系图..."
    "$CALL_GRAPH" "$cache_dir"

    ok "调用图构建完成!"
    info "输出文件: ${cache_dir}/call_graph.json"
}

# ============================================
# 子命令: search - 语义搜索
# ============================================
cmd_search() {
    local cache_dir="${1:-}"
    local query="${2:-}"
    local max_results="${3:-10}"

    if [[ -z "$cache_dir" || -z "$query" ]]; then
        echo "用法: $0 search <cache_dir> <query> [max_results]"
        echo "  cache_dir:    缓存目录路径"
        echo "  query:        自然语言查询（用引号包裹）"
        echo "  max_results:  返回结果数量 (默认: 10)"
        echo ""
        echo "示例:"
        echo "  $0 search ./sd_cache \"upscale image\" 5"
        echo "  $0 search ./sd_cache \"memory allocation buffer pool\" 10"
        echo "  $0 search ./sd_cache \"VAE encode latent\" 5"
        exit 1
    fi

    if [[ ! -d "${cache_dir}/vectors" ]]; then
        error "未找到向量文件，请先运行: $0 vector ${cache_dir}"
        exit 1
    fi

    info "语义搜索: \"$query\""
    info "  缓存: $cache_dir"
    info "  最大结果: $max_results"
    echo ""

    # 使用 Jina 模型 + rich 模式 + callgraph 显示
    "$VECTOR_SEARCH" "$cache_dir" "$query" "$max_results" \
        --model jina --rich --callgraph 2>/dev/null
}

# ============================================
# 子命令: snippet - 代码片段搜索
# ============================================
cmd_snippet() {
    local cache_dir="${1:-}"
    local code_file="${2:-}"
    local max_results="${3:-5}"

    if [[ -z "$cache_dir" || -z "$code_file" ]]; then
        echo "用法: $0 snippet <cache_dir> <code_file> [max_results]"
        echo "  cache_dir:  缓存目录路径"
        echo "  code_file:  要搜索的代码片段文件"
        echo "  max_results: 返回结果数量 (默认: 5)"
        echo ""
        echo "示例:"
        echo "  $0 snippet ./sd_cache ./my_kernel.cpp 5"
        exit 1
    fi

    if [[ ! -f "$code_file" ]]; then
        error "文件不存在: $code_file"
        exit 1
    fi

    info "代码片段搜索: $code_file"
    info "  缓存: $cache_dir"
    echo ""

    "$VECTOR_SEARCH" "$cache_dir" --snippet "$code_file" "$max_results" \
        --model jina --rich --callgraph 2>/dev/null
}

# ============================================
# 子命令: analyze - 一键完整分析
# ============================================
cmd_analyze() {
    local repo_path="${1:-}"
    local cache_dir="${2:-./ai_code_memory}"

    if [[ -z "$repo_path" ]]; then
        echo "用法: $0 analyze <repo_path> [cache_dir]"
        echo "  repo_path:  源码目录路径"
        echo "  cache_dir:  缓存目录 (默认: ./ai_code_memory)"
        echo ""
        echo "示例: $0 analyze /opt/stable-diffusion.cpp ./sd_cache"
        exit 1
    fi

    local project_name
    project_name=$(basename "$cache_dir" | sed 's/_cache$//')

    echo "============================================"
    echo "  AI Agent 代码语义分析 - 一键流程"
    echo "============================================"
    echo ""

    # Step 1: 索引
    cmd_index "$repo_path" "$cache_dir"
    echo ""

    # Step 2: 生成向量
    cmd_vector "$cache_dir" "$project_name"
    echo ""

    ok "分析完成! 现在可以搜索了:"
    echo ""
    echo "  $0 search ${cache_dir} \"<你的查询>\""
    echo ""
    echo "示例查询:"
    echo "  $0 search ${cache_dir} \"pipeline architecture\" 5"
    echo "  $0 search ${cache_dir} \"memory allocation\" 10"
    echo "  $0 search ${cache_dir} \"VAE encoder decoder\" 5"
}

# ============================================
# 子命令: demo - 演示系统能力
# ============================================
cmd_demo() {
    local cache_dir="${1:-./sd_cache}"

    if [[ ! -d "${cache_dir}/vectors" ]]; then
        error "未找到向量数据，请先运行: $0 vector ${cache_dir}"
        exit 1
    fi

    echo "============================================"
    echo "  AI Agent 代码语义搜索 - 能力演示"
    echo "============================================"
    echo ""

    # 演示 1: 自然语言搜索
    echo -e "${GREEN}演示 1: 自然语言搜索 - \"upscale image\"${NC}"
    echo "--------------------------------------------"
    "$VECTOR_SEARCH" "$cache_dir" "upscale image" 3 --model jina --rich --callgraph 2>/dev/null
    echo ""

    # 演示 2: 架构搜索
    echo -e "${GREEN}演示 2: 架构理解 - \"text encoder embedding\"${NC}"
    echo "--------------------------------------------"
    "$VECTOR_SEARCH" "$cache_dir" "text encoder embedding" 3 --model jina --rich 2>/dev/null
    echo ""

    # 演示 3: 跨文件调用关系
    echo -e "${GREEN}演示 3: 调用关系 - \"memory allocation buffer pool\"${NC}"
    echo "--------------------------------------------"
    "$VECTOR_SEARCH" "$cache_dir" "memory allocation buffer pool" 3 --model jina --rich --callgraph 2>/dev/null
    echo ""

    ok "演示完成!"
}

# ============================================
# 子命令: dataflow - 变量数据流追踪
# ============================================
cmd_dataflow() {
    local cache_dir="${1:-}"
    local var_name="${2:-}"

    if [[ -z "$cache_dir" || -z "$var_name" ]]; then
        echo "用法: $0 dataflow <cache_dir> <var_name>"
        echo "  cache_dir: 缓存目录路径"
        echo "  var_name:  要追踪的变量名"
        echo ""
        echo "示例:"
        echo "  $0 dataflow ./nginx_cache c          # 追踪 connection 指针"
        echo "  $0 dataflow ./nginx_cache rc         # 追踪返回值"
        echo "  $0 dataflow ./sd_cache ctx           # 追踪上下文指针"
        exit 1
    fi

    if [[ ! -f "${cache_dir}/chunks_meta.jsonl" ]]; then
        error "未找到索引文件，请先运行: $0 index <repo> ${cache_dir}"
        exit 1
    fi

    info "追踪变量 '${var_name}' 的数据流..."
    "$DATAFLOW" show "$cache_dir" "$var_name"
}

# ============================================
# 主程序
# ============================================
main() {
    check_tools

    local cmd="${1:-help}"
    shift || true

    case "$cmd" in
        index|i)
            cmd_index "$@"
            ;;
        vector|v)
            cmd_vector "$@"
            ;;
        callgraph|c)
            cmd_callgraph "$@"
            ;;
        search|s)
            cmd_search "$@"
            ;;
        snippet|snip)
            cmd_snippet "$@"
            ;;
        dataflow|df)
            cmd_dataflow "$@"
            ;;
        analyze|a)
            cmd_analyze "$@"
            ;;
        demo|d)
            cmd_demo "$@"
            ;;
        help|--help|-h|*)
            echo "AI Agent 代码语义搜索系统"
            echo "========================="
            echo ""
            echo "子命令:"
            echo "  index <repo> [cache] [workers]  - 索引代码库"
            echo "  vector <cache> [name]           - 生成语义向量"
            echo "  callgraph <cache>               - 构建调用关系图"
            echo "  search <cache> <query> [n]      - 语义搜索"
            echo "  snippet <cache> <file> [n]      - 代码片段搜索"
            echo "  dataflow <cache> <var>          - 变量数据流追踪"
            echo "  analyze <repo> [cache]          - 一键完整分析"
            echo "  demo [cache]                    - 演示系统能力"
            echo ""
            echo "示例:"
            echo "  $0 index /opt/stable-diffusion.cpp ./sd_cache 4"
            echo "  $0 vector ./sd_cache stable-diffusion.cpp"
            echo "  $0 search ./sd_cache \"upscale image\" 5"
            echo "  $0 snippet ./sd_cache ./my_code.cpp 5"
            echo "  $0 dataflow ./nginx_cache c     # 追踪 connection 变量"
            echo "  $0 analyze /opt/stable-diffusion.cpp ./sd_cache"
            echo ""
            echo "高级搜索选项（直接用 vector_search 工具）:"
            echo "  --model jina      使用 Jina v2 代码模型"
            echo "  --rich            显示完整代码上下文"
            echo "  --callgraph       显示函数调用关系"
            echo "  --kind function   只搜索函数"
            echo "  --lang cpp        只搜索 C++ 代码"
            echo "  --file cuda       只搜索文件名含 cuda 的文件"
            ;;
    esac
}

main "$@"