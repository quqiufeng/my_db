# mydb LuaJIT FFI 示例

> 📖 **详细文档参见 [database.md > 6.3 LuaJIT FFI 示例](../database.md#63-luajit-ffi-示例)**

## 快速运行

```bash
# CRUD + 事务 + 批量插入 + 压缩
luajit examples/luajit_crud_example.lua

# JOIN 关联查询 + 索引 + 多表关联
luajit examples/luajit_join_example.lua

# 运行时动态 Schema + 零拷贝 reload
luajit examples/luajit_dynamic_schema.lua
```

## 前置要求

- LuaJIT 2.1+
- `libmydb.so` 位于项目根目录
- Linux 系统 (使用 `RTLD_DEEPBIND` 隔离符号冲突)

## 已知注意事项

### 字段大小需手动指定
LuaJIT FFI **不支持** `ffi.sizeof("struct_t", "field_name")` 语法，注册 Schema 时必须手动指定字段大小：

```lua
-- ❌ 错误：返回整个结构体大小，不是字段大小
fields[0].size = ffi.sizeof("user_t", "id")  -- 返回 48 (整个 struct)

-- ✅ 正确：手动指定字段大小
fields[0].size = 4   -- INT32
fields[1].size = 32  -- STRING[32]
fields[2].size = 8   -- DOUBLE
```

### cstring_copy 辅助函数
对 FFI char 数组不能直接用 `#dest` 求长度，应使用 `ffi.sizeof(dest)`：

```lua
local function cstring_copy(dest, src, max_len)
    max_len = max_len or ffi.sizeof(dest)  -- 不是 #dest
    local len = math.min(#src, max_len - 1)
    ffi.copy(dest, src, len)
    dest[len] = 0
end
```
