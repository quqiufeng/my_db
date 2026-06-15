#!/bin/bash
#
# =============================================================================
# analyze_repo.sh — 代码分析主控脚本（Code Analysis Master Orchestrator）
# =============================================================================
#
# 功能描述:
#   将任意代码仓库（GitHub URL 或本地路径）一键转换为可查询的 KV Cache 记忆。
#   支持完整的 6 步流水线：源码获取 → 索引 → 向量化 → 调用图 → 数据流 → 记忆存储。
#
# 适用场景:
#   - 将开源项目（nginx、Redis、Linux 等）导入记忆系统
#   - 构建"全人类编码记忆"的知识库
#   - 为 AI Agent 提供可查询的代码知识
#
# 依赖要求:
#   - 编译好的 C 工具链：code_indexer、batch_embedder、cache_import
#   - GPU 环境：TensorRT + cuDNN + CUDA（ONNX Runtime GPU 推理）
#   - 系统命令：git、wc、nproc
#   - KV Cache 目录：/memory/（自动创建）
#
# =============================================================================
# 使用方法
# =============================================================================
#
# 基本用法:
#   ./analyze_repo.sh <source> [namespace] [options]
#
# 参数说明:
#   source      (必填)  GitHub URL 或本地路径
#   namespace   (可选)  记忆命名空间，默认自动检测
#
# 命名空间自动检测规则:
#   - GitHub URL:  https://github.com/owner/repo → /code/owner/repo
#   - 本地路径:    /opt/nginx → /code/local/nginx
#
# 选项说明:
#   --skip-vectors        跳过向量生成（更快，但无语义搜索能力）
#   --skip-callgraph      跳过调用图分析
#   --skip-dataflow       跳过数据流分析
#   --cache-dir <dir>     KV Cache 目录（默认: /memory）
#   --jobs <n>            并行工作进程数（默认: CPU 核心数）
#   --name <name>         项目名（默认: 从命名空间提取）
#
# 使用示例:
#   # 分析 GitHub 仓库（自动克隆）
#   ./analyze_repo.sh https://github.com/redis/redis
#
#   # 分析本地项目
#   ./analyze_repo.sh /opt/nginx
#
#   # 指定命名空间
#   ./analyze_repo.sh /opt/nginx /code/nginx
#
#   # 快速分析（跳过 GPU 向量生成）
#   ./analyze_repo.sh /opt/redis --skip-vectors
#
#   # 仅索引+调用图（适合快速浏览代码结构）
#   ./analyze_repo.sh /opt/sqlite --skip-vectors --skip-dataflow
#
# =============================================================================
# 内部流水线（6 步）
# =============================================================================
#
# Step 1: 获取源码
#   - GitHub URL: git clone --depth 1 到临时目录
#   - 本地路径: 直接使用，不复制
#   - 临时目录在脚本结束时自动清理
#
# Step 2: 索引代码 (code_indexer.c)
#   - C 多进程并行解析（ctags + AST）
#   - 输入: 源码目录
#   - 输出:
#     - chunks_text.txt: 代码内容（一行一个 chunk）
#     - chunks_meta.jsonl: 元数据（函数名/文件/行号/签名/语言）
#   - 性能: 每秒 1000+ 文件（6 核并行）
#
# Step 3: 生成向量 (batch_embedder.c)
#   - TensorRT GPU 加速语义编码
#   - 模型: Jina v2 code embedding (768 维 float32)
#   - 性能: 98-114 items/s (RTX 3080)
#   - 输出:
#     - vectors/code_local_{project}.jina.bin: 二进制向量文件
#     - vectors/code_local_{project}.jina.idx: 名称→偏移索引
#   - 后台自动 fork HNSW 索引构建进程
#
# Step 4: 调用图分析 (call_graph)
#   - 基于函数名匹配构建 caller/callee 关系
#   - 输出: call_graph.json (JSON 格式)
#   - 包含: 调用点、参数列表、文件位置
#
# Step 5: 数据流分析 (dataflow)
#   - 字段级变量追踪
#   - 跨函数数据流分析
#   - 输出: dataflow.json
#
# Step 6: 导入 KV Cache (cache_import.c)
#   - 将所有分析结果写入 mmap 持久化存储
#   - 层级命名空间: /code/{project}/functions/{name}
#   - 实时落盘（msync MS_SYNC），断电不丢
#   - 自动构建符号索引和调用关系索引
#
# =============================================================================
# 输出数据结构
# =============================================================================
#
# 分析目录: ./{project}_cache/
#   ├── chunks_text.txt              # 代码内容（文本）
#   ├── chunks_meta.jsonl            # 元数据（JSON Lines）
#   ├── call_graph.json              # 调用关系图
#   ├── dataflow.json                # 数据流分析
#   └── vectors/
#       ├── code_local_{project}.jina.bin       # 语义向量
#       ├── code_local_{project}.jina.idx       # 向量索引
#       └── code_local_{project}.jina.bin.hnsw  # HNSW 近似索引
#
# KV Cache 目录: /memory/
#   ├── cache.bin    # mmap 数据文件（零拷贝持久化）
#   └── index.bin    # 索引文件（Hash + Skip List）
#
# 内存中的 Key 格式:
#   /code/{project}/chunks/{file}/{function}   # 代码块
#   /code/{project}/symbols/{name}             # 符号索引
#   /code/{project}/callers/{function}         # 调用者列表
#   /code/{project}/callees/{function}         # 被调用者列表
#   /code/{project}/_meta/info                 # 项目元数据
#
# =============================================================================
# 查询方法（分析完成后）
# =============================================================================
#
# 符号上下文查询（含 caller/callee）:
#   ./tools/cache_query <function> --repo /code/local/redis --type context --depth 2
#
# 语义搜索（自然语言）:
#   ./tools/cache_query "memory allocation" --repo /code/local/redis --type search
#
# 精确查找:
#   ./tools/cache_query /code/local/redis/symbols/zmalloc --type exact
#
# =============================================================================

