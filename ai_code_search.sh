#!/bin/bash
#
# AI Agent 代码语义搜索系统 - 统一脚本
# ======================================
#
# 功能：对代码库进行索引、生成向量、构建调用图、语义搜索、变量数据流追踪
# 技术栈：C 多进程索引 + Jina v2 语义向量 + TensorRT GPU 推理
#
# 使用方式:
#   ./ai_code_search.sh index <repo_path> [cache_dir] [workers]  # 索引代码
#   ./ai_code_search.sh vector <cache_dir> [project_name]        # 生成向量+调用图+数据流
#   ./ai_code_search.sh callgraph <cache_dir>                    # 构建调用图
#   ./ai_code_search.sh search <cache_dir> <query> [max_results] # 语义搜索
#   ./ai_code_search.sh snippet <cache_dir> <code_file>          # 代码片段搜索
#   ./ai_code_search.sh dataflow <cache_dir> <var_name>          # 变量数据流追踪（字段级+跨函数）
#   ./ai_code_search.sh analyze <repo_path> [cache_dir]          # 一键分析
#
# 示例:
#   ./ai_code_search.sh index /opt/stable-diffusion.cpp ./sd_cache 4
#   ./ai_code_search.sh vector ./sd_cache stable-diffusion.cpp
#   ./ai_code_search.sh search ./sd_cache "upscale image" 10
#   ./ai_code_search.sh dataflow ./nginx_cache c                 # 追踪 connection 指针的字段级数据流
#

# ============================================
# 系统架构说明
# ============================================
#
# 本系统的核心代码 100% 使用 C 语言实现，符合 C toolchain preferred for production 的原则。
#
# 【自研工具（C 语言）】
#   tools/code_indexer.c     - C 多进程代码索引器（ctags + AST 提取）
#   tools/batch_embedder.c   - C 批量向量生成器（调用 ONNX C API）
#   tools/vector_search.c    - C 语义搜索引擎（HNSW + TF-IDF + PageRank）
#   tools/call_graph.c       - C 调用关系分析器（扫描函数体找调用）
#   tools/dataflow.c         - C 变量数据流追踪器（字段级 + 跨函数）
#   tools/word_freq.c        - C 词频统计器（TF-IDF 权重计算）
#   ai_code_search.sh        - Shell 统一入口脚本
#
# 【第三方依赖（C++ 库，通过 C API 调用）】
#   ONNX Runtime            - 微软 C++ 库，提供 ONNX 模型推理 C API
#   TensorRT                - NVIDIA C++ 库，GPU 推理加速（CUDA 内核）
#   tokenizers-cpp          - HuggingFace C++ 库，BPE 分词器
#   jansson                 - C 语言 JSON 解析库（dataflow 用）
#
# 【模型层】
#   Jina v2 代码嵌入模型    - 用 Python/PyTorch 训练，导出为 .onnx 格式
#   运行时不依赖 Python，纯 C + ONNX Runtime + TensorRT 推理
#
# 【架构特点】
#   - 用户代码层：100% C（索引、搜索、分析、调用图、数据流）
#   - 推理引擎层：C++ 库（ONNX Runtime + TensorRT 提供 GPU 加速）
#   - 脚本包装层：Shell（一键命令封装）
#   - 模型训练层：Python（一次性导出 ONNX，运行时不需 Python）
#
# 因此可称为：C 语言实现的 AI 代码语义搜索系统（基于 TensorRT GPU 加速）

