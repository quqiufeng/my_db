#include "mydb_internal.h"

// 简单的开链法哈希表
// key: uint64_t 或任意二进制数据
// value: rowid_t

typedef struct hash_node {
    void*           key;
    size_t          key_len;
    rowid_t         value;
    struct hash_node* next;
} hash_node_t;

typedef struct {
    hash_node_t**   buckets;
    size_t          bucket_count;
    size_t          size;
} hash_table_t;

static uint64_t hash_fnv1a(const void* data, size_t len) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint64_t hash = 0xcbf29ce484222325;
    for (size_t i = 0; i < len; i++) {
        hash ^= bytes[i];
        hash *= 0x100000001b3;
    }
    return hash;
}

void* hash_create(void) {
    hash_table_t* ht = (hash_table_t*)calloc(1, sizeof(hash_table_t));
    if (!ht) return NULL;
    
    ht->bucket_count = 64;
    ht->buckets = (hash_node_t**)calloc(ht->bucket_count, sizeof(hash_node_t*));
    if (!ht->buckets) {
        free(ht);
        return NULL;
    }
    
    return ht;
}

void hash_destroy(void* hash) {
    hash_table_t* ht = (hash_table_t*)hash;
    if (!ht) return;
    
    for (size_t i = 0; i < ht->bucket_count; i++) {
        hash_node_t* node = ht->buckets[i];
        while (node) {
            hash_node_t* next = node->next;
            free(node->key);
            free(node);
            node = next;
        }
    }
    
    free(ht->buckets);
    free(ht);
}

int hash_insert(void* hash, const void* key, size_t key_len, rowid_t value) {
    hash_table_t* ht = (hash_table_t*)hash;
    if (!ht || !key || key_len == 0) return -1;
    
    uint64_t h = hash_fnv1a(key, key_len);
    size_t idx = h % ht->bucket_count;
    
    // 检查是否已存在
    hash_node_t* node = ht->buckets[idx];
    while (node) {
        if (node->key_len == key_len && memcmp(node->key, key, key_len) == 0) {
            node->value = value; // 更新
            return 0;
        }
        node = node->next;
    }
    
    // 插入新节点
    hash_node_t* new_node = (hash_node_t*)malloc(sizeof(hash_node_t));
    if (!new_node) return -1;
    
    new_node->key = malloc(key_len);
    if (!new_node->key) {
        free(new_node);
        return -1;
    }
    memcpy(new_node->key, key, key_len);
    new_node->key_len = key_len;
    new_node->value = value;
    new_node->next = ht->buckets[idx];
    ht->buckets[idx] = new_node;
    ht->size++;
    
    return 0;
}

rowid_t hash_lookup(void* hash, const void* key, size_t key_len) {
    hash_table_t* ht = (hash_table_t*)hash;
    if (!ht || !key || key_len == 0) return 0;
    
    uint64_t h = hash_fnv1a(key, key_len);
    size_t idx = h % ht->bucket_count;
    
    hash_node_t* node = ht->buckets[idx];
    while (node) {
        if (node->key_len == key_len && memcmp(node->key, key, key_len) == 0) {
            return node->value;
        }
        node = node->next;
    }
    
    return 0;
}

int hash_delete(void* hash, const void* key, size_t key_len) {
    hash_table_t* ht = (hash_table_t*)hash;
    if (!ht || !key || key_len == 0) return -1;
    
    uint64_t h = hash_fnv1a(key, key_len);
    size_t idx = h % ht->bucket_count;
    
    hash_node_t** pp = &ht->buckets[idx];
    hash_node_t* node = ht->buckets[idx];
    while (node) {
        if (node->key_len == key_len && memcmp(node->key, key, key_len) == 0) {
            *pp = node->next;
            free(node->key);
            free(node);
            ht->size--;
            return 0;
        }
        pp = &node->next;
        node = node->next;
    }
    
    return -1;
}

// 构建索引键（支持复合索引）
static void* build_key_from_row(db_index_t* index, void* row_ptr, size_t* key_len) {
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
            // 字符串字段只复制实际长度
            size_t str_len = 0;
            while (str_len < sz && ((char*)field_ptr)[str_len] != '\0') str_len++;
            sz = str_len;
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
    
    strncpy(index->name, field_name, MYDB_TABLE_NAME_LEN - 1);
    index->field_count = 1;
    index->field_offsets[0] = field_offset;
    index->field_types[0] = field_type;
    
    // 查找字段大小
    for (size_t i = 0; i < table->field_count; i++) {
        if (table->fields[i].offset == field_offset) {
            index->field_sizes[0] = table->fields[i].size;
            break;
        }
    }
    
    if (field_type == DB_TYPE_STRING) {
        index->type = INDEX_HASH;
    } else {
        index->type = INDEX_BTREE;
    }
    
    index->data = hash_create();
    if (!index->data) {
        free(index);
        return -1;
    }
    
    // 添加到索引链表
    index->next = table->indexes;
    table->indexes = index;
    
    return 0;
}

void index_insert(db_table_t* table, rowid_t rowid, void* row_ptr) {
    if (!table || !row_ptr) return;
    
    db_index_t* index = table->indexes;
    while (index) {
        size_t key_len = 0;
        void* key = build_key_from_row(index, row_ptr, &key_len);
        if (key) {
            hash_insert(index->data, key, key_len, rowid);
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
        void* key = build_key_from_row(index, row_ptr, &key_len);
        if (key) {
            hash_delete(index->data, key, key_len);
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
            
            rowid_t rowid = hash_lookup(index->data, value, key_len);
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
