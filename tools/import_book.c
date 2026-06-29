/*
 * import_book.c — 电子书导入记忆系统（优化版）
 * 
 * 功能：将 MOBI/PDF/AZW3 电子书导入 KV Cache 记忆系统
 * 
 * 架构设计：
 *   1. 电子书内容保存为 Markdown 文件到磁盘（/opt/books/{name}/chapters/.../page_{idx}.md）
 *   2. KV Cache 只存储元数据 + md 文件路径 + 预览 + 向量
 *   3. AI 通过 KV Cache 定位到 md 文件路径，然后读取完整内容
 * 
 * 支持的格式：
 *   - MOBI (.mobi, .azw, .azw3)
 *   - PDF (.pdf)
 * 
 * 向量模型：Jina v2 code embedding (768-dim)
 * 
 * 使用方法：
 *   ./import_book <cache_dir> <book_file> [namespace]
 * 
 * 示例：
 *   ./import_book /memory ~/book.mobi /books/my_book
 *   ./import_book /memory ~/paper.pdf
 */

#define _GNU_SOURCE
#include "cache.h"
#include "onnx_embedder.h"
#include "metrics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <omp.h>  // OpenMP for parallel chapter processing
#include "ocr_helper.h"

// =============================================================================
// 跨文件系统目录移动辅助函数（替代 system("mv ...")）
// =============================================================================

#include <dirent.h>
#include <fcntl.h>

static int copy_file(const char* src, const char* dst) {
    int src_fd = open(src, O_RDONLY);
    if (src_fd < 0) return -1;
    int dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) { close(src_fd); return -1; }
    
    char buf[65536];
    ssize_t n;
    while ((n = read(src_fd, buf, sizeof(buf))) > 0) {
        if (write(dst_fd, buf, n) != n) {
            close(src_fd); close(dst_fd); unlink(dst);
            return -1;
        }
    }
    close(src_fd);
    close(dst_fd);
    return (n < 0) ? -1 : 0;
}

static int rm_rf(const char* path);

static int copy_dir_recursive(const char* src, const char* dst) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    
    if (S_ISREG(st.st_mode)) {
        return copy_file(src, dst);
    }
    if (!S_ISDIR(st.st_mode)) return -1;
    
    if (mkdir(dst, 0755) != 0 && errno != EEXIST) return -1;
    
    DIR* dir = opendir(src);
    if (!dir) return -1;
    
    struct dirent* entry;
    int ret = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char src_path[1024], dst_path[1024];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, entry->d_name);
        struct stat entry_st;
        if (stat(src_path, &entry_st) != 0) { ret = -1; break; }
        if (S_ISDIR(entry_st.st_mode)) {
            if (copy_dir_recursive(src_path, dst_path) != 0) { ret = -1; break; }
        } else {
            if (copy_file(src_path, dst_path) != 0) { ret = -1; break; }
        }
    }
    closedir(dir);
    return ret;
}

static int rm_rf(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return (errno == ENOENT) ? 0 : -1;
    
    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path);
    }
    if (!S_ISDIR(st.st_mode)) return -1;
    
    DIR* dir = opendir(path);
    if (!dir) return -1;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        rm_rf(child);
    }
    closedir(dir);
    return rmdir(path);
}

static int move_dir(const char* src, const char* dst) {
    // Try rename first (fast, O(1) if same filesystem)
    if (rename(src, dst) == 0) return 0;
    // Fallback: copy + delete (cross-filesystem)
    if (copy_dir_recursive(src, dst) != 0) return -1;
    return rm_rf(src);
}

// =============================================================================
// 配置
// =============================================================================

// Jina v2 模型配置（与代码分析统一）
#define MODEL_PATH      "/opt/models/jina-embeddings-v2-base-code/model.onnx"
#define VOCAB_PATH      "/opt/models/jina-embeddings-v2-base-code/vocab.json"
#define MODEL_SEQ_LEN   512
#define EMBEDDING_DIM   768

// 分块配置
#define MAX_PARAGRAPHS      5000    // 单章节最大段落数
#define MIN_PARA_LEN        50      // 最小段落长度（过滤碎片）
#define TARGET_CHUNK_SIZE   8000     // 目标段落长度（字符）
#define MAX_CHUNK_SIZE      16000    // 最大段落长度（超过则强制切分）
#define MERGE_THRESHOLD     300     // 短页合并阈值（小于此值尝试合并）
#define PREVIEW_LEN         200     // KV Cache 中保存的预览长度

// 章节信息结构（与 wrapper 兼容）
typedef struct {
    char* title;
    int level;
    size_t offset;  // MOBI: byte offset, PDF: page number
} ChapterInfo;

// 页面项（用于并行章节处理后的序列化写入）
typedef struct {
    char* text;                     // 段落文本（由 paragraphs 数组拥有，不单独释放）
    char md_path[1024];
    char preview_escaped[PREVIEW_LEN * 2];
    size_t content_len;
    int page_num;
    float* vector;                  // 向量数据（EMBEDDING_DIM floats），NULL 表示无向量
} page_item_t;

// 章节处理结果（并行阶段产出，序列化阶段消费）
typedef struct {
    char chapter_dirname[256];
    char chapter_title[512];
    char chapter_md_dir[1024];
    page_item_t* items;
    int item_count;
    int total_pages;
} chapter_result_t;

// 全局嵌入器
static onnx_embedder_t* g_embedder = NULL;

// =============================================================================
// 工具函数
// =============================================================================

