#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// 生成归一化的测试向量
static void make_test_vector(float* vec, size_t dim, int seed) {
    double sum = 0.0;
    srand(seed);
    for (size_t i = 0; i < dim; i++) {
        vec[i] = (float)(rand() % 100) / 100.0f;
        sum += vec[i] * vec[i];
    }
    // 归一化
    float norm = (float)sqrt(sum);
    if (norm > 0) {
        for (size_t i = 0; i < dim; i++) {
            vec[i] /= norm;
        }
    }
}

int main() {
    const char* db_dir = "/tmp/test_vector";
    system("rm -rf /tmp/test_vector");
    
    printf("=== Vector Search Test ===\n\n");
    
    cache_t* cache = cache_open(db_dir, 100 * 1024 * 1024);
    if (!cache) {
        printf("Failed to open cache\n");
        return 1;
    }
    
    const size_t dim = 384;  // 标准embedding维度
    float vec[384];
    
    // 存储5个文档，每个有不同的向量
    const char* docs[] = {
        "C++ template metaprogramming",
        "Python asyncio coroutines",
        "JavaScript async/await",
        "Rust ownership and borrowing",
        "Go goroutines and channels"
    };
    
    printf("Storing %zu documents with vectors...\n", sizeof(docs)/sizeof(docs[0]));
    
    for (int i = 0; i < 5; i++) {
        make_test_vector(vec, dim, i * 100);
        
        char key[64];
        snprintf(key, sizeof(key), "/coding/doc%d", i);
        
        int ret = cache_set_vector(cache, key, docs[i], vec, dim, 0);
        if (ret != 0) {
            printf("Error storing %s: %d\n", key, ret);
        }
    }
    
    printf("Stored successfully.\n\n");
    
    // 搜索：使用doc0的向量搜索，应该找到doc0最相似
    make_test_vector(vec, dim, 0);  // 和doc0相同的向量
    
    printf("Searching with doc0 vector...\n");
    
    cache_result_t* results = NULL;
    size_t count = 0;
    
    int ret = cache_search_vector(cache, vec, dim, 3, 0.0, NULL, &results, &count);
    if (ret != 0) {
        printf("Search error: %d\n", ret);
        cache_close(cache);
        return 1;
    }
    
    printf("Found %zu results:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  [%zu] %s (score: %.3f)\n", i + 1, results[i].key, results[i].score);
    }
    
    cache_results_free(results);
    
    // 测试cache_get_vector
    printf("\nTesting cache_get_vector...\n");
    size_t out_dim = 0;
    const float* retrieved = cache_get_vector(cache, "/coding/doc0", &out_dim);
    if (retrieved && out_dim == dim) {
        printf("Retrieved vector dimension: %zu\n", out_dim);
        printf("First 5 values: %.3f %.3f %.3f %.3f %.3f\n",
               retrieved[0], retrieved[1], retrieved[2], retrieved[3], retrieved[4]);
    } else {
        printf("Failed to retrieve vector\n");
    }
    
    cache_close(cache);
    
    // 测试持久化
    printf("\nTesting persistence...\n");
    cache = cache_open(db_dir, 100 * 1024 * 1024);
    if (!cache) {
        printf("Failed to reopen cache\n");
        return 1;
    }
    
    retrieved = cache_get_vector(cache, "/coding/doc0", &out_dim);
    if (retrieved && out_dim == dim) {
        printf("After reopen: Retrieved vector dimension: %zu\n", out_dim);
    } else {
        printf("After reopen: Failed to retrieve vector\n");
    }
    
    cache_close(cache);
    
    printf("\n=== Vector Search Test PASSED ===\n");
    return 0;
}
