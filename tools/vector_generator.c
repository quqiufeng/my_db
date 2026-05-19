#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
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

static void build_text(const char* json, char* text, int max_len);
static void extract_name(const char* key, char* name, int max_len);

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <cache_dir> [namespace]\n", argv[0]);
        return 1;
    }
    
    const char* cache_dir = argv[1];
    const char* namespace = argc > 2 ? argv[2] : "/code/local/stable-diffusion.cpp";
    
    printf("Opening cache: %s\n", cache_dir);
    cache_t* cache = cache_open(cache_dir, 1024 * 1024 * 1024);
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
    
    // 收集 chunks
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
        
        // Check if vector already exists (check cache key)
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
    
    // 编码
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
    
    // 存储到二进制文件（不经过 cache JSON）
    printf("Storing vectors to binary file...\n");
    clock_t store_start = clock();
    
    // Create vectors directory
    char vec_dir[512];
    snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    mkdir(vec_dir, 0755);
    
    // Build safe filename from namespace
    char safe_ns[256];
    int si = 0;
    for (int i = 0; namespace[i] && si < 255; i++) {
        if (namespace[i] == '/') {
            if (si > 0 && safe_ns[si-1] != '_') {
                safe_ns[si++] = '_';
            }
        } else {
            safe_ns[si++] = namespace[i];
        }
    }
    safe_ns[si] = '\0';
    
    char vec_file[512], idx_file[512];
    snprintf(vec_file, sizeof(vec_file), "%s/%s.bin", vec_dir, safe_ns);
    snprintf(idx_file, sizeof(idx_file), "%s/%s.idx", vec_dir, safe_ns);
    
    // Write binary file: [count:4][dim:4][vectors...]
    FILE* fp = fopen(vec_file, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to create %s\n", vec_file);
        free(items);
        free(vectors);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    uint32_t count = item_count;
    uint32_t dim = DIM;
    fwrite(&count, 4, 1, fp);
    fwrite(&dim, 4, 1, fp);
    fwrite(vectors, sizeof(float), item_count * DIM, fp);
    fclose(fp);
    
    // Write index file: JSON mapping name -> offset
    size_t offset = 8;  // skip header
    FILE* idx_fp = fopen(idx_file, "w");
    if (idx_fp) {
        fprintf(idx_fp, "{");
        for (int i = 0; i < item_count; i++) {
            if (i > 0) fprintf(idx_fp, ",");
            fprintf(idx_fp, "\"%s\":%zu", items[i].name, offset);
            offset += DIM * sizeof(float);
        }
        fprintf(idx_fp, "}\n");
        fclose(idx_fp);
    }
    
    // Skip writing to cache (slow for large datasets)
    // Binary file is the source of truth
    printf("Binary file: %s\n", vec_file);
    printf("Index file: %s\n", idx_file);
    
    double store_time = (double)(clock() - store_start) / CLOCKS_PER_SEC;
    printf("Stored %d vectors in %.1fs (%.1f items/s)\n",
           item_count, store_time, item_count / store_time);
    
    // Verify file size
    struct stat st;
    if (stat(vec_file, &st) == 0) {
        size_t expected = 8 + item_count * DIM * sizeof(float);
        printf("File size: %ld bytes (expected: %zu) OK=%d\n", 
               st.st_size, expected, st.st_size == (long)expected);
    }
    
    printf("Done! Total: %.1fs\n", encode_time + store_time);
    
    free(items);
    free(vectors);
    onnx_embedder_free(embedder);
    cache_close(cache);
    
    return 0;
}

// 从 JSON 提取字段
static int extract_field(const char* json, const char* field, char* out, int max_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char* p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
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

static void build_text(const char* json, char* text, int max_len) {
    char sig[512] = {0};
    char doc[512] = {0};
    char content[1024] = {0};
    
    extract_field(json, "signature", sig, sizeof(sig));
    extract_field(json, "docstring", doc, sizeof(doc));
    extract_field(json, "content", content, sizeof(content));
    
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
