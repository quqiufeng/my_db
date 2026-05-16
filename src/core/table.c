#include "mydb_internal.h"

table_t db_table_register(db_t db, const char* name, size_t row_size,
                          const db_field_def_t* fields, size_t field_count) {
    if (!db || !name || row_size == 0 || !fields || field_count == 0) return NULL;
    db_instance_t* inst = (db_instance_t*)db;
    
    if (inst->table_count >= MYDB_MAX_TABLES) {
        db_set_error(inst, DB_ERR_NOMEM, "too many tables");
        return NULL;
    }
    
    // 检查表名是否已存在
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i] && strcmp(inst->tables[i]->name, name) == 0) {
            db_set_error(inst, DB_ERR_EXIST, "table '%s' already exists", name);
            return NULL;
        }
    }
    
    db_table_t* table = (db_table_t*)calloc(1, sizeof(db_table_t));
    if (!table) {
        db_set_error(inst, DB_ERR_NOMEM, "out of memory");
        return NULL;
    }
    
    strncpy(table->name, name, MYDB_TABLE_NAME_LEN - 1);
    table->row_size = row_size;
    table->row_stride = ((sizeof(row_header_t) + row_size + MYDB_ALIGN - 1) / MYDB_ALIGN) * MYDB_ALIGN;
    table->data_pool = &inst->data_pool;
    table->db = inst;
    
    // 复制字段定义（深拷贝字段名）
    table->fields = (db_field_def_t*)malloc(sizeof(db_field_def_t) * field_count);
    if (!table->fields) {
        free(table);
        return NULL;
    }
    for (size_t i = 0; i < field_count; i++) {
        table->fields[i] = fields[i];
        // 深拷贝字段名
        if (fields[i].name) {
            size_t name_len = strlen(fields[i].name) + 1;
            table->fields[i].name = (char*)malloc(name_len);
            if (!table->fields[i].name) {
                // 清理已分配的内存
                for (size_t j = 0; j < i; j++) {
                    free((void*)table->fields[j].name);
                }
                free(table->fields);
                free(table);
                return NULL;
            }
            memcpy((char*)table->fields[i].name, fields[i].name, name_len);
        }
    }
    table->field_count = field_count;
    
    // 分配数据区（每张表独立区域）
    size_t initial_rows = 16;
    size_t reserve_size = initial_rows * table->row_stride;
    void* reserved = pool_alloc(&inst->data_pool, reserve_size);
    if (!reserved) {
        free(table->fields);
        free(table);
        return NULL;
    }
    table->data_offset = OFF(inst->data_pool.base, reserved);
    
    inst->tables[inst->table_count++] = table;
    
    // 自动为主键创建索引（第一字段是 uint64_t 且 offset 为 0）
    if (field_count > 0 && fields[0].type == DB_TYPE_UINT64 && fields[0].offset == 0) {
        index_create(table, "id", fields[0].offset, fields[0].type);
    }
    
    return table;
}

table_t db_table(db_t db, const char* name) {
    if (!db || !name) return NULL;
    db_instance_t* inst = (db_instance_t*)db;
    
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i] && strcmp(inst->tables[i]->name, name) == 0) {
            return inst->tables[i];
        }
    }
    
    return NULL;
}

static void free_table_fields(db_table_t* table) {
    if (!table || !table->fields) return;
    for (size_t i = 0; i < table->field_count; i++) {
        free((void*)table->fields[i].name);
    }
    free(table->fields);
    table->fields = NULL;
}

int db_table_drop(db_t db, const char* name) {
    if (!db || !name) return -1;
    db_instance_t* inst = (db_instance_t*)db;
    
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i] && strcmp(inst->tables[i]->name, name) == 0) {
            free_table_fields(inst->tables[i]);
            free(inst->tables[i]);
            // 移动后续表
            for (size_t j = i; j < inst->table_count - 1; j++) {
                inst->tables[j] = inst->tables[j + 1];
            }
            inst->table_count--;
            return DB_OK;
        }
    }
    
    return DB_ERR_NOENT;
}

size_t db_table_count(table_t table) {
    if (!table) return 0;
    db_table_t* t = (db_table_t*)table;
    return t->row_count;
}

size_t db_table_compact(table_t table) {
    if (!table) return 0;
    db_table_t* t = (db_table_t*)table;
    
    // 简化实现：统计已删除行数
    size_t deleted = 0;
    for (size_t i = 0; i < t->max_rowid; i++) {
        row_header_t* header = (row_header_t*)PTR(t->data_pool->base, t->data_offset + i * t->row_stride);
        if (header->flags & MYDB_DELETED_FLAG) {
            deleted++;
        }
    }
    
    // 完整实现需要重建数据区并更新索引
    // 简化版：仅统计，实际重建后续实现
    
    return deleted;
}

rowid_t db_insert(table_t table, const void* row, size_t row_size) {
    if (!table || !row) return 0;
    db_table_t* t = (db_table_t*)table;
    
    if (row_size != t->row_size) return 0;
    
    rowid_t id = ++t->max_rowid;
    
    // 确保数据区足够大
    size_t need_offset = t->data_offset + id * t->row_stride;
    if (need_offset > t->data_pool->size) {
        if (pool_resize(t->data_pool, need_offset + (1024 * 1024)) < 0) {
            return 0;
        }
    }
    
    // 计算行位置
    size_t row_offset = t->data_offset + (id - 1) * t->row_stride;
    void* ptr = (char*)t->data_pool->base + row_offset;
    
    // 写入 header
    row_header_t* header = (row_header_t*)ptr;
    header->rowid = id;
    header->flags = 0;
    
    // 写入数据
    memcpy((char*)ptr + sizeof(row_header_t), row, row_size);
    
    // 自动填充 id 到用户数据的第一个字段
    if (t->field_count > 0 && t->fields[0].type == DB_TYPE_UINT64 && t->fields[0].offset == 0) {
        *(uint64_t*)((char*)ptr + sizeof(row_header_t)) = id;
    }
    
    // 更新计数
    t->row_count++;
    
    // 更新池已用大小
    if (need_offset > t->data_pool->used) {
        t->data_pool->used = need_offset;
    }
    
    // 更新索引
    void* row_ptr = (char*)ptr + sizeof(row_header_t);
    index_insert(t, id, row_ptr);
    
    // 写 WAL
    if (t->db) {
        wal_append(&t->db->wal, 1, t->name, row_ptr, row_size, id);
    }
    
    return id;
}

