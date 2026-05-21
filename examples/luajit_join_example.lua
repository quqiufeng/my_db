#!/usr/bin/env luajit
-- ============================================================
-- mydb LuaJIT FFI JOIN 查询示例
-- ============================================================
-- 展示内存数据库的 JOIN 操作
-- 包括:  INNER JOIN, 条件过滤 JOIN, 多表关联
--
-- 运行方式:
--   luajit examples/luajit_join_example.lua
-- ============================================================

local ffi = require("ffi")
local os = require("os")

-- 加载共享库
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

-- ============================================================
-- 数据结构定义
-- ============================================================

-- 用户表
ffi.cdef[[
    typedef struct {
        int     user_id;
        char    username[32];
        int     department_id;
        double  salary;
    } employee_t;
]]

-- 部门表
ffi.cdef[[
    typedef struct {
        int     dept_id;
        char    dept_name[32];
        char    location[32];
    } department_t;
]]

-- 订单表 (用于三表 JOIN 演示)
ffi.cdef[[
    typedef struct {
        int     order_id;
        int     user_id;
        double  amount;
        char    product[32];
    } order_t;
]]

-- ============================================================
-- 辅助函数
-- ============================================================

local function cstring_copy(dest, src, max_len)
    max_len = max_len or #dest
    local len = math.min(#src, max_len - 1)
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

-- ============================================================
-- 初始化数据库
-- ============================================================

os.execute("rm -rf /tmp/mydb_join_example")

local db = _lib.db_open("/tmp/mydb_join_example", 10 * 1024 * 1024)
if db == nil then error("Failed to open database") end
print("✓ Database opened")

-- 注册 employee 表
local emp_fields = ffi.new("db_field_def_t[4]")
emp_fields[0] = {"user_id",       ffi.offsetof("employee_t", "user_id"),       ffi.sizeof("employee_t", "user_id"),       ffi.C.DB_TYPE_INT32}
emp_fields[1] = {"username",      ffi.offsetof("employee_t", "username"),      ffi.sizeof("employee_t", "username"),      ffi.C.DB_TYPE_STRING}
emp_fields[2] = {"department_id", ffi.offsetof("employee_t", "department_id"), ffi.sizeof("employee_t", "department_id"), ffi.C.DB_TYPE_INT32}
emp_fields[3] = {"salary",        ffi.offsetof("employee_t", "salary"),        ffi.sizeof("employee_t", "salary"),        ffi.C.DB_TYPE_DOUBLE}

local employees = _lib.db_table_register(db, "employees", ffi.sizeof("employee_t"), emp_fields, 4)
if employees == nil then error("Failed to register employees") end
print("✓ Table 'employees' registered")

-- 注册 department 表
local dept_fields = ffi.new("db_field_def_t[3]")
dept_fields[0] = {"dept_id",    ffi.offsetof("department_t", "dept_id"),    ffi.sizeof("department_t", "dept_id"),    ffi.C.DB_TYPE_INT32}
dept_fields[1] = {"dept_name",  ffi.offsetof("department_t", "dept_name"),  ffi.sizeof("department_t", "dept_name"),  ffi.C.DB_TYPE_STRING}
dept_fields[2] = {"location",   ffi.offsetof("department_t", "location"),   ffi.sizeof("department_t", "location"),   ffi.C.DB_TYPE_STRING}

local departments = _lib.db_table_register(db, "departments", ffi.sizeof("department_t"), dept_fields, 3)
if departments == nil then error("Failed to register departments") end
print("✓ Table 'departments' registered")

-- 注册 orders 表
local order_fields = ffi.new("db_field_def_t[4]")
order_fields[0] = {"order_id", ffi.offsetof("order_t", "order_id"), ffi.sizeof("order_t", "order_id"), ffi.C.DB_TYPE_INT32}
order_fields[1] = {"user_id",  ffi.offsetof("order_t", "user_id"),  ffi.sizeof("order_t", "user_id"),  ffi.C.DB_TYPE_INT32}
order_fields[2] = {"amount",   ffi.offsetof("order_t", "amount"),   ffi.sizeof("order_t", "amount"),   ffi.C.DB_TYPE_DOUBLE}
order_fields[3] = {"product",  ffi.offsetof("order_t", "product"),  ffi.sizeof("order_t", "product"),  ffi.C.DB_TYPE_STRING}

local orders = _lib.db_table_register(db, "orders", ffi.sizeof("order_t"), order_fields, 4)
if orders == nil then error("Failed to register orders") end
print("✓ Table 'orders' registered")

-- 创建索引加速 JOIN
_lib.db_table_add_index(employees, "department_id", ffi.offsetof("employee_t", "department_id"), ffi.C.DB_TYPE_INT32)
_lib.db_table_add_index(orders, "user_id", ffi.offsetof("order_t", "user_id"), ffi.C.DB_TYPE_INT32)
print("✓ Indexes created")

-- ============================================================
-- 插入测试数据
-- ============================================================
print("\n--- Inserting Data ---")

-- 部门数据
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
print(string.format("✓ Inserted %d departments", #depts))

-- 员工数据
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
print(string.format("✓ Inserted %d employees", #emps))

-- 订单数据
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
print(string.format("✓ Inserted %d orders", #ords))

-- ============================================================
-- JOIN 查询
-- ============================================================
print("\n--- JOIN Queries ---")

-- 1. 基本 INNER JOIN: employees JOIN departments
-- 关联条件: employees.department_id == departments.dept_id
print("\n[1] INNER JOIN: employees × departments")
print("    ON employees.department_id = departments.dept_id")
print_json("Result",
    _lib.db_join_json(employees,  ffi.offsetof("employee_t", "department_id"),
                      departments, ffi.offsetof("department_t", "dept_id"),
                      ffi.sizeof("employee_t", "department_id")))

-- 2. 带 WHERE 过滤的 JOIN
-- 先过滤 employees 表，只保留 Engineering 部门的员工，再 JOIN
print("\n[2] Filtered JOIN: Engineering department only")
local cond = ffi.new("db_condition_t[1]")
cond[0].field_offset = ffi.offsetof("employee_t", "department_id")
cond[0].field_size = ffi.sizeof("employee_t", "department_id")
local dept_id_val = ffi.new("int[1]", 1)  -- Engineering
cond[0].value = dept_id_val
cond[0].op = 0  -- EQ

print_json("Engineering employees",
    _lib.db_select_where_json(employees, cond, 1))

print_json("JOIN result (Engineering)",
    _lib.db_join_json(employees,  ffi.offsetof("employee_t", "department_id"),
                      departments, ffi.offsetof("department_t", "dept_id"),
                      ffi.sizeof("employee_t", "department_id")))

-- 3. 多表 JOIN (应用程序层实现)
-- 数据库原生支持两表 JOIN，三表 JOIN 可以通过两次 JOIN 或应用层处理
print("\n[3] Multi-table JOIN: employees × departments × orders")
print("    (Application-layer implementation)")

-- 先获取 employees × departments 结果
local emp_dept_json = _lib.db_join_json(employees,  ffi.offsetof("employee_t", "department_id"),
                                         departments, ffi.offsetof("department_t", "dept_id"),
                                         ffi.sizeof("employee_t", "department_id"))

if emp_dept_json ~= nil then
    local emp_dept_str = ffi.string(emp_dept_json)
    print("  employees × departments result available")
    _lib.db_json_free(emp_dept_json)
end

-- 再获取 employees × orders 结果 (通过 user_id)
local emp_order_json = _lib.db_join_json(employees, ffi.offsetof("employee_t", "user_id"),
                                          orders,    ffi.offsetof("order_t", "user_id"),
                                          ffi.sizeof("employee_t", "user_id"))

if emp_order_json ~= nil then
    local emp_order_str = ffi.string(emp_order_json)
    print("  employees × orders result:")
    print("  " .. emp_order_str)
    _lib.db_json_free(emp_order_json)
end

-- 4. 高级: 高工资员工 JOIN 部门
print("\n[4] High-salary employees JOIN departments")
local salary_cond = ffi.new("db_condition_t[1]")
salary_cond[0].field_offset = ffi.offsetof("employee_t", "salary")
salary_cond[0].field_size = ffi.sizeof("employee_t", "salary")
local salary_val = ffi.new("double[1]", 100000)
salary_cond[0].value = salary_val
salary_cond[0].op = 1  -- GT

print_json("High salary employees (>100k)",
    _lib.db_select_where_json(employees, salary_cond, 1))

print_json("JOIN result (high salary)",
    _lib.db_join_json(employees,  ffi.offsetof("employee_t", "department_id"),
                      departments, ffi.offsetof("department_t", "dept_id"),
                      ffi.sizeof("employee_t", "department_id")))

-- ============================================================
-- 统计
-- ============================================================
print("\n--- Statistics ---")
print(string.format("Employees:    %d", tonumber(_lib.db_table_count(employees))))
print(string.format("Departments:  %d", tonumber(_lib.db_table_count(departments))))
print(string.format("Orders:       %d", tonumber(_lib.db_table_count(orders))))

-- ============================================================
-- 清理
-- ============================================================
print("\n--- Cleanup ---")
_lib.db_sync(db)
_lib.db_close(db)
print("✓ Database closed")

print("\n=== JOIN Example Complete ===")
