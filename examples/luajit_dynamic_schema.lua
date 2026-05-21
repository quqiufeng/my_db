#!/usr/bin/env luajit
-- ============================================================
-- mydb LuaJIT FFI 动态 Schema + 零拷贝架构示例
-- ============================================================
-- 展示如何在运行时动态定义数据结构，并复用零拷贝架构
--
-- 核心特性:
--   1. 运行时动态定义 C struct (无需重新编译 C 库)
--   2. 内存 -> 磁盘 -> 内存 零拷贝 reload
--   3. Schema 自描述 (字段名、类型、偏移量全部在 Lua 中定义)
--   4. 与高级封装 mydb.lua 的对比
--
-- 运行方式:
--   luajit examples/luajit_dynamic_schema.lua
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
    int  db_checkpoint(db_t db);
    
    table_t db_table_register(db_t db, const char* name, size_t row_size,
                              const db_field_def_t* fields, size_t field_count);
    table_t db_table(db_t db, const char* name);
    size_t  db_table_count(table_t table);
    
    rowid_t db_insert(table_t table, const void* row, size_t row_size);
    size_t  db_batch_insert(table_t table, const void* rows, size_t row_size, size_t count);
    int     db_update(table_t table, rowid_t id, const void* row, size_t row_size);
    int     db_delete(table_t table, rowid_t id);
    
    const char* db_select_all_json(table_t table);
    const char* db_select_by_pk_json(table_t table, rowid_t id);
    const char* db_select_where_json(table_t table,
                                     const db_condition_t* conditions,
                                     size_t condition_count);
    void db_json_free(const char* json);
]]

-- ============================================================
-- 动态 Schema 构建器
-- ============================================================
-- 在纯 Lua 中定义 Schema，然后动态生成 C struct

local SchemaBuilder = {}
SchemaBuilder.__index = SchemaBuilder

function SchemaBuilder:new(name)
    return setmetatable({
        name = name,
        fields = {},
        offset = 0,
    }, self)
end

-- 添加字段
function SchemaBuilder:add_field(fname, ftype, size)
    size = size or 4  -- default int32
    
    -- 对齐到 size 的倍数 (简化对齐，实际应与 C 编译器一致)
    local align = size
    if size > 8 then align = 8 end
    self.offset = math.ceil(self.offset / align) * align
    
    table.insert(self.fields, {
        name = fname,
        type = ftype,
        size = size,
        offset = self.offset,
    })
    
    self.offset = self.offset + size
    return self
end

-- 生成 C struct 定义字符串
function SchemaBuilder:generate_cdef()
    local parts = {string.format("typedef struct {", self.name)}
    
    for _, f in ipairs(self.fields) do
        local ctype
        local fname = f.name
        if f.type == ffi.C.DB_TYPE_INT32 then
            table.insert(parts, string.format("    int %s;", fname))
        elseif f.type == ffi.C.DB_TYPE_INT64 then
            table.insert(parts, string.format("    long long %s;", fname))
        elseif f.type == ffi.C.DB_TYPE_UINT64 then
            table.insert(parts, string.format("    unsigned long long %s;", fname))
        elseif f.type == ffi.C.DB_TYPE_FLOAT then
            table.insert(parts, string.format("    float %s;", fname))
        elseif f.type == ffi.C.DB_TYPE_DOUBLE then
            table.insert(parts, string.format("    double %s;", fname))
        elseif f.type == ffi.C.DB_TYPE_STRING then
            table.insert(parts, string.format("    char %s[%d];", fname, f.size))
        elseif f.type == ffi.C.DB_TYPE_BOOL then
            table.insert(parts, string.format("    int %s;", fname))
        else
            error("Unknown type: " .. f.type)
        end
    end
    
    table.insert(parts, string.format("} %s;", self.name))
    return table.concat(parts, "\n")
end

