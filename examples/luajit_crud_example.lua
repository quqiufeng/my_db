#!/usr/bin/env luajit
-- ============================================================
-- mydb LuaJIT FFI CRUD 示例
-- ============================================================
-- 展示如何使用 LuaJIT FFI 直接操作内存数据库
-- 包括: INSERT, SELECT, UPDATE, DELETE, 批量插入, 事务
--
-- 运行方式:
--   luajit examples/luajit_crud_example.lua
--   或: chmod +x examples/luajit_crud_example.lua && ./examples/luajit_crud_example.lua
-- ============================================================

local ffi = require("ffi")
local os = require("os")

-- 加载共享库 (使用 RTLD_DEEPBIND 隔离符号冲突)
local RTLD_NOW = 0x00002
local RTLD_DEEPBIND = 0x00008
local _lib = ffi.load("./libmydb.so", RTLD_NOW + RTLD_DEEPBIND)

-- ============================================================
-- 1. FFI C 结构定义 (零拷贝架构核心)
-- ============================================================
-- 在 LuaJIT 中定义 C struct，内存布局与 C 代码完全一致
-- 插入时直接传递 struct 指针，无序列化/反序列化开销

ffi.cdef[[
    typedef struct db_instance* db_t;
    typedef struct db_table*    table_t;
    typedef uint64_t            rowid_t;
    
    // 字段类型枚举
    enum {
        DB_TYPE_INT32      = 0,
        DB_TYPE_INT64      = 1,
        DB_TYPE_UINT64     = 2,
        DB_TYPE_FLOAT      = 3,
        DB_TYPE_DOUBLE     = 4,
        DB_TYPE_STRING     = 5,
        DB_TYPE_VARSTRING  = 6,
        DB_TYPE_BOOL       = 7,
    };
    
    // 字段定义
    typedef struct {
        const char* name;
        size_t      offset;
        size_t      size;
        int         type;
    } db_field_def_t;
    
    // WHERE 条件
    typedef struct {
        size_t      field_offset;
        size_t      field_size;
        const void* value;
        int         op;  // 0=EQ, 1=GT, 2=LT
    } db_condition_t;
    
    // 数据库生命周期
    db_t db_open(const char* db_dir, size_t pool_size);
    void db_close(db_t db);
    int  db_sync(db_t db);
    int  db_check(const char* db_dir);
    
    // Schema 注册
    table_t db_table_register(db_t db, const char* name, size_t row_size,
                              const db_field_def_t* fields, size_t field_count);
    table_t db_table(db_t db, const char* name);
    size_t  db_table_count(table_t table);
    
    // CRUD
    rowid_t db_insert(table_t table, const void* row, size_t row_size);
    size_t  db_batch_insert(table_t table, const void* rows, size_t row_size, size_t count);
    int     db_update(table_t table, rowid_t id, const void* row, size_t row_size);
    int     db_delete(table_t table, rowid_t id);
    
    // 查询 (JSON 模式)
    const char* db_select_by_pk_json(table_t table, rowid_t id);
    const char* db_select_all_json(table_t table);
    const char* db_select_where_json(table_t table,
                                     const db_condition_t* conditions,
                                     size_t condition_count);
    const char* db_select_json(table_t table,
                               const db_condition_t* conditions, size_t condition_count,
                               size_t order_field_offset, int ascending,
                               size_t limit_offset, size_t limit_count);
    void db_json_free(const char* json);
    
    // 事务
    int db_begin(db_t db);
    int db_commit(db_t db);
    int db_rollback(db_t db);
    
    // 压缩
    size_t db_table_compact(table_t table);
]]

-- ============================================================
-- 2. 定义业务数据结构 (C struct)
-- ============================================================
-- 这是一个 "用户" 表结构，与 C 侧的 struct 内存布局完全兼容
-- 使用 ffi.new() 创建的 struct 直接位于 LuaJIT 的 GC 内存中
-- 传递给 db_insert() 时，直接传指针，零拷贝

ffi.cdef[[
    typedef struct {
        int     id;
        char    name[32];
        char    email[64];
        int     age;
        double  score;
    } user_t;
]]

-- ============================================================
-- 3. 辅助函数
-- ============================================================

