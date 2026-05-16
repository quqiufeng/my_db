#ifndef MYDB_INTERNAL_H
#define MYDB_INTERNAL_H

#include "mydb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>

// ====== 常量 ======
#define MYDB_MAGIC_DATA     "MYDB"
#define MYDB_MAGIC_INDEX    "MYIX"
#define MYDB_VERSION        1
#define MYDB_ALIGN          8
#define MYDB_DEFAULT_POOL_SIZE  (1024 * 1024 * 100)  // 100MB
#define MYDB_MAX_ROWS_DEFAULT   10000
#define MYDB_TABLE_NAME_LEN     64
#define MYDB_MAX_TABLES         256
#define MYDB_DELETED_FLAG       0x01

// ====== 偏移寻址宏 ======
#define PTR(base, offset) ((void*)((char*)(base) + (offset)))
#define OFF(base, ptr)    ((size_t)((char*)(ptr) - (char*)(base)))

// ====== 内存池 ======
typedef struct {
    int     fd;
    void*   base;
    size_t  size;
    size_t  used;
    size_t  capacity;
} db_pool_t;

// ====== WAL ======
typedef struct {
    int         fd;
    uint64_t    lsn;
} db_wal_t;

// ====== 行头 ======
typedef struct {
    rowid_t rowid;
    uint8_t flags;
} row_header_t;

// ====== 索引类型 ======
typedef enum {
    INDEX_HASH,
    INDEX_BTREE,
} index_type_t;

#define MYDB_MAX_INDEX_FIELDS   4

// ====== 索引项 ======
typedef struct db_index {
    char            name[MYDB_TABLE_NAME_LEN];
    int             field_count;                    // 字段数量（1=单列，>1=复合）
    size_t          field_offsets[MYDB_MAX_INDEX_FIELDS];
    size_t          field_sizes[MYDB_MAX_INDEX_FIELDS];
    int             field_types[MYDB_MAX_INDEX_FIELDS];
    index_type_t    type;
    void*           data;       // 哈希表或 B+树指针
    struct db_index* next;
} db_index_t;

// ====== 表结构 ======
typedef struct db_table {
    char            name[MYDB_TABLE_NAME_LEN];
    size_t          row_size;
    size_t          row_stride;     // 对齐后的行大小（包含 header）
    size_t          row_count;
    size_t          max_rowid;
    size_t          data_offset;    // 数据区起始偏移
    size_t          data_used;      // 数据区已用字节
    db_field_def_t* fields;
    size_t          field_count;
    db_index_t*     indexes;        // 索引链表
    db_pool_t*      data_pool;      // 数据文件 mmap
} db_table_t;

// ====== 数据库实例 ======
typedef struct db_instance {
    db_pool_t       data_pool;
    db_pool_t       index_pool;
    db_wal_t        wal;
    db_table_t*     tables[MYDB_MAX_TABLES];
    size_t          table_count;
    size_t          max_rows;
    int             last_error;
    char            error_msg[256];
} db_instance_t;

// ====== 工具函数 ======
uint32_t crc32(const void* data, size_t len);
void db_set_error(db_instance_t* db, int code, const char* fmt, ...);

// ====== 池操作 ======
int pool_init(db_pool_t* pool, const char* path, size_t initial_size);
void pool_close(db_pool_t* pool);
int pool_sync(db_pool_t* pool);
void* pool_alloc(db_pool_t* pool, size_t size);
int pool_resize(db_pool_t* pool, size_t new_size);

// ====== WAL 操作 ======
int wal_init(db_wal_t* wal, const char* path);
void wal_close(db_wal_t* wal);
int wal_append(db_wal_t* wal, int op, const char* table_name,
               const void* row_data, size_t row_size, rowid_t rowid);
int wal_replay(db_wal_t* wal, db_instance_t* db);
int wal_fsync(db_wal_t* wal);

// ====== 哈希表操作 ======
void* hash_create(void);
void hash_destroy(void* hash);
int hash_insert(void* hash, const void* key, size_t key_len, rowid_t value);
rowid_t hash_lookup(void* hash, const void* key, size_t key_len);
int hash_delete(void* hash, const void* key, size_t key_len);

// ====== B+树操作 ======
void* btree_create(size_t key_size, int key_type);
void btree_destroy(void* tree);
int btree_insert(void* tree, void* key, rowid_t value);
rowid_t* btree_range(void* tree, void* min_key, void* max_key, size_t* count);

// ====== 索引操作 ======
int index_create(db_table_t* table, const char* field_name,
                 size_t field_offset, int field_type);
void index_insert(db_table_t* table, rowid_t rowid, void* row_ptr);
void index_delete(db_table_t* table, rowid_t rowid, void* row_ptr);
rowid_t* index_lookup(db_table_t* table, const char* field_name,
                      const void* value, size_t* count);

// ====== JSON 序列化 ======
char* json_row(db_table_t* table, void* row_ptr);
char* json_rows(db_table_t* table, rowid_t* rowids, size_t count);
char* json_join_rows(db_table_t* left, db_table_t* right,
                     void** left_rows, void** right_rows, size_t count);

// ====== 查询 ======
bool row_match(db_table_t* table, void* row_ptr,
               const db_condition_t* conditions, size_t count);
int compare_field(const void* a, const void* b,
                  size_t offset, size_t size, int type);

#endif
