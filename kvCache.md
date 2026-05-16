# my_db KV Cache - AI Agent 体系化记忆存储

> **定位**: 专为 AI Agent 设计的层级化记忆存储系统  
> **核心**: 纯 KV + Namespace 体系 + 多维搜索  
> **哲学**: 只存精华，大文件存路径引用

---

## 1. 设计哲学

### 为什么不是传统数据库？

传统数据库（MySQL/PostgreSQL）是为**结构化业务数据**设计的：
- 严格的 Schema，需要预定义字段
- 复杂的 JOIN 和事务
- 查询优化器开销大
- 不适合灵活的记忆组织

### 为什么不是 Redis？

Redis 是为**高性能缓存**设计的：
- 纯内存，重启丢失（除非 RDB/AOF）
- 没有层级命名空间
- 不支持模糊/正则搜索
- 不适合长期知识积累

### my_db KV Cache 的独特之处

1. **层级命名空间**: `/coding/cpp/move-semantics` 像文件系统一样组织知识
2. **磁盘持久化**: mmap 零拷贝，重启不丢失，自动加载
3. **多维搜索**: 前缀搜索 + 范围搜索 + 正则搜索 + 标签搜索
4. **大文件引用**: 精华存 KV，原始资料存文件路径，自动关联
5. **TTL 生命周期**: 支持记忆过期（工作记忆 → 短期记忆 → 长期记忆）

---

## 2. 架构设计

### 2.1 存储层（复用现有零拷贝架构）

```
cache.bin (mmap pool)
├── [文件头: 16 bytes]
│   ├── magic: "MYCA" (4 bytes)
│   ├── version: 1 (4 bytes)
│   └── used: 8 bytes
│
└── [Entries...] 变长存储，pool_alloc 分配
    ├── key_len: 4 bytes
    ├── key: N bytes (UTF-8，含 / 分隔符)
    ├── value_len: 4 bytes
    ├── value: N bytes (JSON 元数据)
    ├── expire_at: 8 bytes (毫秒时间戳，0=永久)
    └── access_time: 8 bytes (毫秒时间戳，用于 LRU)
```

**特点**:
- 复用 `db_pool_t` + `pool_init/alloc/sync/close`
- 零拷贝：entry 通过 offset 访问，mmap 自动映射到文件
- `cache_sync()` 调用 `msync(MS_SYNC)` 强制落盘

### 2.2 双索引架构

**Hash 索引**: key_hash → entry_offset
```
O(1) get/set/del/exists
冲突处理：链地址法
自动扩容：负载因子 > 0.75 时 bucket 翻倍
```

**排序数组**: 所有 key_offset 按字典序排列
```
pool_alloc 分配连续数组
二分查找：O(log n) 定位
范围扫描：O(k) 返回 k 个结果
前缀搜索：O(log n + k)
正则搜索：O(log n + k) 定位起点 + 遍历匹配
```

### 2.3 Namespace 设计

**逻辑层级**（非物理文件夹）:
```
/coding                    ← 一级：编程知识
/coding/cpp                ← 二级：C++ 语言
/coding/cpp/move-semantics ← 三级：具体知识点
/coding/python             ← 二级：Python 语言
/coding/python/async       ← 三级：异步编程

/agent                     ← 一级：Agent 配置
/agent/personality         ← 二级：性格设定
/agent/session/20240115    ← 二级：对话历史

/knowledge                 ← 一级：通用知识
/knowledge/architecture    ← 二级：架构设计
/knowledge/algorithm       ← 二级：算法
```

**句柄 API**:
```c
cache_ns_t ns = cache_ns(cache, "/coding/cpp");
cache_set_ns(ns, "templates", "模板元编程精华...", TTL);
// 实际存储 key = "/coding/cpp/templates"
```

---

## 3. 数据格式

### 3.1 Value 格式（JSON）

```json
{
    "t": "concept",              // type: summary|concept|pattern|conversation|reference
    "c": "右值引用实现完美转发...", // content: 精华摘要（纯文本）
    "s": "path/to/file",         // source: 外部文件路径（相对 db_dir）
    "p": "page:15,line:20",      // position: 具体位置引用
    "i": 5,                      // importance: 1-5 重要性评分
    "tags": ["cpp11", "move"]    // tags: 额外标签，用于交叉搜索
}
```

### 3.2 三种内容类型

