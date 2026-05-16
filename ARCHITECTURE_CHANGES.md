# my_db 架构变更记录

## 版本对比：原设计 vs 实际实现

### 1. 存储架构：从"单文件双池"到"多文件目录模式"

**原设计：**
- 全局共享文件：`data.bin` + `data.index` + `wal.bin`
- 所有表的数据共存于一个 `.bin` 文件
- 所有索引共存于一个 `.index` 文件
- `db_open()` 接收三个文件路径参数

```c
// 原设计 API
db_t db_open("data.bin", "data.index", "wal.bin", 1024*1024*100);
```

**实际实现：**
- 目录模式：`db_open(db_dir, pool_size)`
- 每张表拥有**独立文件**：
  - `{tablename}.bin` —— 表数据
  - `{tablename}.index` —— 索引数据（零拷贝 mmap）
  - `{tablename}.idxmeta` —— 索引元数据（持久化索引定义）
  - `{tablename}.strings` —— 变长字符串池
  - `wal.bin` —— 单文件 WAL（放在目录下）

```c
// 实际 API
db_t db_open("game_data", 1024*1024*100);  // 只传目录名
```

**变更理由：**
- 单文件多表会导致 compact 时影响所有表
- 独立文件让每张表可独立扩展/compact
- 简化文件管理，目录即数据库

---

### 2. 索引持久化：从"内存指针"到"零拷贝 mmap"

**原设计：**
- 索引使用 `void* data` 指针指向堆内存（malloc）
- 关闭数据库时**丢失所有索引**
- 重启后需手动重建索引

```c
typedef struct db_index {
    void* data;  // 哈希表或 B+树指针（堆内存）
} db_index_t;
```

**实际实现：**
- 索引数据存储在 `{tablename}.index` mmap 文件中
- 使用 `size_t data_offset` 替代指针（相对 pool base 的偏移）
- 重启后索引**自动恢复**，无需重建

```c
typedef struct db_index {
    size_t data_offset;  // 索引数据在 index_pool 中的偏移
} db_index_t;
```

**新增索引元数据文件（`.idxmeta`）：**
- 原设计：索引定义和索引数据混存在同一个 `.index` 文件
- 实际：索引定义序列化在 `.idxmeta`，索引数据结构在 `.index`
- 避免索引元数据覆盖索引 hash bucket 区域

**变更理由：**
- 真正的零拷贝：内存布局 = 磁盘布局
- 重启后索引可用，避免全表重建的开销
- 用户无感知，体验一致

---

### 3. JOIN 实现：从"纯嵌套循环"到"Hash Join 优化"

**原设计：**
- 纯嵌套循环 JOIN：O(left_rows × right_rows)
- 无论右表是否有索引，都逐行比较

**实际实现：**
- **自动检测**右表是否有 hash 索引
- 有索引 → Hash Join：O(left_rows)
- 无索引 → 回退嵌套循环：O(left_rows × right_rows)

```c
// 实际实现逻辑
db_index_t* idx = find_index(right, right_field_offset);
if (idx && idx->type == INDEX_HASH) {
    // Hash Join：左表每行用 hash 查找右表
    for (each left row) {
        rid = hash_lookup(&right->index_pool, idx->data_offset, key, key_len);
        // O(1) 找到匹配行
    }
} else {
    // Nested Loop Join
    for (each left row) {
        for (each right row) {
            if (memcmp(...)) { ... }
        }
    }
}
```

**变更理由：**
- 关联查询性能提升 orders of magnitude
- 透明优化，无需用户干预

---

### 4. 内存管理：从"只增不减"到"Free List + Compact"

**原设计：**
- Bump Allocator，只分配不回收
- 删除只是标记位，内存永不释放
- 需要手动调用 `db_table_compact()` 整理

**实际实现：**
- **Free List**：删除的 rowid 放入空闲列表，INSERT 优先复用
- **自动 Compact**：删除率 ≥ 30% 时自动触发
- 数据迁移 + 索引重建 + 文件收缩

```c
typedef struct db_table {
    rowid_t*  free_list;        // 空闲 rowid 数组
    size_t    free_count;       // 空闲数量
    size_t    deleted_count;    // 累计删除数
    float     compact_threshold; // 自动 compact 阈值（默认 0.3）
} db_table_t;
```

**API 新增：**
```c
void db_table_set_compact_threshold(table_t table, float ratio);  // 设置阈值
```

**变更理由：**
- 避免频繁删除后内存膨胀
- 自动回收，用户无感知
- Free List 让 rowid 复用，避免文件无限增长

