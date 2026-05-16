#ifndef CACHE_INTERNAL_H
#define CACHE_INTERNAL_H

#include "cache.h"
#include "mydb_internal.h"  // 复用 db_pool_t, error handling 等
#include <stdbool.h>
#include <regex.h>

// ====== 文件格式常量 ======
#define CACHE_MAGIC     "MYCA"   // Magic: "MYCA"
#define CACHE_VERSION   1
#define CACHE_HEADER_SIZE 48     // 对齐到 8

// ====== Entry 标志位 ======
#define CACHE_ENTRY_DELETED     0x01  // 逻辑删除
#define CACHE_ENTRY_PERMANENT   0x02  // 永久不过期

// ====== Entry Header（变长 entry 的头部）======
typedef struct __attribute__((packed)) {
    uint32_t key_len;           // key 长度
    uint32_t value_len;         // value 长度
    uint64_t expire_at;         // 过期时间（毫秒时间戳，0=永久）
    uint64_t access_time;       // 最后访问时间（毫秒时间戳）
    uint16_t flags;             // 标志位
    uint16_t reserved;          // 保留（对齐到 8）
} cache_entry_header_t;

// 总大小：24 bytes（对齐到 8）
_Static_assert(sizeof(cache_entry_header_t) == 24, "Entry header must be 24 bytes");

// ====== Entry（内存中的表示）======
typedef struct {
    size_t offset;              // 在 pool 中的偏移
    size_t key_offset;          // key 在 pool 中的偏移
    size_t value_offset;        // value 在 pool 中的偏移
    uint32_t key_len;
    uint32_t value_len;
    uint64_t expire_at;
    uint64_t access_time;
    uint16_t flags;
} cache_entry_t;

// ====== Hash 索引（借鉴 code_bin hashmap）======
// 使用 my_db 风格的 pool offset，不存指针

typedef struct {
    size_t entry_offset;        // 指向 cache_entry_t 的 offset
    size_t next_offset;         // 冲突链下一个 bucket 的 offset（0=无）
} cache_hash_bucket_t;

typedef struct {
    size_t bucket_count;        // bucket 数量
    size_t size;                // 当前元素数量
    size_t buckets_offset;      // bucket 数组在 pool 中的 offset
} cache_hash_index_t;

// ====== 排序数组（借鉴 code_bin sorted array）======
typedef struct {
    size_t* offsets;            // entry offset 数组（按 key 字典序排列）
    size_t count;               // 当前数量
    size_t capacity;            // 数组容量
} cache_sorted_array_t;

// ====== Namespace 索引 ======
typedef struct cache_ns_node {
    char* path;                 // namespace 路径（如 "/coding/cpp"）
    size_t* entry_offsets;      // 属于该 namespace 的 entry offsets
    size_t entry_count;
    size_t entry_capacity;
    struct cache_ns_node** children;  // 子 namespace
    size_t child_count;
    size_t child_capacity;
} cache_ns_node_t;

// ====== Cache 实例 ======
struct cache {
    db_pool_t pool;             // mmap pool（复用 my_db）
    
    // 索引
    cache_hash_index_t hash;    // Hash 索引：key → entry_offset
    cache_sorted_array_t sorted; // 排序数组：按 key 字典序排列
    cache_ns_node_t* ns_root;   // Namespace 树
    
    // 统计
    size_t entry_count;         // 总条目数（不含已删除）
    size_t deleted_count;       // 已删除条目数
    size_t memory_used;         // 已用内存（估算）
    size_t memory_max;          // 最大内存限制
    
    // 状态
    char db_dir[256];           // 数据库目录
};

// ====== 内部函数（其他模块使用）======

// Hash 索引操作
int cache_hash_init(cache_t* cache);
int cache_hash_insert(cache_t* cache, size_t entry_offset, const char* key, size_t key_len);
int cache_hash_remove(cache_t* cache, const char* key, size_t key_len);
size_t cache_hash_lookup(cache_t* cache, const char* key, size_t key_len);

// 排序数组操作
int cache_sorted_init(cache_t* cache);
int cache_sorted_insert(cache_t* cache, size_t entry_offset);
int cache_sorted_remove(cache_t* cache, const char* key, size_t key_len);
size_t cache_sorted_find_lower_bound(cache_t* cache, const char* key, size_t key_len);
size_t cache_sorted_find_upper_bound(cache_t* cache, const char* key, size_t key_len);

// Namespace 操作
int cache_ns_init(cache_t* cache);
int cache_ns_add(cache_t* cache, const char* key, size_t entry_offset);
int cache_ns_remove(cache_t* cache, const char* key);
cache_ns_node_t* cache_ns_find(cache_t* cache, const char* ns_path);

// 工具函数
uint64_t cache_now_ms(void);
int cache_entry_is_expired(cache_entry_t* entry, uint64_t now);
size_t cache_entry_total_size(cache_entry_header_t* header);

// 搜索内部函数
int cache_search_internal_prefix(cache_t* cache, const char* prefix,
                                 const cache_search_options_t* options,
                                 cache_result_t** out_results, size_t* out_count);
int cache_search_internal_regex(cache_t* cache, const char* pattern,
                                const cache_search_options_t* options,
                                cache_result_t** out_results, size_t* out_count);
int cache_search_internal_fuzzy(cache_t* cache, const char* query,
                                const cache_search_options_t* options,
                                cache_result_t** out_results, size_t* out_count);

#endif /* CACHE_INTERNAL_H */