# ============================================
# 代码库探索方法论（以 nginx 为例）
# ============================================
#
# 本系统用于对陌生代码库进行深度语义分析。以下是以 nginx 为例的完整探索流程：
#
# 【Phase 1: 索引与向量化】
#   ./ai_code_search.sh index /opt/nginx ./nginx_cache 4
#   ./ai_code_search.sh vector ./nginx_cache nginx
#   # 说明：先索引源码生成 chunks，再用 TensorRT GPU 生成语义向量
#
# 【Phase 2: 架构概览 - 用自然语言搜索核心概念】
#   ./ai_code_search.sh search ./nginx_cache "event loop epoll kqueue" 10
#   ./ai_code_search.sh search ./nginx_cache "HTTP request phase handler" 10
#   ./ai_code_search.sh search ./nginx_cache "master process worker process fork" 10
#   ./ai_code_search.sh search ./nginx_cache "memory pool palloc" 10
#   # 原理：用架构关键词搜索，找到核心实现文件和函数
#   # 验证：查看返回的函数名、文件名、调用关系，确认是否为核心实现
#
# 【Phase 3: 调用关系分析 - 理解架构依赖】
#   ./ai_code_search.sh search ./nginx_cache "upstream load balancing" 10
#   # 关键发现：ngx_http_upstream_init_round_robin_peer 被多个算法调用
#   # 洞察：Round-robin 是所有负载均衡算法的基础
#
#   ./ai_code_search.sh callgraph ./nginx_cache
#   # 然后查看 call_graph.json，搜索某个函数的调用者列表
#   # 例如：ngx_spawn_process 被用于启动 worker、cache manager、回收死亡 worker
#
# 【Phase 4: 子系统深入 - 逐步细化】
#   # 内存管理
#   ./ai_code_search.sh search ./nginx_cache "shared memory zone slab" 10
#   ./ai_code_search.sh dataflow ./nginx_cache c
#   # 配置解析
#   ./ai_code_search.sh search ./nginx_cache "configuration parser lexer" 10
#   # SSL/TLS
#   ./ai_code_search.sh search ./nginx_cache "SSL certificate handshake" 10
#   # 缓存系统
#   ./ai_code_search.sh search ./nginx_cache "file cache open read" 10
#
# 【Phase 5: 代码片段搜索 - 找到相似实现】
#   ./ai_code_search.sh snippet ./nginx_cache ./my_epoll_code.cpp 5
#   # 原理：将你的代码片段向量化，搜索代码库中最相似的实现
#   # 用途：学习最佳实践、找到参考实现
#
# 【Phase 6: 变量数据流 - 追踪生命周期】
#   ./ai_code_search.sh dataflow ./nginx_cache c
#   # 追踪 connection 指针的定义、赋值、使用位置
#   # 理解变量在代码库中的流转路径
#
# 【搜索技巧】
# 1. 用英文自然语言描述你想找的功能（如 "event loop" 而非 "事件循环"）
# 2. 组合关键词提高精度（如 "memory allocation buffer pool"）
# 3. 查看调用关系（--callgraph）理解函数在架构中的角色
# 4. 用 --rich 查看完整代码上下文
# 5. 用 --kind function 只搜索函数，过滤变量和宏
#
# 【分析框架】
# 对每个代码库，建议按以下框架分析：
# 1. 核心架构（事件循环、进程模型、请求管线）
# 2. 内存管理（分配器、内存池、垃圾回收）
# 3. 配置系统（解析器、热加载、配置继承）
# 4. 模块系统（初始化、加载、钩子机制）
# 5. 网络处理（连接管理、协议实现、负载均衡）
# 6. 安全机制（认证、加密、访问控制）
# 7. 缓存策略（文件缓存、元数据缓存、缓存失效）
# 8. 日志系统（分级日志、格式化、输出目标）
# 9. 高级特性（重写引擎、正则表达式、新协议）
# 10. 源码质量评价（架构清晰度、可扩展性、性能优化）
#
# 【nginx 验证结果】
# 使用本系统分析 nginx 400 文件、9174 符号：
# - ✅ 事件循环：找到 epoll(Linux) + kqueue(BSD) 双平台实现
# - ✅ Phase Handler：发现 11 阶段 HTTP 请求处理管线
# - ✅ 内存池：请求级别分配（create → palloc → destroy）
# - ✅ Slab Allocator：共享内存用于 upstream 状态、rate limiting、SSL session
# - ✅ 负载均衡：Round-robin 是所有算法（least_conn/ip_hash/hash）的基础
# - ✅ 配置解析：词法分析 + 语法分析，支持 include 递归
# - ✅ 多进程：Master-Worker + Cache Manager，prefork 模型
#
set -euo pipefail

