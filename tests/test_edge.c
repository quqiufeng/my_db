#include "mydb.h"
#include <stdio.h>
#include <string.h>

struct edge_user {
    uint64_t id;
    char     name[32];
    int32_t  age;
};

DB_TABLE(edge_user, "edge_users",
    DB_FIELD(edge_user, id,   DB_TYPE_UINT64),
    DB_FIELD(edge_user, name, DB_TYPE_STRING),
    DB_FIELD(edge_user, age,  DB_TYPE_INT32)
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
    printf("=== my_db 边界情况测试 ===\n\n");
    
    db_t db = db_open("edge_data_db", 1024*1024*10);
    if (!db) {
        printf("ERROR: db_open failed\n");
        return 1;
    }
    
    // 注册表
    db_register_edge_user(db);
    table_t users = db_table(db, "edge_users");
    
    // 测试1: 空表查询返回 "[]"
    {
        const char* all = db_select_all_json(users);
        CHECK(all && strcmp(all, "[]") == 0, "空表查询返回 []");
        db_json_free(all);
    }
    
    // 测试2: LIMIT offset > 总行数返回 "[]"
    {
        // 插入1行数据
        struct edge_user u = {0, "Test", 20};
        db_insert(users, &u, sizeof(u));
        
        const char* json = db_select_json(users, NULL, 0, 0, 1, 100, 10);
        CHECK(json && strcmp(json, "[]") == 0, "LIMIT offset > 总行数返回 []");
        db_json_free(json);
    }
    
    // 测试3: 删除不存在的行返回 DB_ERR_NOENT
    {
        int ret = db_delete(users, 99999);
        CHECK(ret == DB_ERR_NOENT, "删除不存在行返回 DB_ERR_NOENT");
    }
    
    // 测试4: 更新不存在的行返回 DB_ERR_NOENT
    {
        struct edge_user u = {0, "Ghost", 99};
        int ret = db_update(users, 99999, &u, sizeof(u));
        CHECK(ret == DB_ERR_NOENT, "更新不存在行返回 DB_ERR_NOENT");
    }
    
    // 测试5: 重复注册表返回 NULL
    {
        db_field_def_t fields[] = {
            {"id", 0, 8, DB_TYPE_UINT64},
            {"name", 8, 32, DB_TYPE_STRING},
        };
        table_t dup = db_table_register(db, "edge_users", sizeof(struct edge_user), fields, 2);
        CHECK(dup == NULL, "重复注册表返回 NULL");
    }
    
    // 测试6: 查询结果超过 max_rows
    {
        db_config_max_rows(db, 0); // 设置 max_rows = 0
        const char* json = db_select_all_json(users);
        // max_rows = 0 应该返回 NULL（DB_ERR_RESULT_TOO_LARGE）
        CHECK(json == NULL, "max_rows=0 返回 NULL（超限）");
        if (json) db_json_free(json);
        db_config_max_rows(db, 10000); // 恢复
    }
    
    // 测试7: WAL 回放函数存在且可调用
    {
        int ret = db_wal_replay(db);
        CHECK(ret == 0 || ret == -1, "db_wal_replay 可调用");
    }
    
    // 测试8: db_check 对已存在的数据库返回 0
    {
        int ret = db_check("edge_data_db");
        CHECK(ret == 0, "db_check 对已存在数据库返回 0");
    }
    
    db_close(db);
    
    // 测试9: db_check 对不存在的目录返回 DB_ERR_IO
    {
        int ret = db_check("nonexistent_db_dir_12345");
        CHECK(ret == DB_ERR_IO, "db_check 对不存在目录返回 DB_ERR_IO");
    }
    
    printf("\n=== 测试结果: %d/%d 通过 ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
