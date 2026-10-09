#include "cache.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <time.h>

static void test_basic_crud() {
    printf("=== Test: Basic CRUD ===\n");
    
    system("rm -rf /tmp/test_crud");
    cache_t* cache = cache_open("/tmp/test_crud", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    // set/get
    assert(cache_set(cache, "key1", "value1", 0) == CACHE_OK);
    assert(strcmp(cache_get(cache, "key1"), "value1") == 0);
    
    // update
    assert(cache_set(cache, "key1", "value2", 0) == CACHE_OK);
    assert(strcmp(cache_get(cache, "key1"), "value2") == 0);
    
    // non-existent
    assert(cache_get(cache, "nonexistent") == NULL);
    
    // exists
    assert(cache_exists(cache, "key1") == 1);
    assert(cache_exists(cache, "nonexistent") == 0);
    
    // delete
    assert(cache_del(cache, "key1") == CACHE_OK);
    assert(cache_get(cache, "key1") == NULL);
    assert(cache_del(cache, "key1") == CACHE_ERR_NOENT);
    
    cache_close(cache);
    system("rm -rf /tmp/test_crud");
    printf("[PASS]\n\n");
}

static void test_namespace() {
    printf("=== Test: Namespace ===\n");
    
    system("rm -rf /tmp/test_ns");
    cache_t* cache = cache_open("/tmp/test_ns", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    // set_ns/get_ns
    assert(cache_set_ns(cache, "/coding/cpp", "move", "右值引用...", 0) == CACHE_OK);
    assert(strcmp(cache_get_ns(cache, "/coding/cpp", "move"), "右值引用...") == 0);
    
    // verify full key exists
    assert(cache_get(cache, "/coding/cpp/move") != NULL);
    
    // del_ns
    assert(cache_del_ns(cache, "/coding/cpp", "move") == CACHE_OK);
    assert(cache_get_ns(cache, "/coding/cpp", "move") == NULL);
    
    // nested namespace
    assert(cache_set_ns(cache, "/a/b/c", "key", "deep", 0) == CACHE_OK);
    assert(strcmp(cache_get_ns(cache, "/a/b/c", "key"), "deep") == 0);
    
    cache_close(cache);
    system("rm -rf /tmp/test_ns");
    printf("[PASS]\n\n");
}

static void test_search() {
    printf("=== Test: Search ===\n");
    
    system("rm -rf /tmp/test_search");
    cache_t* cache = cache_open("/tmp/test_search", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    cache_set(cache, "/coding/cpp/move", "右值引用...", 0);
    cache_set(cache, "/coding/cpp/template", "模板元编程...", 0);
    cache_set(cache, "/coding/python/async", "asyncio...", 0);
    cache_set(cache, "/agent/personality", "友好...", 0);
    
    cache_search_options_t opts = cache_search_options_default();
    cache_result_t* results = NULL;
    size_t count = 0;
    
    // prefix search
    assert(cache_search_prefix(cache, "/coding/cpp", &opts, &results, &count) == CACHE_OK);
    assert(count == 2);
    cache_results_free(results);
    printf("  prefix: %zu results\n", count);
    
    // range search
    assert(cache_search_range(cache, "/coding/cpp/", "/coding/cpp0", &opts, &results, &count) == CACHE_OK);
    assert(count == 2);
    cache_results_free(results);
    printf("  range: %zu results\n", count);
    
    // regex search
    assert(cache_search_regex(cache, "/coding/.*/async", &opts, &results, &count) == CACHE_OK);
    assert(count == 1);
    cache_results_free(results);
    printf("  regex: %zu results\n", count);
    
    // fuzzy search
    assert(cache_search_fuzzy(cache, "/coding/cpp/vect", &opts, &results, &count) == CACHE_OK);
    assert(count >= 1);
    cache_results_free(results);
    printf("  fuzzy: %zu results\n", count);
    
    cache_close(cache);
    system("rm -rf /tmp/test_search");
    printf("[PASS]\n\n");
}

static void test_ttl_lru() {
    printf("=== Test: TTL & LRU ===\n");
    
    system("rm -rf /tmp/test_ttl");
    cache_t* cache = cache_open("/tmp/test_ttl", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    // TTL test
    assert(cache_set(cache, "/ttl/key", "value", 100) == CACHE_OK);  // 100ms
    assert(cache_get(cache, "/ttl/key") != NULL);
    
    struct timespec ts = {0, 150000000};  // 150ms
    nanosleep(&ts, NULL);
    
    assert(cache_get(cache, "/ttl/key") == NULL);
    printf("  TTL: OK\n");
    
    // LRU test
    cache_close(cache);
    system("rm -rf /tmp/test_ttl");
    
    cache = cache_open("/tmp/test_ttl", 60);  // very small
    assert(cache != NULL);
    
    assert(cache_set(cache, "a", "1", 1000) == CACHE_OK);
    assert(cache_set(cache, "b", "2", 1000) == CACHE_OK);
    
    // 'a' should be evicted
    assert(cache_get(cache, "a") == NULL);
    assert(cache_get(cache, "b") != NULL);
    printf("  LRU: OK\n");
    
    cache_close(cache);
    system("rm -rf /tmp/test_ttl");
    printf("[PASS]\n\n");
}

static void test_iterator() {
    printf("=== Test: Iterator ===\n");
    
    system("rm -rf /tmp/test_iter");
    cache_t* cache = cache_open("/tmp/test_iter", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    cache_set(cache, "z", "last", 0);
    cache_set(cache, "a", "first", 0);
    cache_set(cache, "m", "middle", 0);
    
    cache_iter_t* iter = cache_iter_create(cache);
    assert(iter != NULL);
    
    const char* key;
    const char* value;
    int count = 0;
    
    printf("  Iteration order: ");
    while (cache_iter_next(iter, &key, &value)) {
        printf("%s ", key);
        count++;
    }
    printf("\n");
    
    assert(count == 3);
    
    // reset and iterate again
    cache_iter_reset(iter);
    count = 0;
    while (cache_iter_next(iter, &key, &value)) {
        count++;
    }
    assert(count == 3);
    
    cache_iter_destroy(iter);
    cache_close(cache);
    system("rm -rf /tmp/test_iter");
    printf("[PASS]\n\n");
}

static void test_persistence() {
    printf("=== Test: Persistence ===\n");
    
    system("rm -rf /tmp/test_persist");
    
    // First session
    cache_t* cache = cache_open("/tmp/test_persist", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    cache_set(cache, "persist_key", "persist_value", 0);
    cache_set(cache, "/ns/item", "ns_value", 0);
    cache_sync(cache);
    cache_close(cache);
    
    // Second session
    cache = cache_open("/tmp/test_persist", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    assert(strcmp(cache_get(cache, "persist_key"), "persist_value") == 0);
    assert(strcmp(cache_get(cache, "/ns/item"), "ns_value") == 0);
    
    cache_close(cache);
    system("rm -rf /tmp/test_persist");
    printf("[PASS]\n\n");
}

static void test_stats() {
    printf("=== Test: Stats ===\n");
    
    system("rm -rf /tmp/test_stats");
    cache_t* cache = cache_open("/tmp/test_stats", 10 * 1024 * 1024);
    assert(cache != NULL);
    
    assert(cache_count(cache) == 0);
    assert(cache_memory_used(cache) == 0);
    assert(cache_memory_max(cache) == 10 * 1024 * 1024);
    
    cache_set(cache, "key1", "value1", 0);
    assert(cache_count(cache) == 1);
    assert(cache_memory_used(cache) > 0);
    
    cache_set(cache, "key2", "value2", 0);
    assert(cache_count(cache) == 2);
    
    cache_del(cache, "key1");
    assert(cache_count(cache) == 1);
    
    cache_close(cache);
    system("rm -rf /tmp/test_stats");
    printf("[PASS]\n\n");
}

static void test_lru_many() {
    printf("=== Test: LRU (many candidates → qsort 淘汰) ===\n");

    system("rm -rf /tmp/test_lru_many");
    cache_t* cache = cache_open("/tmp/test_lru_many", 256 * 1024);  // 256KB
    assert(cache != NULL);

    char k[32], v[256];
    memset(v, 'x', sizeof(v) - 1);
    v[sizeof(v) - 1] = '\0';

    // 插入远超容量的非永久条目 → 触发批量淘汰（多候选，走 qsort）
    for (int i = 0; i < 3000; i++) {
        snprintf(k, sizeof(k), "key_%d", i);
        assert(cache_set(cache, k, v, 100000) == CACHE_OK);
    }

    size_t cnt = cache_count(cache);
    printf("  3000 inserts → count=%zu (应 <3000，发生淘汰)\n", cnt);
    assert(cnt < 3000);
    assert(cache_get(cache, "key_2999") != NULL);

    cache_close(cache);
    system("rm -rf /tmp/test_lru_many");
    printf("[PASS]\n\n");
}

int main() {
    printf("========================================\n");
    printf("     KV Cache Test Suite\n");
    printf("========================================\n\n");
    
    test_basic_crud();
    test_namespace();
    test_search();
    test_ttl_lru();
    test_lru_many();
    test_iterator();
    test_persistence();
    test_stats();
    
    printf("========================================\n");
    printf("     All Tests PASSED!\n");
    printf("========================================\n");
    
    return 0;
}