**Type A: 内联文本**（小内容，直接存储）
```c
cache_set(cache, "/agent/personality", "友好、专业、简洁", 0);
```

**Type B: 外部引用**（大文件，存路径）
```c
// 先保存 PDF 到文件系统
save_file("/data/knowledge/cpp/move-semantics.pdf", pdf_data);

// KV 中存引用
cache_set_json(cache, "/coding/cpp/move-semantics",
    "{\"t\":\"concept\",\"c\":\"右值引用和移动语义...\",\"s\":\"knowledge/cpp/move-semantics.pdf\",\"p\":\"page:15\",\"i\":5}",
    0);
```

**Type C: 混合模式**（精华 + 引用）
```c
cache_set_json(cache, "/coding/design-patterns/singleton",
    "{\"t\":\"pattern\",\"c\":\"单例模式确保全局唯一实例...\",\"s\":\"repos/design-patterns/src/singleton.cpp\",\"p\":\"line:42-80\",\"i\":4,\"tags\":\"[\"creational\",\"thread-safe\"]\"}",
    0);
```

### 3.3 路径规范

- **分隔符**: `/`（类 Unix 路径）
- **命名规范**: 小写字母 + 数字 + 连字符，不用空格
- **最大深度**: 建议不超过 10 层
- **最大长度**: key 总长度 <= 1024 bytes
- **保留路径**: 以 `_` 开头的为系统保留（如 `_meta`）

---

## 4. API 设计

### 4.1 基础操作

```c
// 生命周期
cache_t cache_open(const char* db_dir, size_t max_memory);
void cache_close(cache_t cache);
int cache_sync(cache_t cache);

// 基础 CRUD
int cache_set(cache_t cache, const char* key, const char* value, uint64_t ttl_ms);
const char* cache_get(cache_t cache, const char* key);  // 返回指针，不拷贝
int cache_del(cache_t cache, const char* key);
int cache_exists(cache_t cache, const char* key);

// JSON 便捷操作
int cache_set_json(cache_t cache, const char* key, const char* json, uint64_t ttl_ms);
const char* cache_get_json(cache_t cache, const char* key);
```

### 4.2 Namespace 操作

```c
// 创建命名空间句柄（相对路径）
cache_ns_t cache_ns(cache_t cache, const char* prefix);
cache_ns_t cache_ns_child(cache_ns_t parent, const char* name);

// 在命名空间内操作
int cache_set_ns(cache_ns_t ns, const char* key, const char* value, uint64_t ttl_ms);
const char* cache_get_ns(cache_ns_t ns, const char* key);
int cache_del_ns(cache_ns_t ns, const char* key);

// 获取当前 namespace 的完整路径
const char* cache_ns_path(cache_ns_t ns);
```

### 4.3 搜索操作

```c
// 前缀搜索：返回匹配的所有 key
cache_result_t* cache_prefix(cache_t cache, const char* prefix, size_t* count);

// 范围搜索：[start_key, end_key)
cache_result_t* cache_range(cache_t cache, const char* start, const char* end, size_t* count);

// 正则搜索（POSIX 扩展正则）
cache_result_t* cache_regex(cache_t cache, const char* pattern, size_t* count);

// 标签搜索：搜索包含指定标签的所有 entry
cache_result_t* cache_tag(cache_t cache, const char* tag, size_t* count);

// 结果结构
typedef struct {
    char* key;
    char* value;     // JSON 格式
    uint64_t access_time;
} cache_result_t;

void cache_result_free(cache_result_t* results, size_t count);
```

### 4.4 管理操作

```c
// 统计信息
size_t cache_count(cache_t cache);                    // 总条目数
size_t cache_memory_used(cache_t cache);              // 已用内存
size_t cache_memory_max(cache_t cache);               // 最大内存

// 生命周期管理
int cache_expire(cache_t cache, const char* key);     // 立即过期
int cache_touch(cache_t cache, const char* key);      // 更新 access_time

// 清理
size_t cache_compact(cache_t cache);                  // 物理清理过期/删除的条目
size_t cache_purge_expired(cache_t cache);            // 清理所有过期条目

// 诊断
int cache_check(const char* db_dir);                  // 检查 cache 文件完整性
```

---

## 5. 使用示例

### 5.1 Coding 知识库

