# my_db

基于 C 语言数据结构的**单机嵌入式零拷贝存储引擎**

无网络、无端口、无协议，直接嵌入应用进程。完整的程序化 CRUD + JOIN 查询，零拷贝持久化。

## 项目定位

**不是数据库，是带关联查询能力的零拷贝嵌入式存储**

| | Redis | my_db |
|--|-------|-------|
| **本质** | 内存缓存/数据结构服务器 | **带 JOIN 查询的嵌入式存储引擎** |
| **数据模型** | string / hash / list / set / zset | C `struct` 数组（表） |
| **查询能力** | 键值查找 | **完整 CRUD + WHERE + ORDER BY + LIMIT + JOIN** |
| **访问方式** | 网络协议（TCP） | FFI 嵌入（无网络延迟） |
| **持久化** | RDB 快照 + AOF 日志（需序列化） | mmap 零拷贝（内存 = 磁盘） |
| **操作数据** | 发送命令 | **直接操作内存指针** |
| **部署** | 独立进程 | 嵌入应用进程 |

**核心优势**：
1. **单机嵌入**：无网络、无端口、无协议，直接嵌入应用进程，FFI 调用即内存操作
2. **直接操作内存**：获取结构体指针后，读写字段就是读写数据，没有序列化、没有网络、没有拷贝
3. **完整 CRUD**：INSERT/SELECT/UPDATE/DELETE 齐全，以函数 API 形式提供（非字符串 SQL）
4. **JOIN 关联**：支持表与表的等值关联查询，填补嵌入式零拷贝领域的空白
5. **零拷贝持久化**：mmap 让内存和磁盘是同一回事

**明确排除（第一版不做）**：
- ❌ TCP/网络服务层
- ❌ 多进程并发访问
- ❌ 分布式/集群
- ❌ SQL 字符串解析

## 适用场景

- **游戏服务器**：玩家表 JOIN 装备表、背包表
- **嵌入式设备**：传感器数据表 JOIN 配置表
- **高频交易**：订单表 JOIN 用户表、产品表
- **内存分析**：多表关联分析，无需导出到传统数据库

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

## 架构设计

### 整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                     FFI 客户端                               │
│  LuaJIT FFI / Python ctypes / 其他语言                       │
└──────────────────────┬──────────────────────────────────────┘
                       │
                       ▼
            ┌──────────────────────┐
            │    libmydb.so        │
            │  (纯 C 导出接口)      │
            └──────────┬───────────┘
                       │
    ┌──────────────────┼──────────────────┐
    ▼                  ▼                  ▼
┌─────────┐     ┌──────────┐      ┌──────────────┐
│ 操作处理 │ ──→ │ WAL 日志  │      │  mmap 数据池  │
│  引擎    │     │(同步写入)│      │ (异步刷盘)    │
└─────────┘     └──────────┘      └──────────────┘
```

### 双文件 mmap 设计

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

**数据文件（data.bin）**：只存原始行数据，结构简单
**索引文件（data.index）**：独立 mmap，主键哈希 + 可选 B+树/哈希
**WAL 日志（wal.bin）**：每次写操作先同步写入，崩溃后回放恢复

### 内存布局

数据文件内，每张表有独立的连续数据区：

```
┌──────────────────────────────────────────┐
│  表 "users" (row_size = sizeof(struct user))
│                                          │
│  ┌────────┬────────┬────────┬────────┐  │
│  │ user[0]│ user[1]│ user[2]│ user[3]│  │  ← 连续存储
│  │ header │ header │ header │ header │  │
│  │  data  │  data  │  data  │  data  │  │
│  └────────┴────────┴────────┴────────┘  │
│                                          │
│  数据库不解析 name/age/score，只认       │
│  "一张表有 N 行，每行 56 字节"           │
└──────────────────────────────────────────┘
```

### 写操作流程

1. 构造 WAL Entry，同步 `fsync` 写入 WAL 文件（保证不丢失）
2. 修改 mmap 内存池中的数据（内存操作，极快）
3. 返回成功给调用者
4. 后台线程定期 `msync(pool, MS_ASYNC)` 异步刷盘

### 恢复流程

1. 打开 mmap 数据文件（可能不是最新状态）
2. 扫描 WAL 日志，按 LSN 顺序重放
3. 重放完成后，mmap 状态 = 崩溃前状态
4. 启动后台刷盘线程
5. 清空/归档已确认落盘的 WAL

## 快速开始

### 1. 定义数据结构

```c
#include "mydb.h"

