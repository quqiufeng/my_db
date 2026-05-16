# my_db 设计方案

## 1. 项目概述

**程序化 CRUD + C struct + mmap 零拷贝 + JOIN**

一个基于 C 语言数据结构的**嵌入式内存存储引擎**：
- 数据**全部驻留内存**，读写就是直接读写 C 结构体字段（零拷贝）
- 提供**完整的程序化 CRUD API**：INSERT、SELECT（WHERE + ORDER BY + LIMIT）、UPDATE、DELETE
- 支持**JOIN 关联查询**（等值关联），这在嵌入式零拷贝领域极为稀缺
- 通过 `mmap` 映射到磁盘文件实现**零拷贝持久化**（内存布局 = 磁盘布局）
- **异步刷盘** + **WAL 日志**保证数据不丢失
- 对外暴露 C API，支持 LuaJIT FFI / Python ctypes 嵌入调用
- **表 = C struct 数组**，新建表就是定义一个 `struct`，系统不解析内部字段，只存原始字节

### 1.1 定位：带关联查询能力的零拷贝嵌入式存储

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
3. **完整 CRUD**：不是玩具，INSERT/SELECT/UPDATE/DELETE 齐全，只是以函数 API 形式提供（非字符串 SQL）
4. **JOIN 关联**：支持表与表的等值关联查询，填补嵌入式零拷贝领域的空白
5. **零拷贝持久化**：mmap 让内存和磁盘是同一回事

**明确排除（第一版不做）：**
- ❌ TCP/网络服务层
- ❌ 多进程并发访问
- ❌ 分布式/集群
- ❌ SQL 字符串解析

**未来可能扩展（稳定后）：**
- 可选的 TCP 服务层（兼容 Redis 协议或自定义协议）
- 但核心永远是单机嵌入，网络层只是包装

### 1.2 市场空白分析

**在嵌入式零拷贝领域，支持 JOIN 的几乎没有：**

| 项目 | 零拷贝 | C struct | CRUD | WHERE | ORDER BY | LIMIT | **JOIN** | 嵌入 FFI |
|------|--------|----------|------|-------|----------|-------|----------|----------|
| **SQLite** | ❌ 磁盘页式 | ❌ SQL 类型 | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ 非零拷贝 |
| **LMDB** | ✅ mmap | ❌ 纯 KV | ✅ KV | ❌ | ❌ | ❌ | ❌ | ❌ 无表概念 |
| **Redis** | ❌ 网络序列化 | ❌ 数据结构 | ✅ 命令 | ❌ | ❌ | ❌ | ❌ | ❌ 需网络 |
| **Tarantool** | ❌ 内存拷贝 | ❌ Lua 表 | ✅ | ✅ | ❌ | ❌ | ✅ | ❌ 需网络 |
| **my_db** | ✅ **mmap** | ✅ **C struct** | ✅ **API** | ✅ | ✅ | ✅ | **✅** | **✅** |

**唯一满足全部条件的项目。**

适用场景：
- **游戏服务器**：玩家表 JOIN 装备表、背包表
- **嵌入式设备**：传感器数据表 JOIN 配置表
- **高频交易**：订单表 JOIN 用户表、产品表
- **内存分析**：多表关联分析，无需导出到传统数据库

## 2. 核心架构

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

### 2.1 写操作流程

1. 构造 WAL Entry，同步 `fsync` 写入 WAL 文件（保证不丢失）
2. 修改 mmap 内存池中的数据（内存操作，极快）
3. 返回成功给调用者
4. 后台线程定期 `msync(pool, MS_ASYNC)` 异步刷盘

### 2.2 恢复流程

1. 打开 mmap 数据文件（可能不是最新状态）
2. 扫描 WAL 日志，按 LSN 顺序重放
   - `INSERT`：在 mmap 中分配空间，写入数据
   - `UPDATE`：定位到 offset，更新数据
   - `DELETE`：标记删除
3. 重放完成后，mmap 状态 = 崩溃前状态
4. 启动后台刷盘线程
5. 清空/归档已确认落盘的 WAL

## 3. 内存布局设计（双文件 mmap）

**数据文件（.bin）和索引文件（.index）分开保存，各自独立 mmap。**

### 3.1 数据文件（data.bin）

只存储原始行数据，结构最简单。

