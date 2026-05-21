#!/bin/bash
#
# AI Agent 超大项目语义搜索系统 - 分治策略
# ===========================================
#
# 针对 Linux 内核等超大项目（5万+文件），按子系统分治处理：
# - 自动分析项目结构，智能拆分子系统
# - 每个子系统独立索引和生成向量
# - 支持全局跨子系统搜索
# - 保留子系统间调用关系
#
# 使用方式:
#   ./ai_code_search_large.sh init <repo_path> [config]    # 初始化（自动分析结构）
#   ./ai_code_search_large.sh index [workers]              # 索引所有子系统
#   ./ai_code_search_large.sh vector [workers]             # 生成所有子系统向量
#   ./ai_code_search_large.sh search <query> [n]           # 全局搜索所有子系统
#   ./ai_code_search_large.sh search-sub <sub> <query> [n] # 搜索指定子系统
#   ./ai_code_search_large.sh dataflow <var> [sub]         # 全局变量追踪
#   ./ai_code_search_large.sh status                       # 查看处理状态
#
# 示例:
#   ./ai_code_search_large.sh init /opt/linux
#   ./ai_code_search_large.sh index 8
#   ./ai_code_search_large.sh vector 2
#   ./ai_code_search_large.sh search "schedule task" 10
#   ./ai_code_search_large.sh search-sub kernel "scheduler" 5
#   ./ai_code_search_large.sh dataflow task_struct

