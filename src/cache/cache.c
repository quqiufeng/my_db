#define _GNU_SOURCE
#include "cache_internal.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

// 从 pool 分配内存并返回 offset
static size_t cache_pool_alloc(cache_t* cache, size_t size) {
    void* ptr = pool_alloc(&cache->pool, size);
    if (!ptr) return 0;
    return (size_t)((char*)ptr - (char*)cache->pool.base);
}

// 通过 offset 获取 pool 中的指针
#define CACHE_PTR(cache, offset) ((void*)((char*)(cache)->pool.base + (offset)))

// 获取当前时间（毫秒）
uint64_t cache_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// 检查 entry 是否过期
int cache_entry_is_expired(cache_entry_t* entry, uint64_t now) {
    if (!entry || entry->expire_at == 0) return 0;
    return entry->expire_at < now;
}

// 计算 entry 总大小（含 header + key + '\0' + value + '\0'，对齐）
size_t cache_entry_total_size(cache_entry_header_t* header) {
    size_t size = sizeof(cache_entry_header_t) + header->key_len + 1 + header->value_len + 1;
    return (size + MYDB_ALIGN - 1) & ~(MYDB_ALIGN - 1);
}

// 初始化 cache 文件头（在 pool 头基础上写入扩展信息）
static void cache_header_init(cache_t* cache) {
    void* base = cache->pool.base;
    // pool_init 已经写了前 16 bytes: [magic:4][version:4][used:8]
    // 现在写入 cache 扩展信息（offset 16 开始）
    *(uint64_t*)((char*)base + 16) = 0;  // entry_count
    *(uint64_t*)((char*)base + 24) = 0;  // hash_offset
    *(uint64_t*)((char*)base + 32) = 0;  // sorted_offset
    *(uint64_t*)((char*)base + 40) = 0;  // reserved
}

// 验证 cache 文件头
static int cache_header_validate(cache_t* cache) {
    void* base = cache->pool.base;
    if (memcmp(base, CACHE_MAGIC, 4) != 0) return -1;
    uint32_t version = *(uint32_t*)((char*)base + 4);
    if (version != CACHE_VERSION) return -1;
    return 0;
}

// 保存 header 统计信息
static void cache_header_save(cache_t* cache) {
    void* base = cache->pool.base;
    *(uint64_t*)((char*)base + 8) = cache->pool.used;
    *(uint64_t*)((char*)base + 16) = cache->entry_count;
    *(uint64_t*)((char*)base + 24) = cache->hash.buckets_offset;
    *(uint64_t*)((char*)base + 32) = 0;  // sorted_offset（动态管理，不持久化）
}

// 解析现有 entry（从 pool offset 读取）
// Layout: [header][key(key_len+1)][value(value_len+1)]
static void cache_entry_parse(cache_t* cache, size_t offset, cache_entry_t* entry) {
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
    entry->offset = offset;
    entry->key_offset = offset + sizeof(cache_entry_header_t);
    entry->value_offset = entry->key_offset + header->key_len + 1;  // +1 for key's '\0'
    entry->key_len = header->key_len;
    entry->value_len = header->value_len;
    entry->expire_at = header->expire_at;
    entry->access_time = header->access_time;
    entry->flags = header->flags;
}

// Hot Cache 辅助函数
static uint64_t hash_key_fnv(const char* key, size_t key_len) {
    const uint8_t* bytes = (const uint8_t*)key;
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < key_len; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3;
    }
    return hash;
}

static void hot_cache_invalidate(cache_t* cache, const char* key, size_t key_len) {
    uint64_t h = hash_key_fnv(key, key_len);
    for (size_t i = 0; i < cache->hot_count; i++) {
        if (cache->hot_cache[i].key_hash == h) {
            // Swap with last and pop
            cache->hot_cache[i] = cache->hot_cache[cache->hot_count - 1];
            cache->hot_count--;
            return;
        }
    }
}

