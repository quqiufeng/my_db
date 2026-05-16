#include "mydb_internal.h"

// 零拷贝哈希表：所有数据分配在 mmap pool 中
// 使用 offset 而非指针（因为 mmap 地址可能变化）

// Hash 表头（存储在 pool 中）
typedef struct {
    size_t  bucket_count;
    size_t  size;
    size_t  buckets_offset;  // bucket 数组的 offset
} hash_header_t;

// Hash 节点（存储在 pool 中）
typedef struct {
    size_t  key_offset;      // key 数据的 offset
    size_t  key_len;
    rowid_t value;
    size_t  next_offset;     // 下一个节点的 offset（0 表示无）
} hash_node_t;

static uint64_t hash_fnv1a(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3;
    }
    return hash;
}

// 从 pool 分配内存，返回 offset
size_t hash_create(db_pool_t* pool) {
    if (!pool) return 0;
    
    size_t header_off = pool_alloc_offset(pool, sizeof(hash_header_t));
    if (!header_off) return 0;
    
    hash_header_t* header = (hash_header_t*)POOL_PTR(pool, header_off);
    header->bucket_count = 64;
    header->size = 0;
    
    size_t buckets_size = sizeof(size_t) * 64;
    size_t buckets_off = pool_alloc_offset(pool, buckets_size);
    if (!buckets_off) return 0;
    
    memset(POOL_PTR(pool, buckets_off), 0, buckets_size);
    header->buckets_offset = buckets_off;
    
    return header_off;
}

void hash_destroy(db_pool_t* pool, size_t table_offset) {
    if (!pool || !table_offset) return;
    // 零拷贝：无需释放，pool_close 时统一释放 mmap
    (void)table_offset;
}

// 重新哈希所有节点到新 bucket 数组
static int hash_resize(db_pool_t* pool, hash_header_t* header) {
    size_t old_count = header->bucket_count;
    size_t new_count = old_count * 2;
    if (new_count < old_count) return -1; // 溢出
    
    size_t new_buckets_size = sizeof(size_t) * new_count;
    size_t new_buckets_off = pool_alloc_offset(pool, new_buckets_size);
    if (!new_buckets_off) return -1;
    
    size_t* new_buckets = (size_t*)POOL_PTR(pool, new_buckets_off);
    memset(new_buckets, 0, new_buckets_size);
    
    size_t* old_buckets = (size_t*)POOL_PTR(pool, header->buckets_offset);
    
    // 遍历旧 bucket，重新分配到新 bucket
    for (size_t i = 0; i < old_count; i++) {
        size_t node_off = old_buckets[i];
        while (node_off) {
            hash_node_t* node = (hash_node_t*)POOL_PTR(pool, node_off);
            size_t next_off = node->next_offset;
            
            // 重新计算 hash
            void* node_key = POOL_PTR(pool, node->key_offset);
            uint64_t h = hash_fnv1a(node_key, node->key_len);
            size_t idx = h % new_count;
            
            // 头插法插入新 bucket
            node->next_offset = new_buckets[idx];
            new_buckets[idx] = node_off;
            
            node_off = next_off;
        }
    }
    
    header->bucket_count = new_count;
    header->buckets_offset = new_buckets_off;
    return 0;
}

