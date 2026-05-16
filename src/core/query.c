#include "mydb_internal.h"
#include <stdio.h>
#include <stdlib.h>

// 查找是否有索引匹配给定的字段偏移
static db_index_t* find_index(db_table_t* table, size_t field_offset) {
    if (!table || !table->indexes) return NULL;
    
    db_index_t* index = table->indexes;
    while (index) {
        if (index->field_count >= 1 && index->field_offsets[0] == field_offset) {
            return index;
        }
        index = index->next;
    }
    return NULL;
}

// 构建索引键（支持复合索引）
static void* build_index_key(db_index_t* index, const db_condition_t* conditions,
                             size_t condition_count, size_t* key_len) {
    if (!index || !conditions || condition_count == 0 || !key_len) return NULL;
    
    // 计算需要的键大小
    size_t total_size = 0;
    for (int i = 0; i < index->field_count && (size_t)i < condition_count; i++) {
        total_size += index->field_sizes[i];
    }
    
    if (total_size == 0) return NULL;
    
    void* key = malloc(total_size);
    if (!key) return NULL;
    
    size_t offset = 0;
    for (int i = 0; i < index->field_count && (size_t)i < condition_count; i++) {
        size_t sz = index->field_sizes[i];
        memcpy((char*)key + offset, conditions[i].value, sz);
        offset += sz;
    }
    
    *key_len = offset;
    return key;
}

// 使用索引查找
static rowid_t* query_with_index(db_table_t* table, db_index_t* index, 
                                 const db_condition_t* cond, size_t* count) {
    if (!table || !index || !cond || !count) return NULL;
    *count = 0;
    
    if (index->type == INDEX_HASH && cond->op == 0) {
        // 哈希索引只支持等值查询
        size_t key_len = 0;
        void* key = build_index_key(index, cond, 1, &key_len);
        if (!key) return NULL;
        
        if (index->field_types[0] == DB_TYPE_STRING && index->field_count == 1) {
            key_len = strlen((char*)cond->value);
        } else if (index->field_count == 1) {
            key_len = sizeof(uint64_t);
        }
        
        rowid_t rowid = hash_lookup(index->data, key, key_len);
        free(key);
        
        if (rowid > 0) {
            rowid_t* result = (rowid_t*)malloc(sizeof(rowid_t));
            if (result) {
                result[0] = rowid;
                *count = 1;
            }
            return result;
        }
    }
    
    return NULL;
}

