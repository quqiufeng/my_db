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
 *   ./import_book ./ai_code_memory ~/book.mobi /books/my_book
 *   ./import_book ./ai_code_memory ~/paper.pdf
 */

#include "cache.h"
#include "onnx_embedder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

// =============================================================================
// 配置
// =============================================================================

// Jina v2 模型配置（与代码分析统一）
#define MODEL_PATH      "models/jina-embeddings-v2-base-code/model.onnx"
#define VOCAB_PATH      "models/jina-embeddings-v2-base-code/vocab.json"
#define MODEL_SEQ_LEN   512
#define EMBEDDING_DIM   768

// 分块配置
#define MAX_PARAGRAPHS      5000    // 单章节最大段落数
#define MIN_PARA_LEN        20      // 最小段落长度
#define TARGET_CHUNK_SIZE   800     // 目标段落长度（字符）
#define PREVIEW_LEN         200     // KV Cache 中保存的预览长度

// 章节信息结构（与 wrapper 兼容）
typedef struct {
    char* title;
    int level;
    size_t offset;  // MOBI: byte offset, PDF: page number
} ChapterInfo;

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
        } else if (c == ' ' || c == '\t') {
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
        safe_title[60] = '\0';
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
 * 将文本分割为语义段落
 * 策略：
 *   1. 优先按空白行（段落边界）分割
 *   2. 如果段落太长，按句子边界分割
 *   3. 如果段落太短（< MIN_PARA_LEN），合并到下一个
 */
static int split_paragraphs(const char* text, size_t text_len,
                            char*** out_paragraphs, int* out_count) {
    int capacity = 1000;
    int count = 0;
    char** paragraphs = malloc(sizeof(char*) * capacity);
    if (!paragraphs) return -1;
    
    const char* p = text;
    const char* end = text + text_len;
    
    while (p < end) {
        // 跳过空白
        while (p < end && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        
        // 找段落结束
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
            // 扩展容量
            if (count >= capacity) {
                capacity *= 2;
                char** new_p = realloc(paragraphs, sizeof(char*) * capacity);
                if (!new_p) break;
                paragraphs = new_p;
            }
            
            paragraphs[count] = malloc(len + 1);
            if (paragraphs[count]) {
                memcpy(paragraphs[count], p, len);
                paragraphs[count][len] = '\0';
                count++;
            }
        }
        
        p = para_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
    
    *out_paragraphs = paragraphs;
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

/*
 * 将段落保存为 Markdown 文件
 * 返回：实际写入的文件路径（相对路径）
 */
static int save_page_as_md(const char* md_dir, const char* chapter_dir,
                           int page_idx, const char* content,
                           char* out_md_path, size_t out_path_size) {
    char md_file[1024];
    snprintf(md_file, sizeof(md_file), "%s/chapters/%s/page_%04d.md", 
             md_dir, chapter_dir, page_idx);
    
    FILE* fp = fopen(md_file, "w");
    if (!fp) {
        fprintf(stderr, "Failed to create markdown file: %s (errno=%d)\n", md_file, errno);
        return -1;
    }
    
    // 写入 Markdown 格式
    fprintf(fp, "<!--\n");
    fprintf(fp, "  Auto-generated by import_book\n");
    fprintf(fp, "  Page: %d\n", page_idx);
    fprintf(fp, "  Chapter: %s\n", chapter_dir);
    fprintf(fp, "-->\n\n");
    fprintf(fp, "%s\n", content);
    
    fclose(fp);
    
    // 返回相对路径
    snprintf(out_md_path, out_path_size, "%s/chapters/%s/page_%04d.md",
             md_dir, chapter_dir, page_idx);
    
    return 0;
}

// =============================================================================
// 核心导入函数
// =============================================================================

/*
 * 导入单个章节
 * 
 * 流程：
 *   1. 分块 → 2. 过滤噪音 → 3. 保存 md 文件 → 4. 生成向量 → 5. 存入 KV Cache
 */
static int import_chapter(cache_t* cache, const char* namespace,
                          const char* md_dir, const char* chapter_dirname,
                          const char* chapter_title,
                          const char* text, size_t text_len) {
    char key[1024];
    char value[4096];
    
    // 创建章节目录
    char chapter_md_dir[1024];
    snprintf(chapter_md_dir, sizeof(chapter_md_dir), "%s/chapters/%s", md_dir, chapter_dirname);
    mkdir_p(chapter_md_dir);
    
    // 保存章节元数据到 KV Cache
    snprintf(key, sizeof(key), "%s/chapters/%s/_meta", namespace, chapter_dirname);
    snprintf(value, sizeof(value),
             "{\"type\":\"chapter\",\"title\":\"%s\",\"md_dir\":\"%s\"}",
             chapter_title, chapter_md_dir);
    cache_set(cache, key, value, 0);
    
    // 分块
    char** paragraphs = NULL;
    int para_count = 0;
    if (split_paragraphs(text, text_len, &paragraphs, &para_count) != 0) {
        fprintf(stderr, "Failed to split paragraphs\n");
        return 0;
    }
    
    printf("  Chapter '%s': %d raw paragraphs\n", chapter_title, para_count);
    
    int valid_pages = 0;
    int total_pages = 0;
    
    for (int i = 0; i < para_count; i++) {
        // 过滤噪音
        if (is_css_content(paragraphs[i]) || 
            is_html_markup(paragraphs[i]) ||
            is_noise(paragraphs[i])) {
            continue;
        }
        
        // 保存为 Markdown 文件
        char md_path[1024];
        if (save_page_as_md(md_dir, chapter_dirname, total_pages,
                            paragraphs[i], md_path, sizeof(md_path)) != 0) {
            continue;
        }
        
        // 生成预览（前 PREVIEW_LEN 字符）
        char preview[PREVIEW_LEN + 1];
        size_t content_len = strlen(paragraphs[i]);
        size_t preview_len = content_len < PREVIEW_LEN ? content_len : PREVIEW_LEN;
        memcpy(preview, paragraphs[i], preview_len);
        preview[preview_len] = '\0';
        
        // 转义 JSON 中的引号
        char preview_escaped[PREVIEW_LEN * 2];
        int j = 0;
        for (size_t k = 0; k < preview_len && j < sizeof(preview_escaped) - 1; k++) {
            if (preview[k] == '"') {
                preview_escaped[j++] = '\\';
                preview_escaped[j++] = '"';
            } else if (preview[k] == '\n') {
                preview_escaped[j++] = '\\';
                preview_escaped[j++] = 'n';
            } else if (preview[k] == '\\') {
                preview_escaped[j++] = '\\';
                preview_escaped[j++] = '\\';
            } else if ((unsigned char)preview[k] < 0x20) {
                // 跳过控制字符
            } else {
                preview_escaped[j++] = preview[k];
            }
        }
        preview_escaped[j] = '\0';
        
        // 生成向量
        float* vector = NULL;
        if (g_embedder) {
            vector = malloc(sizeof(float) * EMBEDDING_DIM);
            if (vector) {
                if (onnx_embedder_encode(g_embedder, paragraphs[i], vector) != 0) {
                    fprintf(stderr, "    Embedding failed for page %d: %s\n",
                            total_pages, onnx_embedder_error());
                    free(vector);
                    vector = NULL;
                }
            }
        }
        
        // 存入 KV Cache（元数据 + 路径 + 预览 + 向量）
        snprintf(key, sizeof(key), "%s/chapters/%s/page_%04d",
                 namespace, chapter_dirname, total_pages);
        
        snprintf(value, sizeof(value),
                 "{\"type\":\"page\",\"md_file\":\"%s\",\"preview\":\"%s\",\"content_len\":%zu}",
                 md_path, preview_escaped, content_len);
        
        cache_set(cache, key, value, 0);
        
        // 存储向量（如果成功生成）
        if (vector) {
            int vret = cache_set_vector(cache, key, value, vector, EMBEDDING_DIM, 0);
            if (vret != CACHE_OK) {
                fprintf(stderr, "    Failed to store vector for page %d: %d\n", 
                        total_pages, vret);
            }
            free(vector);
        }
        
        valid_pages++;
        total_pages++;
        
        // 每 100 页同步一次
        if (total_pages % 100 == 0) {
            cache_sync(cache);
            printf("    Progress: %d pages imported...\n", total_pages);
        }
    }
    
    free_paragraphs(paragraphs, para_count);
    
    printf("  Chapter '%s': %d valid pages saved to %s/\n", 
           chapter_title, valid_pages, chapter_md_dir);
    
    return valid_pages;
}

/*
 * 导入整本书
 */
static int import_book(cache_t* cache, const char* namespace,
                       const char* md_dir,
                       const char* title, const char* author,
                       const char* text, size_t text_len,
                       ChapterInfo* chapters, int chapter_count) {
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
        for (int i = 0; i < chapter_count; i++) {
            char ch_dir[256];
            chapter_dirname(ch_dir, sizeof(ch_dir), i, chapters[i].title);
            
            // 定位章节文本范围
            const char* chapter_text = text;
            size_t chapter_len = text_len;
            
            if (chapter_count > 1 && chapters[i].title && strlen(chapters[i].title) > 0) {
                // 策略1: 尝试完整标题匹配
                const char* best_pos = NULL;
                size_t best_len = 0;
                const char* search = text;
                
                while (search < text + text_len) {
                    const char* found = strstr(search, chapters[i].title);
                    if (!found) break;
                    
                    size_t body_len = text_len - (found - text);
                    if (i < chapter_count - 1) {
                        const char* next_title = strstr(found + 1, chapters[i+1].title);
                        if (next_title) {
                            body_len = next_title - found;
                        }
                    }
                    
                    if (body_len > best_len) {
                        best_len = body_len;
                        best_pos = found;
                    }
                    
                    search = found + 1;
                }
                
                // 策略2: 如果完整标题匹配失败，尝试提取章节号匹配（如"第一章"）
                if (!best_pos) {
                    char chapter_prefix[64] = {0};
                    const char* p = chapters[i].title;
                    // 提取"第X章"前缀
                    while (*p && (*p == ' ' || *p == '\t')) p++;
                    if (strncmp(p, "第", 3) == 0 || strncmp(p, "推荐序", 9) == 0 ||
                        strncmp(p, "附录", 6) == 0 || strncmp(p, "后记", 6) == 0) {
                        // 找到前缀结束位置（第一个空格或字符串结束）
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
                                // 获取下一章的前缀
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
                                    if (next_found) {
                                        body_len = next_found - found;
                                    }
                                }
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
            
            int pages = import_chapter(cache, namespace, md_dir, ch_dir,
                                       chapters[i].title, chapter_text, chapter_len);
            total_pages += pages;
        }
    }
    
    // 更新元数据中的总页数
    snprintf(key, sizeof(key), "%s/_meta", namespace);
    snprintf(value, sizeof(value),
             "{\"type\":\"book\",\"title\":\"%s\",\"author\":\"%s\",\"chapters\":%d,\"pages\":%d}",
             title, author, chapter_count, total_pages);
    cache_set(cache, key, value, 0);
    
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
    
    // 构建文件名
    char bin_file[1024], idx_file[1024];
    snprintf(bin_file, sizeof(bin_file), "%s/books_%s.jina.bin", vec_dir, book_name);
    snprintf(idx_file, sizeof(idx_file), "%s/books_%s.jina.idx", vec_dir, book_name);
    
    // 收集所有带向量的 entry
    VectorEntry* entries = NULL;
    int count = 0;
    int capacity = 1000;
    entries = malloc(sizeof(VectorEntry) * capacity);
    if (!entries) return -1;
    
    size_t ns_len = strlen(namespace);
    
    cache_iter_t* iter = cache_iter_create(cache);
    if (!iter) {
        free(entries);
        return -1;
    }
    
    const char* key;
    const char* value;
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
        
        entries[count].name = strdup(key);
        entries[count].vector = malloc(sizeof(float) * dim);
        if (entries[count].vector) {
            memcpy(entries[count].vector, vec, sizeof(float) * dim);
        }
        count++;
    }
    cache_iter_destroy(iter);
    
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
    char hnsw_cmd[2048];
    snprintf(hnsw_cmd, sizeof(hnsw_cmd),
             "./tools/build_hnsw_index %s >/dev/null 2>&1", bin_file);
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
// 主函数
// =============================================================================

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <cache_dir> <book_file> [namespace] [output_dir]\n", argv[0]);
        printf("\n");
        printf("Arguments:\n");
        printf("  cache_dir   KV Cache 目录 (如 ./ai_code_memory)\n");
        printf("  book_file   电子书文件 (.mobi, .azw, .azw3, .pdf, .epub)\n");
        printf("  namespace   命名空间 (可选，默认从文件名生成)\n");
        printf("  output_dir  Markdown 输出目录 (可选，默认 /opt/books)\n");
        printf("\n");
        printf("Examples:\n");
        printf("  %s ./ai_code_memory ~/book.mobi /books/my_book\n", argv[0]);
        printf("  %s ./ai_code_memory ~/paper.pdf\n", argv[0]);
        printf("  %s ./ai_code_memory ~/paper.pdf /books/paper /data/books\n", argv[0]);
        printf("\n");
        printf("Output structure:\n");
        printf("  /opt/books/{book_name}/\n");
        printf("    ├── _meta.json\n");
        printf("    └── chapters/\n");
        printf("        └── 01-Chapter_Title/\n");
        printf("            └── page_0000.md\n");
        return 1;
    }
    
    const char* cache_dir = argv[1];
    const char* book_path = argv[2];
    const char* ns = NULL;
    const char* output_dir = "/opt/books";
    
    // 智能解析可选参数
    if (argc > 3) {
        if (argv[3][0] == '/') {
            // 以 / 开头，视为 namespace
            ns = argv[3];
            if (argc > 4) {
                output_dir = argv[4];
            }
        } else {
            // 不以 / 开头，视为 output_dir
            output_dir = argv[3];
        }
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
    cache_t* cache = cache_open(cache_dir, 500 * 1024 * 1024);  // 500MB
    if (!cache) {
        fprintf(stderr, "Failed to open cache: %s\n", cache_dir);
        return 1;
    }
    
    printf("Cache opened: %s\n", cache_dir);
    
    // 初始化 Jina v2 嵌入模型
    printf("Loading embedding model (Jina v2, %d-dim)...\n", EMBEDDING_DIM);
    g_embedder = onnx_embedder_init(MODEL_PATH, VOCAB_PATH, MODEL_SEQ_LEN, EMBEDDING_DIM);
    if (g_embedder) {
        printf("  Embedding model loaded successfully\n");
    } else {
        printf("  Warning: Failed to load embedding model (%s)\n", onnx_embedder_error());
        printf("  Continuing without vector generation\n");
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
        
        char* text = NULL;
        size_t text_len = 0;
        if (mobi_extract_text) {
            mobi_extract_text(handle, &text, &text_len);
        }
        
        printf("Text length: %zu bytes\n", text_len);
        
        if (text && text_len > 0) {
            total_pages = import_book(cache, namespace, md_dir,
                                      title, author, text, text_len,
                                      chapters, chapter_count);
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
        
        char title[256];
        strncpy(title, book_name, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        
        if (text && text_len > 0) {
            total_pages = import_book(cache, namespace, md_dir,
                                      title, "Unknown", text, text_len,
                                      chapters, chapter_count);
        }
        
        pdf_close(handle);
        dlclose(lib);
        
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
    int exported = export_vectors(cache, cache_dir, namespace, book_name);
    if (exported > 0) {
        printf("  Exported %d vectors for semantic search\n", exported);
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
    
    return 0;
}
