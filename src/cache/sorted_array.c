#include "cache_internal.h"
#include <string.h>

#define CACHE_PTR(cache, offset) ((void*)((char*)(cache)->pool.base + (offset)))

// 比较两个 entry 的 key（用于 qsort 和二分查找）
static int compare_entries(const void* a, const void* b, void* arg) {
    cache_t* cache = (cache_t*)arg;
    size_t offset_a = *(const size_t*)a;
    size_t offset_b = *(const size_t*)b;
    
    cache_entry_header_t* ha = (cache_entry_header_t*)CACHE_PTR(cache, offset_a);
    cache_entry_header_t* hb = (cache_entry_header_t*)CACHE_PTR(cache, offset_b);
    
    const char* key_a = (const char*)CACHE_PTR(cache, offset_a + sizeof(cache_entry_header_t));
    const char* key_b = (const char*)CACHE_PTR(cache, offset_b + sizeof(cache_entry_header_t));
    
    size_t min_len = ha->key_len < hb->key_len ? ha->key_len : hb->key_len;
    int cmp = memcmp(key_a, key_b, min_len);
    if (cmp != 0) return cmp;
    return (int)(ha->key_len - hb->key_len);
}

// 比较 key 字符串和 entry（用于二分查找）
static int compare_key_with_entry(const char* key, size_t key_len, cache_t* cache, size_t entry_offset) {
    cache_entry_header_t* h = (cache_entry_header_t*)CACHE_PTR(cache, entry_offset);
    const char* entry_key = (const char*)CACHE_PTR(cache, entry_offset + sizeof(cache_entry_header_t));
    
    size_t min_len = key_len < h->key_len ? key_len : h->key_len;
    int cmp = memcmp(key, entry_key, min_len);
    if (cmp != 0) return cmp;
    return (int)(key_len - h->key_len);
}

// 初始化排序数组
int cache_sorted_init(cache_t* cache) {
    if (!cache) return -1;
    
    cache->sorted.offsets = NULL;
    cache->sorted.count = 0;
    cache->sorted.capacity = 0;
    
    return 0;
}

// 确保容量足够
static int ensure_capacity(cache_sorted_array_t* sorted, size_t need) {
    if (need <= sorted->capacity) return 0;
    
    size_t new_cap = sorted->capacity * 2;
    if (new_cap < need) new_cap = need;
    if (new_cap < 16) new_cap = 16;
    
    size_t* new_offsets = (size_t*)realloc(sorted->offsets, sizeof(size_t) * new_cap);
    if (!new_offsets) return -1;
    
    sorted->offsets = new_offsets;
    sorted->capacity = new_cap;
    return 0;
}

// 插入 entry_offset（保持有序）
int cache_sorted_insert(cache_t* cache, size_t entry_offset) {
    if (!cache) return -1;
    
    cache_sorted_array_t* sorted = &cache->sorted;
    
    if (ensure_capacity(sorted, sorted->count + 1) < 0) return -1;
    
    // 找到插入位置（二分查找）
    size_t left = 0, right = sorted->count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (compare_entries(&sorted->offsets[mid], &entry_offset, cache) < 0) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    
    // 后移元素
    memmove(&sorted->offsets[left + 1], &sorted->offsets[left], 
            (sorted->count - left) * sizeof(size_t));
    
    sorted->offsets[left] = entry_offset;
    sorted->count++;
    
    return 0;
}

// 删除 entry_offset
int cache_sorted_remove(cache_t* cache, const char* key, size_t key_len) {
    if (!cache || !key || key_len == 0) return -1;
    
    cache_sorted_array_t* sorted = &cache->sorted;
    if (sorted->count == 0) return -1;
    
    // 二分查找
    size_t left = 0, right = sorted->count;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        int cmp = compare_key_with_entry(key, key_len, cache, sorted->offsets[mid]);
        if (cmp > 0) {
            left = mid + 1;
        } else if (cmp < 0) {
            right = mid;
        } else {
            // 找到，删除
            memmove(&sorted->offsets[mid], &sorted->offsets[mid + 1],
                    (sorted->count - mid - 1) * sizeof(size_t));
            sorted->count--;
            return 0;
        }
    }
    
    return -1;  // 未找到
}

// 二分查找 lower_bound（第一个 >= key 的位置）
size_t cache_sorted_find_lower_bound(cache_t* cache, const char* key, size_t key_len) {
    if (!cache || !key || key_len == 0 || cache->sorted.count == 0) return 0;
    
    cache_sorted_array_t* sorted = &cache->sorted;
    size_t left = 0, right = sorted->count;
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (compare_key_with_entry(key, key_len, cache, sorted->offsets[mid]) <= 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    
    return left;
}

// 二分查找 upper_bound（第一个 > key 的位置）
size_t cache_sorted_find_upper_bound(cache_t* cache, const char* key, size_t key_len) {
    if (!cache || !key || key_len == 0 || cache->sorted.count == 0) return 0;
    
    cache_sorted_array_t* sorted = &cache->sorted;
    size_t left = 0, right = sorted->count;
    
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (compare_key_with_entry(key, key_len, cache, sorted->offsets[mid]) < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    
    return left;
}

// 获取指定位置的 entry_offset
size_t cache_sorted_get(cache_t* cache, size_t index) {
    if (!cache || index >= cache->sorted.count) return 0;
    return cache->sorted.offsets[index];
}

// qsort 比较函数（需要 cache 指针，通过全局 TLS 传递）
static __thread cache_t* g_compare_cache = NULL;

static int compare_entries_qsort(const void* a, const void* b) {
    cache_t* cache = g_compare_cache;
    if (!cache) return 0;
    
    size_t offset_a = *(const size_t*)a;
    size_t offset_b = *(const size_t*)b;
    
    cache_entry_header_t* ha = (cache_entry_header_t*)CACHE_PTR(cache, offset_a);
    cache_entry_header_t* hb = (cache_entry_header_t*)CACHE_PTR(cache, offset_b);
    
    const char* key_a = (const char*)CACHE_PTR(cache, offset_a + sizeof(cache_entry_header_t));
    const char* key_b = (const char*)CACHE_PTR(cache, offset_b + sizeof(cache_entry_header_t));
    
    size_t min_len = ha->key_len < hb->key_len ? ha->key_len : hb->key_len;
    int cmp = memcmp(key_a, key_b, min_len);
    if (cmp != 0) return cmp;
    return (int)(ha->key_len - hb->key_len);
}

// 重建排序数组（全量 qsort，用于 batch insert 后）
void cache_sorted_rebuild(cache_t* cache) {
    if (!cache || cache->sorted.count <= 1) return;
    
    g_compare_cache = cache;
    qsort(cache->sorted.offsets, cache->sorted.count, sizeof(size_t), compare_entries_qsort);
    g_compare_cache = NULL;
}

// 释放排序数组内存
void cache_sorted_destroy(cache_t* cache) {
    if (!cache) return;
    if (cache->sorted.offsets) {
        free(cache->sorted.offsets);
        cache->sorted.offsets = NULL;
    }
    cache->sorted.count = 0;
    cache->sorted.capacity = 0;
}