static void hot_cache_update(cache_t* cache, const char* key, size_t key_len, size_t entry_offset) {
    uint64_t h = hash_key_fnv(key, key_len);
    uint64_t now = cache_now_ms();
    
    // 查找是否已存在
    for (size_t i = 0; i < cache->hot_count; i++) {
        if (cache->hot_cache[i].key_hash == h) {
            cache->hot_cache[i].entry_offset = entry_offset;
            cache->hot_cache[i].access_time = now;
            return;
        }
    }
    
    // 插入新条目（如果未满）或替换最老的
    size_t idx = cache->hot_count;
    if (cache->hot_count < CACHE_HOT_SIZE) {
        cache->hot_count++;
    } else {
        // 找到最老的替换
        uint64_t oldest = now;
        for (size_t i = 0; i < CACHE_HOT_SIZE; i++) {
            if (cache->hot_cache[i].access_time < oldest) {
                oldest = cache->hot_cache[i].access_time;
                idx = i;
            }
        }
    }
    
    cache->hot_cache[idx].key_hash = h;
    cache->hot_cache[idx].entry_offset = entry_offset;
    cache->hot_cache[idx].access_time = now;
}

static size_t hot_cache_lookup(cache_t* cache, const char* key, size_t key_len) {
    uint64_t h = hash_key_fnv(key, key_len);
    uint64_t now = cache_now_ms();
    
    for (size_t i = 0; i < cache->hot_count; i++) {
        if (cache->hot_cache[i].key_hash == h) {
            cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, cache->hot_cache[i].entry_offset);
            if (!(header->flags & CACHE_ENTRY_DELETED) &&
                (header->expire_at == 0 || header->expire_at >= now)) {
                cache->hot_cache[i].access_time = now;
                return cache->hot_cache[i].entry_offset;
            }
        }
    }
    return 0;
}

// 使用 Hash 索引查找 key（优先查 hot cache）
static cache_entry_t* cache_find_entry(cache_t* cache, const char* key) {
    size_t key_len = strlen(key);
    uint64_t now = cache_now_ms();
    
    // 1. 先查 hot cache
    size_t offset = hot_cache_lookup(cache, key, key_len);
    
    // 2. 再查 Hash 索引
    if (!offset) {
        offset = cache_hash_lookup(cache, key, key_len);
        if (!offset) return NULL;
    }
    
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
    
    // 检查是否已删除
    if (header->flags & CACHE_ENTRY_DELETED) return NULL;
    
    // 检查是否过期
    if (header->expire_at > 0 && header->expire_at < now) return NULL;
    
    cache_entry_t* entry = malloc(sizeof(cache_entry_t));
    cache_entry_parse(cache, offset, entry);
    return entry;
}

#include <sys/stat.h>

static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1; // 存在但不是目录
    }
    return mkdir(path, 0755);
}

