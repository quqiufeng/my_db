#include "mydb_internal.h"

// 零拷贝 B+树（基于 pool offset）
// 所有节点和 key 数据从 pool_alloc 分配，使用 offset 寻址

#define BTREE_ORDER 32

// B+树头（存储在 pool 中）
typedef struct {
    size_t root_offset;   // 根节点 offset
    size_t key_size;      // 键大小
    int    key_type;      // 键类型
} btree_header_t;

// B+树节点（固定大小，存储在 pool 中）
typedef struct {
    int     is_leaf;
    int     num_keys;
    size_t  keys[BTREE_ORDER];        // key 数据在 pool 中的 offset
    rowid_t values[BTREE_ORDER];      // 值数组
    size_t  children[BTREE_ORDER + 1]; // 子节点 offset（0 表示无）
    size_t  next;                     // 下一个叶子节点 offset
} btree_node_t;

// 辅助宏：通过 offset 获取指针
#define BTREE_HDR(pool, off) ((btree_header_t*)POOL_PTR(pool, off))
#define BTREE_NODE(pool, off) ((btree_node_t*)POOL_PTR(pool, off))

static int btree_compare(db_pool_t* pool, size_t key_a_off, size_t key_b_off,
                         size_t key_size, int key_type) {
    void* a = POOL_PTR(pool, key_a_off);
    void* b = POOL_PTR(pool, key_b_off);
    
    if (key_type == DB_TYPE_STRING || key_type == DB_TYPE_VARSTRING) {
        return strcmp((char*)a, (char*)b);
    } else if (key_size == 4) {
        int32_t va = *(int32_t*)a;
        int32_t vb = *(int32_t*)b;
        return (va > vb) - (va < vb);
    } else if (key_size == 8) {
        int64_t va = *(int64_t*)a;
        int64_t vb = *(int64_t*)b;
        return (va > vb) - (va < vb);
    }
    return memcmp(a, b, key_size);
}

static int btree_compare_key(db_pool_t* pool, const void* key_a, size_t key_b_off,
                             size_t key_size, int key_type) {
    void* b = POOL_PTR(pool, key_b_off);
    
    if (key_type == DB_TYPE_STRING || key_type == DB_TYPE_VARSTRING) {
        return strcmp((char*)key_a, (char*)b);
    } else if (key_size == 4) {
        int32_t va = *(int32_t*)key_a;
        int32_t vb = *(int32_t*)b;
        return (va > vb) - (va < vb);
    } else if (key_size == 8) {
        int64_t va = *(int64_t*)key_a;
        int64_t vb = *(int64_t*)b;
        return (va > vb) - (va < vb);
    }
    return memcmp(key_a, b, key_size);
}

// 从 pool 分配一个节点
static size_t btree_alloc_node(db_pool_t* pool, int is_leaf) {
    size_t node_off = pool_alloc_offset(pool, sizeof(btree_node_t));
    if (!node_off) return 0;
    
    btree_node_t* node = BTREE_NODE(pool, node_off);
    memset(node, 0, sizeof(btree_node_t));
    node->is_leaf = is_leaf;
    return node_off;
}

// 从 pool 分配 key 数据
static size_t btree_alloc_key(db_pool_t* pool, const void* key, size_t key_size) {
    size_t key_off = pool_alloc_offset(pool, key_size);
    if (!key_off) return 0;
    memcpy(POOL_PTR(pool, key_off), key, key_size);
    return key_off;
}

size_t btree_create(db_pool_t* pool, size_t key_size, int key_type) {
    if (!pool) return 0;
    
    // 分配头
    size_t hdr_off = pool_alloc_offset(pool, sizeof(btree_header_t));
    if (!hdr_off) return 0;
    
    btree_header_t* hdr = BTREE_HDR(pool, hdr_off);
    hdr->key_size = key_size;
    hdr->key_type = key_type;
    
    // 分配根节点（叶子）
    size_t root_off = btree_alloc_node(pool, 1);
    if (!root_off) return 0;
    hdr->root_offset = root_off;
    
    return hdr_off;
}