// ⚠️ 第一个字段必须是 uint64_t id（数据库自动填充）
struct user {
    uint64_t id;
    char     name[32];
    int32_t  age;
    double   score;
};
```

### 2. 编译时注册 Schema

```c
// 一行宏自动注册（编译时展开）
DB_TABLE(user, "users",
    DB_FIELD(user, id,    DB_TYPE_UINT64),
    DB_FIELD(user, name,  DB_TYPE_STRING),
    DB_FIELD(user, age,   DB_TYPE_INT32),
    DB_FIELD(user, score, DB_TYPE_DOUBLE)
)
```

**编译时完成**：
- `DB_FIELD(user, name, DB_TYPE_STRING)` → `{"name", 8, 32, DB_TYPE_STRING}`（offset 和 size 由编译器自动计算）
- 生成 `db_register_user()` 静态函数
- 零运行时开销

### 3. 使用数据库

```c
int main() {
    // 打开数据库（数据文件 + 索引文件 + WAL）
    db_t db = db_open("data.bin", "data.index", "wal.bin", 1024*1024*100);
    
    // 注册表（调用编译时生成的函数）
    db_register_user(db);
    
    // 获取表句柄
    table_t users = db_table(db, "users");
    
    // 插入数据（传入 C struct）
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
make              # 编译 libmydb.so
make test         # 运行基础测试
make test_join    # 运行 JOIN 测试
make test_perf    # 运行性能测试（100万行）
```

## 完整示例

### JOIN 关联查询

```c
struct user {
    uint64_t id;
    char     name[32];
};

struct order {
    uint64_t id;
    uint64_t user_id;   // 外键，关联 users.id
    double   amount;
};

DB_TABLE(user, "users",
    DB_FIELD(user, id,   DB_TYPE_UINT64),
    DB_FIELD(user, name, DB_TYPE_STRING)
)

DB_TABLE(order, "orders",
    DB_FIELD(order, id,      DB_TYPE_UINT64),
    DB_FIELD(order, user_id, DB_TYPE_UINT64),
    DB_FIELD(order, amount,  DB_TYPE_DOUBLE)
)

// JOIN 查询
const char* result = db_join_json(
    users, offsetof(struct user, id),      // 左表关联字段
    orders, offsetof(struct order, user_id), // 右表关联字段
    sizeof(uint64_t)                        // 字段大小
);
// 结果：[{"users.id":1,"users.name":"Alice","orders.id":1,"orders.user_id":1,"orders.amount":100.0}]
```

### WHERE + ORDER BY + LIMIT

```c
// WHERE age > 18
int32_t age = 18;
db_condition_t cond = {offsetof(struct user, age), sizeof(int32_t), &age, 1};
const char* result = db_select_where_json(users, &cond, 1);

// WHERE + ORDER BY + LIMIT
const char* result2 = db_select_json(
    users,
    &cond, 1,                    // WHERE 条件
    offsetof(struct user, age),  // ORDER BY 字段
    1,                           // 升序
    0, 10                        // LIMIT 0, 10
);
```

### 流式查询（大数据量）

```c
// 逐行回调，不生成完整 JSON，内存占用恒定
int callback(const char* json, void* user_data) {
    printf("%s\n", json);
    return 0;  // 返回 0 继续，非 0 停止
}

db_select_all_stream(users, callback, NULL);
```

## API 概览

### 数据库生命周期
- `db_open(data_path, index_path, wal_path, pool_size)` → db_t
- `db_close(db)`
- `db_sync(db)` — 强制刷盘
- `db_config_max_rows(db, max_rows)` — 配置最大返回行数（默认 10000）

### Schema 注册（编译时）
- `DB_TABLE(struct_name, table_name, DB_FIELD(struct, field, type), ...)` — 定义表
- `db_register_struct_name(db)` — 注册表（编译时生成的函数）
- `db_table(db, "table_name")` → table_t — 按名称获取表句柄

### CRUD
- `db_insert(table, row, size)` → rowid — 插入行，返回自增主键
- `db_update(table, id, row, size)` — 按主键更新
- `db_delete(table, id)` — 按主键删除（软删除）

### 查询（JSON 模式）
- `db_select_by_pk_json(table, id)` → `{"id":1,"name":"Alice",...}`
- `db_select_all_json(table)` → `[{...},{...}]`
- `db_select_where_json(table, conditions, count)` → `[{...}]`
- `db_select_json(table, conds, count, order_offset, asc, limit_offset, limit_count)` → `[{...}]`
- `db_join_json(left, left_offset, right, right_offset, field_size)` → `[{"left.field":...,"right.field":...}]`
- `db_json_free(json)` — 释放 JSON 字符串

### 流式查询
- `db_select_all_stream(table, callback, userdata)` — 全表流式
- `db_select_where_stream(table, conds, count, callback, userdata)` — 条件流式

### 内存回收
- `db_table_compact(table)` — 单表重建，回收已删除空间
- `db_compact(db)` — 全盘重建，回收所有已删除空间

## 动态脚本调用（LuaJIT / Python）

my_db 提供高级封装层，让 LuaJIT 和 Python 无需预定义 C struct，纯动态注册 Schema：

### LuaJIT 动态 Schema

```lua
local mydb = require("mydb")

-- 打开数据库
local db = mydb.open("app_data.bin")

-- 动态注册表（无需 C struct，纯 Lua 表定义）
local users = db:register("users", {
    {name = "id",    type = "uint64"},     -- 第一字段必须是 uint64 id
    {name = "name",  type = "string", size = 32},
    {name = "age",   type = "int32"},
    {name = "score", type = "double"},
})

-- 创建索引
users:create_index("name")

-- 插入（Lua 表自动序列化）
local id1 = users:insert({name = "Alice", age = 25, score = 95.5})
local id2 = users:insert({name = "Bob",   age = 30, score = 88.0})

-- 查询
local row = users:find(id1)              -- 主键查询
local all = users:select()               -- 全表查询
local adults = users:where({
    {field = "age", op = 1, value = 25}  -- age > 25
})

-- 更新/删除
users:update(id1, {name = "Alice Updated", age = 26, score = 96.0})
users:delete(id2)

-- Checkpoint 和关闭
db:checkpoint()
db:close()
```

### Python 动态 Schema

```python
from mydb import create

# 创建数据库并注册表（支持 with 语句自动关闭）
with create("app_data.bin", schemas={
    "users": [
        {"name": "id",    "type": "uint64"},
        {"name": "name",  "type": "string", "size": 32},
        {"name": "age",   "type": "int32"},
        {"name": "score", "type": "double"},
    ]
}) as db:
    users = db.table("users")
    
    # 创建索引
    users.create_index("name")
    
    # 插入（Python dict 自动序列化）
    id1 = users.insert({"name": "Alice", "age": 25, "score": 95.5})
    id2 = users.insert({"name": "Bob",   "age": 30, "score": 88.0})
    
    # 查询
    row = users.find(id1)              # 主键查询，返回 dict
    all_rows = users.select()          # 全表查询，返回 [dict]
    adults = users.where(age__gt=25)   # WHERE age > 25
    
    # 更新/删除
    users.update(id1, {"name": "Alice Updated", "age": 26, "score": 96.0})
    users.delete(id2)
    
    # Checkpoint
    db.checkpoint()
```

**核心优势**：
- **零 C 代码**：脚本层完全控制 Schema，无需编译 C struct
- **自动序列化**：Lua 表/Python dict ↔ C 内存自动转换
- **自动偏移计算**：封装层自动处理字段对齐和偏移
- **JSON 反序列化**：查询结果自动解析为原生数据类型

## 性能

| 操作 | 性能 |
|------|------|
| 插入 | ~50万行/秒（单线程） |
| 主键查询 | O(1)，纳秒级 |
| WHERE 查询 | 全表扫描 O(n)，内存中极快 |
| 流式查询 | 100万行/秒+ |

## 设计决策

| 决策项 | 选择 | 理由 |
|--------|------|------|
| **定位** | 零拷贝嵌入式存储引擎 | 内存数据库，直接操作 struct，无网络、无序列化 |
| Schema 定义 | **编译时宏自动生成** | `DB_TABLE` / `DB_FIELD` 宏自动计算 offset/size，无需运行时注册 |
| 持久化机制 | mmap + 异步 msync | 零拷贝，内存布局 = 磁盘布局 |
| 数据安全 | WAL 同步写入 | 每次写先 fsync WAL，再改内存 |
| 主键 | 自增 uint64_t | 简单高效，FFI 兼容 |
| 读取方式 | SELECT 返回 JSON 字符串 | 无需 FFI 侧定义 struct，直接解析 JSON |
| 写入方式 | 传入 C struct 直接写入 | `db_insert()` / `db_update()` 零拷贝写入 |
| 删除 | 软删除（标记位） | 避免内存碎片，定期 compact |
| 索引 | 主键哈希 + 单列/复合 B+树 + 哈希 | 复合索引第一版即支持，简单高效 |
| 内存分配 | Bump Allocator | 顺序分配，先不实现复杂回收 |
| 并发 | **无锁，用户自行保证** | 类似 Redis，简单快速第一，用户自己管理并发 |
| 查询优化 | **简单规则匹配**（第一版） | 复合索引优先 → 单列索引 → 全表扫描，无复杂成本估算 |

## 并发设计

**本项目不提供任何锁机制**，并发安全由用户自行保证：

- **单线程使用**：最简单，无并发问题（推荐）
- **多线程读**：如果数据不修改，多个线程可同时读
- **多线程写**：用户需在外层自行加锁

```c
// 用户自行加锁示例
pthread_mutex_t db_mutex;

void thread_safe_insert(table_t t, void* row) {
    pthread_mutex_lock(&db_mutex);
    db_insert(t, row, sizeof(row));
    pthread_mutex_unlock(&db_mutex);
}
```

## 内存管理

### 内存膨胀问题

Bump Allocator + 软删除 = 内存只增不减

### 解决方案：Compact

```c
// 单表重建，回收已删除空间
size_t deleted = db_table_compact(table);

// 全盘重建，回收所有已删除空间
size_t total = db_compact(db);
```

**使用建议**：
- 删除比例 > 20% 时触发
- 业务低峰期执行（用户自行加锁保证独占访问）

### 大结果集保护

| 模式 | API | 适用场景 | 内存占用 |
|------|-----|---------|---------|
| **完整 JSON** | `db_select_*_json()` | 结果 < 1000 行 | 与结果集成正比 |
| **流式查询** | `db_select_*_stream()` | 结果任意大小 | 恒定（单行 JSON） |

**保护措施**：
- 默认 `max_rows = 10000`，超过返回错误
- 可配置：`db_config_max_rows(db, 50000)`

## 市场空白

**在嵌入式零拷贝领域，支持 JOIN 的几乎没有：**

| 项目 | 零拷贝 | C struct | CRUD | WHERE | ORDER BY | LIMIT | **JOIN** | 嵌入 FFI |
|------|--------|----------|------|-------|----------|-------|----------|----------|
| **SQLite** | ❌ 磁盘页式 | ❌ SQL 类型 | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ 非零拷贝 |
| **LMDB** | ✅ mmap | ❌ 纯 KV | ✅ KV | ❌ | ❌ | ❌ | ❌ | ❌ 无表概念 |
| **Redis** | ❌ 网络序列化 | ❌ 数据结构 | ✅ 命令 | ❌ | ❌ | ❌ | ❌ | ❌ 需网络 |
| **Tarantool** | ❌ 内存拷贝 | ❌ Lua 表 | ✅ | ✅ | ❌ | ❌ | ✅ | ❌ 需网络 |
| **my_db** | ✅ **mmap** | ✅ **C struct** | ✅ **API** | ✅ | ✅ | ✅ | **✅** | **✅** |

## 目录结构

```
my_db/
├── design.md           # 设计方案
├── README.md           # 本文件
├── task.md             # 开发任务列表
├── LICENSE
├── Makefile
├── include/
│   ├── mydb.h          # 公共头文件（FFI 依赖）
│   └── mydb_internal.h # 内部头文件
├── src/
│   ├── core/
│   │   ├── db.c        # 数据库生命周期
│   │   ├── table.c     # 表操作
│   │   └── query.c     # 查询引擎
│   ├── storage/
│   │   ├── wal.c       # WAL 日志
│   │   └── mmap.c      # mmap 封装
│   └── utils/
│       ├── crc32.c     # 校验和
│       ├── error.c     # 错误处理
│       └── json.c      # JSON 序列化
└── tests/
    ├── test_basic.c    # 基础功能测试
    ├── test_join.c     # JOIN 测试
    └── test_perf.c     # 性能测试
```

## 许可证

Apache License 2.0
