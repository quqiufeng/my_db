-- my_db LuaJIT FFI 测试
local ffi = require("ffi")
local C = ffi.C

-- 加载动态库
local mydb = ffi.load("./libmydb.so")

-- 定义 C 结构（与 C 头文件保持一致）
ffi.cdef[[
    typedef struct db_instance* db_t;
    typedef struct db_table*    table_t;
    typedef uint64_t            rowid_t;
    
    enum {
        DB_OK                    =  0,
        DB_ERR_NOMEM             = -1,
        DB_ERR_IO                = -2,
        DB_ERR_NOENT             = -3,
        DB_ERR_EXIST             = -4,
        DB_ERR_INVAL             = -5,
        DB_ERR_RESULT_TOO_LARGE  = -6,
        DB_ERR_CORRUPTED         = -7,
        DB_ERR_WAL_REPLAY        = -8,
    };
    
    enum db_field_type {
        DB_TYPE_INT32,
        DB_TYPE_INT64,
        DB_TYPE_UINT64,
        DB_TYPE_FLOAT,
        DB_TYPE_DOUBLE,
        DB_TYPE_STRING,
        DB_TYPE_BOOL,
    };
    
    typedef struct {
        const char* name;
        size_t      offset;
        size_t      size;
        int         type;
    } db_field_def_t;
    
    typedef struct {
        size_t      field_offset;
        size_t      field_size;
        const void* value;
        int         op;
    } db_condition_t;
    
    db_t db_open(const char* data_path, const char* index_path, const char* wal_path, size_t pool_size);
    void db_close(db_t db);
    int  db_sync(db_t db);
    const char* db_errstr(db_t db);
    void db_config_max_rows(db_t db, size_t max_rows);
    int db_wal_replay(db_t db);
    
    table_t db_table_register(db_t db, const char* name, size_t row_size,
                              const db_field_def_t* fields, size_t field_count);
    table_t db_table(db_t db, const char* name);
    size_t  db_table_count(table_t table);
    
    rowid_t db_insert(table_t table, const void* row, size_t row_size);
    int db_update(table_t table, rowid_t id, const void* row, size_t row_size);
    int db_delete(table_t table, rowid_t id);
    
    const char* db_select_by_pk_json(table_t table, rowid_t id);
    const char* db_select_all_json(table_t table);
    const char* db_select_where_json(table_t table,
                                     const db_condition_t* conditions,
                                     size_t condition_count);
    void db_json_free(const char* json);
]]

-- 定义 Lua 侧的 struct（必须与 C 的 struct user 完全一致）
ffi.cdef[[
    struct ffi_user {
        uint64_t id;
        char     name[32];
        int32_t  age;
        double   score;
    };
]]

local test_count = 0
local pass_count = 0

local function check(cond, msg)
    test_count = test_count + 1
    if cond then
        pass_count = pass_count + 1
        print("[OK] " .. msg)
    else
        print("[FAIL] " .. msg)
    end
end

print("=== my_db LuaJIT FFI 测试 ===\n")

-- 打开数据库
local db = mydb.db_open("ffi_data.bin", "ffi_index.index", "ffi_wal.bin", 1024*1024*10)
check(db ~= nil, "数据库打开成功")

-- 注册表结构
local fields = ffi.new("db_field_def_t[4]")
fields[0].name = "id"
fields[0].offset = 0
fields[0].size = 8
fields[0].type = ffi.C.DB_TYPE_UINT64

fields[1].name = "name"
fields[1].offset = 8
fields[1].size = 32
fields[1].type = ffi.C.DB_TYPE_STRING

fields[2].name = "age"
fields[2].offset = 40
fields[2].size = 4
fields[2].type = ffi.C.DB_TYPE_INT32

fields[3].name = "score"
fields[3].offset = 48
fields[3].size = 8
fields[3].type = ffi.C.DB_TYPE_DOUBLE

local users = mydb.db_table_register(db, "ffi_users", ffi.sizeof("struct ffi_user"), fields, 4)
check(users ~= nil, "Schema 注册成功")

-- 插入数据
local u1 = ffi.new("struct ffi_user")
u1.id = 0
ffi.copy(u1.name, "Alice", 5)
u1.age = 25
u1.score = 95.5

local id1 = mydb.db_insert(users, u1, ffi.sizeof("struct ffi_user"))
check(id1 == 1, "插入 Alice, id=1")

local u2 = ffi.new("struct ffi_user")
u2.id = 0
ffi.copy(u2.name, "Bob", 3)
u2.age = 30
u2.score = 88.0

local id2 = mydb.db_insert(users, u2, ffi.sizeof("struct ffi_user"))
check(id2 == 2, "插入 Bob, id=2")

-- 查询单条
local json1 = mydb.db_select_by_pk_json(users, id1)
check(json1 ~= nil, "主键查询成功")
if json1 ~= nil then
    local s = ffi.string(json1)
    check(s:find("Alice") ~= nil, "查询结果包含 'Alice'")
    mydb.db_json_free(json1)
end

-- 全表查询
local all_json = mydb.db_select_all_json(users)
check(all_json ~= nil, "全表查询成功")
if all_json ~= nil then
    local s = ffi.string(all_json)
    check(s:find("Alice") ~= nil, "全表结果包含 Alice")
    check(s:find("Bob") ~= nil, "全表结果包含 Bob")
    mydb.db_json_free(all_json)
end

-- WHERE 查询
local age_val = ffi.new("int32_t[1]", 20)
local cond = ffi.new("db_condition_t[1]")
cond[0].field_offset = 40  -- age offset
cond[0].field_size = 4
cond[0].value = age_val
cond[0].op = 1  -- >

local where_json = mydb.db_select_where_json(users, cond, 1)
check(where_json ~= nil, "WHERE 查询成功")
if where_json ~= nil then
    local s = ffi.string(where_json)
    check(s:find("Alice") ~= nil, "WHERE 结果包含 Alice (age 25 > 20)")
    check(s:find("Bob") ~= nil, "WHERE 结果包含 Bob (age 30 > 20)")
    mydb.db_json_free(where_json)
end

-- 更新
local u1_new = ffi.new("struct ffi_user")
u1_new.id = id1
ffi.copy(u1_new.name, "Alice Updated", 13)
u1_new.age = 26
u1_new.score = 96.0

local ret = mydb.db_update(users, id1, u1_new, ffi.sizeof("struct ffi_user"))
check(ret == 0, "更新 Alice 成功")

local updated_json = mydb.db_select_by_pk_json(users, id1)
if updated_json ~= nil then
    local s = ffi.string(updated_json)
    check(s:find("Alice Updated") ~= nil, "更新后查询结果正确")
    mydb.db_json_free(updated_json)
end

-- 删除
ret = mydb.db_delete(users, id2)
check(ret == 0, "删除 Bob 成功")

local count = mydb.db_table_count(users)
check(count == 1, "删除后表行数=1")

-- 关闭数据库
mydb.db_close(db)

print("\n=== 测试结果: " .. pass_count .. "/" .. test_count .. " 通过 ===")

if pass_count ~= test_count then
    os.exit(1)
end
