#include "mydb.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct wal_user {
    uint64_t id;
    char     name[32];
    int32_t  age;
};

DB_TABLE(wal_user, "wal_users",
    DB_FIELD(wal_user, id,   DB_TYPE_UINT64),
    DB_FIELD(wal_user, name, DB_TYPE_STRING),
    DB_FIELD(wal_user, age,  DB_TYPE_INT32)
)

static int test_count = 0;
static int pass_count = 0;

#define CHECK(cond, msg) do { \
    test_count++; \
    if (cond) { \
        pass_count++; \
        printf("[OK] %s\n", msg); \
    } else { \
        printf("[FAIL] %s\n", msg); \
    } \
} while(0)

int main() {
    printf("=== my_db WAL 测试 ===\n\n");
    
    // 测试1: WAL 写入和回放
    {
        db_t db = db_open("wal_data_db", 1024*1024*10);
        CHECK(db != NULL, "数据库打开成功");
        
        db_register_wal_user(db);
        table_t users = db_table(db, "wal_users");
        CHECK(users != NULL, "获取表句柄成功");
        
        // 插入数据
        struct wal_user u1 = {0, "Alice", 25};
        struct wal_user u2 = {0, "Bob", 30};
        rowid_t id1 = db_insert(users, &u1, sizeof(u1));
        rowid_t id2 = db_insert(users, &u2, sizeof(u2));
        CHECK(id1 == 1, "插入 Alice");
        CHECK(id2 == 2, "插入 Bob");
        
        // 检查数据存在
        const char* json = db_select_all_json(users);
        CHECK(json != NULL && strstr(json, "Alice") != NULL, "插入后数据可读");
        db_json_free(json);
        
        // 同步（确保 WAL 写入）
        CHECK(db_sync(db) == 0, "db_sync 成功");
        
        // 关闭数据库
        db_close(db);
        
        // 重新打开并回放 WAL
        db = db_open("wal_data_db", 1024*1024*10);
        CHECK(db != NULL, "重新打开数据库成功");
        
        db_register_wal_user(db);
        users = db_table(db, "wal_users");
        
        // 回放 WAL
        int ret = db_wal_replay(db);
        CHECK(ret == 0, "WAL 回放成功");
        
        // 验证数据恢复
        json = db_select_all_json(users);
        if (json) {
            CHECK(strstr(json, "Alice") != NULL, "回放后 Alice 存在");
            CHECK(strstr(json, "Bob") != NULL, "回放后 Bob 存在");
            db_json_free(json);
        }
        
        db_close(db);
    }
    
    // 测试2: Checkpoint 机制
    {
        db_t db = db_open("wal_data2_db", 1024*1024*10);
        CHECK(db != NULL, "Checkpoint 测试：数据库打开");
        
        db_register_wal_user(db);
        table_t users = db_table(db, "wal_users");
        
        // 插入数据
        struct wal_user u = {0, "Charlie", 35};
        db_insert(users, &u, sizeof(u));
        
        // Checkpoint 前验证数据可读
        const char* json = db_select_all_json(users);
        CHECK(json != NULL && strstr(json, "Charlie") != NULL, "Checkpoint 前数据可读");
        db_json_free(json);
        
        // Checkpoint
        int ret = db_checkpoint(db);
        CHECK(ret == 0, "Checkpoint 成功");
        
        // Checkpoint 后验证数据还在（内存中）
        json = db_select_all_json(users);
        if (json) {
            CHECK(strstr(json, "Charlie") != NULL, "Checkpoint 后数据仍在内存");
            db_json_free(json);
        }
        
        db_close(db);
    }
    
    printf("\n=== 测试结果: %d/%d 通过 ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
