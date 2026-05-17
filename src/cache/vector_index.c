#include "cache_internal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CACHE_PTR(cache, offset) ((void*)((char*)(cache)->pool.base + (offset)))

// ====== 向量索引操作 ======

int cache_vector_index_init(cache_t* cache) {
    if (!cache) return CACHE_ERR_INVAL;
    
    cache->vector_index.entries = NULL;
    cache->vector_index.count = 0;
    cache->vector_index.capacity = 0;
    return CACHE_OK;
}

void cache_vector_index_destroy(cache_t* cache) {
    if (!cache) return;
    
    free(cache->vector_index.entries);
    cache->vector_index.entries = NULL;
    cache->vector_index.count = 0;
    cache->vector_index.capacity = 0;
}

// 在pool中分配向量存储空间
static size_t alloc_vector_data(cache_t* cache, const float* vector, size_t dim) {
    size_t data_size = sizeof(float) * dim;
    size_t offset = pool_alloc_offset(&cache->pool, data_size);
    if (!offset) return 0;
    
    float* dest = (float*)CACHE_PTR(cache, offset);
    memcpy(dest, vector, data_size);
    return offset;
}

// 添加向量到索引
int cache_vector_index_add(cache_t* cache, size_t entry_offset, const float* vector, size_t dim) {
    if (!cache || !vector || dim == 0 || dim > CACHE_MAX_VECTOR_DIM) return CACHE_ERR_INVAL;
    
    // 检查是否已存在（更新）
    for (size_t i = 0; i < cache->vector_index.count; i++) {
        if (cache->vector_index.entries[i].entry_offset == entry_offset) {
            // 更新现有向量
            size_t new_offset = alloc_vector_data(cache, vector, dim);
            if (!new_offset) return CACHE_ERR_NOMEM;
            
            cache->vector_index.entries[i].vector_offset = new_offset;
            cache->vector_index.entries[i].dim = dim;
            return CACHE_OK;
        }
    }
    
    // 分配新条目
    if (cache->vector_index.count >= cache->vector_index.capacity) {
        size_t new_cap = cache->vector_index.capacity == 0 ? 16 : cache->vector_index.capacity * 2;
        cache_vector_entry_t* new_entries = realloc(cache->vector_index.entries, 
                                                      sizeof(cache_vector_entry_t) * new_cap);
        if (!new_entries) return CACHE_ERR_NOMEM;
        cache->vector_index.entries = new_entries;
        cache->vector_index.capacity = new_cap;
    }
    
    // 分配向量数据
    size_t vector_offset = alloc_vector_data(cache, vector, dim);
    if (!vector_offset) return CACHE_ERR_NOMEM;
    
    cache_vector_entry_t* entry = &cache->vector_index.entries[cache->vector_index.count];
    entry->entry_offset = entry_offset;
    entry->vector_offset = vector_offset;
    entry->dim = dim;
    cache->vector_index.count++;
    
    return CACHE_OK;
}

// 从索引中移除向量
void cache_vector_index_remove(cache_t* cache, size_t entry_offset) {
    if (!cache) return;
    
    size_t write = 0;
    for (size_t i = 0; i < cache->vector_index.count; i++) {
        if (cache->vector_index.entries[i].entry_offset != entry_offset) {
            cache->vector_index.entries[write++] = cache->vector_index.entries[i];
        }
    }
    cache->vector_index.count = write;
}