# ============================================
# Linux 内核探索实战记录（完整工作流程）
# ============================================
#
# 【实战案例：Linux 内核内存管理子系统探索】
#
# 项目规模：
#   - Linux 内核源码：58,373 文件，1,469,782 chunks
#   - 直接处理会导致：4小时+向量生成，1.1GB内存占用
#   - 分治后：mm/子系统 188文件，11,405 chunks，2分钟完成
#
# 探索流程：
#
# Step 1: 初始化（智能拆分）
#   ./ai_code_search_large.sh init /opt/linux
#   # 自动检测到大项目，启用智能拆分：
#   # - kernel/  → kernel_cache (630文件, 29,411 chunks)
#   # - mm/      → mm_cache (188文件, 11,405 chunks)
#   # - fs/      → fs_cache (2,160文件, 76,092 chunks)
#   # - net/     → net_cache (1,729文件, 66,571 chunks)
#   # - drivers/ → drivers_gpu, drivers_net 等 40+ 子系统
#   # - arch/    → arch_x86, arch_arm 等 17 子系统
#   # - 排除：testing/, Documentation/, samples/ 等非核心目录
#
# Step 2: 索引核心子系统（按需选择）
#   ./tools/code_indexer /opt/linux/mm ./linux_subsystems/mm_cache 8
#   # 结果：188文件，11,405 chunks，0.1秒完成
#   # 每个子系统独立索引，不相互影响
#
# Step 3: 生成语义向量
#   ./tools/batch_embedder ./linux_subsystems/mm_cache --model jina --name linux-mm
#   # 结果：11,405 chunks → 向量，116秒，98 items/s
#   # 子系统小，GPU显存足够，不会OOM
#
# Step 4: 构建分析数据
#   ./tools/call_graph ./linux_subsystems/mm_cache     # 调用关系图
#   ./tools/dataflow analyze ./linux_subsystems/mm_cache # 变量数据流
#   ./tools/word_freq ./linux_subsystems/mm_cache       # TF-IDF词频
#
# Step 5: 语义搜索探索（自然语言查询）
#
#   # 搜索 "slab allocator cache"
#   → [1] allocate_slab (0.9201)        slub.c:3441
#   → [2] ___slab_alloc (0.9178)        slub.c:4405
#   → [3] alloc_from_new_slab (0.9152)  slub.c
#
#   # 搜索 "page allocation order zone"
#   → [1] prepare_alloc_pages (0.8928)   page_alloc.c:4973
#   → [2] __alloc_pages_may_oom (0.8821) page_alloc.c:4047
#
#   # 搜索 "memory compaction migrate"
#   → [1] migrate_vma_pages (0.8559)     migrate_device.c:1263
#   → [2] isolate_migratepages (0.8352)  compact.c
#
# Step 6: 调用关系分析
#   ./tools/vector_search ./linux_subsystems/mm_cache "__alloc_pages_slowpath" \
#       3 --rich --callgraph
#   # 结果：显示 __alloc_pages_slowpath → prepare_alloc_pages → get_page_from_freelist
#   #       → __alloc_pages_direct_compact → compact_zone
#
# Step 7: 数据流追踪
#   ./tools/dataflow show ./linux_subsystems/mm_cache page
#   # 结果：
#   #   📌 DEFINITIONS: __inc_zone_page_state()
#   #   ✏️  ASSIGNMENTS: __page_frag_cache_refill(), alloc_zpdesc()
#   #   👁️  USAGES: page_zone(), page_pgdat(), put_page_testzero()
#   #   🔍 FIELDS: page->lru, page->buddy_list, page->_mapcount
#
# Step 8: 跨函数数据流
#   # 追踪 page 变量如何传递
#   # 显示：定义函数 → 被传递到哪些函数 → 哪些函数使用了它
#
# 【发现的核心架构】
#
# 内存分配三级层次：
#   kmalloc/vmalloc (用户接口)
#       ↓
#   SLUB 分配器 (对象缓存：___slab_alloc → allocate_slab)
#       ↓
#   Buddy 系统 (物理页面：__alloc_pages_slowpath)
#
# 关键发现：
#   - Buddy: order参数决定2^order个连续页面
#   - SLUB: percpu cpu_slab → node partial → allocate_slab 三级缓存
#   - OOM: __alloc_pages_may_oom → out_of_memory → oom_kill_process
#   - 压缩: __alloc_pages_direct_compact → compact_zone
#
# ============================================
# AI Agent Linux 内核开发完整工作流
# ============================================
#
# 【完整闭环：理解 → 修改 → 编译 → 调试 → 验证】
#
# 场景示例：修改 page allocation 失败时的调试信息
#
# 1. 理解代码（语义搜索）
#    ./tools/vector_search ./linux_subsystems/mm_cache \
#        "page allocation fail slowpath" 5 --rich
#    # → 找到 __alloc_pages_slowpath 是核心入口
#    # → 找到 prepare_alloc_pages 解析 gfp_mask
#    # → 找到 __alloc_pages_may_oom 触发 OOM
#
# 2. 分析影响范围（数据流 + 调用图）
#    ./tools/dataflow show ./linux_subsystems/mm_cache gfp
#    # → gfp_mask 传递到 __alloc_pages_may_oom
#
#    ./tools/vector_search ./linux_subsystems/mm_cache \
#        "__alloc_pages_slowpath" 3 --callgraph
#    # → 被 alloc_pages, __get_free_pages 调用
#
# 3. 修改代码（编辑器）
#    vim /opt/linux/mm/page_alloc.c
#    # 在 __alloc_pages_slowpath 中添加 printk 或 tracepoint
#    # 修改后保存
#
# 4. 编译内核
#    cd /opt/linux
#    make oldconfig          # 确认配置
#    make -j$(nproc)         # 编译（15-30分钟）
#    # 或使用 ./tools/bach_compile.sh（如果存在）
#
# 5. 运行测试（QEMU）
#    qemu-system-x86_64 \
#        -kernel arch/x86/boot/bzImage \
#        -append "console=ttyS0 debug loglevel=8" \
#        -serial stdio \
#        -m 512M \
#        -initrd rootfs.cpio.gz
#    # 在 QEMU 中运行，查看 dmesg 输出
#
# 6. 验证修改（搜索确认）
#    ./tools/vector_search ./linux_subsystems/mm_cache \
#        "your_new_debug_function" 5 --callgraph
#    # → 确认新函数被正确调用
#
# 7. 调试分析（如果有问题）
#    # 在 QEMU 中用 gdb 调试
#    gdb ./vmlinux
#    (gdb) target remote :1234
#    (gdb) break __alloc_pages_slowpath
#    (gdb) continue
#
# 【系统优势】
#
# 相比传统开发：
#   ✅ 自然语言搜索："how does page allocation work" → 直接找到核心函数
#   ✅ 调用关系图：一键查看函数依赖链
#   ✅ 数据流追踪：变量在哪里被修改一目了然
#   ✅ 子系统隔离：只编译修改的子系统，不处理整个内核
#   ✅ 快速迭代：子系统小，向量生成快，搜索秒级响应
#
# 【注意事项】
#
#   - 分治后每个子系统独立，跨子系统调用需要全局搜索
#   - 大子系统（如 drivers_net）仍可能需要进一步拆分
#   - 排除的目录（testing/, doc/）如果有需要可手动添加
#   - QEMU 运行需要 rootfs，可用 busybox 制作
#
# ============================================

set -euo pipefail

