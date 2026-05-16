#include "cache.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main() {
    printf("=== KV Cache 基础测试 ===\n\n");
    
    // 1. 打开 cache
    cache_t* cache = cache_open("test_cache_dir", 10 * 1024 * 1024);  // 10MB
    assert(cache != NULL);
    printf("[OK] cache_open 成功\n");
    
    // 2. 设置 key-value
    int ret = cache_set(cache, "/coding/cpp/move", "右值引用实现完美转发...", 0);
    assert(ret == CACHE_OK);
    printf("[OK] cache_set 成功\n");
    
    // 3. 获取 value
    const char* value = cache_get(cache, "/coding/cpp/move");
    assert(value != NULL);
    assert(strcmp(value, "右值引用实现完美转发...") == 0);
    printf("[OK] cache_get 成功: %s\n", value);
    
    // 4. 设置多个 key
    cache_set(cache, "/coding/python/async", "asyncio 并发执行协程...", 0);
    cache_set(cache, "/agent/personality", "友好、专业、简洁", 0);
    printf("[OK] 设置多个 key\n");
    
    // 5. 统计信息
    printf("  条目数: %zu\n", cache_count(cache));
    printf("  已用内存: %zu bytes\n", cache_memory_used(cache));
    printf("  最大内存: %zu bytes\n", cache_memory_max(cache));
    
    // 6. 检查存在
    assert(cache_exists(cache, "/coding/cpp/move") == 1);
    assert(cache_exists(cache, "/not/exist") == 0);
    printf("[OK] cache_exists 正确\n");
    
    // 7. 删除
    ret = cache_del(cache, "/coding/cpp/move");
    assert(ret == CACHE_OK);
    assert(cache_get(cache, "/coding/cpp/move") == NULL);
    printf("[OK] cache_del 成功\n");
    
    // 8. 删除不存在的 key
    ret = cache_del(cache, "/not/exist");
    assert(ret == CACHE_ERR_NOENT);
    printf("[OK] 删除不存在 key 返回 CACHE_ERR_NOENT\n");
    
    // 9. sync
    ret = cache_sync(cache);
    assert(ret == CACHE_OK);
    printf("[OK] cache_sync 成功\n");
    
    // 10. 关闭
    cache_close(cache);
    printf("[OK] cache_close 成功\n");
    
    // 11. 重新打开（验证持久化）
    cache = cache_open("test_cache_dir", 10 * 1024 * 1024);
    assert(cache != NULL);
    value = cache_get(cache, "/coding/python/async");
    assert(value != NULL);
    assert(strcmp(value, "asyncio 并发执行协程...") == 0);
    printf("[OK] 重启后数据持久化正确\n");
    
    cache_close(cache);
    
    printf("\n=== 搜索测试 ===\n");
    
    // 重新打开并添加更多测试数据
    cache = cache_open("test_cache_dir", 10 * 1024 * 1024);
    cache_set(cache, "/coding/cpp/template", "模板元编程...", 0);
    cache_set(cache, "/coding/cpp/vector", "STL vector 动态数组...", 0);
    cache_set(cache, "/coding/python/decorator", "装饰器模式...", 0);
    cache_set(cache, "/knowledge/architecture/microservice", "微服务架构...", 0);
    
    // 12. 前缀搜索
    cache_search_options_t opts = cache_search_options_default();
    cache_result_t* results = NULL;
    size_t count = 0;
    
    ret = cache_search_prefix(cache, "/coding/cpp", &opts, &results, &count);
    assert(ret == CACHE_OK);
    printf("[OK] 前缀搜索 '/coding/cpp': %zu 条\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("    - %s: %s\n", results[i].key, results[i].value);
    }
    cache_results_free(results);
    
    // 13. 范围搜索
    ret = cache_search_range(cache, "/coding/cpp/", "/coding/cpp0", &opts, &results, &count);
    assert(ret == CACHE_OK);
    printf("[OK] 范围搜索 ['/coding/cpp/', '/coding/cpp0'): %zu 条\n", count);
    cache_results_free(results);
    
    // 14. 正则搜索
    ret = cache_search_regex(cache, "/coding/.*/async", &opts, &results, &count);
    assert(ret == CACHE_OK);
    printf("[OK] 正则搜索 '/coding/.*/async': %zu 条\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("    - %s\n", results[i].key);
    }
    cache_results_free(results);
    
    // 15. 模糊搜索
    ret = cache_search_fuzzy(cache, "/coding/cpp/vect", &opts, &results, &count);
    assert(ret == CACHE_OK);
    printf("[OK] 模糊搜索 '/coding/cpp/vect': %zu 条\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("    - %s (score: %.2f)\n", results[i].key, results[i].score);
    }
    cache_results_free(results);
    
    // 16. 标签搜索（value 中包含关键词）
    ret = cache_search_tag(cache, "协程", &opts, &results, &count);
    assert(ret == CACHE_OK);
    printf("[OK] 标签搜索 '协程': %zu 条\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("    - %s: %s\n", results[i].key, results[i].value);
    }
    cache_results_free(results);
    
    cache_close(cache);
    
    printf("\n=== 所有测试通过 ===\n");
    return 0;
}