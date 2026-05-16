#include "mydb_internal.h"

db_t db_open(const char* data_path, const char* index_path, const char* wal_path, size_t pool_size) {
    db_instance_t* db = (db_instance_t*)calloc(1, sizeof(db_instance_t));
    if (!db) return NULL;
    
    db->max_rows = MYDB_MAX_ROWS_DEFAULT;
    
    // 初始化数据池
    if (pool_init(&db->data_pool, data_path, pool_size) < 0) {
        free(db);
        return NULL;
    }
    
    // 初始化索引池
    if (pool_init(&db->index_pool, index_path, pool_size / 10) < 0) {
        pool_close(&db->data_pool);
        free(db);
        return NULL;
    }
    
    // 初始化 WAL
    if (wal_init(&db->wal, wal_path) < 0) {
        pool_close(&db->index_pool);
        pool_close(&db->data_pool);
        free(db);
        return NULL;
    }
    
    return db;
}

void db_close(db_t db) {
    if (!db) return;
    db_instance_t* inst = (db_instance_t*)db;
    
    wal_close(&inst->wal);
    pool_close(&inst->index_pool);
    pool_close(&inst->data_pool);
    
    // 释放表结构
    for (size_t i = 0; i < inst->table_count; i++) {
        if (inst->tables[i]) {
            free(inst->tables[i]->fields);
            free(inst->tables[i]);
        }
    }
    
    free(inst);
}

int db_sync(db_t db) {
    if (!db) return -1;
    db_instance_t* inst = (db_instance_t*)db;
    
    int r1 = pool_sync(&inst->data_pool);
    int r2 = pool_sync(&inst->index_pool);
    int r3 = wal_fsync(&inst->wal);
    
    return (r1 < 0 || r2 < 0 || r3 < 0) ? -1 : 0;
}

void db_config_max_rows(db_t db, size_t max_rows) {
    if (!db) return;
    ((db_instance_t*)db)->max_rows = max_rows;
}

int db_begin(db_t db) {
    (void)db;
    // 简化实现：单操作原子性已由 WAL 保证
    return DB_OK;
}

int db_commit(db_t db) {
    // 简化实现
    return db_sync(db);
}

int db_rollback(db_t db) {
    (void)db;
    // 简化实现：复杂事务需要保存点机制
    return DB_OK;
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
