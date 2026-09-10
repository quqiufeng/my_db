#!/bin/bash
# build.sh — 编译 ocrdec 为独立 ELF 二进制（StaticPy → Chez AOT → ELF）
#
# ocrdec 是 Baidu Unlimited-OCR 文本解码器的 StaticPy 重构：用 StaticPy 语言
# （/opt/ReScheme 自研 Python 子集）表达 12 层 MoE 解码器 + KV cache，经
# static_translate.py → Chez Scheme → libTorch 桥 → gcc，产出独立 ELF。
#
# 产物只依赖 libtorch_std_helper.so + torch 运行时，不依赖 Python。
#
# 用法:
#   bash ocrdec/build.sh               # 编译（增量）
#   bash ocrdec/build.sh --force       # 清缓存强制全量重编
#   bash ocrdec/build.sh --run         # 编译 + 运行（需已配置输入图片）
#   bash ocrdec/build.sh clean         # 删除产物
#
# 运行环境（脚本内自动导出）:
#   LD_LIBRARY_PATH 需要包含 torch 运行时目录（libtorch.so 等）
#   权重 /data/models/baidu/weights.tpack
#   视觉 /data/models/baidu/vision_pipeline.pt

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# StaticPy 工具链所在目录（static_translate.py / static_prelude.scm /
# static_stdlib.scm / libtorch_std_helper.so 都在这里）。本目录（ocrdec）
# 只放 ocr_kv.py 源码，编译时引用 /opt/ReScheme 的工具链。
RESCHEME_DIR="${RESCHEME_DIR:-/opt/ReScheme}"

# 输入源与产物
SOURCE="$SCRIPT_DIR/ocr_kv.py"        # StaticPy 源（生产入口）
STEM="ocr_kv"                          # 输出名
OUT="$SCRIPT_DIR/$STEM"               # 最终 ELF
CACHE_DIR="/tmp/staticpy-cache"

# libTorch 运行时路径（helper .so 的 rpath + 运行 LD_LIBRARY_PATH）
TORCH_DIR="/data/venv/lib/python3.12/site-packages/torch/lib"
HELPER="$RESCHEME_DIR/libtorch_std_helper.so"   # libTorch C API 桥
HELPER_BUILD="$RESCHEME_DIR/build_torch_std_helper.sh"

# ── 颜色/日志 ──
GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; RED='\033[0;31m'; NC='\033[0m'
info() { echo -e "${BLUE}[INFO]${NC} $1"; }
ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
err()  { echo -e "${RED}[ERROR]${NC} $1"; }

# ── 参数 ──
FORCE=""; DO_RUN=""
while [ $# -gt 0 ]; do
    case "$1" in
        --force) FORCE="1"; shift ;;
        --run)   DO_RUN="1"; shift ;;
        clean)   rm -f "$OUT" "$OUT.so"; rm -rf "$CACHE_DIR"; ok "cleaned"; exit 0 ;;
        *) err "Unknown: $1"; exit 1 ;;
    esac
done

# ── 前置检查 ──
[ -f "$SOURCE" ] || { err "缺少源码: $SOURCE"; exit 1; }
[ -f "$RESCHEME_DIR/static_build.sh" ] || { err "缺少 static_build.sh"; exit 1; }
[ -f "$HELPER_BUILD" ] || { err "缺少 $HELPER_BUILD"; exit 1; }

# ── Step 0: 编译 libTorch 桥（ABI 自动检测，见 build_torch_std_helper.sh）──
if [ "$FORCE" = "1" ] || [ ! -f "$HELPER" ]; then
    info "编译 libtorch_std_helper.so ..."
    (cd "$RESCHEME_DIR" && bash build_torch_std_helper.sh)
    ok "libtorch_std_helper.so 就绪"
else
    info "libtorch_std_helper.so 已存在（--force 强制重编）"
fi

# ── Step 1: StaticPy 源码 → ELF ──
if [ "$FORCE" = "1" ]; then
    rm -rf "$CACHE_DIR"
    info "已清缓存（--force）"
fi

info "编译 $STEM ..."
(cd "$RESCHEME_DIR" && bash static_build.sh "$SOURCE" "$STEM")
# static_build.sh 把产物 cp 到 ReScheme 根目录（$RESCHEME_DIR/$STEM），拷回本目录
# ELF launcher 会按 argv[0] 同目录找 <name>.so（Chez AOT Scheme），必须一起拷
if [ -f "$RESCHEME_DIR/$STEM" ]; then
    cp -f "$RESCHEME_DIR/$STEM" "$OUT"
    chmod +x "$OUT"
    cp -f "$RESCHEME_DIR/$STEM.so" "$OUT.so" 2>/dev/null || true
    rm -f "$RESCHEME_DIR/$STEM" "$RESCHEME_DIR/$STEM.so"
fi
[ -x "$OUT" ] || { err "编译产物未生成: $OUT"; exit 1; }
ok "ELF 已生成: $OUT"

# ── Step 2: 运行（可选）──
if [ "$DO_RUN" = "1" ]; then
    export LD_LIBRARY_PATH="$TORCH_DIR:$SCRIPT_DIR:$RESCHEME_DIR:${LD_LIBRARY_PATH:-}"
    info "运行 $STEM（torch: $TORCH_DIR）..."
    "$OUT" "$@"
fi