cache_t* cache_open(const char* db_dir, size_t max_memory) {
    if (!db_dir) return NULL;
    
    // 确保目录存在
    if (ensure_dir(db_dir) < 0) return NULL;
    
    cache_t* cache = (cache_t*)calloc(1, sizeof(cache_t));
    if (!cache) return NULL;
    
    strncpy(cache->db_dir, db_dir, sizeof(cache->db_dir) - 1);
    cache->memory_max = max_memory > 0 ? max_memory : CACHE_MAX_MEMORY_DEFAULT;
    
    // 构建 cache.bin 路径
    char path[512];
    snprintf(path, sizeof(path), "%s/cache.bin", db_dir);
    
    // 打开/创建 pool（使用自定义 magic）
    size_t pool_used = 0;
    if (pool_init_with_header(&cache->pool, path, 1024 * 1024, 
                               CACHE_MAGIC, CACHE_VERSION, &pool_used) < 0) {
        free(cache);
        return NULL;
    }
    
    // 检查是否是新文件（pool 头大小为 16）
    bool is_new = (pool_used == 16);
    
    if (is_new) {
        // 初始化文件头（在 pool 头基础上扩展）
        cache_header_init(cache);
        cache->pool.used = CACHE_HEADER_SIZE;
        // 更新 pool 文件头中的 used
        *(uint64_t*)((char*)cache->pool.base + 8) = CACHE_HEADER_SIZE;
    } else {
        // 验证文件头
        if (cache_header_validate(cache) < 0) {
            // 文件头损坏：尝试恢复
            fprintf(stderr, "[CACHE] Header corrupted, attempting recovery...\n");
            cache_header_init(cache);
            cache->pool.used = CACHE_HEADER_SIZE;
            *(uint64_t*)((char*)cache->pool.base + 8) = CACHE_HEADER_SIZE;
            // 标记需要扫描恢复
            cache->entry_count = 0;
            cache->hash.buckets_offset = 0;
            cache->hash.bucket_count = 0;
            cache->hash.size = 0;
        } else {
            // 读取统计信息
            void* base = cache->pool.base;
            cache->entry_count = *(uint64_t*)((char*)base + 16);
            cache->hash.buckets_offset = *(uint64_t*)((char*)base + 24);
            cache->hash.bucket_count = 0;
            cache->hash.size = 0;
        }
    }
    
    // 初始化索引
    if (cache_hash_init(cache) < 0 || cache_sorted_init(cache) < 0 || 
        cache_ns_init(cache) < 0 || cache_tag_index_init(cache) < 0) {
        pool_close(&cache->pool);
        free(cache);
        return NULL;
    }

    if (!is_new) {
        // 已有文件：重建 hash 索引
        // 扫描所有 entry（通过 key_len 识别有效的 entry）
        size_t offset = CACHE_HEADER_SIZE;
        size_t valid_count = 0;
        while (offset + sizeof(cache_entry_header_t) <= cache->pool.used) {
            cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
            
            // 检查是否是有效的 entry header
            if (header->key_len == 0 || header->key_len > CACHE_MAX_KEY_LEN ||
                header->value_len > CACHE_MAX_VALUE_LEN) {
                // 不是 entry，跳过对齐大小
                offset += MYDB_ALIGN;
                continue;
            }
            
            size_t total_size = cache_entry_total_size(header);
            if (offset + total_size > cache->pool.used) break;
            
            if (!(header->flags & CACHE_ENTRY_DELETED)) {
                char* key = (char*)CACHE_PTR(cache, offset + sizeof(cache_entry_header_t));
                cache_hash_insert(cache, offset, key, header->key_len);
                cache_sorted_insert(cache, offset);
                cache_ns_add(cache, key, offset);
                valid_count++;
            }
            
            offset += total_size;
        }
        cache->entry_count = valid_count;
    }
    
    return cache;
}

void cache_close(cache_t* cache) {
    if (!cache) return;
    
    // 保存 header
    cache_header_save(cache);
    
    // 关闭 pool
    pool_sync(&cache->pool);
    pool_close(&cache->pool);
    
    // 释放排序数组内存
    cache_sorted_destroy(cache);
    
    // 释放 namespace 索引
    cache_ns_destroy(cache);
    
    // 释放 tag 索引
    cache_tag_index_destroy(cache);
    
    free(cache);
}

int cache_sync(cache_t* cache) {
    if (!cache) return CACHE_ERR_INVAL;
    cache_header_save(cache);
    return pool_sync(&cache->pool) < 0 ? CACHE_ERR_IO : CACHE_OK;
}

