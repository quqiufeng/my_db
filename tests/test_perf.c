#include "mydb.h"
#include <stdio.h>
#include <time.h>

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

static double get_time_ms(struct timespec* start, struct timespec* end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

int main() {
    printf("=== 压力测试：大规模数据操作 ===\n\n");
    
    db_t db = db_open("perf_data_db", 1024*1024*500);
    db_register_user(db);
    table_t users = db_table(db, "users");
    
    struct timespec t1, t2;
    struct user u;
    
    // 1. 批量插入 10 万行
    printf("1. 插入 10 万行...\n");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < 100000; i++) {
        snprintf(u.name, sizeof(u.name), "User%d", i);
        u.age = 18 + (i % 50);
        u.score = (double)(i % 100);
        db_insert(users, &u, sizeof(u));
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("   [OK] 插入完成，总行数: %zu，耗时: %.2f ms (%.0f 行/秒)\n",
           db_table_count(users), get_time_ms(&t1, &t2), 100000.0 / (get_time_ms(&t1, &t2) / 1000.0));
    
    // 2. 主键查询 1 万次
    printf("2. 主键查询 1 万次...\n");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < 10000; i++) {
        rowid_t id = 1 + (i % 100000);
        const char* json = db_select_by_pk_json(users, id);
        if (json) db_json_free(json);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("   [OK] 主键查询 1 万次，耗时: %.2f ms (%.0f QPS)\n",
           get_time_ms(&t1, &t2), 10000.0 / (get_time_ms(&t1, &t2) / 1000.0));
    
    // 3. 更新 1 万行
    printf("3. 更新 1 万行...\n");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < 10000; i++) {
        rowid_t id = 1 + (i % 100000);
        snprintf(u.name, sizeof(u.name), "Updated%d", i);
        u.age = 50;
        u.score = 99.9;
        db_update(users, id, &u, sizeof(u));
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("   [OK] 更新 1 万行，耗时: %.2f ms (%.0f 行/秒)\n",
           get_time_ms(&t1, &t2), 10000.0 / (get_time_ms(&t1, &t2) / 1000.0));
    
    // 4. 删除 5 万行（触发 compact）
    printf("4. 删除 5 万行...\n");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < 50000; i++) {
        rowid_t id = 1 + i;
        db_delete(users, id);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("   [OK] 删除 5 万行，耗时: %.2f ms，剩余行数: %zu\n",
           get_time_ms(&t1, &t2), db_table_count(users));
    
    // 5. compact 后查询
    printf("5. Compact + 查询...\n");
    clock_gettime(CLOCK_MONOTONIC, &t1);
    size_t compacted = db_table_compact(users);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("   [OK] Compact 回收 %zu 行，耗时: %.2f ms\n", compacted, get_time_ms(&t1, &t2));
    
    // 6. 流式查询全表
    printf("6. 流式查询全表...\n");
    int count = 0;
    int cb(const char* json __attribute__((unused)), void* d __attribute__((unused))) {
        count++;
        return 0;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    db_select_all_stream(users, cb, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    printf("   [OK] 流式查询 %d 行，耗时: %.2f ms\n", count, get_time_ms(&t1, &t2));
    
    // 7. db_check 完整性验证
    printf("7. 数据库完整性检查...\n");
    int check = db_check("perf_data_db");
    printf("   [OK] db_check 结果: %s\n", check == 0 ? "通过" : "失败");
    
    db_close(db);
    printf("\n=== 压力测试完成 ===\n");
    return 0;
}
