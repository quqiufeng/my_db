#include "mydb_internal.h"

// 构建路径：dir/name.ext
static void build_path(char* out, size_t out_size, const char* dir, const char* name, const char* ext) {
    snprintf(out, out_size, "%s/%s%s", dir, name, ext);
}

// ====== 索引持久化 ======
// 格式：[index_count:4][index_defs...]
// 每个索引：[type:4][field_count:4][fields...]

static void serialize_uint32(uint8_t** p, uint32_t v) {
    memcpy(*p, &v, 4); *p += 4;
}
static void serialize_uint64(uint8_t** p, uint64_t v) {
    memcpy(*p, &v, 8); *p += 8;
}
static uint32_t deserialize_uint32(uint8_t** p) {
    uint32_t v; memcpy(&v, *p, 4); *p += 4; return v;
}
static uint64_t deserialize_uint64(uint8_t** p) {
    uint64_t v; memcpy(&v, *p, 8); *p += 8; return v;
}

void save_index_defs(db_table_t* t) {
    if (!t || !t->indexes) return;
    
    // 计算需要的大小
    size_t size = 4; // index_count
    int count = 0;
    db_index_t* idx = t->indexes;
    while (idx) {
        count++;
        size += 4 + 4 + 8; // type + field_count + data_offset
        for (int i = 0; i < idx->field_count && i < MYDB_MAX_INDEX_FIELDS; i++) {
            size += 8 + 8 + 4 + 4 + strlen(idx->name) + 1; // offset + size + type + name_len + name
        }
        idx = idx->next;
    }
    
    // 确保 meta_pool 足够大
    if (size + 16 > t->index_meta_pool.size) {
        pool_resize(&t->index_meta_pool, size + 16 + 1024);
    }
    
    uint8_t* p = (uint8_t*)t->index_meta_pool.base + 16; // 跳过文件头
    serialize_uint32(&p, count);
    
    idx = t->indexes;
    while (idx) {
        serialize_uint32(&p, (uint32_t)idx->type);
        serialize_uint32(&p, (uint32_t)idx->field_count);
        serialize_uint64(&p, idx->data_offset);  // 保存 data_offset
        for (int i = 0; i < idx->field_count && i < MYDB_MAX_INDEX_FIELDS; i++) {
            serialize_uint64(&p, idx->field_offsets[i]);
            serialize_uint64(&p, idx->field_sizes[i]);
            serialize_uint32(&p, (uint32_t)idx->field_types[i]);
            uint32_t name_len = strlen(idx->name) + 1;
            serialize_uint32(&p, name_len);
            memcpy(p, idx->name, name_len);
            p += name_len;
        }
        idx = idx->next;
    }
    
    // 更新 used
    t->index_meta_pool.used = (p - (uint8_t*)t->index_meta_pool.base);
    *(size_t*)((char*)t->index_meta_pool.base + 8) = t->index_meta_pool.used;
}

static void load_index_defs(db_table_t* t) {
    if (!t || t->index_meta_pool.used <= 16) return;
    
    uint8_t* p = (uint8_t*)t->index_meta_pool.base + 16;
    uint32_t count = deserialize_uint32(&p);
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = deserialize_uint32(&p);
        uint32_t field_count = deserialize_uint32(&p);
        size_t data_offset = deserialize_uint64(&p);  // 读取 data_offset
        if (field_count == 0 || field_count > MYDB_MAX_INDEX_FIELDS) break;
        
        size_t offsets[MYDB_MAX_INDEX_FIELDS];
        size_t sizes[MYDB_MAX_INDEX_FIELDS];
        int types[MYDB_MAX_INDEX_FIELDS];
        char name[MYDB_TABLE_NAME_LEN] = {0};
        
        for (uint32_t j = 0; j < field_count; j++) {
            offsets[j] = deserialize_uint64(&p);
            sizes[j] = deserialize_uint64(&p);
            types[j] = (int)deserialize_uint32(&p);
            uint32_t name_len = deserialize_uint32(&p);
            if (name_len > 0 && name_len < MYDB_TABLE_NAME_LEN) {
                memcpy(name, p, name_len);
                p += name_len;
            }
        }
        
        // 创建索引（直接恢复，不创建新的 hash/btree）
        db_index_t* index = (db_index_t*)calloc(1, sizeof(db_index_t));
        if (!index) continue;
        
        snprintf(index->name, MYDB_TABLE_NAME_LEN, "%s", name);
        index->type = (index_type_t)type;
        index->field_count = (int)field_count;
        for (uint32_t j = 0; j < field_count && j < MYDB_MAX_INDEX_FIELDS; j++) {
            index->field_offsets[j] = offsets[j];
            index->field_sizes[j] = sizes[j];
            index->field_types[j] = types[j];
        }
        index->data_offset = data_offset;
        
        index->next = t->indexes;
        t->indexes = index;
    }
}