```
┌────────────────────────────────────────────────────────┐
│                 data.bin 文件布局                       │
├────────────────────────────────────────────────────────┤
│  魔数 "MYDB" (4 bytes)                                  │
│  版本号 (4 bytes)                                       │
│  页大小/对齐要求 (4 bytes)                              │
│  池总大小 (8 bytes)                                     │
│  当前已用偏移 (8 bytes)                                 │
│  表数量 (4 bytes)                                       │
├────────────────────────────────────────────────────────┤
│  表目录数组 (struct table_entry[])                      │
│    - 表名 (64 bytes)                                    │
│    - 行大小 (4 bytes)                                   │
│    - 行数量 (8 bytes)                                   │
│    - 行数据区 offset (8 bytes)                          │
├────────────────────────────────────────────────────────┤
│                    数据区域                             │
│  ┌──────────────┐                                      │
│  │  表 1 数据   │  struct user[]                       │
│  │              │  连续存储，紧凑排列                   │
│  ├──────────────┤                                      │
│  │  表 2 数据   │  struct order[]                      │
│  └──────────────┘                                      │
├────────────────────────────────────────────────────────┤
│  空闲区（预留扩展）                                     │
└────────────────────────────────────────────────────────┘
```

### 3.2 索引文件（data.index）

存储所有索引结构，与数据文件一一对应。

```
┌────────────────────────────────────────────────────────┐
│                data.index 文件布局                      │
├────────────────────────────────────────────────────────┤
│  魔数 "MYIX" (4 bytes)                                  │
│  版本号 (4 bytes)                                       │
│  对应数据文件 checksum (8 bytes)                        │
│  索引数量 (4 bytes)                                     │
├────────────────────────────────────────────────────────┤
│  索引目录数组 (struct index_entry[])                    │
│    - 表名 (64 bytes)                                    │
│    - 字段偏移 (4 bytes)                                 │
│    - 索引类型 (4 bytes)  1=哈希, 2=B+树                │
│    - 索引数据区 offset (8 bytes)                        │
├────────────────────────────────────────────────────────┤
│                    索引数据区域                         │
│  ┌──────────────┐                                      │
│  │ 主键索引     │  哈希表：rowid → 数据文件 offset     │
│  │ (users.pk)   │  offset_t key, offset_t value        │
│  ├──────────────┤                                      │
│  │ age 索引     │  B+树：age → rowid 数组              │
│  │ (users.age)  │                                      │
│  ├──────────────┤                                      │
│  │ name 索引    │  哈希表：name → rowid                │
│  │ (users.name) │                                      │
│  ├──────────────┤                                      │
│  │ 主键索引     │  哈希表：rowid → 数据文件 offset     │
│  │ (orders.pk)  │                                      │
│  └──────────────┘                                      │
└────────────────────────────────────────────────────────┘
```

### 3.1 偏移寻址

- **内部全部使用相对偏移（`offset_t`）而非绝对指针**
- 访问时：`PTR(base, offset) = (char*)base + offset`
- 刷盘：直接将内存池 `write()` / 或依赖 mmap 自动回写
- 加载：直接将文件映射到内存，仅需一次基地址重定位

### 3.2 内存分配策略

采用 **Bump Allocator（顺序分配）**：
- 分配：从当前已用偏移处直接切出一块
- 不回收（先不实现复杂内存管理）
- 删除采用软删除（标记位），定期 compact

## 4. WAL 日志设计

### 4.1 WAL Entry 格式

```
┌─────────────────────────────────────────────────────┐
│ WAL Entry                                           │
├─────────────────────────────────────────────────────┤
│ 条目长度 (4 bytes)                                   │
│ CRC32 校验 (4 bytes)                                │
│ LSN 日志序列号 (8 bytes)                            │
│ 操作类型 (4 bytes)                                  │
│    1 = INSERT, 2 = UPDATE, 3 = DELETE               │
│ 表名长度 (4 bytes)                                  │
│ 表名 (变长，无\0)                                    │
│ 行数据 offset (8 bytes, UPDATE/DELETE 用)           │
│ 行数据大小 (4 bytes)                                │
│ 行数据 (变长)                                       │
└─────────────────────────────────────────────────────┘
```

### 4.2 刷盘策略

- **每条事务后立即 `fsync`**：最安全，性能略低
- **批量 `fsync`**（可配置）：每 N 条或每 T 毫秒 fsync 一次

### 4.3 清理策略

- 后台刷盘完成后，对应 WAL 可安全截断
- Checkpoint 机制：记录已刷盘的 LSN，之前的 WAL 可删除

## 5. 表结构设计

### 5.1 核心思想：表 = C 结构体数组（编译时确定）

**C struct 就是数据库里的表，编译时完全确定：**

