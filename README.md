# my_db — 零拷贝嵌入式存储引擎 + AI 记忆系统

> 一个基于 C 语言数据结构的**单机嵌入式零拷贝存储引擎**，同时是 **AI Agent 代码/电子书探索记忆系统**的技术底座。

## 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                        应用层                                │
│  ┌──────────────────┐  ┌──────────────────┐                │
│  │   代码探索系统    │  │   电子书系统      │                │
│  │  [coding.md]     │  │  [ebook.md]      │                │
│  │                  │  │                  │                │
│  │  analyze_repo.sh │  │  import_book     │                │
│  │  explore_repo.sh │  │  explore_book.sh │                │
│  └────────┬─────────┘  └────────┬─────────┘                │
│           │                     │                          │
│           └──────────┬──────────┘                          │
│                      │                                      │
├──────────────────────┼──────────────────────────────────────┤
│                      │                                      │
│           ┌──────────▼──────────┐                          │
│           │    KV Cache 系统     │                          │
│           │   [kvCache.md]      │                          │
│           │                     │                          │
│           │  层级化记忆存储      │                          │
│           │  语义搜索 / HNSW    │                          │
│           │  向量索引 / TTL     │                          │
│           └──────────┬──────────┘                          │
│                      │                                      │
├──────────────────────┼──────────────────────────────────────┤
│                      │                                      │
│           ┌──────────▼──────────┐                          │
│           │  零拷贝存储引擎      │                          │
│           │ [database.md]       │                          │
│           │                     │                          │
│           │  mmap 持久化        │                          │
│           │  WAL 日志           │                          │
│           │  HASH/BTREE 索引    │                          │
│           │  JOIN / CRUD        │                          │
│           └──────────┬──────────┘                          │
│                      │                                      │
├──────────────────────┼──────────────────────────────────────┤
│                      │                                      │
│           ┌──────────▼──────────┐                          │
│           │   第三方集成         │                          │
│           │ [tokenizers-cpp.md] │                          │
│           │                     │                          │
│           │  ONNX Runtime       │                          │
│           │  Jina v2 嵌入模型    │                          │
│           │  TensorRT GPU 加速   │                          │
│           └─────────────────────┘                          │
└─────────────────────────────────────────────────────────────┘
```

## 核心子系统

### 1. 零拷贝存储引擎 [database.md](database.md)

基于 C struct 的嵌入式数据库，mmap 零拷贝持久化。

- **完整 CRUD**：INSERT / SELECT / UPDATE / DELETE
- **JOIN 关联**：Hash Join + Nested Loop，等值关联查询
- **索引**：HASH（字符串）+ B+树（数值），自动选择
- **持久化**：mmap + WAL，每次写入自动 fsync，断电不丢
- **事务**：BEGIN / COMMIT / ROLLBACK（WAL 截断回滚）
- **内存回收**：软删除 + Free List + 自动 Compact（删除率≥30%触发）
- **变长字符串**：`DB_TYPE_VARSTRING` 支持任意长度文本

```c
db_t db = db_open("game_data", 1024*1024*100);
table_t users = db_table(db, "users");

struct user u = {0, "Alice", 25, 95.5};
rowid_t id = db_insert(users, &u, sizeof(u));

const char* json = db_select_by_pk_json(users, id);
// {"id":1,"name":"Alice","age":25,"score":95.500000}
```

### 2. KV Cache 记忆系统 [kvCache.md](kvCache.md)

专为 AI Agent 设计的层级化记忆存储，是代码探索和电子书系统的技术底座。

- **8 种搜索**：精确 / 前缀 / 范围 / 正则 / 模糊 / 标签 / 向量 / AST
- **语义搜索**：Jina v2 嵌入模型 + HNSW 近似索引，召回率 >95%
- **Namespace**：`/` 分隔的路径式 key，如 `/coding/cpp/move-semantics`
- **TTL + LRU**：自动过期 + 内存不足时批量淘汰
- **零拷贝**：复用 database.md 的 mmap 架构，内存 = 磁盘

### 3. 代码探索记忆系统 [coding.md](coding.md)

将任意代码仓库转换为可自然语言查询的智能记忆库。

**三条工具链：**

| 脚本 | 定位 | 适用场景 |
|------|------|----------|
| `ai_code_search.sh` | 磁盘版通用分析 | 中小项目（<1万文件），人工交互 |
| `ai_code_search_large.sh` | 超大项目分治 | Linux 内核（5万+文件），自动拆分子系统 |
| `analyze_repo.sh` + `explore_repo.sh` | 记忆系统版 | AI Agent 自动集成，跨项目关联 |

**核心能力：**
- 语义搜索：自然语言找代码（"event loop epoll"）
- 调用图：caller/callee 关系分析
- 数据流：变量定义/赋值/使用追踪（字段级 + 跨函数）
- 代码片段搜索：向量化你的代码，找最相似的实现

```bash
# 一键分析
./analyze_repo.sh https://github.com/redis/redis

# 语义搜索
./explore_repo.sh /code/redis search "memory allocation" 10

# 数据流追踪
./explore_repo.sh /code/redis dataflow c
```

### 4. 电子书记忆系统 [ebook.md](ebook.md)

将 EPUB/MOBI/AZW3/PDF 导入记忆系统，支持语义搜索和标准化探索。

**核心能力：**
- 多格式支持：EPUB / MOBI / AZW / AZW3 / PDF
- 章节切分：自动提取目录结构，按章节存储
- 语义搜索：自然语言查询书中内容
- 标准化接口：`explore_book.sh` 提供 overview/search/read/toc 四命令

```bash
# 导入电子书
./tools/import_book ./ai_code_memory ~/book.epub /books/my_book

