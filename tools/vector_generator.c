#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "cache.h"
#include "onnx_embedder.h"

#define DIM 384
#define BATCH_SIZE 64
#define MAX_TEXT_LEN 2048
#define MAX_ITEMS 100000

typedef struct {
    char name[256];
    char text[MAX_TEXT_LEN];
} item_t;

// Base64 encoding table
static const char base64_chars[] = 
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode float array to base64 string
// output buffer must be at least (count * 4 / 3 + 4) * 1.5 to be safe
static int encode_vector_base64(const float* vec, int dim, char* out, int out_len) {
    const unsigned char* data = (const unsigned char*)vec;
    int data_len = dim * sizeof(float);  // 1536 bytes for dim=384
    int i = 0, j = 0;
    unsigned char arr[3];
    int n = 0;
    
    while (i < data_len) {
        arr[0] = data[i++];
        arr[1] = (i < data_len) ? data[i++] : 0;
        arr[2] = (i < data_len) ? data[i++] : 0;
        
        if (n + 4 >= out_len) return -1;
        
        out[n++] = base64_chars[arr[0] >> 2];
        out[n++] = base64_chars[((arr[0] & 3) << 4) | (arr[1] >> 4)];
        out[n++] = base64_chars[((arr[1] & 15) << 2) | (arr[2] >> 6)];
        out[n++] = base64_chars[arr[2] & 63];
        j++;
    }
    
    // Pad if needed
    int pad = (3 - (data_len % 3)) % 3;
    for (i = 0; i < pad; i++) {
        out[n - 1 - i] = '=';
    }
    out[n] = '\0';
    return n;
}

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
    cache_t* cache = cache_open(cache_dir, 1024 * 1024 * 1024);  // 1GB
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
    item_t* items = malloc(MAX_ITEMS * sizeof(item_t));
    int item_count = 0;
    
    char chunk_prefix[512];
    snprintf(chunk_prefix, sizeof(chunk_prefix), "%s/chunks/", namespace);
    size_t prefix_len = strlen(chunk_prefix);
    
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        if (strncmp(key, chunk_prefix, prefix_len) != 0) continue;
        
        char name[256];
        extract_name(key, name, sizeof(name));
        
        char vec_key[512];
        snprintf(vec_key, sizeof(vec_key), "%s/vectors/%s", namespace, name);
        if (cache_get(cache, vec_key)) continue;
        
        build_text(value, items[item_count].text, MAX_TEXT_LEN);
        strcpy(items[item_count].name, name);
        
        if (strlen(items[item_count].text) > 10) {
            item_count++;
        }
        
        if (item_count >= MAX_ITEMS) break;
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
    clock_t encode_start = clock();
    
    const char** texts = malloc(BATCH_SIZE * sizeof(char*));
    
    for (int i = 0; i < item_count; i += BATCH_SIZE) {
        int batch_end = i + BATCH_SIZE;
        if (batch_end > item_count) batch_end = item_count;
        int batch_count = batch_end - i;
        
        for (int j = 0; j < batch_count; j++) {
            texts[j] = items[i + j].text;
        }
        
        int ret = onnx_embedder_encode_batch(embedder, texts, batch_count, vectors + i * DIM);
        if (ret == 0) {
            processed += batch_count;
        } else {
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
    double encode_time = (double)(clock() - encode_start) / CLOCKS_PER_SEC;
    printf("Encoded %d/%d items in %.1fs (%.1f items/s)\n", 
           processed, item_count, encode_time, processed / encode_time);
    
    // 存储向量（使用 base64 紧凑格式）
    printf("Storing vectors (base64 compact format)...\n");
    int stored = 0;
    clock_t store_start = clock();
    char* base64_buf = malloc(4096);  // enough for 384 floats base64
    
    for (int i = 0; i < item_count; i++) {
        char vec_key[512];
        snprintf(vec_key, sizeof(vec_key), "%s/vectors/%s", namespace, items[i].name);
        
        // Encode vector as base64 (no JSON overhead)
        int len = encode_vector_base64(vectors + i * DIM, DIM, base64_buf, 4096);
        if (len > 0) {
            cache_set(cache, vec_key, base64_buf, 0);
            stored++;
        }
        
        // Sync every 5000 items to avoid cache_close freeze
        if (stored % 5000 == 0) {
            cache_sync(cache);
            printf("  Synced at %d\n", stored);
        }
    }
    
    free(base64_buf);
    double store_time = (double)(clock() - store_start) / CLOCKS_PER_SEC;
    printf("Stored %d vectors in %.1fs (%.1f items/s)\n",
           stored, store_time, stored / store_time);
    
    // 最终保存
    printf("Final sync...\n");
    cache_sync(cache);
    
    free(items);
    free(vectors);
    onnx_embedder_free(embedder);
    cache_close(cache);
    
    printf("Done! Total: %.1fs\n", encode_time + store_time);
    return 0;
}