// ====== 表元数据持久化 ======
// 存储在 data_pool 的 offset 16 开始处（紧接文件头）
// 格式：[row_count:8][max_rowid:8][deleted_count:8]

#define TABLE_META_OFFSET 16

static void save_table_meta(db_table_t* t) {
    if (!t || !t->data_pool.base) return;
    uint8_t* p = (uint8_t*)t->data_pool.base + TABLE_META_OFFSET;
    *(size_t*)(p + 0) = t->row_count;
    *(size_t*)(p + 8) = t->max_rowid;
    *(size_t*)(p + 16) = t->deleted_count;
}

static void load_table_meta(db_table_t* t) {
    if (!t || !t->data_pool.base) return;
    uint8_t* p = (uint8_t*)t->data_pool.base + TABLE_META_OFFSET;
    t->row_count = *(size_t*)(p + 0);
    t->max_rowid = *(size_t*)(p + 8);
    t->deleted_count = *(size_t*)(p + 16);
}

// Free list 操作
static void free_list_push(db_table_t* t, rowid_t id) {
    if (!t->free_list) {
        t->free_capacity = 16;
        t->free_list = (rowid_t*)malloc(sizeof(rowid_t) * t->free_capacity);
    }
    if (t->free_count >= t->free_capacity) {
        t->free_capacity *= 2;
        t->free_list = (rowid_t*)realloc(t->free_list, sizeof(rowid_t) * t->free_capacity);
    }
    t->free_list[t->free_count++] = id;
}

static rowid_t free_list_pop(db_table_t* t) {
    if (!t->free_list || t->free_count == 0) return 0;
    return t->free_list[--t->free_count];
}

