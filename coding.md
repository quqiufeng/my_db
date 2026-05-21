# 代码探索记忆系统 - 永不丢失的代码知识大脑

> [← 返回项目总览](README.md)

## 系统概述

本项目实现了一套**本地代码语义搜索与记忆系统**，能够将任意代码仓库（从几百文件的小项目到 Linux 内核级别的超大项目）转换为可自然语言查询的智能记忆库。系统基于 C 语言实现核心引擎，使用 GPU 加速语义向量生成，通过 mmap 持久化存储确保知识永不丢失。

**核心理念**：把代码库变成你的"第二大脑"——一次索引，永久记忆，随时用自然语言"回忆"起任何实现细节、调用关系和数据流路径。

### 系统特点

- **完全本地运行**：代码不离开本地机器，企业级隐私安全
- **GPU 加速**：TensorRT + cuDNN + CUDA，向量生成速度 ~100 items/s
- **持久化存储**：mmap + msync(MS_SYNC)，断电不丢数据
- **自然语言搜索**：用英文描述想找的功能，无需记住函数名
- **多维度分析**：语义搜索 + 调用图 + 数据流追踪 + 代码片段匹配
- **超大项目支持**：智能分治策略，支持 Linux 内核（5万+文件）

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        用户接口层                                │
│  ┌──────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │ analyze_repo.sh  │  │ ai_code_search.sh│  │ai_code_search_ │ │
│  │  (分析主控)       │  │  (统一搜索)       │  │   large.sh     │ │
│  │                  │  │                  │  │ (超大项目分治)  │ │
│  └────────┬─────────┘  └────────┬─────────┘  └───────┬────────┘ │
└───────────┼─────────────────────┼────────────────────┼──────────┘
            │                     │                    │
            ▼                     ▼                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                      核心工具链（C 实现）                         │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐            │
│  │ code_indexer │ │batch_embedder│ │ vector_search│            │
│  │   (索引器)    │ │  (向量化)     │ │   (搜索引擎)  │            │
│  └──────────────┘ └──────────────┘ └──────────────┘            │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐            │
│  │  call_graph  │ │   dataflow   │ │   word_freq  │            │
│  │  (调用图)     │ │  (数据流)     │ │  (词频统计)   │            │
│  └──────────────┘ └──────────────┘ └──────────────┘            │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      KV Cache 存储层                             │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │   cache.bin     │  │    index.bin     │  │   vectors/      │ │
│  │   (mmap数据)     │  │   (Hash+SkipList)│  │   (语义向量)     │ │
│  └─────────────────┘  └──────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      推理加速层                                  │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │  model.onnx     │  │ ONNX Runtime     │  │  TensorRT       │ │
│  │  (Jina v2模型)   │  │   (推理引擎)      │  │   (GPU加速)      │ │
│  └─────────────────┘  └──────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 数据流

1. **导入阶段**：脚本读取源码 → code_indexer 提取函数/代码块 → batch_embedder 生成语义向量 → cache_import 写入 KV Cache
2. **分析阶段**：call_graph 构建调用关系、dataflow 追踪变量、word_freq 统计词频
3. **存储阶段**：所有数据通过 mmap 持久化到磁盘，实时 msync 保证不丢失
4. **查询阶段**：自然语言查询 → 向量化 → HNSW 近似搜索 → 返回相关代码块 + 调用关系 + 数据流

---

## 核心组件详解

### 1. analyze_repo.sh — 代码分析主控脚本

**定位**：将任意代码仓库一键转换为可查询的记忆（完整 6 步流水线）

**路径**：`./analyze_repo.sh`

**功能**：
- 自动获取源码（GitHub URL 自动 clone，本地路径直接使用）
- 并行索引代码（C 多进程，ctags + AST，每秒 1000+ 文件）
- GPU 生成语义向量（TensorRT，~100 items/s）
- 构建调用图（caller/callee 关系）
- 数据流分析（变量定义/赋值/使用追踪）
- 导入 KV Cache（mmap 持久化，层级命名空间）

