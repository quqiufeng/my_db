// my_db C 示例程序
// 演示：注册 Schema、CRUD、查询、索引

#include "mydb.h"
#include <stdio.h>
#include <string.h>

// 定义用户结构体
struct user {
    uint64_t id;        // 第一字段必须是 uint64_t id，自动填充
    char     name[32];
    int32_t  age;
    double   score;
};

// 编译时注册 Schema
DB_TABLE(user, "users",
    DB_FIELD(user, id,    DB_TYPE_UINT64),
    DB_FIELD(user, name,  DB_TYPE_STRING),
    DB_FIELD(user, age,   DB_TYPE_INT32),
    DB_FIELD(user, score, DB_TYPE_DOUBLE)
)

int main() {
    printf("=== my_db C 示例 ===\n\n");
    
    // 1. 打开数据库
    db_t db = db_open("example_data_db", 1024*1024*10);
    if (!db) {
        printf("打开数据库失败\n");
        return 1;
    }
    printf("1. 数据库打开成功\n");
    
    // 2. 注册表（编译时生成的函数）
    db_register_user(db);
    printf("2. Schema 注册成功\n");
    
    // 3. 获取表句柄
    table_t users = db_table(db, "users");
    if (!users) {
        printf("获取表失败\n");
        db_close(db);
        return 1;
    }
    
    // 4. 创建索引
    db_table_add_index(users, "name", offsetof(struct user, name), DB_TYPE_STRING);
    printf("3. 创建 name 索引成功\n");
    
    // 5. 插入数据（id 自动填充为 0，数据库自动分配）
    struct user alice = {0, "Alice", 25, 95.5};
    struct user bob   = {0, "Bob",   30, 88.0};
    struct user carol = {0, "Carol", 28, 92.0};
    
    rowid_t id1 = db_insert(users, &alice, sizeof(alice));
    rowid_t id2 = db_insert(users, &bob,   sizeof(bob));
    rowid_t id3 = db_insert(users, &carol, sizeof(carol));
    
    printf("4. 插入数据: Alice(id=%lu), Bob(id=%lu), Carol(id=%lu)\n",
           (unsigned long)id1, (unsigned long)id2, (unsigned long)id3);
    
    // 6. 主键查询
    const char* json = db_select_by_pk_json(users, id1);
    printf("5. 主键查询 id=1: %s\n", json);
    db_json_free(json);
    
    // 7. 全表查询
    json = db_select_all_json(users);
    printf("6. 全表查询: %s\n", json);
    db_json_free(json);
    
    // 8. WHERE 查询（age > 25）
    int32_t age_val = 25;
    db_condition_t cond = {offsetof(struct user, age), sizeof(int32_t), &age_val, 1};
    json = db_select_where_json(users, &cond, 1);
    printf("7. WHERE age>25: %s\n", json);
    db_json_free(json);
    
    // 9. 更新
    struct user alice_new = {id1, "Alice Updated", 26, 96.0};
    db_update(users, id1, &alice_new, sizeof(alice_new));
    printf("8. 更新 Alice 成功\n");
    
    // 10. 删除
    db_delete(users, id3);
    printf("9. 删除 Carol 成功\n");
    
    // 11. 验证删除后
    json = db_select_all_json(users);
    printf("10. 删除后: %s\n", json);
    db_json_free(json);
    
    // 12. Checkpoint
    db_checkpoint(db);
    printf("11. Checkpoint 完成\n");
    
    // 13. 关闭数据库
    db_close(db);
    printf("12. 数据库关闭\n");
    
    printf("\n=== 示例完成 ===\n");
    return 0;
}