int cache_set(cache_t* cache, const char* key, const char* value, uint64_t ttl_ms) {
    if (!cache || !key || !value) return CACHE_ERR_INVAL;
    
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    
    if (key_len == 0 || key_len >= CACHE_MAX_KEY_LEN) return CACHE_ERR_INVAL;
    if (value_len >= CACHE_MAX_VALUE_LEN) return CACHE_ERR_INVAL;
    
    // 检查内存限制（+2: key 和 value 各一个 '\0'，方便 C 字符串处理）
    size_t entry_size = sizeof(cache_entry_header_t) + key_len + 1 + value_len + 1;
    size_t aligned_size = (entry_size + MYDB_ALIGN - 1) & ~(MYDB_ALIGN - 1);
    
    // LRU 淘汰：如果内存不足，批量淘汰最老的非永久条目
    if (cache->memory_used + aligned_size > cache->memory_max) {
        // 收集所有可淘汰的条目（非永久、未删除）
        typedef struct {
            uint64_t access_time;
            size_t offset;
            size_t key_len;
            size_t total_size;
        } evict_candidate_t;
        
        // 一次性扫描所有条目
        evict_candidate_t* candidates = malloc(sizeof(evict_candidate_t) * cache->entry_count);
        if (!candidates) return CACHE_ERR_NOMEM;
        
        size_t candidate_count = 0;
        size_t offset = CACHE_HEADER_SIZE;
        while (offset + sizeof(cache_entry_header_t) <= cache->pool.used) {
            cache_entry_header_t* h = (cache_entry_header_t*)CACHE_PTR(cache, offset);
            
            // 检查是否是有效的 entry header
            if (h->key_len == 0 || h->key_len > CACHE_MAX_KEY_LEN ||
                h->value_len > CACHE_MAX_VALUE_LEN) {
                offset += MYDB_ALIGN;
                continue;
            }
            
            size_t total_size = cache_entry_total_size(h);
            if (offset + total_size > cache->pool.used) break;
            
            if (!(h->flags & CACHE_ENTRY_DELETED) && !(h->flags & CACHE_ENTRY_PERMANENT)) {
                candidates[candidate_count].access_time = h->access_time;
                candidates[candidate_count].offset = offset;
                candidates[candidate_count].key_len = h->key_len;
                candidates[candidate_count].total_size = total_size;
                candidate_count++;
            }
            
            offset += total_size;
        }
        
        if (candidate_count == 0) {
            free(candidates);
            return CACHE_ERR_NOMEM;  // 没有可淘汰的条目
        }
        
        // 按 access_time 排序（升序，最老的在前）— 使用 qsort
        // 简单的冒泡/插入排序，因为 candidate_count 不大（最多淘汰几百个）
        for (size_t i = 0; i < candidate_count - 1; i++) {
            for (size_t j = i + 1; j < candidate_count; j++) {
                if (candidates[j].access_time < candidates[i].access_time) {
                    evict_candidate_t tmp = candidates[i];
                    candidates[i] = candidates[j];
                    candidates[j] = tmp;
                }
            }
        }
        
        // 批量淘汰，直到有足够空间（+ 额外预留 10% 避免频繁淘汰）
        size_t needed = aligned_size + cache->memory_max / 10;
        size_t evicted = 0;
        
        for (size_t i = 0; i < candidate_count && cache->memory_used + aligned_size > cache->memory_max - evicted; i++) {
            evict_candidate_t* c = &candidates[i];
            cache_entry_header_t* h = (cache_entry_header_t*)CACHE_PTR(cache, c->offset);
            
            if (h->flags & CACHE_ENTRY_DELETED) continue;  // 可能已被之前的淘汰标记
            
            // 读取 key
            char* key_ptr = (char*)CACHE_PTR(cache, c->offset + sizeof(cache_entry_header_t));
            
            // 标记删除
            h->flags |= CACHE_ENTRY_DELETED;
            cache->entry_count--;
            cache->deleted_count++;
            cache->memory_used -= c->total_size;
            evicted += c->total_size;
            
            // 更新索引
            cache_hash_remove(cache, key_ptr, c->key_len);
            // sorted_remove 会触发 ensure_sorted，但我们已经在批量处理了
            // 直接标记 dirty，稍后统一重建
            cache_ns_remove(cache, key_ptr);
        }
        
        free(candidates);
        
        // 如果有淘汰，标记 sorted array 需要重建（因为批量删除了多个条目）
        if (evicted > 0) {
            cache->sorted.dirty = 1;
            // 重新构建 sorted array（移除已删除的条目）
            size_t new_count = 0;
            for (size_t i = 0; i < cache->sorted.count; i++) {
                size_t entry_off = cache_sorted_get(cache, i);
                if (!entry_off) continue;
                cache_entry_header_t* h = (cache_entry_header_t*)CACHE_PTR(cache, entry_off);
                if (!(h->flags & CACHE_ENTRY_DELETED)) {
                    cache->sorted.offsets[new_count++] = entry_off;
                }
            }
            cache->sorted.count = new_count;
            cache->sorted.dirty = 1;  // 需要重新排序
        }
        
        // 检查是否仍有足够空间
        if (cache->memory_used + aligned_size > cache->memory_max) {
            return CACHE_ERR_NOMEM;
        }
    }
    
    // 检查是否是更新已有 key
    size_t old_offset = cache_hash_lookup(cache, key, key_len);
    if (old_offset) {
        // 更新已有 entry：标记旧条目为删除
        cache_entry_header_t* old_header = (cache_entry_header_t*)CACHE_PTR(cache, old_offset);
        if (!(old_header->flags & CACHE_ENTRY_DELETED)) {
            old_header->flags |= CACHE_ENTRY_DELETED;
            cache->entry_count--;
            cache->deleted_count++;
            cache->memory_used -= cache_entry_total_size(old_header);
            
            // 从索引中移除旧条目
            cache_hash_remove(cache, key, key_len);
            cache_sorted_remove(cache, key, key_len);
            cache_ns_remove(cache, key);
            
            // 从 tag 索引中移除
            const char* old_value = (const char*)CACHE_PTR(cache, old_offset + sizeof(cache_entry_header_t) + old_header->key_len + 1);
            cache_tag_index_remove(cache, old_offset, old_value, old_header->value_len);
            
            // 从 hot cache 中移除
            hot_cache_invalidate(cache, key, key_len);
        }
    }
    
    // 分配 entry 内存
    size_t offset = cache_pool_alloc(cache, aligned_size);
    if (!offset) return CACHE_ERR_NOMEM;
    
    // 写入 header
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
    header->key_len = key_len;
    header->value_len = value_len;
    header->expire_at = ttl_ms > 0 ? cache_now_ms() + ttl_ms : 0;
    header->access_time = cache_now_ms();
    header->flags = ttl_ms == 0 ? CACHE_ENTRY_PERMANENT : 0;
    header->reserved = 0;
    
    // 写入 key 和 value
    char* key_ptr = (char*)CACHE_PTR(cache, offset + sizeof(cache_entry_header_t));
    memcpy(key_ptr, key, key_len);
    key_ptr[key_len] = '\0';  // null-terminate for C string compatibility
    
    char* value_ptr = key_ptr + key_len + 1;
    memcpy(value_ptr, value, value_len);
    value_ptr[value_len] = '\0';  // null-terminate for C string compatibility
    
    // 更新统计
    cache->entry_count++;
    cache->memory_used += aligned_size;
    
    // 更新索引
    cache_hash_insert(cache, offset, key, key_len);
    cache_sorted_insert(cache, offset);
    cache_ns_add(cache, key, offset);
    cache_tag_index_add(cache, offset, value, value_len);
    
    return CACHE_OK;
}