// 创建目录（递归）
static int mkdir_p(const char* path) {
    char tmp[1024];
    char* p = NULL;
    size_t len;
    
    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

// 安全文件名：替换非法字符
static void sanitize_filename(char* dst, size_t dst_size, const char* src) {
    size_t i, j;
    for (i = 0, j = 0; src[i] && j < dst_size - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        // 处理 UTF-8 多字节字符中的常见 Unicode 标点
        if (c == 0xe2 && (unsigned char)src[i+1] == 0x80) {
            unsigned char c3 = (unsigned char)src[i+2];
            if (c3 == 0x98 || c3 == 0x99) {  // U+2018, U+2019 单引号
                dst[j++] = '\'';
                i += 2;
                continue;
            } else if (c3 == 0x9c || c3 == 0x9d) {  // U+201C, U+201D 双引号
                dst[j++] = '"';
                i += 2;
                continue;
            } else if (c3 == 0x93 || c3 == 0x94) {  // U+2013, U+2014 短横线
                dst[j++] = '-';
                i += 2;
                continue;
            }
        }
        if (c == '/' || c == '\\' || c == ':' || c == '*' || 
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            dst[j++] = '_';
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            dst[j++] = '_';
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    
    // 去掉末尾的下划线
    while (j > 0 && dst[j-1] == '_') {
        dst[--j] = '\0';
    }
}

// 生成安全的章节目录名
static void chapter_dirname(char* dst, size_t dst_size, int idx, const char* title) {
    char safe_title[128];
    sanitize_filename(safe_title, sizeof(safe_title), title);
    
    if (strlen(safe_title) > 60) {
        // 确保不在多字节 UTF-8 字符中间截断
        int cut_pos = 60;
        // 如果截断位置是 UTF-8 续字节 (0x80-0xBF)，向前找到字符起始字节
        while (cut_pos > 0 && ((unsigned char)safe_title[cut_pos] >= 0x80 && (unsigned char)safe_title[cut_pos] < 0xC0)) {
            cut_pos--;
        }
        safe_title[cut_pos] = '\0';
    }
    
    snprintf(dst, dst_size, "%02d-%s", idx + 1, safe_title);
}

// 检查是否是 CSS 内容
static int is_css_content(const char* text) {
    if (strstr(text, "{") && strstr(text, "}")) return 1;
    if (strstr(text, "@media")) return 1;
    if (strstr(text, "@font-face")) return 1;
    if (strstr(text, ".calibre")) return 1;
    
    const char* css_keywords[] = {
        "margin:", "padding:", "font-size:", "display:", "text-align:",
        "text-indent:", "line-height:", "color:", "background", "border",
        "position:", "width:", "height:", NULL
    };
    int css_count = 0;
    for (int i = 0; css_keywords[i]; i++) {
        if (strstr(text, css_keywords[i])) css_count++;
    }
    return css_count >= 2;
}

// 检查是否是 HTML 标签
static int is_html_markup(const char* text) {
    if (text[0] == '<' && strchr(text, '>')) return 1;
    return 0;
}

// 检查是否是空白或噪音
static int is_noise(const char* text) {
    size_t len = strlen(text);
    if (len < MIN_PARA_LEN) return 1;
    
    int alpha_count = 0;
    for (size_t i = 0; i < len; i++) {
        if ((text[i] >= 'a' && text[i] <= 'z') || 
            (text[i] >= 'A' && text[i] <= 'Z') ||
            ((unsigned char)text[i] >= 0x80)) {  // 包含中文（强制 unsigned char）
            alpha_count++;
        }
    }
    // 如果字母比例太低，可能是噪音
    if (alpha_count < (int)(len * 0.3)) return 1;
    
    return 0;
}

// =============================================================================
// 分块函数
// =============================================================================

/*
 * 检测是否是句子结束边界（多格式支持）
 * 支持：英文 . ! ? + 空格，中文 。！？；：
 */
/**
 * 检测是否是句子结束边界（多格式支持）
 * 返回边界字符的长度（0 表示不是边界）
 * 支持：英文 . ! ? + 空格（长度1），中文 。！？；：（长度3），换行（长度2）
 */
static int is_sentence_boundary(const char* p, const char* end) {
    if (p >= end) return 0;
    
    // 英文标点 + 空白
    if ((*p == '.' || *p == '!' || *p == '?') && p + 1 < end) {
        char next = p[1];
        if (next == ' ' || next == '\n' || next == '\r' || next == '"' || next == '\'') {
            return 1;
        }
    }
    
    // 中文标点（UTF-8，3字节）
    if ((unsigned char)*p == 0xe3 && p + 2 < end) {
        unsigned char c2 = (unsigned char)p[1];
        unsigned char c3 = (unsigned char)p[2];
        if (c2 == 0x80) {
            // 。 ！ ？ ； ：
            if (c3 == 0x82 || c3 == 0x81 || c3 == 0x9c || c3 == 0x83 || c3 == 0x9a) {
                return 3;
            }
        }
    }
    
    // 换行符（某些格式用单行换行分隔段落）
    if (*p == '\n' && p + 1 < end && p[1] == '\n') {
        return 2;
    }
    
    return 0;
}

/*
 * 在指定范围内寻找最佳切分点（优先语义边界，fallback到空白处）
 */
static const char* find_best_cut(const char* start, const char* end, size_t target) {
    const char* p = start + target;
    if (p >= end) return end;
    
    // 向前搜索语义边界（最多回退 target/3）
    const char* best = p;
    size_t max_backtrack = target / 3;
    if (max_backtrack > 300) max_backtrack = 300;
    
    for (size_t i = 0; i < max_backtrack && p > start; i++, p--) {
        int boundary_len = is_sentence_boundary(p, end);
        if (boundary_len > 0) {
            best = p + boundary_len;
            break;
        }
    }
    
    // 如果没找到语义边界，在空白处切分
    if (best == start + target) {
        p = start + target;
        for (size_t i = 0; i < max_backtrack && p > start; i++, p--) {
            if (*p == ' ' || *p == '\n') {
                best = p + 1;
                break;
            }
        }
    }
    
    return best;
}

/*
 * 将文本分割为语义段落（通用版本，支持所有电子书格式）
 * 
 * 策略：
 *   1. 优先按空白行分割（EPUB/PDF 常见）
 *   2. 如果段落太少，按句子边界再分割（AZW3/MOBI 中文）
 *   3. 超长段落按语义边界+固定大小分割
 *   4. 合并相邻短页，避免碎片
 */
static int split_paragraphs(const char* text, size_t text_len,
                            char*** out_paragraphs, int* out_count) {
    int capacity = 1000;
    int count = 0;
    char** raw = malloc(sizeof(char*) * capacity);
    if (!raw) return -1;
    
    const char* end = text + text_len;
    
    // ===== 阶段 1: 按空白行分割 =====
    const char* p = text;
    while (p < end) {
        while (p < end && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        
        const char* para_end = p;
        int blank_lines = 0;
        
        while (para_end < end) {
            if (*para_end == '\n') {
                const char* next = para_end + 1;
                while (next < end && (*next == '\r' || *next == '\n' || *next == ' ' || *next == '\t')) {
                    if (*next == '\n') blank_lines++;
                    next++;
                }
                if (blank_lines >= 1) break;
                para_end = next;
            } else {
                para_end++;
            }
        }
        
        size_t len = para_end - p;
        if (len >= MIN_PARA_LEN) {
            if (count >= capacity) {
                capacity *= 2;
                char** np = realloc(raw, sizeof(char*) * capacity);
                if (!np) break;
                raw = np;
            }
            raw[count] = malloc(len + 1);
            if (raw[count]) {
                memcpy(raw[count], p, len);
                raw[count][len] = '\0';
                count++;
            }
        }
        p = para_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
    
    // ===== 阶段 2: 如果段落太少，按句子边界分割 =====
    if (count < 5 && text_len > 2000) {
        for (int i = 0; i < count; i++) free(raw[i]);
        count = 0;
        
        const char* chunk_start = text;
        const char* q = text;
        
        while (q < end) {
            int boundary_len = is_sentence_boundary(q, end);
            if (boundary_len > 0) {
                size_t len = q - chunk_start + boundary_len;
                if (len >= TARGET_CHUNK_SIZE || (len >= MIN_PARA_LEN && q + 3 >= end)) {
                    if (count >= capacity) {
                        capacity *= 2;
                        char** np = realloc(raw, sizeof(char*) * capacity);
                        if (!np) break;
                        raw = np;
                    }
                    raw[count] = malloc(len + 1);
                    if (raw[count]) {
                        memcpy(raw[count], chunk_start, len);
                        raw[count][len] = '\0';
                        count++;
                    }
                    chunk_start = q + boundary_len;
                }
            }
            q++;
        }
        
        // 剩余文本
        size_t len = end - chunk_start;
        if (len >= MIN_PARA_LEN) {
            if (count >= capacity) {
                capacity *= 2;
                char** np = realloc(raw, sizeof(char*) * capacity);
                if (!np) goto done;
                raw = np;
            }
            raw[count] = malloc(len + 1);
            if (raw[count]) {
                memcpy(raw[count], chunk_start, len);
                raw[count][len] = '\0';
                count++;
            }
        }
    }
    
    // ===== 阶段 3: 超长段落按语义边界分割 =====
    int stage3_count = 0;
    char** stage3 = malloc(sizeof(char*) * capacity * 2);
    if (!stage3) goto done;
    
    for (int i = 0; i < count; i++) {
        size_t len = strlen(raw[i]);
        if (len > MAX_CHUNK_SIZE) {
            const char* fp = raw[i];
            const char* fend = raw[i] + len;
            
            while (fp < fend) {
                size_t remain = fend - fp;
                size_t target = (remain > TARGET_CHUNK_SIZE * 2) ? TARGET_CHUNK_SIZE * 2 : remain;
                const char* cut = find_best_cut(fp, fend, target);
                size_t chunk = cut - fp;
                if (chunk == 0) chunk = remain;
                
                if (stage3_count >= capacity * 2) {
                    capacity *= 2;
                    char** np = realloc(stage3, sizeof(char*) * capacity * 2);
                    if (!np) break;
                    stage3 = np;
                }
                stage3[stage3_count] = malloc(chunk + 1);
                if (stage3[stage3_count]) {
                    memcpy(stage3[stage3_count], fp, chunk);
                    stage3[stage3_count][chunk] = '\0';
                    stage3_count++;
                }
                fp += chunk;
            }
        } else {
            if (stage3_count >= capacity * 2) {
                capacity *= 2;
                char** np = realloc(stage3, sizeof(char*) * capacity * 2);
                if (!np) break;
                stage3 = np;
            }
            stage3[stage3_count] = strdup(raw[i]);
            if (stage3[stage3_count]) stage3_count++;
        }
    }
    
    for (int i = 0; i < count; i++) free(raw[i]);
    free(raw);
    
    // ===== 阶段 4: 合并相邻短页 =====
    int final_count = 0;
    char** final = malloc(sizeof(char*) * capacity * 2);
    if (!final) {
        *out_paragraphs = stage3;
        *out_count = stage3_count;
        return 0;
    }
    
    for (int i = 0; i < stage3_count; i++) {
        size_t len = strlen(stage3[i]);
        
        // 当前页太短，尝试与上一页或下一页合并
        if (len < MERGE_THRESHOLD && final_count > 0) {
            size_t prev_len = strlen(final[final_count - 1]);
            // 如果上一页也不长，合并到上一页
            if (prev_len < MAX_CHUNK_SIZE - len - 10) {
                size_t new_len = prev_len + len + 2;
                char* merged = malloc(new_len + 1);
                if (merged) {
                    snprintf(merged, new_len + 1, "%s\n%s", final[final_count - 1], stage3[i]);
                    free(final[final_count - 1]);
                    final[final_count - 1] = merged;
                    free(stage3[i]);
                    continue;
                }
            }
        }
        
        final[final_count] = stage3[i];
        final_count++;
    }
    
    free(stage3);
    
    *out_paragraphs = final;
    *out_count = final_count;
    return 0;
    
done:
    *out_paragraphs = raw;
    *out_count = count;
    return 0;
}

static void free_paragraphs(char** paragraphs, int count) {
    for (int i = 0; i < count; i++) {
        free(paragraphs[i]);
    }
    free(paragraphs);
}

// =============================================================================
// Markdown 文件保存
// =============================================================================

// =============================================================================
// 核心导入函数
// =============================================================================

/*
 * 处理单个章节（CPU/GPU 密集阶段，可并行）
 * 
 * 流程：
 *   1. 清理 UTF-8 → 2. 分块 → 3. 过滤噪音 → 4. 生成预览 → 5. 批量生成向量
 * 不触及磁盘和 KV Cache（线程不安全）
 */
static chapter_result_t* process_chapter(const char* md_dir, const char* chapter_dirname,
                                          const char* chapter_title,
                                          const char* text, size_t text_len) {
    chapter_result_t* result = calloc(1, sizeof(chapter_result_t));
    if (!result) return NULL;
    
    strncpy(result->chapter_dirname, chapter_dirname, sizeof(result->chapter_dirname) - 1);
    strncpy(result->chapter_title, chapter_title, sizeof(result->chapter_title) - 1);
    snprintf(result->chapter_md_dir, sizeof(result->chapter_md_dir), "%s/chapters/%s", md_dir, chapter_dirname);
    
    // 清理不完整的 UTF-8 序列
    char* clean_text = malloc(text_len + 1);
    if (!clean_text) {
        free(result);
        return NULL;
    }
    size_t clean_len = 0;
    int skipped_bytes = 0;
    for (size_t i = 0; i < text_len; ) {
        unsigned char c = (unsigned char)text[i];
        int char_len = 1;
        if (((c & 0x80) == 0) && (c >= 0x20 || c == 0x09 || c == 0x0A || c == 0x0D) && c != 0x7F) {
            char_len = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text_len && ((unsigned char)text[i+1] & 0xC0) == 0x80) {
            char_len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text_len && ((unsigned char)text[i+1] & 0xC0) == 0x80 && ((unsigned char)text[i+2] & 0xC0) == 0x80) {
            char_len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text_len && ((unsigned char)text[i+1] & 0xC0) == 0x80 && ((unsigned char)text[i+2] & 0xC0) == 0x80 && ((unsigned char)text[i+3] & 0xC0) == 0x80) {
            char_len = 4;
        } else {
            skipped_bytes++;
            i++;
            continue;
        }
        memcpy(clean_text + clean_len, text + i, char_len);
        clean_len += char_len;
        i += char_len;
    }
    clean_text[clean_len] = '\0';
    
    // 分块
    char** paragraphs = NULL;
    int para_count = 0;
    if (split_paragraphs(clean_text, clean_len, &paragraphs, &para_count) != 0) {
        fprintf(stderr, "Failed to split paragraphs\n");
        free(clean_text);
        free(result);
        return NULL;
    }
    free(clean_text);
    
    // 收集有效页面（不写入磁盘，只准备数据）
    result->items = malloc(para_count * sizeof(page_item_t));
    if (!result->items) {
        free_paragraphs(paragraphs, para_count);
        free(result);
        return NULL;
    }
    
    int item_count = 0;
    int total_pages = 0;
    
    for (int i = 0; i < para_count; i++) {
        if (is_css_content(paragraphs[i]) || 
            is_html_markup(paragraphs[i]) ||
            is_noise(paragraphs[i])) {
            continue;
        }
        
        size_t content_len = strlen(paragraphs[i]);
        size_t preview_len = content_len < PREVIEW_LEN ? content_len : PREVIEW_LEN;
        
        // 转义 JSON 中的引号
        char preview_escaped[PREVIEW_LEN * 2];
        size_t j = 0;
        for (size_t k = 0; k < preview_len && j < sizeof(preview_escaped) - 1; k++) {
            if (paragraphs[i][k] == '"') {
                preview_escaped[j++] = '\\';
                preview_escaped[j++] = '"';
            } else if (paragraphs[i][k] == '\n') {
                preview_escaped[j++] = '\\';
                preview_escaped[j++] = 'n';
            } else if (paragraphs[i][k] == '\\') {
                preview_escaped[j++] = '\\';
                preview_escaped[j++] = '\\';
            } else if ((unsigned char)paragraphs[i][k] < 0x20) {
                continue;
            } else {
                preview_escaped[j++] = paragraphs[i][k];
            }
        }
        preview_escaped[j] = '\0';
        
        // 清理 NULL 字节
        for (size_t k = 0; k < content_len; k++) {
            if (paragraphs[i][k] == '\0') paragraphs[i][k] = ' ';
        }
        
        page_item_t* item = &result->items[item_count];
        item->text = paragraphs[i];  // 借用 paragraphs 数组的内存，不复制
        snprintf(item->md_path, sizeof(item->md_path), "%s/chapters/%s/page_%04d.md",
                 md_dir, chapter_dirname, total_pages);
        strncpy(item->preview_escaped, preview_escaped, sizeof(item->preview_escaped) - 1);
        item->preview_escaped[sizeof(item->preview_escaped) - 1] = '\0';
        item->content_len = content_len;
        item->page_num = total_pages;
        item->vector = NULL;
        
        item_count++;
        total_pages++;
    }
    
    result->item_count = item_count;
    result->total_pages = total_pages;

    
    // 批量生成向量
    if (g_embedder && item_count > 0) {
        float* all_vectors = calloc(item_count, EMBEDDING_DIM * sizeof(float));
        if (all_vectors) {
            int use_batch = 0;  // batch encoding causes tokenizer panic
            #define EMBED_BATCH_SIZE 32
            for (int batch_start = 0; batch_start < item_count; batch_start += EMBED_BATCH_SIZE) {
                int batch_end = batch_start + EMBED_BATCH_SIZE;
                if (batch_end > item_count) batch_end = item_count;
                int bcount = batch_end - batch_start;
                
                if (use_batch) {
                    const char** texts = malloc(bcount * sizeof(char*));
                    for (int j = 0; j < bcount; j++) {
                        texts[j] = result->items[batch_start + j].text;
                    }
                    
                    int ret;
                    #pragma omp critical(embedder)
                    ret = onnx_embedder_encode_batch(g_embedder, texts, bcount, 
                                                         all_vectors + batch_start * EMBEDDING_DIM);
                    free(texts);
                    
                    if (ret != 0) {
                        use_batch = 0;
                        memset(all_vectors + batch_start * EMBEDDING_DIM, 0, 
                               bcount * EMBEDDING_DIM * sizeof(float));
                        for (int j = 0; j < bcount; j++) {
                            int idx = batch_start + j;
                            #pragma omp critical(embedder)
                            if (onnx_embedder_encode(g_embedder, result->items[idx].text, 
                                                     all_vectors + idx * EMBEDDING_DIM) != 0) {
                                fprintf(stderr, "    Single embedding failed for page %d\n",
                                        result->items[idx].page_num);
                            }
                        }
                    }
                } else {
                    for (int j = 0; j < bcount; j++) {
                        int idx = batch_start + j;
                        if (onnx_embedder_encode(g_embedder, result->items[idx].text, 
                                                 all_vectors + idx * EMBEDDING_DIM) != 0) {
                            fprintf(stderr, "    Single embedding failed for page %d\n",
                                    result->items[idx].page_num);
                        }
                    }
                }
            }
            #undef EMBED_BATCH_SIZE
            
            // 将向量附加到每个 item
            for (int i = 0; i < item_count; i++) {
                float* vec = all_vectors + i * EMBEDDING_DIM;
                float norm_sq = 0;
                for (int d = 0; d < EMBEDDING_DIM; d++) norm_sq += vec[d] * vec[d];
                if (norm_sq > 1e-12f) {
                    result->items[i].vector = malloc(sizeof(float) * EMBEDDING_DIM);
                    if (result->items[i].vector) {
                        memcpy(result->items[i].vector, vec, sizeof(float) * EMBEDDING_DIM);
                    }
                }
            }
            free(all_vectors);
        }
    }
    
    // 注意：paragraphs 数组的内存被 items[].text 借用，现在释放
    // 但每个 item->text 指向 paragraphs[i]，所以不能在 free_paragraphs 之前释放 items
    // 实际上 items 会拷贝 text 内容到 cache，所以这里先保留 paragraphs
    // 在 write_chapter_results 之后统一释放
    // 为简化，我们复制 text 到 item
    for (int i = 0; i < item_count; i++) {
        char* copied = strdup(result->items[i].text);
        if (copied) result->items[i].text = copied;
    }
    free_paragraphs(paragraphs, para_count);
    
    return result;
}

/*
 * 将处理后的章节结果写入磁盘和 KV Cache（串行阶段，线程安全）
 * tmp_md_dir: 如果非 NULL，md 文件先写入 tmpfs，KV Cache 仍记录最终路径
 */
static int write_chapter_results(cache_t* cache, const char* namespace,
                                  chapter_result_t* result, const char* tmp_md_dir) {
    if (!result || result->item_count == 0) return 0;
    
    char key[1024];
    char value[4096];
    
    // 创建章节目录（最终目录，用于 KV Cache 记录）
    // 如果使用 tmpfs，最终目录在后面 mv 时自动创建
    if (!tmp_md_dir) {
        mkdir_p(result->chapter_md_dir);
    }
    
    // 保存章节元数据到 KV Cache
    snprintf(key, sizeof(key), "%s/chapters/%s/_meta", namespace, result->chapter_dirname);
    snprintf(value, sizeof(value),
             "{\"type\":\"chapter\",\"title\":\"%s\",\"md_dir\":\"%s\"}",
             result->chapter_title, result->chapter_md_dir);
    cache_set(cache, key, value, 0);
    
    // 写入所有页面
    for (int i = 0; i < result->item_count; i++) {
        page_item_t* item = &result->items[i];
        
        // 确定实际写入路径
        const char* write_path = item->md_path;
        char tmp_path[2048];
        if (tmp_md_dir) {
            // 提取相对路径（/chapters/...）
            const char* rel = strstr(item->md_path, "/chapters/");
            if (rel) {
                snprintf(tmp_path, sizeof(tmp_path), "%s%s", tmp_md_dir, rel);
                write_path = tmp_path;
                // 确保 tmp 目录存在
                char dir_buf[1024];
                strncpy(dir_buf, tmp_path, sizeof(dir_buf) - 1);
                dir_buf[sizeof(dir_buf) - 1] = '\0';
                char* last_slash = strrchr(dir_buf, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    mkdir_p(dir_buf);
                }
            }
        }
        
        // 保存 Markdown 文件（写到 tmpfs 或最终目录）
        FILE* fp = fopen(write_path, "w");
        if (fp) {
            fprintf(fp, "---\n");
            fprintf(fp, "Page: chapters/%s/page_%04d\n", result->chapter_dirname, item->page_num);
            fprintf(fp, "File: %s\n", item->md_path);  // KV Cache 中记录最终路径
            fprintf(fp, "---\n\n");
            fprintf(fp, "<!--\n");
            fprintf(fp, "  Auto-generated by import_book\n");
            fprintf(fp, "  Page: %d\n", item->page_num);
            fprintf(fp, "  Chapter: %s\n", result->chapter_dirname);
            fprintf(fp, "-->\n\n");
            fprintf(fp, "%s\n", item->text);
            fclose(fp);
        }
        
        // 存入 KV Cache（md_file 始终为最终路径）
        snprintf(key, sizeof(key), "%s/chapters/%s/page_%04d",
                 namespace, result->chapter_dirname, item->page_num);
        
        snprintf(value, sizeof(value),
                 "{\"type\":\"page\",\"md_file\":\"%s\",\"preview\":\"%s\",\"content_len\":%zu}",
                 item->md_path, item->preview_escaped, item->content_len);
        
        cache_set(cache, key, value, 0);
        
        if (item->vector) {
            int vret = cache_set_vector(cache, key, value, item->vector, EMBEDDING_DIM, 0);
            if (vret != CACHE_OK) {
                fprintf(stderr, "    Failed to store vector for page %d: %d\n", 
                        item->page_num, vret);
            }
        }
    }
    
    return result->item_count;
}

/*
 * 释放章节处理结果
 */
static void free_chapter_result(chapter_result_t* result) {
    if (!result) return;
    if (result->items) {
        for (int i = 0; i < result->item_count; i++) {
            free(result->items[i].text);
            free(result->items[i].vector);
        }
        free(result->items);
    }
    free(result);
}

/*
 * 导入单个章节（兼容旧接口，内部使用两阶段处理）
 */
static int import_chapter(cache_t* cache, const char* namespace,
                          const char* md_dir, const char* chapter_dirname,
                          const char* chapter_title,
                          const char* text, size_t text_len) {
    chapter_result_t* result = process_chapter(md_dir, chapter_dirname, chapter_title, text, text_len);
    if (!result) return 0;
    
    int pages = write_chapter_results(cache, namespace, result, NULL);
    free_chapter_result(result);
    
    printf("  Chapter '%s': %d valid pages\n", chapter_title, pages);
    return pages;
}

/*
 * 导入整本书
 */
static int import_book(cache_t* cache, const char* namespace,
                       const char* md_dir,
                       const char* title, const char* author,
                       const char* text, size_t text_len,
                       ChapterInfo* chapters, int chapter_count) {
    metric_timer_ctx_t import_timer = metric_timer_start("mydb_import_duration_seconds", "format=text");
    char key[1024];
    char value[4096];
    
    // 创建书籍输出目录
    mkdir_p(md_dir);
    
    // 存储书籍元数据
    snprintf(key, sizeof(key), "%s/_meta", namespace);
    snprintf(value, sizeof(value),
             "{\"type\":\"book\",\"title\":\"%s\",\"author\":\"%s\",\"chapters\":%d}",
             title, author, chapter_count);
    cache_set(cache, key, value, 0);
    
    // 保存元数据到磁盘（方便直接读取）
    char meta_file[1024];
    snprintf(meta_file, sizeof(meta_file), "%s/_meta.json", md_dir);
    FILE* meta_fp = fopen(meta_file, "w");
    if (meta_fp) {
        fprintf(meta_fp, "{\n");
        fprintf(meta_fp, "  \"title\": \"%s\",\n", title);
        fprintf(meta_fp, "  \"author\": \"%s\",\n", author);
        fprintf(meta_fp, "  \"chapters\": %d,\n", chapter_count);
        fprintf(meta_fp, "  \"total_paragraphs\": 0,\n");
        fprintf(meta_fp, "  \"imported_at\": \"%s\"\n", "2026-01-01");
        fprintf(meta_fp, "}\n");
        fclose(meta_fp);
    }
    
    printf("Importing book: %s by %s\n", title, author);
    printf("Output directory: %s\n", md_dir);
    printf("Namespace: %s\n", namespace);
    printf("Chapters: %d\n", chapter_count);
    printf("\n");
    
    int total_pages = 0;
    
    if (chapter_count <= 0 || chapters == NULL) {
        // 没有章节信息，整本书作为一个章节
        char chapter_ns[256];
        snprintf(chapter_ns, sizeof(chapter_ns), "full");
        
        total_pages = import_chapter(cache, namespace, md_dir, chapter_ns,
                                     title, text, text_len);
    } else {
        // 按章节导入
        // 过滤深层级小节，避免空章节和重复内容
        int main_chapter_count = 0;
        for (int i = 0; i < chapter_count; i++) {
            if (chapters[i].level < 2) main_chapter_count++;
        }
        printf("Main chapter count: %d\n", main_chapter_count);
        
        // ====== 阶段一：串行收集所有章节边界和目录名 ======
        typedef struct {
            char ch_dir[256];
            const char* chapter_text;
            size_t chapter_len;
            int valid;
        } chapter_bound_t;
        
        chapter_bound_t* bounds = calloc(chapter_count, sizeof(chapter_bound_t));
        if (!bounds) {
            fprintf(stderr, "Failed to allocate chapter bounds\n");
            return 0;
        }
        
        int chapter_idx = 0;
        int valid_chapter_count = 0;
        for (int i = 0; i < chapter_count; i++) {
            // 跳过深层级小节（level >= 2）
            if (chapters[i].level >= 2) continue;
            
            // 跳过 Part 级别章节
            const char* p = chapters[i].title;
            while (*p && (*p == ' ' || *p == '\t')) p++;
            if (strncasecmp(p, "Part", 4) == 0 && chapters[i].level == 0) continue;
            
            chapter_dirname(bounds[valid_chapter_count].ch_dir, 
                           sizeof(bounds[valid_chapter_count].ch_dir), chapter_idx, chapters[i].title);
            chapter_idx++;
            
            // 定位章节文本范围（串行，因为涉及全局 text 指针运算）
            const char* chapter_text = text;
            size_t chapter_len = text_len;
            const char* best_pos = NULL;
            size_t best_len = 0;
            
            if (main_chapter_count > 1) {
                // 策略0: 优先使用MOBI wrapper提供的offset信息
                if (chapters[i].offset > 0 && chapters[i].offset < text_len) {
                    chapter_text = text + chapters[i].offset;
                    size_t next_offset = text_len;
                    for (int j = i + 1; j < chapter_count; j++) {
                        if (chapters[j].level < 2 && chapters[j].offset > chapters[i].offset 
                            && chapters[j].offset < text_len) {
                            next_offset = chapters[j].offset;
                            break;
                        }
                    }
                    chapter_len = next_offset - chapters[i].offset;
                    if (chapter_len > 100 && chapter_len < text_len * 3 / 10) {
                        goto store_bound;
                    }
                }
                
                // 策略1: 行首标题匹配
                best_pos = NULL;
                best_len = 0;
                const char* search = text;
                size_t max_chapter_len = text_len / 3;
                
                while (search < text + text_len) {
                    const char* found = strstr(search, chapters[i].title);
                    if (!found) break;
                    int is_line_start = (found == text) || 
                                        (found[-1] == '\n') || (found[-1] == '\r') ||
                                        (found[-1] == ' ') || (found[-1] == '\t');
                    size_t body_len = text_len - (found - text);
                    if (i < chapter_count - 1) {
                        const char* next_title = strstr(found + 1, chapters[i+1].title);
                        if (next_title) body_len = next_title - found;
                    }
                    if (!is_line_start && body_len > max_chapter_len) {
                        search = found + 1; continue;
                    }
                    if (body_len < 1000) {
                        search = found + 1; continue;
                    }
                    if (body_len > best_len) {
                        best_len = body_len;
                        best_pos = found;
                    }
                    search = found + 1;
                }
                
                // 策略2: 章节号前缀匹配
                if (!best_pos) {
                    char chapter_prefix[64] = {0};
                    const char* p = chapters[i].title;
                    while (*p && (*p == ' ' || *p == '\t')) p++;
                    if (strncmp(p, "第", 3) == 0 || strncmp(p, "推荐序", 9) == 0 ||
                        strncmp(p, "附录", 6) == 0 || strncmp(p, "后记", 6) == 0) {
                        const char* end = p;
                        while (*end && *end != ' ' && *end != '\t') end++;
                        size_t prefix_len = end - p;
                        if (prefix_len < sizeof(chapter_prefix)) {
                            memcpy(chapter_prefix, p, prefix_len);
                            chapter_prefix[prefix_len] = '\0';
                        }
                    }
                    if (chapter_prefix[0]) {
                        search = text;
                        while (search < text + text_len) {
                            const char* found = strstr(search, chapter_prefix);
                            if (!found) break;
                            size_t body_len = text_len - (found - text);
                            if (i < chapter_count - 1) {
                                char next_prefix[64] = {0};
                                const char* np = chapters[i+1].title;
                                while (*np && (*np == ' ' || *np == '\t')) np++;
                                if (strncmp(np, "第", 3) == 0 || strncmp(np, "推荐序", 9) == 0 ||
                                    strncmp(np, "附录", 6) == 0 || strncmp(np, "后记", 6) == 0) {
                                    const char* end = np;
                                    while (*end && *end != ' ' && *end != '\t') end++;
                                    size_t prefix_len = end - np;
                                    if (prefix_len < sizeof(next_prefix)) {
                                        memcpy(next_prefix, np, prefix_len);
                                        next_prefix[prefix_len] = '\0';
                                    }
                                }
                                if (next_prefix[0]) {
                                    const char* next_found = strstr(found + strlen(chapter_prefix), next_prefix);
                                    if (next_found) body_len = next_found - found;
                                }
                            }
                            if (body_len > max_chapter_len) {
                                search = found + 1; continue;
                            }
                            if (body_len > best_len) {
                                best_len = body_len;
                                best_pos = found;
                            }
                            search = found + 1;
                        }
                    }
                }
                
                // 策略3: 英文书章节前缀匹配
                if (!best_pos) {
                    char chapter_prefix[64] = {0};
                    const char* p = chapters[i].title;
                    while (*p && (*p == ' ' || *p == '\t')) p++;
                    if (isdigit((unsigned char)*p)) {
                        int chapter_num = atoi(p);
                        if (chapter_num > 0) {
                            snprintf(chapter_prefix, sizeof(chapter_prefix), "Chapter %d:", chapter_num);
                        }
                    } else if (strncasecmp(p, "chapter", 7) == 0) {
                        const char* end = p + 7;
                        while (*end && (*end == ' ' || isdigit((unsigned char)*end))) end++;
                        size_t prefix_len = end - p;
                        if (prefix_len > 7 && prefix_len < sizeof(chapter_prefix)) {
                            memcpy(chapter_prefix, p, prefix_len);
                            chapter_prefix[prefix_len] = '\0';
                        }
                    }
                    if (chapter_prefix[0]) {
                        search = text;
                        while (search < text + text_len) {
                            const char* found = strstr(search, chapter_prefix);
                            if (!found) break;
                            int is_line_start = (found == text) || 
                                                (found[-1] == '\n') || (found[-1] == '\r') ||
                                                (found[-1] == ' ') || (found[-1] == '\t');
                            size_t body_len = text_len - (found - text);
                            if (i < chapter_count - 1) {
                                char next_prefix[64] = {0};
                                const char* np = chapters[i+1].title;
                                while (*np && (*np == ' ' || *np == '\t')) np++;
                                if (isdigit((unsigned char)*np)) {
                                    int next_num = atoi(np);
                                    if (next_num > 0) {
                                        snprintf(next_prefix, sizeof(next_prefix), "Chapter %d", next_num);
                                    }
                                } else if (strncasecmp(np, "chapter", 7) == 0) {
                                    const char* end = np + 7;
                                    while (*end && (*end == ' ' || isdigit((unsigned char)*end))) end++;
                                    size_t prefix_len = end - np;
                                    if (prefix_len > 7 && prefix_len < sizeof(next_prefix)) {
                                        memcpy(next_prefix, np, prefix_len);
                                        next_prefix[prefix_len] = '\0';
                                    }
                                }
                                if (next_prefix[0]) {
                                    const char* next_found = strstr(found + strlen(chapter_prefix), next_prefix);
                                    if (next_found) body_len = next_found - found;
                                }
                            }
                            if (!is_line_start && body_len > max_chapter_len) {
                                search = found + 1; continue;
                            }
                            if (body_len > best_len) {
                                best_len = body_len;
                                best_pos = found;
                            }
                            search = found + 1;
                        }
                    }
                }
                
                if (best_pos) {
                    chapter_text = best_pos;
                    chapter_len = best_len;
                }
            }
            
            store_bound:
            if (chapter_len > 200000) {
                fprintf(stderr, "  Warning: Chapter '%s' too long (%zu bytes), truncating to 200KB\n", 
                        chapters[i].title, chapter_len);
                chapter_len = 200000;
            }
            
            bounds[valid_chapter_count].chapter_text = chapter_text;
            bounds[valid_chapter_count].chapter_len = chapter_len;
            bounds[valid_chapter_count].valid = 1;
            valid_chapter_count++;
        }
        
        // ====== 阶段二：并行处理所有章节（CPU/GPU 密集）======
        printf("  Processing chapters (serial for vector safety)...\n", valid_chapter_count);
        
        chapter_result_t** results = calloc(valid_chapter_count, sizeof(chapter_result_t*));
        if (!results) {
            fprintf(stderr, "Failed to allocate results array\n");
            free(bounds);
            return 0;
        }
        
        // serial (vector safety)
        for (int i = 0; i < valid_chapter_count; i++) {
            if (!bounds[i].valid) continue;
            
            // 找到对应的 chapter 标题
            int ch_idx = 0;
            const char* title = "Unknown";
            for (int j = 0; j < chapter_count; j++) {
                if (chapters[j].level >= 2) continue;
                const char* p = chapters[j].title;
                while (*p && (*p == ' ' || *p == '\t')) p++;
                if (strncasecmp(p, "Part", 4) == 0 && chapters[j].level == 0) continue;
                if (ch_idx == i) {
                    title = chapters[j].title;
                    break;
                }
                ch_idx++;
            }
            
            results[i] = process_chapter(md_dir, bounds[i].ch_dir, title,
                                         bounds[i].chapter_text, bounds[i].chapter_len);
        }
        
        // ====== 阶段三：串行写入所有结果（磁盘 + KV Cache）======
            // 创建 tmpfs 临时目录用于高速写入
            const char* book_name_ptr = strrchr(namespace, '/');
            if (!book_name_ptr) book_name_ptr = namespace;
            else book_name_ptr++;
            char tmp_md_dir[1024];
            snprintf(tmp_md_dir, sizeof(tmp_md_dir), "/dev/shm/import_book_%s_%d", book_name_ptr, getpid());
            mkdir_p(tmp_md_dir);
            
            printf("  Writing %d chapters to disk and cache (tmpfs)...\n", valid_chapter_count);
            for (int i = 0; i < valid_chapter_count; i++) {
                if (results[i]) {
                    int pages = write_chapter_results(cache, namespace, results[i], tmp_md_dir);
                    total_pages += pages;
                    free_chapter_result(results[i]);
                }
            }
            
            // 将 tmpfs 中的文件批量移动到最终目录
            // 先删除可能存在的旧 chapters 目录（来自之前失败的导入或 write_chapter_results 的 mkdir_p）
            printf("  Moving files from tmpfs to %s...\n", md_dir);
            char src_chapters[1024];
            char dst_chapters[1024];
            snprintf(src_chapters, sizeof(src_chapters), "%s/chapters", tmp_md_dir);
            snprintf(dst_chapters, sizeof(dst_chapters), "%s/chapters", md_dir);
            // 删除已存在的目标目录，避免 mv 失败
            char rm_cmd[2048];
            snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s 2>/dev/null; mv %s %s", dst_chapters, src_chapters, dst_chapters);
            if (system(rm_cmd) != 0) {
                // fallback: cp + rm
                snprintf(rm_cmd, sizeof(rm_cmd), "cp -r %s %s && rm -rf %s", src_chapters, md_dir, src_chapters);
                if (system(rm_cmd) != 0) {
                    fprintf(stderr, "Warning: Failed to move files from tmpfs\n");
                }
            }
            
            free(results);
            free(bounds);
    }
    
    // 更新元数据中的总页数
    snprintf(key, sizeof(key), "%s/_meta", namespace);
    snprintf(value, sizeof(value),
             "{\"type\":\"book\",\"title\":\"%s\",\"author\":\"%s\",\"chapters\":%d,\"pages\":%d}",
             title, author, chapter_count, total_pages);
    cache_set(cache, key, value, 0);
    
    METRIC_COUNTER_INC("mydb_imports_total", "format=text");
    METRIC_COUNTER_INC("mydb_import_pages_total", "format=text");
    LOG_INFO("import.complete", "\"title\":\"%s\",\"pages\":%d", title, total_pages);
    metric_timer_stop(&import_timer);
    
    return total_pages;
}

// =============================================================================
// 向量导出（供 vector_engine 语义搜索使用）
// =============================================================================

typedef struct {
    char* name;
    float* vector;
} VectorEntry;

static int export_vectors(cache_t* cache, const char* cache_dir,
                          const char* namespace, const char* book_name) {
    // 创建 vectors 目录
    char vec_dir[1024];
    snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    mkdir_p(vec_dir);
    
    // 构建文件名（禁用 snprintf 截断警告，实际运行时路径不会超长）
    char bin_file[1024], idx_file[1024];
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(bin_file, sizeof(bin_file), "%s/books_%s.jina.bin", vec_dir, book_name);
    snprintf(idx_file, sizeof(idx_file), "%s/books_%s.jina.idx", vec_dir, book_name);
    #pragma GCC diagnostic pop
    
    // 收集所有带向量的 entry
    VectorEntry* entries = NULL;
    int count = 0;
    int capacity = 1000;
    entries = malloc(sizeof(VectorEntry) * capacity);
    if (!entries) return -1;
    
    size_t ns_len = strlen(namespace);
    
    // 先统计总页数（用于进度显示）
    int total_pages_approx = 0;
    cache_iter_t* count_iter = cache_iter_create(cache);
    if (count_iter) {
        const char* k;
        const char* v;
        while (cache_iter_next(count_iter, &k, &v) == 1) {
            if (strncmp(k, namespace, ns_len) != 0) continue;
            if (strstr(k, "/_meta")) continue;
            if (!strstr(k, "/page_")) continue;
            total_pages_approx++;
        }
        cache_iter_destroy(count_iter);
    }
    if (total_pages_approx > 0) {
        printf("  Found %d pages, collecting vectors...\n", total_pages_approx);
    }
    
    cache_iter_t* iter = cache_iter_create(cache);
    if (!iter) {
        free(entries);
        return -1;
    }
    
    const char* key;
    const char* value;
    int progress_counter = 0;
    int next_progress = 0;
    int progress_step = (total_pages_approx > 100) ? total_pages_approx / 20 : 1;  // 5% intervals
    
    clock_t progress_start = clock();
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        // 只收集属于本书的 key
        if (strncmp(key, namespace, ns_len) != 0) continue;
        // 跳过 _meta 等非页面 key
        if (strstr(key, "/_meta")) continue;
        if (!strstr(key, "/page_")) continue;
        
        size_t dim = 0;
        const float* vec = cache_get_vector(cache, key, &dim);
        if (!vec || dim == 0) continue;
        
        if (count >= capacity) {
            capacity *= 2;
            VectorEntry* new_entries = realloc(entries, sizeof(VectorEntry) * capacity);
            if (!new_entries) break;
            entries = new_entries;
        }
        
        // 进度显示（每 5% 输出一次）
        progress_counter++;
        if (total_pages_approx > 0 && progress_counter >= next_progress) {
            int pct = (progress_counter * 100) / total_pages_approx;
            if (pct > 100) pct = 100;
            double elapsed = (double)(clock() - progress_start) / CLOCKS_PER_SEC;
            double rate = (elapsed > 0) ? progress_counter / elapsed : 0;
            printf("\r    Progress: %d/%d (%d%%) - %.0f vectors/s  ",
                   progress_counter, total_pages_approx, pct, rate);
            fflush(stdout);
            next_progress = progress_counter + progress_step;
        }
        
        entries[count].name = strdup(key);
        entries[count].vector = malloc(sizeof(float) * dim);
        if (entries[count].vector) {
            memcpy(entries[count].vector, vec, sizeof(float) * dim);
        }
        count++;
    }
    cache_iter_destroy(iter);
    
    if (count > 0 && total_pages_approx > 0) {
        double elapsed = (double)(clock() - progress_start) / CLOCKS_PER_SEC;
        printf("\r    Progress: %d/%d (100%%) - %.1fs total          \n", 
               count, total_pages_approx, elapsed);
    }
    
    if (count == 0) {
        free(entries);
        return 0;
    }
    
    // 写入 bin 文件
    FILE* bin_fp = fopen(bin_file, "wb");
    FILE* idx_fp = fopen(idx_file, "w");
    if (!bin_fp || !idx_fp) {
        if (bin_fp) fclose(bin_fp);
        if (idx_fp) fclose(idx_fp);
        for (int i = 0; i < count; i++) {
            free(entries[i].name);
            free(entries[i].vector);
        }
        free(entries);
        return -1;
    }
    
    uint32_t h_count = count;
    uint32_t h_dim = EMBEDDING_DIM;
    fwrite(&h_count, 4, 1, bin_fp);
    fwrite(&h_dim, 4, 1, bin_fp);
    
    size_t offset = 8;
    for (int i = 0; i < count; i++) {
        uint32_t name_len = strlen(entries[i].name);
        fwrite(&name_len, 4, 1, bin_fp);
        fwrite(entries[i].name, 1, name_len, bin_fp);
        
        // 归一化
        float norm = 0;
        for (int j = 0; j < EMBEDDING_DIM; j++) {
            norm += entries[i].vector[j] * entries[i].vector[j];
        }
        norm = sqrtf(norm);
        if (norm > 1e-12f) {
            for (int j = 0; j < EMBEDDING_DIM; j++) {
                entries[i].vector[j] /= norm;
            }
        }
        
        fwrite(entries[i].vector, sizeof(float), EMBEDDING_DIM, bin_fp);
        
        // idx 偏移指向 vec_data
        fprintf(idx_fp, "\"%s\":%zu\n", entries[i].name, offset + 4 + name_len);
        
        offset += 4 + name_len + EMBEDDING_DIM * sizeof(float);
    }
    
    fclose(bin_fp);
    fclose(idx_fp);
    
    printf("\nVectors exported:\n");
    printf("  Binary: %s (%d vectors)\n", bin_file, count);
    printf("  Index:  %s\n", idx_file);
    
    // 构建 HNSW 索引（语义搜索必需）
    struct stat st_hnsw;
    if (stat("./tools/build_hnsw_index", &st_hnsw) != 0) {
        printf("  Warning: build_hnsw_index not found. Building...\n");
        int make_ret = system("make tools/build_hnsw_index >/dev/null 2>&1");
        if (make_ret != 0) {
            printf("  Warning: Failed to build HNSW index tool (search may not work)\n");
            printf("  Fix: run 'make tools/build_hnsw_index' manually\n");
        }
    }
    
    char hnsw_cmd[2048];
    snprintf(hnsw_cmd, sizeof(hnsw_cmd),
             "./tools/build_hnsw_index %s 2>&1", bin_file);
    int hnsw_ret = system(hnsw_cmd);
    if (hnsw_ret == 0) {
        printf("  HNSW index built successfully\n");
    } else {
        printf("  Warning: HNSW build failed (search may not work)\n");
    }
    
    for (int i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].vector);
    }
    free(entries);
    
    return count;
}

// =============================================================================
// 动态库加载
// =============================================================================

static void* load_mobi_lib(void) {
    void* lib = dlopen("src/importer/libs/libmobiparse.so", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "Failed to load libmobiparse.so: %s\n", dlerror());
        return NULL;
    }
    return lib;
}

static void* load_pdf_lib(void) {
    void* lib = dlopen("src/importer/libs/libpdfparse.so", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "Failed to load libpdfparse.so: %s\n", dlerror());
        return NULL;
    }
    return lib;
}

