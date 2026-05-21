#!/bin/bash
#
# =============================================================================
# explore_book.sh — AI Agent 电子书探索脚本
# =============================================================================
#
# 设计目标：
#   为 AI Agent 提供标准化的电子书查询接口。支持语义搜索（向量检索）和
#   页面内容读取，实现"搜索→定位→阅读"的完整工作流。
#
# 核心特性：
#   1. 语义搜索：基于 Jina v2 向量嵌入，自然语言查询
#   2. 页面读取：从 KV Cache 获取 md 文件路径，直接读取 Markdown 内容
#   3. 章节浏览：列出书籍目录结构和章节内容
#   4. 自动 GPU 环境配置（TensorRT + cuDNN + CUDA）
#
# 依赖：
#   - KV Cache 记忆系统（ai_code_memory/）
#   - tools/cache_query （C 实现，支持向量搜索）
#   - 电子书 Markdown 文件（由 import_book 生成）
#
# =============================================================================
# 快速开始（AI 使用指南）
# =============================================================================
#
# 基本用法：
#   ./explore_book.sh <namespace> <command> [options]
#
# namespace 是什么？
#   电子书在 KV Cache 中的命名空间，导入时自动生成：
#   - 默认：/books/{book_name}
#   - 自定义：导入时指定，如 /books/ddia
#
# =============================================================================
# 命令详解
# =============================================================================
#
# 1. overview — 书籍概览
#    查看书籍的基本信息：标题、作者、章节数、页数
#    用法：./explore_book.sh /books/my_book overview
#    适用：第一次接触新书，了解结构
#
# 2. search "<query>" — 语义搜索（核心功能）
#    用自然语言搜索书中内容，返回最相关的页面
#    用法：./explore_book.sh /books/my_book search "consensus algorithm"
#    适用：快速定位感兴趣的主题，不知道具体章节位置
#    参数：--max N  返回结果数量（默认5）
#    输出：每页显示预览、相关度分数、Markdown 文件路径
#
# 3. read <page_key> — 读取页面内容
#    读取指定页面的完整 Markdown 内容
#    用法：./explore_book.sh /books/my_book read page_0005
#    用法：./explore_book.sh /books/my_book read chapters/03-Consensus/page_0005
#    适用：通过搜索结果定位到具体页面后，阅读完整内容
#
# 4. chapter [name] — 章节浏览
#    列出所有章节，或查看某个章节的页面列表
#    用法：./explore_book.sh /books/my_book chapter
#    用法：./explore_book.sh /books/my_book chapter "03-Consensus"
#    适用：按章节浏览书籍结构
#
# 5. toc — 目录结构
#    显示书籍的完整目录树
#    用法：./explore_book.sh /books/my_book toc
#    适用：了解书籍整体组织架构
#
# =============================================================================
# 典型工作流（AI 阅读电子书）
# =============================================================================
#
# Step 1: 了解书籍概览
#   ./explore_book.sh /books/ddia overview
#
# Step 2: 语义搜索感兴趣的主题
#   ./explore_book.sh /books/ddia search "consensus algorithm"
#   → 返回相关页面列表，包含预览和 md 文件路径
#
# Step 3: 阅读具体内容
#   ./explore_book.sh /books/ddia read chapters/08-Distributed_Consensus/page_0012
#   → 输出完整的 Markdown 内容
#
# Step 4: 继续搜索或浏览
#   ./explore_book.sh /books/ddia search "Raft leader election"
#   ./explore_book.sh /books/ddia chapter "08-Distributed_Consensus"
#
# =============================================================================
# 使用示例
# =============================================================================
#
# 示例 1: 搜索分布式系统相关内容
#   ./explore_book.sh /books/ddia search "distributed transaction"
#   → 返回最相关的 5 个页面，包含文件路径
#
# 示例 2: 搜索并阅读
#   ./explore_book.sh /books/ddia search "CAP theorem"
#   # 看到结果：chapters/09-Consistency/page_0003.md
#   ./explore_book.sh /books/ddia read chapters/09-Consistency/page_0003
#
# 示例 3: 浏览章节
#   ./explore_book.sh /books/ddia chapter
#   → 列出所有章节
#   ./explore_book.sh /books/ddia chapter "02-Data_Models"
#   → 显示该章节的所有页面
#
# 示例 4: 查看目录
#   ./explore_book.sh /books/ddia toc
#   → 树形结构显示书籍目录
#
# =============================================================================
# 注意事项
# =============================================================================
#
# 1. 命名空间获取：
#    如果不确定 namespace，可以查看已导入的书籍：
#    strings ai_code_memory/cache.bin | grep "^/books/" | sort -u
#
# 2. 搜索依赖向量缓存：
#    语义搜索需要 import_book 成功生成向量。如果搜索无结果，
#    可能是向量生成失败（检查 GPU 环境）。
#
# 3. 读取路径格式：
#    read 命令支持两种格式：
#    - page_0005（自动查找所属章节）
#    - chapters/03-Consensus/page_0005（完整路径）
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_QUERY="${SCRIPT_DIR}/tools/cache_query"

