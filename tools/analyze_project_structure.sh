#!/bin/bash
#
# AI Agent 超大项目预处理 - 智能子系统拆分
# ==========================================
#
# 功能：统计每个目录的文件数量，自动拆分大目录，合并小目录
# 确保每个子系统的 chunk 数在合理范围内（1万 ~ 10万）
#
# 使用方式:
#   ./tools/analyze_project_structure.sh <repo_path> [max_chunks_per_sub]
#
# 输出:
#   控制台显示目录统计和建议拆分方案
#   生成 .large_project_config 配置文件

set -euo pipefail

REPO_PATH="${1:-}"
MAX_CHUNKS="${2:-100000}"
MIN_FILES="${3:-100}"

if [[ -z "$REPO_PATH" ]]; then
    echo "用法: $0 <repo_path> [max_chunks_per_sub] [min_files]"
    echo "  repo_path:          源码目录"
    echo "  max_chunks_per_sub: 每个子系统最大 chunk 数 (默认: 100000)"
    echo "  min_files:          最小文件数才成为独立子系统 (默认: 100)"
    echo ""
    echo "示例:"
    echo "  $0 /opt/linux 100000 100"
    echo "  $0 /opt/linux 50000 50"
    exit 1
fi

if [[ ! -d "$REPO_PATH" ]]; then
    echo "错误: 目录不存在: $REPO_PATH"
    exit 1
fi

REPO_PATH="$(cd "$REPO_PATH" && pwd)"
PROJECT_NAME=$(basename "$REPO_PATH")

echo "╔══════════════════════════════════════════════════════════════════╗"
echo "║ 项目结构分析"
echo "║ 路径: $REPO_PATH"
echo "║ 最大 chunk/子系统: $MAX_CHUNKS"
echo "║ 最小文件数: $MIN_FILES"
echo "╚══════════════════════════════════════════════════════════════════╝"
echo ""

# 统计顶层目录
echo "📊 顶层目录统计:"
echo "─────────────────────────────────────────────────────────────────"
printf "%-20s %10s %12s %12s\n" "目录" "文件数" "预估chunks" "建议"
printf "%-20s %10s %12s %12s\n" "────────────────────" "──────────" "────────────" "────────────"

declare -A dir_files
declare -A dir_chunks
declare -a large_dirs
declare -a normal_dirs
declare -a small_dirs