-- 安全地复制字符串到 C struct 的固定长度字符数组
local function cstring_copy(dest, src, max_len)
    max_len = max_len or ffi.sizeof(dest)
    local src_len = #src
    local len = math.min(src_len, max_len - 1)
    ffi.copy(dest, src, len)
    dest[len] = 0  -- null terminator
end

-- 打印 JSON 结果
local function print_json(label, json_ptr)
    if json_ptr == nil then
        print(label .. " (nil)")
        return
    end
    print(label .. ": " .. ffi.string(json_ptr))
    _lib.db_json_free(json_ptr)
end

-- ============================================================
-- 4. 初始化数据库
-- ============================================================

-- 清理旧数据
os.execute("rm -rf /tmp/mydb_crud_example")

-- 打开数据库 (内存池 10MB)
local db = _lib.db_open("/tmp/mydb_crud_example", 10 * 1024 * 1024)
if db == nil then
    error("Failed to open database")
end
print("✓ Database opened: /tmp/mydb_crud_example")

-- 定义 user_t 表的 schema
-- 注意: offset 必须与 C struct 中的字段偏移一致
local fields = ffi.new("db_field_def_t[5]")
fields[0].name = "id"
fields[0].offset = ffi.offsetof("user_t", "id")
fields[0].size = ffi.sizeof("user_t", "id")
fields[0].type = ffi.C.DB_TYPE_INT32

fields[1].name = "name"
fields[1].offset = ffi.offsetof("user_t", "name")
fields[1].size = ffi.sizeof("user_t", "name")
fields[1].type = ffi.C.DB_TYPE_STRING

fields[2].name = "email"
fields[2].offset = ffi.offsetof("user_t", "email")
fields[2].size = ffi.sizeof("user_t", "email")
fields[2].type = ffi.C.DB_TYPE_STRING

fields[3].name = "age"
fields[3].offset = ffi.offsetof("user_t", "age")
fields[3].size = ffi.sizeof("user_t", "age")
fields[3].type = ffi.C.DB_TYPE_INT32

fields[4].name = "score"
fields[4].offset = ffi.offsetof("user_t", "score")
fields[4].size = ffi.sizeof("user_t", "score")
fields[4].type = ffi.C.DB_TYPE_DOUBLE

-- 注册表
local users = _lib.db_table_register(db, "users", ffi.sizeof("user_t"), fields, 5)
if users == nil then
    error("Failed to register table")
end
print("✓ Table 'users' registered")

-- ============================================================
-- 5. INSERT 操作
-- ============================================================
print("\n--- INSERT ---")

-- 单条插入
local user1 = ffi.new("user_t")
user1.id = 1
user1.age = 25
user1.score = 95.5
cstring_copy(user1.name, "Alice", 32)
cstring_copy(user1.email, "alice@example.com", 64)

local rowid1 = _lib.db_insert(users, user1, ffi.sizeof("user_t"))
print(string.format("✓ Inserted user1, rowid=%d", tonumber(rowid1)))

-- 批量插入 (零拷贝: 连续内存数组直接传递)
local batch = ffi.new("user_t[3]")
batch[0].id = 2; batch[0].age = 30; batch[0].score = 88.0
cstring_copy(batch[0].name, "Bob", 32)
cstring_copy(batch[0].email, "bob@test.com", 64)

batch[1].id = 3; batch[1].age = 22; batch[1].score = 92.5
cstring_copy(batch[1].name, "Carol", 32)
cstring_copy(batch[1].email, "carol@dev.io", 64)

batch[2].id = 4; batch[2].age = 35; batch[2].score = 78.5
cstring_copy(batch[2].name, "David", 32)
cstring_copy(batch[2].email, "david@corp.com", 64)

local inserted = _lib.db_batch_insert(users, batch, ffi.sizeof("user_t"), 3)
print(string.format("✓ Batch inserted %d users", tonumber(inserted)))

-- ============================================================
-- 6. SELECT 操作
-- ============================================================
print("\n--- SELECT ---")

-- 查询所有
print_json("All users", _lib.db_select_all_json(users))

-- 按主键查询
print_json("User by PK (rowid=1)", _lib.db_select_by_pk_json(users, 1))

