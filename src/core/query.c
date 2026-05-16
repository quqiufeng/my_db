#include "mydb_internal.h"
#include <stdio.h>
#include <stdlib.h>

bool row_match(db_table_t* table, void* row_ptr,
               const db_condition_t* conditions, size_t count) {
    if (!conditions || count == 0) return true;
    
    for (size_t i = 0; i < count; i++) {
        const db_condition_t* cond = &conditions[i];
        void* field_ptr = (char*)row_ptr + cond->field_offset;
        
        int cmp = 0;
        switch (cond->field_size) {
            case 1:
                cmp = *(int8_t*)field_ptr - *(int8_t*)cond->value;
                break;
            case 2:
                cmp = *(int16_t*)field_ptr - *(int16_t*)cond->value;
                break;
            case 4:
                if (cond->op == 1 || cond->op == 2) {
                    // 数值比较
                    cmp = (*(int32_t*)field_ptr > *(int32_t*)cond->value) ? 1 :
                          (*(int32_t*)field_ptr < *(int32_t*)cond->value) ? -1 : 0;
                } else {
                    cmp = *(int32_t*)field_ptr - *(int32_t*)cond->value;
                }
                break;
            case 8:
                if (cond->op == 1 || cond->op == 2) {
                    cmp = (*(int64_t*)field_ptr > *(int64_t*)cond->value) ? 1 :
                          (*(int64_t*)field_ptr < *(int64_t*)cond->value) ? -1 : 0;
                } else {
                    cmp = (*(int64_t*)field_ptr > *(int64_t*)cond->value) ? 1 :
                          (*(int64_t*)field_ptr < *(int64_t*)cond->value) ? -1 : 0;
                }
                break;
            default:
                cmp = memcmp(field_ptr, cond->value, cond->field_size);
                break;
        }
        
        bool match = false;
        switch (cond->op) {
            case 0: match = (cmp == 0); break;  // 等于
            case 1: match = (cmp > 0); break;   // 大于
            case 2: match = (cmp < 0); break;   // 小于
            default: match = (cmp == 0); break;
        }
        
        if (!match) return false;
    }
    
    return true;
}

static int* g_sort_field = NULL;
static size_t g_sort_field_size = 0;

static int sort_compare(const void* a, const void* b) {
    if (!g_sort_field) return 0;
    
    void* field_a = (char*)a + *g_sort_field;
    void* field_b = (char*)b + *g_sort_field;
    
    int cmp = 0;
    switch (g_sort_field_size) {
        case 4:
            cmp = *(int32_t*)field_a - *(int32_t*)field_b;
            break;
        case 8:
            cmp = (*(int64_t*)field_a > *(int64_t*)field_b) ? 1 :
                  (*(int64_t*)field_a < *(int64_t*)field_b) ? -1 : 0;
            break;
        default:
            cmp = memcmp(field_a, field_b, g_sort_field_size);
            break;
    }
    
    return cmp;
}

static rowid_t* query_internal(db_table_t* table,
                                const db_condition_t* conditions, size_t condition_count,
                                size_t order_field_offset, int ascending,
                                size_t limit_offset, size_t limit_count,
                                size_t* out_count) {
    if (!table) {
        *out_count = 0;
        return NULL;
    }
    
    // 收集匹配的行
    rowid_t* results = (rowid_t*)malloc(sizeof(rowid_t) * table->row_count);
    if (!results) {
        *out_count = 0;
        return NULL;
    }
    
    size_t match_count = 0;
    for (rowid_t id = 1; id <= table->max_rowid; id++) {
        size_t offset = table->data_offset + (id - 1) * (sizeof(row_header_t) + table->row_size);
        row_header_t* header = (row_header_t*)PTR(table->data_pool->base, offset);
        
        if (header->flags & MYDB_DELETED_FLAG) continue;
        
        void* row_ptr = (char*)header + sizeof(row_header_t);
        if (row_match(table, row_ptr, conditions, condition_count)) {
            results[match_count++] = id;
        }
    }
    
    // ORDER BY
    if (order_field_offset > 0 && match_count > 1) {
        g_sort_field = (int*)(uintptr_t)order_field_offset;
        g_sort_field_size = 0;
        
        // 查找字段大小
        for (size_t i = 0; i < table->field_count; i++) {
            if (table->fields[i].offset == order_field_offset) {
                g_sort_field_size = table->fields[i].size;
                break;
            }
        }
        
        if (g_sort_field_size > 0) {
            // 创建临时数组用于排序
            void* sort_array = malloc(match_count * table->row_size);
            if (sort_array) {
                for (size_t i = 0; i < match_count; i++) {
                    size_t offset = table->data_offset + (results[i] - 1) * (sizeof(row_header_t) + table->row_size);
                    row_header_t* header = (row_header_t*)PTR(table->data_pool->base, offset);
                    memcpy((char*)sort_array + i * table->row_size,
                           (char*)header + sizeof(row_header_t), table->row_size);
                }
                
                qsort(sort_array, match_count, table->row_size, sort_compare);
                
                // 重新映射回 rowid（简化：直接返回排序后的 rowid）
                // 完整实现需要更复杂的映射
                
                free(sort_array);
            }
        }
        
        g_sort_field = NULL;
    }
    
    // LIMIT
    if (limit_offset > 0 || limit_count > 0) {
        size_t start = (limit_offset < match_count) ? limit_offset : match_count;
        size_t end = (limit_count > 0 && start + limit_count < match_count) ? start + limit_count : match_count;
        
        size_t new_count = end - start;
        if (new_count > 0 && start > 0) {
            memmove(results, results + start, new_count * sizeof(rowid_t));
        }
        match_count = new_count;
    }
    
    *out_count = match_count;
    return results;
}