| C 语言概念 | 数据库概念 |
|-----------|-----------|
| `struct user` | 表 `users` |
| struct 的字段（`name`, `age`） | 表的列（Column） |
| struct 的实例 | 表的行（Row） |
| `uint64_t id`（第一个字段） | 自增主键 |

**约定：每个 struct 第一个字段必须是 `uint64_t id`，作为主键。** 数据库自动维护自增。

```c
// 用户在自己的代码中定义表结构
// ⚠️ 第一个字段必须是 uint64_t id（数据库自动填充）
struct user {
    uint64_t id;            // 主键（自增，数据库自动填充，用户无需赋值）
    char     name[32];      // 定长字符串
    int32_t  age;
    double   score;
} __attribute__((packed));

struct order {
    uint64_t id;            // ⚠️ 必须是第一个字段，自增主键
    uint64_t user_id;       // 外键，用于 JOIN
    double   amount;
    int64_t  created_at;
} __attribute__((packed));
```

### 5.2 Schema 自动生成（编译时宏）

**不需要手动计算 offset 和 size，不需要运行时 `db_table_create()`。**

使用 C 宏自动提取 struct 的字段信息：

```c
#include "mydb.h"

// 字段类型枚举（编译时常量）
enum db_field_type {
    DB_TYPE_INT32,
    DB_TYPE_INT64,
    DB_TYPE_UINT64,
    DB_TYPE_FLOAT,
    DB_TYPE_DOUBLE,
    DB_TYPE_STRING,   // 定长字符串（char[N]）
    DB_TYPE_BOOL,
};

// ========== 宏魔法：自动注册 Schema ==========

// 定义字段（自动计算 offset 和 size）
#define DB_FIELD(name, type) \
    {#name, offsetof(__db_current_struct, name), sizeof(((struct __db_current_struct*)0)->name), type}

// 定义表
#define DB_TABLE(struct_name, table_name_str, ...) \
    static void db_register_##struct_name(db_t db) { \
        typedef struct struct_name __db_current_struct; \
        db_field_def_t fields[] = {__VA_ARGS__}; \
        db_table_register(db, table_name_str, sizeof(struct struct_name), fields, sizeof(fields)/sizeof(fields[0])); \
    }

// ========== 用户代码：定义表 ==========

DB_TABLE(user, "users",
    DB_FIELD(id,    DB_TYPE_UINT64),
    DB_FIELD(name,  DB_TYPE_STRING),
    DB_FIELD(age,   DB_TYPE_INT32),
    DB_FIELD(score, DB_TYPE_DOUBLE)
)

DB_TABLE(order, "orders",
    DB_FIELD(id,         DB_TYPE_UINT64),
    DB_FIELD(user_id,    DB_TYPE_UINT64),
    DB_FIELD(amount,     DB_TYPE_DOUBLE),
    DB_FIELD(created_at, DB_TYPE_INT64)
)

// ========== 初始化时自动注册所有表 ==========

void db_register_all_schemas(db_t db) {
    db_register_user(db);
    db_register_order(db);
}
```

**编译时完成的事情：**
- `DB_FIELD(name, DB_TYPE_STRING)` 自动展开为 `{"name", 8, 32, DB_TYPE_STRING}`（offset 和 size 由编译器计算）
- 每个 `DB_TABLE` 生成一个静态注册函数
- 所有 struct 信息在编译时完全确定，运行时零开销

**运行时获取表句柄：**
```c
db_t db = db_open("data.bin", "data.index", "wal.bin", 1024*1024*100);
db_register_all_schemas(db);  // 编译时生成的注册函数

table_t users  = db_table(db, "users");   // 按名称获取已注册表
table_t orders = db_table(db, "orders");
```

### 5.2 数据库视角：透明行存储

```
┌──────────────────────────────────────────┐
│  表 "users" (row_size = sizeof(struct user))
│                                          │
│  ┌────────┬────────┬────────┬────────┐  │
│  │ user[0]│ user[1]│ user[2]│ user[3]│  │  ← 连续存储
│  │ 56字节 │ 56字节 │ 56字节 │ 56字节 │  │
│  └────────┴────────┴────────┴────────┘  │
│                                          │
│  数据库不解析 name/age/score，只认       │
│  "一张表有 N 行，每行 56 字节"           │
└──────────────────────────────────────────┘
```

### 5.3 行数据布局（内部）

```c
// 数据库在结构体前附加内部元数据（对用户透明）
struct row_header {
    uint64_t rowid;         // 自增主键
    uint8_t  flags;         // 软删除标记等内部状态
    // 之后紧跟用户结构体数据（原始字节拷贝）
};
```