# ============================================
# 环境变量配置（TensorRT + cuDNN + CUDA，兼容本地和远程）
# ============================================
GPU_LIBS=""
[ -d "/home/dministrator/my_db" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/my_db"
[ -d "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" ] && GPU_LIBS="${GPU_LIBS}:$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -d "/opt/TensorRT-10/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
[ -d "/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib"
[ -d "/opt/cuda/lib64" ] && GPU_LIBS="${GPU_LIBS}:/opt/cuda/lib64"
[ -d "/home/dministrator/anaconda3/envs/dl/lib" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib"
[ -d "/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/tensorrt_libs" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/tensorrt_libs"
[ -d "/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/nvidia/cudnn/lib" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/nvidia/cudnn/lib"
GPU_LIBS="${GPU_LIBS#:}"
export LD_LIBRARY_PATH="${GPU_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

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
    info "Step 1/4: 统计词频..."
    "$WORD_FREQ" "$cache_dir" || warn "词频统计失败（非致命）"

    # Step 2: 生成语义向量
    info "Step 2/4: 生成语义向量（TensorRT GPU）..."
    "$BATCH_EMBEDDER" "$cache_dir" --model jina --name "$project_name"

    # Step 3: 构建调用关系图（含参数信息）
    info "Step 3/4: 构建调用关系图（含参数）..."
    "$CALL_GRAPH" "$cache_dir" || warn "调用图构建失败（非致命）"

    # Step 4: 生成变量数据流分析（含字段级+跨函数）
    info "Step 4/4: 生成变量数据流分析（字段级+跨函数）..."
    "$DATAFLOW" analyze "$cache_dir" || warn "数据流分析失败（非致命）"

    ok "分析完成!"
    info "输出文件:"
    info "  ${cache_dir}/vectors/code_local_${project_name}.jina.bin - 语义向量"
    info "  ${cache_dir}/vectors/code_local_${project_name}.jina.idx - 向量索引"
    info "  ${cache_dir}/call_graph.json - 调用关系图（含参数列表）"
    info "  ${cache_dir}/dataflow.json - 变量数据流（字段级+跨函数）"
    info "  ${cache_dir}/word_freq.json - 词频统计（TF-IDF）"
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

    ok "分析完成! 现在可以使用以下功能:"
    echo ""
    echo "  语义搜索:   $0 search ${cache_dir} \"<查询>\""
    echo "  变量追踪:   $0 dataflow ${cache_dir} <变量名>"
    echo ""
    echo "示例:"
    echo "  $0 search ${cache_dir} \"event loop epoll\" 10"
    echo "  $0 search ${cache_dir} \"memory pool allocation\" 5"
    echo "  $0 dataflow ${cache_dir} c       # 追踪 connection 变量"
    echo "  $0 dataflow ${cache_dir} pool    # 追踪内存池"
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
        echo "功能:"
        echo "  - 字段级追踪: 自动区分 var->field (如 c->fd, c->data)"
        echo "  - 跨函数流:   追踪变量在调用链中的传递路径"
        echo "  - 分类显示:   定义(DEF) / 赋值(SET) / 使用(USE)"
        echo ""
        echo "示例:"
        echo "  $0 dataflow ./nginx_cache c          # 追踪 connection 指针"
        echo "    输出: c->fd, c->data, c->ssl, c->sockaddr 等字段级分析"
        echo "    输出: 跨函数传递链 (哪些函数接收并使用了 c)"
        echo ""
        echo "  $0 dataflow ./nginx_cache pool       # 追踪内存池"
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
            echo "  dataflow <cache> <var>          - 变量数据流追踪 (字段级+跨函数)"
            echo "  analyze <repo> [cache]          - 一键完整分析"
            echo "  demo [cache]                    - 演示系统能力"
            echo ""
            echo "示例:"
            echo "  $0 index /opt/stable-diffusion.cpp ./sd_cache 4"
            echo "  $0 vector ./sd_cache stable-diffusion.cpp"
            echo "  $0 search ./sd_cache \"upscale image\" 5"
            echo "  $0 snippet ./sd_cache ./my_code.cpp 5"
            echo "  $0 dataflow ./nginx_cache c     # 追踪 connection (含字段级和跨函数流)"
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