# my_db

基于 C 语言数据结构的**单机嵌入式零拷贝存储引擎**

无网络、无端口、无协议，直接嵌入应用进程。完整的程序化 CRUD + JOIN 查询，零拷贝持久化。

**🚀 KV Cache 系统（Agent 记忆存储）**: [kvCache.md](kvCache.md) - 专为 AI Agent 设计的层级化记忆存储，支持前缀/范围/正则搜索

**🎯 AI Agent 代码语义搜索**: `./ai_code_search.sh` - 一键对任意代码库进行语义索引和智能搜索

```bash
# 一键分析代码库（索引 + 向量生成 + 调用图构建）
./ai_code_search.sh analyze /opt/stable-diffusion.cpp ./sd_cache

# 自然语言搜索代码
./ai_code_search.sh search ./sd_cache "VAE encoder decoder" 10

# 用代码片段搜索相似实现
./ai_code_search.sh snippet ./sd_cache ./my_kernel.cpp 5
```

技术栈：C 多进程索引 + Jina v2 代码语义模型 + TensorRT GPU 向量生成 + 调用关系图分析

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
- **多文件目录架构**：每表独立文件（.bin + .index + .idxmeta），目录即数据库
- **零拷贝索引持久化**：Hash 索引直接 mmap，重启后自动恢复，无需重建
- **Hash Join 优化**：自动检测右表索引，Hash Join + 嵌套循环 fallback
- **Free List 复用**：删除的 rowid 自动复用，删除率 ≥ 30% 自动 compact
- **变长字符串**：DB_TYPE_VARSTRING 支持任意长度文本，独立 strings 池
- **WAL 日志**：同步写入保证不丢失，异步刷盘保证性能
- **JSON 查询结果**：SELECT 自动返回 JSON，无需 FFI 侧定义 struct
- **流式查询**：逐行回调，无内存上限，适合大数据量
- **内存回收**：`db_table_compact()` / `db_compact()` 回收已删除空间
- **无锁设计**：类似 Redis，单线程/用户自行保证并发，简单高效
- **极致性能**：单线程 50万+ 行/秒插入速度
- **KV Cache**：Agent 记忆存储，支持 5 种搜索 + TTL/LRU
- **Tag 索引**：Hash-based 反向索引，O(1) 标签搜索
- **Skip List**：20 级跳表，数据量 > 100 万自动启用
- **向量搜索**：Float embedding + 余弦相似度，语义搜索（ONNX Runtime C 推理）
- **Jina v2 C 推理**：纯 C 推理 + tokenizers-cpp（HuggingFace 官方 tokenizer），代码语义检索专用
- **源码分析**：7 种语言 AST 提取（函数/类/结构体/导入）
- **TCP 远程**：文本协议服务器，支持端口远程操作

## KV Cache（AI Agent 记忆存储）

专为 AI Agent 设计的**层级化记忆存储子系统**，支持 Namespace 组织、多维搜索和 TTL/LRU 管理。

### 核心特性

- **Namespace 层级**：`/` 分隔的路径式 key，如 `/coding/cpp/move-semantics`
- **5 种搜索**：前缀、范围、正则、模糊（Levenshtein）、标签
- **Tag 反向索引**：Hash-based `tag → offsets`，O(1) 标签查找
- **Skip List 跳表**：20 级跳表，数据量 > 100 万时自动启用 O(log n) 搜索
- **向量相似度搜索**：Float embedding + 余弦相似度，支持 top-k 过滤
- **HNSW 近似索引**：>1000 条向量自动启用，O(log n) 近似最近邻，召回率 >95%，比暴力搜索快 4x
- **源码语义分析**：7 种语言 AST 提取（函数/类/结构体/导入），自动语义标签
- **TTL + LRU**：自动过期 + 内存不足时批量淘汰最老条目
- **Hot Cache**：64-entry LRU 热点缓存，重复读取加速 2-3x
- **批量插入**：`cache_batch_set()` 10K 条批量写入，2.5x 提速
- **零拷贝持久化**：复用 my_db mmap 架构，内存 = 磁盘
- **Python FFI**：`mydb/cache.py` 提供类 dict 接口
- **TCP 远程操作**：文本协议服务器，支持端口远程访问

### 索引体系与搜索对照

**8 种搜索方式，各解决不同问题：**

| 搜索方式 | 索引结构 | 查询维度 | 适用场景 | 时间复杂度 |
|---------|---------|---------|---------|-----------|
| **Exact** (`get`) | Hot Cache → Hash 表 | Key 完全相等 | 精确查找某条记忆 | O(1) |
| **Prefix** | Sorted Array + Skip List | Key 前缀 | 浏览 namespace，如 `/coding/cpp/*` | O(log n) ~ O(1) |
| **Range** | Sorted Array + Skip List | Key 区间 | 分页、区间扫描 | O(log n) |
| **Regex** | 扫描 Sorted Array | Key 模式匹配 | 复杂 key 过滤，如 `user_\d+` | O(n) |
| **Fuzzy** | 扫描 Sorted Array | Key 近似匹配 | 拼写容错，如 "recieve" → "receive" | O(n) |
| **Tag** | Hash 反向索引 | Value 标签字段 | 分类过滤，如所有带 "async" 标签的条目 | O(1) |
| **Vector** | HNSW 图索引 | Value 语义向量 | 语义相似，如 "并发" 和 "多线程" | O(log n) |
| **AST** | 扫描 Value | 代码符号 | 搜索函数名、类名 | O(n) |

### 索引协作机制

**不同查询走不同索引，形成检索矩阵：**

```
查询意图 → 选择索引 → 执行搜索

Exact Key:     Hot Cache (64-entry LRU) → Hash Index (O(1))
Prefix/Range:  Skip List (if >1M) → Sorted Array lower_bound (O(log n))
Vector:        HNSW (if >1000) → 暴力搜索 (O(log n) / O(n))
Tag:           Tag Hash Index → offset list (O(1))
Namespace:     Namespace Tree → 子树定位 (O(depth))
```