static void* load_epub_lib(void) {
    void* lib = dlopen("src/importer/libs/libepubparse.so", RTLD_LAZY | RTLD_DEEPBIND);
    if (!lib) {
        fprintf(stderr, "Failed to load libepubparse.so: %s\n", dlerror());
        return NULL;
    }
    return lib;
}

// =============================================================================
// OCR 提取（通过 ocr_cuda C++ 二进制）
// =============================================================================

// Simple JSON chapter parser for the format produced by ocr_cuda:
// {"chapters":[{"title":"...","level":1,"page":0},...]}
// Returns 0 on success, -1 on parse error.
static int parse_chapters_json(const char* json, ChapterInfo** out_chapters, int* out_count) {
    *out_chapters = NULL;
    *out_count = 0;
    
    if (!json) return -1;
    
    // Find the chapters array
    const char* arr = strstr(json, "\"chapters\"");
    if (!arr) return 0;  // no chapters key, that's OK
    
    arr = strchr(arr, '[');
    if (!arr) return -1;
    arr++;  // skip '['
    
    const char* end = strrchr(arr, ']');
    if (!end) return -1;
    
    // Count entries by counting '{'
    int count = 0;
    for (const char* p = arr; p < end; p++) {
        if (*p == '{') count++;
    }
    if (count == 0) return 0;
    
    ChapterInfo* chapters = calloc(count, sizeof(ChapterInfo));
    if (!chapters) return -1;
    
    int idx = 0;
    const char* p = arr;
    while (p < end && idx < count) {
        // Skip to next '{'
        while (p < end && *p != '{') p++;
        if (p >= end) break;
        
        // Find matching '}'
        int brace = 1;
        const char* entry_end = p + 1;
        while (entry_end < end && brace > 0) {
            if (*entry_end == '{') brace++;
            else if (*entry_end == '}') brace--;
            entry_end++;
        }
        if (brace != 0) break;  // malformed
        
        // title
        const char* t = strstr(p, "\"title\":\"");
        if (t && t < entry_end) {
            t += 9;  // skip past "\"title\":\""
            const char* t_end = strchr(t, '"');
            if (t_end && t_end < entry_end) {
                size_t tlen = t_end - t;
                chapters[idx].title = malloc(tlen + 1);
                if (chapters[idx].title) {
                    memcpy(chapters[idx].title, t, tlen);
                    chapters[idx].title[tlen] = '\0';
                }
            }
        }
        if (!chapters[idx].title) {
            chapters[idx].title = strdup("");
        }
        
        // level
        const char* l = strstr(p, "\"level\":");
        if (l && l < entry_end) {
            l += 8;  // skip past "\"level\":"
            chapters[idx].level = atoi(l);
        }
        
        // page
        const char* pg = strstr(p, "\"page\":");
        if (pg && pg < entry_end) {
            pg += 7;  // skip past "\"page\":"
            chapters[idx].offset = (size_t)atoi(pg);
        }
        
        idx++;
        p = entry_end;
    }
    
    *out_chapters = chapters;
    *out_count = idx;
    return 0;
}