### 5.4 变长字段处理

如果需要变长数据（字符串、数组），有两种方式：

**方式 A：定长内联（推荐，简单）**
```c
struct user {
    char name[64];   // 最大 64 字节，内部存储
};
```

**方式 B：offset 外置（高级，零拷贝）**
```c
struct user {
    uint64_t id;
    offset_t bio;    // 指向池中另一区域的 offset
    // 数据库提供辅助函数管理外置内存
};
```

### 5.5 索引设计

**主键索引（自动创建）：**
- 每张表自动创建主键索引（哈希表：rowid → offset）
- `db_select_by_pk()` 直接使用，O(1) 查询

**二级索引（可选创建）：**
- 用户显式创建，指定字段偏移和类型
- 数据结构根据类型自动选择：
  - 数值类型（int, double）→ B+树（支持范围查询和排序）
  - 字符串类型 → 哈希表（等值查询）

```c
// 单列索引
// 在 "age" 字段上建索引（偏移 40，类型 int32）
// 自动选择 B+树（因为是数值）
db_table_add_index(users, "age", 40, DB_TYPE_INT32);

// 在 "name" 字段上建索引（偏移 8，类型 char[32]）
// 自动选择哈希表（因为是字符串）
db_table_add_index(users, "name", 8, DB_TYPE_STRING, 32);
```

**复合索引（多列联合索引）：**
- 第一版即支持，实现简单
- 多个字段组合成一个索引键，按顺序比较
- 适用场景：`WHERE age = ? AND score = ?`（两列同时等值查询）

```c
// 复合索引：(age, score)
// 适用于 WHERE age = 25 AND score > 90
db_field_def_t composite_fields[] = {
    {NULL, offsetof(struct user, age),   sizeof(int32_t),  DB_TYPE_INT32},
    {NULL, offsetof(struct user, score), sizeof(double),   DB_TYPE_DOUBLE},
};
db_table_add_index_composite(users, composite_fields, 2);
```

**查询优化器（简单规则，第一版即实现）：**
- 不是基于成本的复杂优化器，而是简单规则匹配
- 规则：
  1. `WHERE id = ?` → 主键索引，O(1)
  2. `WHERE age = ? AND score = ?`（有复合索引）→ 复合索引，O(1)
  3. `WHERE age = ?`（有单列索引）→ 二级索引，O(1) 或 O(log n)
  4. `WHERE age > ? ORDER BY age`（有 B+树索引）→ B+树范围扫描
  5. `WHERE name = ? AND age = ?`（name 有索引）→ 先查 name 索引，再过滤 age
  6. 无匹配索引 → 全表扫描（兜底）

**索引选择逻辑（伪代码）：**
```c
cursor_t db_select_where_optimized(table_t table, conditions, count) {
    // 1. 检查是否有复合索引完全匹配所有等值条件
    if (composite_index_exists_matching_all_eq_conditions) {
        return query_composite_index();
    }
    
    // 2. 检查是否有单列索引匹配第一个条件
    if (single_column_index_exists_for_first_condition) {
        return query_single_index_then_filter_rest();
    }
    
    // 3. 检查是否可以用索引加速 ORDER BY
    if (order_by_field_has_btree_index) {
        return index_scan_with_filter();
    }
    
    // 4. 兜底：全表扫描
    return full_table_scan();
}
```

### 5.6 删除策略与内存回收

**软删除机制：**
- 删除时设置 `flags & DELETED`，数据仍在原地，不立即回收
- 避免内存碎片，保证行 offset 不变（外部索引不受影响）

**内存膨胀问题与解决方案：**

Bump Allocator + 软删除 = 内存只增不减。解决方案：

**方案 1：表级 Compact（推荐）**
```c
// 重建单张表，回收已删除行的空间
// 过程：
//   1. 扫描表，复制未删除行到新内存区（紧凑排列）
//   2. 更新主键索引（rowid → 新 offset）
//   3. 更新二级索引（rowid 不变，offset 更新）
//   4. 原子替换旧数据区
//   5. 旧空间标记为空闲（ Bump Allocator 可回收）
int db_table_compact(table_t table);
```

**方案 2：数据库级 Compact（全盘重建）**
```c
// 重建整个数据库，回收所有已删除空间
// 适合多张表都有大量删除的场景
int db_compact(db_t db);
```