**协作示例**：语义搜索限定某本书
```
用户: "马斯克创办特斯拉" 在《硅谷钢铁侠》中

1. 生成 query_vector (384 维)
2. cache_search_vector(..., ns_filter="/books/硅谷钢铁侠")
   ├─ HNSW 搜索 → 返回 Top-K 候选 (entry_offset)
   ├─ 验证候选 key 是否以 "/books/硅谷钢铁侠" 开头
   └─ 返回过滤后的结果

3. 通过 entry_offset 读取 value
   ├─ key: "/books/硅谷钢铁侠/chapters/07-.../content/p0026"
   └─ value: {"t":"paragraph","c":"2008年，特斯拉推出Roadster..."}
```

**索引互补性：**

| 查询意图 | 首选索引 | 为什么 |
|---------|---------|--------|
| 我知道 exact key | Hash | O(1) 最快 |
| 我想浏览某目录 | Sorted Array | 按键序排列，天然适合前缀 |
| 我记不太清关键词 | HNSW | 理解语义，同义词也能找到 |
| 我要过滤特定标签 | Tag | O(1) 反向查找 |
| 我想看某书某章 | Namespace + Hash | 先定位 namespace，再精确查找 |

### 索引文件结构（`index.bin`）

所有索引保存到**单个文件**，原子性 rename：

```
index.bin (3.5MB for 1261 entries)
├─ File Header (24 bytes)
│   Magic "MYIX"      (4 bytes)
│   Version 1         (4 bytes)
│   Total Size        (8 bytes)
│   Index Count 5     (4 bytes)
│   Reserved          (4 bytes)
│
├─ Index Entry Headers (36 bytes × 5)
│   [Hash]     type=1, offset, size, entry_count
│   [Sorted]   type=2, offset, size, entry_count
│   [Vector]   type=3, offset, size, entry_count
│   [HNSW]     type=5, offset, size, entry_count
│   [Namespace] type=6, offset, size, entry_count
│
└─ Data Section (连续存放)
    [Hash Buckets]       → bucket_count + size + bucket_array[]
    [Sorted Offsets]     → count + capacity + dirty + offset_array[]
    [Vector Entries]     → count + capacity + entry_array[]
    [HNSW Graph]         → dim + M + nodes + vectors + neighbor_lists
    [Namespace Tree]     → recursive: path + entries + children[]
```

**为什么放一个文件：**
1. 原子性：`rename` 一次完成，不会读到半成品
2. 简单：`mmap` 一次，解析 header 后直接指向各数据区
3. 紧凑：比 5 个独立文件少 4 次 open/mmap 开销

### 各索引内部布局

**1. Hash 索引**
```
Data: [bucket_count:8][size:8][bucket_array[]]
Bucket: [entry_offset:8][next_offset:8]  // 16 bytes each
```
- 使用 pool offset 而非指针，重启后仍然有效
- 冲突链通过 `next_offset` 链接

**2. Sorted Array**
```
Data: [count:8][capacity:8][dirty:4][padding:4][offsets[]]
offsets[]: [entry_offset:8] × count  // 按 key 字典序排列
```
- `dirty=0` 表示已排序，可直接二分查找
- 延迟排序：批量插入后统一 qsort

**3. Vector 索引**
```
Data: [count:8][capacity:8][entries[]]
Entry: [entry_offset:8][vector_offset:8][dim:2][padding:6]  // 24 bytes
```
- `vector_offset` 指向 `cache.bin` 中的向量数据
- 不重复存储向量，节省空间

**4. HNSW 图索引**
```
Data: [dim:8][M:4][M_max:4][ef:4][max_level:4][entry_point:8]
      [node_count:8][nodes[]][vectors[]][neighbor_lists[]]

Node: [id:8][level:4][valid:4][vector_offset:8]
      [neighbor_offsets:8 × (level+1)]

Neighbor List: [count:8][capacity:8][ids:8 × count]
```
- 自包含：向量数据也保存在 index.bin 中（不依赖 cache.bin）
- 加载后直接重建内存中的 neighbor_list_t 结构

**5. Namespace 树**
```
Node: [path_len:4][path[]][entry_count:8][offsets:8 × entry_count]
      [child_count:8][children[]]  // 递归
```
- 根节点 path=""
- 子节点 path 继承父节点：`/books/硅谷钢铁侠/chapters/07-...`

### 索引持久化说明

> ✅ **索引已自动持久化到 `index.bin`，重启时零拷贝加载。**
>
> 每次 `cache_close()` 或 `cache_sync()` 自动保存所有 5 个索引到 `{db_dir}/index.bin`。
>
> 重启时 `cache_open()` 会：
> 1. 尝试加载 `index.bin`（mmap 零拷贝，~4ms）
> 2. 如果加载失败或损坏，自动回退到扫描重建（O(n)，~890ms）
> 3. 重建完成后自动保存新的 `index.bin`
>
> **性能对比**（1261 条目）：
> | 方式 | 时间 | 加速 |
> |------|------|------|
> | 持久化加载 | ~4ms | **209x** |
> | 扫描重建 | ~890ms | - |
>
> **Snapshot 工具**（零拷贝导出/导入）：
> ```bash
> # 导出（sendfile 零拷贝）
> ./tools/cache_snapshot export ./my_cache ./snapshot
>
> # 导入
> ./tools/cache_snapshot import ./snapshot ./new_cache
>
> # 验证
> ./tools/cache_snapshot verify ./snapshot
> ```
>
> **设计原则**：
> - 电子书/知识库只追加不修改，`index.bin` 与 `cache.bin` 永远一致
> - 不需要版本号，重建失败自动回退
> - 所有索引一次性保存/加载，原子性 rename

### 快速开始

```python
from mydb.cache import open_cache

# 打开 cache
cache = open_cache("./agent_memory", 100*1024*1024)

# 基础 CRUD
cache.set("/agent/personality", "友好、专业、简洁", ttl_ms=0)
value = cache.get("/agent/personality")

# Namespace 操作
cache.set_ns("/coding/cpp", "move", "右值引用实现完美转发...")
cpp_knowledge = cache.get_ns("/coding/cpp", "move")

# 搜索
results = cache.search_prefix("/coding/cpp")  # 前缀搜索
results = cache.search_regex(".*async.*")      # 正则搜索
results = cache.search_fuzzy("/coding/vect")   # 模糊搜索
results = cache.search_tag("协程")              # 标签搜索

# 迭代
for key, value in cache.items():
    print(f"{key}: {value}")

# 统计
print(f"Entries: {cache.count}, Memory: {cache.memory_used / 1024} KB")

cache.close()
```

### CLI 工具

