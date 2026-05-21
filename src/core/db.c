#include "mydb_internal.h"
#include <sys/stat.h>
#include <libgen.h>
#include <dirent.h>
#include <string.h>

// 确保目录存在
static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        return -1; // 存在但不是目录
    }
    return mkdir(path, 0755);
}

// 构建路径：dir/name.ext
static void build_path(char* out, size_t out_size, const char* dir, const char* name, const char* ext) {
    snprintf(out, out_size, "%s/%s%s", dir, name, ext);
}

db_t db_open(const char* db_dir, size_t pool_size) {
    (void)pool_size;
    if (!db_dir) return NULL;
    
    db_instance_t* db = (db_instance_t*)calloc(1, sizeof(db_instance_t));
    if (!db) return NULL;
    
    db->max_rows = MYDB_MAX_ROWS_DEFAULT;
    strncpy(db->db_dir, db_dir, MYDB_TABLE_NAME_LEN - 1);
    
    // 确保目录存在
    if (ensure_dir(db_dir) < 0) {
        free(db);
        return NULL;
    }
    
    // 初始化 WAL（单文件，放在目录下）
    char wal_path[MYDB_TABLE_NAME_LEN * 2];
    build_path(wal_path, sizeof(wal_path), db_dir, "wal", ".bin");
    if (wal_init(&db->wal, wal_path) < 0) {
        free(db);
        return NULL;
    }
    
    return db;
}

void db_close(db_t db) {
    if (!db) return;
    db_instance_t* inst = (db_instance_t*)db;
    
    wal_close(&inst->wal);
    
    // 关闭每张表的独立文件
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i]) {
            save_index_defs(inst->tables[i]);
            pool_close(&inst->tables[i]->string_pool);
            pool_close(&inst->tables[i]->index_meta_pool);
            pool_close(&inst->tables[i]->index_pool);
            pool_close(&inst->tables[i]->data_pool);
            free_table_indexes(inst->tables[i]);
            free_table_fields(inst->tables[i]);
            free_list_destroy(inst->tables[i]);
            free(inst->tables[i]);
        }
    }
    
    free(inst);
}

int db_sync(db_t db) {
    if (!db) return -1;
    db_instance_t* inst = (db_instance_t*)db;
    
    int ret = 0;
    
    // 同步所有表的数据文件
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i]) {
            if (pool_sync(&inst->tables[i]->data_pool) < 0) ret = -1;
            if (pool_sync(&inst->tables[i]->index_pool) < 0) ret = -1;
            if (pool_sync(&inst->tables[i]->index_meta_pool) < 0) ret = -1;
        }
    }
    
    if (wal_fsync(&inst->wal) < 0) ret = -1;
    
    return ret;
}

void db_config_max_rows(db_t db, size_t max_rows) {
    if (!db) return;
    ((db_instance_t*)db)->max_rows = max_rows;
}

int db_begin(db_t db) {
    if (!db) return DB_ERR_INVAL;
    db_instance_t* inst = (db_instance_t*)db;
    
    if (inst->in_transaction) return DB_ERR_INVAL; // 不支持嵌套事务
    
    // 记录事务开始时的状态
    inst->in_transaction = 1;
    
    // 记录 WAL 文件当前大小
    struct stat st;
    if (fstat(inst->wal.fd, &st) == 0) {
        inst->txn_wal_offset = st.st_size;
    } else {
        inst->txn_wal_offset = 0;
    }
    
    // 记录各表的 max_rowid 和 row_count
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i]) {
            inst->txn_max_rowid[i] = inst->tables[i]->max_rowid;
            inst->txn_row_count[i] = inst->tables[i]->row_count;
        }
    }
    
    return DB_OK;
}

int db_commit(db_t db) {
    if (!db) return DB_ERR_INVAL;
    db_instance_t* inst = (db_instance_t*)db;
    
    if (!inst->in_transaction) return DB_ERR_INVAL;
    
    inst->in_transaction = 0;
    
    // 同步所有数据到磁盘
    return db_sync(db);
}

int db_rollback(db_t db) {
    if (!db) return DB_ERR_INVAL;
    db_instance_t* inst = (db_instance_t*)db;
    
    if (!inst->in_transaction) return DB_ERR_INVAL;
    
    // 1. 截断 WAL 到事务开始时的位置
    if (inst->wal.fd >= 0) {
        ftruncate(inst->wal.fd, inst->txn_wal_offset);
        lseek(inst->wal.fd, inst->txn_wal_offset, SEEK_SET);
    }
    
    // 2. 关闭并重新打开所有表的数据文件（从磁盘重新加载）
    for (size_t i = 0; i < inst->table_count; i++) {
        db_table_t* t = inst->tables[i];
        if (!t) continue;
        
        char path[MYDB_TABLE_NAME_LEN * 2];
        
        // 关闭旧 pool
        pool_close(&t->data_pool);
        pool_close(&t->index_pool);
        pool_close(&t->string_pool);
        
        // 重新打开（从磁盘加载）
        build_path(path, sizeof(path), inst->db_dir, t->name, ".bin");
        pool_init(&t->data_pool, path, 1024 * 1024);
        
        build_path(path, sizeof(path), inst->db_dir, t->name, ".index");
        pool_init(&t->index_pool, path, 1024 * 1024);
        
        build_path(path, sizeof(path), inst->db_dir, t->name, ".strings");
        pool_init(&t->string_pool, path, 1024 * 1024);
        
        // 恢复 max_rowid 和 row_count
        t->max_rowid = inst->txn_max_rowid[i];
        t->row_count = inst->txn_row_count[i];
        t->deleted_count = 0;
        
        // 清空 free list
        if (t->free_list) {
            free(t->free_list);
            t->free_list = NULL;
        }
        t->free_count = 0;
        t->free_capacity = 0;
        
        // 重建索引
        if (t->indexes) {
            // 重置索引数据
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
            
            // 从数据重新插入索引
            for (rowid_t id = 1; id <= t->max_rowid; id++) {
                size_t offset = id * t->row_stride;
                row_header_t* header = (row_header_t*)PTR(t->data_pool.base, offset);
                if (!(header->flags & MYDB_DELETED_FLAG)) {
                    void* row_ptr = (char*)header + sizeof(row_header_t);
                    index_insert(t, id, row_ptr);
                }
            }
        }
        
        // 保存元数据
        save_table_meta(t);
    }
    
    inst->in_transaction = 0;
    
    return DB_OK;
}