**用法**：
```bash
./analyze_repo.sh <source> [namespace] [options]
```

**参数说明**：
| 参数 | 必需 | 说明 |
|------|------|------|
| `source` | 是 | GitHub URL 或本地路径 |
| `namespace` | 否 | 记忆命名空间，默认自动检测 |
| `--skip-vectors` | 否 | 跳过向量生成（更快，但无语义搜索） |
| `--skip-callgraph` | 否 | 跳过调用图分析 |
| `--skip-dataflow` | 否 | 跳过数据流分析 |
| `--cache-dir <dir>` | 否 | KV Cache 目录（默认: ./ai_code_memory） |
| `--jobs <n>` | 否 | 并行工作进程数（默认: CPU 核心数） |
| `--name <name>` | 否 | 项目名（用于目录命名） |

**命名空间自动检测**：
- GitHub URL: `https://github.com/owner/repo` → `/code/owner/repo`
- 本地路径: `/opt/nginx` → `/code/local/nginx`

**使用示例**：
```bash
# 分析 GitHub 仓库（自动 clone）
./analyze_repo.sh https://github.com/redis/redis

# 分析本地项目
./analyze_repo.sh /opt/nginx

# 指定命名空间
./analyze_repo.sh /opt/nginx /code/nginx

# 快速分析（跳过 GPU 向量生成，适合只看代码结构）
./analyze_repo.sh /opt/redis --skip-vectors

# 仅索引+调用图（适合快速浏览）
./analyze_repo.sh /opt/sqlite --skip-vectors --skip-dataflow

# 指定并行度和缓存目录
./analyze_repo.sh /opt/linux /code/linux --jobs 8 --cache-dir /data/cache
```

**6 步流水线详解**：

