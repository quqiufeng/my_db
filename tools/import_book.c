#include "cache.h"
#include "onnx_embedder.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

// Global embedder instance (initialized in main)
static onnx_embedder_t* g_embedder = NULL;
static int g_embedding_dim = 384;

// 章节信息结构（必须与 wrapper 中完全一致）
// MOBI wrapper: struct MobiChapter { char* title; int level; size_t offset; }
// PDF wrapper:  struct PdfChapter  { char* title; int level; size_t page; }
typedef struct {
    char* title;
    int level;
    size_t offset;  // MOBI: byte offset, PDF: page number
} ChapterInfo;

// 分块函数：将文本按段落或固定长度分成多个块
static int split_into_paragraphs(const char* text, size_t text_len,
                                  char*** out_paragraphs, int max_paragraphs) {
    int count = 0;
    const char* p = text;
    const char* end = text + text_len;
    
    char** paragraphs = (char**)malloc(sizeof(char*) * max_paragraphs);
    if (!paragraphs) return 0;
    
    // 首先尝试按换行分段
    while (p < end && count < max_paragraphs) {
        // 跳过空白
        while (p < end && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')) p++;
        if (p >= end) break;
        
        // 找段落结束（连续换行或文本结束）
        const char* para_end = p;
        while (para_end < end) {
            if (*para_end == '\n') {
                // 检查是否连续换行（段落分隔）
                const char* next = para_end + 1;
                while (next < end && (*next == '\r' || *next == '\n')) next++;
                if (next > para_end + 1) {
                    // 连续换行，段落结束
                    break;
                }
            }
            para_end++;
        }
        
        size_t len = para_end - p;
        if (len > 10) {  // 至少10个字符才算有效段落
            paragraphs[count] = (char*)malloc(len + 1);
            if (paragraphs[count]) {
                memcpy(paragraphs[count], p, len);
                paragraphs[count][len] = '\0';
                count++;
            }
        }
        
        p = para_end;
        while (p < end && (*p == '\n' || *p == '\r')) p++;
    }
    
    // 如果段落太少（每个章节少于3段），使用固定长度分段
    if (count > 0 && count < 3) {
        // 释放之前的段落
        for (int i = 0; i < count; i++) {
            free(paragraphs[i]);
        }
        count = 0;
        
        const char* p = text;
        size_t chunk_size = 500;  // 每段约500字符
        
        while (p < end && count < max_paragraphs) {
            // 跳过空白
            while (p < end && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')) p++;
            if (p >= end) break;
            
            // 找段落结束：优先在句号后断行
            const char* para_end = p;
            size_t current_size = 0;
            const char* last_period = NULL;
            
            while (para_end < end && current_size < chunk_size) {
                if (*para_end == '。' || *para_end == '.' || *para_end == '！' || *para_end == '？') {
                    last_period = para_end;
                }
                para_end++;
                current_size++;
            }
            
            // 如果在句号后断行更好
            if (last_period && last_period > p + 100) {
                para_end = last_period + 1;
            }
            
            size_t len = para_end - p;
            if (len > 10) {
                paragraphs[count] = (char*)malloc(len + 1);
                if (paragraphs[count]) {
                    memcpy(paragraphs[count], p, len);
                    paragraphs[count][len] = '\0';
                    count++;
                }
            }
            
            p = para_end;
        }
    }
    
    *out_paragraphs = paragraphs;
    return count;
}

// 清理段落数组
static void free_paragraphs(char** paragraphs, int count) {
    for (int i = 0; i < count; i++) {
        free(paragraphs[i]);
    }
    free(paragraphs);
}

// 检查段落是否是 CSS 样式
static int is_css_content(const char* text) {
    // CSS 特征：包含 .class 或 { ... } 或 @media 等
    if (strstr(text, "{") && strstr(text, "}")) return 1;
    if (strstr(text, "@media")) return 1;
    if (strstr(text, "@-webkit-")) return 1;
    if (strstr(text, ".calibre")) return 1;
    // 检查是否全是 CSS 属性（如 margin:、padding:、font- 等）
    int css_props = 0;
    const char* css_keywords[] = {"margin", "padding", "font-", "display:", "text-align:", "text-indent:", "line-height:", "height:", "width:", "color:", "background", "border", "position:", NULL};
    for (int i = 0; css_keywords[i]; i++) {
        if (strstr(text, css_keywords[i])) css_props++;
    }
    if (css_props >= 2) return 1;
    return 0;
}

// 检查段落是否是 HTML 标签残留
static int is_html_markup(const char* text) {
    // 如果以 < 开头且包含 >，可能是 HTML
    if (text[0] == '<' && strchr(text, '>')) return 1;
    return 0;
}

// 存储章节内容到 cache
static int import_chapter(cache_t* cache, const char* chapter_ns,
                          const char* title, const char* text, size_t text_len) {
    char key[1024];
    char value[1024 * 1024];
    int total_paragraphs = 0;
    
    // 存储章节标题
    snprintf(key, sizeof(key), "%s/title", chapter_ns);
    snprintf(value, sizeof(value), "{\"t\":\"chapter_title\",\"c\":\"%s\"}", title);
    cache_set(cache, key, value, 0);
    
    // 按段落分块存储内容
    char** paragraphs = NULL;
    int para_count = split_into_paragraphs(text, text_len, &paragraphs, 1000);
    
    int valid_count = 0;
    for (int i = 0; i < para_count; i++) {
        if (!is_css_content(paragraphs[i]) && !is_html_markup(paragraphs[i])) {
            valid_count++;
        }
    }
    
    printf("  Chapter '%s': %d paragraphs (%d valid)\n", title, para_count, valid_count);
    if (para_count == 0) {
        printf("  WARNING: No paragraphs found!\n");
    }
    
    int idx = 0;
    for (int i = 0; i < para_count; i++) {
        // 跳过 CSS 和 HTML 内容
        if (is_css_content(paragraphs[i]) || is_html_markup(paragraphs[i])) {
            continue;
        }
        
        snprintf(key, sizeof(key), "%s/content/p%04d", chapter_ns, idx);
        
        // JSON value
        snprintf(value, sizeof(value),
                 "{\"t\":\"paragraph\",\"c\":\"%.900s\",\"idx\":%d,\"total\":%d}",
                 paragraphs[i], idx, valid_count);
        
        int ret = cache_set(cache, key, value, 0);
        if (ret != CACHE_OK) {
            printf("    Failed to set paragraph %d: %d\n", idx, ret);
        }
        
        // Generate and store vector embedding
        if (g_embedder) {
            float* vector = malloc(sizeof(float) * g_embedding_dim);
            if (vector) {
                if (onnx_embedder_encode(g_embedder, paragraphs[i], vector) == 0) {
                    int vret = cache_set_vector(cache, key, value, vector, g_embedding_dim, 0);
                    if (vret != CACHE_OK) {
                        printf("    Failed to set vector for paragraph %d: %d\n", idx, vret);
                    }
                } else {
                    printf("    Embedding failed for paragraph %d: %s\n", idx, onnx_embedder_error());
                }
                free(vector);
            }
        }
        
        total_paragraphs++;
        idx++;
    }
    
    free_paragraphs(paragraphs, para_count);
    return total_paragraphs;
}

// 导入整本书（按章节）
static int import_book_chapters(cache_t* cache, const char* namespace,
                                 const char* title, const char* author,
                                 const char* text, size_t text_len,
                                 ChapterInfo* chapters, int chapter_count) {
    char key[1024];
    char value[1024 * 1024];
    
    // 存储书籍元数据
    snprintf(key, sizeof(key), "%s/_meta/title", namespace);
    snprintf(value, sizeof(value), "{\"t\":\"meta\",\"c\":\"%s\",\"author\":\"%s\"}", title, author);
    cache_set(cache, key, value, 0);
    
    snprintf(key, sizeof(key), "%s/_meta/author", namespace);
    cache_set(cache, key, author, 0);
    
    // 存储章节列表
    snprintf(key, sizeof(key), "%s/_meta/chapters", namespace);
    char chapter_list[4096] = "[";
    int list_len = 1;
    for (int i = 0; i < chapter_count && list_len < 4000; i++) {
        int n = snprintf(chapter_list + list_len, sizeof(chapter_list) - list_len,
                         "%s\"%s\"", i > 0 ? "," : "", chapters[i].title);
        if (n > 0) list_len += n;
    }
    strcat(chapter_list, "]");
    cache_set(cache, key, chapter_list, 0);
    
    printf("Importing %d chapters...\n", chapter_count);
    
    int total_paragraphs = 0;
    
    if (chapter_count <= 1) {
        // 没有章节信息，整本书作为一个章节
        char chapter_ns[512];
        snprintf(chapter_ns, sizeof(chapter_ns), "%s/chapters/full", namespace);
        total_paragraphs = import_chapter(cache, chapter_ns, title, text, text_len);
    } else {
        // 按章节导入
        for (int i = 0; i < chapter_count; i++) {
            char chapter_ns[512];
            // 生成章节 namespace：使用章节标题的简化版本
            char safe_title[128];
            strncpy(safe_title, chapters[i].title, sizeof(safe_title) - 1);
            safe_title[sizeof(safe_title) - 1] = '\0';
            
            // 替换不安全的字符
            for (char* p = safe_title; *p; p++) {
                if (*p == '/' || *p == '\\' || *p == ':' || *p == '*' || 
                    *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') {
                    *p = '_';
                }
            }
            
            snprintf(chapter_ns, sizeof(chapter_ns), "%s/chapters/%02d-%s", 
                     namespace, i + 1, safe_title);
            
            // 计算章节文本范围：在文本中搜索章节标题位置
            const char* chapter_text = text;
            size_t chapter_len = text_len;
            
            if (chapter_count > 1) {
                // 方法：查找所有匹配该标题的位置，选择正文中的那个（后面有大量正文）
                const char* best_pos = NULL;
                size_t best_body_len = 0;
                
                const char* search_start = text;
                while (search_start < text + text_len) {
                    const char* found = strstr(search_start, chapters[i].title);
                    if (!found) break;
                    
                    // 计算该位置后面的正文长度（到下一个章节标题或文本结束）
                    size_t body_len = text_len - (found - text);
                    if (i < chapter_count - 1) {
                        const char* next = strstr(found + 1, chapters[i+1].title);
                        if (next) {
                            body_len = next - found;
                        }
                    }
                    
                    // 选择正文最长的位置（跳过目录中的短匹配）
                    if (body_len > best_body_len) {
                        best_body_len = body_len;
                        best_pos = found;
                    }
                    
                    search_start = found + 1;
                }
                
                if (best_pos) {
                    chapter_text = best_pos;
                    chapter_len = best_body_len;
                } else {
                    // 回退到均分
                    size_t start = (i * text_len) / chapter_count;
                    if (i < chapter_count - 1) {
                        size_t end = ((i + 1) * text_len) / chapter_count;
                        chapter_text = text + start;
                        chapter_len = end - start;
                    } else {
                        chapter_text = text + start;
                        chapter_len = text_len - start;
                    }
                }
            }
            
            total_paragraphs += import_chapter(cache, chapter_ns, chapters[i].title,
                                                  chapter_text, chapter_len);
        }
    }
    
    return total_paragraphs;
}

// 加载 MOBI 解析库
static void* load_mobi_lib(void) {
    void* lib = dlopen("src/importer/libs/libmobiparse.so", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "Failed to load libmobiparse.so: %s\n", dlerror());
        return NULL;
    }
    return lib;
}

// 加载 PDF 解析库
static void* load_pdf_lib(void) {
    void* lib = dlopen("src/importer/libs/libpdfparse.so", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "Failed to load libpdfparse.so: %s\n", dlerror());
        return NULL;
    }
    return lib;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage: %s <cache_dir> <ebook_file> [namespace]\n", argv[0]);
        printf("  Example: %s ./my_cache ~/book.mobi /books/cybersecurity\n", argv[0]);
        return 1;
    }
    
    const char* cache_dir = argv[1];
    const char* book_path = argv[2];
    const char* ns = argc > 3 ? argv[3] : NULL;
    
    // 确定文件类型
    size_t path_len = strlen(book_path);
    const char* ext = "";
    for (size_t i = path_len; i > 0; i--) {
        if (book_path[i-1] == '.') {
            ext = book_path + i - 1;
            break;
        }
    }
    
    // 打开 cache
    cache_t* cache = cache_open(cache_dir, 100 * 1024 * 1024);  // 100MB
    if (!cache) {
        fprintf(stderr, "Failed to open cache: %s\n", cache_dir);
        return 1;
    }
    
    printf("Cache opened: %s\n", cache_dir);
    
    // Initialize ONNX embedder for vector generation
    const char* model_path = getenv("EMBEDDING_MODEL") ? getenv("EMBEDDING_MODEL") : "models/all-MiniLM-L6-v2/model.onnx";
    const char* vocab_path = getenv("EMBEDDING_VOCAB") ? getenv("EMBEDDING_VOCAB") : "models/all-MiniLM-L6-v2/vocab.txt";
    
    g_embedder = onnx_embedder_init(model_path, vocab_path, 128, 384);
    if (g_embedder) {
        printf("Embedding model loaded: %s\n", model_path);
    } else {
        printf("Warning: Failed to load embedding model (%s), continuing without vectors\n",
               onnx_embedder_error());
    }
    printf("Importing: %s\n", book_path);
    
    // 生成 namespace
    char namespace[256];
    if (ns) {
        strncpy(namespace, ns, sizeof(namespace) - 1);
    } else {
        // 从文件名生成 namespace
        const char* basename = strrchr(book_path, '/');
        if (basename) basename++;
        else basename = book_path;
        
        char name[128];
        strncpy(name, basename, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        
        // 去掉扩展名
        char* dot = strrchr(name, '.');
        if (dot) *dot = '\0';
        
        snprintf(namespace, sizeof(namespace), "/books/%s", name);
    }
    
    printf("Namespace: %s\n", namespace);
    
    // 解析并导入
    int paragraphs = 0;
    
    if (strcasecmp(ext, ".mobi") == 0 || strcasecmp(ext, ".azw") == 0 || strcasecmp(ext, ".azw3") == 0) {
        void* lib = load_mobi_lib();
        if (!lib) {
            cache_close(cache);
            return 1;
        }
        
        void* (*mobi_open)(const char*) = dlsym(lib, "mobi_open");
        int (*mobi_extract_text)(void*, char**, size_t*) = dlsym(lib, "mobi_extract_text");
        int (*mobi_get_chapter_text)(void*, int, char**, size_t*) = dlsym(lib, "mobi_get_chapter_text");
        int (*mobi_get_metadata)(void*, char*, size_t, char*, size_t) = dlsym(lib, "mobi_get_metadata");
        int (*mobi_get_chapters)(void*, void**, int*) = dlsym(lib, "mobi_get_chapters");
        void (*mobi_close)(void*) = dlsym(lib, "mobi_close");
        
        void* handle = mobi_open(book_path);
        if (!handle) {
            fprintf(stderr, "Failed to open MOBI: %s\n", book_path);
            dlclose(lib);
            cache_close(cache);
            return 1;
        }
        
        char title[512] = {0}, author[512] = {0};
        mobi_get_metadata(handle, title, sizeof(title), author, sizeof(author));
        
        printf("Title: %s\n", title);
        printf("Author: %s\n", author);
        
        // 获取章节列表
        ChapterInfo* chapters = NULL;
        int chapter_count = 0;
        if (mobi_get_chapters) {
            mobi_get_chapters(handle, (void**)&chapters, &chapter_count);
        }
        
        printf("Chapters: %d\n", chapter_count);
        for (int i = 0; i < chapter_count; i++) {
            printf("  [%d] %s (level=%d)\n", i + 1, chapters[i].title, chapters[i].level);
        }
        
        // 使用 mobi_extract_text 提取整本书文本，然后按标题切分
        if (mobi_extract_text) {
            char* text = NULL;
            size_t text_len = 0;
            mobi_extract_text(handle, &text, &text_len);
            printf("Text length: %zu\n", text_len);
            
            if (text && text_len > 0) {
                paragraphs = import_book_chapters(cache, namespace, title, author, 
                                                   text, text_len, chapters, chapter_count);
            }
        }
        
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
        
        void* handle = pdf_open(book_path);
        if (!handle) {
            fprintf(stderr, "Failed to open PDF: %s\n", book_path);
            dlclose(lib);
            cache_close(cache);
            return 1;
        }
        
        int pages = pdf_get_page_count(handle);
        
        char* text = NULL;
        size_t text_len = 0;
        pdf_extract_text(handle, &text, &text_len);
        
        printf("Pages: %d\n", pages);
        printf("Text length: %zu\n", text_len);
        
        // 获取章节列表
        ChapterInfo* chapters = NULL;
        int chapter_count = 0;
        if (pdf_get_chapters) {
            pdf_get_chapters(handle, (void**)&chapters, &chapter_count);
        }
        
        printf("Chapters: %d\n", chapter_count);
        for (int i = 0; i < chapter_count; i++) {
            printf("  [%d] %s (level=%d, page=%zu)\n", i + 1, chapters[i].title, 
                   chapters[i].level, chapters[i].offset);
        }
        
        char title[256];
        const char* basename = strrchr(book_path, '/');
        if (basename) basename++;
        else basename = book_path;
        strncpy(title, basename, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        char* dot = strrchr(title, '.');
        if (dot) *dot = '\0';
        
        if (text && text_len > 0) {
            paragraphs = import_book_chapters(cache, namespace, title, "Unknown", 
                                               text, text_len, chapters, chapter_count);
        }
        
        pdf_close(handle);
        dlclose(lib);
    } else {
        fprintf(stderr, "Unsupported format: %s\n", ext);
        cache_close(cache);
        return 1;
    }
    
    printf("\nImport complete: %d paragraphs\n", paragraphs);
    printf("Total entries in cache: %zu\n", cache_count(cache));
    
    cache_sync(cache);
    cache_close(cache);
    
    // Cleanup embedder
    if (g_embedder) {
        onnx_embedder_free(g_embedder);
        g_embedder = NULL;
    }
    
    return 0;
}