```bash
# 基础 CRUD
cache set /coding/cpp/move "右值引用实现完美转发"
cache get /coding/cpp/move
cache del /coding/cpp/move
cache list --prefix /coding

# 搜索
cache search --prefix /coding
cache search --regex ".*async.*"
cache search --fuzzy "vect"

# 管理
cache stats
cache compact
cache purge

# 导入
cache import-book ~/book.mobi /books/cpp
cache import-github https://github.com/redis/redis

# 快照（零拷贝导出/导入）
cache_snapshot export ./my_cache ./snapshot      # 导出
cache_snapshot import ./snapshot ./new_cache     # 导入
cache_snapshot verify ./snapshot                 # 验证

# 启动 TCP 服务器（远程操作）
cache_server --port 7777 --db ./cache_data

# 客户端连接
# cache_client_connect("127.0.0.1", 7777)
# cache_client_set(client, "key", "value", 0)
# cache_client_get(client, "key")
```

### 存储格式

```
cache_data/
├── cache.bin          # mmap 零拷贝存储（entry 数据）
│   # Entry: [header][key][\0][value][\0]
│   # Header: key_len, value_len, expire_at, access_time, flags
└── index.bin          # 索引持久化文件（5 个索引）
    # [Magic][Version][Index Headers...][Hash Data][Sorted Data]
    # [Vector Data][HNSW Graph][Namespace Tree]
```

**index.bin 结构**：
- 文件头：Magic "MYIX" + 版本 + 总大小 + 索引数量
- 索引条目：类型 + 数据偏移 + 数据大小 + 条目数量
- 数据区：各索引的二进制 dump，加载时直接 memcpy 到内存

### 性能基准

| 操作 | 性能 | 说明 |
|------|------|------|
| Insert | ~7,000 ops/sec | 5K entries，sorted array O(n) |
| Get | ~5,000,000 ops/sec | Hash 索引 + Hot Cache |
| Prefix Search | ~1,000,000 searches/sec | 排序数组 lower_bound |
| **Cache 启动** | **~4ms** | **加载 index.bin（5 个索引），1261 条目** |
| Cache 启动（无索引）| ~890ms | 扫描重建所有索引 |
| 语义搜索 | ~10ms | HNSW 近似搜索，384 维向量 |
| Tag Search | O(1) 索引查找 | Hash-based 反向索引 |
| Vector Search | O(n) 暴力搜索 | 精确余弦相似度，适合 < 10万条 |
| Skip List | O(log n) | 数据量 > 100万时自动启用 |
| Iterate | < 1ms | 遍历 5K 条目 |

**优化说明**：
- **延迟排序**：sorted array 采用 O(1) append + 延迟 qsort，批量插入 2.5x 提速
- **Hot Cache**：64-entry LRU 热点缓存，重复读取加速 2-3x
- **跳表升级**：数据量 > 100万时自动启用 20 级跳表，前缀/范围搜索 O(log n)

### 高级搜索

**向量相似度搜索**（语义搜索）
```c
// C API
float vector[384] = {...};  // embedding from sentence-transformers
cache_set_vector(cache, "doc1", "content", vector, 384, 0);

cache_result_t* results;
cache_search_vector(cache, query_vector, 384, top_k=5, min_score=0.5, NULL, &results, &count);
```

```bash
# Python helper
python3 tools/vector_helper.py embed "text to embed"
python3 tools/vector_helper.py store --key doc1 "content"
python3 tools/vector_helper.py search "query text"
```

**电子书语义搜索**（ONNX Runtime C 推理）

导入电子书时自动为每个段落生成 384 维语义向量：
```bash
# 1. 导出 ONNX 模型（首次使用，已包含在项目中）
python3 tools/export_onnx.py
# 输出：models/all-MiniLM-L6-v2/model.onnx + vocab.txt + config.json

# 2. 导入电子书（自动向量化）
./tools/import_book ./my_cache ~/book.azw3
# 输出：1235 paragraphs, 每段生成 embedding 并存储

# 3. 语义搜索
python3 tests/test_semantic_search.py "马斯克创办特斯拉的故事"
# 返回：第七章（全电动车）、第十章（电动车的复仇）等相关段落
```

**技术细节**：
- **模型**：sentence-transformers `all-MiniLM-L6-v2`（384 维）
- **推理**：ONNX Runtime C API，本地推理无需 Python
- **Tokenizer**：WordPiece（哈希表优化），30522 词表
- **处理流程**：段落文本 → Tokenize → ONNX 推理 → Mean Pooling → L2 归一化 → 384 维向量
- **性能**：单段落 ~100ms（含 tokenizer），导入 1200+ 段落的电子书约 2 分钟
- **存储**：向量与文本共用 key，`/books/{书名}/chapters/{章}/content/p{段落号}`

**源码语义分析**（AST 提取）
```c
// 分析源代码，自动提取函数/类/结构体/导入
cache_ast_tree_t* tree = cache_analyze_source("main.c", source_code);
char* json = cache_ast_to_json(tree, "main.c");
// 输出: {"file":"main.c","nodes":[{"type":"function","name":"main"},...]}

// 从源码提取语义标签
cache_ast_extract_tags("main.c", source, &tags, &tag_count);
// 输出: "func:main func:foo class:Bar import:stdio.h "
```

支持语言：C/C++, Python, JavaScript, Java, Go, Rust

### TCP 远程服务器

启动服务器：
```bash
./tools/cache_server --port 7777 --db ./cache_data
```

客户端 API（C）：
```c
#include "cache_server.h"

cache_client_t* client = cache_client_connect("127.0.0.1", 7777);
cache_client_ping(client);                          // PING → PONG
cache_client_set(client, "key", "value", 0);        // SET
cache_client_get(client, "key");                    // GET
int count = cache_client_count(client);             // COUNT
cache_client_disconnect(client);
```

协议（文本行协议，Redis-like）：
```
PING                    → +PONG
SET "key" "value"       → +OK
GET "key"               → $5\r\nvalue\r\n
DEL "key"               → :1
SEARCH prefix "/coding" → *OK 2\r\n...$key...$score...$value...
```

### HTTP RESTful API

启动 HTTP 服务器：
```bash
./tools/cache_http_server --port 8080 --db ./cache_http_data
```

RESTful 端点：

