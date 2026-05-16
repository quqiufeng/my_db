#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

// 简单的分块函数：将文本按 max_chunk_size 分成多个块
static int split_into_chunks(const char* text, size_t text_len,
                              char** chunks, size_t* chunk_sizes,
                              int max_chunks, size_t max_chunk_size) {
    int count = 0;
    size_t pos = 0;
    
    while (pos < text_len && count < max_chunks) {
        size_t end = pos + max_chunk_size;
        if (end > text_len) end = text_len;
        
        // 尽量在段落边界分割（找最近的换行）
        if (end < text_len) {
            size_t newline_pos = end;
            while (newline_pos > pos && text[newline_pos] != '\n') newline_pos--;
            if (newline_pos > pos) end = newline_pos + 1;
        }
        
        size_t len = end - pos;
        chunks[count] = malloc(len + 1);
        if (!chunks[count]) break;
        
        memcpy(chunks[count], text + pos, len);
        chunks[count][len] = '\0';
        chunk_sizes[count] = len;
        
        count++;
        pos = end;
    }
    
    return count;
}

// 导入文本到 cache
static int import_text(cache_t* cache, const char* namespace,
                       const char* title, const char* author,
                       const char* text, size_t text_len) {
    char key[1024];
    char value[1024 * 1024];
    
    // 存储元数据
    snprintf(key, sizeof(key), "%s/_meta/title", namespace);
    snprintf(value, sizeof(value), "{\"t\":\"meta\",\"c\":\"%s\",\"author\":\"%s\"}", title, author);
    cache_set(cache, key, value, 0);
    
    snprintf(key, sizeof(key), "%s/_meta/author", namespace);
    cache_set(cache, key, author, 0);
    
    // 分块存储内容
    char* chunks[1000];
    size_t chunk_sizes[1000];
    int chunk_count = split_into_chunks(text, text_len, chunks, chunk_sizes, 1000, 4096);
    
    printf("Importing %d chunks...\n", chunk_count);
    
    for (int i = 0; i < chunk_count; i++) {
        snprintf(key, sizeof(key), "%s/chunk-%04d", namespace, i);
        
        // JSON value: {type, content, chunk_index, total_chunks}
        snprintf(value, sizeof(value),
                 "{\"t\":\"content\",\"c\":\"%.900s\",\"idx\":%d,\"total\":%d}",
                 chunks[i], i, chunk_count);
        
        int ret = cache_set(cache, key, value, 0);
        if (ret != CACHE_OK) {
            printf("Failed to set chunk %d: %d\n", i, ret);
        }
        
        free(chunks[i]);
    }
    
    return chunk_count;
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
    int chunks = 0;
    
    if (strcasecmp(ext, ".mobi") == 0 || strcasecmp(ext, ".azw") == 0) {
        void* lib = load_mobi_lib();
        if (!lib) {
            cache_close(cache);
            return 1;
        }
        
        void* (*mobi_open)(const char*) = dlsym(lib, "mobi_open");
        int (*mobi_extract_text)(void*, char**, size_t*) = dlsym(lib, "mobi_extract_text");
        int (*mobi_get_metadata)(void*, char*, size_t, char*, size_t) = dlsym(lib, "mobi_get_metadata");
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
        
        if (text && text_len > 0) {
            chunks = import_text(cache, namespace, title, author, text, text_len);
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
        
        char title[256];
        const char* basename = strrchr(book_path, '/');
        if (basename) basename++;
        else basename = book_path;
        strncpy(title, basename, sizeof(title) - 1);
        title[sizeof(title) - 1] = '\0';
        char* dot = strrchr(title, '.');
        if (dot) *dot = '\0';
        
        if (text && text_len > 0) {
            chunks = import_text(cache, namespace, title, "Unknown", text, text_len);
        }
        
        pdf_close(handle);
        dlclose(lib);
    } else {
        fprintf(stderr, "Unsupported format: %s\n", ext);
        cache_close(cache);
        return 1;
    }
    
    printf("\nImport complete: %d chunks\n", chunks);
    printf("Total entries in cache: %zu\n", cache_count(cache));
    
    cache_sync(cache);
    cache_close(cache);
    
    return 0;
}
