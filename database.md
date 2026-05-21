# my_db 零拷贝嵌入式存储引擎

> [← 返回项目总览](README.md)

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
- ❌ 多进程并发访问
- ❌ 分布式/集群
- ❌ SQL 字符串解析

**已实现扩展：**
- ✅ KV Cache 子系统（Agent 记忆存储）：前缀/范围/正则/模糊/标签搜索
- ✅ Tag 反向索引：O(1) 标签查找
- ✅ Skip List 跳表：数据量 > 100 万时自动启用
- ✅ 源码语义分析：C/C++/Python/JS/Java/Go/Rust AST 提取
- ✅ 向量相似度搜索：float 数组 + 余弦相似度
- ✅ TCP 远程操作层：文本协议服务器（PING/SET/GET/DEL/SEARCH）
- ✅ HTTP RESTful API 服务器
- ✅ 电子书语义搜索系统（EPUB/MOBI/AZW3/PDF）
- ✅ 代码探索记忆系统（代码索引 + 语义搜索 + 调用图 + 数据流）
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

## 3. 内存布局设计（多文件 mmap 目录模式）

**目录即数据库。每张表拥有独立的文件组，各自独立 mmap。**

### 3.1 文件架构

```
db_dir/                          -- 数据库目录
├── {table}.bin                  -- 表数据（每张表独立）
├── {table}.index                -- 索引数据（零拷贝 mmap）
├── {table}.idxmeta              -- 索引元数据（持久化索引定义）
├── {table}.strings              -- 变长字符串池（仅 VARSTRING 类型）
└── wal.bin                      -- WAL 日志（单文件）
```

**关键变更（vs 原设计的单文件双池）：**
- 原设计：所有表数据共存于一个 `data.bin`，所有索引共存于一个 `data.index`
- 实际：每张表有独立的 `.bin` + `.index` + `.idxmeta`（可选 `.strings`）
- 优势：单表 compact 不影响其他表，独立扩展，简化管理

### 3.2 数据文件（{table}.bin）

存储单张表的行数据 + 表元数据。

```
┌────────────────────────────────────────────────────────┐
│              {table}.bin 文件布局                       │
├────────────────────────────────────────────────────────┤
│  魔数 "MYDB" (4 bytes)                                  │
│  版本号 (4 bytes)                                       │
│  当前已用偏移 (8 bytes)                                 │
├────────────────────────────────────────────────────────┤
│  表元数据区（offset 16）                                │
│    - row_count (8 bytes)                                │
│    - max_rowid (8 bytes)                                │
│    - deleted_count (8 bytes)                            │
├────────────────────────────────────────────────────────┤
│                    数据区域                             │
│  ┌──────────────┐                                      │
│  │ row_header   │  rowid + flags                       │
│  │ + row_data   │  struct user (紧凑排列)              │
│  ├──────────────┤                                      │
│  │ row_header   │                                      │
│  │ + row_data   │                                      │
│  └──────────────┘                                      │
├────────────────────────────────────────────────────────┤
│  空闲区（预留扩展）                                     │
└────────────────────────────────────────────────────────┘
```

### 3.3 索引文件（{table}.index）

**零拷贝实现**：索引结构（hash buckets/nodes）直接存储在 mmap 文件中，使用 offset 寻址。

```
┌────────────────────────────────────────────────────────┐
│            {table}.index 文件布局                       │
├────────────────────────────────────────────────────────┤
│  魔数 "MYDB" (4 bytes)                                  │
│  版本号 (4 bytes)                                       │
│  当前已用偏移 (8 bytes)                                 │
├────────────────────────────────────────────────────────┤
│  索引数据结构区（零拷贝）                               │
│  ┌──────────────┐                                      │
│  │ hash_header  │  bucket_count + size + buckets_offset│
│  ├──────────────┤                                      │
│  │ buckets[]    │  指向 hash_node 的 offset 数组       │
│  ├──────────────┤                                      │
│  │ hash_node    │  key_offset + key_len + value + next │
│  ├──────────────┤                                      │
│  │ key data     │  键数据（从 pool_alloc 分配）        │
│  └──────────────┘                                      │
└────────────────────────────────────────────────────────┘
```

