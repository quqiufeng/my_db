# mydb LuaJIT FFI 示例

> 📖 **详细文档参见 [database.md > 6.3 LuaJIT FFI 示例](../database.md#63-luajit-ffi-示例)**

## 快速运行

```bash
# CRUD + 事务 + 压缩
luajit examples/luajit_crud_example.lua

# JOIN 关联查询 + 索引
luajit examples/luajit_join_example.lua

# 运行时动态 Schema + 零拷贝 reload
luajit examples/luajit_dynamic_schema.lua
```

## 前置要求

- LuaJIT 2.1+
- `libmydb.so` 位于项目根目录
- Linux 系统 (使用 `RTLD_DEEPBIND` 隔离符号冲突)