// 批量设置（性能优化：先批量写入 pool，最后统一排序数组 qsort）
int cache_batch_set(cache_t* cache, const cache_batch_item_t* items, size_t count) {
    if (!cache || !items || count == 0) return CACHE_ERR_INVAL;
    
    // 预分配 offset 数组
    size_t* offsets = malloc(sizeof(size_t) * count);
    if (!offsets) return CACHE_ERR_NOMEM;
    
    uint64_t now = cache_now_ms();
    size_t total_aligned = 0;
    
    // 第一遍：计算总大小并检查内存
    for (size_t i = 0; i < count; i++) {
        size_t key_len = strlen(items[i].key);
        size_t value_len = strlen(items[i].value);
        size_t entry_size = sizeof(cache_entry_header_t) + key_len + 1 + value_len + 1;
        size_t aligned_size = (entry_size + MYDB_ALIGN - 1) & ~(MYDB_ALIGN - 1);
        total_aligned += aligned_size;
    }
    
    // 检查内存限制（简单检查，不考虑 LRU 淘汰）
    if (cache->memory_used + total_aligned > cache->memory_max) {
        free(offsets);
        return CACHE_ERR_NOMEM;
    }
    
    // 第二遍：批量写入 pool
    for (size_t i = 0; i < count; i++) {
        const char* key = items[i].key;
        const char* value = items[i].value;
        size_t key_len = strlen(key);
        size_t value_len = strlen(value);
        uint64_t ttl_ms = items[i].ttl_ms;
        
        size_t entry_size = sizeof(cache_entry_header_t) + key_len + 1 + value_len + 1;
        size_t aligned_size = (entry_size + MYDB_ALIGN - 1) & ~(MYDB_ALIGN - 1);
        
        size_t offset = cache_pool_alloc(cache, aligned_size);
        if (!offset) {
            free(offsets);
            return CACHE_ERR_NOMEM;
        }
        
        // 写入 header
        cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
        header->key_len = key_len;
        header->value_len = value_len;
        header->expire_at = ttl_ms > 0 ? now + ttl_ms : 0;
        header->access_time = now;
        header->flags = ttl_ms == 0 ? CACHE_ENTRY_PERMANENT : 0;
        header->reserved = 0;
        
        // 写入 key 和 value
        char* key_ptr = (char*)CACHE_PTR(cache, offset + sizeof(cache_entry_header_t));
        memcpy(key_ptr, key, key_len);
        key_ptr[key_len] = '\0';
        
        char* value_ptr = key_ptr + key_len + 1;
        memcpy(value_ptr, value, value_len);
        value_ptr[value_len] = '\0';
        
        offsets[i] = offset;
        cache->entry_count++;
        cache->memory_used += aligned_size;
    }
    
    // 第三遍：批量更新 hash 索引
    for (size_t i = 0; i < count; i++) {
        const char* key = items[i].key;
        size_t key_len = strlen(key);
        cache_hash_insert(cache, offsets[i], key, key_len);
    }
    
    // 第四遍：批量更新 namespace 索引
    for (size_t i = 0; i < count; i++) {
        cache_ns_add(cache, items[i].key, offsets[i]);
    }
    
    // 第五遍：排序数组优化——直接追加到末尾，最后统一 qsort
    cache_sorted_array_t* sorted = &cache->sorted;
    size_t new_count = sorted->count + count;
    
    if (new_count > sorted->capacity) {
        size_t new_cap = sorted->capacity * 2;
        if (new_cap < new_count) new_cap = new_count;
        if (new_cap < 16) new_cap = 16;
        
        size_t* new_offsets = realloc(sorted->offsets, sizeof(size_t) * new_cap);
        if (!new_offsets) {
            free(offsets);
            return CACHE_ERR_NOMEM;
        }
        sorted->offsets = new_offsets;
        sorted->capacity = new_cap;
    }
    
    // 复制新 offsets 到数组末尾
    memcpy(&sorted->offsets[sorted->count], offsets, sizeof(size_t) * count);
    sorted->count = new_count;
    free(offsets);
    
    // 统一排序（使用 cache_sorted_rebuild，O(N log N) 一次排序）
    cache_sorted_rebuild(cache);
    
    return CACHE_OK;
}