| 方法 | 端点 | 说明 |
|------|------|------|
| GET | `/health` | 健康检查 |
| GET | `/stats` | 统计信息（entries, memory_used, memory_max） |
| GET | `/cache/:key` | 获取 key 的值 |
| PUT | `/cache/:key` | 设置 key（JSON body: `{"value":"...","ttl_ms":1234}`） |
| DELETE | `/cache/:key` | 删除 key |
| GET | `/cache/:key/exists` | 检查 key 是否存在 |
| GET | `/search?pattern=...&type=prefix|regex|fuzzy|tag` | 搜索 |
| POST | `/batch` | 批量设置（JSON array: `[{"key":"...","value":"..."}, ...]`） |
| GET | `/namespaces` | 列出所有 namespace |
| GET | `/namespace/:ns` | 获取 namespace 下的所有 key |
| POST | `/sync` | 同步到磁盘 |

示例：
```bash
# 设置值
curl -X PUT -H "Content-Type: application/json" \
  -d '{"value":"hello world","ttl_ms":60000}' \
  http://localhost:8080/cache/mykey

# 获取值
curl http://localhost:8080/cache/mykey
# → {"status":"ok","data":{"key":"mykey","value":"hello world"}}

# 前缀搜索
curl "http://localhost:8080/search?pattern=user:&type=prefix&max_results=10"

# 批量设置
curl -X POST -H "Content-Type: application/json" \
  -d '[{"key":"k1","value":"v1"},{"key":"k2","value":"v2"}]' \
  http://localhost:8080/batch
```

### 导入工具

- **电子书**：`tools/import_book.c` — 支持 MOBI/AZW3/PDF，导入时自动生成语义向量
- **GitHub 源码**：`tools/import_github.py` — ctags 提取符号 + 源码存储

### 电子书语义搜索用法

**1. 导入电子书（自动向量化）**

```bash
# 编译
make tools/import_book

# 导入单本书（自动为每个段落生成 384 维向量）
./tools/import_book ./my_cache ~/book.azw3
# 输出：Import complete: 1235 paragraphs

# 导入多本书到同一 cache（自动跨书搜索）
./tools/import_book ./my_cache ~/book1.mobi
./tools/import_book ./my_cache ~/book2.pdf
./tools/import_book ./my_cache ~/book3.azw3
```

**2. 语义搜索（跨所有电子书）**

```bash
# Python 搜索脚本（自动生成查询向量，搜索最相关段落）
python3 tests/test_semantic_search.py "马斯克创办特斯拉的故事"

# 输出示例：
# Query: '马斯克创办特斯拉的故事'
# Found 5 results:
#
# [1] /books/硅谷钢铁侠/chapters/13-第七章 全电动车.../content/p0026
#     Score: 0.5674
#     Text: 2008年，特斯拉推出Roadster...
#
# [2] /books/硅谷钢铁侠/chapters/16-第十章 电动车的复仇.../content/p0030
#     Score: 0.5772
#     Text: Model S的诞生标志着...
```

**3. 搜索特定书籍（Namespace 过滤）**

```c
// C API：限定只搜索某本书
cache_search_options_t opts = cache_search_options_default();
opts.ns_filter = "/books/硅谷钢铁侠";  // 只搜这本书

cache_search_vector(cache, query_vector, 384, top_k=5, min_score=0.3, &opts, &results, &count);
```

**4. 存储结构**

导入后存储路径格式：
```
/books/{书名}/_meta/title              # 书名
/books/{书名}/_meta/author             # 作者
/books/{书名}/_meta/chapters           # 章节列表
/books/{书名}/chapters/{章节号}-{标题}/title       # 章节标题
/books/{书名}/chapters/{章节号}-{标题}/content/p0000   # 段落 0（含向量）
/books/{书名}/chapters/{章节号}-{标题}/content/p0001   # 段落 1（含向量）
```

**5. 性能**

- 导入速度：~600 段落/分钟（含 ONNX 推理）
- 搜索速度：~10ms/查询（HNSW 索引，>1000 向量自动启用）
- 向量维度：384 维（sentence-transformers all-MiniLM-L6-v2）

## AI Agent 源码探索（C 工具链）

基于 C 语言实现的 AI Agent 代码记忆系统，提供极速的源码索引、向量生成和自然语言语义搜索。

### 🎯 推荐使用一键脚本（新手友好）

```bash
# 查看所有命令
./ai_code_search.sh --help

# 一键完整分析代码库（索引 + 向量 + 调用图）
./ai_code_search.sh analyze /opt/stable-diffusion.cpp ./sd_cache

# 自然语言搜索
./ai_code_search.sh search ./sd_cache "upscale image" 10

# 代码片段相似搜索
./ai_code_search.sh snippet ./sd_cache ./my_code.cpp 5

# 演示系统能力
./ai_code_search.sh demo ./sd_cache
```

脚本自动处理环境变量、GPU 检测和错误提示。详细用法见脚本头部注释。

### 核心工具

| 工具 | 文件 | 功能 | 技术 |
|------|------|------|------|
| **代码索引** | `tools/code_indexer.c` | 多进程源码索引（C） | ctags + C + fork |
| **向量生成** | `tools/batch_embedder.c` | GPU 批量编码向量 | C + ONNX Runtime + TensorRT |
| **语义搜索** | `tools/vector_search.c` | 自然语言查询代码 | C + 内存 Hash 表 + 向量 |

### 完整工作流程（llama.cpp 示例）

**Step 0: 环境准备（必须执行）**

```bash
# 设置 GPU 库路径（TensorRT + cuDNN + CUDA）
export LD_LIBRARY_PATH=/home/dministrator/my_db:\
    /opt/TensorRT-10/lib:\
    /home/dministrator/anaconda3/envs/dl/lib:\
    $LD_LIBRARY_PATH

# 验证 GPU 可用
nvidia-smi
```

**Step 1: 扫描代码目录生成索引**

```bash
# 使用 C 多进程索引器（比 Python 快 37x）
# 参数：源码路径  输出目录  工作进程数
./tools/code_indexer /home/dministrator/llama.cpp ./ai_code_memory 4

# 输出：
# Phase 1: Scanning source files...
#   Found 677 source files
# Phase 2: Forking 4 workers...
#   Worker 0: 170 files
# Phase 3: Waiting for workers...
#   4/4 workers completed
# Phase 4: Merging output files...
# 
# Indexing complete!
#   Files: 677
#   Chunks: 18742
#   Workers: 4
#   Time: 0.5s
#   Output: ./ai_code_memory/chunks_text.txt, ./ai_code_memory/chunks_meta.jsonl
```