**使用建议：**
- 删除比例超过 20% 时触发 compact
- 业务低峰期手动调用（如凌晨）
- Compact 期间需要独占访问（用户自行加锁）

**分区 Bump Allocator（未来优化）：**
- 每张表独立内存区，删除表即回收整区
- 当前版本先实现表级 compact

## 6. FFI API 设计

### 6.1 设计原则

| 原则 | 说明 |
|------|------|
| 纯 C | 无 C++、无宏、无可变参数 |
| 不透明句柄 | `typedef struct X* handle_t` |
| 原始字节 | 行数据用 `void* + size` |
| 错误码 | 整数返回，配合 `db_errstr()` 取描述 |
| 显式生命周期 | 谁创建谁释放 |
| 固定 ABI | 结构体明确 pack 或自然对齐 |

### 6.2 C 头文件接口

```c
#ifndef MYDB_H
#define MYDB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ====== 不透明句柄 ======
typedef struct db_instance* db_t;
typedef struct db_table*    table_t;
typedef struct db_cursor*   cursor_t;
typedef uint64_t            rowid_t;

// ====== 错误码 ======
enum {
    DB_OK                    =  0,
    DB_ERR_NOMEM             = -1,
    DB_ERR_IO                = -2,
    DB_ERR_NOENT             = -3,
    DB_ERR_EXIST             = -4,
    DB_ERR_INVAL             = -5,
    DB_ERR_RESULT_TOO_LARGE  = -6,  // 查询结果超过最大限制
    DB_ERR_CORRUPTED         = -7,  // 数据文件损坏
    DB_ERR_WAL_REPLAY        = -8,  // WAL 回放失败
};

// ====== 数据库 ======
// data_path: 数据文件路径 (.bin)
// index_path: 索引文件路径 (.index)
// wal_path: WAL 日志路径
// pool_size: 初始内存池大小
db_t db_open(const char* data_path, const char* index_path, const char* wal_path, size_t pool_size);
void db_close(db_t db);
int  db_sync(db_t db);                    // 强制刷盘（等待后台完成）
const char* db_errstr(db_t db);           // 最后错误信息

// ====== Schema 注册（编译时宏自动生成调用）=====
// 内部函数，由 DB_TABLE 宏自动调用，用户不直接调用
table_t db_table_register(db_t db, const char* name, size_t row_size,
                          const db_field_def_t* fields, size_t field_count);

// 获取已注册表的句柄（运行时按名称查找）
table_t db_table(db_t db, const char* name);

// 删除表（清空数据，保留 schema 定义）
int     db_table_drop  (db_t db, const char* name);
size_t  db_table_count (table_t table);

// ====== CRUD 通用接口 ======

// ---- Create ----
// 插入一行（传入 C struct），返回自增 rowid，失败返回 0
// 零拷贝写入：直接拷贝到 mmap 内存
rowid_t db_insert(table_t table, const void* row, size_t row_size);

// 批量插入，返回成功插入的行数
size_t db_batch_insert(table_t table, const void* rows, size_t row_size, size_t count);

// ---- Read ----

// 全局配置：最大返回行数（防止 OOM，默认 10000）
void db_config_max_rows(db_t db, size_t max_rows);

// ===== 方式 1：完整 JSON（适合小结果集，< 1000 行）=====
// 返回完整 JSON 字符串，内存由系统管理，需 db_json_free()

// 按主键查询，返回单行 JSON
const char* db_select_by_pk_json(table_t table, rowid_t id);

// 全表扫描（自动受 max_rows 限制）
const char* db_select_all_json(table_t table);

// WHERE 条件查询（自动受 max_rows 限制）
const char* db_select_where_json(table_t table,
                                 const db_condition_t* conditions,
                                 size_t condition_count);

// ORDER BY + LIMIT 组合查询（自动受 max_rows 限制）
const char* db_select_json(table_t table,
                            const db_condition_t* conditions, size_t condition_count,
                            size_t order_field_offset, int ascending,
                            size_t limit_offset, size_t limit_count);

// JOIN 关联查询（自动受 max_rows 限制）
const char* db_join_json(table_t left_table,  size_t left_field_offset,
                          table_t right_table, size_t right_field_offset,
                          size_t field_size);

// 释放 JSON 字符串内存
void db_json_free(const char* json);

// ===== 方式 2：流式查询（适合大结果集，无内存上限）=====
// 逐行回调，不生成完整 JSON，内存占用恒定

// 单行回调函数：返回 0 继续，非 0 停止
typedef int (*db_row_cb_t)(const char* json, void* user_data);

// 流式全表扫描
int db_select_all_stream(table_t table, db_row_cb_t cb, void* user_data);

// 流式 WHERE 查询
int db_select_where_stream(table_t table,
                           const db_condition_t* conditions, size_t condition_count,
                           db_row_cb_t cb, void* user_data);

// 流式 ORDER BY + LIMIT 查询
int db_select_stream(table_t table,
                     const db_condition_t* conditions, size_t condition_count,
                     size_t order_field_offset, int ascending,
                     size_t limit_offset, size_t limit_count,
                     db_row_cb_t cb, void* user_data);

// ---- Update ----
// 按主键更新一行（传入 C struct）
int db_update(table_t table, rowid_t id, const void* row, size_t row_size);

// ---- Delete ----
// 按主键删除一行（软删除，标记 DELETED）
int db_delete(table_t table, rowid_t id);

// ---- Compact（内存回收）----
// 重建单张表，回收已删除行的空间，更新所有索引
// 返回：删除行数
size_t db_table_compact(table_t table);

// 重建整个数据库，回收所有已删除空间
// 返回：总删除行数
size_t db_compact(db_t db);

// ====== 事务（Transaction）=====
// 注：当前版本 WAL 已保证单操作原子性，事务为多操作原子性扩展

int db_begin  (db_t db);      // 开始事务
int db_commit (db_t db);      // 提交事务
int db_rollback(db_t db);     // 回滚事务

#ifdef __cplusplus
}
#endif

#endif
```