---

### 5. 字符串类型：从"定长唯一"到"定长 + 变长双模式"

**原设计：**
- 仅支持 `DB_TYPE_STRING`（定长 char[N]）
- 变长字段只能通过"offset 指向池中"间接实现
- 用户需要手动管理字符串池

**实际实现：**
- `DB_TYPE_STRING`：定长字符串（char[N]），适合短字符串
- **`DB_TYPE_VARSTRING`**：变长字符串，独立 `.strings` mmap 文件

```c
// 变长字符串在行中的存储
typedef struct {
    size_t offset;   // 在 string_pool 中的偏移
    size_t length;   // 字符串长度
} db_string_ref_t;
```

**使用方式：**
```c
// 定长（C struct 中定义 char[N]）
struct user {
    uint64_t id;
    char     name[32];   // 定长，最大 32 字节
};

// 变长（C struct 中定义 char*）
struct article {
    uint64_t id;
    char*    title;      // 变长，任意长度
    char*    content;    // 变长，任意长度
};
```

**变更理由：**
- 定长字符串有长度限制，浪费空间
- 变长字符串适合大文本、URL 等场景
- 独立 string pool 避免数据文件膨胀

---

### 6. 表元数据持久化

**原设计：**
- 表元数据（row_count, max_rowid 等）仅在内存中维护
- 重启后需要扫描全表重建

**实际实现：**
- 表元数据持久化在 `{tablename}.bin` 文件头部

```
data.bin 文件头（offset 16 起）：
  [row_count:8][max_rowid:8][deleted_count:8]
```

- 重启后直接从文件读取，无需扫描

---

### 7. `db_open` API 签名变更

| | 原设计 | 实际实现 |
|--|--------|----------|
| 参数 | `db_open(data_path, index_path, wal_path, pool_size)` | `db_open(db_dir, pool_size)` |
| 文件模式 | 3 个独立文件路径 | 1 个目录，自动管理所有表文件 |
| 示例 | `db_open("a.bin", "a.index", "wal.bin", size)` | `db_open("game_data", size)` |

---

### 8. 设计决策对比表（更新版）

| 决策项 | 原设计 | 实际实现 | 变更理由 |
|--------|--------|----------|----------|
| **存储架构** | 单文件双池 | 多文件目录模式（每表独立） | 避免单文件膨胀，独立 compact |
| **索引持久化** | 堆内存指针（重启丢失） | mmap 零拷贝 offset（重启保留） | 真正的零拷贝，重启可用 |
| **索引元数据** | 与索引数据混存 | 独立 `.idxmeta` 文件 | 避免元数据覆盖索引结构 |
| **JOIN 算法** | 纯嵌套循环 | Hash Join + 嵌套循环 fallback | 自动优化，性能提升 |
| **内存回收** | 只增不减，手动 compact | Free List + 自动 compact | 自动回收，无感知 |
| **字符串类型** | 仅定长 STRING | 定长 STRING + 变长 VARSTRING | 支持任意长度文本 |
| **字符串存储** | 内联在行中 | 独立 `.strings` mmap 文件 | 避免数据文件膨胀 |
| **表元数据** | 内存维护 | 持久化在 `.bin` 头部 | 重启无需扫描 |
| **db_open 参数** | 3 个文件路径 | 1 个目录路径 | 简化 API |
| **db_sync** | MS_ASYNC（异步） | MS_SYNC（同步） | 确保数据真正落盘 |

---

### 9. 新增文件类型

```
db_dir/
├── {table}.bin          # 表数据（新增：表元数据区）
├── {table}.index        # 索引数据（零拷贝 mmap）
├── {table}.idxmeta      # 索引元数据（新增）
├── {table}.strings      # 变长字符串池（新增）
└── wal.bin              # WAL 日志
```

---

### 10. 未变更的核心设计

以下设计与原方案一致：

- ✅ **mmap 零拷贝**：内存布局 = 磁盘布局
- ✅ **WAL 日志**：每次写先 fsync WAL
- ✅ **软删除**：标记位删除，避免内存碎片
- ✅ **无锁设计**：单线程/用户自行保证并发
- ✅ **JSON 查询结果**：SELECT 返回 JSON 字符串
- ✅ **流式查询**：逐行回调，恒定内存
- ✅ **编译时宏**：`DB_TABLE` / `DB_FIELD` 自动注册
- ✅ **主键自增**：`uint64_t id` 自动填充
- ✅ **复合索引**：多列联合索引
