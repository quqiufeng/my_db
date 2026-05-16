#include "mydb_internal.h"
#include <sys/stat.h>
#include <libgen.h>

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
            for (size_t j = 0; j < inst->tables[i]->field_count; j++) {
                free((void*)inst->tables[i]->fields[j].name);
            }
            free(inst->tables[i]->fields);
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
    (void)db;
    return DB_OK;
}

int db_commit(db_t db) {
    return db_sync(db);
}

int db_rollback(db_t db) {
    (void)db;
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