### 6.3 C 主程序示例（编译时 Schema）

```c
#include "mydb.h"

// ========== 1. 定义数据结构（编译时确定） ==========
struct user {
    uint64_t id;
    char     name[32];
    int32_t  age;
    double   score;
};

struct order {
    uint64_t id;
    uint64_t user_id;
    double   amount;
    int64_t  created_at;
};

// ========== 2. 宏自动注册 Schema（编译时展开） ==========
DB_TABLE(user, "users",
    DB_FIELD(id,    DB_TYPE_UINT64),
    DB_FIELD(name,  DB_TYPE_STRING),
    DB_FIELD(age,   DB_TYPE_INT32),
    DB_FIELD(score, DB_TYPE_DOUBLE)
)

DB_TABLE(order, "orders",
    DB_FIELD(id,         DB_TYPE_UINT64),
    DB_FIELD(user_id,    DB_TYPE_UINT64),
    DB_FIELD(amount,     DB_TYPE_DOUBLE),
    DB_FIELD(created_at, DB_TYPE_INT64)
)

// ========== 3. 主程序 ==========
int main() {
    db_t db = db_open("data.bin", "data.index", "wal.bin", 1024*1024*100);
    
    // 注册所有表（自动调用 DB_TABLE 生成的函数）
    db_register_user(db);
    db_register_order(db);
    
    // 获取表句柄
    table_t users  = db_table(db, "users");
    table_t orders = db_table(db, "orders");
    
    // INSERT: 插入数据
    struct user u = {0, "Alice", 25, 95.5};
    rowid_t uid = db_insert(users, &u, sizeof(u));
    
    struct order o = {0, uid, 100.0, 1234567890};
    rowid_t oid = db_insert(orders, &o, sizeof(o));
    
    // SELECT: 查询返回 JSON
    const char* json = db_select_by_pk_json(users, uid);
    printf("%s\n", json);  // {"id":1,"name":"Alice","age":25,"score":95.5}
    db_json_free(json);
    
    // JOIN: 关联查询
    const char* join_json = db_join_json(users, offsetof(struct user, id),
                                         orders, offsetof(struct order, user_id),
                                         sizeof(uint64_t));
    printf("%s\n", join_json);  // [{"users.id":1,...,"orders.id":1,...}]
    db_json_free(join_json);
    
    db_close(db);
    return 0;
}
```

### 6.4 LuaJIT FFI 示例（JSON 查询结果）

**注意：FFI 客户端需要 C 主程序先注册好 Schema，FFI 侧直接使用表名获取句柄。**