// Run ocr_cuda on a PDF, extract text + chapters.
// Uses shared ocr_helper for binary location and I/O.
// Returns 0 on success, -1 on failure.
static int ocr_extract_pages(const char* book_path, const char* exe_path,
                              char** out_text, size_t* out_text_len,
                              ChapterInfo** out_chapters, int* out_chapter_count) {
    *out_text = NULL;
    *out_text_len = 0;
    *out_chapters = NULL;
    *out_chapter_count = 0;

    // Locate ocr_cuda binary via shared helper
    char ocr_bin[1024];
    ocr_find_binary(exe_path, ocr_bin, sizeof(ocr_bin));

    // Run OCR: get chapters JSON + raw text
    char *chapters_json = NULL;
    if (ocr_run(book_path, ocr_bin, &chapters_json, out_text, out_text_len) != 0) {
        fprintf(stderr, "OCR extraction failed\n");
        free(chapters_json);
        return -1;
    }

    // Parse chapters JSON
    if (chapters_json) {
        if (parse_chapters_json(chapters_json, out_chapters, out_chapter_count) != 0) {
            fprintf(stderr, "Warning: failed to parse OCR chapters JSON\n");
        }
        free(chapters_json);
    }

    printf("OCR chapters: %d\n", *out_chapter_count);
    printf("OCR text length: %zu bytes\n", *out_text_len);
    printf("\n");

    return 0;
}