# ============================================
# 环境变量
# ============================================
# 动态 GPU 环境（兼容本地和远程）
GPU_LIBS=""
[ -d "/home/dministrator/my_db" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/my_db"
[ -d "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)" ] && GPU_LIBS="${GPU_LIBS}:$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -d "/opt/TensorRT-10/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/TensorRT-10/lib"
[ -d "/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib" ] && GPU_LIBS="${GPU_LIBS}:/opt/cudnn-linux-x86_64-8.9.7.29_cuda12/lib"
[ -d "/opt/cuda/lib64" ] && GPU_LIBS="${GPU_LIBS}:/opt/cuda/lib64"
[ -d "/home/dministrator/anaconda3/envs/dl/lib" ] && GPU_LIBS="${GPU_LIBS}:/home/dministrator/anaconda3/envs/dl/lib"
GPU_LIBS="${GPU_LIBS#:}"
export LD_LIBRARY_PATH="${GPU_LIBS}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LARGE_CONFIG="${SCRIPT_DIR}/.large_project_config"

# 颜色输出
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${BLUE}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ============================================
# 配置管理
# ============================================

load_config() {
    if [[ ! -f "$LARGE_CONFIG" ]]; then
        error "未找到配置文件: $LARGE_CONFIG"
        error "请先运行: $0 init <repo_path>"
        exit 1
    fi
    source "$LARGE_CONFIG"
}

# 获取子系统的源路径（支持多路径，空格分隔）
get_sub_path() {
    local sub="$1"
    local var_name="SUB_${sub}"
    echo "${!var_name:-}"
}

# 获取子系统的 cache 路径
get_sub_cache() {
    local sub="$1"
    echo "${BASE_CACHE}/${sub}_cache"
}

# ============================================
# 子命令: init - 初始化配置（智能分析）
# ============================================
cmd_init() {
    local repo_path="${1:-}"
    local config_file="${2:-$LARGE_CONFIG}"
    
    if [[ -z "$repo_path" ]]; then
        echo "用法: $0 init <repo_path> [config_file]"
        echo "  repo_path:   源码目录路径"
        echo "  config_file: 配置文件路径 (默认: .large_project_config)"
        echo ""
        echo "说明: 自动分析项目结构，按文件数量智能拆分子系统"
        echo "      大目录(如drivers/)拆成子目录，小目录合并到misc"
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
    
    # 统计总文件数
    local total_files=$(find "$repo_path" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) 2>/dev/null | wc -l)
    info "  总文件数: $total_files"
    
    # 如果是超大项目(>10000文件)，调用智能分析
    local use_smart_split=0
    if [[ $total_files -gt 10000 ]]; then
        info "检测到超大项目，启用智能子系统拆分..."
        use_smart_split=1
    fi
    
    if [[ $use_smart_split -eq 1 && -x "${SCRIPT_DIR}/tools/analyze_project_structure.sh" ]]; then
        # 使用智能分析
        info "运行项目结构分析..."
        "${SCRIPT_DIR}/tools/analyze_project_structure.sh" "$repo_path" 100000 100 > /tmp/project_analysis.txt 2>&1 || true
        
        # 从分析结果提取建议的子系统列表
        # 这里简化处理：直接使用分析脚本的输出格式
        info "使用智能拆分方案生成配置..."
        generate_smart_config "$repo_path" "$config_file" "$base_cache" "$project_name"
    else
        # 使用简单拆分
        info "使用简单子系统拆分..."
        generate_simple_config "$repo_path" "$config_file" "$base_cache" "$project_name"
    fi
    
    ok "配置已保存到: $config_file"
    
    # 创建 cache 目录
    load_config
    mkdir -p "$base_cache"
    for sub in "${SUBSYSTEMS[@]}"; do
        mkdir -p "${BASE_CACHE}/${sub}_cache"
    done
    ok "Cache 目录已创建"
    ok "子系统数: ${#SUBSYSTEMS[@]}"
}

