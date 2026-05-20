#!/bin/bash
#
# AI Agent 超大项目语义搜索系统 - 分治策略
# ===========================================
#
# 针对 Linux 内核等超大项目（5万+文件），按子系统分治处理：
# - 每个子系统独立索引和生成向量
# - 支持全局跨子系统搜索
# - 保留子系统间调用关系
#
# 使用方式:
#   ./ai_code_search_large.sh init <repo_path> [config]    # 初始化子系统配置
#   ./ai_code_search_large.sh index <repo_path> [workers]  # 索引所有子系统
#   ./ai_code_search_large.sh vector [workers]             # 生成所有子系统向量
#   ./ai_code_search_large.sh search <query> [n]           # 全局搜索所有子系统
#   ./ai_code_search_large.sh search-sub <sub> <query> [n] # 搜索指定子系统
#   ./ai_code_search_large.sh dataflow <var>               # 全局变量追踪
#   ./ai_code_search_large.sh status                       # 查看处理状态
#
# 示例:
#   ./ai_code_search_large.sh init /opt/linux
#   ./ai_code_search_large.sh index /opt/linux 8
#   ./ai_code_search_large.sh vector 4
#   ./ai_code_search_large.sh search "schedule task" 10
#   ./ai_code_search_large.sh search-sub mm "page fault" 5
#   ./ai_code_search_large.sh dataflow task_struct

set -euo pipefail

# ============================================
# 环境变量
# ============================================
export LD_LIBRARY_PATH="/home/dministrator/my_db:/opt/TensorRT-10/lib:/home/dministrator/anaconda3/envs/dl/lib:${LD_LIBRARY_PATH:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LARGE_CONFIG="${SCRIPT_DIR}/.large_project_config"

# 颜色输出
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ============================================
# 子系统配置
# ============================================
# Linux 内核默认子系统映射
declare -A DEFAULT_SUBSYSTEMS=(
    ["kernel"]="kernel"
    ["mm"]="mm"
    ["fs"]="fs"
    ["net"]="net"
    ["drivers"]="drivers"
    ["arch"]="arch"
    ["ipc"]="ipc"
    ["security"]="security"
    ["crypto"]="crypto"
    ["block"]="block"
    ["init"]="init"
    ["lib"]="lib"
    ["include"]="include"
    ["scripts"]="scripts"
    ["tools"]="tools"
)

# 加载配置
load_config() {
    if [[ ! -f "$LARGE_CONFIG" ]]; then
        error "未找到配置文件: $LARGE_CONFIG"
        error "请先运行: $0 init <repo_path>"
        exit 1
    fi
    source "$LARGE_CONFIG"
}