# 语义搜索
./explore_book.sh /books/my_book search "distributed consensus"

# 阅读页面
./explore_book.sh /books/my_book read chapters/03-Consensus/page_0005

# 查看目录
./explore_book.sh /books/my_book toc
```

### 5. 文本处理集成 [tokenizers-cpp.md](tokenizers-cpp.md)

HuggingFace tokenizers-cpp 的集成方案，用于文本分词和嵌入向量生成。

- Jina v2 嵌入模型（768 维）
- ONNX Runtime C API 推理
- TensorRT GPU 加速
- 纯 C 实现，运行时不依赖 Python

## 快速开始

### 1. 克隆并编译

```bash
git clone https://github.com/quqiufeng/my_db.git
cd my_db
make
```

### 2. 运行基础测试

```bash
./tests/test_basic      # CRUD 测试
./tests/test_join       # JOIN 测试
./tests/test_wal        # WAL 恢复测试
```

### 3. 探索代码库（示例：redis）

```bash
# 方式一：磁盘版（人工交互）
./ai_code_search.sh analyze /opt/redis ./redis_cache
./ai_code_search.sh search ./redis_cache "memory allocation" 10

# 方式二：记忆系统版（AI Agent 集成）
./analyze_repo.sh /opt/redis
./explore_repo.sh /code/local/redis search "memory allocation" 10
```

### 4. 探索电子书（示例：DDIA）

```bash
# 导入
./tools/import_book ./ai_code_memory ~/ddia.epub /books/ddia

# 搜索并阅读
./explore_book.sh /books/ddia search "consensus algorithm"
./explore_book.sh /books/ddia read chapters/08-Distributed_Consensus/page_0012
```

## 项目目录结构

```
my_db/
├── README.md                 # 本文件（项目总览）
├── database.md               # 零拷贝存储引擎设计文档
├── kvCache.md                # KV Cache 记忆系统设计文档
├── coding.md                 # 代码探索记忆系统设计文档
├── ebook.md                  # 电子书记忆系统设计文档
├── tokenizers-cpp.md         # 第三方文本处理集成文档
│
├── include/
│   ├── mydb.h               # 存储引擎 C API 头文件
│   ├── cache.h              # KV Cache C API 头文件
│   └── mydb_internal.h      # 内部数据结构定义
│
├── src/
│   ├── core/                # 存储引擎核心
│   │   ├── db.c             # 数据库生命周期
│   │   ├── table.c          # 表操作（CRUD + Compact）
│   │   └── query.c          # 查询引擎（JOIN + 索引优化）
│   ├── storage/             # 持久化层
│   │   ├── mmap.c           # mmap 零拷贝内存池
│   │   └── wal.c            # WAL 日志读写
│   ├── types/               # 数据结构
│   │   ├── hash.c           # 哈希表索引
│   │   └── btree.c          # B+树索引
│   ├── cache/               # KV Cache 子系统
│   │   ├── cache.c          # 核心实现
│   │   ├── vector_index.c   # 向量索引 + HNSW
│   │   ├── search.c         # 语义搜索
│   │   └── ...
│   └── embedding/           # ONNX 嵌入推理
│       └── onnx_embedder.c  # Jina v2 模型推理
│
├── tools/                   # 用户工具
│   ├── import_book          # 电子书导入
│   ├── cache_query          # KV Cache 查询
│   ├── code_indexer         # 代码索引器
│   ├── batch_embedder       # 批量向量生成
│   └── vector_search        # 语义搜索引擎
│
├── analyze_repo.sh          # 代码分析主控脚本（记忆系统版）
├── explore_repo.sh          # 代码探索脚本（AI Agent 接口）
├── ai_code_search.sh        # 代码分析脚本（磁盘版）
├── ai_code_search_large.sh  # 超大项目分治脚本
├── explore_book.sh          # 电子书探索脚本
│
├── tests/                   # 测试用例
│   ├── test_basic.c         # 基础 CRUD 测试
│   ├── test_join.c          # JOIN 测试
│   ├── test_wal.c           # WAL 恢复测试
│   └── ...
│
└── models/                  # AI 模型
    └── jina-embeddings-v2-base-code/  # Jina v2 嵌入模型
```

## 文档导航

| 文档 | 内容 | 适合读者 |
|------|------|----------|
| **[database.md](database.md)** | 存储引擎设计：mmap、WAL、索引、JOIN、事务 | 想了解底层存储原理的开发者 |
| **[kvCache.md](kvCache.md)** | KV Cache 设计：8 种搜索、HNSW、TTL/LRU | 想了解 AI Agent 记忆系统的开发者 |
| **[coding.md](coding.md)** | 代码探索系统：语义搜索、调用图、数据流 | 想用 AI 探索代码库的开发者 |
| **[ebook.md](ebook.md)** | 电子书系统：导入、搜索、阅读 | 想用 AI 阅读电子书的用户 |
| **[tokenizers-cpp.md](tokenizers-cpp.md)** | 文本处理：Jina v2、ONNX、TensorRT | 想了解嵌入推理实现的开发者 |

## 技术栈

- **核心引擎**：100% C11，零拷贝 mmap
- **嵌入推理**：ONNX Runtime C API + TensorRT
- **向量模型**：Jina v2（768 维）/ all-MiniLM-L6-v2（384 维）
- **分词器**：tokenizers-cpp（HuggingFace 官方）
- **GPU 加速**：TensorRT + cuDNN + CUDA
- **构建系统**：GNU Make

## License

MIT License