const char* cache_get(cache_t* cache, const char* key) {
    if (!cache || !key) return NULL;
    
    size_t key_len = strlen(key);
    
    // 1. 查 hot cache
    size_t offset = hot_cache_lookup(cache, key, key_len);
    
    // 2. 查 Hash 索引
    if (!offset) {
        offset = cache_hash_lookup(cache, key, key_len);
        if (!offset) return NULL;
    }
    
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
    
    // 检查是否已删除
    if (header->flags & CACHE_ENTRY_DELETED) return NULL;
    
    // 检查是否过期
    uint64_t now = cache_now_ms();
    if (header->expire_at > 0 && header->expire_at < now) return NULL;
    
    // 更新 access_time
    header->access_time = now;
    
    // 更新 hot cache
    hot_cache_update(cache, key, key_len, offset);
    
    return (const char*)CACHE_PTR(cache, offset + sizeof(cache_entry_header_t) + header->key_len + 1);
}

int cache_del(cache_t* cache, const char* key) {
    if (!cache || !key) return CACHE_ERR_INVAL;
    
    cache_entry_t* entry = cache_find_entry(cache, key);
    if (!entry) return CACHE_ERR_NOENT;
    
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, entry->offset);
    
    // 从 tag 索引中移除（在标记删除前）
    const char* value = (const char*)CACHE_PTR(cache, entry->offset + sizeof(cache_entry_header_t) + header->key_len + 1);
    cache_tag_index_remove(cache, entry->offset, value, header->value_len);
    
    // 标记删除
    header->flags |= CACHE_ENTRY_DELETED;
    
    cache->entry_count--;
    cache->deleted_count++;
    
    // 更新索引
    size_t key_len = strlen(key);
    cache_hash_remove(cache, key, key_len);
    cache_sorted_remove(cache, key, key_len);
    cache_ns_remove(cache, key);
    
    // 从 hot cache 中移除
    hot_cache_invalidate(cache, key, key_len);
    
    free(entry);
    return CACHE_OK;
}

int cache_exists(cache_t* cache, const char* key) {
    if (!cache || !key) return 0;
    return cache_get(cache, key) != NULL;
}

size_t cache_count(cache_t* cache) {
    if (!cache) return 0;
    return cache->entry_count;
}

size_t cache_memory_used(cache_t* cache) {
    if (!cache) return 0;
    return cache->memory_used;
}

size_t cache_memory_max(cache_t* cache) {
    if (!cache) return 0;
    return cache->memory_max;
}