| 步骤 | 工具 | 输入 | 输出 | 说明 |
|------|------|------|------|------|
| Step 1 | git clone | GitHub URL | 临时目录 | 自动清理临时文件 |
| Step 2 | code_indexer | 源码目录 | chunks_text.txt, chunks_meta.jsonl | 多进程并行，每秒 1000+ 文件 |
| Step 3 | batch_embedder | chunks_text.txt | vectors/*.jina.bin | TensorRT GPU，~100 items/s |
| Step 4 | call_graph | chunks_meta.jsonl | call_graph.json | 函数调用关系 |
| Step 5 | dataflow | chunks_meta.jsonl | dataflow.json | 变量数据流 |
| Step 6 | cache_import | 所有分析结果 | cache.bin, index.bin | mmap 持久化，实时落盘 |

**输出数据结构**：
```
./{project}_cache/                    # 分析目录（临时数据）
├── chunks_text.txt                   # 代码内容（一行一个 chunk）
├── chunks_meta.jsonl                 # 元数据（函数名/文件/行号/签名）
├── call_graph.json                   # 调用关系图
├── dataflow.json                     # 数据流分析
└── vectors/
    ├── code_local_{project}.jina.bin       # 语义向量（768维 float32）
    ├── code_local_{project}.jina.idx       # 名称→偏移索引
    └── code_local_{project}.jina.bin.hnsw  # HNSW 近似索引

./ai_code_memory/                     # KV Cache（持久化存储）
├── cache.bin                         # mmap 数据文件
└── index.bin                         # 索引文件（Hash + Skip List）
```

**内存中的 Key 格式**：
```
/code/{project}/chunks/{file}/{function}     # 代码块
/code/{project}/symbols/{name}               # 符号索引
/code/{project}/callers/{function}           # 调用者列表
/code/{project}/callees/{function}           # 被调用者列表
/code/{project}/_meta/info                   # 项目元数据
```

**查询示例**（分析完成后）：
```bash
# 符号上下文查询（含 caller/callee）
./tools/cache_query <function> --repo /code/local/redis --type context --depth 2

# 语义搜索（自然语言）
./tools/cache_query "memory allocation" --repo /code/local/redis --type search

# 精确查找
./tools/cache_query /code/local/redis/symbols/zmalloc --type exact
```

---

### 2. ai_code_search.sh — AI Agent 统一搜索脚本

**定位**：对已索引的代码库进行查询和探索的统一入口

**路径**：`./ai_code_search.sh`

**核心思想**：100% C 语言实现核心引擎，Shell 脚本做统一命令封装，用户无需关心底层工具链。

**架构特点**：
- **用户代码层**：100% C（索引、搜索、分析、调用图、数据流）
- **推理引擎层**：C++ 库（ONNX Runtime + TensorRT 提供 GPU 加速）
- **脚本包装层**：Shell（一键命令封装）
- **模型训练层**：Python（一次性导出 ONNX，运行时不需 Python）

**子命令列表**：

| 命令 | 别名 | 功能 | 使用场景 |
|------|------|------|----------|
| `index` | `i` | 索引代码库 | 首次分析项目 |
| `vector` | `v` | 生成向量+调用图+数据流 | 索引后生成语义数据 |
| `search` | `s` | **语义搜索** | 用自然语言找代码 |
| `snippet` | `snip` | **代码片段搜索** | 找相似实现 |
| `dataflow` | `df` | **变量数据流追踪** | 追踪变量生命周期 |
| `callgraph` | `c` | 构建调用图 | 分析函数依赖 |
| `analyze` | `a` | **一键完整分析** | 快速开始 |
| `demo` | `d` | 演示系统能力 | 了解系统功能 |

#### 2.1 index — 索引代码库

```bash
./ai_code_search.sh index <repo_path> [cache_dir] [workers]
```

**示例**：
```bash
./ai_code_search.sh index /opt/stable-diffusion.cpp ./sd_cache 4
```

**输出**：
- `chunks_text.txt` — 代码文本（用于向量生成）
- `chunks_meta.jsonl` — 元数据（用于搜索展示）

#### 2.2 vector — 生成语义向量

```bash
./ai_code_search.sh vector <cache_dir> [project_name]
```

**示例**：
```bash
./ai_code_search.sh vector ./sd_cache stable-diffusion.cpp
```

**内部流程**：
1. 统计词频（TF-IDF 排序优化）
2. 生成语义向量（TensorRT GPU）
3. 构建调用关系图（含参数信息）
4. 生成变量数据流分析（字段级+跨函数）

**输出**：
- `vectors/*.jina.bin` — 语义向量
- `vectors/*.jina.idx` — 向量索引
- `call_graph.json` — 调用关系图
- `dataflow.json` — 变量数据流
- `word_freq.json` — 词频统计

#### 2.3 search — 语义搜索（核心功能）

```bash
./ai_code_search.sh search <cache_dir> "<query>" [max_results]
```

**示例**：
```bash
./ai_code_search.sh search ./sd_cache "upscale image" 5
./ai_code_search.sh search ./sd_cache "memory allocation buffer pool" 10
./ai_code_search.sh search ./sd_cache "VAE encode latent" 5
```

**搜索技巧**：
1. 用英文自然语言描述想找的功能（如 "event loop" 而非 "事件循环"）
2. 组合关键词提高精度（如 "memory allocation buffer pool"）
3. 查看调用关系（--callgraph）理解函数在架构中的角色
4. 用 `--rich` 查看完整代码上下文
5. 用 `--kind function` 只搜索函数，过滤变量和宏

#### 2.4 snippet — 代码片段搜索

```bash
./ai_code_search.sh snippet <cache_dir> <code_file> [max_results]
```

**示例**：
```bash
./ai_code_search.sh snippet ./sd_cache ./my_kernel.cpp 5
```

**原理**：将你的代码片段向量化，搜索代码库中最相似的实现。

**用途**：学习最佳实践、找到参考实现、发现更好的写法。

#### 2.5 dataflow — 变量数据流追踪

```bash
./ai_code_search.sh dataflow <cache_dir> <var_name>
```

**示例**：
```bash
# 追踪 connection 指针（含字段级和跨函数流）
./ai_code_search.sh dataflow ./nginx_cache c

# 追踪内存池
./ai_code_search.sh dataflow ./nginx_cache pool

# 追踪上下文指针
./ai_code_search.sh dataflow ./sd_cache ctx
```

**输出示例**：
```
📌 DEFINITIONS: __inc_zone_page_state()
✏️  ASSIGNMENTS: __page_frag_cache_refill(), alloc_zpdesc()
👁️  USAGES: page_zone(), page_pgdat(), put_page_testzero()
🔍 FIELDS: page->lru, page->buddy_list, page->_mapcount
```

**功能**：
- **字段级追踪**：自动区分 `var->field`（如 `c->fd`, `c->data`）
- **跨函数流**：追踪变量在调用链中的传递路径
- **分类显示**：定义(DEF) / 赋值(SET) / 使用(USE)

#### 2.6 analyze — 一键完整分析

```bash
./ai_code_search.sh analyze <repo_path> [cache_dir]
```

**示例**：
```bash
./ai_code_search.sh analyze /opt/stable-diffusion.cpp ./sd_cache
```

**流程**：index → vector → 输出使用提示

#### 2.7 demo — 演示系统能力

```bash
./ai_code_search.sh demo [cache_dir]
```

**示例**：
```bash
./ai_code_search.sh demo ./sd_cache
```

**演示内容**：
1. 自然语言搜索（"upscale image"）
2. 架构理解（"text encoder embedding"）
3. 跨文件调用关系（"memory allocation buffer pool"）

---

### 3. ai_code_search_large.sh — 超大项目分治脚本

**定位**：处理 Linux 内核级别（5万+文件，100万+ chunks）的超大项目

**路径**：`./ai_code_search_large.sh`

**核心问题**：
- 直接处理 5 万文件 → 4 小时+ 向量生成，1.1GB 内存占用，可能 OOM
- 分治后：每个子系统独立处理，不会 OOM，可并行索引、串行向量化

**分治策略**：
1. **智能拆分**：自动分析项目结构，按文件数量拆分子系统
2. **大目录细分**：如 `drivers/` 按二级目录拆分为 `drivers_gpu`、`drivers_net`...
3. **小目录合并**：小目录合并到 `misc` 子系统
4. **排除非核心目录**：`testing/`、`Documentation/`、`samples/` 等

**子命令列表**：

| 命令 | 功能 | 使用场景 |
|------|------|----------|
| `init` | 初始化配置（智能分析并拆分子系统） | 首次处理超大项目 |
| `index` | 索引所有子系统 | 生成代码索引 |
| `vector` | 生成所有子系统向量 | 生成语义向量 |
| `search` | 全局搜索（聚合所有子系统） | 跨子系统搜索 |
| `search-sub` | 搜索指定子系统 | 精准搜索特定模块 |
| `dataflow` | 全局变量追踪 | 追踪变量在全局的范围 |
| `status` | 查看处理状态 | 查看进度 |

#### 3.1 init — 初始化配置

```bash
./ai_code_search_large.sh init <repo_path> [config_file]
```

**示例**：
```bash
./ai_code_search_large.sh init /opt/linux
```

**自动检测结果**（Linux 内核示例）：
```
kernel/  → kernel_cache (630文件, 29,411 chunks)
mm/      → mm_cache (188文件, 11,405 chunks)
fs/      → fs_cache (2,160文件, 76,092 chunks)
net/     → net_cache (1,729文件, 66,571 chunks)
drivers/ → drivers_gpu, drivers_net 等 40+ 子系统
arch/    → arch_x86, arch_arm 等 17 子系统
排除：testing/, Documentation/, samples/ 等非核心目录
```

#### 3.2 index — 索引所有子系统

```bash
./ai_code_search_large.sh index [workers]
```

**示例**：
```bash
./ai_code_search_large.sh index 8
```

**特点**：
- 并行索引（每 4 个子系统一批，避免 fork 过多）
- 独立输出目录，互不干扰
- 自动跳过不存在的子系统

#### 3.3 vector — 生成所有子系统向量

```bash
./ai_code_search_large.sh vector [workers]
```

**示例**：
```bash
./ai_code_search_large.sh vector 1
```

**特点**：
- 串行生成（避免 GPU 显存 OOM）
- 自动跳过超过 chunk 数限制的子系统
- 同时生成调用图和数据流

#### 3.4 search — 全局搜索

```bash
./ai_code_search_large.sh search "<query>" [max_results]
```

**示例**：
```bash
./ai_code_search_large.sh search "schedule task" 10
./ai_code_search_large.sh search "page fault handler" 5
```

**特点**：
- 聚合所有子系统结果
- 自动去重
- 显示结果来源子系统

#### 3.5 search-sub — 子系统内搜索

```bash
./ai_code_search_large.sh search-sub <subsystem> "<query>" [max_results]
```

**示例**：
```bash
./ai_code_search_large.sh search-sub kernel "scheduler" 5
./ai_code_search_large.sh search-sub mm "slab allocator" 5
```

**特点**：更精准，只在指定子系统内搜索

#### 3.6 dataflow — 全局变量追踪

```bash
./ai_code_search_large.sh dataflow <var_name> [subsystem]
```

**示例**：
```bash
# 全局追踪
./ai_code_search_large.sh dataflow task_struct

# 指定子系统
./ai_code_search_large.sh dataflow task_struct kernel
```

**特点**：
- 全局搜索：在所有子系统中查找变量
- 自动跳过不包含该变量的子系统
- 显示子系统分隔线

#### 3.7 status — 查看处理状态

```bash
./ai_code_search_large.sh status
```

**输出示例**：
```
╔══════════════════════════════════════════════════════════════════╗
║ 项目: linux                                                      ║
║ 路径: /opt/linux                                                 ║
╚══════════════════════════════════════════════════════════════════╝

子系统                      Chunks        向量         调用图
───────────────────────── ──────────── ──────────── ──────────
kernel                     29411        ✓            ✓
mm                         11405        ✓            ✓
fs                         76092        ✓            ✓
net                        66571        ✓            ✓
...
───────────────────────── ────────────
总计                       1469782

Cache 目录: ./linux_subsystems
```

---

## 代码探索方法论

### 以 nginx 为例的完整探索流程

**Phase 1: 索引与向量化**
```bash
./ai_code_search.sh index /opt/nginx ./nginx_cache 4
./ai_code_search.sh vector ./nginx_cache nginx
```

**Phase 2: 架构概览（自然语言搜索核心概念）**
```bash
./ai_code_search.sh search ./nginx_cache "event loop epoll kqueue" 10
./ai_code_search.sh search ./nginx_cache "HTTP request phase handler" 10
./ai_code_search.sh search ./nginx_cache "master process worker process fork" 10
./ai_code_search.sh search ./nginx_cache "memory pool palloc" 10
```

**Phase 3: 调用关系分析（理解架构依赖）**
```bash
./ai_code_search.sh search ./nginx_cache "upstream load balancing" 10
# 关键发现：ngx_http_upstream_init_round_robin_peer 被多个算法调用
# 洞察：Round-robin 是所有负载均衡算法的基础

./ai_code_search.sh callgraph ./nginx_cache
# 查看 call_graph.json，搜索某个函数的调用者列表
```

**Phase 4: 子系统深入（逐步细化）**
```bash
# 内存管理
./ai_code_search.sh search ./nginx_cache "shared memory zone slab" 10
./ai_code_search.sh dataflow ./nginx_cache c

# 配置解析
./ai_code_search.sh search ./nginx_cache "configuration parser lexer" 10

# SSL/TLS
./ai_code_search.sh search ./nginx_cache "SSL certificate handshake" 10

# 缓存系统
./ai_code_search.sh search ./nginx_cache "file cache open read" 10
```

**Phase 5: 代码片段搜索（找到相似实现）**
```bash
./ai_code_search.sh snippet ./nginx_cache ./my_epoll_code.cpp 5
```

**Phase 6: 变量数据流（追踪生命周期）**
```bash
./ai_code_search.sh dataflow ./nginx_cache c
# 追踪 connection 指针的定义、赋值、使用位置
```

### 分析框架

对每个代码库，建议按以下框架分析：

1. **核心架构**（事件循环、进程模型、请求管线）
2. **内存管理**（分配器、内存池、垃圾回收）
3. **配置系统**（解析器、热加载、配置继承）
4. **模块系统**（初始化、加载、钩子机制）
5. **网络处理**（连接管理、协议实现、负载均衡）
6. **安全机制**（认证、加密、访问控制）
7. **缓存策略**（文件缓存、元数据缓存、缓存失效）
8. **日志系统**（分级日志、格式化、输出目标）
9. **高级特性**（重写引擎、正则表达式、新协议）
10. **源码质量评价**（架构清晰度、可扩展性、性能优化）

---

## 已验证的实战案例

### nginx（400 文件，9,174 符号）

使用本系统分析 nginx：

- ✅ **事件循环**：找到 epoll(Linux) + kqueue(BSD) 双平台实现
- ✅ **Phase Handler**：发现 11 阶段 HTTP 请求处理管线
- ✅ **内存池**：请求级别分配（create → palloc → destroy）
- ✅ **Slab Allocator**：共享内存用于 upstream 状态、rate limiting、SSL session
- ✅ **负载均衡**：Round-robin 是所有算法（least_conn/ip_hash/hash）的基础
- ✅ **配置解析**：词法分析 + 语法分析，支持 include 递归
- ✅ **多进程**：Master-Worker + Cache Manager，prefork 模型

### Linux 内核（58,373 文件，1,469,782 chunks）

使用分治策略分析 Linux 内核：

**内存管理子系统（mm/）**：
- 分治后：188 文件，11,405 chunks，2 分钟完成向量化
- 搜索 "slab allocator cache" → 找到 allocate_slab (0.9201)
- 搜索 "page allocation order zone" → 找到 prepare_alloc_pages (0.8928)
- 搜索 "memory compaction migrate" → 找到 migrate_vma_pages (0.8559)

**核心发现**：
```
内存分配三级层次：
  kmalloc/vmalloc (用户接口)
      ↓
  SLUB 分配器 (对象缓存：___slab_alloc → allocate_slab)
      ↓
  Buddy 系统 (物理页面：__alloc_pages_slowpath)

关键发现：
  - Buddy: order参数决定2^order个连续页面
  - SLUB: percpu cpu_slab → node partial → allocate_slab 三级缓存
  - OOM: __alloc_pages_may_oom → out_of_memory → oom_kill_process
  - 压缩: __alloc_pages_direct_compact → compact_zone
```

### AI Agent Linux 内核开发完整工作流

**场景**：修改 page allocation 失败时的调试信息

```bash
# 1. 理解代码（语义搜索）
./tools/vector_search ./linux_subsystems/mm_cache \
    "page allocation fail slowpath" 5 --rich
# → 找到 __alloc_pages_slowpath 是核心入口
# → 找到 prepare_alloc_pages 解析 gfp_mask
# → 找到 __alloc_pages_may_oom 触发 OOM

# 2. 分析影响范围（数据流 + 调用图）
./tools/dataflow show ./linux_subsystems/mm_cache gfp
# → gfp_mask 传递到 __alloc_pages_may_oom

./tools/vector_search ./linux_subsystems/mm_cache \
    "__alloc_pages_slowpath" 3 --callgraph
# → 被 alloc_pages, __get_free_pages 调用

# 3. 修改代码（编辑器）
vim /opt/linux/mm/page_alloc.c
# 在 __alloc_pages_slowpath 中添加 printk 或 tracepoint

# 4. 编译内核
cd /opt/linux
make oldconfig
make -j$(nproc)

# 5. 运行测试（QEMU）
qemu-system-x86_64 \
    -kernel arch/x86/boot/bzImage \
    -append "console=ttyS0 debug loglevel=8" \
    -serial stdio \
    -m 512M \
    -initrd rootfs.cpio.gz

# 6. 验证修改（搜索确认）
./tools/vector_search ./linux_subsystems/mm_cache \
    "your_new_debug_function" 5 --callgraph
# → 确认新函数被正确调用
```

---

## 数据存储与持久化

### KV Cache 存储结构

```
ai_code_memory/
├── cache.bin                    # mmap 数据文件（零拷贝持久化）
│   ├── 实时落盘：每次写入后 msync(MS_SYNC)
│   ├── 断电不丢：操作系统保证 mmap 数据落盘
│   └── 零拷贝：读取时直接从磁盘映射到内存
│
├── index.bin                    # 索引文件（Hash + Skip List）
│   ├── Hash 索引：O(1) 精确查找
│   ├── Skip List：O(log n) 范围查询
│   └── 自动加载：启动时从磁盘加载，无需重建
│
└── vectors/
    ├── code_local_{project}.jina.bin       # 二进制向量文件
    │   ├── 格式：每个向量 768 * 4 = 3072 字节（float32）
    │   └── 直接映射：可用于 GPU 推理
    │
    ├── code_local_{project}.jina.idx       # 偏移索引
    │   ├── 格式：名称 → (偏移, 维度) 映射
    │   └── 用途：快速定位向量位置
    │
    └── code_local_{project}.jina.bin.hnsw  # HNSW 近似索引
        ├── 算法：Hierarchical Navigable Small World
        ├── 复杂度：构建 O(n log n)，搜索 O(log n)
        └── 精度：>95% 召回率，毫秒级响应
```

### 持久化保证

- **写入即持久**：每次 `cache_set()` 后调用 `msync(MS_SYNC)`，数据立即落盘
- **断电安全**：即使系统崩溃，已写入的数据不会丢失
- **原子更新**：索引和数据文件同步更新，不会出现不一致状态
- **增量友好**：支持多次导入同一项目，更新已有条目

---

## 故障排查

### 常见问题

#### 1. 索引失败

**症状**：`code_indexer` 返回非零退出码

**解决**：
```bash
# 检查工具是否存在
ls -la tools/code_indexer

# 重新编译
make clean && make tools/code_indexer

# 检查源码目录权限
ls -la /opt/nginx
```

#### 2. 向量生成失败

**症状**：`batch_embedder` 报错或返回非零

**解决**：
```bash
# 检查 GPU 环境
nvidia-smi

# 检查 LD_LIBRARY_PATH
echo $LD_LIBRARY_PATH

# 检查模型文件
ls -la models/jina-embeddings-v2-base-code/

# 跳过向量生成（仅索引+调用图）
./analyze_repo.sh /opt/nginx --skip-vectors
```

#### 3. 搜索无结果

**症状**：语义搜索返回空结果

**解决**：
```bash
# 检查向量文件是否存在
ls -la {cache_dir}/vectors/*.hnsw

# 检查 chunks 是否生成
wc -l {cache_dir}/chunks_meta.jsonl

# 重新生成向量
./ai_code_search.sh vector {cache_dir}
```

#### 4. 调用图/数据流为空

**症状**：`call_graph.json` 或 `dataflow.json` 为空

**解决**：
```bash
# 检查 chunks_meta.jsonl 是否有内容
head -5 {cache_dir}/chunks_meta.jsonl

# 手动运行分析工具
./tools/call_graph {cache_dir}
./tools/dataflow analyze {cache_dir}
```

#### 5. 超大项目 OOM

**症状**：处理大项目时内存溢出

**解决**：
```bash
# 使用分治脚本
./ai_code_search_large.sh init /opt/linux
./ai_code_search_large.sh index 4
./ai_code_search_large.sh vector 1  # 串行，避免 GPU OOM
```

---

## 与其他系统的对比

| 维度 | 本系统 | IDE 搜索 | grep/ack | Sourcegraph |
|------|--------|----------|----------|-------------|
| **搜索方式** | 自然语言语义 | 符号跳转 | 关键词正则 | 符号+正则 |
| **理解能力** | 概念级匹配 | 精确匹配 | 字面匹配 | 精确匹配 |
| **调用关系** | 自动构建 | 静态分析 | 无 | 静态分析 |
| **数据流** | 变量追踪 | 有限 | 无 | 有限 |
| **代码片段** | 相似度匹配 | 无 | 无 | 无 |
| **本地运行** | ✅ 完全本地 | ✅ | ✅ | ❌ 需服务端 |
| **隐私安全** | ✅ 不上传 | ✅ | ✅ | ❌ 上传代码 |
| **大项目** | ✅ 分治策略 | ⚠️ 慢 | ⚠️ 慢 | ✅ |
| **离线使用** | ✅ | ✅ | ✅ | ❌ |

---

## 可以做的应用方向

基于这套代码探索基础设施，可以构建：

### 1. AI 代码审查助手
- 自动找潜在 bug、性能问题、安全漏洞的相似模式
- "这段代码是否有内存泄漏风险？" → 搜索相似模式的已知问题

### 2. 跨项目代码迁移
- 搜 "how to implement thread pool" → 找到多个高质量参考实现
- 比较不同项目的实现差异，选择最佳方案

### 3. 遗留代码理解
- 给 20 年前的 C 代码库做语义索引
- 新人用自然语言提问，无需阅读全部代码

### 4. API 使用示例搜索
- 搜 "how to use libcurl multi interface" → 找到真实代码中的使用方式
- 比文档更贴近实际使用场景

### 5. 漏洞分析
- 追踪危险函数（如 `strcpy`）的数据流
- 找到所有调用点，评估影响范围

### 6. 架构可视化
- 基于调用图自动生成架构图
- 可对接 Graphviz、D3.js 做交互式展示

### 7. 代码推荐系统
- 输入功能描述，推荐最佳实现文件
- "我想加一个连接池，应该改哪些文件？"

### 8. 知识沉淀
- 团队代码库索引后，新成员可快速查询
- "为什么这里要用红黑树而不是哈希表？" → 找到设计决策的上下文

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `analyze_repo.sh` | 代码分析主控脚本（6 步流水线） |
| `ai_code_search.sh` | AI Agent 统一搜索脚本 |
| `ai_code_search_large.sh` | 超大项目分治脚本 |
| `tools/code_indexer` | C 多进程代码索引器 |
| `tools/batch_embedder` | C 批量向量生成器 |
| `tools/vector_search` | C 语义搜索引擎 |
| `tools/call_graph` | C 调用关系分析器 |
| `tools/dataflow` | C 变量数据流追踪器 |
| `tools/word_freq` | C 词频统计器 |
| `tools/cache_import` | C KV Cache 导入工具 |
| `tools/cache_query` | C KV Cache 查询工具 |
| `models/jina-embeddings-v2-base-code/` | Jina v2 嵌入模型 |
| `ai_code_memory/` | KV Cache 数据目录 |

---

## 总结

这套系统本质上是一个**代码的「第二大脑」**：

1. **记忆**：你把代码丢进去，系统帮你建立语义记忆（向量索引 + 调用图 + 数据流）
2. **持久**：mmap 持久化保证知识永不丢失，断电也不怕
3. **回忆**：用自然语言提问，系统帮你「回忆」起相关的实现细节、调用关系和数据流路径
4. **理解**：不仅是找到代码，还能理解代码在架构中的角色、变量生命周期、影响范围

相比传统工具（grep、IDE 搜索），这套系统的核心优势是**语义理解能力**——你不需要记住函数名，只需要描述想找的功能，系统就能理解你的意图，返回最相关的代码。

**一句话描述**：这是你的私人代码知识库，一次索引，终身受用。

---

*文档版本：2025-05-21*
*适用于：analyze_repo.sh + ai_code_search.sh + ai_code_search_large.sh 最新版本*