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
    if (cache_hash_init(cache) < 0 || cache_sorted_init(cache) < 0 || cache_ns_init(cache) < 0) {
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
    
    // LRU 淘汰：如果内存不足，淘汰最老的非永久条目
    while (cache->memory_used + aligned_size > cache->memory_max) {
        // 找到 access_time 最老的非永久、未删除条目
        uint64_t oldest_time = UINT64_MAX;
        size_t oldest_offset = 0;
        char oldest_key[1024];
        size_t oldest_key_len = 0;
        size_t oldest_size = 0;
        
        for (size_t i = 0; i < cache->sorted.count; i++) {
            size_t offset = cache_sorted_get(cache, i);
            if (!offset) continue;
            
            cache_entry_header_t* h = (cache_entry_header_t*)CACHE_PTR(cache, offset);
            if (h->flags & CACHE_ENTRY_DELETED) continue;
            if (h->flags & CACHE_ENTRY_PERMANENT) continue;
            
            if (h->access_time < oldest_time) {
                oldest_time = h->access_time;
                oldest_offset = offset;
                oldest_key_len = h->key_len < sizeof(oldest_key) - 1 ? h->key_len : sizeof(oldest_key) - 1;
                memcpy(oldest_key, (char*)CACHE_PTR(cache, offset + sizeof(cache_entry_header_t)), oldest_key_len);
                oldest_key[oldest_key_len] = '\0';
                oldest_size = cache_entry_total_size(h);
            }
        }
        
        if (!oldest_offset) {
            return CACHE_ERR_NOMEM;  // 没有可淘汰的条目
        }
        
        // 直接淘汰（不调用 cache_del 避免递归和重复操作）
        cache_entry_header_t* h = (cache_entry_header_t*)CACHE_PTR(cache, oldest_offset);
        h->flags |= CACHE_ENTRY_DELETED;
        cache->entry_count--;
        cache->deleted_count++;
        cache->memory_used -= oldest_size;
        
        // 更新索引
        cache_hash_remove(cache, oldest_key, oldest_key_len);
        cache_sorted_remove(cache, oldest_key, oldest_key_len);
        cache_ns_remove(cache, oldest_key);
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
    cache_sorted_remove(cache, key, strlen(key));
    cache_ns_remove(cache, key);
    
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