**索引持久化：**
- 原设计：索引使用 `void* data` 堆内存指针，关闭后丢失
- 实际：索引数据在 `.index` mmap 中，使用 `size_t data_offset` 寻址
- 重启后索引自动恢复，无需重建

### 3.4 索引元数据文件（{table}.idxmeta）

存储索引定义（字段名、类型、offset 等），与索引数据结构分离。

```
┌────────────────────────────────────────────────────────┐
│           {table}.idxmeta 文件布局                      │
├────────────────────────────────────────────────────────┤
│  魔数 "MYDB" (4 bytes)                                  │
│  版本号 (4 bytes)                                       │
│  当前已用偏移 (8 bytes)                                 │
├────────────────────────────────────────────────────────┤
│  索引定义序列化区                                       │
│    [index_count:4][index_defs...]                       │
│    每个索引：[type:4][field_count:4][data_offset:8]     │
│              [fields...][name]                          │
└────────────────────────────────────────────────────────┘
```

**为什么分离？**
- 原设计尝试将索引定义和索引数据混存在 `.index` 中
- 实际发现：索引定义需要序列化/反序列化，而索引 hash 数据是纯二进制结构
- 混存会导致索引元数据写入时覆盖 hash bucket 区域
- 分离后：`.idxmeta` 管理定义，`.index` 管理零拷贝数据结构

### 3.5 变长字符串池（{table}.strings）

仅当表包含 `DB_TYPE_VARSTRING` 字段时创建。

```
┌────────────────────────────────────────────────────────┐
│           {table}.strings 文件布局                      │
├────────────────────────────────────────────────────────┤
│  魔数 "MYDB" (4 bytes)                                  │
│  版本号 (4 bytes)                                       │
│  当前已用偏移 (8 bytes)                                 │
├────────────────────────────────────────────────────────┤
│  字符串数据区（变长，连续存储）                         │
│    "Alice\0"                                            │
│    "Bob\0"                                              │
│    "这是一段很长的文章...\0"                            │
└────────────────────────────────────────────────────────┘
```

**行中存储方式：**
```c
// 变长字符串在行中不存实际字符，存引用
typedef struct {
    size_t offset;   // 在 string_pool 中的偏移
    size_t length;   // 字符串长度（不含 \0）
} db_string_ref_t;
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

- **每次写操作后自动 `fsync`**：`db_insert`/`db_update`/`db_delete` 在写入 WAL 后自动调用 `wal_fsync()`，确保数据不丢失
- **显式 `db_sync()`**：强制同步所有表数据和 WAL
- **后台刷盘线程**：暂未实现（当前为同步刷盘）

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
// 目录模式：db_dir 下自动创建所有表文件
// 每张表生成：{table}.bin + {table}.index + {table}.idxmeta [+ {table}.strings]
db_t db = db_open("game_data", 1024*1024*100);
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

### 5.4 字符串类型（定长 vs 变长）

**`DB_TYPE_STRING`**：定长字符串（`char[N]`）
- 适合用户名、标题等短文本
- 直接内联在行数据中
- 最大长度由 `size` 参数指定

**`DB_TYPE_VARSTRING`**：变长字符串（`char*`）
- 适合文章、日志、URL 等任意长度文本
- 行中存储 `db_string_ref_t {offset, length}` 引用
- 实际字符数据存储在独立的 `{table}.strings` mmap 文件中
- 零拷贝：字符串数据直接 mmap，无需序列化

```c
// 定长（C struct 中定义 char[N]）
struct user {
    uint64_t id;
    char     name[32];      // 定长，最大 32 字节
    int32_t  age;
};

