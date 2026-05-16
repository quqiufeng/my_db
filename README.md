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
- **动态 Schema**：LuaJIT/Python 纯动态注册，无需预定义 C struct
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
│                     LuaJIT FFI 客户端                         │
│  纯 Lua 脚本控制 Schema + CRUD + JOIN + 索引                   │
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
│  表 "users" (row_size = 56 字节)          │
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

## 快速开始（LuaJIT）

### 1. 加载封装层

```lua
local mydb = require("mydb")
```

### 2. 打开数据库

```lua
local db = mydb.open("game_data.bin")
```

### 3. 注册表（纯 Lua 动态定义 Schema）

```lua
-- 注册表时同时指定索引：
-- 字符串 = 单列索引，表 = 复合索引
local users = db:register("users", {
    {name = "id",    type = "uint64"},
    {name = "name",  type = "string", size = 32},
    {name = "age",   type = "int32"},
    {name = "score", type = "double"},
}, {
    "name",                    -- 单列索引
    {"name", "age"},           -- 复合索引（name + age）
})
```

**无需 C struct**：封装层自动计算偏移和对齐，索引在注册时自动创建并维护

### 4. 完整生命周期示例

```lua
local mydb = require("mydb")

-- ═══════════════════════════════════════════════════════
-- 1. 打开数据库
-- ═══════════════════════════════════════════════════════
local db = mydb.open("game_data.bin")

-- ═══════════════════════════════════════════════════════
-- 2. 注册 Schema（纯 Lua 动态定义，无需 C struct）
--    第三个参数是索引列表：字符串=单列，表=复合
-- ═══════════════════════════════════════════════════════
local users = db:register("users", {
    {name = "id",     type = "uint64"},        -- 第一字段必须是 uint64 id
    {name = "name",   type = "string", size = 32},
    {name = "age",    type = "int32"},
    {name = "score",  type = "double"},
}, {
    "name",                    -- 单列索引（name 字段）
    {"name", "age"},           -- 复合索引（name + age）
})

local orders = db:register("orders", {
    {name = "id",      type = "uint64"},
    {name = "user_id", type = "uint64"},       -- 外键，关联 users.id
    {name = "amount",  type = "double"},
}, {
    "user_id",                 -- 单列索引（user_id 字段）
})

-- ═══════════════════════════════════════════════════════
-- 4. 插入数据（Lua 表自动序列化为 C 内存）
-- ═══════════════════════════════════════════════════════
local alice_id = users:insert({name = "Alice", age = 25, score = 95.5})
local bob_id   = users:insert({name = "Bob",   age = 30, score = 88.0})
local carol_id = users:insert({name = "Carol", age = 28, score = 92.0})

print("插入用户: Alice(id=" .. alice_id .. "), Bob(id=" .. bob_id .. ")")

-- 插入订单（user_id 关联到用户）
orders:insert({user_id = alice_id, amount = 100.0})
orders:insert({user_id = alice_id, amount = 250.0})
orders:insert({user_id = bob_id,   amount = 50.0})

-- ═══════════════════════════════════════════════════════
-- 5. 查询（JSON 结果自动反序列化为 Lua 表）
-- ═══════════════════════════════════════════════════════

-- 5.1 主键查询
local alice = users:find(alice_id)
print("主键查询: " .. alice.name .. ", age=" .. alice.age)

-- 5.2 全表查询
local all_users = users:select()
print("全表共 " .. #all_users .. " 人")
for _, u in ipairs(all_users) do
    print("  " .. u.name .. ", age=" .. u.age)
end

-- 5.3 WHERE 条件查询
local adults = users:where({
    {field = "age", op = 1, value = 25}   -- age > 25
})
print("成年人（age>25）:")
for _, u in ipairs(adults) do
    print("  " .. u.name)
end

-- 5.4 JOIN 关联查询（用户 JOIN 订单）
local ffi = require("ffi")
local user_orders = db:join_json(
    users,  ffi.offsetof("struct _", "id"),
    orders, ffi.offsetof("struct _", "user_id"),
    8   -- uint64 大小
)
-- 结果自动解析为 Lua 表数组
print("Alice 的订单:")
for _, row in ipairs(user_orders) do
    if row["users.name"] == "Alice" then
        print("  金额: " .. row["orders.amount"])
    end
end

-- ═══════════════════════════════════════════════════════
-- 6. 更新数据
-- ═══════════════════════════════════════════════════════
users:update(alice_id, {name = "Alice Updated", age = 26, score = 96.0})
print("更新 Alice 成功")

-- ═══════════════════════════════════════════════════════
-- 7. 删除数据（软删除）
-- ═══════════════════════════════════════════════════════
users:delete(carol_id)
print("删除 Carol 后，剩余 " .. users:count() .. " 人")

-- ═══════════════════════════════════════════════════════
-- 8. 持久化（确保数据落盘）
-- ═══════════════════════════════════════════════════════

-- 8.1 强制刷盘（同步等待写入完成）
db:sync()
print("数据已强制刷盘")

-- 8.2 Checkpoint（刷盘 + 清空 WAL）
db:checkpoint()
print("Checkpoint 完成，WAL 已清空")

-- ═══════════════════════════════════════════════════════
-- 9. 内存回收（Compact）
-- ═══════════════════════════════════════════════════════
local deleted_count = users:compact()
print("回收已删除空间: " .. deleted_count .. " 行")

-- ═══════════════════════════════════════════════════════
-- 10. 关闭数据库
-- ═══════════════════════════════════════════════════════
db:close()
print("数据库已安全关闭")
```