# GPU 环境
export LD_LIBRARY_PATH="/home/dministrator/anaconda3/envs/dl/lib:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/tensorrt_libs:/home/dministrator/anaconda3/envs/dl/lib/python3.10/site-packages/nvidia/cudnn/lib:${LD_LIBRARY_PATH:-}"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${BLUE}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1" &&2; }
ok() { echo -e "${GREEN}[OK]${NC} $1"; }

# 检查工具
check_tool() {
    if [[ ! -f "$CACHE_QUERY" ]]; then
        error "cache_query not found at $CACHE_QUERY"
        error "Please build: make tools/cache_query"
        exit 1
    fi
}

# JSON 提取辅助函数
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
    echo "  overview              书籍概览"
    echo '  search "<query>"      语义搜索（向量检索）'
    echo "  read <page_key>       读取页面内容（Markdown）"
    echo "  chapter [name]        列出章节或查看章节页面"
    echo "  toc                   目录结构"
    echo ""
    echo "Examples:"
    echo '  $0 /books/ddia overview'
    echo '  $0 /books/ddia search "consensus algorithm"'
    echo '  $0 /books/ddia read chapters/08-Consensus/page_0012'
    echo '  $0 /books/ddia chapter'
    echo '  $0 /books/ddia toc'
    exit 1
fi

NAMESPACE="$1"
COMMAND="$2"
shift 2
EXTRA_ARGS="$@"

# =============================================================================
# 命令实现
# =============================================================================

cmd_overview() {
    log "Book Overview: $NAMESPACE"
    echo ""
    
    local meta_key="${NAMESPACE}/_meta"
    local result
    result=$("$CACHE_QUERY" "$meta_key" --type exact --pretty 2>&1 | extract_json)
    
    echo "$result" | python3 -c "
import sys, json
d = json.load(sys.stdin)
results = d.get('results', [])
if results:
    r = results[0]
    print(f'书名: {r.get(\"title\", \"N/A\")}')
    print(f'作者: {r.get(\"author\", \"N/A\")}')
    print(f'章节数: {r.get(\"chapters\", 0)}')
    print(f'总页数: {r.get(\"pages\", 0)}')
    print(f'命名空间: {r.get(\"_key\", \"\")}')
else:
    print('未找到书籍元数据')
"
    
    echo ""
    log "To explore, try:"
    echo "  $0 $NAMESPACE search \"your topic\""
    echo "  $0 $NAMESPACE toc"
}