size_t cache_compact(cache_t* cache) {
    if (!cache) return 0;
    // TODO: 实现 compact - 重建 pool，移除已删除的 entry
    return cache->deleted_count;
}

size_t cache_purge_expired(cache_t* cache) {
    if (!cache) return 0;
    
    size_t count = 0;
    uint64_t now = cache_now_ms();
    size_t offset = CACHE_HEADER_SIZE;
    
    while (offset < cache->pool.used) {
        cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
        if (header->key_len == 0) break;
        
        if (!(header->flags & CACHE_ENTRY_DELETED) && header->expire_at > 0) {
            if (header->expire_at < now) {
                header->flags |= CACHE_ENTRY_DELETED;
                cache->entry_count--;
                cache->deleted_count++;
                count++;
            }
        }
        
        offset += cache_entry_total_size(header);
    }
    
    return count;
}

int cache_check(const char* db_dir) {
    if (!db_dir) return CACHE_ERR_INVAL;
    
    char path[512];
    snprintf(path, sizeof(path), "%s/cache.bin", db_dir);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return CACHE_ERR_IO;
    
    // 读取并验证文件头
    char header_buf[CACHE_HEADER_SIZE];
    ssize_t n = read(fd, header_buf, CACHE_HEADER_SIZE);
    if (n != CACHE_HEADER_SIZE) {
        close(fd);
        return CACHE_ERR_CORRUPTED;
    }
    
    if (memcmp(header_buf, CACHE_MAGIC, 4) != 0) {
        close(fd);
        return CACHE_ERR_CORRUPTED;
    }
    
    uint32_t version = *(uint32_t*)(header_buf + 4);
    if (version != CACHE_VERSION) {
        close(fd);
        return CACHE_ERR_CORRUPTED;
    }
    
    uint64_t file_used = *(uint64_t*)(header_buf + 8);
    uint64_t entry_count = *(uint64_t*)(header_buf + 16);
    
    // 获取文件大小
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        close(fd);
        return CACHE_ERR_IO;
    }
    
    // 映射文件进行完整检查
    void* map = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    
    if (map == MAP_FAILED) {
        return CACHE_ERR_IO;
    }
    
    // 扫描所有 entry（跳过 hash bucket 等非 entry 数据）
    size_t offset = CACHE_HEADER_SIZE;
    size_t valid_count = 0;
    size_t deleted_count = 0;
    size_t expired_count = 0;
    size_t skipped_count = 0;
    uint64_t now = cache_now_ms();
    
    while (offset + sizeof(cache_entry_header_t) <= (size_t)file_used) {
        cache_entry_header_t* h = (cache_entry_header_t*)((char*)map + offset);
        
        // 检查是否是有效的 entry header（同 cache_open 重建逻辑）
        if (h->key_len == 0 || h->key_len > CACHE_MAX_KEY_LEN ||
            h->value_len > CACHE_MAX_VALUE_LEN) {
            // 不是 entry（可能是 hash bucket 或其他内部数据），跳过
            offset += MYDB_ALIGN;
            skipped_count++;
            continue;
        }
        
        // 计算 entry 总大小
        size_t total_size = cache_entry_total_size(h);
        
        // 检查是否越界
        if (offset + total_size > (size_t)file_used) {
            fprintf(stderr, "  Warning: entry at offset %zu exceeds file size\n", offset);
            break;
        }
        
        // 检查对齐
        if (total_size % MYDB_ALIGN != 0) {
            fprintf(stderr, "  Warning: entry at offset %zu not aligned\n", offset);
            offset += MYDB_ALIGN;
            continue;
        }
        
        // 统计
        if (h->flags & CACHE_ENTRY_DELETED) {
            deleted_count++;
        } else if (h->expire_at > 0 && h->expire_at < now) {
            expired_count++;
        } else {
            valid_count++;
        }
        
        offset += total_size;
    }
    
    munmap(map, file_size);
    
    // 输出检查结果（到 stderr，因为这是一个诊断工具）
    fprintf(stderr, "Cache check: %s\n", path);
    fprintf(stderr, "  File size: %ld bytes\n", (long)file_size);
    fprintf(stderr, "  Header used: %lu bytes\n", (unsigned long)file_used);
    fprintf(stderr, "  Valid entries: %zu\n", valid_count);
    fprintf(stderr, "  Deleted entries: %zu\n", deleted_count);
    fprintf(stderr, "  Expired entries: %zu\n", expired_count);
    fprintf(stderr, "  Skipped (internal): %zu\n", skipped_count);
    
    // 验证 entry_count 是否匹配
    if (valid_count != entry_count) {
        fprintf(stderr, "  Warning: entry_count mismatch (header=%lu, actual=%zu)\n",
                (unsigned long)entry_count, valid_count);
    }
    
    return CACHE_OK;
}