**输出文件**：
```
ai_code_memory/
├── chunks_text.txt        # 代码内容（一行一个 chunk，用于向量生成）
├── chunks_meta.jsonl      # 元数据（名称/文件/行号/签名，用于搜索展示）
└── vectors/               # 语义向量（由 Step 2 自动生成）
    ├── code_local_llama.cpp.jina.bin    # Jina 二进制向量（768维 float32）
    └── code_local_llama.cpp.jina.idx    # 名称→偏移索引（文本格式）
```

**目录结构说明**：

| 文件 | 大小（llama.cpp） | 用途 | 生成工具 |
|------|-------------------|------|---------|
| `chunks_text.txt` | ~10MB | 代码内容文本，一行一个 chunk | `code_indexer` |
| `chunks_meta.jsonl` | ~4MB | 元数据（名称/文件/行号/签名/语言） | `code_indexer` |
| `vectors/*.bin` | ~40MB | 语义向量（768维 float32） | `batch_embedder` |
| `vectors/*.idx` | ~3MB | 名称→字节偏移索引 | `batch_embedder` |

**Step 2: 生成向量索引（C + TensorRT GPU）**

```bash
# 编译（首次使用）
make tools/batch_embedder

# 生成 Jina v2 向量（代码专用，推荐）
./tools/batch_embedder ./ai_code_memory --model jina

# 输出：
# Loading items from ./ai_code_memory...
# Loaded 18742 items
# Loading jina embedder...
#   Using TensorRT GPU acceleration (FP32 for Jina)
# Encoding...
#   Progress: 6400/18742 (81 items/s)
#   Progress: 12800/18742 (86 items/s)
# Done in 207.0s (91 items/s)
# Saved vectors to ./ai_code_memory/vectors/code_local_llama.cpp.jina.bin
# Saved index to ./ai_code_memory/vectors/code_local_llama.cpp.jina.idx
```

**模型文件（`models/` 目录）**：

| 模型 | 大小 | 维度 | 适用场景 | 状态 |
|------|------|------|---------|------|
| **jina-embeddings-v2-base-code** | 617MB | 768 | **代码检索（推荐）** | ✅ 当前使用 |
| **all-mpnet-base-v2** | 419MB | 768 | 通用文本 | ✅ 可用 |
| **all-MiniLM-L6-v2** | 88MB | 384 | 通用文本 | ✅ 可用 |

```bash
models/
├── jina-embeddings-v2-base-code/     # 代码专用（推荐）
│   ├── model.onnx                    # ONNX 模型（612MB）
│   ├── vocab.json                    # BPE 词表
│   ├── merges.txt                    # BPE 合并规则
│   └── tokenizer.json                # Tokenizer 配置
│
├── all-mpnet-base-v2/                # 通用文本
│   ├── model.onnx                    # ONNX 模型（418MB）
│   └── vocab.txt                     # WordPiece 词表
│
└── all-MiniLM-L6-v2/                 # 轻量通用文本
    ├── model.onnx                    # ONNX 模型（87MB）
    └── vocab.txt                     # WordPiece 词表
```

**模型选择**：

| 模型 | 适用场景 | 速度 | 特点 |
|------|---------|------|------|
| **Jina v2** | 代码仓库（**推荐**） | ⭐⭐⭐⭐ | 代码专用嵌入，对比学习训练，语义区分度强 |
| **MPNet** | 电子书、文档、通用文本 | ⭐⭐⭐⭐⭐ | 通用文本理解，C 原生 tokenizer |

```bash
# 使用 Jina v2（代码专用，推荐）
./tools/batch_embedder ./ai_code_memory --model jina
# 生成: vectors/code_local_project.jina.bin + .jina.idx

# 使用 MPNet（通用文本）
./tools/batch_embedder ./ai_code_memory --model mpnet
# 生成: vectors/code_local_project.mpnet.bin + .mpnet.idx
```

> **注意**：向量文件名自动包含模型标识（`.jina` / `.mpnet`），不同模型向量不会互相覆盖。

**Jina v2 模型准备**：
```bash
# 1. 下载模型
# 从 https://huggingface.co/jinaai/jina-embeddings-v2-base-code 下载全部文件
# 放置到 models/jina-embeddings-v2-base-code/

# 2. 导出 ONNX（只需执行一次）
python3 tools/export_jina_onnx.py
# 输出: models/jina-embeddings-v2-base-code/model.onnx
```

**性能对比**：

| 方案 | 速度 | 19K 向量耗时 | 显存 |
|------|------|-------------|------|
| **C + TensorRT (Jina)** | **91 items/s** | **~207 秒** | **~2 GB** |

**说明**：
- `batch_embedder` 使用 C tokenizer + ONNX Runtime C API + TensorRT GPU
- 自动保存到 `vectors/code_local_<repo>.<model>.bin`
- 自动生成索引 `vectors/code_local_<repo>.<model>.idx`
- 无需手动移动文件

**Step 3: AI Agent 使用 C 应用探索源码**

```bash
# 编译语义搜索工具（首次使用）
make tools/vector_search

# 自然语言搜索代码（自动发现所有向量文件）
./tools/vector_search --model jina ./ai_code_memory "quantization" 5

# Rich 模式：返回完整代码上下文（文件路径、行号、签名）
./tools/vector_search --model jina --rich ./ai_code_memory "quantization" 5

# JSON 模式：结构化输出，AI 可直接解析
./tools/vector_search --model jina --json --rich ./ai_code_memory "quantization" 5

# 搜索示例输出（Rich 模式）：
# Top 5 results:
# ──────────────────────────────────────────────────────────────────────
# [1] GGMLQuantizationType (0.0800)
#     Location:  /home/dministrator/llama.cpp/gguf-py/gguf/gguf_writer.py:23
#     Language:  python
# 
# ──────────────────────────────────────────────────────────────────────
# [2] tensor_allows_quantization (0.0800)
#     Signature: (const llama_model_quantize_params * params,llm_arch arch,const ggml_tensor * tensor)
#     Location:  /home/dministrator/llama.cpp/src/llama-quant.cpp:288
#     Language:  cpp
#
# ──────────────────────────────────────────────────────────────────────
# [3] llama_quant_tensor_allows_quantization (0.0800)
#     Signature: (const quantize_state_impl * qs,const ggml_tensor * tensor)
#     Location:  /home/dministrator/llama.cpp/src/llama-quant.cpp:1363
#     Language:  cpp
```