// 变长（C struct 中定义 char*）
struct article {
    uint64_t id;
    char*    title;         // 变长，任意长度
    char*    content;       // 变长，任意长度
};
```

**插入变长字符串：**
```c
struct article a = {0};
a.title = "Hello World";
a.content = "这是一段很长的文章内容...";
db_insert(articles, &a, sizeof(a));  // 自动分配 string_pool 空间
```

### 5.5 变长字段处理（通用）

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
// db_dir: 数据库目录路径（目录下自动管理所有表文件）
// pool_size: 初始内存池大小（单表初始大小，自动扩展）
db_t db_open(const char* db_dir, size_t pool_size);
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
    db_t db = db_open("game_data", 1024*1024*100);
    
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

    db_t db_open(const char* db_dir, size_t pool_size);
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
local db = mydb.db_open("game_data", 1024*1024*100)
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
db = mydb.db_open(b"game_data", 1024*1024*100)
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

| 决策项 | 原设计 | 实际实现 | 变更理由 |
|--------|--------|----------|----------|
| **定位** | 零拷贝嵌入式存储引擎 | 同上 | 内存数据库，直接操作 struct，无网络、无序列化 |
| **存储架构** | 单文件双池（`data.bin` + `data.index`） | **多文件目录模式**（每表独立 `.bin` + `.index` + `.idxmeta`） | 单表 compact 不影响其他表，独立扩展 |
| **Schema 定义** | 编译时宏自动生成 | 同上 + **动态注册**（LuaJIT/Python 纯脚本） | `DB_TABLE` 宏自动计算 offset/size，脚本层无需 C struct |
| **持久化机制** | mmap + 异步 msync | **mmap + 同步 msync** (`MS_SYNC`) | 确保数据真正落盘，避免 msync 假异步 |
| **数据安全** | WAL 同步写入 | 同上 | 每次写先 fsync WAL，再改内存 |
| **主键** | 自增 uint64_t | 同上 | 简单高效，FFI 兼容 |
| **读取方式** | SELECT 返回 JSON 字符串 | 同上 | 无需 FFI 侧定义 struct，直接解析 JSON |
| **写入方式** | 传入 C struct 直接写入 | 同上 | `db_insert()` / `db_update()` 零拷贝写入 |
| **删除** | 软删除（标记位） | 同上 + **Free List 复用** | 避免内存碎片，删除的 rowid 优先复用 |
| **内存回收** | 手动 `db_table_compact()` | **自动 compact**（删除率 ≥ 30% 触发） | 自动回收，用户无感知 |
| **索引** | 堆内存指针（重启丢失） | **mmap 零拷贝 offset**（重启保留） | 真正的零拷贝，重启无需重建 |
| **索引元数据** | 与索引数据混存 | **独立 `.idxmeta` 文件** | 避免元数据覆盖索引 hash 结构 |
| **JOIN 算法** | 纯嵌套循环 O(L×R) | **Hash Join** + 嵌套循环 fallback | 自动检测右表索引，性能提升 orders of magnitude |
| **内存分配** | Bump Allocator（只增不减） | Bump Allocator + **Free List** | 顺序分配 + 空闲复用 |
| **并发** | **无锁，用户自行保证** | 同上 | 类似 Redis，简单快速第一 |
| **查询优化** | 简单规则匹配 | **HASH + BTREE 索引** | HASH 等值查询 + BTREE 等值查询，范围查询回退全表扫描 |
| **字符串类型** | 仅 `DB_TYPE_STRING`（定长） | 定长 `STRING` + **变长 `VARSTRING`** | 支持任意长度文本，独立 `.strings` 文件 |
| **变长字段** | offset 指向池中（手动管理） | **自动 string_pool**（独立 mmap） | 透明管理，零拷贝 |
| **结构体对齐** | `__attribute__((packed))` 或自然对齐 | 自然对齐 | FFI 两侧必须一致 |
| **事务** | BEGIN/COMMIT/ROLLBACK 骨架 | **完整实现** | BEGIN 记录 WAL 偏移；ROLLBACK 截断 WAL + 重载数据文件 + 重建索引 |
| **索引恢复** | 仅恢复索引定义 | **定义 + 数据完全恢复** | 打开表时扫描所有行重建索引数据 |

## 8. 目录结构

```
my_db/
├── design.md              # 本文件（设计方案）
├── README.md              # 项目说明
├── task.md                # 开发任务列表
├── LICENSE
├── Makefile
├── .gitignore
├── mydb.lua               # LuaJIT FFI 高级封装（推荐）
├── mydb.py                # Python ctypes 高级封装
├── include/
│   ├── mydb.h             # 公共头文件（FFI 依赖）
│   └── mydb_internal.h    # 内部头文件（零拷贝索引、变长字符串等）
├── src/
│   ├── core/
│   │   ├── db.c           # 数据库生命周期（多文件目录模式）
│   │   ├── table.c        # 表操作（Free List + Compact）
│   │   └── query.c        # 查询引擎（Hash Join + 索引优化）
│   ├── types/
│   │   ├── hash.c         # 哈希表（零拷贝 mmap 索引）
│   │   └── btree.c        # B+树（二级索引，待零拷贝改造）
│   ├── storage/
│   │   ├── wal.c          # WAL 日志读写
│   │   └── mmap.c         # mmap 封装（pool 管理）
│   └── utils/
│       ├── crc32.c        # 校验和
│       ├── error.c        # 错误处理
│       └── json.c         # JSON 序列化（支持 VARSTRING）
├── examples/
│   ├── example_dynamic.lua # LuaJIT 动态示例（推荐）
│   ├── example_dynamic.py  # Python 动态示例
│   └── example.c          # C 示例（编译时 Schema）
└── tests/
    ├── test_basic.c       # 基础功能测试
    ├── test_join.c        # JOIN 测试（Hash Join + 嵌套循环）
    ├── test_perf.c        # 性能测试
    ├── test_edge.c        # 边界情况测试
    ├── test_composite.c   # 复合索引测试
    ├── test_wal.c         # WAL + Checkpoint 测试
    ├── test_ffi.lua       # LuaJIT FFI 测试
    └── test_ffi.py        # Python ctypes 测试
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