// 递归销毁节点（仅清空，不 free，因为 pool 统一管理）
static void btree_destroy_node(db_pool_t* pool, size_t node_off) {
    if (!node_off) return;
    
    btree_node_t* node = BTREE_NODE(pool, node_off);
    if (!node->is_leaf) {
        for (int i = 0; i <= node->num_keys; i++) {
            btree_destroy_node(pool, node->children[i]);
        }
    }
    // 注意：pool 空间不单独释放，由 pool_close 统一释放
}

void btree_destroy(db_pool_t* pool, size_t tree_offset) {
    if (!pool || !tree_offset) return;
    
    btree_header_t* hdr = BTREE_HDR(pool, tree_offset);
    btree_destroy_node(pool, hdr->root_offset);
    // pool 空间由 pool_close 统一释放
}

static void btree_split_child(db_pool_t* pool, size_t parent_off, int idx,
                               size_t key_size);
static void btree_insert_non_full(db_pool_t* pool, size_t node_off,
                                   const void* key, rowid_t value,
                                   size_t key_size, int key_type);

int btree_insert(db_pool_t* pool, size_t tree_offset, const void* key, rowid_t value) {
    if (!pool || !tree_offset || !key) return -1;
    
    btree_header_t* hdr = BTREE_HDR(pool, tree_offset);
    size_t root_off = hdr->root_offset;
    size_t key_size = hdr->key_size;
    int key_type = hdr->key_type;
    
    btree_node_t* root = BTREE_NODE(pool, root_off);
    
    if (root->num_keys == BTREE_ORDER) {
        size_t new_root_off = btree_alloc_node(pool, 0);
        if (!new_root_off) return -1;
        
        btree_node_t* new_root = BTREE_NODE(pool, new_root_off);
        new_root->children[0] = root_off;
        btree_split_child(pool, new_root_off, 0, key_size);
        hdr->root_offset = new_root_off;
        
        // 找到正确的子节点插入
        int i = 0;
        if (btree_compare_key(pool, key, new_root->keys[0], key_size, key_type) > 0) {
            i++;
        }
        btree_insert_non_full(pool, new_root->children[i], key, value, key_size, key_type);
    } else {
        btree_insert_non_full(pool, root_off, key, value, key_size, key_type);
    }
    
    return 0;
}

static void btree_split_child(db_pool_t* pool, size_t parent_off, int idx,
                               size_t key_size) {
    btree_node_t* parent = BTREE_NODE(pool, parent_off);
    size_t child_off = parent->children[idx];
    btree_node_t* child = BTREE_NODE(pool, child_off);
    
    size_t new_child_off = btree_alloc_node(pool, child->is_leaf);
    if (!new_child_off) return;
    
    btree_node_t* new_child = BTREE_NODE(pool, new_child_off);
    int mid = BTREE_ORDER / 2;
    new_child->num_keys = child->num_keys - mid - 1;
    
    for (int i = 0; i < new_child->num_keys; i++) {
        new_child->keys[i] = child->keys[i + mid + 1];
        new_child->values[i] = child->values[i + mid + 1];
    }
    
    if (!child->is_leaf) {
        for (int i = 0; i <= new_child->num_keys; i++) {
            new_child->children[i] = child->children[i + mid + 1];
        }
    } else {
        new_child->next = child->next;
        child->next = new_child_off;
    }
    
    child->num_keys = mid;
    
    for (int i = parent->num_keys; i >= idx + 1; i--) {
        parent->children[i + 1] = parent->children[i];
    }
    parent->children[idx + 1] = new_child_off;
    
    for (int i = parent->num_keys - 1; i >= idx; i--) {
        parent->keys[i + 1] = parent->keys[i];
        parent->values[i + 1] = parent->values[i];
    }
    
    parent->keys[idx] = child->keys[mid];
    parent->values[idx] = child->values[mid];
    parent->num_keys++;
}