```c
#include "cache.h"

int main() {
    cache_t cache = cache_open("coding-knowledge", 100 * 1024 * 1024);  // 100MB
    
    // 1. 存储 C++ 知识点
    cache_set_json(cache, "/coding/cpp/move-semantics",
        "{\"t\":\"concept\",\"c\":\"右值引用(T&&)实现完美转发...\",\"i\":5,\"tags\":[\"cpp11\",\"performance\"]}",
        0);  // TTL=0 表示永久
    
    cache_set_json(cache, "/coding/cpp/templates/sfinae",
        "{\"t\":\"concept\",\"c\":\"SFINAE: Substitution Failure Is Not An Error...\",\"i\":4,\"tags\":[\"cpp03\",\"metaprogramming\"]}",
        0);
    
    cache_set_json(cache, "/coding/python/async/await",
        "{\"t\":\"pattern\",\"c\":\"asyncio.gather 并发执行多个协程...\",\"i\":4,\"tags\":[\"python3.5\",\"async\"]}",
        0);
    
    // 2. 前缀搜索：获取所有 C++ 知识点
    size_t count;
    cache_result_t* results = cache_prefix(cache, "/coding/cpp/", &count);
    printf("C++ 知识点: %zu 条\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("  %s: %s\n", results[i].key, results[i].value);
    }
    cache_result_free(results, count);
    
    // 3. 正则搜索：找所有包含 "async" 的知识点
    results = cache_regex(cache, ".*async.*", &count);
    printf("Async 相关: %zu 条\n", count);
    cache_result_free(results, count);
    
    // 4. 标签搜索：找所有 "performance" 相关的
    results = cache_tag(cache, "performance", &count);
    printf("Performance 相关: %zu 条\n", count);
    cache_result_free(results, count);
    
    // 5. 使用 namespace 句柄
    cache_ns_t cpp_ns = cache_ns(cache, "/coding/cpp");
    cache_set_ns(cpp_ns, "rvalue-reference", "右值引用详解...", 0);
    // 实际存储: /coding/cpp/rvalue-reference
    
    cache_sync(cache);
    cache_close(cache);
    return 0;
}
```

### 5.2 Agent 记忆管理

```c
// Agent 性格设定（永久）
cache_set_json(cache, "/agent/personality",
    "{\"t\":\"config\",\"c\":\"友好、专业、简洁，使用中文回答\",\"i\":5}", 0);

// 工作记忆（5 分钟 TTL）
cache_set_json(cache, "/agent/working/current-task",
    "{\"t\":\"task\",\"c\":\"正在分析用户提供的 C++ 代码...\",\"i\":3}",
    5 * 60 * 1000);  // 5 分钟

// 短期记忆（1 小时 TTL）
cache_set_json(cache, "/agent/session/20240115-001",
    "{\"t\":\"conversation\",\"c\":\"用户询问 C++ 内存模型，已解释内存顺序...\",\"i\":4}",
    60 * 60 * 1000);  // 1 小时

// 长期知识（永久）
cache_set_json(cache, "/knowledge/cpp/memory-model",
    "{\"t\":\"concept\",\"c\":\"C++11 内存模型：sequenced-before, happens-before...\",\"s\":\"docs/cpp-memory-model.pdf\",\"i\":5}",
    0);
```

### 5.3 大文件引用

```c
// 保存 PDF 到文件系统
FILE* f = fopen("coding-knowledge/docs/design-pattern.pdf", "wb");
fwrite(pdf_data, 1, pdf_size, f);
fclose(f);

// KV 中存引用 + 精华摘要
cache_set_json(cache, "/coding/design-patterns/factory",
    "{\"t\":\"pattern\",\"c\":\"工厂模式：将对象创建逻辑封装...\",\"s\":\"docs/design-pattern.pdf\",\"p\":\"page:42-50\",\"i\":5,\"tags\":[\"creational\"]}",
    0);

// 读取时，先获取 KV，再根据 source 打开文件
const char* json = cache_get(cache, "/coding/design-patterns/factory");
// 解析 JSON，获取 "s" 字段，然后 fopen 读取 PDF
```

---

## 6. 生命周期管理

### 6.1 TTL 策略

```c
// 三级记忆系统
#define TTL_WORKING   (5 * 60 * 1000)     // 5 分钟：工作记忆
#define TTL_SHORT     (60 * 60 * 1000)    // 1 小时：短期记忆
#define TTL_LONG      (24 * 60 * 60 * 1000) // 1 天：中期记忆
#define TTL_PERMANENT 0                    // 0：永久记忆

// 使用场景
cache_set(cache, key, value, TTL_WORKING);   // 临时上下文
cache_set(cache, key, value, TTL_SHORT);     // 会话记忆
cache_set(cache, key, value, TTL_LONG);      // 日内记忆
cache_set(cache, key, value, TTL_PERMANENT); // 知识积累
```