cmd_search() {
    local query="${1:-}"
    if [[ -z "$query" ]]; then
        error 'Usage: $0 $NAMESPACE search "<query>" [--max N]'
        exit 1
    fi
    
    local max_results=5
    if [[ "$EXTRA_ARGS" == *"--max"* ]]; then
        max_results=$(echo "$EXTRA_ARGS" | sed -n 's/.*--max *\([0-9]*\).*/\1/p')
        [[ -z "$max_results" ]] && max_results=5
    fi
    
    log "Semantic search: \"$query\""
    echo ""
    
    # 从命名空间提取书籍名
    local book_name=$(basename "$NAMESPACE")
    local cache_dir="/opt/books/${book_name}"
    
    if [[ ! -d "$cache_dir" ]]; then
        warn "Book directory not found: $cache_dir"
        warn "The book may not have been imported correctly."
        exit 1
    fi
    
    # 执行语义搜索
    local search_result
    search_result=$("$CACHE_QUERY" "$query" --repo "$NAMESPACE" --type search --analysis-dir "$cache_dir" --max-results "$max_results" --pretty 2>&1 | extract_json)
    
    echo "$search_result" | python3 -c "
import sys, json
d = json.load(sys.stdin)
results = d.get('results', [])

if not results:
    print('未找到相关结果。')
    print('可能原因：')
    print('  1. 向量未生成（检查 import_book 是否成功）')
    print('  2. 查询词与书籍内容不匹配')
    print('  3. GPU 环境未配置（检查 LD_LIBRARY_PATH）')
    sys.exit(0)

print(f'找到 {len(results)} 个相关页面：\n')

for i, r in enumerate(results, 1):
    name = r.get('name', 'N/A')
    score = r.get('score', 0)
    file = r.get('file', '')
    
    # 尝试解析 KV value 获取 md_file 和 preview
    print(f'{i}. [{score:.3f}] {name}')
    print(f'   File: {file}')
    print()
"
    
    echo ""
    log "To read a page, use:"
    echo "  $0 $NAMESPACE read <page_key>"
    echo ""
    log "Example:"
    echo "  $0 $NAMESPACE read chapters/01-Intro/page_0000"
}