// =============================================================================
// 主函数
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <cache_dir> <book_file> [namespace] [output_dir] [options]\n", argv[0]);
        printf("\n");
        printf("Arguments:\n");
        printf("  cache_dir   KV Cache 目录 (默认 /book/cache)\n");
        printf("  book_file   电子书文件 (.mobi, .azw, .azw3, .pdf, .epub)\n");
        printf("  namespace   命名空间 (可选，默认从文件名生成)\n");
        printf("  output_dir  Markdown 输出目录 (可选，默认 /opt/books)\n");
        printf("\n");
        printf("Options:\n");
        printf("  --skip-vectors   跳过向量生成（更快，但无语义搜索）\n");
        printf("  --only-vectors   仅重新导出已有向量（需已生成过）\n");
        printf("  --generate-vectors 为已导入但无向量的页面生成向量\n");
        printf("  --ocr            对 PDF 使用 Unlimited-OCR 高精度识别（替代 MuPDF）\n");
        printf("\n");
        printf("Examples:\n");
        printf("  %s /memory ~/book.mobi /books/my_book\n", argv[0]);
        printf("  %s /memory ~/paper.pdf\n", argv[0]);
        printf("  %s /memory ~/paper.pdf --skip-vectors\n", argv[0]);
        printf("  %s /memory /books/my_book --only-vectors\n", argv[0]);
        printf("\n");
        printf("Output structure:\n");
        printf("  /opt/books/{book_name}/\n");
        printf("    ├── _meta.json\n");
        printf("    └── chapters/\n");
        printf("        └── 01-Chapter_Title/\n");
        printf("            └── page_0000.md\n");
        return 1;
    }
    
    int flag_skip_vectors = 0;
    int flag_only_vectors = 0;
    int flag_generate_vectors = 0;
    int flag_ocr = 0;
    
    // 解析 flags
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--skip-vectors") == 0) {
            flag_skip_vectors = 1;
        } else if (strcmp(argv[i], "--only-vectors") == 0) {
            flag_only_vectors = 1;
        } else if (strcmp(argv[i], "--generate-vectors") == 0) {
            flag_generate_vectors = 1;
        } else if (strcmp(argv[i], "--ocr") == 0) {
            flag_ocr = 1;
        }
    }
    
    // 过滤 flags 后的位置参数
    int pos_count = 0;
    const char* pos_args[4];
    for (int i = 1; i < argc && pos_count < 4; i++) {
        if (strcmp(argv[i], "--skip-vectors") == 0 || strcmp(argv[i], "--only-vectors") == 0 || strcmp(argv[i], "--generate-vectors") == 0 || strcmp(argv[i], "--ocr") == 0) {
            continue;
        }
        pos_args[pos_count++] = argv[i];
    }
    
    if (pos_count < 2) {
        printf("Usage: %s <cache_dir> <book_file> [namespace] [output_dir] [options]\n", argv[0]);
        printf("  Use --help for details\n");
        cache_t* dummy = NULL; // avoid unused warning
        (void)dummy;
        return 1;
    }
    
    const char* cache_dir = pos_args[0];
    const char* book_path = (flag_only_vectors || flag_generate_vectors) ? NULL : pos_args[1];
    const char* ns = NULL;
    const char* output_dir = "/opt/books";
    int next_pos = (flag_only_vectors || flag_generate_vectors) ? 1 : 2;
    
    // 智能解析可选参数
    if (pos_count > next_pos) {
        if (pos_args[next_pos][0] == '/') {
            ns = pos_args[next_pos];
            if (pos_count > next_pos + 1) {
                output_dir = pos_args[next_pos + 1];
            }
        } else {
            output_dir = pos_args[next_pos];
        }
    }
    
    // --only-vectors 需要 namespace
    if ((flag_only_vectors || flag_generate_vectors) && !ns) {
        fprintf(stderr, "--only-vectors requires a namespace (e.g., /books/my_book)\n");
        return 1;
    }
    
    // --only-vectors: 跳过文本解析，直接导出向量
    if (flag_only_vectors) {
        printf("Only-vectors mode: skipping text parsing\n");
        printf("Namespace: %s\n", ns);
        printf("Cache: %s\n", cache_dir);
        printf("\n");
        
        // 从 namespace 提取 book_name
        char ns_book_name[128];
        const char* last_slash = strrchr(ns, '/');
        strncpy(ns_book_name, last_slash ? last_slash + 1 : ns, sizeof(ns_book_name) - 1);
        ns_book_name[sizeof(ns_book_name) - 1] = '\0';
        
        // 只导出向量，不重新导入文本
        cache_t* exp_cache = cache_open(cache_dir, 4ULL * 1024 * 1024 * 1024);
        if (!exp_cache) {
            fprintf(stderr, "Failed to open cache: %s\n", cache_dir);
            return 1;
        }
        printf("Cache opened: %s\n", cache_dir);
        
        g_embedder = onnx_embedder_init(MODEL_PATH, VOCAB_PATH, MODEL_SEQ_LEN, EMBEDDING_DIM);
        if (!g_embedder) {
            fprintf(stderr, "Failed to load embedding model (%s)\n", onnx_embedder_error());
            cache_close(exp_cache);
            return 1;
        }
        printf("Embedding model loaded\n");
        
        int exported = export_vectors(exp_cache, cache_dir, ns, ns_book_name);
        if (exported > 0) {
            printf("\nDone: %d vectors exported for %s\n", exported, ns);
        } else {
            printf("\nNo vectors found for %s\n", ns);
        }
        
        onnx_embedder_free(g_embedder);
        g_embedder = NULL;
        cache_close(exp_cache);
        return (exported >= 0) ? 0 : 1;
    }
    
    // --generate-vectors: 为已导入但无向量的页面生成向量
    if (flag_generate_vectors) {
        printf("Generate-vectors mode: scanning for pages without vectors...\n");
        printf("Namespace: %s\n", ns);
        printf("Cache: %s\n", cache_dir);
        printf("\n");
        
        if (!ns) {
            fprintf(stderr, "--generate-vectors requires a namespace (e.g., /books/my_book)\n");
            return 1;
        }
        
        // Extract book name from namespace
        char ns_book_name2[128];
        const char* last_slash2 = strrchr(ns, '/');
        strncpy(ns_book_name2, last_slash2 ? last_slash2 + 1 : ns, sizeof(ns_book_name2) - 1);
        ns_book_name2[sizeof(ns_book_name2) - 1] = '\0';
        
        cache_t* gen_cache = cache_open(cache_dir, 4ULL * 1024 * 1024 * 1024);
        if (!gen_cache) {
            fprintf(stderr, "Failed to open cache: %s\n", cache_dir);
            return 1;
        }
        printf("Cache opened: %s\n", cache_dir);
        
        g_embedder = onnx_embedder_init(MODEL_PATH, VOCAB_PATH, MODEL_SEQ_LEN, EMBEDDING_DIM);
        if (!g_embedder) {
            fprintf(stderr, "Failed to load embedding model (%s)\n", onnx_embedder_error());
            cache_close(gen_cache);
            return 1;
        }
        printf("Embedding model loaded\n");
        
        // Iterate cache to find pages without vectors
        size_t ns_len2 = strlen(ns);
        cache_iter_t* iter2 = cache_iter_create(gen_cache);
        if (!iter2) {
            onnx_embedder_free(g_embedder);
            g_embedder = NULL;
            cache_close(gen_cache);
            return 1;
        }
        
        int gen_count = 0;
        int skip_count = 0;
        const char* k2;
        const char* v2;
        while (cache_iter_next(iter2, &k2, &v2) == 1) {
            if (strncmp(k2, ns, ns_len2) != 0) continue;
            if (!strstr(k2, "/page_")) continue;
            
            // Check if page already has a vector
            size_t existing_dim = 0;
            const float* existing_vec = cache_get_vector(gen_cache, k2, &existing_dim);
            if (existing_vec && existing_dim > 0) {
                skip_count++;
                continue;
            }
            
            // Parse value JSON to get md_file
            const char* md_found = strstr(v2, "\"md_file\":\"");
            if (!md_found) continue;
            md_found += 11; // skip past "md_file":"
            const char* md_end = strchr(md_found, '"');
            if (!md_end) continue;
            
            char md_path[1024];
            size_t md_len = md_end - md_found;
            if (md_len >= sizeof(md_path)) md_len = sizeof(md_path) - 1;
            memcpy(md_path, md_found, md_len);
            md_path[md_len] = '\0';
            
            // Read markdown content (skip YAML front matter)
            FILE* md_fp = fopen(md_path, "r");
            if (!md_fp) continue;
            
            // Read file, skip front matter (between --- lines)
            char md_content[16384];
            size_t md_content_len = 0;
            char line[1024];
            int in_front = 0;
            int front_skipped = 0;
            while (fgets(line, sizeof(line), md_fp) && md_content_len < sizeof(md_content) - 1) {
                if (strcmp(line, "---\n") == 0 || strcmp(line, "---\r\n") == 0) {
                    if (!front_skipped) { front_skipped = 1; in_front = 1; continue; }
                    if (in_front) { in_front = 0; continue; }
                }
                if (!in_front) {
                    size_t line_len = strlen(line);
                    if (md_content_len + line_len < sizeof(md_content)) {
                        memcpy(md_content + md_content_len, line, line_len);
                        md_content_len += line_len;
                    }
                }
            }
            fclose(md_fp);
            md_content[md_content_len] = '\0';
            
            if (md_content_len < 10) continue;
            
            // Generate vector
            float vec[EMBEDDING_DIM];
            memset(vec, 0, sizeof(vec));
            if (onnx_embedder_encode(g_embedder, md_content, vec) == 0) {
                // Store vector in cache
                cache_set_vector(gen_cache, k2, v2, vec, EMBEDDING_DIM, 0);
                gen_count++;
            }
        }
        cache_iter_destroy(iter2);
        
        printf("\nVector generation complete: %d generated, %d already had vectors\n", gen_count, skip_count);
        
        // Export vectors and build HNSW
        if (gen_count > 0 || skip_count > 0) {
            int exported = export_vectors(gen_cache, cache_dir, ns, ns_book_name2);
            if (exported > 0) {
                printf("\nDone: %d vectors exported for %s\n", exported, ns);
            }
        }
        
        onnx_embedder_free(g_embedder);
        g_embedder = NULL;
        cache_close(gen_cache);
        return 0;
    }
    
    // 确定文件类型
    size_t path_len = strlen(book_path);
    const char* ext = "";
    for (size_t i = path_len; i > 0; i--) {
        if (book_path[i-1] == '.') {
            ext = book_path + i - 1;
            break;
        }
    }
    
    // 打开 KV Cache
    cache_t* cache = cache_open(cache_dir, 4ULL * 1024 * 1024 * 1024);  // 4GB (align with cache_import.c)
    if (!cache) {
        fprintf(stderr, "Failed to open cache: %s\n", cache_dir);
        return 1;
    }
    
    printf("Cache opened: %s\n", cache_dir);
    
    // 初始化 Jina v2 嵌入模型（--skip-vectors 时不初始化，加速导入）
    if (!flag_skip_vectors) {
        printf("Loading embedding model (Jina v2, %d-dim)...\n", EMBEDDING_DIM);
        g_embedder = onnx_embedder_init(MODEL_PATH, VOCAB_PATH, MODEL_SEQ_LEN, EMBEDDING_DIM);
        if (g_embedder) {
            printf("  Embedding model loaded successfully\n");
        } else {
            printf("  Warning: Failed to load embedding model (%s)\n", onnx_embedder_error());
            printf("  Continuing without vector generation\n");
        }
    } else {
        printf("  Skipping embedding model (--skip-vectors)\n");
    }
    
    // 生成命名空间
    char namespace[256];
    char book_name[128];
    if (ns) {
        strncpy(namespace, ns, sizeof(namespace) - 1);
        namespace[sizeof(namespace) - 1] = '\0';
        
        // 从 namespace 提取 book_name
        const char* last_slash = strrchr(ns, '/');
        if (last_slash) {
            strncpy(book_name, last_slash + 1, sizeof(book_name) - 1);
        } else {
            strncpy(book_name, ns, sizeof(book_name) - 1);
        }
        book_name[sizeof(book_name) - 1] = '\0';
    } else {
        const char* basename = strrchr(book_path, '/');
        if (basename) basename++;
        else basename = book_path;
        
        strncpy(book_name, basename, sizeof(book_name) - 1);
        book_name[sizeof(book_name) - 1] = '\0';
        
        char* dot = strrchr(book_name, '.');
        if (dot) *dot = '\0';
        
        snprintf(namespace, sizeof(namespace), "/books/%s", book_name);
    }
    
    // 创建 Markdown 输出目录
    char md_dir[1024];
    snprintf(md_dir, sizeof(md_dir), "%s/%s", output_dir, book_name);
    mkdir_p(md_dir);
    
    printf("Importing: %s\n", book_path);
    printf("Namespace: %s\n", namespace);
    printf("Markdown output: %s/\n", md_dir);
    printf("\n");
    
    // 解析并导入
    int total_pages = 0;
    
    if (strcasecmp(ext, ".mobi") == 0 || strcasecmp(ext, ".azw") == 0 || strcasecmp(ext, ".azw3") == 0) {
        void* lib = load_mobi_lib();
        if (!lib) {
            cache_close(cache);
            return 1;
        }
        
        void* (*mobi_open)(const char*) = dlsym(lib, "mobi_open");
        int (*mobi_extract_text)(void*, char**, size_t*) = dlsym(lib, "mobi_extract_text");
        int (*mobi_get_metadata)(void*, char*, size_t, char*, size_t) = dlsym(lib, "mobi_get_metadata");
        int (*mobi_get_chapters)(void*, void**, int*) = dlsym(lib, "mobi_get_chapters");
        int (*mobi_get_chapter_text)(void*, int, char**, size_t*) = dlsym(lib, "mobi_get_chapter_text");
        void (*mobi_free_chapter_text)(char*) = dlsym(lib, "mobi_free_chapter_text");
        void (*mobi_close)(void*) = dlsym(lib, "mobi_close");
        
        if (!mobi_open || !mobi_extract_text || !mobi_close) {
            fprintf(stderr, "Missing required MOBI functions\n");
            dlclose(lib);
            cache_close(cache);
            return 1;
        }
        
        void* handle = mobi_open(book_path);
        if (!handle) {
            fprintf(stderr, "Failed to open MOBI: %s\n", book_path);
            dlclose(lib);
            cache_close(cache);
            return 1;
        }
        
        char title[512] = {0}, author[512] = {0};
        if (mobi_get_metadata) {
            mobi_get_metadata(handle, title, sizeof(title), author, sizeof(author));
        }
        if (strlen(title) == 0) {
            strncpy(title, book_name, sizeof(title) - 1);
        }
        if (strlen(author) == 0) {
            strcpy(author, "Unknown");
        }
        
        ChapterInfo* chapters = NULL;
        int chapter_count = 0;
        if (mobi_get_chapters) {
            mobi_get_chapters(handle, (void**)&chapters, &chapter_count);
        }
        
        printf("Title: %s\n", title);
        printf("Author: %s\n", author);
        printf("Chapters: %d\n", chapter_count);
        printf("\n");
        
        // 创建书籍输出目录
        mkdir_p(md_dir);
        
        // 存储书籍元数据
        char key[1024];
        char value[4096];
        snprintf(key, sizeof(key), "%s/_meta", namespace);
        snprintf(value, sizeof(value),
                 "{\"type\":\"book\",\"title\":\"%s\",\"author\":\"%s\",\"chapters\":%d}",
                 title, author, chapter_count);
        cache_set(cache, key, value, 0);
        
        printf("Importing book: %s by %s\n", title, author);
        printf("Output directory: %s\n", md_dir);
        printf("Namespace: %s\n", namespace);
        printf("Chapters: %d\n", chapter_count);
        printf("\n");
        
        // MOBI: 尝试使用 mobi_get_chapter_text() 逐章提取（比全文切分更精确）
        int use_per_chapter = (mobi_get_chapter_text != NULL);
        int chapter_idx = 0;
        int imported_chapters = 0;
        
        if (use_per_chapter) {
            for (int i = 0; i < chapter_count; i++) {
                if (chapters[i].level >= 2) continue;
                
                char* chapter_text = NULL;
                size_t chapter_len = 0;
                mobi_get_chapter_text(handle, i, &chapter_text, &chapter_len);
                
                if (!chapter_text || chapter_len < 100) {
                    use_per_chapter = 0;
                    break;
                }
                imported_chapters++;
                if (mobi_free_chapter_text && chapter_text) {
                    mobi_free_chapter_text(chapter_text);
                }
            }
        }
        
        if (use_per_chapter) {
            printf("Using per-chapter text extraction (%d chapters)\n", imported_chapters);
            
            // ====== 并行处理所有 MOBI 章节 ======
            // 先收集所有章节文本
            typedef struct {
                char ch_dir[256];
                char* chapter_text;
                size_t chapter_len;
                const char* title;
            } mobi_chapter_data_t;
            
            mobi_chapter_data_t* mobi_chapters = calloc(imported_chapters, sizeof(mobi_chapter_data_t));
            if (!mobi_chapters) {
                fprintf(stderr, "Failed to allocate mobi chapters array\n");
                use_per_chapter = 0;
                goto mobi_fallback;
            }
            
            chapter_idx = 0;
            for (int i = 0; i < chapter_count; i++) {
                if (chapters[i].level >= 2) continue;
                
                chapter_dirname(mobi_chapters[chapter_idx].ch_dir, 
                               sizeof(mobi_chapters[chapter_idx].ch_dir), chapter_idx, chapters[i].title);
                mobi_chapters[chapter_idx].title = chapters[i].title;
                mobi_get_chapter_text(handle, i, 
                                     &mobi_chapters[chapter_idx].chapter_text, 
                                     &mobi_chapters[chapter_idx].chapter_len);
                chapter_idx++;
            }
            
            // 并行处理所有章节（文本清理、分块、向量生成）
            printf("  Processing chapters (serial for vector safety)...\n", imported_chapters);
            chapter_result_t** results = calloc(imported_chapters, sizeof(chapter_result_t*));
            
            // serial (vector safety)
            for (int i = 0; i < imported_chapters; i++) {
                if (mobi_chapters[i].chapter_text && mobi_chapters[i].chapter_len > 0) {
                    results[i] = process_chapter(md_dir, mobi_chapters[i].ch_dir, 
                                                mobi_chapters[i].title,
                                                mobi_chapters[i].chapter_text, 
                                                mobi_chapters[i].chapter_len);
                }
            }
            
            // 串行写入所有结果
            // 创建 tmpfs 临时目录用于高速写入
            const char* book_name_ptr2 = strrchr(namespace, '/');
            if (!book_name_ptr2) book_name_ptr2 = namespace;
            else book_name_ptr2++;
            char tmp_md_dir[1024];
            snprintf(tmp_md_dir, sizeof(tmp_md_dir), "/dev/shm/import_book_%s_%d", book_name_ptr2, getpid());
            mkdir_p(tmp_md_dir);
            
            printf("  Writing %d chapters to disk and cache (tmpfs)...\n", imported_chapters);
            for (int i = 0; i < imported_chapters; i++) {
                if (results[i]) {
                    int pages = write_chapter_results(cache, namespace, results[i], tmp_md_dir);
                    total_pages += pages;
                    free_chapter_result(results[i]);
                }
                if (mobi_free_chapter_text && mobi_chapters[i].chapter_text) {
                    mobi_free_chapter_text(mobi_chapters[i].chapter_text);
                }
            }
            
            // 将 tmpfs 中的文件批量移动到最终目录
            printf("  Moving files from tmpfs to %s...\n", md_dir);
            char src_chapters2[1024];
            char dst_chapters2[1024];
            snprintf(src_chapters2, sizeof(src_chapters2), "%s/chapters", tmp_md_dir);
            snprintf(dst_chapters2, sizeof(dst_chapters2), "%s/chapters", md_dir);
            // 先删除已存在的目标目录，避免 mv 跨文件系统失败
            char rm_cmd2[2048];
            snprintf(rm_cmd2, sizeof(rm_cmd2), "rm -rf %s 2>/dev/null; mv %s %s", dst_chapters2, src_chapters2, dst_chapters2);
            if (system(rm_cmd2) != 0) {
                // fallback: cp + rm
                snprintf(rm_cmd2, sizeof(rm_cmd2), "cp -r %s %s && rm -rf %s", src_chapters2, md_dir, src_chapters2);
                if (system(rm_cmd2) != 0) {
                    fprintf(stderr, "Warning: Failed to move files from tmpfs\n");
                }
            }
            rmdir(tmp_md_dir);
            
            free(results);
            free(mobi_chapters);
        } else {
            mobi_fallback:
            // Fallback: 提取全文后手动切分（兼容旧格式）
            printf("Using full-text chapter splitting\n");
            char* text = NULL;
            size_t text_len = 0;
            if (mobi_extract_text) {
                mobi_extract_text(handle, &text, &text_len);
            }
            
            if (text && text_len > 0) {
                total_pages = import_book(cache, namespace, md_dir,
                                          title, author, text, text_len,
                                          chapters, chapter_count);
            }
        }
        
        // 更新元数据中的总页数
        snprintf(key, sizeof(key), "%s/_meta", namespace);
        snprintf(value, sizeof(value),
                 "{\"type\":\"book\",\"title\":\"%s\",\"author\":\"%s\",\"chapters\":%d,\"pages\":%d}",
                 title, author, chapter_count, total_pages);
        cache_set(cache, key, value, 0);
        
        mobi_close(handle);
        dlclose(lib);
        
    } else if (strcasecmp(ext, ".pdf") == 0) {
        char title[256];
        strncpy(title, book_name, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        
        if (flag_ocr) {
            // ---- OCR 模式：使用 ocr_cuda C++ 二进制高精度识别 ----
            printf("OCR mode: using Unlimited-OCR for PDF recognition\n");
            printf("\n");
            
            char* text = NULL;
            size_t text_len = 0;
            ChapterInfo* chapters = NULL;
            int chapter_count = 0;
            
            if (ocr_extract_pages(book_path, argv[0],
                                  &text, &text_len,
                                  &chapters, &chapter_count) == 0) {
                if (text && text_len > 0) {
                    total_pages = import_book(cache, namespace, md_dir,
                                              title, "Unknown", text, text_len,
                                              chapters, chapter_count);
                }
            } else {
                fprintf(stderr, "OCR extraction failed\n");
                cache_close(cache);
                return 1;
            }
            
            free(text);
            if (chapters) {
                for (int i = 0; i < chapter_count; i++) free(chapters[i].title);
                free(chapters);
            }
            
        } else {
            // ---- 原有模式：使用 MuPDF 文本提取 ----
            void* lib = load_pdf_lib();
            if (!lib) {
                cache_close(cache);
                return 1;
            }
            
            void* (*pdf_open)(const char*) = dlsym(lib, "pdf_open");
            int (*pdf_get_page_count)(void*) = dlsym(lib, "pdf_get_page_count");
            int (*pdf_extract_text)(void*, char**, size_t*) = dlsym(lib, "pdf_extract_text");
            int (*pdf_get_chapters)(void*, void**, int*) = dlsym(lib, "pdf_get_chapters");
            void (*pdf_close)(void*) = dlsym(lib, "pdf_close");
            
            if (!pdf_open || !pdf_extract_text || !pdf_close) {
                fprintf(stderr, "Missing required PDF functions\n");
                dlclose(lib);
                cache_close(cache);
                return 1;
            }
            
            void* handle = pdf_open(book_path);
            if (!handle) {
                fprintf(stderr, "Failed to open PDF: %s\n", book_path);
                dlclose(lib);
                cache_close(cache);
                return 1;
            }
            
            int pages = 0;
            if (pdf_get_page_count) {
                pages = pdf_get_page_count(handle);
            }
            
            char* text = NULL;
            size_t text_len = 0;
            pdf_extract_text(handle, &text, &text_len);
            
            ChapterInfo* chapters = NULL;
            int chapter_count = 0;
            if (pdf_get_chapters) {
                pdf_get_chapters(handle, (void**)&chapters, &chapter_count);
            }
            
            printf("Pages: %d\n", pages);
            printf("Text length: %zu bytes\n", text_len);
            printf("Chapters: %d\n", chapter_count);
            printf("\n");
            
            if (text && text_len > 0) {
                total_pages = import_book(cache, namespace, md_dir,
                                          title, "Unknown", text, text_len,
                                          chapters, chapter_count);
            }
            
            pdf_close(handle);
            dlclose(lib);
        }
    } else if (strcasecmp(ext, ".epub") == 0) {
        void* lib = load_epub_lib();
        if (!lib) {
            cache_close(cache);
            return 1;
        }
        
        void* (*epub_open)(const char*) = dlsym(lib, "epub_open");
        int (*epub_extract_text)(void*, char**, size_t*) = dlsym(lib, "epub_extract_text");
        int (*epub_get_metadata)(void*, char*, size_t, char*, size_t) = dlsym(lib, "epub_get_metadata");
        int (*epub_get_chapters)(void*, void**, int*) = dlsym(lib, "epub_get_chapters");
        void (*epub_close)(void*) = dlsym(lib, "epub_close");
        
        if (!epub_open || !epub_extract_text || !epub_close) {
            fprintf(stderr, "Missing required EPUB functions\n");
            dlclose(lib);
            cache_close(cache);
            return 1;
        }
        
        void* handle = epub_open(book_path);
        if (!handle) {
            fprintf(stderr, "Failed to open EPUB: %s\n", book_path);
            dlclose(lib);
            cache_close(cache);
            return 1;
        }
        
        char title[512] = {0}, author[512] = {0};
        if (epub_get_metadata) {
            epub_get_metadata(handle, title, sizeof(title), author, sizeof(author));
        }
        if (strlen(title) == 0) {
            strncpy(title, book_name, sizeof(title) - 1);
        }
        if (strlen(author) == 0) {
            strcpy(author, "Unknown");
        }
        
        ChapterInfo* chapters = NULL;
        int chapter_count = 0;
        if (epub_get_chapters) {
            epub_get_chapters(handle, (void**)&chapters, &chapter_count);
        }
        
        char* text = NULL;
        size_t text_len = 0;
        if (epub_extract_text) {
            epub_extract_text(handle, &text, &text_len);
        }
        
        printf("Title: %s\n", title);
        printf("Author: %s\n", author);
        printf("Text length: %zu bytes\n", text_len);
        printf("Chapters: %d\n", chapter_count);
        printf("\n");
        
        if (text && text_len > 0) {
            total_pages = import_book(cache, namespace, md_dir,
                                      title, author, text, text_len,
                                      chapters, chapter_count);
        }
        
        epub_close(handle);
        dlclose(lib);
        
    } else {
        fprintf(stderr, "Unsupported format: %s\n", ext);
        fprintf(stderr, "Supported: .mobi, .azw, .azw3, .pdf, .epub\n");
        cache_close(cache);
        return 1;
    }
    
    // 最终同步和统计
    cache_sync(cache);
    
    // 导出向量供 vector_engine 使用
    if (flag_skip_vectors) {
        printf("  Skipping vector export (--skip-vectors)\n");
        printf("  Run later: %s %s %s --only-vectors\n", 
               argv[0], cache_dir, namespace);
    } else {
        int exported = export_vectors(cache, cache_dir, namespace, book_name);
        if (exported > 0) {
            printf("  Exported %d vectors for semantic search\n", exported);
        }
    }
    
    printf("\n========================================\n");
    printf("Import complete!\n");
    printf("  Total pages: %d\n", total_pages);
    printf("  KV Cache entries: %zu\n", cache_count(cache));
    printf("  Markdown directory: %s/\n", md_dir);
    printf("  Namespace: %s\n", namespace);
    printf("\n");
    printf("Query examples:\n");
    printf("  ./tools/cache_query \"%s/_meta\" --type exact\n", namespace);
    printf("  ./tools/cache_query \"chapter title\" --repo %s --type search\n", namespace);
    printf("  ./tools/cache_query \"%s/chapters/01-xxx/page_0000\" --type exact\n", namespace);
    printf("========================================\n");
    
    cache_close(cache);
    
    if (g_embedder) {
        onnx_embedder_free(g_embedder);
        g_embedder = NULL;
    }
    
    // Export Prometheus metrics
    char metrics_path[1024];
    snprintf(metrics_path, sizeof(metrics_path), "%s/metrics.txt", cache_dir);
    if (metrics_prometheus_write(metrics_path) == 0) {
        printf("Metrics exported: %s\n", metrics_path);
    }
    
    return 0;
}
