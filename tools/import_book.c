#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

// 章节信息结构（与 wrapper 中一致）
typedef struct {
    char* title;
    int level;
    size_t offset;  // MOBI 用字节偏移，PDF 用页码
} ChapterInfo;

// 简单的分块函数：将文本按段落分成多个块
static int split_into_paragraphs(const char* text, size_t text_len,
                                  char*** out_paragraphs, int max_paragraphs) {
    int count = 0;
    const char* p = text;
    const char* end = text + text_len;
    
    char** paragraphs = (char**)malloc(sizeof(char*) * max_paragraphs);
    if (!paragraphs) return 0;
    
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
        if (len > 0) {
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
    
    printf("  Chapter '%s': %d paragraphs\n", title, para_count);
    
    for (int i = 0; i < para_count; i++) {
        snprintf(key, sizeof(key), "%s/content/p%04d", chapter_ns, i);
        
        // JSON value
        snprintf(value, sizeof(value),
                 "{\"t\":\"paragraph\",\"c\":\"%.900s\",\"idx\":%d,\"total\":%d}",
                 paragraphs[i], i, para_count);
        
        int ret = cache_set(cache, key, value, 0);
        if (ret != CACHE_OK) {
            printf("    Failed to set paragraph %d: %d\n", i, ret);
        }
        total_paragraphs++;
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
            
            // 计算章节文本范围
            const char* chapter_text = text;
            size_t chapter_len = text_len;
            
            if (i < chapter_count - 1) {
                // 使用下一个章节的偏移作为结束
                // 简化处理：MOBI 用字节偏移，PDF 用页码
                // 这里我们简单地将文本按章节数均分
                size_t start = (i * text_len) / chapter_count;
                size_t end = ((i + 1) * text_len) / chapter_count;
                chapter_text = text + start;
                chapter_len = end - start;
            } else {
                // 最后一个章节
                size_t start = (i * text_len) / chapter_count;
                chapter_text = text + start;
                chapter_len = text_len - start;
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
        
        char* text = NULL;
        size_t text_len = 0;
        mobi_extract_text(handle, &text, &text_len);
        
        printf("Title: %s\n", title);
        printf("Author: %s\n", author);
        printf("Text length: %zu\n", text_len);
        
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
        
        if (text && text_len > 0) {
            paragraphs = import_book_chapters(cache, namespace, title, author, 
                                               text, text_len, chapters, chapter_count);
        }
        
        mobi_close(handle);
        dlclose(lib);
        
    } else if (strcasecmp(ext, ".pdf") == 0 || strcasecmp(ext, ".epub") == 0) {
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
    
    return 0;
}
