#include "mydb.h"
#include <stdio.h>
#include <string.h>

struct user {
    uint64_t id;
    char     name[32];
    int32_t  age;
    double   score;
};

DB_TABLE(user, "users",
    DB_FIELD(user, id,    DB_TYPE_UINT64),
    DB_FIELD(user, name,  DB_TYPE_STRING),
    DB_FIELD(user, age,   DB_TYPE_INT32),
    DB_FIELD(user, score, DB_TYPE_DOUBLE)
)

int main() {
    printf("=== my_db 基础测试 ===\n\n");
    
    db_t db = db_open("test_data_db", 1024*1024*10);
    if (!db) {
        printf("ERROR: db_open failed\n");
        return 1;
    }
    printf("[OK] 数据库打开成功\n");
    
    // 注册表
    db_register_user(db);
    printf("[OK] Schema 注册成功\n");
    
    table_t users = db_table(db, "users");
    if (!users) {
        printf("ERROR: 获取表失败\n");
        db_close(db);
        return 1;
    }
    printf("[OK] 获取表句柄成功\n");
    
    // 插入数据
    struct user u1 = {0, "Alice", 25, 95.5};
    rowid_t id1 = db_insert(users, &u1, sizeof(u1));
    printf("[OK] 插入 Alice, id=%lu\n", (unsigned long)id1);
    
    struct user u2 = {0, "Bob", 30, 88.0};
    rowid_t id2 = db_insert(users, &u2, sizeof(u2));
    printf("[OK] 插入 Bob, id=%lu\n", (unsigned long)id2);
    
    struct user u3 = {0, "Charlie", 18, 72.5};
    rowid_t id3 = db_insert(users, &u3, sizeof(u3));
    printf("[OK] 插入 Charlie, id=%lu\n", (unsigned long)id3);
    
    // 查询单条
    const char* json1 = db_select_by_pk_json(users, id1);
    if (json1) {
        printf("[OK] 主键查询: %s\n", json1);
        db_json_free(json1);
    }
    
    // 全表查询
    const char* all = db_select_all_json(users);
    if (all) {
        printf("[OK] 全表查询: %s\n", all);
        db_json_free(all);
    }
    
    // WHERE 查询（age > 20）
    int32_t age_val = 20;
    db_condition_t cond = {offsetof(struct user, age), sizeof(int32_t), &age_val, 1};
    const char* where = db_select_where_json(users, &cond, 1);
    if (where) {
        printf("[OK] WHERE 查询(age>20): %s\n", where);
        db_json_free(where);
    }
    
    // 更新
    struct user u1_new = {id1, "Alice Updated", 26, 96.0};
    int ret = db_update(users, id1, &u1_new, sizeof(u1_new));
    if (ret == DB_OK) {
        printf("[OK] 更新 Alice\n");
        const char* updated = db_select_by_pk_json(users, id1);
        printf("[OK] 更新后: %s\n", updated);
        db_json_free(updated);
    }
    
    // 删除
    ret = db_delete(users, id3);
    if (ret == DB_OK) {
        printf("[OK] 删除 Charlie\n");
        const char* after_delete = db_select_all_json(users);
        printf("[OK] 删除后: %s\n", after_delete);
        db_json_free(after_delete);
    }
    
    // 流式查询
    printf("[OK] 流式查询:\n");
    int count = 0;
    int callback(const char* json, void* data) {
        (void)data;
        count++;
        printf("  行 %d: %s\n", count, json);
        return 0;
    }
    db_select_all_stream(users, callback, NULL);
    
    db_close(db);
    printf("\n=== 测试完成 ===\n");
    
    return 0;
}
