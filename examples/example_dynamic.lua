#!/usr/bin/env luajit
-- 使用 mydb.lua 封装层的动态 Schema 示例

local mydb = require("mydb")

print("=== mydb.lua 动态封装示例 ===\n")

-- 打开数据库
local db = mydb.open("dyn_lua.bin")
print("1. 数据库打开成功")

-- 动态注册表（无需 C struct，纯 Lua 表定义 Schema）
local users = db:register("users", {
    {name = "id",    type = "uint64"},   -- 第一字段必须是 uint64 id
    {name = "name",  type = "string", size = 32},
    {name = "age",   type = "int32"},
    {name = "score", type = "double"},
})
print("2. 动态注册 users 表成功")

-- 创建索引
users:create_index("name")
print("3. 创建 name 索引成功")

-- 插入数据（用 Lua 表，自动序列化）
local id1 = users:insert({name = "Alice", age = 25, score = 95.5})
local id2 = users:insert({name = "Bob",   age = 30, score = 88.0})
local id3 = users:insert({name = "Carol", age = 28, score = 92.0})
print("4. 插入数据: Alice(id=" .. id1 .. "), Bob(id=" .. id2 .. "), Carol(id=" .. id3 .. ")")

-- 主键查询
local row = users:find(id1)
print("5. 主键查询 id=" .. id1 .. ": " .. (row and row.name or "nil"))

-- 全表查询（返回 Lua 表数组）
local all = users:select()
print("6. 全表查询，共 " .. #all .. " 行:")
for _, r in ipairs(all) do
    print("   " .. r.name .. ", age=" .. r.age .. ", score=" .. r.score)
end

-- WHERE 查询
local adults = users:where({{field = "age", op = 1, value = 25}})  -- age > 25
print("7. WHERE age>25:")
for _, r in ipairs(adults) do
    print("   " .. r.name)
end

-- 更新
users:update(id1, {name = "Alice Updated", age = 26, score = 96.0})
print("8. 更新 Alice 成功")

-- 删除
users:delete(id3)
print("9. 删除 Carol 成功")

-- 验证删除
print("10. 当前行数: " .. users:count())

-- Checkpoint
db:checkpoint()
print("11. Checkpoint 完成")

-- 关闭
db:close()
print("12. 数据库关闭")

print("\n=== 示例完成 ===")
