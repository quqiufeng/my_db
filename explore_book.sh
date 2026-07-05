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
#   5. 兼容新旧导入格式（有/无 chapters/ 子目录）
#
# 依赖：
#   - KV Cache 记忆系统（/book/cache/）
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
#    查看书籍的基本信息：标题、作者、章节数、总页数
#    用法：./explore_book.sh /books/my_book overview
#    适用：第一次接触新书，快速了解整体结构
#
# 2. search "<query>" — 语义搜索（核心功能）
#    用自然语言搜索书中内容，返回最相关的页面
#    用法：./explore_book.sh /books/my_book search "consensus algorithm"
#    用法：./explore_book.sh /books/my_book search "SpaceX" --max 10
#    适用：快速定位感兴趣的主题，无需知道具体章节位置
#    参数：--max N  返回结果数量（默认5）
#    输出：每页显示 [相关度分数] 页面路径 和可直接执行的 read 命令
#    注意：搜索结果中的 Read: 提示可以直接复制执行
#
# 3. read <page_key> — 读取页面内容
#    读取指定页面的完整 Markdown 内容
#    用法（推荐）：./explore_book.sh /books/my_book read chapters/03-Consensus/page_0005
#    用法（旧格式）：./explore_book.sh /books/my_book read 03-Consensus/page_0005
#    用法（简写）：./explore_book.sh /books/my_book read page_0005
#    适用：通过搜索结果定位到具体页面后，阅读完整内容
#    注意：脚本会自动兼容新旧导入格式（有无 chapters/ 前缀）
#
# 4. chapter [name] — 章节浏览
#    列出所有章节，或查看某个章节的页面列表
#    用法：./explore_book.sh /books/my_book chapter
#    用法：./explore_book.sh /books/my_book chapter "03-Consensus"
#    适用：按章节浏览书籍结构
#
# 5. toc — 目录结构
#    显示书籍的完整目录树（章节名 + 页面数 + 示例页面）
#    用法：./explore_book.sh /books/my_book toc
#    适用：了解书籍整体组织架构，查找特定章节
#    注意：支持 Unicode 章节名（中文、特殊符号等）
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
#   → 返回相关页面列表，包含预览和可直接执行的 read 命令
#
# Step 3: 阅读具体内容（直接复制搜索结果的 Read: 提示）
#   ./explore_book.sh /books/ddia read chapters/08-Distributed_Consensus/page_0012
#   → 输出完整的 Markdown 内容
#
# Step 4: 继续搜索或浏览
#   ./explore_book.sh /books/ddia search "Raft leader election"
#   ./explore_book.sh /books/ddia toc
#   ./explore_book.sh /books/ddia chapter "08-Distributed_Consensus"
#
# =============================================================================
# 使用示例
# =============================================================================
#
# 示例 1: 搜索并直接阅读
#   ./explore_book.sh /books/ddia search "distributed transaction"
#   # 看到结果中的 Read: 提示，直接复制执行：
#   ./explore_book.sh /books/ddia read chapters/07-Transactions/page_0005
#
# 示例 2: 查看目录后按章节阅读
#   ./explore_book.sh /books/elon_musk toc
#   ./explore_book.sh /books/elon_musk read chapters/12-SpaceX/page_0000
#
# 示例 3: 跨书搜索（不指定 --repo）
#   ./tools/cache_query "concurrency" --type search --analysis-dir /book/cache --cache-dir /book/cache --max-results 10
#   → 同时在所有已导入书籍中搜索
#
# 示例 4: 浏览章节
#   ./explore_book.sh /books/ddia chapter
#   → 列出所有章节
#   ./explore_book.sh /books/ddia chapter "02-Data_Models"
#   → 显示该章节的所有页面
#
# =============================================================================
# 注意事项
# =============================================================================
#
# 1. 命名空间获取：
#    如果不确定 namespace，可以查看已导入的书籍：
#    strings /book/cache/cache.bin | grep "^/books/" | sort -u
#
# 2. 搜索依赖向量缓存：
#    语义搜索需要 import_book 成功生成向量文件（vectors/*.jina.bin.hnsw）。
#    如果搜索无结果，可能是向量未生成（检查 GPU 环境和 import_book 输出）。
#
# 3. 读取路径格式：
#    read 命令支持多种格式，脚本会自动处理：
#    - 新格式（推荐）：chapters/03-Consensus/page_0005
#    - 旧格式：03-Consensus/page_0005
#    - 简写：page_0005（自动查找所属章节）
#
# 4. Unicode 章节名：
#    中文书籍的章节名可能包含 Unicode 字符（如 U+FFFD 替换字符）。
#    toc 命令使用 find -print0 处理，不会出错，但显示可能为 "�"。
#
# 5. GPU 环境：
#    脚本自动配置 LD_LIBRARY_PATH 加载 TensorRT 和 cuDNN。
#    如果看到 "CUDA not available, falling back to CPU"，搜索会变慢但仍可用。
#
# 6. 向量文件位置：
#    向量索引文件位于 /book/cache/vectors/books_{name}.jina.bin.hnsw
#    搜索时 --analysis-dir 指向 /memory/ 目录
#
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_QUERY="${SCRIPT_DIR}/tools/cache_query"