-- WHERE 条件查询: age > 25
local cond = ffi.new("db_condition_t[1]")
cond[0].field_offset = ffi.offsetof("user_t", "age")
cond[0].field_size = ffi.sizeof("user_t", "age")
local age_val = ffi.new("int[1]", 25)
cond[0].value = age_val
cond[0].op = 1  -- GT (大于)

print_json("Users age > 25", _lib.db_select_where_json(users, cond, 1))

-- 高级查询: score >= 90, 按 score 降序, 限制 2 条
local cond2 = ffi.new("db_condition_t[1]")
cond2[0].field_offset = ffi.offsetof("user_t", "score")
cond2[0].field_size = ffi.sizeof("user_t", "score")
local score_val = ffi.new("double[1]", 90.0)
cond2[0].value = score_val
cond2[0].op = 1  -- GT

print_json("Top 2 by score",
    _lib.db_select_json(users, cond2, 1,
                        ffi.offsetof("user_t", "score"), 0,  -- order by score, desc
                        0, 2))  -- limit 2

-- ============================================================
-- 7. UPDATE 操作
-- ============================================================
print("\n--- UPDATE ---")

local update_user = ffi.new("user_t")
update_user.id = 1
update_user.age = 26
update_user.score = 97.0
cstring_copy(update_user.name, "Alice Updated", 32)
cstring_copy(update_user.email, "alice.new@example.com", 64)

local ret = _lib.db_update(users, 1, update_user, ffi.sizeof("user_t"))
if ret == 0 then
    print("✓ Updated rowid=1")
    print_json("After update", _lib.db_select_by_pk_json(users, 1))
else
    print("✗ Update failed: " .. ret)
end

-- ============================================================
-- 8. DELETE 操作
-- ============================================================
print("\n--- DELETE ---")

ret = _lib.db_delete(users, 4)  -- 删除 David
if ret == 0 then
    print("✓ Deleted rowid=4 (David)")
    print_json("After delete", _lib.db_select_all_json(users))
else
    print("✗ Delete failed: " .. ret)
end

-- ============================================================
-- 9. 事务操作
-- ============================================================
print("\n--- TRANSACTION ---")

-- BEGIN
ret = _lib.db_begin(db)
if ret ~= 0 then error("BEGIN failed: " .. ret) end
print("✓ BEGIN transaction")

-- 在事务中插入
local tx_user = ffi.new("user_t")
tx_user.id = 99; tx_user.age = 40; tx_user.score = 100.0
cstring_copy(tx_user.name, "Transaction User", 32)
cstring_copy(tx_user.email, "tx@test.com", 64)

local tx_rowid = _lib.db_insert(users, tx_user, ffi.sizeof("user_t"))
print(string.format("✓ Inserted in TX, rowid=%d", tonumber(tx_rowid)))
print_json("In TX (before commit)", _lib.db_select_all_json(users))

-- ROLLBACK (撤销)
ret = _lib.db_rollback(db)
if ret ~= 0 then error("ROLLBACK failed: " .. ret) end
print("✓ ROLLBACK transaction")
print_json("After rollback", _lib.db_select_all_json(users))

-- 再次 BEGIN + COMMIT
_lib.db_begin(db)
tx_user.id = 100
local tx_rowid2 = _lib.db_insert(users, tx_user, ffi.sizeof("user_t"))
ret = _lib.db_commit(db)
if ret ~= 0 then error("COMMIT failed: " .. ret) end
print("✓ COMMIT transaction")
print_json("After commit", _lib.db_select_all_json(users))

-- ============================================================
-- 10. 统计与压缩
-- ============================================================
print("\n--- STATISTICS ---")
print(string.format("Table row count: %d", tonumber(_lib.db_table_count(users))))

local freed = _lib.db_table_compact(users)
print(string.format("Compact freed: %d bytes", tonumber(freed)))

-- ============================================================
-- 11. 关闭数据库 (数据持久化到磁盘)
-- ============================================================
print("\n--- CLEANUP ---")
_lib.db_sync(db)  -- 强制刷盘
print("✓ Database synced to disk")

_lib.db_close(db)
print("✓ Database closed")

print("\n=== CRUD Example Complete ===")
print("Data persisted to: /tmp/mydb_crud_example/")
print("You can re-open this directory to verify zero-copy reload")
