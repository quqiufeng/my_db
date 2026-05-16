#define _GNU_SOURCE
#include "cache_internal.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

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

// 计算 entry 总大小（含 header + key + value，对齐）
size_t cache_entry_total_size(cache_entry_header_t* header) {
    size_t size = sizeof(cache_entry_header_t) + header->key_len + header->value_len;
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
static void cache_entry_parse(cache_t* cache, size_t offset, cache_entry_t* entry) {
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, offset);
    entry->offset = offset;
    entry->key_offset = offset + sizeof(cache_entry_header_t);
    entry->value_offset = entry->key_offset + header->key_len;
    entry->key_len = header->key_len;
    entry->value_len = header->value_len;
    entry->expire_at = header->expire_at;
    entry->access_time = header->access_time;
    entry->flags = header->flags;
}

// 使用 Hash 索引查找 key
static cache_entry_t* cache_find_entry(cache_t* cache, const char* key) {
    size_t key_len = strlen(key);
    uint64_t now = cache_now_ms();
    
    // 使用 Hash 索引查找
    size_t offset = cache_hash_lookup(cache, key, key_len);
    if (!offset) return NULL;
    
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
            pool_close(&cache->pool);
            free(cache);
            return NULL;
        }
        
        // 读取统计信息
        void* base = cache->pool.base;
        cache->entry_count = *(uint64_t*)((char*)base + 16);
        cache->hash.buckets_offset = *(uint64_t*)((char*)base + 24);
        cache->hash.bucket_count = 0;
        cache->hash.size = 0;
    }
    
    // 初始化索引
    if (cache_hash_init(cache) < 0) {
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
    
    // 释放索引内存（后续实现）
    // cache_hash_destroy(cache);
    // cache_sorted_destroy(cache);
    // cache_ns_destroy(cache);
    
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
    
    // 检查内存限制
    size_t entry_size = sizeof(cache_entry_header_t) + key_len + value_len;
    size_t aligned_size = (entry_size + MYDB_ALIGN - 1) & ~(MYDB_ALIGN - 1);
    if (cache->memory_used + aligned_size > cache->memory_max) {
        return CACHE_ERR_NOMEM;
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
    
    char* value_ptr = key_ptr + key_len;
    memcpy(value_ptr, value, value_len);
    
    // 更新统计
    cache->entry_count++;
    cache->memory_used += aligned_size;
    
    // 更新索引
    cache_hash_insert(cache, offset, key, key_len);
    // cache_sorted_insert(cache, offset);
    // cache_ns_add(cache, key, offset);
    
    return CACHE_OK;
}

const char* cache_get(cache_t* cache, const char* key) {
    if (!cache || !key) return NULL;
    
    cache_entry_t* entry = cache_find_entry(cache, key);
    if (!entry) return NULL;
    
    // 更新 access_time
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, entry->offset);
    header->access_time = cache_now_ms();
    
    const char* value = (const char*)CACHE_PTR(cache, entry->value_offset);
    free(entry);
    return value;
}

int cache_del(cache_t* cache, const char* key) {
    if (!cache || !key) return CACHE_ERR_INVAL;
    
    cache_entry_t* entry = cache_find_entry(cache, key);
    if (!entry) return CACHE_ERR_NOENT;
    
    // 标记删除
    cache_entry_header_t* header = (cache_entry_header_t*)CACHE_PTR(cache, entry->offset);
    header->flags |= CACHE_ENTRY_DELETED;
    
    cache->entry_count--;
    cache->deleted_count++;
    
    // 更新索引
    cache_hash_remove(cache, key, strlen(key));
    // cache_sorted_remove(cache, key, strlen(key));
    // cache_ns_remove(cache, key);
    
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
    
    char magic[4];
    if (read(fd, magic, 4) != 4 || memcmp(magic, CACHE_MAGIC, 4) != 0) {
        close(fd);
        return CACHE_ERR_CORRUPTED;
    }
    
    uint32_t version;
    if (read(fd, &version, 4) != 4 || version != CACHE_VERSION) {
        close(fd);
        return CACHE_ERR_CORRUPTED;
    }
    
    close(fd);
    return CACHE_OK;
}