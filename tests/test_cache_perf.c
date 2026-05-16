#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>

#define N_KEYS 5000
#define KEY_SIZE 32
#define VALUE_SIZE 256

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void gen_key(char* buf, int idx) {
    snprintf(buf, KEY_SIZE, "/perf/test-%08d", idx);
}

static void gen_value(char* buf, int idx) {
    snprintf(buf, VALUE_SIZE, 
             "{\"t\":\"content\",\"idx\":%d,\"data\":\"test value %d\"}",
             idx, idx);
}

static void test_insert(cache_t* cache) {
    printf("=== Insert %d entries ===\n", N_KEYS);
    
    char key[KEY_SIZE];
    char value[VALUE_SIZE];
    
    uint64_t start = now_ms();
    for (int i = 0; i < N_KEYS; i++) {
        gen_key(key, i);
        gen_value(value, i);
        cache_set(cache, key, value, 0);
    }
    uint64_t elapsed = now_ms() - start;
    
    printf("  Time: %lu ms\n", elapsed);
    printf("  OPS:  %.0f ops/sec\n", (double)N_KEYS / (elapsed / 1000.0));
    printf("  Memory: %.1f MB\n", cache_memory_used(cache) / 1024.0 / 1024.0);
}

static void test_get(cache_t* cache) {
    printf("\n=== Random Get ===\n");
    
    char key[KEY_SIZE];
    int hits = 0;
    
    uint64_t start = now_ms();
    for (int i = 0; i < N_KEYS; i++) {
        int idx = (i % 2 == 0) ? (i / 2) : (N_KEYS + i);
        gen_key(key, idx);
        if (cache_get(cache, key)) hits++;
    }
    uint64_t elapsed = now_ms() - start;
    
    printf("  Time: %lu ms\n", elapsed);
    printf("  OPS:  %.0f ops/sec\n", (double)N_KEYS / (elapsed / 1000.0));
    printf("  Hits: %d/%d\n", hits, N_KEYS);
}

static void test_search(cache_t* cache) {
    printf("\n=== Prefix Search ===\n");
    
    cache_search_options_t opts = cache_search_options_default();
    cache_result_t* results = NULL;
    size_t count = 0;
    
    uint64_t start = now_ms();
    int n = 1000;
    for (int i = 0; i < n; i++) {
        cache_search_prefix(cache, "/perf/test-00001", &opts, &results, &count);
        if (results) cache_results_free(results);
        results = NULL;
    }
    uint64_t elapsed = now_ms() - start;
    
    printf("  Time: %lu ms (%d searches)\n", elapsed, n);
    printf("  OPS:  %.0f searches/sec\n", (double)n / (elapsed / 1000.0));
}

static void test_iterate(cache_t* cache) {
    printf("\n=== Iterate All ===\n");
    
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    int count = 0;
    
    uint64_t start = now_ms();
    while (cache_iter_next(iter, &key, &value)) {
        count++;
    }
    uint64_t elapsed = now_ms() - start;
    
    printf("  Time: %lu ms\n", elapsed);
    printf("  Entries: %d\n", count);
    printf("  Throughput: %.0f entries/sec\n", count / (elapsed / 1000.0));
    
    cache_iter_destroy(iter);
}

static void test_delete(cache_t* cache) {
    printf("\n=== Delete Every 10th ===\n");
    
    char key[KEY_SIZE];
    int deleted = 0;
    
    uint64_t start = now_ms();
    for (int i = 0; i < N_KEYS; i += 10) {
        gen_key(key, i);
        if (cache_del(cache, key) == CACHE_OK) deleted++;
    }
    uint64_t elapsed = now_ms() - start;
    
    printf("  Time: %lu ms\n", elapsed);
    printf("  Deleted: %d\n", deleted);
    printf("  Remaining: %zu\n", cache_count(cache));
}

int main(void) {
    printf("========================================\n");
    printf("     KV Cache Performance Benchmark\n");
    printf("========================================\n");
    printf("Keys: %d (sorted array O(n) insert)\n", N_KEYS);
    printf("Key size: ~%d bytes, Value size: ~%d bytes\n", KEY_SIZE, VALUE_SIZE);
    printf("Note: 100K insert is slow due to O(n) sorted array, to be optimized in Phase 10\n");
    printf("========================================\n\n");
    
    system("rm -rf /tmp/test_perf");
    
    cache_t* cache = cache_open("/tmp/test_perf", 500 * 1024 * 1024);
    if (!cache) { fprintf(stderr, "Failed to open cache\n"); return 1; }
    
    test_insert(cache);
    test_get(cache);
    test_search(cache);
    test_iterate(cache);
    test_delete(cache);
    
    printf("\n========================================\n");
    printf("     Benchmark Complete\n");
    printf("========================================\n");
    
    cache_close(cache);
    system("rm -rf /tmp/test_perf");
    return 0;
}
