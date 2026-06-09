/*
 * export_book_vectors.c — 从已有 KV Cache 导出电子书向量
 */

#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>

#define EMBEDDING_DIM 768

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

int main(int argc, char* argv[]) {
    if (argc < 4) {
        printf("Usage: %s <cache_dir> <namespace> <book_name>\n", argv[0]);
        printf("Example: %s /memory /books/ddia ddia\n", argv[0]);
        return 1;
    }
    
    const char* cache_dir = argv[1];
    const char* namespace = argv[2];
    const char* book_name = argv[3];
    
    cache_t* cache = cache_open(cache_dir, 0);
    if (!cache) {
        fprintf(stderr, "Failed to open cache: %s\n", cache_dir);
        return 1;
    }
    
    // 创建 vectors 目录
    char vec_dir[1024];
    snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    mkdir_p(vec_dir);
    
    char bin_file[1024], idx_file[1024];
    snprintf(bin_file, sizeof(bin_file), "%s/books_%s.jina.bin", vec_dir, book_name);
    snprintf(idx_file, sizeof(idx_file), "%s/books_%s.jina.idx", vec_dir, book_name);
    
    // 收集带向量的 entry
    typedef struct {
        char* name;
        float* vector;
    } Entry;
    
    Entry* entries = malloc(sizeof(Entry) * 100000);
    int count = 0;
    size_t ns_len = strlen(namespace);
    
    cache_search_options_t opts = cache_search_options_default();
    opts.max_results = 0; // 无限制
    cache_result_t* results = NULL;
    size_t result_count = 0;
    
    // 使用前缀搜索查找所有属于本书的 key
    char prefix[512];
    snprintf(prefix, sizeof(prefix), "%s/", namespace);
    cache_search_prefix(cache, prefix, &opts, &results, &result_count);
    
    printf("Prefix search found %zu entries for %s\n", result_count, prefix);
    
    for (size_t i = 0; i < result_count; i++) {
        const char* key = results[i].key;
        if (strstr(key, "/_meta")) continue;
        if (!strstr(key, "/page_")) continue;
        
        size_t dim = 0;
        const float* vec = cache_get_vector(cache, key, &dim);
        if (!vec || dim == 0) {
            printf("  No vector for: %s\n", key);
            continue;
        }
        
        entries[count].name = strdup(key);
        entries[count].vector = malloc(sizeof(float) * dim);
        memcpy(entries[count].vector, vec, sizeof(float) * dim);
        count++;
    }
    cache_results_free(results);
    
    printf("Found %d vectors for %s\n", count, namespace);
    
    if (count == 0) {
        free(entries);
        cache_close(cache);
        return 0;
    }
    
    // 写入文件
    FILE* bin_fp = fopen(bin_file, "wb");
    FILE* idx_fp = fopen(idx_file, "w");
    if (!bin_fp || !idx_fp) {
        fprintf(stderr, "Failed to create output files\n");
        if (bin_fp) fclose(bin_fp);
        if (idx_fp) fclose(idx_fp);
        cache_close(cache);
        return 1;
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
        fprintf(idx_fp, "\"%s\":%zu\n", entries[i].name, offset + 4 + name_len);
        
        offset += 4 + name_len + EMBEDDING_DIM * sizeof(float);
    }
    
    fclose(bin_fp);
    fclose(idx_fp);
    
    printf("Exported to:\n");
    printf("  %s (%d vectors)\n", bin_file, count);
    printf("  %s\n", idx_file);
    
    // 构建 HNSW 索引（语义搜索必需）
    printf("Building HNSW index...\n");
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
    cache_close(cache);
    
    return 0;
}
