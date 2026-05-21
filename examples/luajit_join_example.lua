#!/usr/bin/env luajit
-- ============================================================
-- mydb LuaJIT FFI JOIN 查询示例
-- ============================================================
-- 展示内存数据库的 JOIN 操作
--
-- 运行方式:
--   luajit examples/luajit_join_example.lua
-- ============================================================

local ffi = require("ffi")
local os = require("os")

local RTLD_NOW = 0x00002
local RTLD_DEEPBIND = 0x00008
local _lib = ffi.load("./libmydb.so", RTLD_NOW + RTLD_DEEPBIND)

ffi.cdef[[
    typedef struct db_instance* db_t;
    typedef struct db_table*    table_t;
    typedef uint64_t            rowid_t;
    
    enum {
        DB_TYPE_INT32 = 0, DB_TYPE_INT64 = 1, DB_TYPE_UINT64 = 2,
        DB_TYPE_FLOAT = 3, DB_TYPE_DOUBLE = 4, DB_TYPE_STRING = 5,
        DB_TYPE_VARSTRING = 6, DB_TYPE_BOOL = 7,
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
    
    db_t db_open(const char* db_dir, size_t pool_size);
    void db_close(db_t db);
    int  db_sync(db_t db);
    
    table_t db_table_register(db_t db, const char* name, size_t row_size,
                              const db_field_def_t* fields, size_t field_count);
    table_t db_table(db_t db, const char* name);
    size_t  db_table_count(table_t table);
    
    rowid_t db_insert(table_t table, const void* row, size_t row_size);
    int db_table_add_index(table_t table, const char* field_name, size_t field_offset, int field_type);
    
    const char* db_select_all_json(table_t table);
    const char* db_select_where_json(table_t table,
                                     const db_condition_t* conditions,
                                     size_t condition_count);
    const char* db_join_json(table_t left_table,  size_t left_field_offset,
                             table_t right_table, size_t right_field_offset,
                             size_t field_size);
    void db_json_free(const char* json);
]]

ffi.cdef[[
    typedef struct {
        int     user_id;
        char    username[32];
        int     department_id;
        double  salary;
    } employee_t;
    
    typedef struct {
        int     dept_id;
        char    dept_name[32];
        char    location[32];
    } department_t;
    
    typedef struct {
        int     order_id;
        int     user_id;
        double  amount;
        char    product[32];
    } order_t;
]]

local function cstring_copy(dest, src, max_len)
    max_len = max_len or ffi.sizeof(dest)
    local src_len = #src
    local len = math.min(src_len, max_len - 1)
    ffi.copy(dest, src, len)
    dest[len] = 0
end

local function print_json(label, json_ptr)
    if json_ptr == nil then
        print(label .. " (nil)")
        return
    end
    print(label .. ": " .. ffi.string(json_ptr))
    _lib.db_json_free(json_ptr)
end

os.execute("rm -rf /tmp/mydb_join_example")

local db = _lib.db_open("/tmp/mydb_join_example", 10 * 1024 * 1024)
if db == nil then error("Failed to open database") end

-- 注册表
local emp_fields = ffi.new("db_field_def_t[4]")
emp_fields[0] = {"user_id",       ffi.offsetof("employee_t", "user_id"),       4,  ffi.C.DB_TYPE_INT32}
emp_fields[1] = {"username",      ffi.offsetof("employee_t", "username"),      32, ffi.C.DB_TYPE_STRING}
emp_fields[2] = {"department_id", ffi.offsetof("employee_t", "department_id"), 4,  ffi.C.DB_TYPE_INT32}
emp_fields[3] = {"salary",        ffi.offsetof("employee_t", "salary"),        8,  ffi.C.DB_TYPE_DOUBLE}

local employees = _lib.db_table_register(db, "employees", ffi.sizeof("employee_t"), emp_fields, 4)
if employees == nil then error("Failed to register employees") end

local dept_fields = ffi.new("db_field_def_t[3]")
dept_fields[0] = {"dept_id",    ffi.offsetof("department_t", "dept_id"),    4,  ffi.C.DB_TYPE_INT32}
dept_fields[1] = {"dept_name",  ffi.offsetof("department_t", "dept_name"),  32, ffi.C.DB_TYPE_STRING}
dept_fields[2] = {"location",   ffi.offsetof("department_t", "location"),   32, ffi.C.DB_TYPE_STRING}

local departments = _lib.db_table_register(db, "departments", ffi.sizeof("department_t"), dept_fields, 3)
if departments == nil then error("Failed to register departments") end

local order_fields = ffi.new("db_field_def_t[4]")
order_fields[0] = {"order_id", ffi.offsetof("order_t", "order_id"), 4,  ffi.C.DB_TYPE_INT32}
order_fields[1] = {"user_id",  ffi.offsetof("order_t", "user_id"),  4,  ffi.C.DB_TYPE_INT32}
order_fields[2] = {"amount",   ffi.offsetof("order_t", "amount"),   8,  ffi.C.DB_TYPE_DOUBLE}
order_fields[3] = {"product",  ffi.offsetof("order_t", "product"),  32, ffi.C.DB_TYPE_STRING}

local orders = _lib.db_table_register(db, "orders", ffi.sizeof("order_t"), order_fields, 4)
if orders == nil then error("Failed to register orders") end

print("Tables registered")

-- 插入数据
local depts = {
    {1, "Engineering", "Building A"},
    {2, "Sales", "Building B"},
    {3, "HR", "Building C"},
}
for _, d in ipairs(depts) do
    local dept = ffi.new("department_t")
    dept.dept_id = d[1]
    cstring_copy(dept.dept_name, d[2], 32)
    cstring_copy(dept.location, d[3], 32)
    _lib.db_insert(departments, dept, ffi.sizeof("department_t"))
end

local emps = {
    {1, "Alice",   1, 120000},
    {2, "Bob",     1, 95000},
    {3, "Carol",   2, 80000},
    {4, "David",   1, 110000},
    {5, "Eve",     3, 70000},
    {6, "Frank",   2, 85000},
}
for _, e in ipairs(emps) do
    local emp = ffi.new("employee_t")
    emp.user_id = e[1]
    cstring_copy(emp.username, e[2], 32)
    emp.department_id = e[3]
    emp.salary = e[4]
    _lib.db_insert(employees, emp, ffi.sizeof("employee_t"))
end

local ords = {
    {101, 1, 999.99,  "Laptop"},
    {102, 1, 49.99,   "Mouse"},
    {103, 2, 199.99,  "Keyboard"},
    {104, 3, 599.99,  "Monitor"},
    {105, 1, 29.99,   "USB Cable"},
    {106, 5, 1499.99, "Workstation"},
}
for _, o in ipairs(ords) do
    local ord = ffi.new("order_t")
    ord.order_id = o[1]
    ord.user_id = o[2]
    ord.amount = o[3]
    cstring_copy(ord.product, o[4], 32)
    _lib.db_insert(orders, ord, ffi.sizeof("order_t"))
end

print(string.format("Inserted %d departments, %d employees, %d orders", #depts, #emps, #ords))

-- ============================================================
-- JOIN 测试用例
-- ============================================================
print("\n=== JOIN Tests ===")

-- 1. 基本 INNER JOIN
print("\n[1] employees × departments (dept_id = department_id)")
print_json("Result",
    _lib.db_join_json(employees,  ffi.offsetof("employee_t", "department_id"),
                      departments, ffi.offsetof("department_t", "dept_id"),
                      4))

-- 2. employees × orders (user_id)
print("\n[2] employees × orders (user_id)")
print_json("Result",
    _lib.db_join_json(employees, ffi.offsetof("employee_t", "user_id"),
                      orders,    ffi.offsetof("order_t", "user_id"),
                      4))

-- 3. 带索引的 JOIN (先建索引)
print("\n[3] Adding indexes...")
_lib.db_table_add_index(employees, "department_id",
                        ffi.offsetof("employee_t", "department_id"), ffi.C.DB_TYPE_INT32)
_lib.db_table_add_index(orders, "user_id",
                        ffi.offsetof("order_t", "user_id"), ffi.C.DB_TYPE_INT32)
print("Indexes created")

print("\n[4] employees × departments (with index)")
print_json("Result",
    _lib.db_join_json(employees,  ffi.offsetof("employee_t", "department_id"),
                      departments, ffi.offsetof("department_t", "dept_id"),
                      4))

-- 4. 验证 JOIN 结果数量
print("\n[5] Verify JOIN counts")
local join_result = _lib.db_join_json(employees, ffi.offsetof("employee_t", "department_id"),
                                       departments, ffi.offsetof("department_t", "dept_id"),
                                       4)
local join_str = ffi.string(join_result)
-- 简单计数: 统计 "employees.user_id" 出现次数
local count = 0
for _ in join_str:gmatch("employees%.user_id") do
    count = count + 1
end
print(" employees × departments 匹配行数:", count)
_lib.db_json_free(join_result)

local join_result2 = _lib.db_join_json(employees, ffi.offsetof("employee_t", "user_id"),
                                        orders, ffi.offsetof("order_t", "user_id"),
                                        4)
local join_str2 = ffi.string(join_result2)
count = 0
for _ in join_str2:gmatch("employees%.user_id") do
    count = count + 1
end
print(" employees × orders 匹配行数:", count)
_lib.db_json_free(join_result2)

-- 5. 应用层多表 JOIN (employees -> departments, employees -> orders)
print("\n[6] Application-layer 3-way JOIN")
-- 先获取 employees × orders
local emp_order_json = _lib.db_join_json(employees, ffi.offsetof("employee_t", "user_id"),
                                          orders, ffi.offsetof("order_t", "user_id"),
                                          4)
print_json("employees × orders", emp_order_json)

-- 统计
print("\nStatistics:")
print(" employees:",    tonumber(_lib.db_table_count(employees)))
print(" departments:",  tonumber(_lib.db_table_count(departments)))
print(" orders:",       tonumber(_lib.db_table_count(orders)))

-- 清理
_lib.db_sync(db)
_lib.db_close(db)

print("\n=== JOIN Example Complete ===")