# ============================================
# 子命令: init - 初始化配置
# ============================================
cmd_init() {
    local repo_path="${1:-}"
    local config_file="${2:-$LARGE_CONFIG}"
    
    if [[ -z "$repo_path" ]]; then
        echo "用法: $0 init <repo_path> [config_file]"
        echo "  repo_path:   源码目录路径"
        echo "  config_file: 配置文件路径 (默认: .large_project_config)"
        echo ""
        echo "示例: $0 init /opt/linux"
        exit 1
    fi
    
    if [[ ! -d "$repo_path" ]]; then
        error "目录不存在: $repo_path"
        exit 1
    fi
    
    repo_path="$(cd "$repo_path" && pwd)"
    local project_name=$(basename "$repo_path")
    local base_cache="${SCRIPT_DIR}/${project_name}_subsystems"
    
    info "初始化超大项目配置..."
    info "  项目: $project_name"
    info "  路径: $repo_path"
    info "  配置: $config_file"
    
    # 检测子系统目录
    info "检测子系统目录..."
    local detected_subs=()
    for sub in "${!DEFAULT_SUBSYSTEMS[@]}"; do
        if [[ -d "${repo_path}/${sub}" ]]; then
            detected_subs+=("$sub")
            info "  ✓ $sub"
        fi
    done
    
    if [[ ${#detected_subs[@]} -eq 0 ]]; then
        warn "未检测到默认子系统，使用整个项目作为单个子系统"
        detected_subs=("main")
    fi
    
    # 生成配置文件
    cat > "$config_file" << EOF
# AI Agent 超大项目配置 - 自动生成
# 项目: $project_name
# 生成时间: $(date)

REPO_PATH="$repo_path"
PROJECT_NAME="$project_name"
BASE_CACHE="$base_cache"
SUBSYSTEMS=(${detected_subs[@]})

# 子系统路径映射
EOF
    
    for sub in "${detected_subs[@]}"; do
        if [[ "$sub" == "main" ]]; then
            echo "SUB_${sub}=\"${repo_path}\"" >> "$config_file"
        else
            echo "SUB_${sub}=\"${repo_path}/${sub}\"" >> "$config_file"
        fi
    done
    
    cat >> "$config_file" << EOF

# 最大 chunk 数（超过则跳过，防止内存溢出）
MAX_CHUNKS_PER_SUB=200000

# 全局符号表文件（轻量级，不生成向量）
GLOBAL_SYMBOLS="${base_cache}/global_symbols.jsonl"
EOF
    
    ok "配置已保存到: $config_file"
    info "检测到 ${#detected_subs[@]} 个子系统"
    
    # 创建 cache 目录
    mkdir -p "$base_cache"
    for sub in "${detected_subs[@]}"; do
        mkdir -p "${base_cache}/${sub}_cache"
    done
    ok "Cache 目录已创建"
}

# ============================================
# 子命令: index - 索引所有子系统
# ============================================
cmd_index() {
    local repo_path="${1:-}"
    local workers="${2:-$(nproc)}"
    
    # 如果传了 repo_path，先 init
    if [[ -n "$repo_path" ]]; then
        cmd_init "$repo_path"
    fi
    
    load_config
    
    info "开始索引所有子系统..."
    info "  项目: $PROJECT_NAME"
    info "  子系统数: ${#SUBSYSTEMS[@]}"
    info "  工作进程: $workers"
    echo ""
    
    local pids=()
    local idx=0
    
    for sub in "${SUBSYSTEMS[@]}"; do
        local sub_path="${repo_path}/${sub}"
        local sub_cache="${BASE_CACHE}/${sub}_cache"
        
        if [[ ! -d "$sub_path" ]]; then
            warn "跳过不存在的子系统: $sub"
            continue
        fi
        
        info "[$((idx+1))/${#SUBSYSTEMS[@]}] 索引子系统: $sub"
        
        # 后台索引
        (
            "${SCRIPT_DIR}/tools/code_indexer" "$sub_path" "$sub_cache" "$workers" 2>/dev/null
            local chunks=$(wc -l < "${sub_cache}/chunks_meta.jsonl" 2>/dev/null || echo 0)
            ok "[$sub] 索引完成: $chunks chunks"
        ) &
        pids+=($!)
        
        # 每 4 个子系统串行，避免同时 fork 太多进程
        if [[ $((idx % 4)) -eq 3 ]]; then
            info "等待当前批次完成..."
            for pid in "${pids[@]}"; do
                wait "$pid" 2>/dev/null || true
            done
            pids=()
        fi
        
        idx=$((idx+1))
    done
    
    # 等待剩余
    for pid in "${pids[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    
    ok "所有子系统索引完成!"
    cmd_status
}

# ============================================
# 子命令: vector - 生成所有子系统向量
# ============================================
cmd_vector() {
    local workers="${1:-2}"
    
    load_config
    
    info "开始生成向量..."
    info "  并行度: $workers (每个子系统需要独立 GPU 显存)"
    echo ""
    
    local idx=0
    for sub in "${SUBSYSTEMS[@]}"; do
        local sub_cache="${BASE_CACHE}/${sub}_cache"
        
        if [[ ! -f "${sub_cache}/chunks_text.txt" ]]; then
            warn "跳过未索引的子系统: $sub"
            continue
        fi
        
        # 检查 chunk 数量
        local chunks=$(wc -l < "${sub_cache}/chunks_meta.jsonl" 2>/dev/null || echo 0)
        if [[ $chunks -gt ${MAX_CHUNKS_PER_SUB:-200000} ]]; then
            warn "[$sub] chunk 数 $chunks 超过限制，跳过向量生成"
            warn "  建议: 进一步拆分该子系统"
            continue
        fi
        
        info "[$((idx+1))/${#SUBSYSTEMS[@]}] 生成向量: $sub ($chunks chunks)"
        
        "${SCRIPT_DIR}/tools/batch_embedder" "$sub_cache" --model jina --name "${PROJECT_NAME}-${sub}" 2>/dev/null || \
            warn "[$sub] 向量生成失败"
        
        # 生成调用图和数据流
        "${SCRIPT_DIR}/tools/call_graph" "$sub_cache" 2>/dev/null || true
        "${SCRIPT_DIR}/tools/dataflow" analyze "$sub_cache" 2>/dev/null || true
        "${SCRIPT_DIR}/tools/word_freq" "$sub_cache" 2>/dev/null || true
        
        ok "[$sub] 向量生成完成"
        echo ""
        
        idx=$((idx+1))
    done
    
    ok "所有子系统向量生成完成!"
}

# ============================================
# 子命令: search - 全局搜索（聚合所有子系统）
# ============================================
cmd_search() {
    local query="${1:-}"
    local max_results="${2:-10}"
    local per_sub=$((max_results / ${#SUBSYSTEMS[@]} + 1))
    
    if [[ -z "$query" ]]; then
        echo "用法: $0 search <query> [max_results]"
        echo "  query:       自然语言查询"
        echo "  max_results: 总返回结果数 (默认: 10)"
        echo ""
        echo "示例:"
        echo "  $0 search \"schedule task\" 10"
        echo "  $0 search \"page fault handler\" 5"
        exit 1
    fi
    
    load_config
    
    info "全局搜索: \"$query\""
    info "  子系统数: ${#SUBSYSTEMS[@]}"
    info "  每子系统搜索: $per_sub 条"
    echo ""
    
    # 临时结果文件
    local tmp_results="/tmp/ai_search_results_$$.txt"
    > "$tmp_results"
    
    for sub in "${SUBSYSTEMS[@]}"; do
        local sub_cache="${BASE_CACHE}/${sub}_cache"
        
        if [[ ! -d "${sub_cache}/vectors" ]]; then
            continue
        fi
        
        # 搜索该子系统，追加到临时文件
        "${SCRIPT_DIR}/tools/vector_search" "$sub_cache" "$query" "$per_sub" \
            --model jina --rich 2>/dev/null | \
            sed "s/^/[${sub}] /" >> "$tmp_results" || true
    done
    
    # 去重并显示
    if [[ -s "$tmp_results" ]]; then
        echo "╔══════════════════════════════════════════════════════════════════╗"
        echo "║ 全局搜索结果: $query"
        echo "╚══════════════════════════════════════════════════════════════════╝"
        echo ""
        cat "$tmp_results" | head -n $((max_results * 10))
    else
        warn "未找到结果"
    fi
    
    rm -f "$tmp_results"
}

# ============================================
# 子命令: search-sub - 搜索指定子系统
# ============================================
cmd_search_sub() {
    local sub="${1:-}"
    local query="${2:-}"
    local max_results="${3:-10}"
    
    if [[ -z "$sub" || -z "$query" ]]; then
        echo "用法: $0 search-sub <subsystem> <query> [max_results]"
        echo "  subsystem:   子系统名称"
        echo "  query:       自然语言查询"
        echo "  max_results: 返回结果数 (默认: 10)"
        echo ""
        echo "可用子系统:"
        load_config 2>/dev/null && echo "  ${SUBSYSTEMS[@]}" || echo "  (请先运行 init)"
        exit 1
    fi
    
    load_config
    
    local sub_cache="${BASE_CACHE}/${sub}_cache"
    
    if [[ ! -d "${sub_cache}/vectors" ]]; then
        error "子系统未生成向量: $sub"
        error "请先运行: $0 vector"
        exit 1
    fi
    
    info "搜索子系统 [$sub]: \"$query\""
    
    "${SCRIPT_DIR}/tools/vector_search" "$sub_cache" "$query" "$max_results" \
        --model jina --rich --callgraph 2>/dev/null
}

# ============================================
# 子命令: dataflow - 全局变量追踪
# ============================================
cmd_dataflow() {
    local var_name="${1:-}"
    local sub_filter="${2:-}"
    
    if [[ -z "$var_name" ]]; then
        echo "用法: $0 dataflow <var_name> [subsystem]"
        echo "  var_name:  要追踪的变量名"
        echo "  subsystem: 指定子系统 (默认: 搜索所有)"
        echo ""
        echo "示例:"
        echo "  $0 dataflow task_struct"
        echo "  $0 dataflow task_struct kernel"
        exit 1
    fi
    
    load_config
    
    info "全局变量追踪: $var_name"
    
    if [[ -n "$sub_filter" ]]; then
        # 只搜索指定子系统
        local sub_cache="${BASE_CACHE}/${sub_filter}_cache"
        if [[ -f "${sub_cache}/chunks_meta.jsonl" ]]; then
            "${SCRIPT_DIR}/tools/dataflow" show "$sub_cache" "$var_name" 2>/dev/null
        else
            error "子系统未索引: $sub_filter"
        fi
    else
        # 搜索所有子系统
        for sub in "${SUBSYSTEMS[@]}"; do
            local sub_cache="${BASE_CACHE}/${sub}_cache"
            
            if [[ ! -f "${sub_cache}/chunks_meta.jsonl" ]]; then
                continue
            fi
            
            # 快速检查变量是否存在于该子系统
            if grep -q "\"$var_name\"" "${sub_cache}/chunks_meta.jsonl" 2>/dev/null; then
                echo ""
                echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
                echo "  子系统: $sub"
                echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
                "${SCRIPT_DIR}/tools/dataflow" show "$sub_cache" "$var_name" 2>/dev/null || true
            fi
        done
    fi
}

# ============================================
# 子命令: status - 查看处理状态
# ============================================
cmd_status() {
    load_config 2>/dev/null || { echo "未初始化"; exit 1; }
    
    echo "╔══════════════════════════════════════════════════════════════════╗"
    echo "║ 项目: $PROJECT_NAME"
    echo "║ 路径: $REPO_PATH"
    echo "╚══════════════════════════════════════════════════════════════════╝"
    echo ""
    printf "%-15s %-12s %-12s %-10s\n" "子系统" "Chunks" "向量" "调用图"
    printf "%-15s %-12s %-12s %-10s\n" "---------------" "------------" "------------" "----------"
    
    local total_chunks=0
    local total_vectors=0
    
    for sub in "${SUBSYSTEMS[@]}"; do
        local sub_cache="${BASE_CACHE}/${sub}_cache"
        local chunks="-"
        local vectors="-"
        local callgraph="-"
        
        if [[ -f "${sub_cache}/chunks_meta.jsonl" ]]; then
            chunks=$(wc -l < "${sub_cache}/chunks_meta.jsonl" 2>/dev/null || echo 0)
            total_chunks=$((total_chunks + chunks))
        fi
        
        if [[ -d "${sub_cache}/vectors" ]]; then
            vectors=$(ls "${sub_cache}/vectors/"*.bin 2>/dev/null | wc -l)
            total_vectors=$((total_vectors + vectors))
            vectors="✓"
        fi
        
        if [[ -f "${sub_cache}/call_graph.json" ]]; then
            callgraph="✓"
        fi
        
        printf "%-15s %-12s %-12s %-10s\n" "$sub" "$chunks" "$vectors" "$callgraph"
    done
    
    printf "%-15s %-12s %-12s %-10s\n" "---------------" "------------" "------------" "----------"
    printf "%-15s %-12s\n" "总计" "$total_chunks"
    echo ""
    echo "Cache 目录: $BASE_CACHE"
}

# ============================================
# 主程序
# ============================================
main() {
    local cmd="${1:-help}"
    shift || true

    case "$cmd" in
        init)
            cmd_init "$@"
            ;;
        index)
            cmd_index "$@"
            ;;
        vector|v)
            cmd_vector "$@"
            ;;
        search|s)
            cmd_search "$@"
            ;;
        search-sub|ss)
            cmd_search_sub "$@"
            ;;
        dataflow|df)
            cmd_dataflow "$@"
            ;;
        status|st)
            cmd_status
            ;;
        help|--help|-h|*)
            echo "AI Agent 超大项目语义搜索系统"
            echo "==============================="
            echo ""
            echo "针对 Linux 内核等超大项目（5万+文件），按子系统分治处理"
            echo ""
            echo "子命令:"
            echo "  init <repo> [config]        - 初始化子系统配置"
            echo "  index <repo> [workers]      - 索引所有子系统"
            echo "  vector [workers]            - 生成所有子系统向量"
            echo "  search <query> [n]          - 全局搜索（聚合所有子系统）"
            echo "  search-sub <sub> <query> [n] - 搜索指定子系统"
            echo "  dataflow <var> [sub]        - 全局变量追踪"
            echo "  status                      - 查看处理状态"
            echo ""
            echo "示例:"
            echo "  $0 init /opt/linux"
            echo "  $0 index /opt/linux 8"
            echo "  $0 vector 2"
            echo "  $0 search \"schedule task\" 10"
            echo "  $0 search-sub mm \"page fault\" 5"
            echo "  $0 dataflow task_struct"
            ;;
    esac
}

main "$@"