int db_wal_replay(db_t db) {
    if (!db) return DB_ERR_INVAL;
    db_instance_t* inst = (db_instance_t*)db;
    return wal_replay(&inst->wal, inst);
}

int db_checkpoint(db_t db) {
    if (!db) return DB_ERR_INVAL;
    db_instance_t* inst = (db_instance_t*)db;
    
    int ret = db_sync(db);
    if (ret < 0) return ret;
    
    return wal_checkpoint(&inst->wal);
}

size_t db_compact(db_t db) {
    if (!db) return 0;
    db_instance_t* inst = (db_instance_t*)db;
    size_t total_deleted = 0;
    
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i]) {
            total_deleted += db_table_compact(inst->tables[i]);
        }
    }
    
    return total_deleted;
}

// 检查单个 pool 文件的头部（magic + version + used）
static int check_pool_file(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return DB_ERR_IO;
    
    char header[16];
    ssize_t n = read(fd, header, 16);
    close(fd);
    
    if (n < 16) return DB_ERR_CORRUPTED;
    
    if (memcmp(header, MYDB_MAGIC_DATA, 4) != 0) return DB_ERR_CORRUPTED;
    
    uint32_t version = *(uint32_t*)(header + 4);
    if (version != MYDB_VERSION) return DB_ERR_CORRUPTED;
    
    size_t used = *(size_t*)(header + 8);
    if (used < 16) return DB_ERR_CORRUPTED;
    
    return DB_OK;
}

// 检查 .bin 文件的表元数据一致性
static int check_table_meta(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return DB_ERR_IO;
    
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return DB_ERR_IO;
    }
    
    if (st.st_size < 16 + 24) {
        close(fd);
        return DB_OK; // 文件太小，没有元数据，视为正常
    }
    
    size_t meta[3];
    if (pread(fd, meta, sizeof(meta), 16) != sizeof(meta)) {
        close(fd);
        return DB_ERR_CORRUPTED;
    }
    
    close(fd);
    
    size_t row_count = meta[0];
    size_t max_rowid = meta[1];
    size_t deleted_count = meta[2];
    
    // 基本一致性检查
    if (row_count > max_rowid) return DB_ERR_CORRUPTED;
    if (deleted_count > max_rowid) return DB_ERR_CORRUPTED;
    
    return DB_OK;
}

int db_check(const char* db_dir) {
    if (!db_dir) return DB_ERR_INVAL;
    
    struct stat st;
    if (stat(db_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return DB_ERR_IO;
    }
    
    DIR* dir = opendir(db_dir);
    if (!dir) return DB_ERR_IO;
    
    int result = DB_OK;
    char path[MYDB_TABLE_NAME_LEN * 4];
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 5) continue; // 至少需要 x.bin
        
        // 检查是否为 .bin 文件
        if (strcmp(entry->d_name + len - 4, ".bin") != 0) continue;
        
        // 跳过 wal.bin
        if (strcmp(entry->d_name, "wal.bin") == 0) continue;
        
        // 提取表名（去掉 .bin 后缀）
        char table_name[MYDB_TABLE_NAME_LEN];
        size_t name_len = len - 4;
        if (name_len >= MYDB_TABLE_NAME_LEN) name_len = MYDB_TABLE_NAME_LEN - 1;
        memcpy(table_name, entry->d_name, name_len);
        table_name[name_len] = '\0';
        
        // 检查 .bin
        snprintf(path, sizeof(path), "%s/%s.bin", db_dir, table_name);
        int ret = check_pool_file(path);
        if (ret != DB_OK) { result = ret; break; }
        
        ret = check_table_meta(path);
        if (ret != DB_OK) { result = ret; break; }
        
        // 检查 .index
        snprintf(path, sizeof(path), "%s/%s.index", db_dir, table_name);
        ret = check_pool_file(path);
        if (ret != DB_OK) { result = ret; break; }
        
        // 检查 .idxmeta
        snprintf(path, sizeof(path), "%s/%s.idxmeta", db_dir, table_name);
        ret = check_pool_file(path);
        if (ret != DB_OK) { result = ret; break; }
        
        // 检查 .strings
        snprintf(path, sizeof(path), "%s/%s.strings", db_dir, table_name);
        ret = check_pool_file(path);
        if (ret != DB_OK) { result = ret; break; }
    }
    
    closedir(dir);
    
    // WAL 文件格式与 pool 不同，只检查可读性
    if (result == DB_OK) {
        snprintf(path, sizeof(path), "%s/wal.bin", db_dir);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            close(fd);
        }
        // WAL 不存在或不可读都不视为错误（WAL 是可选的）
    }
    
    return result;
}