### 6.2 LRU 淘汰

当内存使用超过 `max_memory`:
1. 按 `access_time` 排序（最老的在前）
2. 跳过 `TTL_PERMANENT`（永久记忆不淘汰）
3. 淘汰最老的非永久条目，直到内存 < 80% max
4. 如果还是不够，报错（不淘汰永久记忆）

### 6.3 惰性过期

```c
const char* cache_get(cache_t cache, const char* key) {
    entry_t* entry = hash_lookup(...);
    if (entry && entry->expire_at > 0 && entry->expire_at < now()) {
        // 惰性删除：返回 NULL，标记删除
        entry->flags |= DELETED;
        return NULL;
    }
    entry->access_time = now();  // 更新 LRU
    return entry->value;
}
```

---

## 7. 性能指标（目标）

| 操作 | 复杂度 | 目标性能 |
|------|--------|----------|
| get/set/del | O(1) | > 1M ops/sec |
| prefix search | O(log n + k) | > 100K ops/sec |
| range search | O(log n + k) | > 100K ops/sec |
| regex search | O(n) | > 10K ops/sec |
| memory per entry | - | ~key_len + value_len + 48 bytes |
| max capacity | - | 受 max_memory 限制（默认 100MB） |

---

## 8. 与 my_db 数据库的关系

```
my_db 项目
├── 数据库层（已完成）
│   ├── 结构化数据存储（表、行、Schema）
│   ├── 复杂查询（WHERE/JOIN/ORDER BY）
│   ├── WAL + 事务
│   └── 适用：业务数据、用户数据
│
└── KV Cache 层（新增）
    ├── 非结构化知识存储（key-value）
    ├── 层级命名空间
    ├── 多维搜索（前缀/范围/正则）
    ├── TTL + LRU
    └── 适用：Agent 记忆、知识库、配置缓存
```

**两者可以共存**:
```c
db_t db = db_open("business-data", ...);      // 业务数据库
cache_t cache = cache_open("agent-memory", ...); // Agent 记忆

// 用户数据存数据库
db_insert(users_table, &user, sizeof(user));

// Agent 记忆存 Cache
cache_set_json(cache, "/agent/session/001", "{...}", TTL_SHORT);
```

---

## 9. 搜索索引设计（核心讨论）

> **为什么搜索如此重要？**  
> AI Agent 的上下文窗口有限（4K-128K tokens），不能把全部知识塞进 prompt。  
> Agent 开发大项目时，需要**精准召回**相关知识片段，组装成上下文注入。  
> 搜索的便利性、准确性、速度，直接决定开发效果。

### 9.1 场景分析

**Agent 记忆特点**:
- **数据量**: 几千到几万条（精华知识，不会无限增长）
- **写入频率**: 低（知识积累是渐进的，不是高并发写入）
- **查询频率**: 高（每次推理都可能需要搜索上下文）
- **查询模式**: 前缀搜索（80%）> 正则搜索（15%）> 范围搜索（5%）

**典型搜索场景**:
```
1. "我在写 C++，给我相关知识点" 
   → cache_prefix(cache, "/coding/cpp/")
   
2. "找所有关于 'async' 的记忆"
   → cache_regex(cache, ".*async.*")
   
3. "找 C++ 到 Python 之间的知识点"
   → cache_range(cache, "/coding/cpp/", "/coding/python/")
   
4. "找所有 'performance' 标签的"
   → cache_tag(cache, "performance")
```

### 9.2 索引方案对比

| 方案 | 插入 | 前缀搜索 | 范围搜索 | 正则搜索 | 内存开销 | 实现复杂度 | 适合场景 |
|------|------|----------|----------|----------|----------|------------|----------|
| **排序数组** | O(n) | O(log n + k) | O(log n + k) | O(n) | 低（8 bytes/key） | 低 | **⭐ 推荐** |
| 跳表 | O(log n) | O(log n + k) | O(log n + k) | O(n) | 中（~16 bytes/key） | 中 | 备选 |
| B+树 | O(log n) | O(log n + k) | O(log n + k) | O(n) | 高（节点 overhead） | 高 | 已有，但太重 |
| Trie树 | O(key_len) | O(key_len) | ❌ 不支持 | ❌ 不支持 | 高（每个字符一个节点） | 中 | 仅前缀搜索最优 |
| 倒排索引 | O(1) | ❌ 不支持 | ❌ 不支持 | ❌ 不支持 | 高（词表 +  posting list） | 高 | 全文搜索 |

