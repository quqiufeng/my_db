#include "mydb.h"
#include <stdio.h>

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
    printf("=== 性能测试：100万行插入 + 查询 ===\n");
    
    db_t db = db_open("perf_data_db", 1024*1024*500);
    db_register_user(db);
    table_t users = db_table(db, "users");
    
    // 批量插入 100 万行
    printf("插入 100 万行...\n");
    struct user u;
    for (int i = 0; i < 1000000; i++) {
        snprintf(u.name, sizeof(u.name), "User%d", i);
        u.age = 18 + (i % 50);
        u.score = (double)(i % 100);
        db_insert(users, &u, sizeof(u));
    }
    printf("[OK] 插入完成，总行数: %zu\n", db_table_count(users));
    
    // 查询单条
    const char* json = db_select_by_pk_json(users, 500000);
    printf("[OK] 查询第50万行: %.80s...\n", json);
    db_json_free(json);
    
    // WHERE 查询
    int32_t age = 30;
    db_condition_t cond = {offsetof(struct user, age), sizeof(int32_t), &age, 0};
    const char* where = db_select_where_json(users, &cond, 1);
    printf("[OK] WHERE 查询(age=30): %.80s...\n", where);
    db_json_free(where);
    
    // 流式查询（统计）
    int count = 0;
    int cb(const char* json __attribute__((unused)), void* d __attribute__((unused))) {
        count++;
        return 0;
    }
    db_select_all_stream(users, cb, NULL);
    printf("[OK] 流式查询总行数: %d\n", count);
    
    db_close(db);
    printf("\n=== 性能测试完成 ===\n");
    return 0;
}
