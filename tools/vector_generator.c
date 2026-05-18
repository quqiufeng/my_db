#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "cache.h"
#include "onnx_embedder.h"

#define DIM 384
#define BATCH_SIZE 64
#define MAX_TEXT_LEN 2048

typedef struct {
    char name[256];
    char text[MAX_TEXT_LEN];
} item_t;

// 从 JSON 提取 content
static int extract_content(const char* json, char* out, int max_len) {
    const char* p = strstr(json, "\"content\":");
    if (!p) return 0;
    p += 10;
    while (*p && isspace(*p)) p++;
    if (*p != '"') return 0;
    p++;
    
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

// 从 JSON 提取 signature
static int extract_signature(const char* json, char* out, int max_len) {
    const char* p = strstr(json, "\"signature\":");
    if (!p) return 0;
    p += 12;
    while (*p && isspace(*p)) p++;
    if (*p != '"') return 0;
    p++;
    
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

// 从 JSON 提取 docstring
static int extract_docstring(const char* json, char* out, int max_len) {
    const char* p = strstr(json, "\"docstring\":");
    if (!p) return 0;
    p += 12;
    while (*p && isspace(*p)) p++;
    if (*p != '"') return 0;
    p++;
    
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

// 构建文本：signature + docstring + content 前5行
static void build_text(const char* json, char* text, int max_len) {
    char sig[512] = {0};
    char doc[512] = {0};
    char content[1024] = {0};
    
    extract_signature(json, sig, sizeof(sig));
    extract_docstring(json, doc, sizeof(doc));
    extract_content(json, content, sizeof(content));
    
    // 只取 content 前5行
    char lines[5][256];
    int line_count = 0;
    char* p = content;
    while (*p && line_count < 5) {
        int i = 0;
        while (*p && *p != '\n' && i < 255) {
            lines[line_count][i++] = *p++;
        }
        lines[line_count][i] = '\0';
        line_count++;
        if (*p == '\n') p++;
    }
    
    int n = 0;
    if (sig[0]) n += snprintf(text + n, max_len - n, "%s\n", sig);
    if (doc[0]) n += snprintf(text + n, max_len - n, "%s\n", doc);
    for (int i = 0; i < line_count && n < max_len - 1; i++) {
        n += snprintf(text + n, max_len - n, "%s\n", lines[i]);
    }
}

// 提取函数名从 chunk key
// key format: /code/.../chunks/{file}/{name}
static void extract_name(const char* key, char* name, int max_len) {
    const char* last_slash = strrchr(key, '/');
    if (last_slash) {
        strncpy(name, last_slash + 1, max_len - 1);
        name[max_len - 1] = '\0';
    } else {
        strncpy(name, key, max_len - 1);
        name[max_len - 1] = '\0';
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <cache_dir> [namespace]\n", argv[0]);
        return 1;
    }
    
    const char* cache_dir = argv[1];
    const char* namespace = argc > 2 ? argv[2] : "/code/local/stable-diffusion.cpp";
    
    printf("Opening cache: %s\n", cache_dir);
    cache_t* cache = cache_open(cache_dir, 500 * 1024 * 1024);
    if (!cache) {
        fprintf(stderr, "Failed to open cache\n");
        return 1;
    }
    
    printf("Loading embedder...\n");
    onnx_embedder_t* embedder = onnx_embedder_init(
        "models/all-MiniLM-L6-v2/model.onnx",
        "models/all-MiniLM-L6-v2/vocab.txt",
        128, DIM
    );
    if (!embedder) {
        fprintf(stderr, "Failed: %s\n", onnx_embedder_error());
        cache_close(cache);
        return 1;
    }
    
    // 收集所有 chunks
    printf("Collecting chunks...\n");
    item_t* items = malloc(50000 * sizeof(item_t));
    int item_count = 0;
    
    char chunk_prefix[512];
    snprintf(chunk_prefix, sizeof(chunk_prefix), "%s/chunks/", namespace);
    size_t prefix_len = strlen(chunk_prefix);
    
    // 遍历所有 key
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        if (strncmp(key, chunk_prefix, prefix_len) != 0) continue;
        
        // 检查是否已有向量
        char name[256];
        extract_name(key, name, sizeof(name));
        
        char vec_key[512];
        snprintf(vec_key, sizeof(vec_key), "%s/vectors/%s", namespace, name);
        if (cache_get(cache, vec_key)) continue;  // 已有向量
        
        // 构建文本
        build_text(value, items[item_count].text, MAX_TEXT_LEN);
        strcpy(items[item_count].name, name);
        
        if (strlen(items[item_count].text) > 10) {  // 至少10个字符
            item_count++;
        }
        
        if (item_count >= 50000) break;
    }
    cache_iter_destroy(iter);
    
    printf("Found %d items to encode\n", item_count);
    
    if (item_count == 0) {
        printf("Nothing to do\n");
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 0;
    }
    
    // 批量编码
    printf("Encoding with batch size %d...\n", BATCH_SIZE);
    float* vectors = malloc(item_count * DIM * sizeof(float));
    int processed = 0;
    
    // 分配文本指针数组
    const char** texts = malloc(BATCH_SIZE * sizeof(char*));
    
    for (int i = 0; i < item_count; i += BATCH_SIZE) {
        int batch_end = i + BATCH_SIZE;
        if (batch_end > item_count) batch_end = item_count;
        int batch_count = batch_end - i;
        
        // 准备 batch 文本
        for (int j = 0; j < batch_count; j++) {
            texts[j] = items[i + j].text;
        }
        
        // Batch encode
        int ret = onnx_embedder_encode_batch(embedder, texts, batch_count, vectors + i * DIM);
        if (ret == 0) {
            processed += batch_count;
        } else {
            // Fallback to single encode on batch failure
            for (int j = 0; j < batch_count; j++) {
                if (onnx_embedder_encode(embedder, items[i + j].text, vectors + (i + j) * DIM) == 0) {
                    processed++;
                }
            }
        }
        
        if ((i / BATCH_SIZE) % 10 == 0) {
            printf("  Progress: %d/%d\n", i, item_count);
        }
    }
    
    free(texts);
    printf("Encoded %d/%d items\n", processed, item_count);
    
    // 存储向量
    printf("Storing vectors...\n");
    int stored = 0;
    for (int i = 0; i < item_count; i++) {
        char vec_key[512];
        snprintf(vec_key, sizeof(vec_key), "%s/vectors/%s", namespace, items[i].name);
        
        // 构建 JSON: {"name":"...","vector":[...]}
        char json[DIM * 16 + 256];
        int n = snprintf(json, sizeof(json), "{\"name\":\"%s\",\"vector\":[", items[i].name);
        
        for (int j = 0; j < DIM && n < sizeof(json) - 32; j++) {
            if (j > 0) n += snprintf(json + n, sizeof(json) - n, ",");
            n += snprintf(json + n, sizeof(json) - n, "%.6f", vectors[i * DIM + j]);
        }
        n += snprintf(json + n, sizeof(json) - n, "]}");
        
        cache_set(cache, vec_key, json, 0);
        stored++;
    }
    
    printf("Stored %d vectors\n", stored);
    
    // 保存
    cache_sync(cache);
    
    free(items);
    free(vectors);
    onnx_embedder_free(embedder);
    cache_close(cache);
    
    printf("Done!\n");
    return 0;
}