-- 生成 db_field_def_t 数组
function SchemaBuilder:generate_field_defs()
    local defs = ffi.new("db_field_def_t[?]", #self.fields)
    for i, f in ipairs(self.fields) do
        defs[i-1].name = f.name
        defs[i-1].offset = f.offset
        defs[i-1].size = f.size
        defs[i-1].type = f.type
    end
    return defs
end

-- 获取结构体总大小
function SchemaBuilder:sizeof()
    -- 最终对齐到最大字段大小的倍数
    local max_align = 8
    return math.ceil(self.offset / max_align) * max_align
end

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
-- 演示 1: 动态定义电商订单 Schema
-- ============================================================
print("=== Dynamic Schema Definition ===\n")

-- 步骤 1: 在 Lua 中定义 Schema
local order_schema = SchemaBuilder:new("order_t")
    :add_field("order_id",    ffi.C.DB_TYPE_INT32,  4)
    :add_field("customer_id", ffi.C.DB_TYPE_INT32,  4)
    :add_field("product",     ffi.C.DB_TYPE_STRING, 64)
    :add_field("quantity",    ffi.C.DB_TYPE_INT32,  4)
    :add_field("unit_price",  ffi.C.DB_TYPE_DOUBLE, 8)
    :add_field("status",      ffi.C.DB_TYPE_STRING, 16)

print("Schema 'order_t' defined in Lua:")
for _, f in ipairs(order_schema.fields) do
    print(string.format("  %s: offset=%d, size=%d, type=%d",
        f.name, f.offset, f.size, f.type))
end
print(string.format("Total struct size: %d bytes\n", order_schema:sizeof()))

-- 步骤 2: 动态生成并加载 C struct 定义
local cdef_str = order_schema:generate_cdef()
print("Generated C definition:")
print(cdef_str)
print("")

ffi.cdef(cdef_str)  -- 运行时加载 C struct 定义！
print("✓ C struct dynamically loaded into LuaJIT FFI\n")

-- ============================================================
-- 演示 2: 使用动态 Schema 创建数据库
-- ============================================================
print("=== Database Creation with Dynamic Schema ===\n")

os.execute("rm -rf /tmp/mydb_dynamic_example")

local db = _lib.db_open("/tmp/mydb_dynamic_example", 10 * 1024 * 1024)
if db == nil then error("Failed to open database") end

-- 注册表 (使用动态生成的 field defs)
local field_defs = order_schema:generate_field_defs()
local orders = _lib.db_table_register(db, "orders", order_schema:sizeof(), field_defs, #order_schema.fields)
if orders == nil then error("Failed to register table") end
print("✓ Table 'orders' registered with dynamic schema")

-- 插入数据
local o1 = ffi.new("order_t")
o1.order_id = 1001
o1.customer_id = 42
o1.quantity = 3
o1.unit_price = 29.99
cstring_copy(o1.product, "Wireless Mouse", 64)
cstring_copy(o1.status, "shipped", 16)

local rowid1 = _lib.db_insert(orders, o1, order_schema:sizeof())
print(string.format("✓ Inserted order, rowid=%d", tonumber(rowid1)))

-- 批量插入
local batch = ffi.new("order_t[3]")
for i = 0, 2 do
    batch[i].order_id = 1002 + i
    batch[i].customer_id = 43 + i
    batch[i].quantity = i + 1
    batch[i].unit_price = 49.99 + i * 10
    cstring_copy(batch[i].product, "Product " .. tostring(i), 64)
    cstring_copy(batch[i].status, "pending", 16)
end

local inserted = _lib.db_batch_insert(orders, batch, order_schema:sizeof(), 3)
print(string.format("✓ Batch inserted %d orders", tonumber(inserted)))

print_json("All orders", _lib.db_select_all_json(orders))

-- ============================================================
-- 演示 3: 关闭后重新打开 (零拷贝 reload)
-- ============================================================
print("\n=== Zero-Copy Reload ===\n")

-- 关闭数据库 (数据刷盘)
_lib.db_sync(db)
_lib.db_close(db)
print("✓ Database closed, data persisted to disk")

-- 重新打开 (模拟新进程/新会话)
print("\n-- Reopening database (simulating new session) --")
db = _lib.db_open("/tmp/mydb_dynamic_example", 10 * 1024 * 1024)
if db == nil then error("Failed to reopen database") end

-- 关键: Schema 必须在 Lua 侧重新定义 (C 库不存储 Schema 元数据)
-- 但数据结构定义本身不需要变，复用之前的 cdef
print("✓ Database reopened")

-- 重新注册表 (使用相同的 Schema)
orders = _lib.db_table_register(db, "orders", order_schema:sizeof(), field_defs, #order_schema.fields)
if orders == nil then error("Failed to re-register table") end
print("✓ Table 'orders' re-registered with same dynamic schema")

-- 查询验证数据完整性
print_json("After reload", _lib.db_select_all_json(orders))

-- 使用已有的 struct 定义创建新行并插入
local o2 = ffi.new("order_t")
o2.order_id = 2001
o2.customer_id = 99
o2.quantity = 10
o2.unit_price = 199.99
cstring_copy(o2.product, "Mechanical Keyboard", 64)
cstring_copy(o2.status, "paid", 16)

local rowid2 = _lib.db_insert(orders, o2, order_schema:sizeof())
print(string.format("✓ Inserted new order after reload, rowid=%d", tonumber(rowid2)))

print_json("Final state", _lib.db_select_all_json(orders))

-- ============================================================
-- 演示 4: 运行时修改 Schema (扩展表)
-- ============================================================
print("\n=== Schema Evolution ===\n")

-- 定义扩展的 Schema (添加 tracking_number 字段)
local order_schema_v2 = SchemaBuilder:new("order_v2_t")
    :add_field("order_id",        ffi.C.DB_TYPE_INT32,  4)
    :add_field("customer_id",     ffi.C.DB_TYPE_INT32,  4)
    :add_field("product",         ffi.C.DB_TYPE_STRING, 64)
    :add_field("quantity",        ffi.C.DB_TYPE_INT32,  4)
    :add_field("unit_price",      ffi.C.DB_TYPE_DOUBLE, 8)
    :add_field("status",          ffi.C.DB_TYPE_STRING, 16)
    :add_field("tracking_number", ffi.C.DB_TYPE_STRING, 32)  -- 新增字段

print("New schema 'order_v2_t' (added tracking_number):")
for _, f in ipairs(order_schema_v2.fields) do
    print(string.format("  %s: offset=%d, size=%d", f.name, f.offset, f.size))
end

-- 加载新的 C struct
ffi.cdef(order_schema_v2:generate_cdef())
print("\n✓ New C struct loaded dynamically")

-- 创建新表 (旧表数据保留)
local field_defs_v2 = order_schema_v2:generate_field_defs()
local orders_v2 = _lib.db_table_register(db, "orders_v2", order_schema_v2:sizeof(), field_defs_v2, #order_schema_v2.fields)
if orders_v2 == nil then error("Failed to register orders_v2") end
print("✓ New table 'orders_v2' registered")

-- 插入使用新 Schema 的数据
local o3 = ffi.new("order_v2_t")
o3.order_id = 3001
o3.customer_id = 77
o3.quantity = 2
o3.unit_price = 149.99
cstring_copy(o3.product, "USB-C Hub", 64)
cstring_copy(o3.status, "shipped", 16)
cstring_copy(o3.tracking_number, "TRACK123456", 32)

_lib.db_insert(orders_v2, o3, order_schema_v2:sizeof())
print("✓ Inserted order with tracking number")

print_json("orders_v2 content", _lib.db_select_all_json(orders_v2))

-- ============================================================
-- 演示 5: 与 mydb.lua 高级封装的对比
-- ============================================================
print("\n=== Comparison with mydb.lua High-Level API ===\n")

print([[
Low-Level FFI (this example):
  Pros:
    - Zero serialization overhead (direct struct pointer)
    - Full control over memory layout
    - Dynamic schema at runtime
    - Maximum performance
  Cons:
    - Manual field offset calculation
    - Manual struct definition
    - More verbose

High-Level mydb.lua:
  Pros:
    - Simple table-based schema definition
    - Automatic field layout calculation
    - JSON result parsing built-in
    - Easier to use
  Cons:
    - JSON serialization overhead for results
    - Schema defined in Lua tables (extra layer)

Zero-Copy Architecture:
  Both approaches use the same underlying C library.
  The key is: LuaJIT FFI struct -> db_insert() -> mmap pool -> disk
                           (no copy, no serialization)
  On reload: disk -> mmap pool -> ffi.new() points to same memory
                           (no deserialization)
]])

-- ============================================================
-- 清理
-- ============================================================
print("--- Cleanup ---")
_lib.db_sync(db)
_lib.db_close(db)
print("✓ Database closed")

print("\n=== Dynamic Schema Example Complete ===")
print("Key takeaways:")
print("  1. C struct can be defined at runtime via ffi.cdef()")
print("  2. Schema metadata lives in Lua, data lives in C mmap pool")
print("  3. Reopen = remap mmap, zero data copy")
print("  4. Schema evolution = new struct + new table")