## 构建

```bash
make              # 编译 libmydb.so
make test         # 运行基础测试
make test_join    # 运行 JOIN 测试
make test_perf    # 运行性能测试（100万行）
make test_wal     # 运行 WAL + Checkpoint 测试
make example      # 编译 C 示例
make install      # 安装到 /usr/local
```

## LuaJIT API 参考

### 数据库生命周期

```lua
local db = mydb.open("data.bin")              -- 打开（自动生成 .index 和 .wal）
db:sync()                                      -- 强制刷盘（同步等待落盘）
db:checkpoint()                                -- 刷盘 + 清空 WAL
db:close()                                     -- 关闭数据库
db:max_rows(50000)                             -- 设置最大返回行数（默认 10000）
```

### Schema 注册（动态，含索引）

```lua
-- 注册表时同时指定索引（第三个参数）
-- 字符串 = 单列索引，表 = 复合索引
local users = db:register("users", {
    {name = "id",    type = "uint64"},        -- 第一字段必须是 uint64 id
    {name = "name",  type = "string", size = 32},
    {name = "age",   type = "int32"},
    {name = "score", type = "double"},
}, {
    "name",                    -- 单列索引
    {"name", "age"},           -- 复合索引（最多 4 个字段）
})

-- 支持的类型：int32, int64, uint64, float, double, string, bool
-- string 需指定 size（最大长度）

-- 后续手动创建索引（如果注册时没指定）
users:create_index("score")
users:create_index_composite({"age", "score"})
```

### CRUD

```lua
-- 插入（id 自动填充，返回自增主键）
local id = users:insert({name = "Alice", age = 25, score = 95.5})

-- 主键查询（返回 Lua 表）
local row = users:find(id)
-- row = {id = 1, name = "Alice", age = 25, score = 95.5}

-- 全表查询（返回 Lua 表数组）
local all = users:select()
-- all = {{id = 1, ...}, {id = 2, ...}}

-- WHERE 条件查询
local adults = users:where({
    {field = "age", op = 1, value = 25}   -- op: 0=等于, 1=大于, 2=小于
})

-- 更新
users:update(id, {name = "Alice Updated", age = 26, score = 96.0})

-- 删除（软删除）
users:delete(id)

-- 获取行数
local count = users:count()
```

