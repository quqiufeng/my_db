#include "mydb.h"
#include <stdio.h>
#include <string.h>

struct composite_user {
    uint64_t id;
    char     name[32];
    int32_t  age;
    char     city[32];
};

DB_TABLE(composite_user, "composite_users",
    DB_FIELD(composite_user, id,    DB_TYPE_UINT64),
    DB_FIELD(composite_user, name,  DB_TYPE_STRING),
    DB_FIELD(composite_user, age,   DB_TYPE_INT32),
    DB_FIELD(composite_user, city,  DB_TYPE_STRING)
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
    printf("=== my_db 复合索引测试 ===\n\n");
    
    db_t db = db_open("composite_data.bin", "composite_index.index", "composite_wal.bin", 1024*1024*10);
    if (!db) {
        printf("ERROR: db_open failed\n");
        return 1;
    }
    
    // 注册表
    db_register_composite_user(db);
    table_t users = db_table(db, "composite_users");
    CHECK(users != NULL, "获取表句柄成功");
    
    // 创建单列索引（name 字段）
    int ret = db_table_add_index(users, "name", offsetof(struct composite_user, name), DB_TYPE_STRING);
    CHECK(ret == DB_OK, "创建 name 单列索引成功");
    
    // 创建复合索引（name + age）
    db_field_def_t composite_fields[] = {
        {"name", offsetof(struct composite_user, name), 32, DB_TYPE_STRING},
        {"age",  offsetof(struct composite_user, age),  4,  DB_TYPE_INT32},
    };
    ret = db_table_add_index_composite(users, composite_fields, 2);
    CHECK(ret == DB_OK, "创建 name_age 复合索引成功");
    
    // 插入数据
    struct composite_user u1 = {0, "Alice", 25, "Beijing"};
    struct composite_user u2 = {0, "Alice", 30, "Shanghai"};
    struct composite_user u3 = {0, "Bob",   25, "Beijing"};
    struct composite_user u4 = {0, "Bob",   30, "Shanghai"};
    
    rowid_t id1 = db_insert(users, &u1, sizeof(u1));
    rowid_t id2 = db_insert(users, &u2, sizeof(u2));
    rowid_t id3 = db_insert(users, &u3, sizeof(u3));
    rowid_t id4 = db_insert(users, &u4, sizeof(u4));
    
    CHECK(id1 == 1, "插入 Alice/25/Beijing");
    CHECK(id2 == 2, "插入 Alice/30/Shanghai");
    CHECK(id3 == 3, "插入 Bob/25/Beijing");
    CHECK(id4 == 4, "插入 Bob/30/Shanghai");
    
    // 验证索引被维护（通过查询优化器走索引）
    // WHERE name = "Alice" 应该走单列索引
    char name_val[32] = "Alice";
    db_condition_t cond = {offsetof(struct composite_user, name), 32, name_val, 0};
    const char* json = db_select_where_json(users, &cond, 1);
    CHECK(json != NULL, "WHERE name='Alice' 查询成功");
    if (json) {
        CHECK(strstr(json, "Alice") != NULL, "查询结果包含 Alice");
        // 应该有两条 Alice 记录
        int alice_count = 0;
        const char* p = json;
        while ((p = strstr(p, "Alice")) != NULL) {
            alice_count++;
            p++;
        }
        CHECK(alice_count == 2, "Alice 记录数=2 (id=1, id=2)");
        db_json_free(json);
    }
    
    // 测试更新后索引维护
    struct composite_user u1_new = {id1, "Alice Updated", 26, "Beijing"};
    ret = db_update(users, id1, &u1_new, sizeof(u1_new));
    CHECK(ret == DB_OK, "更新 id=1 成功");
    
    // 更新后旧索引应该失效，新索引生效
    json = db_select_where_json(users, &cond, 1);
    if (json) {
        // 现在只有一条 Alice 记录（id=2），id=1 变成了 Alice Updated
        int alice_count = 0;
        const char* p = json;
        while ((p = strstr(p, "Alice")) != NULL) {
            alice_count++;
            p++;
        }
        CHECK(alice_count == 1, "更新后 Alice 记录数=1 (只有 id=2)");
        db_json_free(json);
    }
    
    // 测试删除后索引维护
    ret = db_delete(users, id2);
    CHECK(ret == DB_OK, "删除 id=2 成功");
    
    json = db_select_where_json(users, &cond, 1);
    if (json) {
        // 现在没有 Alice 记录了
        CHECK(strstr(json, "Alice\",") == NULL, "删除后没有 Alice 记录");
        db_json_free(json);
    }
    
    // 验证表行数
    CHECK(db_table_count(users) == 3, "最终表行数=3 (id=1, id=3, id=4)");
    
    db_close(db);
    
    printf("\n=== 测试结果: %d/%d 通过 ===\n", pass_count, test_count);
    return (pass_count == test_count) ? 0 : 1;
}