set -euo pipefail

# 脚本所在目录（用于查找 tools/ 子目录）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# =============================================================================
# GPU 环境设置
# =============================================================================
# 脚本自动配置 TensorRT + cuDNN + CUDA 库路径（兼容本地和远程）
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

# =============================================================================
# 输出颜色定义（用于美化日志）
# =============================================================================
RED='\033[0;31m'      # 错误
GREEN='\033[0;32m'    # 成功
YELLOW='\033[1;33m'   # 警告
BLUE='\033[0;34m'     # 信息
NC='\033[0m'          # 重置颜色

# =============================================================================
# 日志函数
# =============================================================================

# 普通信息日志，带时间戳
log() {
    echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $1"
}

# 警告日志（黄色，不退出）
warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

# 错误日志（红色，输出到 stderr）
error() {
    echo -e "${RED}[ERROR]${NC} $1" >&2
}

# 成功日志（绿色）
success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

# =============================================================================
# 参数解析
# =============================================================================

# 初始化变量（带默认值）
SOURCE=""                           # 源码来源（必填）
NAMESPACE=""                        # 命名空间（可选，自动检测）
SKIP_VECTORS=false                  # 是否跳过向量生成
SKIP_CALLGRAPH=false                # 是否跳过调用图
SKIP_DATAFLOW=false                 # 是否跳过数据流
CACHE_DIR="/memory"        # KV Cache 目录
JOBS=""                             # 并行进程数（默认 auto）
PROJECT_NAME=""                     # 项目名（用于目录命名）
EXCLUDE_DIRS=""
PLUGINS=""                          # 插件注册表路径

# 解析命令行参数
while [[ $# -gt 0 ]]; do
    case $1 in
        --skip-vectors)
            SKIP_VECTORS=true
            shift
            ;;
        --skip-callgraph)
            SKIP_CALLGRAPH=true
            shift
            ;;
        --skip-dataflow)
            SKIP_DATAFLOW=true
            shift
            ;;
        --cache-dir)
            CACHE_DIR="$2"
            shift 2
            ;;
        --jobs)
            JOBS="$2"
            shift 2
            ;;
        --name)
            PROJECT_NAME="$2"
            shift 2
            ;;
        --exclude-dir)
            EXCLUDE_DIRS="$2"
            shift 2
            ;;
        --plugins)
            PLUGINS="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 <source> [namespace] [options]"
            echo ""
            echo "Arguments:"
            echo "  source     GitHub URL or local path"
            echo "  namespace  Target namespace (default: auto-detect)"
            echo ""
            echo "Options:"
            echo "  --skip-vectors     Skip vector generation"
            echo "  --skip-callgraph   Skip call graph analysis"
            echo "  --skip-dataflow    Skip dataflow analysis"
            echo "  --cache-dir <dir>  KV Cache directory"
            echo "  --jobs <n>         Parallel jobs"
            echo "  --name <name>      Project name"
            echo "  --exclude-dir <dirs>  Comma-separated dirs to skip"
            echo "  --plugins <path>   Plugin registry JSON for code_indexer"
            echo ""
            echo "Examples:"
            echo "  $0 https://github.com/redis/redis"
            echo "  $0 /opt/nginx /code/nginx"
            echo "  $0 /opt/redis --skip-vectors"
            exit 0
            ;;
        -*)
            error "Unknown option: $1"
            exit 1
            ;;
        *)
            # 位置参数：第一个是 source，第二个是 namespace
            if [[ -z "$SOURCE" ]]; then
                SOURCE="$1"
            elif [[ -z "$NAMESPACE" ]]; then
                NAMESPACE="$1"
            else
                error "Too many arguments"
                exit 1
            fi
            shift
            ;;
    esac
