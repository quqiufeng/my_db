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

## 11. KV Cache 审计最佳实践（三轮代码审查经验）

### 11.1 栈分配优于堆分配

**问题**: `cache_find_entry()` 每次查询都 `malloc(sizeof(cache_entry_t))`，在 `cache_get` 返回后 caller 忘记 `free`，造成每查询一次的内存泄漏。

**修复**:
```c
// ❌ 错误：内部 malloc，caller 容易忘记 free
cache_entry_t* cache_find_entry(cache_t* cache, const char* key) {
    cache_entry_t* entry = malloc(sizeof(cache_entry_t));  // 泄漏！
    cache_entry_parse(cache, offset, entry);
    return entry;
}
const char* cache_get(...) {
    cache_entry_t* entry = cache_find_entry(cache, key);
    // ... 使用 entry ...
    // 忘记 free(entry) → 泄漏
}

// ✅ 正确：调用者提供栈上的结构体
static int cache_find_entry(cache_t* cache, const char* key, cache_entry_t* out_entry) {
    cache_entry_parse(cache, offset, out_entry);
    return 1;
}
const char* cache_get(...) {
    cache_entry_t entry;  // 栈上分配
    if (!cache_find_entry(cache, key, &entry)) return NULL;
    // ... 使用 entry ...
    // 自动释放，无需 free
}
```

**实践**: 对于不超过 64 bytes 的临时结构体，优先使用栈分配；如果必须堆分配，确保 `malloc` 和 `free` 在同一函数的同一缩进层级。

### 11.2 多索引一致性：删除/更新时同步所有索引

**问题**: `cache_del` 或 LRU 淘汰时只从部分索引（hash + sorted + namespace）移除，遗漏了 tag_index、vector_index、hot_cache，导致这些索引指向已删除/已释放的 entry。

**修复**:
```c
// ❌ 错误：只移除了部分索引
cache_hash_remove(cache, key, key_len);
cache_sorted_remove(cache, key, key_len);
cache_ns_remove(cache, key);
// 遗漏：tag_index, vector_index, hot_cache 仍指向已删除的 entry

// ✅ 正确：统一维护所有索引
cache_hash_remove(cache, key, key_len);
cache_sorted_remove(cache, key, key_len);
cache_ns_remove(cache, key);
cache_tag_index_remove(cache, entry_offset, value, value_len);  // 不要遗漏
cache_vector_index_remove(cache, entry_offset);                 // 不要遗漏
hot_cache_invalidate(cache, key, key_len);                      // 不要遗漏
```

**实践**: 每次删除 entry 时，写一个 "索引维护清单"，确保 hash、sorted、namespace、tag、vector、hot_cache 全部更新。新增索引时必须同步更新删除路径。

### 11.3 先创建主体再分配辅助数据

**问题**: `cache_set_vector()` 先在 pool 中分配向量数据，再调用 `cache_set()` 创建 entry。如果 `cache_set()` 失败（内存不足），向量数据已写入 pool 但无任何 entry 引用，成为永久孤儿数据。

**修复**:
```c
// ❌ 错误：先分配向量，再创建 entry → 孤儿数据
size_t vector_offset = alloc_vector_data(cache, vector, dim);  // pool 已分配
int ret = cache_set(cache, key, value, ttl_ms);  // 失败 → 向量孤儿

// ✅ 正确：先创建 entry，成功后再分配向量
int ret = cache_set(cache, key, value, ttl_ms);  // 先确保 entry 创建成功
if (ret != CACHE_OK) return ret;
size_t vector_offset = alloc_vector_data(cache, vector, dim);  // 再分配向量
```

**实践**: 遵循"主体优先"原则——先创建可被引用的主体对象，再创建依赖于主体的辅助数据。

### 11.4 循环条件避免双重计算

**问题**: LRU 淘汰循环中，`memory_used` 已在每次淘汰后递减，但循环条件仍使用 `memory_max - evicted`，导致 `evicted` 被重复计算，循环可能提前结束或过度淘汰。

**修复**:
```c
// ❌ 错误：evicted 被重复计算（memory_used 已递减 + memory_max - evicted）
for (size_t i = 0; i < candidate_count && 
     cache->memory_used + aligned_size > cache->memory_max - evicted; i++) {
    // memory_used 在这里 -= c->total_size
    // evicted 又在这里 += c->total_size
    // 双重计算！
}

// ✅ 正确：memory_used 已实时更新，无需再减去 evicted
for (size_t i = 0; i < candidate_count && 
     cache->memory_used + aligned_size > cache->memory_max; i++) {
    cache->memory_used -= c->total_size;  // 直接递减即可
}
```