const char* db_select_by_pk_json(table_t table, rowid_t id) {
    if (!table || id == 0) return NULL;
    db_table_t* t = (db_table_t*)table;
    
    if (id > t->max_rowid) return NULL;
    
    size_t offset = t->data_offset + (id - 1) * (sizeof(row_header_t) + t->row_size);
    row_header_t* header = (row_header_t*)PTR(t->data_pool->base, offset);
    
    if (header->flags & MYDB_DELETED_FLAG) return NULL;
    
    void* row_ptr = (char*)header + sizeof(row_header_t);
    return json_row(t, row_ptr);
}

const char* db_select_all_json(table_t table) {
    if (!table) return NULL;
    db_table_t* t = (db_table_t*)table;
    
    size_t count = 0;
    rowid_t* results = query_internal(t, NULL, 0, 0, 0, 0, 0, &count);
    if (!results) return NULL;
    
    char* json = json_rows(t, results, count);
    free(results);
    return json;
}

const char* db_select_where_json(table_t table,
                                 const db_condition_t* conditions,
                                 size_t condition_count) {
    if (!table) return NULL;
    db_table_t* t = (db_table_t*)table;
    
    size_t count = 0;
    rowid_t* results = query_internal(t, conditions, condition_count, 0, 0, 0, 0, &count);
    if (!results) return NULL;
    
    char* json = json_rows(t, results, count);
    free(results);
    return json;
}

const char* db_select_json(table_t table,
                            const db_condition_t* conditions, size_t condition_count,
                            size_t order_field_offset, int ascending,
                            size_t limit_offset, size_t limit_count) {
    if (!table) return NULL;
    db_table_t* t = (db_table_t*)table;
    
    size_t count = 0;
    rowid_t* results = query_internal(t, conditions, condition_count,
                                       order_field_offset, ascending,
                                       limit_offset, limit_count, &count);
    if (!results) return NULL;
    
    char* json = json_rows(t, results, count);
    free(results);
    return json;
}

const char* db_join_json(table_t left_table, size_t left_field_offset,
                          table_t right_table, size_t right_field_offset,
                          size_t field_size) {
    // 简化实现：嵌套循环 JOIN
    if (!left_table || !right_table) return NULL;
    
    db_table_t* left = (db_table_t*)left_table;
    db_table_t* right = (db_table_t*)right_table;
    
    // 收集结果
    // 简化：返回空数组
    return strdup("[]");
}

void db_json_free(const char* json) {
    free((void*)json);
}

int db_select_all_stream(table_t table, db_row_cb_t cb, void* user_data) {
    if (!table || !cb) return -1;
    db_table_t* t = (db_table_t*)table;
    
    for (rowid_t id = 1; id <= t->max_rowid; id++) {
        size_t offset = t->data_offset + (id - 1) * (sizeof(row_header_t) + t->row_size);
        row_header_t* header = (row_header_t*)PTR(t->data_pool->base, offset);
        
        if (header->flags & MYDB_DELETED_FLAG) continue;
        
        void* row_ptr = (char*)header + sizeof(row_header_t);
        char* json = json_row(t, row_ptr);
        if (json) {
            int ret = cb(json, user_data);
            free(json);
            if (ret != 0) return ret;
        }
    }
    
    return 0;
}

int db_select_where_stream(table_t table,
                           const db_condition_t* conditions, size_t condition_count,
                           db_row_cb_t cb, void* user_data) {
    if (!table || !cb) return -1;
    db_table_t* t = (db_table_t*)table;
    
    for (rowid_t id = 1; id <= t->max_rowid; id++) {
        size_t offset = t->data_offset + (id - 1) * (sizeof(row_header_t) + t->row_size);
        row_header_t* header = (row_header_t*)PTR(t->data_pool->base, offset);
        
        if (header->flags & MYDB_DELETED_FLAG) continue;
        
        void* row_ptr = (char*)header + sizeof(row_header_t);
        if (row_match(t, row_ptr, conditions, condition_count)) {
            char* json = json_row(t, row_ptr);
            if (json) {
                int ret = cb(json, user_data);
                free(json);
                if (ret != 0) return ret;
            }
        }
    }
    
    return 0;
}

int db_select_stream(table_t table,
                     const db_condition_t* conditions, size_t condition_count,
                     size_t order_field_offset, int ascending,
                     size_t limit_offset, size_t limit_count,
                     db_row_cb_t cb, void* user_data) {
    // 简化：先收集所有匹配，再回调
    // 完整实现需要排序和 limit 后再流式输出
    if (!table || !cb) return -1;
    
    size_t count = 0;
    rowid_t* results = query_internal((db_table_t*)table, conditions, condition_count,
                                       order_field_offset, ascending,
                                       limit_offset, limit_count, &count);
    if (!results) return -1;
    
    db_table_t* t = (db_table_t*)table;
    for (size_t i = 0; i < count; i++) {
        size_t offset = t->data_offset + (results[i] - 1) * (sizeof(row_header_t) + t->row_size);
        row_header_t* header = (row_header_t*)PTR(t->data_pool->base, offset);
        void* row_ptr = (char*)header + sizeof(row_header_t);
        
        char* json = json_row(t, row_ptr);
        if (json) {
            int ret = cb(json, user_data);
            free(json);
            if (ret != 0) {
                free(results);
                return ret;
            }
        }
    }
    
    free(results);
    return 0;
}