**结论**: 选择 **排序数组**（Sorted Array）

**理由**:
1. **数据量小**: Agent 记忆通常 < 10万条，O(n) 插入完全可以接受（甚至 < 1ms）
2. **实现简单**: 几百行代码，零外部依赖，符合项目哲学
3. **内存友好**: 只存 key_offset（8 bytes），不存完整 key
4. **前缀搜索天然支持**: 二分找前缀起点，向后遍历即可
5. **范围搜索天然支持**: 二分找起止点
6. **正则搜索可接受**: 遍历范围内 key，用 POSIX regexec 匹配

### 9.3 排序数组实现细节

```c
// 排序数组：存储所有 key 的 offset，按 key 的字典序排列
typedef struct {
    size_t* offsets;    // key_offset 数组（指向 pool 中的 key）
    size_t count;       // 当前数量
    size_t capacity;    // 数组容量
} sorted_keys_t;

// 插入：
// 1. 找到插入位置（二分查找）
// 2. 后面的元素后移一位
// 3. 插入新 offset
// 复杂度：O(n)，但 n < 10万，实际 < 1ms

// 前缀搜索：
// 1. 二分查找 prefix 的 lower_bound
// 2. 从该位置向后遍历，直到 key 不再以 prefix 开头
// 复杂度：O(log n + k)

// 范围搜索：
// 1. 二分查找 start_key 的 lower_bound
// 2. 二分查找 end_key 的 upper_bound
// 3. 返回区间内的所有 key
// 复杂度：O(log n + k)

// 正则搜索：
// 1. 遍历排序数组（或先 prefix 缩小范围）
// 2. 对每个 key 调用 regcomp/regexec
// 3. 复杂度：O(n)，但通常先用 prefix 缩小范围
```

### 9.4 为什么不选跳表？

跳表是优秀的替代品，但我们**不选**的原因：

1. **额外指针开销**: 每个节点需要 4-8 个 level 指针，内存翻倍
2. **实现复杂度**: 需要维护 level 概率、forward/backward 指针
3. **收益有限**: 在 n < 10万 时，O(log n) 和 O(n) 插入差距 < 1ms，不值得增加复杂度
4. **已有排序数组模式**: 项目其他部分（如 query.c 的 WHERE 扫描）也是线性遍历，风格一致

**如果未来数据量 > 100万，再考虑升级为跳表**。

### 9.5 为什么不选 Trie？

Trie 树前缀搜索是 O(key_len)，理论上最优。但：

1. **内存爆炸**: 每个字符一个节点，中文 key 更可怕
2. **不支持范围搜索**: 只能前缀匹配
3. **不支持正则**: 需要回退到遍历
4. **实现复杂**: 需要节点池、子节点指针数组（或 Hash）

**Agent 记忆的 key 是路径形式**（如 `/coding/cpp/move-semantics`），前缀搜索通常只到 `/coding/cpp/` 级别，排序数组完全够用。

### 9.6 标签搜索实现

标签搜索是**反向索引**:
```c
// 每个 entry 的 value JSON 中有 "tags": ["cpp11", "move"]
// 搜索 "performance" 时：
// 1. 遍历所有 entry（或排序数组）
// 2. 解析 JSON，提取 tags 数组
// 3. 匹配 tag
// 
// 优化：如果 tag 搜索频繁，可维护 tag → [key_offsets] 的 Hash 索引
// 但 Phase 1 先不实现，遍历即可（n < 10万）
```

### 9.7 搜索性能预期

| 数据量 | 前缀搜索 | 范围搜索 | 正则搜索 | 标签搜索 |
|--------|----------|----------|----------|----------|
| 1,000 条 | < 0.01ms | < 0.01ms | < 0.1ms | < 0.1ms |
| 10,000 条 | < 0.05ms | < 0.05ms | < 1ms | < 1ms |
| 100,000 条 | < 0.5ms | < 0.5ms | < 10ms | < 10ms |

**完全满足 AI Agent 的实时搜索需求**。

---

## 10. 未来扩展

## 11. 电子书导入设计