### JOIN 查询

```lua
local ffi = require("ffi")

-- users.id = orders.user_id
local results = db:join_json(
    users,  0,      -- users.id 的偏移（id 在 offset 0）
    orders, 8,      -- orders.user_id 的偏移
    8               -- uint64 大小
)
-- 结果自动解析为 Lua 表数组：
-- {{"users.id" = 1, "users.name" = "Alice", "orders.amount" = 100.0}, ...}
```

### 流式查询（大数据量）

```lua
-- 逐行回调，不生成完整 JSON，内存占用恒定
users:stream(function(row)
    print(row.name)
    return 0   -- 返回 0 继续，非 0 停止
end)
```

### 内存回收

```lua
-- 单表重建，回收已删除空间
local deleted = users:compact()

-- 全盘重建，回收所有已删除空间
local total = db:compact()
```

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
| Schema 定义 | **动态 Lua 注册** | 无需编译 C struct，脚本层完全控制 |
| 持久化机制 | mmap + 异步 msync | 零拷贝，内存布局 = 磁盘布局 |
| 数据安全 | WAL 同步写入 | 每次写先 fsync WAL，再改内存 |
| 主键 | 自增 uint64_t | 简单高效，FFI 兼容 |
| 读取方式 | SELECT 返回 JSON 字符串 | 无需 FFI 侧定义 struct，自动解析为 Lua 表 |
| 写入方式 | Lua 表自动序列化 | `insert({...})` 自动转换为 C 内存 |
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

```lua
-- LuaJIT 中无多线程，天然单线程安全
-- 如需多线程，在写入前自行加锁
```

## 内存管理

### 内存膨胀问题

Bump Allocator + 软删除 = 内存只增不减

### 解决方案：Compact

```lua
-- 单表重建，回收已删除空间
local deleted = users:compact()

-- 全盘重建，回收所有已删除空间
local total = db:compact()
```

**使用建议**：
- 删除比例 > 20% 时触发
- 业务低峰期执行（用户自行加锁保证独占访问）

### 大结果集保护

| 模式 | API | 适用场景 | 内存占用 |
|------|-----|---------|---------|
| **完整 JSON** | `users:select()` / `users:where(...)` | 结果 < 1000 行 | 与结果集成正比 |
| **流式查询** | `users:stream(callback)` | 结果任意大小 | 恒定（单行 JSON） |

**保护措施**：
- 默认 `max_rows = 10000`，超过返回错误
- 可配置：`db:max_rows(50000)`

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
├── design.md              # 设计方案
├── README.md              # 本文件
├── task.md                # 开发任务列表
├── LICENSE
├── Makefile
├── mydb.lua               # LuaJIT FFI 高级封装（推荐）
├── mydb.py                # Python ctypes 高级封装
├── include/
│   ├── mydb.h             # 公共头文件（FFI 依赖）
│   └── mydb_internal.h    # 内部头文件
├── src/
│   ├── core/
│   │   ├── db.c           # 数据库生命周期
│   │   ├── table.c        # 表操作
│   │   └── query.c        # 查询引擎
│   ├── storage/
│   │   ├── wal.c          # WAL 日志
│   │   └── mmap.c         # mmap 封装
│   └── utils/
│       ├── crc32.c        # 校验和
│       ├── error.c        # 错误处理
│       └── json.c         # JSON 序列化
├── examples/
│   ├── example_dynamic.lua  # LuaJIT 动态示例（推荐）
│   └── example_dynamic.py   # Python 动态示例
└── tests/
    ├── test_basic.c       # 基础功能测试
    ├── test_join.c        # JOIN 测试
    ├── test_perf.c        # 性能测试
    ├── test_composite.c   # 复合索引测试
    └── test_wal.c         # WAL + Checkpoint 测试
```

## 许可证

Apache License 2.0
