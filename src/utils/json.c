#include "mydb_internal.h"
#include <stdio.h>
#include <stdlib.h>

static char* json_escape_string(const char* str, size_t len) {
    // 计算需要的空间
    size_t need = 2; // 引号
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '\n': case '\r': case '\t': case '\\': case '"':
                need += 2;
                break;
            default:
                if ((unsigned char)str[i] < 0x20) need += 6;
                else need++;
        }
    }
    
    char* out = (char*)malloc(need + 1);
    if (!out) return NULL;
    
    size_t j = 0;
    out[j++] = '"';
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '\n': out[j++] = '\\'; out[j++] = 'n'; break;
            case '\r': out[j++] = '\\'; out[j++] = 'r'; break;
            case '\t': out[j++] = '\\'; out[j++] = 't'; break;
            case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
            case '"': out[j++] = '\\'; out[j++] = '"'; break;
            default:
                if ((unsigned char)str[i] < 0x20) {
                    sprintf(out + j, "\\u%04x", (unsigned char)str[i]);
                    j += 6;
                } else {
                    out[j++] = str[i];
                }
        }
    }
    out[j++] = '"';
    out[j] = '\0';
    
    return out;
}

char* json_row(db_table_t* table, void* row_ptr) {
    if (!table || !row_ptr) return NULL;
    
    // 预估大小
    size_t est_size = 256;
    for (size_t i = 0; i < table->field_count; i++) {
        est_size += 64 + table->fields[i].size * 2;
    }
    
    char* buf = (char*)malloc(est_size);
    if (!buf) return NULL;
    
    size_t offset = 0;
    offset += sprintf(buf + offset, "{");
    
    for (size_t i = 0; i < table->field_count; i++) {
        db_field_def_t* field = &table->fields[i];
        void* field_ptr = (char*)row_ptr + field->offset;
        
        if (i > 0) offset += sprintf(buf + offset, ",");
        offset += sprintf(buf + offset, "\"%s\":", field->name);
        
        switch (field->type) {
            case DB_TYPE_INT32:
                offset += sprintf(buf + offset, "%d", *(int32_t*)field_ptr);
                break;
            case DB_TYPE_INT64:
                offset += sprintf(buf + offset, "%ld", (long)*(int64_t*)field_ptr);
                break;
            case DB_TYPE_UINT64:
                offset += sprintf(buf + offset, "%lu", (unsigned long)*(uint64_t*)field_ptr);
                break;
            case DB_TYPE_FLOAT:
                offset += sprintf(buf + offset, "%.6f", *(float*)field_ptr);
                break;
            case DB_TYPE_DOUBLE:
                offset += sprintf(buf + offset, "%.6f", *(double*)field_ptr);
                break;
            case DB_TYPE_BOOL:
                offset += sprintf(buf + offset, "%s", *(uint8_t*)field_ptr ? "true" : "false");
                break;
            case DB_TYPE_STRING: {
                // 找到实际字符串长度（第一个 \0 之前）
                size_t str_len = 0;
                while (str_len < field->size && ((char*)field_ptr)[str_len] != '\0') str_len++;
                if (str_len == 0) {
                    offset += sprintf(buf + offset, "\"\"");
                } else {
                    char* escaped = json_escape_string((char*)field_ptr, str_len);
                    if (escaped) {
                        offset += sprintf(buf + offset, "%s", escaped);
                        free(escaped);
                    }
                }
                break;
            }
            default:
                offset += sprintf(buf + offset, "null");
                break;
        }
    }
    
    offset += sprintf(buf + offset, "}");
    
    return buf;
}

char* json_rows(db_table_t* table, rowid_t* rowids, size_t count) {
    if (!table || !rowids) return strdup("[]");
    
    // 先计算需要的大小
    size_t total_size = 2; // []
    for (size_t i = 0; i < count; i++) {
        size_t offset = table->data_offset + (rowids[i] - 1) * (sizeof(row_header_t) + table->row_size);
        row_header_t* header = (row_header_t*)PTR(table->data_pool->base, offset);
        if (header->flags & MYDB_DELETED_FLAG) continue;
        
        void* row_ptr = (char*)header + sizeof(row_header_t);
        char* row_json = json_row(table, row_ptr);
        if (row_json) {
            total_size += strlen(row_json) + 2; // + 逗号和空格
            free(row_json);
        }
    }
    
    char* buf = (char*)malloc(total_size + 1);
    if (!buf) return strdup("[]");
    
    size_t off = 0;
    off += sprintf(buf + off, "[");
    
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        size_t offset = table->data_offset + (rowids[i] - 1) * (sizeof(row_header_t) + table->row_size);
        row_header_t* header = (row_header_t*)PTR(table->data_pool->base, offset);
        if (header->flags & MYDB_DELETED_FLAG) continue;
        
        if (!first) off += sprintf(buf + off, ",");
        first = false;
        
        void* row_ptr = (char*)header + sizeof(row_header_t);
        char* row_json = json_row(table, row_ptr);
        if (row_json) {
            off += sprintf(buf + off, "%s", row_json);
            free(row_json);
        }
    }
    
    off += sprintf(buf + off, "]");
    
    return buf;
}

char* json_join_rows(db_table_t* left, db_table_t* right,
                     void** left_rows, void** right_rows, size_t count) {
    // 简化实现
    return strdup("[]");
}
