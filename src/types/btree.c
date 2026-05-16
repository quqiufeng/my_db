#include "mydb_internal.h"

// 简化版 B+树（用于范围查询和排序）
// 每个节点存储多个键值对，使用数组实现

#define BTREE_ORDER 32  // 每个节点的最大键数

typedef struct btree_node {
    int             is_leaf;
    int             num_keys;
    void*           keys[BTREE_ORDER];      // 键数组
    rowid_t         values[BTREE_ORDER];    // 值数组
    struct btree_node* children[BTREE_ORDER + 1]; // 子节点（非叶子节点）
    struct btree_node* next;                // 叶子节点链表（用于范围扫描）
} btree_node_t;

typedef struct {
    btree_node_t*   root;
    size_t          key_size;
    int             key_type;
} btree_t;

static int btree_compare(void* a, void* b, size_t size, int type) {
    if (type == DB_TYPE_STRING) {
        return strcmp((char*)a, (char*)b);
    } else if (size == 4) {
        int32_t va = *(int32_t*)a;
        int32_t vb = *(int32_t*)b;
        return (va > vb) - (va < vb);
    } else if (size == 8) {
        int64_t va = *(int64_t*)a;
        int64_t vb = *(int64_t*)b;
        return (va > vb) - (va < vb);
    }
    return memcmp(a, b, size);
}

static btree_node_t* btree_create_node(int is_leaf) {
    btree_node_t* node = (btree_node_t*)calloc(1, sizeof(btree_node_t));
    if (node) {
        node->is_leaf = is_leaf;
    }
    return node;
}

void* btree_create(size_t key_size, int key_type) {
    btree_t* tree = (btree_t*)malloc(sizeof(btree_t));
    if (!tree) return NULL;
    
    tree->root = btree_create_node(1);
    tree->key_size = key_size;
    tree->key_type = key_type;
    
    if (!tree->root) {
        free(tree);
        return NULL;
    }
    
    return tree;
}

void btree_destroy_node(btree_node_t* node) {
    if (!node) return;
    
    if (!node->is_leaf) {
        for (int i = 0; i <= node->num_keys; i++) {
            btree_destroy_node(node->children[i]);
        }
    } else {
        for (int i = 0; i < node->num_keys; i++) {
            free(node->keys[i]);
        }
    }
    
    free(node);
}

void btree_destroy(void* tree) {
    btree_t* t = (btree_t*)tree;
    if (!t) return;
    
    btree_destroy_node(t->root);
    free(t);
}

static void btree_insert_non_full(btree_t* tree, btree_node_t* node, void* key, rowid_t value);
static void btree_split_child(btree_t* tree, btree_node_t* parent, int idx);

int btree_insert(void* tree, void* key, rowid_t value) {
    btree_t* t = (btree_t*)tree;
    if (!t || !key) return -1;
    
    btree_node_t* root = t->root;
    
    if (root->num_keys == BTREE_ORDER) {
        btree_node_t* new_root = btree_create_node(0);
        if (!new_root) return -1;
        
        new_root->children[0] = root;
        btree_split_child(t, new_root, 0);
        t->root = new_root;
        
        // 找到正确的子节点插入
        int i = 0;
        if (btree_compare(key, new_root->keys[0], t->key_size, t->key_type) > 0) {
            i++;
        }
        btree_insert_non_full(t, new_root->children[i], key, value);
    } else {
        btree_insert_non_full(t, root, key, value);
    }
    
    return 0;
}

static void btree_split_child(btree_t* tree, btree_node_t* parent, int idx) {
    btree_node_t* child = parent->children[idx];
    btree_node_t* new_child = btree_create_node(child->is_leaf);
    if (!new_child) return;
    
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
        child->next = new_child;
    }
    
    child->num_keys = mid;
    
    for (int i = parent->num_keys; i >= idx + 1; i--) {
        parent->children[i + 1] = parent->children[i];
    }
    parent->children[idx + 1] = new_child;
    
    for (int i = parent->num_keys - 1; i >= idx; i--) {
        parent->keys[i + 1] = parent->keys[i];
        parent->values[i + 1] = parent->values[i];
    }
    
    parent->keys[idx] = child->keys[mid];
    parent->values[idx] = child->values[mid];
    parent->num_keys++;
}

static void btree_insert_non_full(btree_t* tree, btree_node_t* node, void* key, rowid_t value) {
    int i = node->num_keys - 1;
    
    if (node->is_leaf) {
        void* key_copy = malloc(tree->key_size);
        if (!key_copy) return;
        memcpy(key_copy, key, tree->key_size);
        
        while (i >= 0 && btree_compare(key, node->keys[i], tree->key_size, tree->key_type) < 0) {
            node->keys[i + 1] = node->keys[i];
            node->values[i + 1] = node->values[i];
            i--;
        }
        
        node->keys[i + 1] = key_copy;
        node->values[i + 1] = value;
        node->num_keys++;
    } else {
        while (i >= 0 && btree_compare(key, node->keys[i], tree->key_size, tree->key_type) < 0) {
            i--;
        }
        i++;
        
        if (node->children[i]->num_keys == BTREE_ORDER) {
            btree_split_child(tree, node, i);
            if (btree_compare(key, node->keys[i], tree->key_size, tree->key_type) > 0) {
                i++;
            }
        }
        btree_insert_non_full(tree, node->children[i], key, value);
    }
}

rowid_t* btree_range(void* tree, void* min_key, void* max_key, size_t* count) {
    btree_t* t = (btree_t*)tree;
    if (!t || !count) return NULL;
    *count = 0;
    
    // 简化实现：遍历所有叶子节点
    // 找到起始叶子节点
    btree_node_t* node = t->root;
    while (node && !node->is_leaf) {
        node = node->children[0];
    }
    
    // 收集范围内的值
    size_t capacity = 64;
    rowid_t* result = (rowid_t*)malloc(sizeof(rowid_t) * capacity);
    if (!result) return NULL;
    
    while (node) {
        for (int i = 0; i < node->num_keys; i++) {
            int after_min = 1;
            int before_max = 1;
            
            if (min_key) {
                after_min = btree_compare(node->keys[i], min_key, t->key_size, t->key_type) >= 0;
            }
            if (max_key) {
                before_max = btree_compare(node->keys[i], max_key, t->key_size, t->key_type) <= 0;
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
        node = node->next;
    }
    
    return result;
}
