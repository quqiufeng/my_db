# my_db

基于 C 语言数据结构的**单机嵌入式零拷贝存储引擎**

无网络、无端口、无协议，直接嵌入应用进程。完整的程序化 CRUD + JOIN 查询，零拷贝持久化。
- 数据文件（.bin）和索引文件（.index）分离，各自独立 mmap 零拷贝
- WAL 日志保证数据不丢失，异步刷盘零性能损耗
- 支持 LuaJIT FFI / Python ctypes 嵌入调用

## 定位：带关联查询的零拷贝嵌入式存储

- **不是** MySQL/PostgreSQL 那种完整 SQL 数据库（没有 SQL 解析器）
- **而是** 提供**程序化 CRUD API**（函数调用而非字符串 SQL），支持 WHERE/ORDER BY/LIMIT/**JOIN**
- 适合**数据量适中**（内存能装下）、需要**多表关联**、**极致性能**、**零拷贝持久化**的场景

## 核心特性

- **完整 CRUD**：INSERT / SELECT / UPDATE / DELETE，齐全的程序化 API
- **JOIN 关联**：等值关联查询，填补嵌入式零拷贝领域的空白
- **零拷贝持久化**：内存布局 = 磁盘布局，mmap 直接映射
- **双文件架构**：数据（.bin）和索引（.index）分离，独立管理
- **WAL 日志**：同步写入保证不丢失，异步刷盘保证性能
- **自动索引**：每张表自带自增主键索引，二级索引可选
- **内存回收**：`db_table_compact()` / `db_compact()` 回收已删除空间
- **大结果集保护**：流式查询 `db_select_stream()` 逐行回调，无内存上限
- **无锁设计**：类似 Redis，单线程/用户自行保证并发，简单高效

## 快速开始

```c
#include "mydb.h"

// ========== 1. 定义数据结构（编译时确定） ==========
struct user {
    uint64_t id;
    char     name[32];
    int32_t  age;
    double   score;
};

// ========== 2. 宏自动注册 Schema（编译时展开） ==========
DB_TABLE(user, "users",
    DB_FIELD(id,    DB_TYPE_UINT64),
    DB_FIELD(name,  DB_TYPE_STRING),
    DB_FIELD(age,   DB_TYPE_INT32),
    DB_FIELD(score, DB_TYPE_DOUBLE)
)

// ========== 3. 使用 ==========
int main() {
    db_t db = db_open("data.bin", "data.index", "wal.bin", 1024*1024*100);
    db_register_user(db);   // 自动生成的注册函数
    
    table_t users = db_table(db, "users");  // 按名称获取表
    
    // 插入数据
    struct user u = {0, "Alice", 25, 95.5};
    rowid_t id = db_insert(users, &u, sizeof(u));
    
    // 查询返回 JSON
    const char* json = db_select_by_pk_json(users, id);
    printf("%s\n", json);  // {"id":1,"name":"Alice","age":25,"score":95.5}
    db_json_free(json);
    
    // JOIN 关联查询
    const char* join_json = db_join_json(users, offsetof(struct user, id), ...);
    
    db_close(db);
    return 0;
}
```

## 构建

```bash
make
```

## 架构

```
data.bin          data.index           wal.bin
┌──────────┐     ┌──────────┐        ┌──────────┐
│ 表数据    │     │ 主键索引  │        │ WAL日志  │
│ 行数组    │     │ 二级索引  │        │ 顺序写入 │
└──────────┘     └──────────┘        └──────────┘
   ↑ mmap            ↑ mmap                ↑ fsync
   └─────────────────┘                     │
                    内存                     │
```

- **数据文件**：只存原始行数据，结构简单
- **索引文件**：独立 mmap，主键哈希 + 可选 B+树/哈希
- **WAL 日志**：每次写操作先同步写入，崩溃后回放恢复

## 更多示例

见 `examples/` 目录：
- `example.c` - C 原生 API
- `example.lua` - LuaJIT FFI
- `example.py` - Python ctypes

## 许可证

Apache License 2.0
