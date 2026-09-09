#!/bin/bash
#
# =============================================================================
# book2audio.sh — 单个电子书文件一键转有声书
# =============================================================================
#
# 自动完成两步流程：
#   1. import_book  解析电子书 → Markdown 章节（跳过向量生成）
#   2. book2audio   Markdown 章节 → WAV → m4b 有声书
#
# 使用方法：
#   ./tools/book2audio.sh <book_file> [选项]
#
# 示例：
#   ./tools/book2audio.sh ~/book.epub
#   ./tools/book2audio.sh ~/book.mobi -v af_sky -s 1.2
#   ./tools/book2audio.sh ~/book.pdf -o /data/audio/my_book
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
IMPORT_BOOK="${SCRIPT_DIR}/import_book"
BOOK2AUDIO="${SCRIPT_DIR}/book2audio"
CACHE_DIR="${BOOK_CACHE_DIR:-/book/cache}"
MD_ROOT="${BOOK_MD_ROOT:-/opt/books}"
DEFAULT_OUT_ROOT="${BOOK_OUT_ROOT:-/opt/audio}"

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

usage() {
    cat <<EOF
$(basename "$0") — 电子书一键转有声书（EPUB/MOBI/AZW3/PDF → m4b）

用法:
  $(basename "$0") <book_file> [选项]

选项:
  -o DIR       音频输出目录（默认 ${DEFAULT_OUT_ROOT}/{书名}）
  -v VOICE     音色，54 种可选（默认 af_sky）
               美式女声: af_sky af_heart af_bella ...  美式男声: am_michael am_adam ...
               英式: bf_emma bm_george ...  中文音色: zf_xiaoyi zm_yunjian ...（需 --phonemes 前端）
  -s SPEED     语速 0.5-2.0（默认 1.0）
  -l LANG      espeak 语言（默认 en-us）
  --force      即使 Markdown 已存在也重新导入
  --cpu        强制 CPU 推理（默认 CUDA，失败自动回退）
  --no-m4b     只生成 WAV，不打包 m4b
  -h           显示帮助

示例:
  $(basename "$0") ~/book.epub
  $(basename "$0") "~/硅谷钢铁侠.azw3" -v af_sky
  $(basename "$0") ~/ddia.epub -o /data/audio/ddia -s 1.3

输出:
  {输出目录}/{书名}.m4b   ← 带章节导航，VLC 等可直接播放
  {输出目录}/*.wav        ← 每章 WAV（24kHz 16bit）

环境变量:
  BOOK_CACHE_DIR  KV Cache 目录（默认 /book/cache）
  BOOK_MD_ROOT    Markdown 中间产物目录（默认 /opt/books）
  BOOK_OUT_ROOT   默认音频输出根目录（默认 /opt/audio）

EOF
}

# =============================================================================
# 参数解析
# =============================================================================
BOOK_FILE=""
OUT_DIR=""
VOICE="af_sky"
SPEED="1.0"
LANG="en-us"
FORCE=0
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o) OUT_DIR="$2"; shift 2 ;;
        -v) VOICE="$2"; shift 2 ;;
        -s) SPEED="$2"; shift 2 ;;
        -l) LANG="$2"; shift 2 ;;
        --force) FORCE=1; shift ;;
        --cpu) EXTRA_ARGS+=("--cpu"); shift ;;
        --phonemes) EXTRA_ARGS+=("--phonemes"); shift ;;
        --no-m4b) EXTRA_ARGS+=("--no-m4b"); shift ;;
        -h|--help) usage; exit 0 ;;
        -*) err "未知选项: $1"; usage; exit 1 ;;
        *)
            if [[ -z "$BOOK_FILE" ]]; then
                BOOK_FILE="$1"
            else
                err "多余的参数: $1"; usage; exit 1
            fi
            shift ;;
    esac
done

if [[ -z "$BOOK_FILE" ]]; then
    usage
    exit 1
fi

# =============================================================================
# 前置检查
# =============================================================================
if [[ ! -f "$BOOK_FILE" ]]; then
    err "文件不存在: $BOOK_FILE"
    exit 1
fi

EXT="${BOOK_FILE##*.}"
EXT_LOWER=$(echo "$EXT" | tr '[:upper:]' '[:lower:]')
if [[ ! "$EXT_LOWER" =~ ^(epub|mobi|azw|azw3|pdf)$ ]]; then
    err "不支持的格式: .$EXT（支持 epub/mobi/azw/azw3/pdf）"
    exit 1
fi

if [[ ! -x "$IMPORT_BOOK" ]]; then
    err "import_book 未编译，请先: make tools/import_book"
    exit 1
fi
if [[ ! -x "$BOOK2AUDIO" ]]; then
    err "book2audio 未编译，请先: make tools/book2audio"
    exit 1
fi
for cmd in espeak-ng ffmpeg; do
    if ! command -v "$cmd" &>/dev/null; then
        err "缺少依赖: $cmd（sudo apt install $cmd）"
        exit 1
    fi
done

# =============================================================================
# 推导书名（与 import_book 命名空间规则一致：文件名去扩展名，空格转下划线）
# =============================================================================
BASENAME=$(basename "$BOOK_FILE")
BOOK_NAME="${BASENAME%.*}"
BOOK_NAME="${BOOK_NAME// /_}"
NAMESPACE="/books/${BOOK_NAME}"
MD_DIR="${MD_ROOT}/${BOOK_NAME}"

if [[ -z "$OUT_DIR" ]]; then
    OUT_DIR="${DEFAULT_OUT_ROOT}/${BOOK_NAME}"
fi

info "书名: ${BOOK_NAME}"
info "输出: ${OUT_DIR}"

# =============================================================================
# Step 1: 电子书 → Markdown（已导入则跳过）
# =============================================================================
if [[ -d "${MD_DIR}/chapters" && $FORCE -eq 0 ]]; then
    ok "Markdown 已存在，跳过导入: ${MD_DIR}"
    info "（如需重新导入，加 --force）"
else
    info "Step 1/2: 解析电子书 → Markdown ..."
    # import_book 以相对路径 dlopen wrapper 库（src/importer/libs/*.so），必须在项目根目录运行
    (cd "$PROJECT_ROOT" && "$IMPORT_BOOK" "$CACHE_DIR" "$BOOK_FILE" "$NAMESPACE" "$MD_ROOT" --skip-vectors)
    if [[ ! -d "${MD_DIR}/chapters" ]]; then
        err "导入后未找到章节目录: ${MD_DIR}/chapters"
        err "请检查上方 import_book 输出的实际书名"
        exit 1
    fi
    ok "Markdown 已生成: ${MD_DIR}"
fi

# =============================================================================
# Step 2: Markdown → 有声书
# =============================================================================
info "Step 2/2: 语音合成（音色 ${VOICE}，语速 ${SPEED}，语言 ${LANG}）..."
"$BOOK2AUDIO" book -i "$MD_DIR" -o "$OUT_DIR" -v "$VOICE" -s "$SPEED" -l "$LANG" "${EXTRA_ARGS[@]}"

M4B="${OUT_DIR}/${BOOK_NAME}.m4b"
if [[ -f "$M4B" ]]; then
    SIZE=$(du -h "$M4B" | cut -f1)
    DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$M4B" 2>/dev/null || echo "0")
    ok "有声书已生成: ${M4B}"
    ok "大小 ${SIZE}，时长 $(( ${DUR%.*} / 60 )) 分钟"
else
    warn "未生成 m4b（可能使用了 --no-m4b），WAV 位于: ${OUT_DIR}"
fi
