# my_db C 语言开发最佳实践

> 基于 my_db 项目所有 git 提交中的 bug 修复和性能优化经验总结
> 项目: https://github.com/quqiufeng/my_db

---

## 1. 内存管理

### 1.1 避免内存泄漏 - 建立清晰的释放链

**问题**: `db_close()` 中遗漏了 `db_index_t` 链表和 `free_list` 的释放。

**修复**:
```c
// ❌ 错误：只释放了部分资源
for (size_t j = 0; j < inst->tables[i]->field_count; j++) {
    free((void*)inst->tables[i]->fields[j].name);
}
free(inst->tables[i]->fields);

// ✅ 正确：完整的释放链
free_table_indexes(inst->tables[i]);   // 释放索引链表
free_table_fields(inst->tables[i]);    // 释放字段定义
free_list_destroy(inst->tables[i]);    // 释放 free_list
free(inst->tables[i]);                 // 释放表本身
```

**实践**: 每个分配资源的函数都应有对应的释放函数，并在同一个地方集中调用。

### 1.2 零拷贝设计 - 用 Offset 替代指针

**问题**: 使用堆指针在 mmap 地址变化时会失效。

**修复**:
```c
// ❌ 错误：堆指针在 mmap 重新映射后失效
hash_node_t* node = malloc(sizeof(hash_node_t));
node->next = some_ptr;  // 地址可能变化

// ✅ 正确：使用 pool offset
#define POOL_PTR(pool, offset) ((void*)((char*)(pool)->base + (offset)))

hash_node_t* node = POOL_PTR(pool, node_offset);
node->next_offset = next_offset;  // 存储 offset，不是指针
```

**实践**: 当数据存储在 mmap 中时，永远使用 offset（`size_t`）而非指针。

### 1.3 复杂的资源分配使用 helper 函数

**问题**: `db_table_register()` 超过 130 行，多处重复的错误处理代码。

**修复**:
```c
// ✅ 正确：拆分为职责单一的 helper
static int copy_field_defs(db_table_t* table, const db_field_def_t* fields, size_t field_count);
static void free_field_defs(db_table_t* table);
static int init_table_pools(db_table_t* table, const char* db_dir, const char* name);
```

**实践**: 当一个函数超过 80 行或有多处重复的错误清理代码时，拆分为 helper 函数。

---

## 2. 字符串安全

### 2.1 永远不要使用 strncpy

**问题**: `strncpy` 不会保证 null 终止，且可能静默截断。

**修复**:
```c
// ❌ 错误：可能不终止，且编译器警告
strncpy(table->name, name, MYDB_TABLE_NAME_LEN - 1);

// ✅ 正确：总是终止，清晰语义
snprintf(table->name, MYDB_TABLE_NAME_LEN, "%s", name);
```

**实践**: 总是使用 `snprintf(dst, sizeof(dst), "%s", src)` 替代 `strncpy`。

### 2.2 字符串比较使用 strcmp，不是 memcmp

**问题**: `memcmp` 比较固定长度，会将填充的 `\0` 也纳入比较。

**修复**:
```c
// ❌ 错误：memcmp 会比较填充字节
if (memcmp(field_ptr, cond->value, field_size) == 0) ...

// ✅ 正确：strcmp 遇到 \0 就停止
if (field_type == DB_TYPE_STRING) {
    cmp = strcmp((char*)field_ptr, (char*)cond->value);
}
```

**实践**: C 字符串（`char*`）始终使用 `strcmp`/`strncmp`，二进制数据才用 `memcmp`。

---

## 3. I/O 与持久化

### 3.1 区分同步与异步刷盘

**问题**: `msync(MS_ASYNC)` 不会等待数据写入磁盘，只是提交到内核。

**修复**:
```c
// ❌ 错误：MS_ASYNC 立即返回，数据可能在内核缓冲
msync(pool->base, pool->size, MS_ASYNC);

// ✅ 正确：MS_SYNC 等待物理写入完成
msync(pool->base, pool->size, MS_SYNC);
```

**实践**: 提供两个接口：
- `db_sync()` - 同步（`MS_SYNC`），等待完成
- 内部写入 - 异步（不 fsync），批量提交