**实践**: 循环条件中不要维护一个"已处理总量"变量，除非原始值不会被修改。优先使用实时更新的状态值作为判断依据。

### 11.5 边界检查：malloc(0) 和空 key

**问题**: 
- `malloc(sizeof(x) * 0)` 的返回值是未定义行为（C 标准允许返回 NULL 或有效指针）
- `cache_set_ns()` 未检查空 key（`key_len == 0`），导致分配了 `ns_len + 2` 字节但实际写入空字符串

**修复**:
```c
// ❌ 错误：未检查 entry_count == 0
evict_candidate_t* candidates = malloc(sizeof(evict_candidate_t) * cache->entry_count);
// entry_count == 0 时，malloc(0) 行为未定义

// ✅ 正确：前置检查
if (cache->entry_count == 0) return CACHE_ERR_NOMEM;
evict_candidate_t* candidates = malloc(sizeof(evict_candidate_t) * cache->entry_count);

// ❌ 错误：未检查空 key
size_t key_len = strlen(key);
char* full_key = malloc(ns_len + 1 + key_len + 1);

// ✅ 正确：前置验证
if (key_len == 0) return CACHE_ERR_INVAL;
```

**实践**: 
- `malloc(N * count)` 前检查 `count > 0`
- 所有字符串参数使用前检查 `strlen > 0`
- 文件加载时的尺寸字段也要验证合理范围

### 11.6 扫描时跳过非目标数据用 continue，不用 break

**问题**: `cache_purge_expired()` 遇到 `key_len == 0` 时使用 `break`，但 pool 中 hash bucket 数组的初始内存也可能为 0，导致扫描提前中断，遗漏后面的过期条目。

**修复**:
```c
// ❌ 错误：遇到 0 就 break，可能提前中断
while (offset < pool.used) {
    cache_entry_header_t* h = CACHE_PTR(cache, offset);
    if (h->key_len == 0) break;  // hash bucket 也可能是 0！
    // 处理 entry ...
}

// ✅ 正确：用 continue + 固定步进跳过非 entry 数据
while (offset + sizeof(cache_entry_header_t) <= pool.used) {
    cache_entry_header_t* h = CACHE_PTR(cache, offset);
    if (h->key_len == 0 || h->key_len > MAX_KEY_LEN || h->value_len > MAX_VALUE_LEN) {
        offset += MYDB_ALIGN;  // 跳过对齐大小（可能是 hash bucket）
        continue;
    }
    // 处理 entry ...
    offset += cache_entry_total_size(h);
}
```

**实践**: 扫描持久化数据时，始终使用 `continue` 跳过非目标数据，只在越界时使用 `break`。同时验证 `offset + header_size <= used` 防止越界读取。

### 11.7 避免不必要的字符串拷贝

**问题**: `cache_search_regex()` 对每个 entry 都 `malloc(key_len + 1)` 复制 key，但实际上 key 在 pool 中已以 `\0` 结尾，可以直接传给 `regexec()`。

**修复**:
```c
// ❌ 错误：不必要的 malloc/free
char* key_copy = malloc(key_len + 1);
memcpy(key_copy, key, key_len);
key_copy[key_len] = '\0';
int match = (regexec(&regex, key_copy, ...) == 0);
free(key_copy);

// ✅ 正确：pool 中的 key 已 null-terminated，直接使用
int match = (regexec(&regex, key, ...) == 0);
```

**实践**: 当数据存储在 pool/mmap 中时，确认 layout 中已包含 null-terminator，避免无谓的拷贝。写入时确保 `key_ptr[key_len] = '\0'`。

### 11.8 批量操作必须更新所有索引

**问题**: `cache_batch_set()` 优化性能时，添加了 hash、sorted、namespace 索引更新，但遗漏了 tag_index。导致批量插入的条目无法通过 tag 搜索找到。