int hash_insert(db_pool_t* pool, size_t table_offset, const void* key, size_t key_len, rowid_t value) {
    if (!pool || !table_offset || !key || key_len == 0) return -1;
    
    hash_header_t* header = (hash_header_t*)POOL_PTR(pool, table_offset);
    size_t* buckets = (size_t*)POOL_PTR(pool, header->buckets_offset);
    
    // 负载因子 > 0.75 时扩容
    if (header->size > 0 && header->size >= header->bucket_count * 3 / 4) {
        if (hash_resize(pool, header) == 0) {
            buckets = (size_t*)POOL_PTR(pool, header->buckets_offset);
        }
    }
    
    uint64_t h = hash_fnv1a(key, key_len);
    size_t idx = h % header->bucket_count;
    
    // 检查是否已存在
    size_t node_off = buckets[idx];
    while (node_off) {
        hash_node_t* node = (hash_node_t*)POOL_PTR(pool, node_off);
        if (node->key_len == key_len) {
            void* node_key = POOL_PTR(pool, node->key_offset);
            if (memcmp(node_key, key, key_len) == 0) {
                node->value = value;
                return 0;
            }
        }
        node_off = node->next_offset;
    }
    
    // 分配新节点
    size_t key_off = pool_alloc_offset(pool, key_len);
    if (!key_off) return -1;
    memcpy(POOL_PTR(pool, key_off), key, key_len);
    
    size_t new_node_off = pool_alloc_offset(pool, sizeof(hash_node_t));
    if (!new_node_off) return -1;
    
    hash_node_t* new_node = (hash_node_t*)POOL_PTR(pool, new_node_off);
    new_node->key_offset = key_off;
    new_node->key_len = key_len;
    new_node->value = value;
    new_node->next_offset = buckets[idx];
    
    buckets[idx] = new_node_off;
    header->size++;
    
    return 0;
}

rowid_t hash_lookup(db_pool_t* pool, size_t table_offset, const void* key, size_t key_len) {
    if (!pool || !table_offset || !key || key_len == 0) return 0;
    
    hash_header_t* header = (hash_header_t*)POOL_PTR(pool, table_offset);
    size_t* buckets = (size_t*)POOL_PTR(pool, header->buckets_offset);
    
    uint64_t h = hash_fnv1a(key, key_len);
    size_t idx = h % header->bucket_count;
    
    size_t node_off = buckets[idx];
    while (node_off) {
        hash_node_t* node = (hash_node_t*)POOL_PTR(pool, node_off);
        if (node->key_len == key_len) {
            void* node_key = POOL_PTR(pool, node->key_offset);
            if (memcmp(node_key, key, key_len) == 0) {
                return node->value;
            }
        }
        node_off = node->next_offset;
    }
    
    return 0;
}

int hash_delete(db_pool_t* pool, size_t table_offset, const void* key, size_t key_len) {
    if (!pool || !table_offset || !key || key_len == 0) return -1;
    
    hash_header_t* header = (hash_header_t*)POOL_PTR(pool, table_offset);
    size_t* buckets = (size_t*)POOL_PTR(pool, header->buckets_offset);
    
    uint64_t h = hash_fnv1a(key, key_len);
    size_t idx = h % header->bucket_count;
    
    size_t* pp = &buckets[idx];
    size_t node_off = buckets[idx];
    
    while (node_off) {
        hash_node_t* node = (hash_node_t*)POOL_PTR(pool, node_off);
        if (node->key_len == key_len) {
            void* node_key = POOL_PTR(pool, node->key_offset);
            if (memcmp(node_key, key, key_len) == 0) {
                *pp = node->next_offset;
                header->size--;
                return 0;
            }
        }
        pp = &node->next_offset;
        node_off = node->next_offset;
    }
    
    return -1;
}

// 构建索引键（支持复合索引）
static void* build_key_from_row(db_table_t* table, db_index_t* index, void* row_ptr, size_t* key_len) {
    if (!index || !row_ptr || !key_len) return NULL;
    
    size_t total_size = 0;
    for (int i = 0; i < index->field_count && i < MYDB_MAX_INDEX_FIELDS; i++) {
        total_size += index->field_sizes[i];
    }
    
    if (total_size == 0) return NULL;
    
    void* key = malloc(total_size);
    if (!key) return NULL;
    
    size_t offset = 0;
    for (int i = 0; i < index->field_count && i < MYDB_MAX_INDEX_FIELDS; i++) {
        void* field_ptr = (char*)row_ptr + index->field_offsets[i];
        size_t sz = index->field_sizes[i];
        
        if (index->field_types[i] == DB_TYPE_STRING) {
            size_t str_len = 0;
            while (str_len < sz && ((char*)field_ptr)[str_len] != '\0') str_len++;
            sz = str_len;
        } else if (index->field_types[i] == DB_TYPE_VARSTRING) {
            db_string_ref_t* ref = (db_string_ref_t*)field_ptr;
            if (ref->length > 0 && ref->offset > 0 && table) {
                char* str = (char*)table->string_pool.base + ref->offset;
                memcpy((char*)key + offset, str, ref->length);
                offset += ref->length;
                continue;
            }
            sz = 0;
        }
        
        memcpy((char*)key + offset, field_ptr, sz);
        offset += sz;
    }
    
    *key_len = offset;
    return key;
}

