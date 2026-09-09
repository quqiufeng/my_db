#!/bin/bash
#
# test_ebooks.sh — 电子书导入黄金样本回归测试
#
# 每种格式导入后校验：
#   1. 章节目录数量符合预期
#   2. 总页数 > 0
#   3. 关键内容存在（防止过滤器静默丢内容，如问题 30 的 CSS 误判）
#
# 真实书籍不存在时自动跳过（SKIP），内置 mini.epub 保证 EPUB 用例始终运行。
#
# 用法: ./tests/test_ebooks.sh
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
IMPORT_BOOK="$ROOT/tools/import_book"
GOLDEN="$SCRIPT_DIR/golden"
TMP=$(mktemp -d /tmp/test_ebooks_XXXXXX)
trap "rm -rf $TMP" EXIT

PASS=0; FAIL=0; SKIP=0

# run_case <name> <book_file> <min_chapter_dirs> <keyword>
run_case() {
    local name="$1" book="$2" min_dirs="$3" keyword="$4"

    if [[ ! -f "$book" ]]; then
        echo "SKIP  $name (样本不存在: $book)"
        SKIP=$((SKIP + 1))
        return
    fi

    local cache="$TMP/cache_$name" out="$TMP/books_$name"
    mkdir -p "$cache"

    (cd "$ROOT" && ./tools/import_book "$cache" "$book" "/books/$name" "$out" --skip-vectors) > "$TMP/log_$name.txt" 2>&1
    if [[ $? -ne 0 ]]; then
        echo "FAIL  $name (import_book 退出码非 0，见 $TMP/log_$name.txt)"
        FAIL=$((FAIL + 1))
        return
    fi

    local chdir="$out/$name/chapters"
    local dirs=0 pages=0
    [[ -d "$chdir" ]] && dirs=$(ls "$chdir" | wc -l)
    [[ -d "$chdir" ]] && pages=$(find "$chdir" -name "page_*.md" | wc -l)

    if [[ $dirs -lt $min_dirs ]]; then
        echo "FAIL  $name (章节目录 $dirs < 预期 $min_dirs)"
        FAIL=$((FAIL + 1))
        return
    fi
    if [[ $pages -eq 0 ]]; then
        echo "FAIL  $name (没有生成任何页面)"
        FAIL=$((FAIL + 1))
        return
    fi
    if ! grep -rqF "$keyword" "$chdir" 2>/dev/null; then
        echo "FAIL  $name (关键词 '$keyword' 未在正文中找到——内容可能被过滤器误删)"
        FAIL=$((FAIL + 1))
        return
    fi

    echo "PASS  $name (章节 $dirs, 页面 $pages)"
    PASS=$((PASS + 1))
}

echo "=== 电子书导入回归测试 ==="

# 内置黄金样本（始终运行）
run_case mini_epub "$GOLDEN/mini.epub" 2 "clocks were striking thirteen"

# 真实书籍（存在才运行）
BOOKS_DIR="$HOME/Downloads/book"
run_case mobi "$BOOKS_DIR/耶路撒冷三千年.mobi" 200 "耶路撒冷"
run_case azw3 "$BOOKS_DIR/A01075. Linux内核API完全参考手册.B009WMAZ02.azw3" 300 "kmalloc"
run_case epub "$BOOKS_DIR/Web.Development.with.Django.6.3rd.2026.3.epub" 20 "Chapter 1: Introduction to Django"
run_case pdf_algorithms "$BOOKS_DIR/Algorithms-JeffE.pdf" 100 "Subset Sum"
run_case pdf_ddia "$HOME/designing-data-intensive-applications.pdf" 15 "Consensus"

echo ""
echo "=== 结果: PASS=$PASS FAIL=$FAIL SKIP=$SKIP ==="
[[ $FAIL -eq 0 ]]
