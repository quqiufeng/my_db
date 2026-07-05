#!/bin/bash
#
# =============================================================================
# import_book.sh — 电子书导入脚本
# =============================================================================
#
# 设计目标：
#   一键导入 EPUB/MOBI/AZW3/PDF 电子书到 KV Cache 记忆系统。
#   自动配置 GPU 环境变量，支持自定义输出目录和命名空间。
#
# 使用方法：
#   ./import_book.sh <cache_dir> <book_file> [namespace] [output_dir]
#
# 示例：
#   ./import_book.sh /memory ~/book.epub
#   ./import_book.sh /memory ~/book.epub /books/ddia
#   ./import_book.sh /memory ~/book.epub /books/ddia /data/books
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMPORT_BOOK="${SCRIPT_DIR}/tools/import_book"

# =============================================================================
# GPU 环境设置（兼容本地和远程服务器）
# =============================================================================
# 动态构建 LD_LIBRARY_PATH：如果目录存在则加入
GPU_LIBS=""

# 项目目录（脚本所在位置）
GPU_LIBS="${GPU_LIBS}:${SCRIPT_DIR}"

# TensorRT
if [ -d "/opt/TensorRT-10/lib" ]; then
    GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
fi

# cuDNN
if [ -d "/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib" ]; then
    GPU_LIBS="${GPU_LIBS}:/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib"
fi

# CUDA
if [ -d "/opt/cuda/lib64" ]; then
    GPU_LIBS="${GPU_LIBS}:/opt/cuda/lib64"
fi

# 本地 anaconda 环境（向后兼容）
if [ -d "/data/venv/lib" ]; then
    GPU_LIBS="${GPU_LIBS}:/data/venv/lib"
fi
if [ -d "/opt/TensorRT-10/lib" ]; then
    GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
fi
if [ -d "/data/venv/lib/python3.12/site-packages/nvidia/cudnn/lib" ]; then
    GPU_LIBS="${GPU_LIBS}:/data/venv/lib/python3.12/site-packages/nvidia/cudnn/lib"
fi

# 去除开头的冒号
GPU_LIBS="${GPU_LIBS#:}"

export LD_LIBRARY_PATH="${GPU_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# =============================================================================
# 颜色输出
# =============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

info() { echo -e "${BLUE}[INFO]${NC} $1"; }
ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
err()  { echo -e "${RED}[ERROR]${NC} $1"; }

# =============================================================================
# 帮助信息
# =============================================================================

usage() {
    cat <<EOF
$(basename "$0") — 导入电子书到记忆系统

用法:
  $(basename "$0") <cache_dir> <book_file> [namespace] [output_dir]

参数:
  cache_dir    KV Cache 目录 (如 /memory)
  book_file    电子书文件 (.epub, .mobi, .azw3, .pdf)
  namespace    命名空间 (可选，默认 /books/{book_name})
  output_dir   Markdown 输出目录 (可选，默认 /opt/books)

示例:
  # 使用默认设置
  $(basename "$0") /memory ~/book.epub
  $(basename "$0") /memory ~/book.epub /books/ddia
  $(basename "$0") /memory ~/book.epub /books/ddia /data/books

输出结构:
  {output_dir}/{book_name}/
    ├── _meta.json
    └── chapters/
        └── 01-Chapter_Title/
            └── page_0000.md

EOF
}

# =============================================================================
# 主函数
# =============================================================================

if [[ $# -lt 2 ]]; then
    usage
    exit 1
fi

# 检查工具是否存在
if [[ ! -x "$IMPORT_BOOK" ]]; then
    err "import_book 工具未找到或未编译"
    echo "请先编译: make tools/import_book"
    exit 1
fi

# 执行导入
info "开始导入电子书..."
"$IMPORT_BOOK" "$@"

ok "导入完成"