static void btree_insert_non_full(db_pool_t* pool, size_t node_off,
                                   const void* key, rowid_t value,
                                   size_t key_size, int key_type) {
    btree_node_t* node = BTREE_NODE(pool, node_off);
    int i = node->num_keys - 1;
    
    if (node->is_leaf) {
        size_t key_off = btree_alloc_key(pool, key, key_size);
        if (!key_off) return;
        
        while (i >= 0 && btree_compare_key(pool, key, node->keys[i], key_size, key_type) < 0) {
            node->keys[i + 1] = node->keys[i];
            node->values[i + 1] = node->values[i];
            i--;
        }
        
        node->keys[i + 1] = key_off;
        node->values[i + 1] = value;
        node->num_keys++;
    } else {
        while (i >= 0 && btree_compare_key(pool, key, node->keys[i], key_size, key_type) < 0) {
            i--;
        }
        i++;
        
        btree_node_t* child = BTREE_NODE(pool, node->children[i]);
        if (child->num_keys == BTREE_ORDER) {
            btree_split_child(pool, node_off, i, key_size);
            if (btree_compare_key(pool, key, node->keys[i], key_size, key_type) > 0) {
                i++;
            }
        }
        btree_insert_non_full(pool, node->children[i], key, value, key_size, key_type);
    }
}

// B+树查找（等值查询）
rowid_t btree_lookup(db_pool_t* pool, size_t tree_offset, const void* key) {
    if (!pool || !tree_offset || !key) return 0;
    
    btree_header_t* hdr = BTREE_HDR(pool, tree_offset);
    size_t key_size = hdr->key_size;
    int key_type = hdr->key_type;
    
    size_t node_off = hdr->root_offset;
    btree_node_t* node = BTREE_NODE(pool, node_off);
    
    // 从根节点向下查找
    while (node && !node->is_leaf) {
        int i = node->num_keys - 1;
        while (i >= 0 && btree_compare_key(pool, key, node->keys[i], key_size, key_type) < 0) {
            i--;
        }
        i++;
        node_off = node->children[i];
        node = BTREE_NODE(pool, node_off);
    }
    
    // 在叶子节点中线性查找
    if (node && node->is_leaf) {
        for (int i = 0; i < node->num_keys; i++) {
            if (btree_compare_key(pool, key, node->keys[i], key_size, key_type) == 0) {
                return node->values[i];
            }
        }
    }
    
    return 0;
}

rowid_t* btree_range(db_pool_t* pool, size_t tree_offset,
                      const void* min_key, const void* max_key, size_t* count) {
    if (!pool || !tree_offset || !count) return NULL;
    *count = 0;
    
    btree_header_t* hdr = BTREE_HDR(pool, tree_offset);
    size_t key_size = hdr->key_size;
    int key_type = hdr->key_type;
    
    // 找到起始叶子节点
    size_t node_off = hdr->root_offset;
    btree_node_t* node = BTREE_NODE(pool, node_off);
    while (node && !node->is_leaf) {
        node_off = node->children[0];
        node = BTREE_NODE(pool, node_off);
    }
    
    // 收集范围内的值
    size_t capacity = 64;
    rowid_t* result = (rowid_t*)malloc(sizeof(rowid_t) * capacity);
    if (!result) return NULL;
    
    while (node_off) {
        node = BTREE_NODE(pool, node_off);
        for (int i = 0; i < node->num_keys; i++) {
            int after_min = 1;
            int before_max = 1;
            
            if (min_key) {
                after_min = btree_compare_key(pool, min_key, node->keys[i], key_size, key_type) <= 0;
            }
            if (max_key) {
                before_max = btree_compare_key(pool, max_key, node->keys[i], key_size, key_type) >= 0;
            }
            
            if (after_min && before_max) {
                if (*count >= capacity) {
                    capacity *= 2;
                    rowid_t* new_result = realloc(result, sizeof(rowid_t) * capacity);
                    if (!new_result) {
                        free(result);
                        return NULL;
                    }
                    result = new_result;
                }
                result[(*count)++] = node->values[i];
            }
        }
        node_off = node->next;
    }
    
    return result;
}