// 余弦相似度计算 (-1 to 1, we normalize to 0 to 1)
float cache_vector_cosine_similarity(const float* a, const float* b, size_t dim) {
    if (!a || !b || dim == 0) return -1.0f;
    
    double dot = 0.0;
    double norm_a = 0.0;
    double norm_b = 0.0;
    
    for (size_t i = 0; i < dim; i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    
    if (norm_a == 0.0 || norm_b == 0.0) return 0.0f;
    
    double cosine = dot / (sqrt(norm_a) * sqrt(norm_b));
    // 归一化到 [0, 1]（假设向量已归一化，cosine范围[-1,1]，映射到[0,1]）
    return (float)((cosine + 1.0) / 2.0);
}

// 重建向量索引（扫描所有entry）
void cache_vector_index_rebuild(cache_t* cache) {
    if (!cache) return;
    
    // 清空现有索引
    free(cache->vector_index.entries);
    cache->vector_index.entries = NULL;
    cache->vector_index.count = 0;
    cache->vector_index.capacity = 0;
    
    uint64_t now = cache_now_ms();
    size_t offset = CACHE_HEADER_SIZE;
    
    while (offset + sizeof(cache_entry_header_t) <= cache->pool.used) {
        cache_entry_header_t* h = (cache_entry_header_t*)CACHE_PTR(cache, offset);
        
        // 跳过无效entry
        if (h->key_len == 0 || h->key_len > CACHE_MAX_KEY_LEN ||
            h->value_len > CACHE_MAX_VALUE_LEN ||
            (h->flags & CACHE_ENTRY_DELETED)) {
            offset += MYDB_ALIGN;
            continue;
        }
        
        // 检查是否过期
        if (h->expire_at > 0 && h->expire_at < now) {
            offset += cache_entry_total_size(h);
            continue;
        }
        
        // 检查value中是否包含向量元数据
        const char* value = (const char*)CACHE_PTR(cache, offset + sizeof(cache_entry_header_t) + h->key_len + 1);
        
        // 查找 "__vector_offset":N 和 "__vector_dim":N
        const char* vec_off_str = strstr(value, "\"__vector_offset\":");
        const char* vec_dim_str = strstr(value, "\"__vector_dim\":");
        
        if (vec_off_str && vec_dim_str) {
            size_t vector_offset = 0;
            size_t vector_dim = 0;
            
            sscanf(vec_off_str + 18, "%zu", &vector_offset);
            sscanf(vec_dim_str + 15, "%zu", &vector_dim);
            
            if (vector_offset > 0 && vector_dim > 0 && vector_dim <= CACHE_MAX_VECTOR_DIM) {
                // 添加到索引
                if (cache->vector_index.count >= cache->vector_index.capacity) {
                    size_t new_cap = cache->vector_index.capacity == 0 ? 16 : cache->vector_index.capacity * 2;
                    cache_vector_entry_t* new_entries = realloc(cache->vector_index.entries,
                                                                  sizeof(cache_vector_entry_t) * new_cap);
                    if (new_entries) {
                        cache->vector_index.entries = new_entries;
                        cache->vector_index.capacity = new_cap;
                    }
                }
                
                if (cache->vector_index.count < cache->vector_index.capacity) {
                    cache_vector_entry_t* entry = &cache->vector_index.entries[cache->vector_index.count];
                    entry->entry_offset = offset;
                    entry->vector_offset = vector_offset;
                    entry->dim = vector_dim;
                    cache->vector_index.count++;
                }
            }
        }
        
        offset += cache_entry_total_size(h);
    }
}

// ====== 公共 API 实现 ======

int cache_set_vector(cache_t* cache, const char* key, const char* value,
                     const float* vector, size_t dim, uint64_t ttl_ms) {
    if (!cache || !key || !value || !vector || dim == 0 || dim > CACHE_MAX_VECTOR_DIM) {
        return CACHE_ERR_INVAL;
    }
    
    // 分配向量数据到pool
    size_t vector_offset = alloc_vector_data(cache, vector, dim);
    if (!vector_offset) return CACHE_ERR_NOMEM;
    
    // 构建包含向量元数据的value
    size_t meta_len = snprintf(NULL, 0, "{\"__vector_offset\":%zu,\"__vector_dim\":%zu,\"content\":", 
                                vector_offset, dim);
    size_t value_len = strlen(value);
    size_t total_value_len = meta_len + value_len + 2;  // +2 for closing }
    
    char* new_value = malloc(total_value_len + 1);
    if (!new_value) return CACHE_ERR_NOMEM;
    
    snprintf(new_value, total_value_len + 1, 
             "{\"__vector_offset\":%zu,\"__vector_dim\":%zu,\"content\":%s}",
             vector_offset, dim, value);
    
    // 设置到cache
    int ret = cache_set(cache, key, new_value, ttl_ms);
    free(new_value);
    
    if (ret == CACHE_OK) {
        // 获取刚插入的entry offset
        size_t entry_offset = cache_hash_lookup(cache, key, strlen(key));
        if (entry_offset) {
            cache_vector_index_add(cache, entry_offset, vector, dim);
        }
    }
    
    return ret;
}

const float* cache_get_vector(cache_t* cache, const char* key, size_t* out_dim) {
    if (!cache || !key || !out_dim) return NULL;
    *out_dim = 0;
    
    size_t key_len = strlen(key);
    size_t entry_offset = cache_hash_lookup(cache, key, key_len);
    if (!entry_offset) return NULL;
    
    // 查找向量索引
    for (size_t i = 0; i < cache->vector_index.count; i++) {
        if (cache->vector_index.entries[i].entry_offset == entry_offset) {
            *out_dim = cache->vector_index.entries[i].dim;
            return (float*)CACHE_PTR(cache, cache->vector_index.entries[i].vector_offset);
        }
    }
    
    return NULL;
}

// 用于qsort的比较结构
typedef struct {
    size_t entry_offset;
    float score;
} vector_score_t;

static int compare_vector_score(const void* a, const void* b) {
    float diff = ((vector_score_t*)b)->score - ((vector_score_t*)a)->score;
    if (diff > 0) return 1;
    if (diff < 0) return -1;
    return 0;
}

int cache_search_vector(cache_t* cache, const float* query_vector, size_t dim,
                        int top_k, double min_score,
                        cache_search_options_t* options,
                        cache_result_t** out_results, size_t* out_count) {
    if (!cache || !query_vector || dim == 0 || !out_results || !out_count) {
        return CACHE_ERR_INVAL;
    }
    
    *out_results = NULL;
    *out_count = 0;
    
    if (cache->vector_index.count == 0) return CACHE_OK;  // 无向量数据
    
    uint64_t now = cache_now_ms();
    int max_results = top_k > 0 ? top_k : 10;
    const char* ns_filter = options ? options->ns_filter : NULL;
    
    // 分配分数数组
    vector_score_t* scores = malloc(sizeof(vector_score_t) * cache->vector_index.count);
    if (!scores) return CACHE_ERR_NOMEM;
    
    size_t score_count = 0;
    
    // 计算所有向量的相似度
    for (size_t i = 0; i < cache->vector_index.count; i++) {
        cache_vector_entry_t* entry = &cache->vector_index.entries[i];
        
        if (entry->dim != dim) continue;  // 维度不匹配
        if (!entry_is_valid(cache, entry->entry_offset, now)) continue;
        if (!ns_filter_match(cache, entry->entry_offset, ns_filter)) continue;
        
        float* vector_data = (float*)CACHE_PTR(cache, entry->vector_offset);
        float score = cache_vector_cosine_similarity(query_vector, vector_data, dim);
        
        if (score >= min_score) {
            scores[score_count].entry_offset = entry->entry_offset;
            scores[score_count].score = score;
            score_count++;
        }
    }
    
    if (score_count == 0) {
        free(scores);
        return CACHE_OK;
    }
    
    // 按相似度排序（降序）
    qsort(scores, score_count, sizeof(vector_score_t), compare_vector_score);
    
    // 取前top_k
    size_t result_count = score_count < (size_t)max_results ? score_count : (size_t)max_results;
    
    cache_result_t* results = malloc(sizeof(cache_result_t) * result_count);
    if (!results) {
        free(scores);
        return CACHE_ERR_NOMEM;
    }
    
    for (size_t i = 0; i < result_count; i++) {
        size_t key_len, value_len;
        results[i].key = entry_key(cache, scores[i].entry_offset, &key_len);
        results[i].value = entry_value(cache, scores[i].entry_offset, &value_len);
        results[i].score = scores[i].score;
    }
    
    free(scores);
    
    *out_results = results;
    *out_count = result_count;
    return CACHE_OK;
}
