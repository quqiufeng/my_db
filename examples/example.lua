-- my_db LuaJIT FFI 示例
local ffi = require("ffi")
local mydb = ffi.load("./libmydb.so")

ffi.cdef[[
    typedef struct db_instance* db_t;
    typedef struct db_table*    table_t;
    typedef uint64_t            rowid_t;
    
    enum db_field_type {
        DB_TYPE_INT32, DB_TYPE_INT64, DB_TYPE_UINT64,
        DB_TYPE_FLOAT, DB_TYPE_DOUBLE, DB_TYPE_STRING, DB_TYPE_BOOL
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
    
    db_t db_open(const char* dp, const char* ip, const char* wp, size_t ps);
    void db_close(db_t db);
    table_t db_table_register(db_t db, const char* name, size_t rs,
                               const db_field_def_t* fields, size_t fc);
    table_t db_table(db_t db, const char* name);
    int db_table_add_index(table_t t, const char* fn, size_t fo, int ft);
    rowid_t db_insert(table_t t, const void* row, size_t rs);
    const char* db_select_all_json(table_t t);
    const char* db_select_by_pk_json(table_t t, rowid_t id);
    void db_json_free(const char* json);
    int db_checkpoint(db_t db);
    
    struct ex_user {
        uint64_t id;
        char     name[32];
        int32_t  age;
        double   score;
    };
]]

print("=== my_db LuaJIT FFI 示例 ===\n")

-- 打开数据库
local db = mydb.db_open("ex_lua.bin", "ex_lua.idx", "ex_lua.wal", 1024*1024*10)
print("1. 数据库打开成功")

-- 注册表
local fields = ffi.new("db_field_def_t[4]")
fields[0] = {"id", 0, 8, ffi.C.DB_TYPE_UINT64}
fields[1] = {"name", 8, 32, ffi.C.DB_TYPE_STRING}
fields[2] = {"age", 40, 4, ffi.C.DB_TYPE_INT32}
fields[3] = {"score", 48, 8, ffi.C.DB_TYPE_DOUBLE}

local users = mydb.db_table_register(db, "users", ffi.sizeof("struct ex_user"), fields, 4)
print("2. Schema 注册成功")

-- 创建索引
mydb.db_table_add_index(users, "name", 8, ffi.C.DB_TYPE_STRING)
print("3. 创建 name 索引成功")

-- 插入数据
local u = ffi.new("struct ex_user")
u.id = 0
ffi.copy(u.name, "Alice")
u.age = 25
u.score = 95.5

local id1 = mydb.db_insert(users, u, ffi.sizeof("struct ex_user"))
print("4. 插入 Alice, id=" .. tonumber(id1))

-- 查询
local json = mydb.db_select_all_json(users)
if json ~= nil then
    print("5. 全表查询: " .. ffi.string(json))
    mydb.db_json_free(json)
end

-- Checkpoint
mydb.db_checkpoint(db)
print("6. Checkpoint 完成")

-- 关闭
mydb.db_close(db)
print("7. 数据库关闭")

print("\n=== 示例完成 ===")