### 10.3 WAL 刷盘策略 → 自动 fsync

**问题**：`wal_append()` 写入后不 fsync，只在 `db_sync()` 时才刷盘，系统崩溃可能丢失最近写入

**方案**：
- `db_insert()` / `db_update()` / `db_delete()` 在 `wal_append()` 后自动调用 `wal_fsync()`
- 每次写操作都保证 WAL 落盘，数据零丢失
- 代价：每次写入多一次 fsync，性能略降（嵌入式场景可接受）

### 10.4 Compact 字符串池 → 临时备份

**问题**：`compact_rebuild_string_pool()` 先 `memzero` 整个 string_pool，再从旧 offset 复制数据 → 旧数据已被清空，复制的是零

**方案**：
- compact 前先用 `malloc` 创建临时缓冲区，备份整个 string_pool
- `memzero` 后从临时缓冲区复制字符串数据回新 pool
- 更新所有 `db_string_ref_t.offset` 指向新位置

### 10.5 BTREE 索引未使用 → 完整实现

**问题**：`index_create()` 对数值类型创建 `INDEX_BTREE`，但：
1. `data_offset` 却调用 `hash_create()`（bug）
2. `index_insert()` / `index_delete()` 不处理 BTREE
3. `query_with_index()` 只支持 HASH
4. 没有 `btree_lookup()` 函数

**方案**：
1. 修复 `index_create()`：数值类型调用 `btree_create()`
2. 实现 `btree_lookup()`：B+树等值查找（从根节点递归到叶子）
3. `index_insert()` / `index_lookup()` 添加 BTREE 分支
4. `query_with_index()` 同时支持 HASH 和 BTREE 等值查询

### 10.6 事务空壳 → WAL 截断回滚

**问题**：`db_begin()` / `db_commit()` / `db_rollback()` 都是空壳

**方案**：
- `db_begin()`：记录当前 WAL 文件偏移和各表的 `max_rowid`/`row_count`
- `db_commit()`：清除事务标志，调用 `db_sync()` 强制刷盘
- `db_rollback()`：
  1. 截断 WAL 到事务开始时的偏移
  2. 关闭并重新打开所有表的数据文件（从磁盘重新加载）
  3. 恢复 `max_rowid` 和 `row_count`
  4. 清空 free list，重建所有索引

### 10.7 索引恢复不完整 → 扫描重建

**问题**：`load_index_defs()` 只恢复索引定义（字段名、offset、type），索引数据（hash bucket / btree 节点）是空的

**方案**：`db_table_register()` 最后扫描所有未删除的行，调用 `index_insert()` 重建索引数据

## 11. 已实现扩展