// 索引操作封装
int index_create(db_table_t* table, const char* field_name,
                 size_t field_offset, int field_type) {
    if (!table || !field_name) return -1;
    
    db_index_t* index = (db_index_t*)calloc(1, sizeof(db_index_t));
    if (!index) return -1;
    
    snprintf(index->name, MYDB_TABLE_NAME_LEN, "%s", field_name);
    index->field_count = 1;
    index->field_offsets[0] = field_offset;
    index->field_types[0] = field_type;
    
    for (size_t i = 0; i < table->field_count; i++) {
        if (table->fields[i].offset == field_offset) {
            index->field_sizes[0] = table->fields[i].size;
            break;
        }
    }
    
    if (field_type == DB_TYPE_STRING || field_type == DB_TYPE_VARSTRING) {
        index->type = INDEX_HASH;
    } else {
        index->type = INDEX_BTREE;
    }
    
    // 从零拷贝 pool 创建 hash 表
    index->data_offset = hash_create(&table->index_pool);
    if (!index->data_offset) {
        free(index);
        return -1;
    }
    
    index->next = table->indexes;
    table->indexes = index;
    
    return 0;
}

void index_insert(db_table_t* table, rowid_t rowid, void* row_ptr) {
    if (!table || !row_ptr) return;
    
    db_index_t* index = table->indexes;
    while (index) {
        size_t key_len = 0;
        void* key = build_key_from_row(table, index, row_ptr, &key_len);
        if (key) {
            if (index->type == INDEX_HASH) {
                hash_insert(&table->index_pool, index->data_offset, key, key_len, rowid);
            }
            // B+树索引后续实现
            free(key);
        }
        index = index->next;
    }
}

void index_delete(db_table_t* table, rowid_t rowid, void* row_ptr) {
    (void)rowid;
    if (!table || !row_ptr) return;
    
    db_index_t* index = table->indexes;
    while (index) {
        size_t key_len = 0;
        void* key = build_key_from_row(table, index, row_ptr, &key_len);
        if (key) {
            if (index->type == INDEX_HASH) {
                hash_delete(&table->index_pool, index->data_offset, key, key_len);
            }
            free(key);
        }
        index = index->next;
    }
}

rowid_t* index_lookup(db_table_t* table, const char* field_name,
                      const void* value, size_t* count) {
    if (!table || !field_name || !value || !count) return NULL;
    *count = 0;
    
    db_index_t* index = table->indexes;
    while (index) {
        if (strcmp(index->name, field_name) == 0) {
            size_t key_len = index->field_sizes[0];
            if (index->field_types[0] == DB_TYPE_STRING) {
                key_len = strlen((char*)value);
            } else {
                key_len = sizeof(uint64_t);
            }
            
            rowid_t rowid = 0;
            if (index->type == INDEX_HASH) {
                rowid = hash_lookup(&table->index_pool, index->data_offset, value, key_len);
            }
            
            if (rowid > 0) {
                rowid_t* result = (rowid_t*)malloc(sizeof(rowid_t));
                if (result) {
                    result[0] = rowid;
                    *count = 1;
                }
                return result;
            }
            return NULL;
        }
        index = index->next;
    }
    
    return NULL;
}