### 3.2 WAL 不要每次 fsync

**问题**: 每次 WAL append 都 `fsync` 导致性能从 60万行/秒降到卡住。

**修复**:
```c
// ❌ 错误：每次写入都刷盘
write(fd, buf, size);
fsync(fd);  // 瓶颈！

// ✅ 正确：批量写入，显式 sync 时统一刷盘
write(fd, buf, size);  // 只写 page cache
// db_sync() 时才 fsync
```

**实践**: WAL 日志可以缓冲，由外部调用 `db_sync()` 或定时 checkpoint 统一刷盘。

---

## 4. 数据结构

### 4.1 Hash 表自动扩容

**问题**: 固定 64 个 bucket，数据量大时冲突链过长。

**修复**:
```c
// ✅ 正确：负载因子 > 0.75 时翻倍扩容
if (header->size > 0 && header->size >= header->bucket_count * 3 / 4) {
    hash_resize(pool, header);  // bucket_count *= 2
}
```

**实践**: Hash 表必须支持动态扩容，负载因子阈值通常为 0.7-0.75。

### 4.2 预留头空间用于元数据

**问题**: 文件开头直接存数据，无法扩展元数据。

**修复**:
```c
// ✅ 正确：文件头预留 16 字节
// [magic:4][version:4][used:8]
#define POOL_HEADER_SIZE 16

// 元数据存储在 offset 16 处
#define TABLE_META_OFFSET 16
// [row_count:8][max_rowid:8][deleted_count:8]
```

**实践**: 数据文件开头始终预留固定大小的文件头，用于存储 magic、version、used 等元数据。

---

## 5. 性能优化

### 5.1 避免频繁的 malloc/free

**问题**: JSON 序列化中每行每字段都 `malloc`/`free`，性能差。

**修复**:
```c
// ❌ 错误：每行都 malloc
typedef struct {
    char* data;
    size_t offset;
    size_t capacity;
} json_buf_t;

// 初始化一次，动态扩展
json_buf_t buf = {malloc(256), 0, 256};
// 多次 append，最后统一 realloc 到实际大小
char* result = realloc(buf.data, buf.offset + 1);
```

**实践**: 批量操作中，使用可复用的动态 buffer，避免每轮都 malloc/free。

### 5.2 批量操作优于单条

**问题**: 单条插入每次更新文件头 `used` 字段。

**优化**:
```c
// 批量插入时，只在最后更新 used
for (...) {
    db_insert(table, row, size);  // 内部可能多次更新 used
}
// 或者提供 db_batch_insert() API
```

**实践**: 提供批量 API（`db_batch_insert`），内部优化减少系统调用和元数据更新次数。

---

## 6. 错误处理

### 6.1 错误码使用负数

**实践**:
```c
#define DB_OK           0
#define DB_ERR_INVAL   -1
#define DB_ERR_IO      -2
#define DB_ERR_NOMEM   -3
#define DB_ERR_NOENT   -4
#define DB_ERR_EXIST   -5
#define DB_ERR_CORRUPTED -6
```

**原则**: 
- `0` = 成功
- 负数 = 错误码
- 正数 = 有意义的返回值（如 rowid、count）

### 6.2 资源分配使用 goto 或嵌套 if

**问题**: 多层资源分配时，错误处理代码重复且容易遗漏。

**修复**:
```c
// ✅ 正确：使用 goto 统一清理
int init_table_pools(db_table_t* table, ...) {
    if (pool_init(&table->data_pool, ...) < 0) return -1;
    if (pool_init(&table->index_pool, ...) < 0) goto fail_data;
    if (pool_init(&table->meta_pool, ...) < 0) goto fail_index;
    return 0;
    
fail_index:
    pool_close(&table->index_pool);
fail_data:
    pool_close(&table->data_pool);
    return -1;
}
```

**实践**: 多层资源分配使用 `goto fail_*` 模式，确保任意层失败都能正确释放已分配资源。

---

## 7. 调试与可观测性

### 7.1 提供 db_check() 诊断工具