```lua
local ffi = require("ffi")
local mydb = ffi.load("./libmydb.so")

ffi.cdef[[
    typedef struct db_instance* db_t;
    typedef struct db_table*    table_t;
    typedef uint64_t rowid_t;

    db_t db_open(const char* data_path, const char* index_path, const char* wal_path, size_t pool_size);
    void db_close(db_t db);
    table_t db_table(db_t db, const char* name);
    rowid_t db_insert(table_t table, const void* row, size_t row_size);
    const char* db_select_by_pk_json(table_t table, rowid_t id);
    const char* db_select_where_json(table_t table, const void* conditions,
                                     size_t condition_count);
    const char* db_select_all_json(table_t table);
    void db_json_free(const char* json);
    int db_delete(table_t table, rowid_t id);
]]

ffi.cdef[[
    struct __attribute__((packed)) user {
        uint64_t id;
        char     name[32];
        int32_t  age;
        double   score;
    };
]]

-- 打开数据库（Schema 已在 C 侧注册）
local db = mydb.db_open("data.bin", "data.index", "wal.bin", 1024*1024*100)
local users = mydb.db_table(db, "users")   -- 直接按名称获取

-- INSERT: 插入数据（传入 C struct）
local row = ffi.new("struct user")
ffi.copy(row.name, "Alice")
row.age = 25
row.score = 95.5
local id = mydb.db_insert(users, row, ffi.sizeof("struct user"))

-- SELECT: 查询返回 JSON 字符串
local json = mydb.db_select_by_pk_json(users, id)
print(ffi.string(json))   -- {"id":1,"name":"Alice","age":25,"score":95.5}
mydb.db_json_free(json)

-- SELECT WHERE: 条件查询
local cond = ffi.new("db_condition_t[1]")
cond[0].field_offset = 40
cond[0].field_size = 4
local age_val = ffi.new("int32_t[1]", 18)
cond[0].value = age_val
cond[0].op = 1  -- 大于

local result_json = mydb.db_select_where_json(users, cond, 1)
print(ffi.string(result_json))   -- [{"id":1,"name":"Alice","age":25,"score":95.5}]
mydb.db_json_free(result_json)

-- DELETE: 删除
mydb.db_delete(users, id)

mydb.db_close(db)
```

### 6.5 Python ctypes 示例（JSON 查询结果）

```python
import ctypes
from ctypes import *
import json

mydb = ctypes.CDLL("./libmydb.so")

# 定义字段结构（FFI 侧不需要，C 侧已注册）
class DBFieldDef(Structure):
    _fields_ = [
        ("name",   c_char_p),
        ("offset", c_size_t),
        ("size",   c_size_t),
        ("type",   c_int),
    ]

class User(Structure):
    _pack_ = 1
    _fields_ = [
        ("id",    c_uint64),
        ("name",  c_char * 32),
        ("age",   c_int32),
        ("score", c_double),
    ]

# 打开数据库（Schema 已在 C 侧注册）
db = mydb.db_open(b"data.bin", b"data.index", b"wal.bin", 1024*1024*100)
users = mydb.db_table(db, b"users")   # 直接按名称获取

# INSERT: 插入（传入 C struct）
row = User(id=0, name=b"Alice", age=25, score=95.5)
rid = mydb.db_insert(users, byref(row), sizeof(User))

# SELECT: 查询返回 JSON
mydb.db_select_by_pk_json.restype = c_char_p
json_str = mydb.db_select_by_pk_json(users, rid)
result = json.loads(json_str.decode())
print(result)   # {'id': 1, 'name': 'Alice', 'age': 25, 'score': 95.5}
mydb.db_json_free(json_str)

# SELECT WHERE: 条件查询
class DBCondition(Structure):
    _fields_ = [
        ("field_offset", c_size_t),
        ("field_size",   c_size_t),
        ("value",        c_void_p),
        ("op",           c_int),
    ]

age_val = c_int32(18)
cond = DBCondition(40, 4, byref(age_val), 1)  # age > 18
mydb.db_select_where_json.restype = c_char_p
json_str = mydb.db_select_where_json(users, byref(cond), 1)
results = json.loads(json_str.decode())
print(results)   # [{'id': 1, 'name': 'Alice', 'age': 25, 'score': 95.5}]
mydb.db_json_free(json_str)

# DELETE: 删除
mydb.db_delete(users, rid)

mydb.db_close(db)
```

## 7. 关键设计决策

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
| 变长字段 | offset 指向池中 | 字符串/数组等大字段外置 |
| 结构体对齐 | `__attribute__((packed))` 或自然对齐 | FFI 两侧必须一致 |

## 8. 目录结构

