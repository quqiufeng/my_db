#!/usr/bin/env luajit
-- 使用 mydb.lua 封装层的动态 Schema 示例

local mydb = require("mydb")

print("=== mydb.lua 多文件存储示例 ===\n")

-- 打开数据库目录（每张表独立文件）
local db = mydb.open("game_db")
print("1. 数据库目录打开成功")
print("   目录结构: game_db/")
print("   ├── wal.bin        (WAL日志)")
print("   ├── users.bin      (users表数据)")
print("   ├── users.index    (users表索引)")
print("   ├── orders.bin     (orders表数据)")
print("   └── orders.index   (orders表索引)")

-- 动态注册表（无需 C struct，纯 Lua 表定义 Schema）
-- 第三个参数是索引列表：字符串=单列索引，表=复合索引
local users = db:register("users", {
    {name = "id",    type = "uint64"},   -- 第一字段必须是 uint64 id
    {name = "name",  type = "string", size = 32},
    {name = "age",   type = "int32"},
    {name = "score", type = "double"},
}, {
    "name",                    -- 单列索引
    {"name", "age"},           -- 复合索引
})
print("2. 动态注册 users 表成功（含 name 单列索引和 name+age 复合索引）")

local orders = db:register("orders", {
    {name = "id",      type = "uint64"},
    {name = "user_id", type = "uint64"},       -- 外键，关联 users.id
    {name = "amount",  type = "double"},
}, {
    "user_id",                 -- 单列索引
})
print("3. 动态注册 orders 表成功")

-- 插入数据（用 Lua 表，自动序列化）
local id1 = users:insert({name = "Alice", age = 25, score = 95.5})
local id2 = users:insert({name = "Bob",   age = 30, score = 88.0})
local id3 = users:insert({name = "Carol", age = 28, score = 92.0})
print("4. 插入数据: Alice(id=" .. id1 .. "), Bob(id=" .. id2 .. "), Carol(id=" .. id3 .. ")")

-- 插入订单
orders:insert({user_id = id1, amount = 100.0})
orders:insert({user_id = id1, amount = 250.0})
orders:insert({user_id = id2, amount = 50.0})
print("5. 插入订单: Alice 2 笔, Bob 1 笔")

-- 主键查询
local row = users:find(id1)
print("6. 主键查询 id=" .. id1 .. ": " .. (row and row.name or "nil"))

-- 全表查询（返回 Lua 表数组）
local all = users:select()
print("7. 全表查询，共 " .. #all .. " 行:")
for _, r in ipairs(all) do
    print("   " .. r.name .. ", age=" .. r.age .. ", score=" .. r.score)
end

-- WHERE 查询（会自动走索引优化）
local adults = users:where({{field = "age", op = 1, value = 25}})  -- age > 25
print("8. WHERE age>25:")
for _, r in ipairs(adults) do
    print("   " .. r.name)
end

-- 更新
users:update(id1, {name = "Alice Updated", age = 26, score = 96.0})
print("9. 更新 Alice 成功")

-- 删除
users:delete(id3)
print("10. 删除 Carol 成功")

-- 验证删除
print("11. 当前行数: " .. users:count())

-- 强制刷盘（数据真正落盘）
db:sync()
print("12. 数据已强制刷盘到各自文件")

-- Checkpoint
db:checkpoint()
print("13. Checkpoint 完成，WAL 已清空")

-- 关闭
db:close()
print("14. 数据库关闭")

print("\n=== 示例完成 ===")
print("\n文件结构:")
os.execute("ls -lh game_db/")
