# my_db

基于 C 语言数据结构的**单机嵌入式零拷贝存储引擎**

无网络、无端口、无协议，直接嵌入应用进程。完整的程序化 CRUD + JOIN 查询，零拷贝持久化。

## 核心特性

- **完整 CRUD**：INSERT / SELECT / UPDATE / DELETE，齐全的程序化 API
- **JOIN 关联**：等值关联查询，填补嵌入式零拷贝领域的空白
- **零拷贝持久化**：内存布局 = 磁盘布局，mmap 直接映射
- **编译时 Schema**：`DB_TABLE` / `DB_FIELD` 宏自动计算 offset/size，无需运行时注册
- **双文件架构**：数据（.bin）和索引（.index）分离，独立管理
- **WAL 日志**：同步写入保证不丢失，异步刷盘保证性能
- **JSON 查询结果**：SELECT 自动返回 JSON，无需 FFI 侧定义 struct
- **流式查询**：逐行回调，无内存上限，适合大数据量
- **内存回收**：`db_table_compact()` / `db_compact()` 回收已删除空间
- **无锁设计**：类似 Redis，单线程/用户自行保证并发，简单高效
- **极致性能**：单线程 50万+ 行/秒插入速度

## 快速开始

```c
#include "mydb.h"

// 1. 定义数据结构（编译时确定）
struct user {
    uint64_t id;
    char     name[32];
    int32_t  age;
    double   score;
};

// 2. 一行宏注册（编译时自动展开）
DB_TABLE(user, "users",
    DB_FIELD(user, id,    DB_TYPE_UINT64),
    DB_FIELD(user, name,  DB_TYPE_STRING),
    DB_FIELD(user, age,   DB_TYPE_INT32),
    DB_FIELD(user, score, DB_TYPE_DOUBLE)
)

// 3. 使用
int main() {
    db_t db = db_open("data.bin", "data.index", "wal.bin", 1024*1024*100);
    db_register_user(db);
    
    table_t users = db_table(db, "users");
    
    // 插入
    struct user u = {0, "Alice", 25, 95.5};
    rowid_t id = db_insert(users, &u, sizeof(u));
    
    // 查询返回 JSON
    const char* json = db_select_by_pk_json(users, id);
    printf("%s\n", json);  // {"id":1,"name":"Alice","age":25,"score":95.5}
    db_json_free(json);
    
    db_close(db);
    return 0;
}
```

## 构建

```bash
make
make test        # 运行基础测试
make perf        # 运行性能测试（100万行）
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

## 完整示例

见 `tests/test_join.c` 和 `tests/test_perf.c`：
- **test_basic.c**：CRUD + WHERE + ORDER BY + 流式查询
- **test_join.c**：多表 JOIN 关联查询
- **test_perf.c**：100万行性能测试

## API 概览

### 数据库生命周期
- `db_open()` / `db_close()` / `db_sync()`

### Schema 注册（编译时）
- `DB_TABLE(struct, name, DB_FIELD(struct, field, type), ...)`
- `db_table(db, "name")`

### CRUD
- `db_insert(table, row, size)` → rowid
- `db_update(table, id, row, size)`
- `db_delete(table, id)`

### 查询（JSON 模式）
- `db_select_by_pk_json(table, id)` → `{"id":1,...}`
- `db_select_all_json(table)` → `[{...},{...}]`
- `db_select_where_json(table, conditions, count)` → `[{...}]`
- `db_select_json(table, conds, count, order_offset, asc, limit_offset, limit_count)`
- `db_join_json(left, left_offset, right, right_offset, field_size)` → `[{"left.field":...,"right.field":...}]`

### 流式查询
- `db_select_all_stream(table, callback, userdata)`
- `db_select_where_stream(table, conds, count, callback, userdata)`

### 内存回收
- `db_table_compact(table)` / `db_compact(db)`

## 性能

| 操作 | 性能 |
|------|------|
| 插入 | ~50万行/秒（单线程） |
| 主键查询 | O(1)，纳秒级 |
| WHERE 查询 | 全表扫描 O(n)，内存中极快 |
| 流式查询 | 100万行/秒+ |

## 定位

- **不是** MySQL/PostgreSQL 那种完整 SQL 数据库（没有 SQL 解析器）
- **而是** 提供**程序化 CRUD API**（函数调用而非字符串 SQL），支持 WHERE/ORDER BY/LIMIT/**JOIN**
- 适合**数据量适中**（内存能装下）、需要**多表关联**、**极致性能**、**零拷贝持久化**的场景
- **明确排除**：TCP/网络层、多进程并发、分布式/集群（稳定后可能扩展网络层）

## 许可证

Apache License 2.0