# 高级过滤（按函数类型、语言、文件名）
./tools/vector_search --model jina --kind function --lang cpp ./ai_code_memory "memory allocation" 5
./tools/vector_search --model jina --file quant ./ai_code_memory "GGML" 5

# 查看所有选项和已生成的向量文件
./tools/vector_search --help
```

### 查询示例

**查询图像生成相关函数**：
```bash
./tools/vector_search ./ai_code_memory "upscale image resolution" 5
```

**查询 CUDA 相关代码**：
```bash
./tools/vector_search ./ai_code_memory "CUDA kernel for GPU" 5
```

**查询内存分配**：
```bash
./tools/vector_search ./ai_code_memory "allocate memory for tensors" 5
```

### 架构设计

**为什么用 C 而不是 Python？**

| 问题 | Python | C |
|------|--------|---|
| TensorRT 加载 | 环境变量复杂，经常失败 | 链接时直接解析，稳定 |
| 速度 | 400 items/s (CUDA fallback) | 2300+ items/s (TensorRT) |
| 内存 | Python 对象开销大 | 直接操作二进制文件 |
| 部署 | 依赖 Python 环境 | 独立可执行文件 |

**向量存储格式**（文件名包含模型标识）：
```
vectors/code_local_llama.cpp.jina.bin
├─ Header: [count:4][dim:4]           # 13238, 768
└─ Vectors: [name_len:4][name][float768] × 13238   # 归一化 float32 数组

vectors/code_local_llama.cpp.jina.idx
└─ Text: "name":offset\n               # 名称到字节偏移的映射

# 同一项目可共存多个模型的向量
vectors/code_local_llama.cpp.jina.bin        # Jina v2 (推荐，代码专用)
vectors/code_local_llama.cpp.jina.idx
vectors/code_local_llama.cpp.mpnet.bin       # MPNet (通用文本)
vectors/code_local_llama.cpp.mpnet.idx
```

**搜索流程**：
```
用户查询 → Tokenize → ONNX Runtime (TensorRT) → 768维向量
                                              ↓
                                           余弦相似度
                                              ↓
读取 .bin 文件 → 遍历所有向量 → 计算相似度 → Top-K 排序 → 输出结果
```

### 与 AI Agent 集成

AI Agent 可以通过调用 `vector_search` C 程序来查询源码：

```python
import subprocess
import json

def search_code(query, cache_dir="./ai_code_memory", top_k=5, namespace=None, rich=True, model="jina"):
    """AI Agent 调用 C 工具进行语义搜索（支持任意项目）
    
    Args:
        query: 自然语言查询
        cache_dir: 缓存目录
        top_k: 返回结果数量
        namespace: 限定项目（如 /code/local/my-project，支持模糊匹配）
        rich: 是否返回完整代码上下文（文件、行号、签名、代码）
        model: 嵌入模型，jina (默认，代码专用) 或 mpnet (通用文本)
    
    Returns:
        JSON 格式的搜索结果，包含 name, score, file, line_start, signature, content
    """
    cmd = ["./tools/vector_search", "--json"]
    if model != "mpnet":
        cmd.extend(["--model", model])
    if rich:
        cmd.append("--rich")
    cmd.extend([cache_dir, query, str(top_k)])
    if namespace:
        cmd.append(namespace)
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode != 0:
        print(f"Search failed: {result.stderr}")
        return []
    
    try:
        data = json.loads(result.stdout)
        return data.get("results", [])
    except json.JSONDecodeError:
        print("Failed to parse JSON output")
        return []

# AI Agent 使用示例（支持任意项目）
code_context = "我需要生成图像的函数"

# 搜索所有项目，获取完整上下文（使用 Jina，代码专用）
results = search_code("generate image from text", top_k=5, rich=True, model="jina")

# 或限定在特定项目
results = search_code("generate image", top_k=3, namespace="/code/local/my-project", model="jina")

# 结果注入提示词（AI 可直接使用）
prompt = f"用户需要: {code_context}\n\n相关源码:\n"
for r in results:
    prompt += f"\n## {r['name']} (相似度: {r['score']:.2%})\n"
    if r.get('file'):
        prompt += f"位置: {r['file']}:{r['line_start']}\n"
    if r.get('signature'):
        prompt += f"签名: {r['signature']}\n"
    if r.get('content'):
        prompt += f"代码:\n```cpp\n{r['content'][:1000]}\n```\n"

# 现在 prompt 包含完整的代码上下文，AI 可以直接分析和生成代码
```

### 完整使用示例

**llama.cpp（中小型项目，677 文件）**

```bash
# 1. 索引（0.7 秒）
./tools/code_indexer /home/dministrator/llama.cpp ./ai_code_memory 4

# 2. 生成向量（3.5 分钟，TensorRT GPU）
export LD_LIBRARY_PATH=/home/dministrator/my_db:/opt/TensorRT-10/lib:/home/dministrator/anaconda3/envs/dl/lib:$LD_LIBRARY_PATH
./tools/batch_embedder ./ai_code_memory --model jina

# 3. 搜索（即时响应）
./tools/vector_search --model jina --rich ./ai_code_memory "quantization" 5
```

**Linux Kernel（大型项目，58K 文件）**

```bash
# 1. 索引（43 秒，8 进程）
./tools/code_indexer /opt/linux ./linux_cache 8

# 2. 生成向量（约 2.5 小时，88 万函数/结构体，TensorRT GPU）
export LD_LIBRARY_PATH=/home/dministrator/my_db:/opt/TensorRT-10/lib:/home/dministrator/anaconda3/envs/dl/lib:$LD_LIBRARY_PATH
./tools/batch_embedder ./linux_cache --model jina