cmd_read() {
    local page_key="${1:-}"
    if [[ -z "$page_key" ]]; then
        error "Usage: $0 $NAMESPACE read <page_key>"
        error "Examples:"
        error "  $0 $NAMESPACE read page_0005"
        error "  $0 $NAMESPACE read chapters/03-Consensus/page_0005"
        exit 1
    fi
    
    # 构建完整的 KV key
    local full_key
    if [[ "$page_key" == */* ]]; then
        # 完整路径：chapters/xx-xxx/page_xxxx
        full_key="${NAMESPACE}/${page_key}"
    else
        # 只有 page_key，需要搜索定位
        full_key="${NAMESPACE}/${page_key}"
    fi
    
    log "Reading: $page_key"
    echo ""
    
    # 查询 KV Cache 获取 md_file 路径
    local result
    result=$("$CACHE_QUERY" "$full_key" --type exact --pretty 2>&1 | extract_json)
    
    local md_file
    md_file=$(echo "$result" | python3 -c 'import sys,json; d=json.load(sys.stdin); r=d.get("results",[]); print(r[0].get("md_file","")) if r else ""' 2>/dev/null)
    
    # 如果上面的方法失败，直接用 grep
    if [[ -z "$md_file" ]]; then
        md_file=$(echo "$result" | grep -oP '"md_file":"\K[^"]+' || true)
    fi
    
    if [[ -z "$md_file" ]]; then
        # 尝试直接构建路径
        local book_name=$(basename "$NAMESPACE")
        md_file="/opt/books/${book_name}/${page_key}.md"
    fi
    
    if [[ ! -f "$md_file" ]]; then
        error "Markdown file not found: $md_file"
        error "The page may not exist or the book was not imported correctly."
        exit 1
    fi
    
    # 输出页面元数据
    echo "---"
    echo "Page: $page_key"
    echo "File: $md_file"
    echo "---"
    echo ""
    
    # 输出 Markdown 内容
    cat "$md_file"
}

cmd_chapter() {
    local chapter_name="${1:-}"
    
    if [[ -z "$chapter_name" ]]; then
        # 列出所有章节
        log "Chapters in $NAMESPACE:"
        echo ""
        
        # 从 KV Cache 查询章节列表
        local meta_key="${NAMESPACE}/_meta"
        local result
        result=$("$CACHE_QUERY" "$meta_key" --type exact --pretty 2>&1 | extract_json)
        
        echo "$result" | python3 -c "
import sys, json
d = json.load(sys.stdin)
results = d.get('results', [])
if results:
    r = results[0]
    chapters = r.get('chapters', 0)
    print(f'总章节数: {chapters}')
    print()
" 2>/dev/null
        
        # 扫描目录结构
        local book_name=$(basename "$NAMESPACE")
        local chapters_dir="/opt/books/${book_name}/chapters"
        
        if [[ -d "$chapters_dir" ]]; then
            echo "章节列表："
            ls -1 "$chapters_dir" | while read ch; do
                local page_count=$(ls -1 "$chapters_dir/$ch"/page_*.md 2>/dev/null | wc -l)
                echo "  $ch ($page_count pages)"
            done
        else
            warn "Chapters directory not found: $chapters_dir"
        fi
    else
        # 显示特定章节的页面
        log "Chapter: $chapter_name"
        echo ""
        
        local book_name=$(basename "$NAMESPACE")
        local chapter_dir="/opt/books/${book_name}/chapters/${chapter_name}"
        
        if [[ ! -d "$chapter_dir" ]]; then
            error "Chapter not found: $chapter_dir"
            exit 1
        fi
        
        echo "Pages in $chapter_name:"
        local pages=$(ls -1 "$chapter_dir"/page_*.md 2>/dev/null | sort)
        if [[ -n "$pages" ]]; then
            echo "$pages" | while read page_file; do
                local page_name=$(basename "$page_file" .md)
                # 读取第一行作为预览
                local preview=$(head -1 "$page_file" | sed 's/<!--.*-->//g' | tr -d '\n' | cut -c1-80)
                echo "  $page_name - $preview..."
            done
        else
            echo "  No pages found"
        fi
    fi
}

cmd_toc() {
    log "Table of Contents: $NAMESPACE"
    echo ""
    
    local book_name=$(basename "$NAMESPACE")
    local book_dir="/opt/books/${book_name}"
    
    if [[ ! -d "$book_dir" ]]; then
        error "Book directory not found: $book_dir"
        exit 1
    fi
    
    echo "$book_name/"
    
    local chapters_dir="${book_dir}/chapters"
    if [[ -d "$chapters_dir" ]]; then
        local chapters=$(ls -1 "$chapters_dir" | sort)
        local total_chapters=$(echo "$chapters" | wc -l)
        local idx=0
        
        echo "$chapters" | while read ch; do
            idx=$((idx + 1))
            local page_count=$(ls -1 "$chapters_dir/$ch"/page_*.md 2>/dev/null | wc -l)
            
            if [[ $idx -eq $total_chapters ]]; then
                echo "└── $ch/ ($page_count pages)"
            else
                echo "├── $ch/ ($page_count pages)"
            fi
            
            # 显示该章节的前几个页面
            local pages=$(ls -1 "$chapters_dir/$ch"/page_*.md 2>/dev/null | sort | head -3)
            if [[ -n "$pages" ]]; then
                local page_idx=0
                local page_total=$(echo "$pages" | wc -l)
                echo "$pages" | while read page_file; do
                    page_idx=$((page_idx + 1))
                    local page_name=$(basename "$page_file" .md)
                    if [[ $idx -eq $total_chapters ]]; then
                        if [[ $page_idx -eq $page_total ]]; then
                            echo "    └── $page_name.md"
                        else
                            echo "    ├── $page_name.md"
                        fi
                    else
                        if [[ $page_idx -eq $page_total ]]; then
                            echo "│   └── $page_name.md"
                        else
                            echo "│   ├── $page_name.md"
                        fi
                    fi
                done
            fi
        done
    fi
    
    echo ""
    log "Total chapters: $(ls -1 "$chapters_dir" 2>/dev/null | wc -l)"
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
        search|s|q)
            cmd_search "$1"
            ;;
        read|r)
            cmd_read "$1"
            ;;
        chapter|ch|c)
            cmd_chapter "${1:-}"
            ;;
        toc|t)
            cmd_toc
            ;;
        help|--help|-h)
            echo "Usage: $0 <namespace> <command> [options]"
            echo ""
            echo "Commands:"
            echo "  overview              书籍概览"
            echo '  search "<query>"      语义搜索（向量检索）'
            echo "  read <page_key>       读取页面内容（Markdown）"
            echo "  chapter [name]        列出章节或查看章节页面"
            echo "  toc                   目录结构"
            echo ""
            echo "Examples:"
            echo '  $0 /books/ddia overview'
            echo '  $0 /books/ddia search "consensus algorithm"'
            echo '  $0 /books/ddia read chapters/08-Consensus/page_0012'
            echo '  $0 /books/ddia chapter'
            echo '  $0 /books/ddia toc'
            ;;
        *)
            error "Unknown command: $COMMAND"
            error "Run '$0 <namespace> help' for usage"
            exit 1
            ;;
    esac
}

main "$@"