```
my_db/
├── design.md           # 本文件
├── README.md
├── LICENSE
├── Makefile
├── include/
│   └── mydb.h          # 公共头文件（FFI 依赖）
├── src/
│   ├── core/
│   │   ├── db.c              # 数据库生命周期、WAL
│   │   ├── table.c           # 表操作（struct 数组管理）
│   │   ├── pool.c            # mmap 内存池管理
│   │   └── query.c           # 查询优化器（简单规则）
│   ├── types/
│   │   ├── vector.c          # 动态数组（池中用）
│   │   ├── hash.c            # 哈希表（主键索引）
│   │   ├── btree.c           # B+树（二级索引）
│   │   └── composite_index.c # 复合索引（多列联合索引）
│   ├── storage/
│   │   ├── wal.c             # WAL 日志读写
│   │   └── mmap.c            # mmap 封装
│   └── utils/
│       ├── crc32.c           # 校验和
│       └── error.c           # 错误处理
├── tests/
│   ├── test_basic.c    # 基础功能测试
│   ├── test_wal.c      # WAL 恢复测试
│   └── test_ffi.lua    # LuaJIT FFI 测试
└── examples/
    ├── example.c       # C 示例（定义 struct user）
    ├── example.lua     # LuaJIT 示例
    └── example.py      # Python 示例
```

## 9. 并发设计：无锁，用户自行保证

### 9.1 设计哲学：类似 Redis

**本项目不提供任何锁机制**，并发安全由用户自行保证：

- **单线程使用**：最简单，无并发问题
- **多线程读**：如果数据不修改，多个线程可同时读
- **多线程写**：用户需在外层自行加锁（如 `pthread_mutex`）

### 9.2 为什么不内置锁？

| 原因 | 说明 |
|------|------|
| **缓存定位** | 类似 Redis，简单快速是第一优先级 |
| **避免复杂** | 锁 = 死锁、性能损耗、调试困难 |
| **用户可控** | 用户比框架更清楚自己的并发模型 |
| **零拷贝冲突** | 返回的指针无法被锁保护，用户必须自己管理生命周期 |

### 9.3 用户并发建议

```c
// 方案 1：单线程（最简单，推荐）
// 一个线程专职处理数据库操作

// 方案 2：读写分离
// 读线程：只读不修改，可并发
// 写线程：唯一写入者，无需锁

// 方案 3：用户自行加锁
pthread_mutex_t db_mutex;

void thread_safe_insert(table_t t, void* row) {
    pthread_mutex_lock(&db_mutex);
    db_insert(t, row, sizeof(row));
    pthread_mutex_unlock(&db_mutex);
}
```

### 9.4 零拷贝指针的并发风险

```c
// 危险：指针可能在其他线程修改后失效
void* ptr = db_select_ptr(table, id);  // 获取指针
// 另一个线程 db_delete(table, id)  ← 指针悬空！

// 安全：拷贝数据
struct user u;
db_select_by_pk(table, id, &u, sizeof(u));  // 拷贝到栈上
```

## 10. 已解决的设计问题

### 10.1 内存膨胀 → Compact 机制

**问题**：Bump Allocator + 软删除 = 内存只增不减

**方案**：
- `db_table_compact(table)`：单表重建，回收已删除空间
- `db_compact(db)`：全盘重建，回收所有已删除空间
- 重建过程：复制未删除行到新区域 → 更新所有索引 → 原子替换

**使用建议**：
- 删除比例 > 20% 时触发
- 业务低峰期执行（用户自行加锁保证独占访问）

### 10.2 JSON 大结果集 → 双模式查询

**问题**：SELECT 返回完整 JSON，大数据量可能 OOM

**方案**：

| 模式 | API | 适用场景 | 内存占用 |
|------|-----|---------|---------|
| **完整 JSON** | `db_select_*_json()` | 结果 < 1000 行 | 与结果集成正比 |
| **流式查询** | `db_select_*_stream()` | 结果任意大小 | 恒定（单行 JSON） |

**保护措施**：
- 默认 `max_rows = 10000`，超过返回 `DB_ERR_RESULT_TOO_LARGE`
- 可配置：`db_config_max_rows(db, 50000)`

## 11. 后续扩展

- [x] **内存回收**：Compact 机制（表级 + 全盘）
- [x] **大结果集保护**：流式查询 + 默认行数限制
- [ ] **自动 WAL 追踪**：`db_get()` 返回的指针被修改后，自动检测并写 WAL
- [ ] 可选锁模块（表级锁/MVCC，作为插件供需要并发的用户使用）
- [x] 查询优化器（简单规则：复合索引优先 → 单列索引 → 全表扫描）
- [x] 复合索引（多列联合索引，第一版即支持）
- [ ] 事务（BEGIN / COMMIT / ROLLBACK）
- [ ] 网络服务层（RPC）
- [ ] 在线备份（热拷贝 mmap 文件）