for dir in "$REPO_PATH"/*/; do
    if [[ ! -d "$dir" ]]; then continue; fi
    
    local_name=$(basename "$dir")
    file_count=$(find "$dir" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) 2>/dev/null | wc -l)
    
    # 预估 chunk 数（每个文件平均 25 个 chunk）
    est_chunks=$((file_count * 25))
    
    dir_files[$local_name]=$file_count
    dir_chunks[$local_name]=$est_chunks
    
    if [[ $est_chunks -gt $MAX_CHUNKS ]]; then
        printf "%-20s %10d %12d %12s\n" "$local_name" "$file_count" "$est_chunks" "⚠️ 需拆分"
        large_dirs+=("$local_name")
    elif [[ $file_count -lt $MIN_FILES ]]; then
        printf "%-20s %10d %12d %12s\n" "$local_name" "$file_count" "$est_chunks" "太小"
        small_dirs+=("$local_name")
    else
        printf "%-20s %10d %12d %12s\n" "$local_name" "$file_count" "$est_chunks" "✅ OK"
        normal_dirs+=("$local_name")
    fi
done

echo ""

# 分析大目录的子目录
echo "🔍 大目录的子目录拆分建议:"
echo "─────────────────────────────────────────────────────────────────"

for large_dir in "${large_dirs[@]}"; do
    echo ""
    echo "目录: $large_dir/ (总计: ${dir_files[$large_dir]} 文件)"
    
    # 统计二级目录
    sub_dirs=()
    for subdir in "$REPO_PATH/$large_dir"/*/; do
        if [[ ! -d "$subdir" ]]; then continue; fi
        sub_name=$(basename "$subdir")
        sub_files=$(find "$subdir" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) 2>/dev/null | wc -l)
        sub_chunks=$((sub_files * 25))
        
        if [[ $sub_files -ge $MIN_FILES ]]; then
            printf "  %-25s %8d files ≈ %8d chunks\n" "$sub_name/" "$sub_files" "$sub_chunks"
            sub_dirs+=("$sub_name")
        fi
    done
    
    if [[ ${#sub_dirs[@]} -eq 0 ]]; then
        echo "  (没有足够大的子目录，建议按文件类型拆分)"
    fi
done

echo ""

# 生成建议的子系统列表
echo "📋 建议的子系统配置:"
echo "─────────────────────────────────────────────────────────────────"

suggested_subs=()

# 1. 添加正常大小的目录
for dir in "${normal_dirs[@]}"; do
    suggested_subs+=("$dir")
done

# 2. 添加大目录的子目录
for large_dir in "${large_dirs[@]}"; do
    for subdir in "$REPO_PATH/$large_dir"/*/; do
        if [[ ! -d "$subdir" ]]; then continue; fi
        sub_name=$(basename "$subdir")
        sub_files=$(find "$subdir" -type f \( -name "*.c" -o -name "*.h" -o -name "*.cpp" -o -name "*.hpp" \) 2>/dev/null | wc -l)
        
        if [[ $sub_files -ge $MIN_FILES ]]; then
            suggested_subs+=("${large_dir}_${sub_name}")
        fi
    done
done

# 3. 合并小目录到一个 misc
if [[ ${#small_dirs[@]} -gt 0 ]]; then
    suggested_subs+=("misc")
fi

echo "建议的子系统数: ${#suggested_subs[@]}"
for sub in "${suggested_subs[@]}"; do
    echo "  - $sub"
done

echo ""

# 生成配置文件示例
echo "📝 配置文件示例 (保存为 .large_project_config):"
echo "─────────────────────────────────────────────────────────────────"

cat << EOF
cat > .large_project_config << 'CONFIG'
# AI Agent 超大项目配置
# 项目: $PROJECT_NAME
# 生成时间: $(date)

REPO_PATH="$REPO_PATH"
PROJECT_NAME="$PROJECT_NAME"
BASE_CACHE="./${PROJECT_NAME}_subsystems"

# 子系统列表
declare -a SUBSYSTEMS=(${suggested_subs[@]})

# 子系统路径映射
CONFIG

EOF

# 输出每个子系统的路径映射
for sub in "${suggested_subs[@]}"; do
    if [[ "$sub" == "misc" ]]; then
        # 小目录合并
        misc_paths=()
        for small_dir in "${small_dirs[@]}"; do
            misc_paths+=("$REPO_PATH/$small_dir")
        done
        echo "echo 'SUB_misc=\"${misc_paths[@]}\"' >> .large_project_config"
    elif [[ "$sub" == *_* ]]; then
        # 大目录的子目录
        parent=$(echo "$sub" | cut -d'_' -f1)
        child=$(echo "$sub" | cut -d'_' -f2-)
        echo "echo 'SUB_${sub}=\"$REPO_PATH/$parent/$child\"' >> .large_project_config"
    else
        # 普通目录
        echo "echo 'SUB_${sub}=\"$REPO_PATH/$sub\"' >> .large_project_config"
    fi
done

cat << EOF

cat >> .large_project_config << 'CONFIG'

# 最大 chunk 数（超过则跳过向量生成）
MAX_CHUNKS_PER_SUB=$MAX_CHUNKS

# 全局符号表文件
GLOBAL_SYMBOLS="./${PROJECT_NAME}_subsystems/global_symbols.jsonl"
CONFIG

EOF

echo ""
echo "💡 使用建议:"
echo "  1. 查看上方统计，确认拆分方案是否合理"
echo "  2. 如需调整，手动修改 .large_project_config"
echo "  3. 运行: ./ai_code_search_large.sh init $REPO_PATH"
echo "  4. 或直接执行生成的配置"