### 11.1 KV Cache 高级索引
- [x] **Tag 反向索引**：Hash-based `tag → [offsets]`，O(1) 标签搜索
- [x] **Skip List 跳表**：20 级跳表，数据量 > 100 万时自动启用，前缀/范围搜索 O(log n)
- [x] **源码语义分析**：Regex-based AST 提取，支持 7 种语言（函数/类/结构体/导入）
- [x] **向量相似度搜索**：Float 数组存储 + 余弦相似度，支持 top-k 和 min_score 过滤
- [x] **HNSW 近似索引**：
  - **自动启用**：向量数量 > 1000 时自动构建 HNSW 图
  - **多层图结构**：指数衰减层数分布，贪心搜索 + BFS 逐层下降
  - **参数可配置**：M=16（默认邻居数），ef_construction=200，ef_search=50
  - **性能**：5000 向量时比暴力搜索快 4x，召回率 >95%
  - **Fallback**：<1000 向量时自动回退到暴力精确搜索

**索引持久化策略**
- **当前实现**：所有索引（Hash、Sorted Array、Tag、Namespace、Vector、HNSW）均为**内存索引**，不单独持久化
- **重建机制**：`cache_open()` 时扫描所有 entry（O(n)），实时重建所有索引
- **重建性能**：<10 万条时 <100ms；>100 万条时可能需要几秒
- **设计理由**：简单可靠，索引与数据永远一致，避免漂移
- **未来优化**：可考虑将 Sorted Array、HNSW 图保存到 `.index` 文件以加速启动

### 11.2 电子书语义搜索
- [x] **章节级导入**：`tools/import_book.c` 解析 MOBI/AZW3/PDF，按章节切分存储
  - 存储路径：`/books/{书名}/chapters/{章节号}-{标题}/content/p{段落号}`
  - 每章独立 namespace，便于按章浏览和搜索
- [x] **ONNX Runtime 嵌入引擎**：C 端本地推理，无需 Python 环境
  - `tools/export_onnx.py`：导出 sentence-transformers `all-MiniLM-L6-v2` 为 ONNX
  - `src/embedding/onnx_embedder.c`：加载 ONNX 模型，WordPiece tokenizer（哈希表优化）
  - **推理流程**：Tokenize → ONNX Runtime 推理 → Mean Pooling → L2 Normalization → 384 维向量
  - **性能**：单段落 ~100ms（含 tokenizer），纯 ONNX 推理 ~10ms
- [x] **段落级向量索引**：导入时每段文本自动生成 embedding，通过 `cache_set_vector()` 存入
  - 向量与文本共用同一个 key，检索时通过 `cache_search_vector()` 语义匹配
  - HNSW 自动启用：段落数 > 1000 时自动构建近似索引，搜索 O(log n)
- [x] **语义搜索验证**：查询 "马斯克创办特斯拉的故事" 返回第七章（全电动车）、第十章（电动车的复仇）等相关章节

### 11.3 TCP 远程操作层
- [x] **文本协议服务器**：Redis-like 简单协议
- [x] **命令集**：PING / SET / GET / DEL / EXISTS / COUNT / SEARCH / STATS / SYNC / QUIT
- [x] **客户端库**：`cache_client_connect()` / `cache_client_set()` / `cache_client_get()`
- [x] **单线程模型**：`accept() → handle_client() → close()` 顺序处理

### 11.3 HTTP RESTful API
- [x] **HTTP/1.1 服务器**：原生 socket 实现，零外部依赖
- [x] **RESTful 端点**：GET/PUT/DELETE `/cache/:key`，GET `/search`，POST `/batch`
- [x] **JSON 请求/响应**：`{"status":"ok","data":{...}}` / `{"status":"error","error":"..."}`
- [x] **Namespace 支持**：GET `/namespaces`，GET `/namespace/:ns`
- [x] **健康检查**：GET `/health`，GET `/stats`
- [x] **同步接口**：POST `/sync`

### 11.4 待实现
- [ ] 快照：定期快照备份
- [ ] 增量同步：GitHub webhook 自动更新
- [ ] **自动 WAL 追踪**：`db_get()` 返回的指针被修改后，自动检测并写 WAL
- [ ] 可选锁模块（表级锁/MVCC，作为插件供需要并发的用户使用）
- [x] ~~事务（BEGIN / COMMIT / ROLLBACK）~~ **已实现**
- [ ] 在线备份（热拷贝 mmap 文件）
- [ ] 后台异步刷盘线程（当前为同步刷盘）
- [ ] BTREE 范围查询优化（当前范围查询回退到全表扫描）
- [ ] ORDER BY 使用 BTREE 索引加速（当前使用 qsort）