// ====== Namespace 便捷操作 ======

int cache_set_ns(cache_t* cache, const char* ns, const char* key, 
                 const char* value, uint64_t ttl_ms) {
    if (!cache || !ns || !key || !value) return CACHE_ERR_INVAL;
    
    // 构建完整 key: ns + "/" + key
    size_t ns_len = strlen(ns);
    size_t key_len = strlen(key);
    
    // 移除 ns 末尾的 '/'（如果有）
    while (ns_len > 0 && ns[ns_len - 1] == '/') ns_len--;
    
    // 分配完整 key 的内存
    char* full_key = malloc(ns_len + 1 + key_len + 1);
    if (!full_key) return CACHE_ERR_NOMEM;
    
    if (ns_len > 0) {
        memcpy(full_key, ns, ns_len);
        full_key[ns_len] = '/';
        memcpy(full_key + ns_len + 1, key, key_len);
        full_key[ns_len + 1 + key_len] = '\0';
    } else {
        memcpy(full_key, key, key_len);
        full_key[key_len] = '\0';
    }
    
    int ret = cache_set(cache, full_key, value, ttl_ms);
    free(full_key);
    return ret;
}

const char* cache_get_ns(cache_t* cache, const char* ns, const char* key) {
    if (!cache || !ns || !key) return NULL;
    
    size_t ns_len = strlen(ns);
    size_t key_len = strlen(key);
    
    while (ns_len > 0 && ns[ns_len - 1] == '/') ns_len--;
    
    char* full_key = malloc(ns_len + 1 + key_len + 1);
    if (!full_key) return NULL;
    
    if (ns_len > 0) {
        memcpy(full_key, ns, ns_len);
        full_key[ns_len] = '/';
        memcpy(full_key + ns_len + 1, key, key_len);
        full_key[ns_len + 1 + key_len] = '\0';
    } else {
        memcpy(full_key, key, key_len);
        full_key[key_len] = '\0';
    }
    
    const char* value = cache_get(cache, full_key);
    free(full_key);
    return value;
}

int cache_del_ns(cache_t* cache, const char* ns, const char* key) {
    if (!cache || !ns || !key) return CACHE_ERR_INVAL;
    
    size_t ns_len = strlen(ns);
    size_t key_len = strlen(key);
    
    while (ns_len > 0 && ns[ns_len - 1] == '/') ns_len--;
    
    char* full_key = malloc(ns_len + 1 + key_len + 1);
    if (!full_key) return CACHE_ERR_NOMEM;
    
    if (ns_len > 0) {
        memcpy(full_key, ns, ns_len);
        full_key[ns_len] = '/';
        memcpy(full_key + ns_len + 1, key, key_len);
        full_key[ns_len + 1 + key_len] = '\0';
    } else {
        memcpy(full_key, key, key_len);
        full_key[key_len] = '\0';
    }
    
    int ret = cache_del(cache, full_key);
    free(full_key);
    return ret;
}

int cache_expire(cache_t* cache, const char* key) {
    if (!cache || !key) return CACHE_ERR_INVAL;
    
    cache_entry_t* entry = cache_find_entry(cache, key);
    if (!entry) return CACHE_ERR_NOENT;
    
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, entry->offset);
    header->expire_at = 1;  // 设置为一个已经过去的时间点
    header->flags |= CACHE_ENTRY_DELETED;
    
    cache->entry_count--;
    cache->deleted_count++;
    
    free(entry);
    return CACHE_OK;
}

int cache_touch(cache_t* cache, const char* key) {
    if (!cache || !key) return CACHE_ERR_INVAL;
    
    cache_entry_t* entry = cache_find_entry(cache, key);
    if (!entry) return CACHE_ERR_NOENT;
    
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, entry->offset);
    header->access_time = cache_now_ms();
    
    free(entry);
    return CACHE_OK;
}