# 3. 搜索（即时响应）
./tools/vector_search --model jina --rich ./linux_cache "memory allocation" 5
./tools/vector_search --model jina --kind function --lang c ./linux_cache "scheduler" 5
./tools/vector_search --model jina --file mm ./linux_cache "page fault" 5
```

**性能基准**：

| 项目 | 文件数 | 索引时间 | 向量数 | 向量时间 | 查询速度 |
|------|--------|----------|--------|----------|----------|
| llama.cpp | 677 | 0.7s | 18,742 | 207s | < 1s |
| Linux kernel | 58,373 | 43s | 881,575 | ~2.5h | < 1s |

### 性能基准

| 项目 | 文件数 | 符号数 | 索引时间 | 向量时间 | 查询速度 |
|------|--------|--------|----------|----------|----------|
| stable-diffusion.cpp | 891 | 41,563 | 6分12秒 | 18秒 | < 1秒 |
| libmobi | 44 | 2,371 | 3秒 | 0.6秒 | < 0.1秒 |

**时间说明**：
- 索引时间：ctags 解析 + 存储到 cache
- 向量时间：TensorRT GPU 编码（batch=512）
- 查询速度：读取 127MB 二进制文件 + 计算余弦相似度

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

### 多文件 mmap 目录架构

```
game_data/              -- 数据库目录
├── users.bin          -- users 表数据
├── users.index        -- users 索引（零拷贝 mmap）
├── users.idxmeta      -- users 索引元数据
├── users.strings      -- users 变长字符串池
├── orders.bin         -- orders 表数据
├── orders.index       -- orders 索引
├── orders.idxmeta     -- orders 索引元数据
└── wal.bin            -- WAL 日志
```

**每表独立文件**：
- **`.bin`**：表数据 + 表元数据（row_count, max_rowid, deleted_count）
- **`.index`**：索引数据结构（hash buckets/nodes），零拷贝 mmap
- **`.idxmeta`**：索引定义（字段名、类型、offset），持久化索引配置
- **`.strings`**：变长字符串池（仅 VARSTRING 字段类型）
- **`wal.bin`**：单文件 WAL，所有表共享

**设计优势**：
- 单表 compact 不影响其他表
- 索引零拷贝：重启后自动恢复，无需重建
- 变长字符串独立存储，避免数据文件膨胀

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

### 电子书解析库编译（可选）

如需使用电子书导入功能（MOBI/AZW3/PDF），需额外编译 wrapper：

**依赖（需预先编译安装）：**
- `/opt/libmobi/src/.libs/libmobi.a` — libmobi 静态库（MOBI/AZW3 解析）
- `/opt/mupdf/build/release/libmupdf.a` — MuPDF 静态库（PDF 解析）
- `/opt/mupdf/build/release/libmupdf-third.a` — MuPDF 第三方库

**编译：**
```bash
cd src/importer/wrappers
make              # 编译 MOBI + PDF 两个 wrapper
make mobi         # 只编译 MOBI/AZW3 wrapper
make pdf          # 只编译 PDF wrapper
make clean        # 清理编译产物
```

**输出：**
- `src/importer/libs/libmobiparse.so` — MOBI/AZW3 解析库
- `src/importer/libs/libpdfparse.so` — PDF 解析库

## LuaJIT API 参考

### 数据库生命周期

```lua
local db = mydb.open("game_data")             -- 打开目录（自动生成所有表文件和 wal）
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

-- 支持的类型：int32, int64, uint64, float, double, string, varstring, bool
-- string  需指定 size（定长，最大长度）
-- varstring 无需 size（变长，任意长度，适合大文本）

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

| 决策项 | 原设计 | 实际实现 | 理由 |
|--------|--------|----------|------|
| **定位** | 零拷贝嵌入式存储引擎 | 同上 | 内存数据库，直接操作 struct，无网络、无序列化 |
| **存储架构** | 单文件双池 | **多文件目录**（每表独立） | 单表 compact 不影响其他表 |
| Schema 定义 | 动态 Lua 注册 | 同上 | 无需编译 C struct，脚本层完全控制 |
| 持久化机制 | mmap + 异步 msync | **mmap + 同步 msync** | 确保数据真正落盘 |
| 数据安全 | WAL 同步写入 | 同上 | 每次写先 fsync WAL，再改内存 |
| 主键 | 自增 uint64_t | 同上 | 简单高效，FFI 兼容 |
| 读取方式 | SELECT 返回 JSON 字符串 | 同上 | 无需 FFI 侧定义 struct |
| 写入方式 | Lua 表自动序列化 | 同上 | `insert({...})` 自动转换为 C 内存 |
| 删除 | 软删除 | 软删除 + **Free List** | 删除的 rowid 优先复用 |
| 内存回收 | 手动 compact | **自动 compact**（≥30% 触发） | 自动回收，无感知 |
| 索引 | 堆内存指针 | **mmap 零拷贝 offset** | 重启保留，无需重建 |
| 索引元数据 | 与索引数据混存 | **独立 `.idxmeta` 文件** | 避免覆盖索引结构 |
| JOIN 算法 | 纯嵌套循环 | **Hash Join + fallback** | 自动检测索引，性能提升 |
| 字符串类型 | 仅定长 STRING | 定长 + **变长 VARSTRING** | 支持任意长度文本 |
| 并发 | **无锁，用户自行保证** | 同上 | 类似 Redis，简单快速第一 |

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
-- 单表重建，回收已删除空间（也可自动触发）
local deleted = users:compact()

-- 全盘重建，回收所有已删除空间
local total = db:compact()