**修复**:
```c
// ❌ 错误：batch set 遗漏 tag 索引
for (size_t i = 0; i < count; i++) {
    cache_hash_insert(cache, offsets[i], key, key_len);
    cache_ns_add(cache, key, offsets[i]);
}

// ✅ 正确：batch set 更新所有索引
for (size_t i = 0; i < count; i++) {
    cache_hash_insert(cache, offsets[i], key, key_len);
    cache_ns_add(cache, key, offsets[i]);
    cache_tag_index_add(cache, offsets[i], value, value_len);  // 不要遗漏
}
```

**实践**: 批量操作优化时，逐一检查所有索引类型。新增索引时，同步更新 batch、single-insert、recovery 三条路径。

### 11.9 重启时必须重建所有内存索引

**问题**: `cache_open()` 恢复时扫描所有 entry 重建 hash、sorted、namespace 索引，但遗漏了 tag_index。重启后 tag 搜索退化为 O(n) 全表扫描。

**修复**:
```c
// ❌ 错误：恢复时遗漏 tag 索引
if (!(header->flags & CACHE_ENTRY_DELETED)) {
    cache_hash_insert(cache, offset, key, header->key_len);
    cache_sorted_insert(cache, offset);
    cache_ns_add(cache, key, offset);
    // 遗漏 cache_tag_index_add
}

// ✅ 正确：恢复时重建所有内存索引
if (!(header->flags & CACHE_ENTRY_DELETED)) {
    const char* value = key + header->key_len + 1;
    cache_hash_insert(cache, offset, key, header->key_len);
    cache_sorted_insert(cache, offset);
    cache_ns_add(cache, key, offset);
    cache_tag_index_add(cache, offset, value, header->value_len);  // 不要遗漏
}
```

**实践**: 所有"内存中构建、不持久化"的索引（sorted array、tag index、vector index、hot cache、skip list），必须在 `cache_open` 的恢复扫描中重建。新增此类索引时，同步更新恢复逻辑。

### 11.10 跳表/排序数组的一致性

**问题**: `cache_sorted_remove()` 找到条目并从 sorted array 删除后，跳表删除代码放在了"未找到"分支中（死代码），导致跳表永远得不到更新，积累大量已删除节点。

**修复**:
```c
// ❌ 错误：skiplist_remove 在未找到分支中（死代码）
if (found) {
    sorted->offsets[mid] = sorted->offsets[count - 1];
    sorted->count--;
    return 0;  // 找到并删除，但没有更新跳表
}
if (sorted->skiplist) {
    cache_skiplist_remove(sorted->skiplist, key, key_len);  // 死代码
}

// ✅ 正确：找到并删除后立即更新跳表
if (found) {
    sorted->offsets[mid] = sorted->offsets[count - 1];
    sorted->count--;
    if (sorted->skiplist) {
        cache_skiplist_remove(sorted->skiplist, key, key_len);  // 正确位置
    }
    return 0;
}
```

**实践**: 复合索引结构（sorted array + skip list）中，每种索引的删除操作必须在同一代码路径中处理。删除前先确认该路径确实会被执行到。

---

## 总结

### 最重要的 5 条原则

1. **Offset 优于指针** - mmap 场景下永远使用 `size_t offset`，配合 `POOL_PTR()` 访问
2. **完整释放链** - 每个 `alloc` 都有对应的 `free`，集中在一个释放函数中
3. **批量优于单条** - WAL 不 fsync、JSON 用动态 buffer、提供 batch API
4. **版本控制文件格式** - magic + version + used 的文件头，加载时严格验证
5. **测试覆盖边界** - 空表、越界、重复、泄漏、性能，每个场景都有断言

### KV Cache 审计追加原则

6. **栈分配 > 堆分配** - 小于 64 bytes 的临时结构体全部用栈，避免泄漏
7. **删除维护清单** - 每次删除 entry 时，逐一检查 hash/sorted/ns/tag/vector/hot 六个索引
8. **主体优先** - 先创建可引用的主体对象，再分配依赖于主体的辅助数据
9. **继续而非中断** - 扫描 pool 时遇到非目标数据用 `continue`，仅在越界时 `break`
10. **批量一致** - batch API 必须更新所有索引，新增索引时同步更新 batch/recovery/single-insert 三条路径
11. **重启重建** - 所有"内存中构建"的索引必须在 `cache_open` 恢复时重建
12. **条件正确性** - 循环条件中避免维护"已处理总量"，使用实时更新的状态值

---

*文档生成时间: 2026-05-17*
*基于提交: 2a354bb .. 4294ebf (含三轮 KV Cache 审计)*
