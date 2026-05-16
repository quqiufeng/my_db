#include "mydb.h"
#include <stdio.h>
#include <string.h>

struct user {
    uint64_t id;
    char     name[32];
    int32_t  age;
    double   score;
};

struct order {
    uint64_t id;
    uint64_t user_id;
    double   amount;
};

DB_TABLE(user, "users",
    DB_FIELD(user, id,    DB_TYPE_UINT64),
    DB_FIELD(user, name,  DB_TYPE_STRING),
    DB_FIELD(user, age,   DB_TYPE_INT32),
    DB_FIELD(user, score, DB_TYPE_DOUBLE)
)

DB_TABLE(order, "orders",
    DB_FIELD(order, id,      DB_TYPE_UINT64),
    DB_FIELD(order, user_id, DB_TYPE_UINT64),
    DB_FIELD(order, amount,  DB_TYPE_DOUBLE)
)

int main() {
    printf("=== my_db JOIN 测试 ===\n\n");
    
    db_t db = db_open("join_data.bin", "join_index.index", "join_wal.bin", 1024*1024*10);
    db_register_user(db);
    db_register_order(db);
    
    table_t users = db_table(db, "users");
    table_t orders = db_table(db, "orders");
    
    // 插入用户
    struct user u1 = {0, "Alice", 25, 95.5};
    rowid_t uid1 = db_insert(users, &u1, sizeof(u1));
    printf("[OK] 用户 Alice id=%lu\n", (unsigned long)uid1);
    
    struct user u2 = {0, "Bob", 30, 88.0};
    rowid_t uid2 = db_insert(users, &u2, sizeof(u2));
    printf("[OK] 用户 Bob id=%lu\n", (unsigned long)uid2);
    
    // 插入订单
    struct order o1 = {0, uid1, 100.0};
    db_insert(orders, &o1, sizeof(o1));
    printf("[OK] 订单1 user_id=%lu\n", (unsigned long)uid1);
    
    // 验证数据
    printf("\n验证数据:\n");
    const char* u1_json = db_select_by_pk_json(users, 1);
    printf("  用户1: %s\n", u1_json);
    db_json_free(u1_json);
    
    const char* o1_json = db_select_by_pk_json(orders, 1);
    printf("  订单1: %s\n", o1_json);
    db_json_free(o1_json);
    
    // JOIN
    printf("\nJOIN 结果:\n");
    const char* join = db_join_json(users, 0, orders, 8, sizeof(uint64_t));
    printf("%s\n", join);
    db_json_free(join);
    
    db_close(db);
    return 0;
}