# 生成智能配置（按文件数拆分）
generate_smart_config() {
    local repo_path="$1"
    local config_file="$2"
    local base_cache="$3"
    local project_name="$4"
    
    local max_chunks=100000
    local min_files=100
    
    # 排除的目录（测试、文档、示例、脚本等非核心源码）
    local exclude_list=("testing" "test" "tests" "Documentation" "docs" "doc" "samples" "examples" "demo" "LICENSES" "licenses" "scripts")
    
    cat > "$config_file" << EOF
# AI Agent 超大项目配置 - 智能拆分
# 项目: $project_name
# 生成时间: $(date)
# 拆分策略: 大目录按二级目录拆分，小目录合并到misc
# 排除目录: testing, test, tests, Documentation, docs, samples, examples, LICENSES

REPO_PATH="$repo_path"
PROJECT_NAME="$project_name"
BASE_CACHE="$base_cache"

# 子系统列表
EOF

    local subs=()
    local misc_paths=()
    
    # 遍历顶层目录
    for dir in "$repo_path"/*/; do
        [[ ! -d "$dir" ]] && continue
        local local_name=$(basename "$dir")
        
        # 检查是否在排除列表中
        local is_excluded=0
        for excluded in "${exclude_list[@]}"; do
            if [[ "$local_name" == "$excluded" ]]; then
                is_excluded=1
                break
            fi
        done
        [[ $is_excluded -eq 1 ]] && continue
        
        local file_count=$(find "$dir" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) 2>/dev/null | wc -l)
        local est_chunks=$((file_count * 25))
        
        if [[ $est_chunks -gt $max_chunks ]]; then
            # 大目录：按二级目录拆分
            for subdir in "$dir"/*/; do
                [[ ! -d "$subdir" ]] && continue
                local sub_name=$(basename "$subdir")
                
                # 检查子目录是否在排除列表中
                local sub_excluded=0
                for excluded in "${exclude_list[@]}"; do
                    if [[ "$sub_name" == "$excluded" ]]; then
                        sub_excluded=1
                        break
                    fi
                done
                [[ $sub_excluded -eq 1 ]] && continue
                
                local sub_files=$(find "$subdir" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) 2>/dev/null | wc -l)
                
                if [[ $sub_files -ge $min_files ]]; then
                    local sub_key="${local_name}_${sub_name}"
                    subs+=("$sub_key")
                    echo "SUB_${sub_key}=\"$subdir\"" >> "$config_file"
                else
                    misc_paths+=("$subdir")
                fi
            done
        elif [[ $file_count -ge $min_files ]]; then
            # 中等目录：作为独立子系统
            subs+=("$local_name")
            echo "SUB_${local_name}=\"$dir\"" >> "$config_file"
        else
            # 小目录：加入misc
            misc_paths+=("$dir")
        fi
    done
    
    # 添加 misc 子系统（合并所有小目录）
    if [[ ${#misc_paths[@]} -gt 0 ]]; then
        subs+=("misc")
        local misc_value=""
        for p in "${misc_paths[@]}"; do
            misc_value="$misc_value $p"
        done
        echo "SUB_misc=\"${misc_value# }\"" >> "$config_file"
    fi
    
    # 输出 SUBSYSTEMS 数组
    {
        echo ""
        echo -n "SUBSYSTEMS=("
        local first=1
        for s in "${subs[@]}"; do
            if [[ $first -eq 1 ]]; then
                first=0
            else
                echo -n " "
            fi
            echo -n "$s"
        done
        echo ")"
        echo ""
        echo "# 最大 chunk 数（超过则跳过向量生成）"
        echo "MAX_CHUNKS_PER_SUB=$max_chunks"
        echo ""
        echo "# 全局符号表文件"
        echo "GLOBAL_SYMBOLS=\"${base_cache}/global_symbols.jsonl\""
    } >> "$config_file"
}

# 生成简单配置（固定子系统列表）
generate_simple_config() {
    local repo_path="$1"
    local config_file="$2"
    local base_cache="$3"
    local project_name="$4"
    
    local detected_subs=()
    for sub in kernel mm fs net drivers arch ipc security crypto block init lib include scripts tools; do
        if [[ -d "${repo_path}/${sub}" ]]; then
            detected_subs+=("$sub")
        fi
    done
    
    if [[ ${#detected_subs[@]} -eq 0 ]]; then
        detected_subs=("main")
    fi
    
    cat > "$config_file" << EOF
# AI Agent 超大项目配置
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
}

# ============================================
# 子命令: index - 索引所有子系统
# ============================================
cmd_index() {
    local workers="${1:-$(nproc)}"
    
    load_config
    
    info "开始索引所有子系统..."
    info "  项目: $PROJECT_NAME"
    info "  子系统数: ${#SUBSYSTEMS[@]}"
    info "  工作进程: $workers"
    echo ""
    
    local pids=()
    local idx=0
    
    for sub in "${SUBSYSTEMS[@]}"; do
        local sub_path=$(get_sub_path "$sub")
        local sub_cache=$(get_sub_cache "$sub")
        
        if [[ -z "$sub_path" ]]; then
            warn "跳过未配置的子系统: $sub"
            continue
        fi
        
        # 检查路径是否存在（支持多路径）
        local path_exists=0
        for p in $sub_path; do
            if [[ -d "$p" ]]; then
                path_exists=1
                break
            fi
        done
        
        if [[ $path_exists -eq 0 ]]; then
            warn "跳过不存在的子系统: $sub"
            continue
        fi
        
        info "[$((idx+1))/${#SUBSYSTEMS[@]}] 索引子系统: $sub"
        
        # 后台索引
        (
            if [[ "$sub" == "misc" ]]; then
                # misc 是多路径，需要创建临时目录列表
                local temp_file_list="/tmp/ai_large_files_${sub}_$$.txt"
                > "$temp_file_list"
                for p in $sub_path; do
                    if [[ -d "$p" ]]; then
                        find "$p" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) >> "$temp_file_list"
                    fi
                done
                
                # 使用 code_indexer 的文件列表模式（如果支持）
                # 如果不支持，创建临时符号链接目录
                local temp_repo="/tmp/ai_large_repo_${sub}_$$"
                mkdir -p "$temp_repo"
                while IFS= read -r file; do
                    local rel_path=$(echo "$file" | sed "s|${REPO_PATH}/||")
                    local target_dir="$temp_repo/$(dirname "$rel_path")"
                    mkdir -p "$target_dir"
                    ln -sf "$file" "$temp_repo/$rel_path" 2>/dev/null || true
                done < "$temp_file_list"
                
                "${SCRIPT_DIR}/tools/code_indexer" "$temp_repo" "$sub_cache" "$workers" 2>/dev/null || true
                
                rm -rf "$temp_repo" "$temp_file_list"
            else
                # 单路径，直接索引
                "${SCRIPT_DIR}/tools/code_indexer" "$sub_path" "$sub_cache" "$workers" 2>/dev/null
            fi
            
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
    local workers="${1:-1}"
    
    load_config
    
    info "开始生成向量..."
    info "  并行度: $workers (每个子系统需要独立 GPU 显存)"
    echo ""
    
    local idx=0
    for sub in "${SUBSYSTEMS[@]}"; do
        local sub_cache=$(get_sub_cache "$sub")
        
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
        local sub_cache=$(get_sub_cache "$sub")
        
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
    
    local sub_cache=$(get_sub_cache "$sub")
    
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
        local sub_cache=$(get_sub_cache "$sub_filter")
        if [[ -f "${sub_cache}/chunks_meta.jsonl" ]]; then
            "${SCRIPT_DIR}/tools/dataflow" show "$sub_cache" "$var_name" 2>/dev/null
        else
            error "子系统未索引: $sub_filter"
        fi
    else
        # 搜索所有子系统
        for sub in "${SUBSYSTEMS[@]}"; do
            local sub_cache=$(get_sub_cache "$sub")
            
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
    printf "%-25s %-12s %-12s %-10s\n" "子系统" "Chunks" "向量" "调用图"
    printf "%-25s %-12s %-12s %-10s\n" "─────────────────────────" "────────────" "────────────" "──────────"
    
    local total_chunks=0
    local total_vectors=0
    
    for sub in "${SUBSYSTEMS[@]}"; do
        local sub_cache=$(get_sub_cache "$sub")
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
        
        printf "%-25s %-12s %-12s %-10s\n" "$sub" "$chunks" "$vectors" "$callgraph"
    done
    
    printf "%-25s %-12s\n" "─────────────────────────" "────────────"
    printf "%-25s %-12s\n" "总计" "$total_chunks"
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
            echo "针对 Linux 内核等超大项目（5万+文件），智能分治处理："
            echo "- 自动分析项目结构，按文件数量拆分子系统"
            echo "- 大目录(如drivers/)按二级目录拆分"
            echo "- 小目录合并到 misc 子系统"
            echo "- 每个子系统独立索引和生成向量"
            echo ""
            echo "子命令:"
            echo "  init <repo> [config]        - 初始化（自动分析并拆分子系统）"
            echo "  index [workers]             - 索引所有子系统"
            echo "  vector [workers]            - 生成所有子系统向量"
            echo "  search <query> [n]          - 全局搜索（聚合所有子系统）"
            echo "  search-sub <sub> <query> [n] - 搜索指定子系统"
            echo "  dataflow <var> [sub]        - 全局变量追踪"
            echo "  status                      - 查看处理状态"
            echo ""
            echo "示例:"
            echo "  $0 init /opt/linux"
            echo "  $0 index 8"
            echo "  $0 vector 1"
            echo "  $0 search \"schedule task\" 10"
            echo "  $0 search-sub kernel \"scheduler\" 5"
            echo "  $0 dataflow task_struct"
            ;;
    esac
}

main "$@"