**实践**:
```c
int db_check(const char* db_dir);
// 返回 0 = 正常
// 返回 DB_ERR_IO = 目录/文件无法访问
// 返回 DB_ERR_CORRUPTED = 文件头损坏或元数据不一致
```

**诊断内容**:
- 目录是否存在
- 每个 pool 文件的 magic/version/used 是否有效
- 表元数据一致性（row_count <= max_rowid 等）

### 7.2 压力测试覆盖主要场景

**实践**: 性能测试应覆盖：
- 批量插入（10万+ 行）
- 主键查询（1万+ QPS）
- 批量更新
- 批量删除（触发 compact）
- Compact 后数据完整性
- 流式查询全表
- db_check 验证

---

## 8. 代码组织

### 8.1 内部 API 与公共 API 分离

```
include/
  mydb.h           # 公共 API（FFI-friendly）
  mydb_internal.h  # 内部数据结构
src/
  core/            # 核心业务逻辑
  types/           # 数据结构实现（hash, btree）
  storage/         # 存储层（mmap, wal）
  utils/           # 工具函数（json, crc32）
tests/
  test_*.c         # 单元测试
```

### 8.2 测试即文档

**实践**: 每个测试用例都应有明确的断言和输出：
```c
#define CHECK(cond, msg) do { \
    test_count++; \
    if (cond) { \
        pass_count++; \
        printf("[OK] %s\n", msg); \
    } else { \
        printf("[FAIL] %s\n", msg); \
    } \
} while(0)
```

---

## 9. 兼容性

### 9.1 文件格式版本控制

```c
#define MYDB_MAGIC_DATA  "MYDB"
#define MYDB_VERSION     1

// 文件头格式（16 字节）
// [magic:4][version:4][used:8]
```

**实践**: 
- 每个数据文件都有 magic 和 version
- 加载时验证，版本不匹配则返回错误
- 预留扩展空间（如 unused 字段）

### 9.2 架构无关的类型

**实践**: 使用 `<stdint.h>` 的固定宽度类型：
```c
uint64_t id;    // 不是 unsigned long
int32_t  age;   // 不是 int
size_t   offset; // 内存偏移用 size_t
```

---

## 10. 项目特定的模式

### 10.1 Pool-Based 内存管理

本项目使用 mmap pool 作为核心抽象：
```c
typedef struct {
    int     fd;
    void*   base;
    size_t  size;
    size_t  used;
    size_t  capacity;
} db_pool_t;
```

**原则**:
- 所有持久化数据通过 `pool_alloc()` 分配
- 使用 `POOL_PTR(pool, offset)` 访问数据
- 数据结构存储 offset，不存储指针
- `pool_close()` 统一释放（munmap）

### 10.2 行存储格式

```c
// 每行：[row_header_t][row_data]
typedef struct {
    rowid_t rowid;
    uint8_t flags;  // MYDB_DELETED_FLAG
} row_header_t;

// 行位置计算：offset = id * row_stride
// row_stride = align(sizeof(row_header_t) + row_size, MYDB_ALIGN)
```

**原则**:
- 固定步长存储，支持 O(1) 直接寻址
- 删除标记（soft delete），支持 free list 复用
- Compact 时物理整理碎片

### 10.3 索引生命周期

```c
// 索引创建时分配 pool 资源
index->data_offset = hash_create(&table->index_pool);

// compact 时重建（因为 rowid 变化）
hash_destroy(&t->index_pool, idx->data_offset);
idx->data_offset = hash_create(&t->index_pool);

// 关闭时 pool_close 统一释放，无需逐个 free
```

---

## 总结

### 最重要的 5 条原则

1. **Offset 优于指针** - mmap 场景下永远使用 `size_t offset`，配合 `POOL_PTR()` 访问
2. **完整释放链** - 每个 `alloc` 都有对应的 `free`，集中在一个释放函数中
3. **批量优于单条** - WAL 不 fsync、JSON 用动态 buffer、提供 batch API
4. **版本控制文件格式** - magic + version + used 的文件头，加载时严格验证
5. **测试覆盖边界** - 空表、越界、重复、泄漏、性能，每个场景都有断言

---

*文档生成时间: 2026-05-16*
*基于提交: 2a354bb .. 97a8a17*