-- 设置自动 compact 阈值（默认 30%）
users:set_compact_threshold(0.3)  -- 删除率 ≥ 30% 时自动触发
```

**自动回收机制**：
- **Free List**：删除的 rowid 放入空闲列表，INSERT 优先复用
- **自动 Compact**：删除率 ≥ 30%（可配置）时自动触发，无需手动调用
- **手动 Compact**：业务低峰期执行（用户自行加锁保证独占访问）

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
├── design.md                # 设计方案
├── README.md                # 本文件
├── task.md                  # 开发任务列表
├── LICENSE
├── Makefile
├── mydb.lua                 # LuaJIT FFI 高级封装（推荐）
├── mydb.py                  # Python ctypes 高级封装
├── include/
│   ├── mydb.h               # 公共头文件（FFI 依赖）
│   └── mydb_internal.h      # 内部头文件
├── src/
│   ├── core/
│   │   ├── db.c             # 数据库生命周期（多文件目录模式）
│   │   ├── table.c          # 表操作（Free List + Compact）
│   │   └── query.c          # 查询引擎（Hash Join + 索引优化）
│   ├── types/
│   │   ├── hash.c           # 哈希表（零拷贝 mmap 索引）
│   │   └── btree.c          # B+树（二级索引）
│   ├── storage/
│   │   ├── wal.c            # WAL 日志
│   │   └── mmap.c           # mmap 封装
│   └── utils/
│       ├── crc32.c          # 校验和
│       ├── error.c          # 错误处理
│       └── json.c           # JSON 序列化（支持 VARSTRING）
├── examples/
│   ├── example_dynamic.lua  # LuaJIT 动态示例（推荐）
│   ├── example_dynamic.py   # Python 动态示例
│   └── example.c            # C 示例（编译时 Schema）
└── tests/
    ├── test_basic.c         # 基础功能测试
    ├── test_join.c          # JOIN 测试
    ├── test_perf.c          # 性能测试
    ├── test_edge.c          # 边界情况测试
    ├── test_composite.c     # 复合索引测试
    ├── test_wal.c           # WAL + Checkpoint 测试
    ├── test_ffi.lua         # LuaJIT FFI 测试
    └── test_ffi.py          # Python ctypes 测试

# KV Cache 子系统（Agent 记忆存储）
├── kvCache.md               # KV Cache 设计文档
├── include/
│   ├── cache.h              # Cache 公共 API
│   ├── cache_internal.h     # Cache 内部结构
│   └── cache_server.h       # TCP 远程服务器 API
├── src/cache/
│   ├── cache.c              # 核心存储层（CRUD + LRU + Hot Cache）
│   ├── hash_index.c         # Hash 索引
│   ├── sorted_array.c       # 延迟排序数组
│   ├── skip_list.c          # 20 级跳表
│   ├── namespace.c          # Namespace 树形索引
│   ├── search.c             # 5 种搜索实现
│   ├── tag_index.c          # Tag 反向索引
│   ├── source_analyzer.c    # 源码语义分析（7 种语言）
│   ├── vector_index.c       # 向量相似度搜索
│   ├── iter.c               # 迭代器
│   └── server.c             # TCP 文本协议服务器
├── mydb/
│   ├── cache.py             # Python Cache FFI（dict-like 接口）
│   └── cache_cli.py         # CLI 工具（cache 命令）
├── models/                   # 预训练模型（ONNX 格式）
│   ├── jina-embeddings-v2-base-code/   # 代码专用（推荐，617MB）
│   ├── all-mpnet-base-v2/              # 通用文本（419MB）
│   └── all-MiniLM-L6-v2/               # 轻量通用文本（88MB）
├── tools/
│   ├── cache_server.c       # 独立服务器可执行文件
│   ├── import_book.c        # 电子书导入（MOBI/AZW3/PDF）
│   ├── import_github.py     # GitHub 源码导入
│   ├── code_indexer.c       # 源码索引（C 多进程）
│   ├── batch_embedder.c     # 向量生成（C + TensorRT GPU）
│   ├── vector_search.c      # 语义搜索（C）
│   ├── embedding_daemon.c   # 查询加速守护进程
│   ├── build_hnsw_index.c   # HNSW 近似索引构建
│   ├── export_jina_onnx.py  # Jina 模型导出 ONNX
│   └── jina_search.py       # Jina 搜索 helper
└── tests/
    ├── test_cache.c         # Cache 基础测试
    ├── test_cache_full.c    # Cache 完整测试（7 项）
    ├── test_cache_perf.c    # Cache 性能基准
    ├── test_vector.c        # 向量搜索测试
    └── test_server.c        # TCP 服务器测试

## 依赖与构建

### 系统依赖

| 依赖 | 版本 | 用途 | 安装 |
|------|------|------|------|
| GCC | ≥9.0 | 编译 C/C++ | `apt install build-essential` |
| ctags | Universal Ctags | 源码符号提取 | `apt install universal-ctags` |
| Python | 3.8+ | 工具脚本 | 系统自带或 conda |
| CUDA | 12.x | GPU 加速（可选） | NVIDIA 官网下载 |

### 第三方库

**核心库**（项目已包含）：

| 库 | 路径 | 说明 |
|----|------|------|
| ONNX Runtime | `libonnxruntime.so.1.23.2` | 推理运行时（GPU 版） |
| TensorRT 10 | `/opt/TensorRT-10/` | NVIDIA 推理优化器 |
| libmobi | `/opt/libmobi/` | MOBI/AZW3 电子书解析 |
| mupdf | `/opt/mupdf/` | PDF 解析 |

**Python 包**：
```bash
pip install onnxruntime-gpu  # GPU 推理
# 或 CPU 版：pip install onnxruntime
```

### 构建步骤

```bash
# 1. 克隆仓库
git clone https://github.com/quqiufeng/my_db.git
cd my_db

# 2. 编译核心库
make

# 3. 编译 ONNX 嵌入器（用于向量生成）
make libonnx_embedder.so

# 4. 编译 C++ 向量生成器
cd tools && \
  g++ -O2 -o vector_indexer_v2 vector_indexer_v2.cpp \
  -I../include -L.. -lonnx_embedder -lonnxruntime_gpu \
  -Wl,-rpath,'$ORIGIN/..' -lm -ldl -lpthread

# 5. 验证
make test  # 运行所有测试
```

### 环境变量

```bash
# 必须：让程序找到 GPU 库
export LD_LIBRARY_PATH=/path/to/my_db:\
    /path/to/anaconda3/envs/dl/lib:$LD_LIBRARY_PATH

# 可选：TensorRT 缓存路径
export TRT_ENGINE_CACHE=./trt_cache
```

### 常见问题

**Q: `libnvinfer.so.10: cannot open shared object file`**
A: TensorRT 10 库未找到。确保 `/opt/TensorRT-10/lib` 在 `LD_LIBRARY_PATH` 中。

**Q: `Provider_GetHost: undefined symbol`**
A: ONNX Runtime 版本不匹配。项目使用 1.23.2，需与 TensorRT 10 配套。

**Q: 向量生成报错 `C++ encoder failed`**
A: C++ 工具未编译。运行 `make libonnx_embedder.so` 和 `cd tools && g++ ...`。

**Q: 大型项目索引崩溃**
A: 使用 `--skip-vectors` 先建立基础索引，后续再分批生成向量。

## 许可证

Apache License 2.0