done

# 检查必填参数
if [[ -z "$SOURCE" ]]; then
    error "No source specified"
    echo "Usage: $0 <source> [namespace] [options]"
    exit 1
fi

# =============================================================================
# 自动检测配置
# =============================================================================

# 自动检测命名空间
# 规则:
#   GitHub URL  -> /code/owner/repo
#   本地路径    -> /code/local/basename
if [[ -z "$NAMESPACE" ]]; then
    if [[ "$SOURCE" =~ ^https://github.com/([^/]+)/([^/]+) ]]; then
        OWNER="${BASH_REMATCH[1]}"
        REPO="${BASH_REMATCH[2]}"
        NAMESPACE="/code/$OWNER/$REPO"
    else
        REPO=$(basename "$SOURCE")
        NAMESPACE="/code/local/$REPO"
    fi
fi

# 自动检测项目名（用于创建分析目录）
if [[ -z "$PROJECT_NAME" ]]; then
    PROJECT_NAME=$(basename "$NAMESPACE")
fi

# 自动检测并行进程数（默认使用所有 CPU 核心）
if [[ -z "$JOBS" ]]; then
    JOBS=$(nproc)
fi

# 分析输出目录（临时数据，可删除）
ANALYSIS_DIR="/opt/code_caches/${PROJECT_NAME}_cache"

# =============================================================================
# 打印配置信息
# =============================================================================
log "================================================================"
log "Analyzing repository: $SOURCE"
log "Namespace: $NAMESPACE"
log "Analysis directory: $ANALYSIS_DIR"
log "Cache directory: $CACHE_DIR"
log "Jobs: $JOBS"
log "Skip vectors: $SKIP_VECTORS"
log "Skip callgraph: $SKIP_CALLGRAPH"
log "Skip dataflow: $SKIP_DATAFLOW"
log "Exclude dirs: ${EXCLUDE_DIRS:-none}"
log "Plugins: ${PLUGINS:-none}"
log "================================================================"

# =============================================================================
# Step 1: 获取源码
# =============================================================================
# 如果输入是 URL，clone 到临时目录
# 如果输入是本地路径，直接使用
# 临时目录在脚本结束时自动清理

SOURCE_DIR=""       # 实际源码目录
CLEANUP_DIR=""      # 需要清理的临时目录

if [[ "$SOURCE" =~ ^https?:// ]]; then
    log "Step 1: Cloning repository..."
    SOURCE_DIR=$(mktemp -d)
    CLEANUP_DIR="$SOURCE_DIR"
    
    if ! git clone --depth 1 "$SOURCE" "$SOURCE_DIR" 2>&1; then
        error "Failed to clone repository"
        rm -rf "$CLEANUP_DIR"
        exit 1
    fi
    
    success "Cloned to $SOURCE_DIR"
else
    # 本地路径验证
    if [[ ! -d "$SOURCE" ]]; then
        error "Source directory does not exist: $SOURCE"
        exit 1
    fi
    SOURCE_DIR="$SOURCE"
    success "Using local directory: $SOURCE_DIR"
fi

# =============================================================================
# Step 2: 索引代码
# =============================================================================
# 使用 code_indexer.c 进行多进程并行索引
# 输出:
#   - chunks_text.txt: 代码内容文本
#   - chunks_meta.jsonl: 元数据（JSON Lines 格式）
# 每个 chunk 包含: 函数名、文件路径、起止行、签名、语言类型

log "Step 2: Indexing code..."

# 查找 code_indexer 工具（优先 tools/ 子目录）
if [[ -f "$SCRIPT_DIR/tools/code_indexer" ]]; then
    INDEXER="$SCRIPT_DIR/tools/code_indexer"
elif [[ -f "$SCRIPT_DIR/code_indexer" ]]; then
    INDEXER="$SCRIPT_DIR/code_indexer"
else
    error "code_indexer not found. Build: make tools/code_indexer"
    exit 1
fi

# 执行索引（容错：非零退出继续执行）
EXCLUDE_ARG=""
if [ -n "$EXCLUDE_DIRS" ]; then
    EXCLUDE_ARG="--exclude-dir $EXCLUDE_DIRS"
fi
PLUGIN_ARG=""
if [ -n "$PLUGINS" ]; then
    PLUGIN_ARG="--plugins $PLUGINS"
fi
if ! "$INDEXER" "$SOURCE_DIR" "$ANALYSIS_DIR" "$JOBS" $EXCLUDE_ARG $PLUGIN_ARG 2>&1; then
    warn "Indexer returned non-zero, continuing..."
fi

# 验证输出文件
if [[ ! -f "$ANALYSIS_DIR/chunks_meta.jsonl" ]]; then
    error "Indexing failed: chunks_meta.jsonl not created"
    rm -rf "$CLEANUP_DIR"
    exit 1
fi

CHUNK_COUNT=$(wc -l < "$ANALYSIS_DIR/chunks_meta.jsonl")
success "Indexed $CHUNK_COUNT chunks"

# =============================================================================
# Step 3: 生成语义向量（可选，需要 GPU）
# =============================================================================
# 使用 batch_embedder.c + TensorRT GPU 生成代码语义向量
# 模型: Jina v2 code embedding (768 维)
# 性能: RTX 3080 约 100 items/s
# 输出:
#   - vectors/code_local_{project}.jina.bin: 二进制向量
#   - vectors/code_local_{project}.jina.idx: 名称索引
# 后台自动 fork HNSW 索引构建进程（加速语义搜索）

if [[ "$SKIP_VECTORS" == false ]]; then
    log "Step 3: Generating vectors..."
    
    # 查找 batch_embedder 工具
    if [[ -f "$SCRIPT_DIR/tools/batch_embedder" ]]; then
        EMBEDDER="$SCRIPT_DIR/tools/batch_embedder"
    elif [[ -f "$SCRIPT_DIR/batch_embedder" ]]; then
        EMBEDDER="$SCRIPT_DIR/batch_embedder"
    else
        warn "batch_embedder not found, skipping vectors"
        SKIP_VECTORS=true
    fi
    
    # 执行向量生成
    if [[ "$SKIP_VECTORS" == false ]]; then
        if ! "$EMBEDDER" "$ANALYSIS_DIR" --model jina --name "$PROJECT_NAME" 2>&1; then
            warn "Vector generation failed, continuing..."
            SKIP_VECTORS=true
        else
            success "Vectors generated"
        fi
    fi
else
    log "Step 3: Skipping vector generation"
fi

# =============================================================================
# Step 4: 调用图分析（可选）
# =============================================================================
# 分析函数间的调用关系（caller/callee）
# 基于 chunks_meta.jsonl 中的函数名进行文本匹配
# 输出: call_graph.json
# 格式: { "function_name": { "calls": [{"function": "...", "file": "...", "line": N}] } }

if [[ "$SKIP_CALLGRAPH" == false ]]; then
    log "Step 4: Analyzing call graph..."
    
    # 查找 call_graph 工具
    if [[ -f "$SCRIPT_DIR/tools/call_graph" ]]; then
        CALLGRAPH="$SCRIPT_DIR/tools/call_graph"
    elif [[ -f "$SCRIPT_DIR/call_graph" ]]; then
        CALLGRAPH="$SCRIPT_DIR/call_graph"
    else
        warn "call_graph tool not found, skipping"
        SKIP_CALLGRAPH=true
    fi
    
    # 执行调用图分析
    if [[ "$SKIP_CALLGRAPH" == false ]]; then
        if ! "$CALLGRAPH" "$ANALYSIS_DIR" 2>&1; then
            warn "Call graph analysis failed, continuing..."
        else
            # 统计调用边数量
            if [[ -f "$ANALYSIS_DIR/call_graph.json" ]]; then
                EDGE_COUNT=$(grep -c '"calls"' "$ANALYSIS_DIR/call_graph.json" 2>/dev/null || echo "0")
                success "Call graph analyzed ($EDGE_COUNT edges)"
            fi
        fi
    fi
else
    log "Step 4: Skipping call graph analysis"
fi

# =============================================================================
# Step 5: 数据流分析（可选）
# =============================================================================
# 追踪变量在代码中的流动路径（字段级 + 跨函数）
# 识别：变量定义、赋值、使用、传递
# 输出: dataflow.json

if [[ "$SKIP_DATAFLOW" == false ]]; then
    log "Step 5: Analyzing dataflow..."
    
    # 查找 dataflow 工具
    if [[ -f "$SCRIPT_DIR/tools/dataflow" ]]; then
        DATAFLOW="$SCRIPT_DIR/tools/dataflow"
    elif [[ -f "$SCRIPT_DIR/dataflow" ]]; then
        DATAFLOW="$SCRIPT_DIR/dataflow"
    else
        warn "dataflow tool not found, skipping"
        SKIP_DATAFLOW=true
    fi
    
    # 执行数据流分析
    if [[ "$SKIP_DATAFLOW" == false ]]; then
        if ! "$DATAFLOW" analyze "$ANALYSIS_DIR" 2>&1; then
            warn "Dataflow analysis failed, continuing..."
        else
            success "Dataflow analyzed"
        fi
    fi
else
    log "Step 5: Skipping dataflow analysis"
fi

# =============================================================================
# Step 6: 导入 KV Cache 记忆系统
# =============================================================================
# 将所有分析结果（chunks、symbols、call_graph、dataflow）导入持久化存储
# 使用 cache_import.c 实现，基于 mmap 零拷贝技术
# 特点:
#   - 实时落盘：每次写入后 msync(MS_SYNC)，断电不丢
#   - 层级命名空间：/code/{project}/{category}/{name}
#   - 自动索引：导入时构建符号索引和调用关系索引
#   - 增量友好：支持多次导入同一项目（更新已有条目）

log "Step 6: Importing to KV Cache..."

# 查找 cache_import 工具
if [[ -f "$SCRIPT_DIR/tools/cache_import" ]]; then
    IMPORTER="$SCRIPT_DIR/tools/cache_import"
elif [[ -f "$SCRIPT_DIR/cache_import" ]]; then
    IMPORTER="$SCRIPT_DIR/cache_import"
else
    error "cache_import not found. Please build: make tools/cache_import"
    rm -rf "$CLEANUP_DIR"
    exit 1
fi

# 确保导入器能找到 libmydb.so（添加到库搜索路径）
export LD_LIBRARY_PATH="$SCRIPT_DIR:${LD_LIBRARY_PATH:-}"

# 执行导入（如果失败则清理临时目录并退出）
if ! "$IMPORTER" "$ANALYSIS_DIR" "$NAMESPACE" --cache-dir "$CACHE_DIR" 2>&1; then
    error "Failed to import to KV Cache"
    rm -rf "$CLEANUP_DIR"
    exit 1
fi

success "Imported to KV Cache"

# =============================================================================
# 清理与总结
# =============================================================================

# 清理临时目录（GitHub clone 时创建）
if [[ -n "$CLEANUP_DIR" ]]; then
    log "Cleaning up temporary files..."
    rm -rf "$CLEANUP_DIR"
fi

# 打印分析完成摘要
log "================================================================"
success "Analysis complete!"
log "Project: $PROJECT_NAME"
log "Namespace: $NAMESPACE"
log "Cache: $CACHE_DIR"
log "Chunks: $CHUNK_COUNT"
log "Vectors: $([ "$SKIP_VECTORS" == true ] && echo "skipped" || echo "generated")"
log "Call graph: $([ "$SKIP_CALLGRAPH" == true ] && echo "skipped" || echo "analyzed")"
log "Dataflow: $([ "$SKIP_DATAFLOW" == true ] && echo "skipped" || echo "analyzed")"
log ""
log "Query examples:"
log "  ./tools/cache_query <symbol> --repo $NAMESPACE --type context"
log "  ./tools/cache_query \"search query\" --repo $NAMESPACE --type search"
log "  ./tools/cache_query <symbol> --repo $NAMESPACE --type context --depth 2"
log "================================================================"

# =============================================================================
# 脚本结束
# =============================================================================