bool row_match(db_table_t* table, void* row_ptr,
               const db_condition_t* conditions, size_t count) {
    (void)table;
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
                                 size_t order_field_offset, int ascending __attribute__((unused)),
                                 size_t limit_offset, size_t limit_count,
                                 size_t* out_count) {
    if (!table) {
        *out_count = 0;
        return NULL;
    }
    
    // 尝试使用索引（如果有的话）
    if (condition_count > 0) {
        db_index_t* idx = find_index(table, conditions[0].field_offset);
        if (idx && conditions[0].op == 0) {  // 等值查询
            size_t index_count = 0;
            rowid_t* index_results = query_with_index(table, idx, &conditions[0], &index_count);
            if (index_results && index_count > 0) {
                // 用索引结果过滤其余条件
                rowid_t* filtered = (rowid_t*)malloc(sizeof(rowid_t) * index_count);
                size_t filtered_count = 0;
                
                for (size_t i = 0; i < index_count; i++) {
                    size_t offset = index_results[i] * table->row_stride;
                    row_header_t* header = (row_header_t*)PTR(table->data_pool.base, offset);
                    if (header->flags & MYDB_DELETED_FLAG) continue;
                    
                    void* row_ptr = (char*)header + sizeof(row_header_t);
                    if (row_match(table, row_ptr, conditions, condition_count)) {
                        filtered[filtered_count++] = index_results[i];
                    }
                }
                
                free(index_results);
                
                // max_rows 限制检查
                if (table->db && filtered_count > table->db->max_rows) {
                    free(filtered);
                    *out_count = 0;
                    db_set_error(table->db, DB_ERR_RESULT_TOO_LARGE,
                                 "query returned %zu rows, exceeds max_rows=%zu",
                                 filtered_count, table->db->max_rows);
                    return NULL;
                }
                
                *out_count = filtered_count;
                return filtered;
            }
            if (index_results) free(index_results);
        }
    }
    
    // 回退到全表扫描
    rowid_t* results = (rowid_t*)malloc(sizeof(rowid_t) * table->row_count);
    if (!results) {
        *out_count = 0;
        return NULL;
    }
    
    size_t match_count = 0;
    for (rowid_t id = 1; id <= table->max_rowid; id++) {
        size_t offset = id * table->row_stride;
        row_header_t* header = (row_header_t*)PTR(table->data_pool.base, offset);
        
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
                    size_t offset = results[i] * table->row_stride;
                    row_header_t* header = (row_header_t*)PTR(table->data_pool.base, offset);
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
    
    // max_rows 限制检查
    if (table->db && match_count > table->db->max_rows) {
        free(results);
        *out_count = 0;
        db_set_error(table->db, DB_ERR_RESULT_TOO_LARGE, 
                     "query returned %zu rows, exceeds max_rows=%zu", 
                     match_count, table->db->max_rows);
        return NULL;
    }
    
    *out_count = match_count;
    return results;
}

const char* db_select_by_pk_json(table_t table, rowid_t id) {
    if (!table || id == 0) return NULL;
    db_table_t* t = (db_table_t*)table;
    
    if (id > t->max_rowid) return NULL;
    
    size_t offset = id * t->row_stride;
    row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
    
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
    // 嵌套循环等值 JOIN
    if (!left_table || !right_table) return NULL;
    
    db_table_t* left = (db_table_t*)left_table;
    db_table_t* right = (db_table_t*)right_table;
    
    // 预估结果大小
    size_t est_size = 2; // []
    size_t join_count = 0;
    
    // 先计算大小
    for (rowid_t lid = 1; lid <= left->max_rowid; lid++) {
        size_t loff = lid * left->row_stride;
        row_header_t* lheader = (row_header_t*)PTR(left->data_pool.base, loff);
        if (lheader->flags & MYDB_DELETED_FLAG) continue;
        
        void* lval = (char*)lheader + sizeof(row_header_t) + left_field_offset;
        
        for (rowid_t rid = 1; rid <= right->max_rowid; rid++) {
            size_t roff = rid * right->row_stride;
            row_header_t* rheader = (row_header_t*)PTR(right->data_pool.base, roff);
            if (rheader->flags & MYDB_DELETED_FLAG) continue;
            
            void* rval = (char*)rheader + sizeof(row_header_t) + right_field_offset;
            
            if (memcmp(lval, rval, field_size) == 0) {
                join_count++;
                est_size += 1024; // 预估每行大小
            }
        }
    }
    
    if (join_count == 0) return strdup("[]");
    
    char* buf = (char*)malloc(est_size);
    if (!buf) return strdup("[]");
    
    size_t offset = 0;
    offset += sprintf(buf + offset, "[");
    
    bool first = true;
    for (rowid_t lid = 1; lid <= left->max_rowid; lid++) {
        size_t loff = lid * left->row_stride;
        row_header_t* lheader = (row_header_t*)PTR(left->data_pool.base, loff);
        if (lheader->flags & MYDB_DELETED_FLAG) continue;
        
        void* lval = (char*)lheader + sizeof(row_header_t) + left_field_offset;
        void* lrow = (char*)lheader + sizeof(row_header_t);
        
        for (rowid_t rid = 1; rid <= right->max_rowid; rid++) {
            size_t roff = rid * right->row_stride;
            row_header_t* rheader = (row_header_t*)PTR(right->data_pool.base, roff);
            if (rheader->flags & MYDB_DELETED_FLAG) continue;
            
            void* rval = (char*)rheader + sizeof(row_header_t) + right_field_offset;
            
            if (memcmp(lval, rval, field_size) == 0) {
                if (!first) offset += sprintf(buf + offset, ",");
                first = false;
                
                void* rrow = (char*)rheader + sizeof(row_header_t);
                
                offset += sprintf(buf + offset, "{");
                
                // 左表字段
                for (size_t i = 0; i < left->field_count; i++) {
                    if (i > 0) offset += sprintf(buf + offset, ",");
                    db_field_def_t* f = &left->fields[i];
                    void* fp = (char*)lrow + f->offset;
                    
                    offset += sprintf(buf + offset, "\"%s.%s\":", left->name, f->name);
                    
                    switch (f->type) {
                        case DB_TYPE_INT32:
                            offset += sprintf(buf + offset, "%d", *(int32_t*)fp);
                            break;
                        case DB_TYPE_UINT64:
                            offset += sprintf(buf + offset, "%lu", (unsigned long)*(uint64_t*)fp);
                            break;
                        case DB_TYPE_STRING: {
                            size_t slen = f->size;
                            while (slen > 0 && ((char*)fp)[slen-1] == '\0') slen--;
                            if (slen == 0) {
                                offset += sprintf(buf + offset, "\"\"");
                            } else {
                                offset += sprintf(buf + offset, "\"");
                                for (size_t k = 0; k < slen; k++) {
                                    if (((char*)fp)[k] == '"' || ((char*)fp)[k] == '\\') {
                                        offset += sprintf(buf + offset, "\\");
                                    }
                                    offset += sprintf(buf + offset, "%c", ((char*)fp)[k]);
                                }
                                offset += sprintf(buf + offset, "\"");
                            }
                            break;
                        }
                        case DB_TYPE_DOUBLE:
                            offset += sprintf(buf + offset, "%.6f", *(double*)fp);
                            break;
                        default:
                            offset += sprintf(buf + offset, "null");
                    }
                }
                
                // 右表字段
                for (size_t i = 0; i < right->field_count; i++) {
                    offset += sprintf(buf + offset, ",");
                    db_field_def_t* f = &right->fields[i];
                    void* fp = (char*)rrow + f->offset;
                    
                    offset += sprintf(buf + offset, "\"%s.%s\":", right->name, f->name);
                    
                    switch (f->type) {
                        case DB_TYPE_INT32:
                            offset += sprintf(buf + offset, "%d", *(int32_t*)fp);
                            break;
                        case DB_TYPE_UINT64:
                            offset += sprintf(buf + offset, "%lu", (unsigned long)*(uint64_t*)fp);
                            break;
                        case DB_TYPE_STRING: {
                            size_t slen = f->size;
                            while (slen > 0 && ((char*)fp)[slen-1] == '\0') slen--;
                            if (slen == 0) {
                                offset += sprintf(buf + offset, "\"\"");
                            } else {
                                offset += sprintf(buf + offset, "\"");
                                for (size_t k = 0; k < slen; k++) {
                                    if (((char*)fp)[k] == '"' || ((char*)fp)[k] == '\\') {
                                        offset += sprintf(buf + offset, "\\");
                                    }
                                    offset += sprintf(buf + offset, "%c", ((char*)fp)[k]);
                                }
                                offset += sprintf(buf + offset, "\"");
                            }
                            break;
                        }
                        case DB_TYPE_DOUBLE:
                            offset += sprintf(buf + offset, "%.6f", *(double*)fp);
                            break;
                        default:
                            offset += sprintf(buf + offset, "null");
                    }
                }
                
                offset += sprintf(buf + offset, "}");
            }
        }
    }
    
    offset += sprintf(buf + offset, "]");
    
    return buf;
}

void db_json_free(const char* json) {
    free((void*)json);
}

int db_select_all_stream(table_t table, db_row_cb_t cb, void* user_data) {
    if (!table || !cb) return -1;
    db_table_t* t = (db_table_t*)table;
    
    for (rowid_t id = 1; id <= t->max_rowid; id++) {
        size_t offset = id * t->row_stride;
        row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
        
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
        size_t offset = id * t->row_stride;
        row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
        
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
        size_t offset = results[i] * t->row_stride;
        row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
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
