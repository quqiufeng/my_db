#include "mydb_internal.h"
#include <stdio.h>
#include <stdlib.h>

// 动态 JSON buffer，避免重复 realloc
typedef struct {
    char*  data;
    size_t offset;
    size_t capacity;
} json_buf_t;

static int json_buf_ensure(json_buf_t* buf, size_t need) {
    if (buf->offset + need <= buf->capacity) return 0;
    size_t new_cap = buf->capacity * 2;
    if (new_cap < buf->offset + need + 256) new_cap = buf->offset + need + 256;
    char* new_data = realloc(buf->data, new_cap);
    if (!new_data) return -1;
    buf->data = new_data;
    buf->capacity = new_cap;
    return 0;
}

static int json_buf_append(json_buf_t* buf, const char* str) {
    size_t len = strlen(str);
    if (json_buf_ensure(buf, len + 1) < 0) return -1;
    memcpy(buf->data + buf->offset, str, len + 1);
    buf->offset += len;
    return 0;
}

static int json_buf_append_escaped(json_buf_t* buf, const char* str, size_t len) {
    // 计算转义后长度
    size_t need = 2; // 引号
    for (size_t i = 0; i < len; i++) {
        switch (str[i]) {
            case '\n': case '\r': case '\t': case '\\': case '"':
                need += 2; break;
            default:
                if ((unsigned char)str[i] < 0x20) need += 6;
                else need++;
        }
    }
    
    if (json_buf_ensure(buf, need + 1) < 0) return -1;
    
    char* out = buf->data + buf->offset;
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
    buf->offset += j;
    buf->data[buf->offset] = '\0';
    return 0;
}

char* json_row(db_table_t* table, void* row_ptr) {
    if (!table || !row_ptr) return NULL;
    
    json_buf_t buf = {malloc(256), 0, 256};
    if (!buf.data) return NULL;
    
    buf.data[0] = '{';
    buf.offset = 1;
    
    for (size_t i = 0; i < table->field_count; i++) {
        db_field_def_t* field = &table->fields[i];
        void* field_ptr = (char*)row_ptr + field->offset;
        
        if (i > 0) {
            if (json_buf_ensure(&buf, 2) < 0) goto fail;
            buf.data[buf.offset++] = ',';
        }
        
        // 字段名
        size_t name_len = strlen(field->name);
        if (json_buf_ensure(&buf, name_len + 4) < 0) goto fail;
        buf.data[buf.offset++] = '"';
        memcpy(buf.data + buf.offset, field->name, name_len);
        buf.offset += name_len;
        buf.data[buf.offset++] = '"';
        buf.data[buf.offset++] = ':';
        buf.data[buf.offset] = '\0';
        
        switch (field->type) {
            case DB_TYPE_INT32: {
                char tmp[32];
                int n = snprintf(tmp, sizeof(tmp), "%d", *(int32_t*)field_ptr);
                if (json_buf_ensure(&buf, n + 1) < 0) goto fail;
                memcpy(buf.data + buf.offset, tmp, n + 1);
                buf.offset += n;
                break;
            }
            case DB_TYPE_INT64: {
                char tmp[32];
                int n = snprintf(tmp, sizeof(tmp), "%ld", (long)*(int64_t*)field_ptr);
                if (json_buf_ensure(&buf, n + 1) < 0) goto fail;
                memcpy(buf.data + buf.offset, tmp, n + 1);
                buf.offset += n;
                break;
            }
            case DB_TYPE_UINT64: {
                char tmp[32];
                int n = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)*(uint64_t*)field_ptr);
                if (json_buf_ensure(&buf, n + 1) < 0) goto fail;
                memcpy(buf.data + buf.offset, tmp, n + 1);
                buf.offset += n;
                break;
            }
            case DB_TYPE_FLOAT: {
                char tmp[48];
                int n = snprintf(tmp, sizeof(tmp), "%.6f", *(float*)field_ptr);
                if (json_buf_ensure(&buf, n + 1) < 0) goto fail;
                memcpy(buf.data + buf.offset, tmp, n + 1);
                buf.offset += n;
                break;
            }
            case DB_TYPE_DOUBLE: {
                char tmp[48];
                int n = snprintf(tmp, sizeof(tmp), "%.6f", *(double*)field_ptr);
                if (json_buf_ensure(&buf, n + 1) < 0) goto fail;
                memcpy(buf.data + buf.offset, tmp, n + 1);
                buf.offset += n;
                break;
            }
            case DB_TYPE_BOOL: {
                const char* v = *(uint8_t*)field_ptr ? "true" : "false";
                if (json_buf_append(&buf, v) < 0) goto fail;
                break;
            }
            case DB_TYPE_STRING: {
                size_t str_len = 0;
                while (str_len < field->size && ((char*)field_ptr)[str_len] != '\0') str_len++;
                if (json_buf_append_escaped(&buf, (char*)field_ptr, str_len) < 0) goto fail;
                break;
            }
            case DB_TYPE_VARSTRING: {
                db_string_ref_t* ref = (db_string_ref_t*)field_ptr;
                if (ref->length == 0 || ref->offset == 0) {
                    if (json_buf_append(&buf, "\"\"") < 0) goto fail;
                } else {
                    char* str = (char*)table->string_pool.base + ref->offset;
                    if (json_buf_append_escaped(&buf, str, ref->length) < 0) goto fail;
                }
                break;
            }
            default:
                if (json_buf_append(&buf, "null") < 0) goto fail;
                break;
        }
    }
    
    if (json_buf_ensure(&buf, 2) < 0) goto fail;
    buf.data[buf.offset++] = '}';
    buf.data[buf.offset] = '\0';
    
    // 收缩到实际大小
    char* result = realloc(buf.data, buf.offset + 1);
    return result ? result : buf.data;
    
fail:
    free(buf.data);
    return NULL;
}

char* json_rows(db_table_t* table, rowid_t* rowids, size_t count) {
    if (!table || !rowids) return strdup("[]");
    
    // 先计算需要的大小
    size_t total_size = 2; // []
    for (size_t i = 0; i < count; i++) {
        size_t offset = rowids[i] * table->row_stride;
        row_header_t* header = (row_header_t*)PTR(table->data_pool.base, offset);
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
        size_t offset = rowids[i] * table->row_stride;
        row_header_t* header = (row_header_t*)PTR(table->data_pool.base, offset);
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

char* json_join_rows(db_table_t* left __attribute__((unused)), db_table_t* right __attribute__((unused)),
                     void** left_rows __attribute__((unused)), void** right_rows __attribute__((unused)),
                     size_t count __attribute__((unused))) {
    // 简化实现
    return strdup("[]");
}