void free_list_destroy(db_table_t* t) {
    if (t->free_list) {
        free(t->free_list);
        t->free_list = NULL;
        t->free_count = 0;
        t->free_capacity = 0;
    }
}

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
    table->db = inst;
    table->compact_threshold = 0.3f; // 默认 30% 删除率触发 compact
    
    // 复制字段定义（深拷贝字段名，变长字符串字段修改 size）
    table->fields = (db_field_def_t*)malloc(sizeof(db_field_def_t) * field_count);
    if (!table->fields) {
        free(table);
        return NULL;
    }
    for (size_t i = 0; i < field_count; i++) {
        table->fields[i] = fields[i];
        if (fields[i].name) {
            size_t name_len = strlen(fields[i].name) + 1;
            table->fields[i].name = (char*)malloc(name_len);
            if (!table->fields[i].name) {
                for (size_t j = 0; j < i; j++) {
                    free((void*)table->fields[j].name);
                }
                free(table->fields);
                free(table);
                return NULL;
            }
            memcpy((char*)table->fields[i].name, fields[i].name, name_len);
        }
        // 变长字符串字段：size 改为 db_string_ref_t 大小
        if (fields[i].type == DB_TYPE_VARSTRING) {
            table->fields[i].size = sizeof(db_string_ref_t);
        }
    }
    table->field_count = field_count;
    
    // 创建表级数据文件：db_dir/name.bin
    char data_path[MYDB_TABLE_NAME_LEN * 2];
    build_path(data_path, sizeof(data_path), inst->db_dir, name, ".bin");
    size_t initial_pool_size = 16 * table->row_stride;
    if (initial_pool_size < 1024 * 1024) initial_pool_size = 1024 * 1024; // 最小 1MB
    if (pool_init(&table->data_pool, data_path, initial_pool_size) < 0) {
        for (size_t i = 0; i < field_count; i++) {
            free((void*)table->fields[i].name);
        }
        free(table->fields);
        free(table);
        return NULL;
    }
    
    // 创建表级索引文件：db_dir/name.index
    char index_path[MYDB_TABLE_NAME_LEN * 2];
    build_path(index_path, sizeof(index_path), inst->db_dir, name, ".index");
    if (pool_init(&table->index_pool, index_path, initial_pool_size / 10) < 0) {
        pool_close(&table->data_pool);
        for (size_t i = 0; i < field_count; i++) {
            free((void*)table->fields[i].name);
        }
        free(table->fields);
        free(table);
        return NULL;
    }
    
    // 创建索引元数据文件：db_dir/name.idxmeta
    char meta_path[MYDB_TABLE_NAME_LEN * 2];
    build_path(meta_path, sizeof(meta_path), inst->db_dir, name, ".idxmeta");
    if (pool_init(&table->index_meta_pool, meta_path, 64 * 1024) < 0) {
        pool_close(&table->index_pool);
        pool_close(&table->data_pool);
        for (size_t i = 0; i < field_count; i++) {
            free((void*)table->fields[i].name);
        }
        free(table->fields);
        free(table);
        return NULL;
    }
    
    // 创建字符串池文件：db_dir/name.strings
    char string_path[MYDB_TABLE_NAME_LEN * 2];
    build_path(string_path, sizeof(string_path), inst->db_dir, name, ".strings");
    if (pool_init(&table->string_pool, string_path, 1024 * 1024) < 0) {
        pool_close(&table->index_meta_pool);
        pool_close(&table->index_pool);
        pool_close(&table->data_pool);
        for (size_t i = 0; i < field_count; i++) {
            free((void*)table->fields[i].name);
        }
        free(table->fields);
        free(table);
        return NULL;
    }
    
    inst->tables[inst->table_count++] = table;
    
    // 自动为主键创建索引（第一字段是 uint64_t 且 offset 为 0）
    if (field_count > 0 && fields[0].type == DB_TYPE_UINT64 && fields[0].offset == 0) {
        index_create(table, "id", fields[0].offset, fields[0].type);
    }
    
    // 加载已存在的索引定义（从 .idxmeta 文件）
    load_index_defs(table);
    
    // 加载表元数据（如果是已有文件）
    if (table->data_pool.used > TABLE_META_OFFSET + 24) {
        load_table_meta(table);
    } else {
        // 新文件：初始化元数据区域
        save_table_meta(table);
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

void free_table_indexes(db_table_t* table) {
    if (!table) return;
    db_index_t* idx = table->indexes;
    while (idx) {
        db_index_t* next = idx->next;
        free(idx);
        idx = next;
    }
    table->indexes = NULL;
}

void free_table_fields(db_table_t* table) {
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
            save_index_defs(inst->tables[i]);
            pool_close(&inst->tables[i]->string_pool);
            pool_close(&inst->tables[i]->index_meta_pool);
            pool_close(&inst->tables[i]->index_pool);
            pool_close(&inst->tables[i]->data_pool);
            free_table_indexes(inst->tables[i]);
            free_table_fields(inst->tables[i]);
            free_list_destroy(inst->tables[i]);
            free(inst->tables[i]);
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
    
    if (t->deleted_count == 0 || t->row_count == 0) return 0;
    
    // 分配新的连续内存用于 compact 后的数据
    size_t new_size = (t->row_count + 16) * t->row_stride;
    if (new_size < 1024 * 1024) new_size = 1024 * 1024;
    
    void* new_base = malloc(new_size);
    if (!new_base) return 0;
    memset(new_base, 0, new_size);
    
    // 复制文件头（16 字节）
    memcpy(new_base, t->data_pool.base, 16);
    
    // 顺序扫描，存活行移到新位置
    rowid_t new_id = 0;
    size_t moved = 0;
    for (rowid_t id = 1; id <= t->max_rowid; id++) {
        size_t old_offset = id * t->row_stride;
        row_header_t* old_header = (row_header_t*)PTR(t->data_pool.base, old_offset);
        
        if (old_header->flags & MYDB_DELETED_FLAG) continue;
        
        new_id++;
        size_t new_offset = new_id * t->row_stride;
        row_header_t* new_header = (row_header_t*)PTR(new_base, new_offset);
        
        memcpy(new_header, old_header, t->row_stride);
        new_header->rowid = new_id;
        moved++;
    }
    
    if (moved == 0) {
        free(new_base);
        return 0;
    }
    
    // 更新内存
    memcpy(t->data_pool.base, new_base, new_size > t->data_pool.size ? t->data_pool.size : new_size);
    free(new_base);
    
    // 更新元数据
    size_t old_max_rowid = t->max_rowid;
    t->max_rowid = new_id;
    t->deleted_count = 0;
    t->free_count = 0; // 清空 free list
    
    // 更新 pool used
    t->data_pool.used = (new_id + 1) * t->row_stride;
    *(size_t*)((char*)t->data_pool.base + 8) = t->data_pool.used;
    save_table_meta(t);
    
    // 重建索引（rowid 变了）
    // 先清空旧索引
    db_index_t* idx = t->indexes;
    while (idx) {
        if (idx->type == INDEX_HASH) {
            hash_destroy(&t->index_pool, idx->data_offset);
            idx->data_offset = hash_create(&t->index_pool);
        } else if (idx->type == INDEX_BTREE) {
            btree_destroy(&t->index_pool, idx->data_offset);
            idx->data_offset = btree_create(&t->index_pool, idx->field_sizes[0], idx->field_types[0]);
        }
        idx = idx->next;
    }
    
    // 重建 string_pool（清理已删除行的字符串）
    if (t->string_pool.base) {
        // 重置 string_pool：保留文件头，清空数据区
        memset((char*)t->string_pool.base + 16, 0, t->string_pool.size - 16);
        t->string_pool.used = 16;
        *(size_t*)((char*)t->string_pool.base + 8) = 16;
        
        // 重新插入所有存活行的字符串
        for (rowid_t id = 1; id <= t->max_rowid; id++) {
            size_t offset = id * t->row_stride;
            row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
            if (header->flags & MYDB_DELETED_FLAG) continue;
            
            void* row_ptr = (char*)header + sizeof(row_header_t);
            char* dst = (char*)row_ptr;
            
            for (size_t i = 0; i < t->field_count; i++) {
                db_field_def_t* f = &t->fields[i];
                if (f->type == DB_TYPE_VARSTRING) {
                    db_string_ref_t* ref = (db_string_ref_t*)(dst + f->offset);
                    if (ref->length > 0) {
                        void* str_ptr = pool_alloc(&t->string_pool, ref->length + 1);
                        if (str_ptr) {
                            size_t old_offset = ref->offset;
                            ref->offset = (size_t)((char*)str_ptr - (char*)t->string_pool.base);
                            memcpy(str_ptr, (char*)t->string_pool.base + old_offset, ref->length + 1);
                        }
                    }
                }
            }
        }
    }
    
    // 重新插入所有数据到索引
    for (rowid_t id = 1; id <= t->max_rowid; id++) {
        size_t offset = id * t->row_stride;
        row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
        void* row_ptr = (char*)header + sizeof(row_header_t);
        index_insert(t, id, row_ptr);
    }
    
    // 收缩文件（如果显著变小）
    if (t->data_pool.size > new_size * 2 && new_size >= 1024 * 1024) {
        pool_resize(&t->data_pool, new_size);
    }
    
    // 保存索引元数据
    save_index_defs(t);
    
    return old_max_rowid - new_id;
}

// 检查是否需要自动 compact
static void check_auto_compact(db_table_t* t) {
    if (t->compact_threshold <= 0) return;
    if (t->max_rowid == 0) return;
    
    float deleted_ratio = (float)t->deleted_count / (float)t->max_rowid;
    if (deleted_ratio >= t->compact_threshold) {
        db_table_compact(t);
    }
}

rowid_t db_insert(table_t table, const void* row, size_t row_size) {
    if (!table || !row) return 0;
    db_table_t* t = (db_table_t*)table;
    
    if (row_size != t->row_size) return 0;
    
    rowid_t id;
    
    // 优先从 free list 复用
    id = free_list_pop(t);
    if (id == 0) {
        // free list 为空，分配新 id
        id = ++t->max_rowid;
    }
    
    // 确保数据区足够大
    size_t need_offset = (id + 1) * t->row_stride;
    if (need_offset > t->data_pool.size) {
        size_t new_size = t->data_pool.size * 2;
        if (need_offset > new_size) {
            new_size = need_offset + (1024 * 1024);
        }
        if (pool_resize(&t->data_pool, new_size) < 0) {
            return 0;
        }
    }
    
    // 计算行位置
    size_t row_offset = id * t->row_stride;
    void* ptr = (char*)t->data_pool.base + row_offset;
    
    // 写入 header
    row_header_t* header = (row_header_t*)ptr;
    header->rowid = id;
    header->flags = 0;
    
    // 写入数据（处理变长字符串）
    void* row_ptr = (char*)ptr + sizeof(row_header_t);
    char* src = (char*)row;
    char* dst = (char*)row_ptr;
    
    for (size_t i = 0; i < t->field_count; i++) {
        db_field_def_t* f = &t->fields[i];
        void* field_src = src + f->offset;
        void* field_dst = dst + f->offset;
        
        if (f->type == DB_TYPE_VARSTRING) {
            // 变长字符串：从传入的 row 中读取 char* 指针
            char* str = *(char**)field_src;
            size_t str_len = str ? strlen(str) : 0;
            
            // 分配字符串池空间
            size_t string_offset = 0;
            if (str_len > 0) {
                void* str_ptr = pool_alloc(&t->string_pool, str_len + 1);
                if (str_ptr) {
                    string_offset = (size_t)((char*)str_ptr - (char*)t->string_pool.base);
                    memcpy(str_ptr, str, str_len + 1);
                }
            }
            
            db_string_ref_t ref = {string_offset, str_len};
            memcpy(field_dst, &ref, sizeof(ref));
        } else {
            memcpy(field_dst, field_src, f->size);
        }
    }
    
    // 自动填充 id 到用户数据的第一个字段
    if (t->field_count > 0 && t->fields[0].type == DB_TYPE_UINT64 && t->fields[0].offset == 0) {
        *(uint64_t*)((char*)ptr + sizeof(row_header_t)) = id;
    }
    
    t->row_count++;
    save_table_meta(t);
    
    // 更新池已用大小
    if (need_offset > t->data_pool.used) {
        t->data_pool.used = need_offset;
        // 更新文件头
        *(size_t*)((char*)t->data_pool.base + 8) = need_offset;
    }
    
    // 更新索引
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
    (void)row_size;  // 参数保留用于API兼容性，实际通过字段列表计算大小
    if (!table || !row || id == 0) return DB_ERR_INVAL;
    db_table_t* t = (db_table_t*)table;
    
    if (id > t->max_rowid) return DB_ERR_NOENT;
    
    size_t offset = id * t->row_stride;
    row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
    
    if (header->flags & MYDB_DELETED_FLAG) return DB_ERR_NOENT;
    
    void* old_row = (char*)header + sizeof(row_header_t);
    
    // 删除旧索引
    index_delete(t, id, old_row);
    
    // 更新数据（处理变长字符串）
    char* src = (char*)row;
    char* dst = (char*)old_row;
    
    for (size_t i = 0; i < t->field_count; i++) {
        db_field_def_t* f = &t->fields[i];
        void* field_src = src + f->offset;
        void* field_dst = dst + f->offset;
        
        if (f->type == DB_TYPE_VARSTRING) {
            char* str = *(char**)field_src;
            size_t str_len = str ? strlen(str) : 0;
            
            size_t string_offset = 0;
            if (str_len > 0) {
                void* str_ptr = pool_alloc(&t->string_pool, str_len + 1);
                if (str_ptr) {
                    string_offset = (size_t)((char*)str_ptr - (char*)t->string_pool.base);
                    memcpy(str_ptr, str, str_len + 1);
                }
            }
            
            db_string_ref_t ref = {string_offset, str_len};
            memcpy(field_dst, &ref, sizeof(ref));
        } else {
            memcpy(field_dst, field_src, f->size);
        }
    }
    
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
    
    size_t offset = id * t->row_stride;
    row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
    
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
    t->deleted_count++;
    save_table_meta(t);
    
    // 加入 free list 供复用
    free_list_push(t, id);
    
    // 检查是否需要自动 compact
    check_auto_compact(t);
    
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
    
    index->type = INDEX_BTREE;
    
    size_t key_size = 0;
    for (size_t i = 0; i < field_count && i < MYDB_MAX_INDEX_FIELDS; i++) {
        key_size += fields[i].size;
    }
    
    // B+树使用 pool 零拷贝
    index->data_offset = btree_create(&table->index_pool, key_size, fields[0].type);
    if (!index->data_offset) {
        free(index);
        return DB_ERR_NOMEM;
    }
    
    index->next = t->indexes;
    t->indexes = index;
    
    // 为已有数据建立索引
    for (rowid_t id = 1; id <= t->max_rowid; id++) {
        size_t offset = id * t->row_stride;
        row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
        if (header->flags & MYDB_DELETED_FLAG) continue;
        
        void* row_ptr = (char*)header + sizeof(row_header_t);
        index_insert(t, id, row_ptr);
    }
    
    return DB_OK;
}

void db_table_set_compact_threshold(table_t table, float threshold) {
    if (!table) return;
    db_table_t* t = (db_table_t*)table;
    if (threshold >= 0 && threshold <= 1.0) {
        t->compact_threshold = threshold;
    }
}