size_t db_batch_insert(table_t table, const void* rows, size_t row_size, size_t count) {
    if (!table || !rows || count == 0) return 0;
    
    size_t inserted = 0;
    const char* ptr = (const char*)rows;
    
    for (size_t i = 0; i < count; i++) {
        if (db_insert(table, ptr + i * row_size, row_size) > 0) {
            inserted++;
        }
    }
    
    return inserted;
}

int db_update(table_t table, rowid_t id, const void* row, size_t row_size) {
    if (!table || !row || id == 0) return DB_ERR_INVAL;
    db_table_t* t = (db_table_t*)table;
    
    if (id > t->max_rowid) return DB_ERR_NOENT;
    
    size_t offset = t->data_offset + (id - 1) * t->row_stride;
    row_header_t* header = (row_header_t*)PTR(t->data_pool->base, offset);
    
    if (header->flags & MYDB_DELETED_FLAG) return DB_ERR_NOENT;
    
    void* old_row = (char*)header + sizeof(row_header_t);
    
    // 删除旧索引
    index_delete(t, id, old_row);
    
    // 更新数据
    memcpy(old_row, row, row_size);
    
    // 插入新索引
    index_insert(t, id, old_row);
    
    // 写 WAL
    if (t->db) {
        wal_append(&t->db->wal, 2, t->name, old_row, t->row_size, id);
    }
    
    return DB_OK;
}

int db_delete(table_t table, rowid_t id) {
    if (!table || id == 0) return DB_ERR_INVAL;
    db_table_t* t = (db_table_t*)table;
    
    if (id > t->max_rowid) return DB_ERR_NOENT;
    
    size_t offset = t->data_offset + (id - 1) * t->row_stride;
    row_header_t* header = (row_header_t*)PTR(t->data_pool->base, offset);
    
    if (header->flags & MYDB_DELETED_FLAG) return DB_ERR_NOENT;
    
    // 删除索引
    void* row_ptr = (char*)header + sizeof(row_header_t);
    index_delete(t, id, row_ptr);
    
    // 写 WAL
    if (t->db) {
        wal_append(&t->db->wal, 3, t->name, NULL, 0, id);
    }
    
    header->flags |= MYDB_DELETED_FLAG;
    t->row_count--;
    
    return DB_OK;
}

int db_table_add_index(table_t table, const char* field_name, size_t field_offset, int field_type) {
    if (!table || !field_name) return DB_ERR_INVAL;
    return index_create((db_table_t*)table, field_name, field_offset, field_type);
}

int db_table_add_index_composite(table_t table, db_field_def_t* fields, size_t field_count) {
    if (!table || !fields || field_count == 0 || field_count > MYDB_MAX_INDEX_FIELDS) {
        return DB_ERR_INVAL;
    }
    
    db_table_t* t = (db_table_t*)table;
    
    db_index_t* index = (db_index_t*)calloc(1, sizeof(db_index_t));
    if (!index) return DB_ERR_NOMEM;
    
    // 生成索引名: field1_field2_...
    char name[MYDB_TABLE_NAME_LEN] = {0};
    size_t name_len = 0;
    for (size_t i = 0; i < field_count && name_len < MYDB_TABLE_NAME_LEN - 1; i++) {
        if (i > 0 && name_len < MYDB_TABLE_NAME_LEN - 1) {
            name[name_len++] = '_';
        }
        size_t flen = strlen(fields[i].name);
        if (name_len + flen >= MYDB_TABLE_NAME_LEN) {
            flen = MYDB_TABLE_NAME_LEN - name_len - 1;
        }
        memcpy(name + name_len, fields[i].name, flen);
        name_len += flen;
    }
    name[name_len] = '\0';
    memcpy(index->name, name, name_len + 1);
    
    index->field_count = (int)field_count;
    for (size_t i = 0; i < field_count && i < MYDB_MAX_INDEX_FIELDS; i++) {
        index->field_offsets[i] = fields[i].offset;
        index->field_sizes[i] = fields[i].size;
        index->field_types[i] = fields[i].type;
    }
    
    // 复合索引用 B+树（支持范围查询和排序）
    index->type = INDEX_BTREE;
    
    // 计算复合键的最大大小
    size_t key_size = 0;
    for (size_t i = 0; i < field_count && i < MYDB_MAX_INDEX_FIELDS; i++) {
        key_size += fields[i].size;
    }
    
    index->data = btree_create(key_size, fields[0].type);
    if (!index->data) {
        free(index);
        return DB_ERR_NOMEM;
    }
    
    // 添加到索引链表
    index->next = t->indexes;
    t->indexes = index;
    
    // 为已有数据建立索引
    for (rowid_t id = 1; id <= t->max_rowid; id++) {
        size_t offset = t->data_offset + (id - 1) * t->row_stride;
        row_header_t* header = (row_header_t*)PTR(t->data_pool->base, offset);
        if (header->flags & MYDB_DELETED_FLAG) continue;
        
        void* row_ptr = (char*)header + sizeof(row_header_t);
        index_insert(t, id, row_ptr);
    }
    
    return DB_OK;
}