### 11.1 复用 WordCard 解析能力

WordCard 项目（`~/WordCard`）已集成 C++ 电子书解析库：
- `libmobiparse.so` - MOBI/AZW3 解析（基于 libmobi）
- `libpdfparse.so` - PDF 解析（基于 MuPDF）

**复用策略**：
```
KV Cache 导入工具
├── 复用 WordCard 的 C++ 解析库（通过 ctypes）
│   ├── libmobiparse.so → 提取 MOBI/AZW3 纯文本
│   └── libpdfparse.so  → 提取 PDF 纯文本 + 页码
│
├── 复用分章逻辑（import_book.py 的 split_into_chapters）
│   ├── PDF：按页码分隔符分章
│   ├── MOBI：按 "Chapter X" 标题分章
│   └──  fallback：固定长度分块
│
└── 不复用词汇提取（KV Cache 不需要背单词）
```

### 11.2 导入流程

```python
# tools/import_book.py

def import_book_to_cache(cache_dir: str, book_path: str, namespace: str):
    """将电子书导入 KV Cache"""
    
    # 1. 解析电子书
    text = parse_book(book_path)  # 复用 WordCard 解析器
    
    # 2. 智能分章
    chapters = split_into_chapters(text)  # 复用 WordCard 分章逻辑
    
    # 3. 逐章处理
    for ch in chapters:
        # 3.1 提取关键段落（信息量最大的段落）
        key_paragraphs = extract_key_paragraphs(ch.text, max_chars=2000)
        
        # 3.2 调用 LLM 生成精华摘要
        summary = call_llm(f"总结以下技术文档的核心要点（200字内）：\n{key_paragraphs}")
        
        # 3.3 生成 tags（LLM 自动提取）
        tags = call_llm(f"从以下文本提取 3-5 个关键词标签（逗号分隔）：\n{summary}")
        
        # 3.4 构建 KV entry
        key = f"{namespace}/chapter-{ch.number}"
        value = json.dumps({
            "t": "book-chapter",
            "c": summary,
            "s": book_path,           # 源文件路径
            "p": f"page:{ch.start_page}",
            "i": 4,
            "tags": tags.split(","),
            "full_text_hash": hash(ch.text)  # 用于去重校验
        })
        
        # 3.5 存入 KV Cache（通过 ctypes 调用 libmydb.so）
        cache_set(cache, key, value, TTL_PERMANENT)
    
    # 4. 生成目录索引
    toc = generate_toc(chapters)
    cache_set(cache, f"{namespace}/_meta/toc", toc, TTL_PERMANENT)
    
    cache_sync(cache)
```

### 11.3 使用示例

```bash
# 导入技术书籍
python tools/import_book.py \
  --book "Effective_CPP.pdf" \
  --cache-dir "./agent-memory" \
  --namespace "/books/effective-cpp" \
  --llm-url "http://localhost:11434" \
  --chunk-size 2000

# 导入后搜索
python -c "
from mydb import Cache
cache = Cache.open('agent-memory')
results = cache.prefix('/books/effective-cpp/')
for r in results:
    print(f'{r.key}: {r.value}')
"
```

### 11.4 与 WordCard 的关系

| | WordCard | KV Cache 导入工具 |
|--|----------|-------------------|
| **目标** | 背单词、学英语 | Agent 知识库 |
| **存储内容** | 单词卡、理解题 | 精华摘要、技术要点 |
| **解析库** | 复用 `libmobiparse.so` + `libpdfparse.so` | **复用相同的库** |
| **分章逻辑** | `split_into_chapters()` | **复用相同的函数** |
| **LLM 用途** | 翻译、出题 | **生成摘要、提取 tags** |
| **存储后端** | SQLite | **KV Cache (mmap)** |

**不重复造轮子**：解析和分章直接复用 WordCard 的成熟代码。

---

## 12. 未来扩展

### Phase 2
- [ ] 向量搜索：集成 HNSW，支持语义相似度搜索（"找和 move semantics 相关的知识点"）
- [ ] 标签索引：维护 tag → [offsets] 反向索引，加速标签搜索
- [ ] 跳表升级：数据量 > 100万时，排序数组升级为跳表

### Phase 3
- [ ] 分布式：多 Agent 共享知识库
- [ ] 快照：定期快照备份

---

*文档版本: 1.1*  
*更新日期: 2026-05-16*  
*状态: 搜索设计完成，索引方案确定（排序数组）*