# GPU 环境（兼容本地和远程）
GPU_LIBS=""
[ -d "${SCRIPT_DIR}" ] && GPU_LIBS="${GPU_LIBS}:${SCRIPT_DIR}"
[ -d "/opt/TensorRT-10/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
[ -d "/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib"
[ -d "/opt/cuda/lib64" ] && GPU_LIBS="${GPU_LIBS}:/opt/cuda/lib64"
[ -d "/data/venv/lib" ] && GPU_LIBS="${GPU_LIBS}:/data/venv/lib"
[ -d "/opt/TensorRT-10/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
[ -d "/data/venv/lib/python3.12/site-packages/nvidia/cudnn/lib" ] && GPU_LIBS="${GPU_LIBS}:/data/venv/lib/python3.12/site-packages/nvidia/cudnn/lib"
GPU_LIBS="${GPU_LIBS#:}"
export LD_LIBRARY_PATH="${GPU_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${BLUE}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }
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
    echo "  list                  列出所有已导入书籍"
    echo "  overview              书籍概览"
    echo '  search "<query>"      语义搜索（向量检索，带内容预览）'
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
    result=$("$CACHE_QUERY" "$meta_key" --type exact --cache-dir /book/cache --pretty 2>&1 | extract_json)
    
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
    local md_dir="/opt/books/${book_name}"
    
    if [[ ! -d "$md_dir" ]]; then
        warn "Book directory not found: $md_dir"
        warn "The book may not have been imported correctly."
        exit 1
    fi
    
    # 执行语义搜索（向量索引在 /memory/vectors/ 中）
    local search_result
    search_result=$("$CACHE_QUERY" "$query" --repo "$NAMESPACE" --type search --analysis-dir /book/cache --cache-dir /book/cache --max-results "$max_results" --pretty 2>&1 | extract_json)
    
    echo "$search_result" | python3 -c '
import sys, json
d = json.load(sys.stdin)
results = d.get("results", [])

if not results:
    print("未找到相关结果。")
    print("可能原因：")
    print("  1. 向量未生成（检查 import_book 是否成功）")
    print("  2. 查询词与书籍内容不匹配")
    print("  3. GPU 环境未配置（检查 LD_LIBRARY_PATH）")
    sys.exit(0)

print("找到 " + str(len(results)) + " 个相关页面：\n")

for i, r in enumerate(results, 1):
    name = r.get("name", "N/A")
    score = r.get("score", 0)
    page_key = name
    ns_prefix = "/books/"
    if name.startswith(ns_prefix):
        parts = name.split("/", 3)
        if len(parts) >= 4:
            page_key = parts[3] if parts[3].startswith("chapters/") else "chapters/" + parts[3]
    parts = name.split("/")
    book_ns = "/books/" + parts[2] if len(parts) > 2 else "/books/unknown"
    preview = ""
    try:
        import subprocess, json as j
        cache_query_path = "/opt/my_db/tools/cache_query"
        kv_result = subprocess.run(
            [cache_query_path, name, "--type", "exact", "--cache-dir", "/book/cache", "--pretty"],
            capture_output=True, text=True, timeout=5
        )
        out = kv_result.stdout
        s = out.find("{")
        e = out.rfind("}")
        if s >= 0 and e > s:
            kv_data = j.loads(out[s:e+1])
            for res in kv_data.get("results", []):
                c = res.get("content", {})
                if isinstance(c, dict):
                    preview = c.get("preview", "")
                elif isinstance(c, str):
                    preview = c[:120]
    except:
        pass
    print(str(i) + ". [" + str(round(score, 3)) + "] " + name)
    if preview:
        p = preview
        for esc in ["\\n", "\\r", "\\t", "\\\""]:
            p = p.replace(esc, " ")
        if len(p) > 120:
            p = p[:120] + "..."
        p = p.strip()
        if p:
            print("   Preview: " + p)
    print("   Read: ./explore_book.sh " + book_ns + " read " + page_key)
    print()
'
    
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
    
    # 处理 --prev / --next 翻页选项
    local direction=""
    if [[ "$page_key" == "--prev" || "$page_key" == "-p" ]]; then
        direction="prev"
        page_key="${2:-}"
        if [[ -z "$page_key" ]]; then
            error "Usage: $0 $NAMESPACE read --prev <page_key>"
            exit 1
        fi
    elif [[ "$page_key" == "--next" || "$page_key" == "-n" ]]; then
        direction="next"
        page_key="${2:-}"
        if [[ -z "$page_key" ]]; then
            error "Usage: $0 $NAMESPACE read --next <page_key>"
            exit 1
        fi
    fi
    
    # 如果有翻页方向，计算目标页面
    if [[ -n "$direction" ]]; then
        local book_name=$(basename "$NAMESPACE")
        local book_dir="/opt/books/${book_name}"
        
        # 解析当前 chapter 和 page 编号
        local current_ch=""
        local current_page=""
        if [[ "$page_key" == chapters/* ]]; then
            current_ch="$(dirname "${page_key#chapters/}")"
            current_page="$(basename "$page_key")"
        else
            current_page="$page_key"
            # 查找 page 属于哪个 chapter（优先用 KV Cache，回退到 find）
            local search_key="${NAMESPACE}/${current_page}"
            local kv_found=$("$CACHE_QUERY" "$search_key" --type exact --cache-dir /book/cache --pretty 2>/dev/null | grep -c '"type":"page"')
            if [[ "$kv_found" -gt 0 ]]; then
                local kv_val=$("$CACHE_QUERY" "$search_key" --type exact --cache-dir /book/cache --pretty 2>/dev/null)
                local md_file=$(echo "$kv_val" | grep -oP '"md_file":"[^"]*"' | head -1 | sed 's/"md_file":"//;s/"//g')
                if [[ -n "$md_file" ]]; then
                    current_ch=$(basename "$(dirname "$md_file")")
                fi
            else
                # Fallback: find-based search
                local ch_dirs=()
                while IFS= read -r -d '' line; do
                    ch_dirs+=("$line")
                done < <(find "${book_dir}/chapters" -maxdepth 1 -mindepth 1 -type d -print0 2>/dev/null | sort -z)
                for ch_path in "${ch_dirs[@]}"; do
                    if [[ -f "${ch_path}/${current_page}.md" ]]; then
                        current_ch=$(basename "$ch_path")
                        break
                    fi
                done
            fi
        fi
        
        # 提取数字编号
        local page_num=$(echo "$current_page" | sed 's/page_//')
        local page_num_int=$((10#$page_num))
        
        local target_page_num
        local target_ch="$current_ch"
        if [[ "$direction" == "next" ]]; then
            target_page_num=$((page_num_int + 1))
        else
            target_page_num=$((page_num_int - 1))
        fi
        
        local target_page=$(printf "page_%04d" $target_page_num)
        local target_file="${book_dir}/chapters/${target_ch}/${target_page}.md"
        
        # 如果同章节没有目标页，尝试相邻章节
        if [[ ! -f "$target_file" ]]; then
            local ch_dirs=()
            while IFS= read -r -d '' line; do
                ch_dirs+=("$line")
            done < <(find "${book_dir}/chapters" -maxdepth 1 -mindepth 1 -type d -print0 2>/dev/null | sort -z)
            
            local ch_idx=-1
            local ch_count=${#ch_dirs[@]}
            for i in "${!ch_dirs[@]}"; do
                if [[ "$(basename "${ch_dirs[$i]}")" == "$current_ch" ]]; then
                    ch_idx=$i
                    break
                fi
            done
            
            if [[ $ch_idx -ge 0 ]]; then
                if [[ "$direction" == "next" && $((ch_idx + 1)) -lt $ch_count ]]; then
                    target_ch=$(basename "${ch_dirs[$((ch_idx + 1))]}")
                    target_page="page_0000"
                    target_file="${book_dir}/chapters/${target_ch}/${target_page}.md"
                elif [[ "$direction" == "prev" && $((ch_idx - 1)) -ge 0 ]]; then
                    target_ch=$(basename "${ch_dirs[$((ch_idx - 1))]}")
                    # 找到上一章的最后一页
                    local last_page=$(ls -1 "${book_dir}/chapters/${target_ch}"/page_*.md 2>/dev/null | sort | tail -1 | xargs basename 2>/dev/null)
                    target_page="${last_page%.md}"
                    target_file="${book_dir}/chapters/${target_ch}/${target_page}.md"
                fi
            fi
        fi
        
        if [[ ! -f "$target_file" ]]; then
            error "No $direction page from $page_key"
            exit 1
        fi
        
        page_key="chapters/${target_ch}/${target_page}"
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
    result=$("$CACHE_QUERY" "$full_key" --type exact --cache-dir /book/cache --pretty 2>&1 | extract_json)
    
    local md_file
    md_file=$(echo "$result" | grep -oP '"md_file":"[^"]*"' | head -1 | sed 's/"md_file":"//;s/"//g')
    
    # 如果上面的方法失败，直接用 grep
    if [[ -z "$md_file" ]]; then
        md_file=$(echo "$result" | grep -oP '"md_file":"\K[^"]+' || true)
    fi
    
    # 兼容旧格式：相对路径转换为绝对路径
    if [[ "$md_file" == ./* ]]; then
        md_file="/opt${md_file#.}"
    fi
    
    if [[ -z "$md_file" ]]; then
        # 尝试直接构建路径
        local book_name=$(basename "$NAMESPACE")
        md_file="/opt/books/${book_name}/${page_key}.md"
        # 兼容旧格式：如果没有 chapters/ 子目录，去掉 chapters/ 前缀
        if [[ ! -f "$md_file" ]] && [[ "$page_key" == chapters/* ]]; then
            local alt_key="${page_key#chapters/}"
            local alt_file="/opt/books/${book_name}/${alt_key}.md"
            if [[ -f "$alt_file" ]]; then
                md_file="$alt_file"
            fi
        fi
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
    
    # 输出翻页提示
    echo ""
    echo "---"
    echo "Navigation:"
    echo "  Prev: $0 $NAMESPACE read --prev $page_key"
    echo "  Next: $0 $NAMESPACE read --next $page_key"
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
        
        echo "$result" | python3 << 'HEREDOC_PYTHON'
import sys, json
d = json.load(sys.stdin)
results = d.get('results', [])
if results:
    r = results[0]
    chapters = r.get('chapters', 0)
    print('总章节数: ' + str(chapters))
    print()
HEREDOC_PYTHON
        2>/dev/null
        
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

cmd_list_books() {
    log "Listing all imported books..."
    echo ""
    
    local count=0
    # 从 cache.bin 中扫描所有 /books/ 开头的 _meta 条目
    if command -v strings &>/dev/null && [[ -f "/memory/cache.bin" ]]; then
        # 直接读取 markdown 输出目录
        if [[ -d "/opt/books" ]]; then
            echo "Books in /opt/books/:"
            echo ""
            for book_dir in /opt/books/*/; do
                local book_name=$(basename "$book_dir")
                local meta_file="${book_dir}_meta.json"
                local title=""
                local author=""
                local pages=0
                local chapters=0
                
                if [[ -f "$meta_file" ]]; then
                    title=$(python3 -c "import json; d=json.load(open('$meta_file')); print(d.get('title',''))" 2>/dev/null)
                    author=$(python3 -c "import json; d=json.load(open('$meta_file')); print(d.get('author',''))" 2>/dev/null)
                    pages=$(python3 -c "import json; d=json.load(open('$meta_file')); print(d.get('pages',0))" 2>/dev/null)
                    chapters=$(python3 -c "import json; d=json.load(open('$meta_file')); print(d.get('chapters',0))" 2>/dev/null)
                else
                    # 统计实际页面数
                    chapters=$(find "$book_dir" -maxdepth 2 -type d 2>/dev/null | wc -l)
                    chapters=$((chapters - 1))
                    pages=$(find "$book_dir" -name "page_*.md" 2>/dev/null | wc -l)
                fi
                
                if [[ -z "$title" ]]; then
                    title="$book_name"
                fi
                if [[ -z "$author" ]]; then
                    author="-"
                fi
                
                count=$((count + 1))
                printf "  %-30s %-20s %4d pages  %3d chapters\n" "$title" "$author" "$pages" "$chapters"
                echo "    explore_book.sh /books/${book_name} overview"
                echo ""
            done
        fi
        
        # 也从 KV Cache 列出
        local cache_books=$(strings /book/cache/cache.bin 2>/dev/null | grep "^/books/" | grep "/_meta\$" | sort -u | head -20)
        if [[ -n "$cache_books" ]]; then
            echo ""
            log "Books in KV Cache:"
            echo "$cache_books" | head -10
        fi
    else
        # Fallback: 直接列目录
        if [[ -d "/opt/books" ]]; then
            ls -1 "/opt/books/"
        else
            warn "No book directory found at /opt/books/"
        fi
    fi
    
    if [[ $count -eq 0 ]]; then
        warn "No books found"
        log "To import a book:"
        echo "  ./tools/import_book /memory <book_file>"
    fi
}

cmd_delete_book() {
    local book_name=$(basename "$NAMESPACE")
    log "Deleting book: $book_name (namespace: $NAMESPACE)"
    echo ""
    
    # 1. Remove from KV Cache
    echo "  Removing from KV Cache..."
    local count=0
    local keys=$(strings /book/cache/cache.bin 2>/dev/null | grep "^$NAMESPACE" | sort -u)
    for key in $keys; do
        "$CACHE_QUERY" "$key" --type delete --cache-dir /book/cache --pretty 2>/dev/null
        count=$((count + 1))
    done
    echo "  Removed $count KV Cache entries"
    
    # 2. Remove markdown files
    if [[ -d "/opt/books/$book_name" ]]; then
        echo "  Removing Markdown directory: /opt/books/$book_name/"
        rm -rf "/opt/books/$book_name"
    fi
    
    # 3. Remove vector files (HNSW index)
    local vec_base="/book/cache/vectors"
    if [[ -d "$vec_base" ]]; then
        for f in "$vec_base"/books_*.jina.bin "$vec_base"/books_*.jina.idx "$vec_base"/books_*.jina.bin.hnsw; do
            if [[ -f "$f" ]]; then
                rm -f "$f"
            fi
        done 2>/dev/null
        echo "  Removed vector files"
    fi
    
    echo ""
    ok "Book deleted: $book_name"
    log "To verify: ./explore_book.sh list"
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
    
    # 优先使用 chapters/ 子目录，兼容旧格式（直接在 book_dir 下）
    local chapters_dir="${book_dir}/chapters"
    local search_dir="$chapters_dir"
    if [[ ! -d "$chapters_dir" ]]; then
        search_dir="$book_dir"
    fi
    
    if [[ -d "$search_dir" ]]; then
        # 使用 find -print0 + read -d '' 避免任何换行符问题
        local ch_dirs=()
        while IFS= read -r -d '' line; do
            ch_dirs+=("$line")
        done < <(find "$search_dir" -maxdepth 1 -mindepth 1 -type d -print0 | sort -z)
        
        local total_chapters=${#ch_dirs[@]}
        local idx=0
        
        for ch_path in "${ch_dirs[@]}"; do
            local ch=$(basename "$ch_path")
            idx=$((idx + 1))
            local page_count=$(find "$ch_path" -maxdepth 1 -name "page_*.md" -type f -print0 2>/dev/null | tr -cd '\0' | wc -c)
            
            if [[ $idx -eq $total_chapters ]]; then
                echo "└── $ch/ ($page_count pages)"
            else
                echo "├── $ch/ ($page_count pages)"
            fi
            
            # 显示该章节的前几个页面
            local page_files=()
            while IFS= read -r -d '' line; do
                page_files+=("$line")
            done < <(find "$ch_path" -maxdepth 1 -name "page_*.md" -type f -print0 | sort -z | head -z -n 3)
            
            local page_total=${#page_files[@]}
            if [[ $page_total -gt 0 ]]; then
                local page_idx=0
                for page_file in "${page_files[@]}"; do
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
    else
        echo "No chapters found"
    fi
    
    echo ""
    log "Total chapters: $(ls -1 "$search_dir" 2>/dev/null | wc -l)"
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
            cmd_read "$@"
            ;;
        chapter|ch|c)
            cmd_chapter "${1:-}"
            ;;
        toc|t)
            cmd_toc
            ;;
        list|ls|l)
            cmd_list_books
            ;;
        delete|remove|rm)
            cmd_delete_book
            ;;
        help|--help|-h)
            echo "Usage: $0 <namespace> <command> [options]"
            echo ""
            echo "Commands:"
            echo "  list                  列出所有已导入书籍"
            echo "  overview              书籍概览"
            echo '  search "<query>"      语义搜索（向量检索，带内容预览）'
            echo "  read <page_key>       读取页面内容（Markdown）"
            echo "  chapter [name]        列出章节或查看章节页面"
            echo "  toc                   目录结构"
            echo "  delete                从 Cache 中删除本书"
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
