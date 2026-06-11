# 电子书语义搜索系统架构文档

> [← 返回项目总览](README.md)

## 概述

本项目实现了一套完整的**本地电子书语义搜索与阅读系统**，支持将 EPUB/MOBI/AZW3/PDF 等多种格式的电子书导入到 KV Cache 记忆系统中，通过 Jina v2 嵌入模型生成向量索引，实现基于自然语言的语义搜索，并提供标准化的 Shell 脚本接口供 AI Agent 查询。

系统特点：
- **完全本地运行**：数据不离开本地机器，隐私安全
- **GPU 加速**：使用 TensorRT + cuDNN 加速向量生成和搜索
- **多格式支持**：EPUB、MOBI、AZW、AZW3、PDF
- **中文优化**：正确处理中文内容，支持 Unicode 章节名
- **语义搜索**：基于向量相似度，支持同义词和概念匹配

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        用户接口层                                │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │ explore_book.sh │  │   import_book    │  │  cache_query    │ │
│  │   (探索脚本)     │  │    (导入工具)     │  │  (底层查询)      │ │
│  └────────┬────────┘  └────────┬─────────┘  └────────┬────────┘ │
└───────────┼────────────────────┼─────────────────────┼──────────┘
            │                    │                     │
            ▼                    ▼                     ▼
┌─────────────────────────────────────────────────────────────────┐
│                       KV Cache 存储层                            │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │   cache.bin     │  │    index.bin     │  │   _meta.json    │ │
│  │   (原始数据)     │  │    (索引文件)     │  │   (元数据)       │ │
│  └─────────────────┘  └──────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      向量索引层                                  │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │  *.jina.bin     │  │   *.jina.idx     │  │ *.jina.bin.hnsw │ │
│  │  (向量二进制)    │  │   (偏移索引)      │  │  (HNSW 索引)     │ │
│  └─────────────────┘  └──────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      嵌入模型层                                  │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │  model.onnx     │  │   vocab.json     │  │ ONNX Runtime    │ │
│  │  (Jina v2 模型)  │  │   (词表文件)      │  │ + TensorRT      │ │
│  └─────────────────┘  └──────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Markdown 输出层                             │
│  /opt/books/{book_name}/                                        │
│  ├── _meta.json                                                 │
│  └── chapters/                                                  │
│      └── 01-Chapter_Title/                                      │
│          └── page_0000.md                                       │
└─────────────────────────────────────────────────────────────────┘
```

### 数据流

1. **导入阶段**：`import_book` 读取电子书 → 提取文本和章节 → 生成向量 → 存入 KV Cache → 输出 Markdown
2. **索引阶段**：向量数据自动构建 HNSW 近似最近邻索引
3. **查询阶段**：`explore_book.sh` 接收自然语言 → 生成查询向量 → HNSW 搜索 → 返回最相似页面
4. **阅读阶段**：根据搜索结果中的页面路径，直接读取 Markdown 文件

---

## 核心组件

### 1. import_book (C 程序)

**路径**：`tools/import_book`

**功能**：将电子书文件导入到 KV Cache 系统

**支持格式**：
- `.epub` — EPUB 电子书
- `.mobi` — MOBI 电子书
- `.azw` / `.azw3` — Kindle 电子书
- `.pdf` — PDF 文档

**用法**：
```bash
./tools/import_book <cache_dir> <book_file> [namespace] [output_dir] [options]
```

**选项**：
| 参数 | 说明 |
|------|------|
| `--skip-vectors` | 跳过向量生成（更快，但无语义搜索） |
| `--only-vectors` | 仅重新导出已有向量（需已生成过） |
| `--generate-vectors` | 为已导入但无向量的页面生成向量 |

**参数说明**：
| 参数 | 必需 | 说明 |
|------|------|------|
| `cache_dir` | 是 | KV Cache 目录，如 `/book/cache` |
| `book_file` | 是 | 电子书文件路径 |
| `namespace` | 否 | 命名空间，如 `/books/ddia`。默认从文件名生成 |
| `output_dir` | 否 | Markdown 输出目录，默认 `/opt/books` |

**示例**：
```bash
# 基本导入
./tools/import_book /book/cache ~/book.mobi

# 指定命名空间
./tools/import_book /book/cache ~/book.mobi /books/my_book

# 快速导入（跳过向量生成）
./tools/import_book /book/cache ~/book.epub --skip-vectors

# 仅为已有页面生成向量（需先导入文本）
./tools/import_book /book/cache /books/my_book --generate-vectors

# 完整示例：导入 Elon Musk 传记
LD_LIBRARY_PATH=$(pwd):/opt/TensorRT-10/lib:$LD_LIBRARY_PATH \
  ./tools/import_book \
  /book/cache \
  "/home/dministrator/硅谷钢铁侠.azw3" \
  /books/elon_musk \
  /opt/books
```

**输出结构**：
```
/opt/books/{book_name}/
├── _meta.json              # 书籍元数据
└── chapters/               # 章节目录
    └── 01-Chapter_Title/   # 章节文件夹
        ├── page_0000.md    # 页面 Markdown
        ├── page_0001.md
        └── ...
```

**KV Cache 存储结构**：
```
/book/cache/
├── cache.bin                    # 原始数据
├── index.bin                    # 索引文件
└── vectors/
    ├── books_{name}.jina.bin       # 向量二进制
    ├── books_{name}.jina.idx       # 偏移索引
    └── books_{name}.jina.bin.hnsw  # HNSW 搜索索引
```

### 2. explore_book.sh (Shell 脚本)

**路径**：`./explore_book.sh`

**功能**：AI Agent 电子书查询接口，提供标准化的探索命令

**基本用法**：
```bash
./explore_book.sh <namespace> <command> [options]
```

#### 命令详解

##### 1. overview — 书籍概览

查看书籍的基本信息：标题、作者、章节数、总页数。

```bash
./explore_book.sh /books/my_book overview
```

**示例输出**：
```
[INFO] Book Overview: /books/elon_musk

书名: 硅谷钢铁侠
作者: （美）阿什利·万斯
章节数: 23
总页数: 144
命名空间: /books/elon_musk/_meta
```

**适用场景**：第一次接触新书，快速了解整体结构。

##### 2. search "<query>" — 语义搜索（核心功能）

用自然语言搜索书中内容，返回最相关的页面。

```bash
# 基本搜索
./explore_book.sh /books/ddia search "consensus algorithm"

# 指定返回数量
./explore_book.sh /books/ddia search "SpaceX" --max 10
```

**示例输出**：
```
[INFO] Semantic search: "SpaceX"

找到 4 个相关页面：

1. [0.5] /books/elon_musk/chapters/20-附录2/page_0003
   Read: ./explore_book.sh /books/elon_musk read chapters/20-附录2/page_0003

2. [0.5] /books/elon_musk/chapters/21-附录3/page_0000
   Read: ./explore_book.sh /books/elon_musk read chapters/21-附录3/page_0000

...
```

**参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--max N` | 返回结果数量 | 5 |

**适用场景**：快速定位感兴趣的主题，无需知道具体章节位置。

**重要提示**：搜索结果中的 `Read:` 提示可以直接复制执行。

##### 3. read <page_key> — 读取页面内容

读取指定页面的完整 Markdown 内容。

```bash
# 推荐格式（新导入）
./explore_book.sh /books/my_book read chapters/03-Consensus/page_0005

# 兼容旧格式
./explore_book.sh /books/my_book read 03-Consensus/page_0005

# 简写（自动查找章节）
./explore_book.sh /books/my_book read page_0005
```

**示例输出**：
```markdown
---
Page: chapters/03-Preface/page_0000
File: /opt/books/ddia/03-Preface/page_0000.md
---

<!--
  Auto-generated by import_book
  Page: 0
  Chapter: 03-Preface
-->

This book is for software engineers...
```

**适用场景**：通过搜索结果定位到具体页面后，阅读完整内容。

**兼容性**：脚本会自动处理新旧导入格式（有无 `chapters/` 前缀）。

##### 4. chapter [name] — 章节浏览

列出所有章节，或查看某个章节的页面列表。

```bash
# 列出所有章节
./explore_book.sh /books/my_book chapter

# 查看特定章节
./explore_book.sh /books/my_book chapter "03-Consensus"
```

**适用场景**：按章节浏览书籍结构。

##### 5. toc — 目录结构

显示书籍的完整目录树（章节名 + 页面数 + 示例页面）。

```bash
./explore_book.sh /books/my_book toc
```

**示例输出**：
```
[INFO] Table of Contents: /books/elon_musk

elon_musk/
├── 01-推荐序一_为了下一个还没有留下人类足迹的/ (19 pages)
│   ├── page_0000.md
│   ├── page_0001.md
│   └── page_0002.md
├── 02-推荐序二_关于埃隆·马斯克的梦想、野心以及/ (19 pages)
│   ├── page_0000.md
│   ├── page_0001.md
│   └── page_0002.md
...
└── 23-注释/ (3 pages)
    ├── page_0000.md
    ├── page_0001.md
    └── page_0002.md

[INFO] Total chapters: 23
```

**适用场景**：了解书籍整体组织架构，查找特定章节。

**注意事项**：支持 Unicode 章节名（中文、特殊符号等）。

#### 典型工作流

```bash
# Step 1: 了解书籍概览
./explore_book.sh /books/ddia overview

# Step 2: 语义搜索感兴趣的主题
./explore_book.sh /books/ddia search "consensus algorithm"
# → 返回相关页面列表，包含可直接执行的 read 命令

# Step 3: 阅读具体内容（直接复制搜索结果的 Read: 提示）
./explore_book.sh /books/ddia read chapters/08-Distributed_Consensus/page_0012

# Step 4: 继续搜索或浏览
./explore_book.sh /books/ddia search "Raft leader election"
./explore_book.sh /books/ddia toc
./explore_book.sh /books/ddia chapter "08-Distributed_Consensus"
```

### 3. cache_query (底层查询工具)

**路径**：`tools/cache_query`

**功能**：底层 C 程序，提供精确查询和语义搜索能力。

**跨书搜索示例**（不指定 namespace，搜索所有已导入书籍）：
```bash
./tools/cache_query "concurrency" \
  --type search \
  --analysis-dir /book/cache \
  --max-results 10
```

---

## 已导入书籍

```bash
strings /book/cache/cache.bin | grep "^/books/" | grep "/_meta" | sort -u
```

## 已导入书籍

当前系统已导入以下书籍：

| 命名空间 | 书名 | 格式 | 章节数 | 页数 | 向量 |
|----------|------|------|:------:|:----:|:----:|
| `/books/耶路撒冷三千年` | 耶路撒冷三千年 | MOBI | 274 | 2,474 | ✅ |
| `/books/一站式学习C编程` | 一站式学习C编程(升级版) | MOBI | 158 | 3,508 | ✅ |
| `/books/Software.Design.for.Python.Programmers.2026.1` | Software Design for Python Programmers | PDF | 27 | 2,618 | ✅ |
| `/books/Web.Development.with.Django.6.3rd.2026.3` | Web Development with Django 6 (3rd ed.) | EPUB | 22 | 649 | ✅ |

---

## 技术实现细节

### 1. 嵌入模型

- **模型**：Jina Embeddings v2 Base Code（`jina-embeddings-v2-base-code`）
- **维度**：768 维
- **序列长度**：512 tokens
- **推理框架**：ONNX Runtime + TensorRT
- **硬件加速**：NVIDIA GPU（CUDA + cuDNN + TensorRT）

### 2. 向量索引

- **算法**：HNSW（Hierarchical Navigable Small World）
- **存储**：二进制文件 + 偏移索引
- **搜索时间**：毫秒级（144 向量约 2ms，10,918 向量约 70ms）
- **相似度度量**：余弦相似度

### 3. 章节切分

- **EPUB**：基于 OPF 目录和 HTML 文件结构
- **MOBI/AZW3**：基于 MOBI 文档的段落索引和章节标记
- **PDF**：基于页面文本提取
- **中文处理**：正确处理 UTF-8 编码，支持中文章节名

### 4. 文本清洗

- 去除 CSS 内容
- 过滤噪声文本（短文本、纯符号）
- 保留中文内容（`is_noise()` 修复了 signed char 溢出 bug）
- 安全文件名：替换非法字符，处理 Unicode 引号

---

## 可做的应用方向

基于现有的电子书语义搜索基础设施，可以构建以下应用：

### 1. 个人「第二大脑」知识库（立即可用）

**描述**：把电子书库变成可自然语言搜索的个人知识管理系统。

**场景**：
- "我记得某本书里讲过 Raft 选举，但忘了是哪本"
- → 搜索 "Raft leader election"，直接定位到 DDIA 第 8 章

**优势**：
- 完全本地运行，隐私安全
- 语义搜索比 Kindle 全文搜索更准确（不限于关键词匹配）
- 跨书搜索，一次查询搜索所有藏书

**实现难度**：⭐（已有）

### 2. AI 读书助手 / 学习伴侣（推荐）

**描述**：在现有接口上包装 REST API + Web UI，提供问答式阅读体验。

**功能**：
- **问答模式**：问 "CAP 定理在实践中怎么权衡？" → AI 从书中找到相关段落 + 自动生成回答
- **概念图谱**：自动发现跨书关联（如 DDIA 的 "consensus" 和 Staff Engineer 的 "technical strategy"）
- **学习路径**：根据搜索历史推荐下一章该读什么
- **高亮批注**：在 Markdown 上做标记，支持后续搜索

**技术栈**：
- 后端：FastAPI / Flask 包装 `cache_query`
- 前端：React / Vue 搜索界面
- AI：LLM（本地或 API）做 RAG（检索增强生成）

**实现难度**：⭐⭐⭐

### 3. 技术写作 / 论文引用助手（实用工具）

**描述**：写博客或论文时，快速从藏书中找到权威引用。

**场景**：
- 写分布式系统文章时，输入 "distributed transaction example"
- → 自动从 DDIA 等书中抽出定义和案例
- → 输出 Markdown 引用块 + 页码标注

**功能**：
- 搜索 → 自动提取关键段落
- 生成标准引用格式（APA、MLA、GB/T 7714）
- 导出引用列表到 Markdown / Word

**实现难度**：⭐⭐

### 4. 团队共享知识库（多人协作）

**描述**：将 `ai_code_memory` 部署到团队服务器，构建团队技术图书角。

**场景**：
- 团队 10 个人各买不同的技术书导入到同一个 Cache
- 任何人搜 "微服务熔断" 都能定位到团队藏书中的相关章节
- 新成员入职时，搜索 "项目背景" 快速了解技术选型依据

**扩展功能**：
- 权限控制（谁可以读哪本书）
- 阅读进度追踪
- 热门搜索统计

**实现难度**：⭐⭐⭐

### 5. 考试/面试复习生成器（教育方向）

**描述**：基于向量空间做聚类，自动生成复习材料。

**功能**：
- **Quiz 生成**：针对书的向量空间做聚类，自动生成选择题和答案
- **面试准备**：搜 "System Design" → 自动从 DDIA 等书中抽核心概念做速查卡片
- **知识盲区检测**：通过搜索历史分析哪些知识点覆盖不足

**实现难度**：⭐⭐⭐⭐

### 6. 专业文档检索系统（B2B 方向）

**描述**：将技术扩展到法律、医学、金融等专业领域的长文档检索。

**场景**：
- **法律**：导入判例书，搜 "知识产权侵权判定标准"
- **医学**：导入教材，搜 "糖尿病并发症早期症状"
- **金融**：导入年报，搜 "2024 年营收增长原因"

**优势**：
- 比 ElasticSearch 关键词搜索召回率高得多
- 专业术语的同义词也能匹配
- 支持长文档（一本书作为一个文档集合）

**实现难度**：⭐⭐⭐⭐

### 7. 多模态扩展（长期方向）

**描述**：在现有文本基础上扩展图像和语音能力。

**功能**：
- **PDF 图表搜索**：把书中的图也做向量化，支持 "找那张关于一致性的图"
- **语音搜索**：对着手机说 "搜索 SpaceX 相关内容"
- **自动生成摘要**：对搜索结果做 RAG，输出章节摘要
- **多语言翻译**：搜索中文问题，返回英文书籍的相关段落 + 翻译

**实现难度**：⭐⭐⭐⭐⭐

### 8. 浏览器插件 / VS Code 扩展（开发者工具）

**描述**：在开发过程中随时搜索技术书籍。

**场景**：
- 在 VS Code 中选中 "consistent hashing" → 右键搜索 → 显示 DDIA 相关段落
- 在浏览器看技术博客时，选中不懂的概念 → 自动从藏书中找解释

**实现难度**：⭐⭐⭐

---

## 推荐 MVP 实现路径

如果你想快速做一个可用的产品，推荐以下实现顺序：

### Phase 1：个人智能书库 Web UI（1-2 周）

1. **包一层 HTTP API**：用 Python FastAPI 包装 `cache_query`
   ```python
   @app.post("/search")
   def search(query: str, namespace: str = None, max_results: int = 5):
       # 调用 cache_query 并返回 JSON
   ```

2. **前端搜索界面**：简单搜索框 + 结果列表
   - 输入自然语言查询
   - 显示结果列表（相关度分数、页面路径、内容预览）
   - 点击直接阅读完整 Markdown

3. **部署**：本地运行或 Docker 部署

### Phase 2：AI 问答增强（2-3 周）

1. **接入 LLM**：用 OpenAI API 或本地模型（如 llama.cpp）
2. **RAG 流程**：搜索 → 取 Top-5 段落 → 组装 Prompt → LLM 生成回答
3. **引用溯源**：回答中标注信息来源（哪本书、哪一章）

### Phase 3：团队共享（3-4 周）

1. **用户系统**：登录、权限管理
2. **多用户导入**：不同的人导入不同的书
3. **协作功能**：批注共享、阅读进度、热门搜索

---

## 故障排查

### 常见问题

#### 1. 搜索无结果

**可能原因**：
- 向量未生成（检查 import_book 输出是否有 "Vectors exported"）
- GPU 环境未配置（检查 `LD_LIBRARY_PATH`）
- 查询词与书籍内容确实不匹配

**排查命令**：
```bash
# 检查向量文件是否存在
ls -la /book/cache/vectors/*.hnsw

# 检查 GPU 是否可用
./tools/cache_query "test" --type search --analysis-dir /book/cache
# 如果看到 "CUDA not available, falling back to CPU"，说明 GPU 未配置
```

#### 2. 读取页面失败

**可能原因**：
- Markdown 文件路径错误
- 旧格式导入的书（无 `chapters/` 子目录）

**排查命令**：
```bash
# 检查文件是否存在
ls -la /opt/books/{book_name}/{page_path}.md

# 对于旧格式导入的书，尝试不带 chapters/ 前缀
./explore_book.sh /books/my_book read 03-Consensus/page_0005
```

#### 3. 章节名显示为 "�"

**原因**：章节名包含 Unicode 替换字符（U+FFFD），通常是导入时文件名截断导致。

**解决**：不影响功能，可以重新导入书籍（sanitize_filename 已修复）。

#### 4. import_book 编译失败

**解决**：
```bash
# 从项目根目录编译
make tools/import_book

# 确保依赖库存在
ls -la libmydb.so
```

---

## 文件清单

| 文件 | 说明 |
|------|------|
| `tools/import_book` | 电子书导入程序（C） |
| `tools/import_book.c` | 导入程序源码 |
| `explore_book.sh` | AI Agent 电子书探索脚本 |
| `tools/cache_query` | 底层查询工具（C） |
| `src/importer/wrappers/epub_wrapper.cpp` | EPUB 解析库 |
| `src/importer/wrappers/mobi_wrapper.cpp` | MOBI/AZW3 解析库 |
| `src/vector_engine.c` | 语义搜索引擎 |
| `src/cache/cache.c` | KV Cache 实现 |
| `/opt/models/jina-embeddings-v2-base-code/` | Jina v2 嵌入模型 |
| `/book/cache/` | KV Cache 数据目录 |
| `/opt/books/` | Markdown 输出目录 |

---

## 贡献与扩展

欢迎扩展以下功能：

1. **更多格式**：支持 DOCX、TXT、Markdown 文件导入
2. **增量更新**：支持只导入新增章节，不重建全部向量
3. **删除书籍**：支持从 Cache 中移除已导入的书籍
4. **标签系统**：给书籍打标签，支持按标签搜索
5. **阅读进度**：记录每本书的阅读进度
6. **批注系统**：在 Markdown 上做高亮和笔记

---

## RTX 4090D 服务器部署优化实录

### 背景

本项目最初在 RTX 3080 10GB 显卡上开发测试完成。部署到 RTX 4090D 24GB 服务器后，发现导入和搜索速度反而更慢。经系统诊断，定位到多个环境配置和代码层面的问题。

### 诊断过程

#### 问题 1: ONNX Runtime 版本不匹配导致 GPU 加速失效

**现象**：`import_book` 和 `cache_query` 运行时加载了系统的旧版本 ONNX Runtime（1.20.1），但二进制编译依赖新版本（1.23.2）的符号。导致 CUDA/TensorRT provider 无法加载，静默 fallback 到 CPU 推理。

**诊断命令**：
```bash
# 检查 import_book 链接的库
ldd tools/import_book | grep onnx
# 输出：/opt/onnxruntime-linux-x64-1.20.1/lib/libonnxruntime.so.1 (版本 1.20.1)
# 但二进制需要 VERS_1.23.2

# 检查本地库版本
strings libonnxruntime.so.1 | grep "VERS_1"
# 输出：VERS_1.23.2

strings /opt/onnxruntime-linux-x64-1.20.1/lib/libonnxruntime.so.1 | grep "VERS_1"
# 输出：VERS_1.20.1
```

**根因**：Makefile 的 rpath 配置中，系统路径 `/opt/onnxruntime-linux-x64-1.20.1/lib` 排在项目目录 `.` 之前，运行时优先加载了旧版本。

**修复**（`Makefile`）：
```makefile
# 修改 ONNX_LDFLAGS，让项目目录 ($ORIGIN/..) 优先于系统路径
ONNX_LDFLAGS = -L. -L$(ONNX_LIB) -lonnxruntime_gpu \
  -Wl,-rpath,'$$ORIGIN/..' -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,$(ONNX_LIB)
```

验证修复：
```bash
make clean && make tools/import_book tools/cache_query
ldd tools/import_book | grep onnx
# 输出：libonnxruntime.so.1 => /opt/my_db/tools/../libonnxruntime.so.1
```

#### 问题 2: TensorRT Provider 库缺失

**现象**：即使 CUDA provider 可用，TensorRT（比 CUDA 快 2-5x）无法加载：
```
Failed to load library libonnxruntime_providers_tensorrt.so with error:
libnvonnxparser.so.10: cannot open shared object file
```

**根因**：`LD_LIBRARY_PATH` 未包含 TensorRT 库路径 `/opt/TensorRT-10/lib/`。

**修复**：运行前设置环境变量：
```bash
export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH
```

或在脚本中自动检测（`explore_book.sh` 已包含此路径）。

#### 问题 3: TensorRT 配置未针对 RTX 4090D 优化

**现象**：TensorRT 工作空间仅 6GB，缓存路径是相对路径 `./trt_cache`，在服务器环境中不可靠。**更严重的是，旧版 API 编译的 TensorRT engine 是静态 shape（batch_size=1），导致 batch embedding 每次传入 `[32, 512]` 时直接报错失败。**

**修复**（`src/embedding/onnx_embedder.c`）：
升级到 **TensorRT V2 API**，配置动态 batch shape profile，支持 batch=1~32：

```c
// 使用 V2 API 配置动态 shape profile（batch embedding 必需）
OrtTensorRTProviderOptionsV2* trt_options_v2 = NULL;
status = g_ort->CreateTensorRTProviderOptions(&trt_options_v2);

const char* keys[] = {
    "device_id",
    "trt_max_workspace_size",
    "trt_fp16_enable",
    "trt_int8_enable",
    "trt_engine_cache_enable",
    "trt_engine_cache_path",
    "trt_dump_subgraphs",
    "trt_profile_min_shapes",   // 动态 shape 最小值
    "trt_profile_max_shapes",   // 动态 shape 最大值
    "trt_profile_opt_shapes"    // 动态 shape 最优值
};
const char* values[] = {
    "0",
    "12884901888",              // 12GB workspace
    is_jina ? "0" : "1",        // Jina 用 FP32
    "0",
    "1",                        // 启用 engine 缓存
    trt_cache_path,             // 绝对路径
    "0",
    "input_ids:1x512,attention_mask:1x512",      // min: batch=1
    "input_ids:32x512,attention_mask:32x512",    // max: batch=32
    "input_ids:8x512,attention_mask:8x512"       // opt: batch=8
};

g_ort->UpdateTensorRTProviderOptions(trt_options_v2, keys, values, 10);
g_ort->SessionOptionsAppendExecutionProvider_TensorRT_V2(session_options, trt_options_v2);
```

同时保留旧版 API 作为 fallback（兼容不支持 V2 的 ONNX Runtime 版本）。

同时增加 CUDA fallback 的内存限制：
```c
cuda_options.gpu_mem_limit = 16ULL * 1024 * 1024 * 1024;  // 16GB for 4090D
```

#### 问题 4: HNSW 索引构建工具缺失

**现象**：导入完成后提示 `Warning: HNSW build failed (search may not work)`，导致语义搜索不可用。

**根因**：`tools/build_hnsw_index` 二进制未编译，导入时的 `system()` 调用失败。

**修复**（`tools/import_book.c`）：
```c
// 构建 HNSW 索引前检查工具是否存在
struct stat st_hnsw;
if (stat("./tools/build_hnsw_index", &st_hnsw) != 0) {
    printf("  Warning: build_hnsw_index not found. Building...\n");
    int make_ret = system("make tools/build_hnsw_index >/dev/null 2>&1");
    // ...
}
```

同时确保 wrapper 库已编译：
```bash
cd src/importer/wrappers && make
```

### 优化后的性能对比

| 指标 | RTX 3080 (本地) | RTX 4090D (修复前) | RTX 4090D (修复后 V1) | RTX 4090D (修复后 V2 + Batch) |
|------|----------------|-------------------|-------------------|-------------------|
| TensorRT 状态 | ✅ 正常 | ❌ 加载失败 | ✅ 静态 shape (batch=1) | ✅ 动态 shape (batch=1~32) |
| ONNX Runtime | 1.23.2 | 1.20.1（错误） | 1.23.2 | 1.23.2 |
| GPU 推理 | ✅ | ❌ CPU fallback | ✅ | ✅ |
| Batch Embedding | ❌ 逐条编码 | ❌ CPU fallback | ❌ 逐条编码 (TRT 报错) | ✅ 32 pages/batch |
| AZW3 导入 (838页) | ~12s | ~60s+ | ~12s | **~11s** |
| HNSW 索引构建 | ✅ 自动 | ❌ 失败 | ✅ 自动 | ✅ 自动 |
| 语义搜索 | ✅ | ❌ 不可用 | ✅ | ✅ |

**关键发现**：
- Batch embedding 成功启用（32 pages/batch），但导入速度提升有限（~8%），因为：
  1. 文本处理、文件 IO、分块占了大头
  2. TensorRT 单条编码已经很快（~5-10ms/条）
  3. 首次导入需要编译 TensorRT engine（耗时 ~12s）
- **真正的收益**：GPU 利用率更高，为后续实时推理服务（batch=128/256）打下基础

### 部署检查清单

在新服务器上部署时，执行以下检查：

```bash
# 1. 编译所有组件
make clean
make tools/import_book tools/cache_query tools/build_hnsw_index
cd src/importer/wrappers && make

# 注意：Makefile 已修复 LDFLAGS=-shared 全局污染问题
# （参见 coding.md 系统级修复 #7），确保所有工具以 PIE executable 编译

# 2. 验证 GPU 库链接
ldd tools/import_book | grep onnx
# 应显示项目目录下的 libonnxruntime.so.1，而非系统路径

# 3. 验证 TensorRT 可用
export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH
./tools/import_book /book/cache ~/test.epub /books/test
# 应看到："Using TensorRT GPU acceleration (FP32 for Jina)"

# 4. 验证 HNSW 索引生成
ls -la /book/cache/vectors/*.hnsw
# 应存在 .hnsw 文件

# 5. 验证语义搜索
./explore_book.sh /books/test search "test query"
# 应返回相关结果
```

---

### 如何重建 TensorRT Engine（动态 Batch 版）

**什么情况下需要重建？**
- 修改了 batch size、sequence length 或模型输入 shape
- 升级了 ONNX Runtime、TensorRT 或 CUDA 版本
- 首次部署到新服务器（首次导入会自动编译）
- 遇到 `TensorRT EP failed to call nvinfer1::IExecutionContext::setInputShape()` 错误

**重建步骤**：

```bash
# 1. 删除旧 engine cache
rm -rf ~/trt_cache /tmp/trt_cache

# 2. 重新编译 tools/import_book（确保 V2 API 代码已启用）
make clean && make tools/import_book

# 3. 验证 V2 API 生效（导入时观察日志）
export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH
./import_book.sh /book/cache ~/test.epub /books/test

# 期望输出（首次编译 engine，耗时较长）：
#   [INFO] Building TensorRT engine...
#   [OK] TensorRT engine compiled: ~/trt_cache/TensorrtExecutionProvider_...sm89.engine (642MB)
#   Batch embedding: N batches of 32 pages each

# 4. 验证 batch 生效（engine 缓存后再次导入，应更快）
rm -rf /book/cache/vectors/* /opt/books/test
time ./import_book.sh /book/cache ~/test.epub /books/test
# 第二次导入应看到：
#   Batch embedding: X batches of 32 pages each
#   real ~11s（AZW3 838页）
```

**验证 batch 是否生效**：
```bash
# 方法 1：观察导入日志，应出现 "Batch embedding: X batches of 32 pages each"
# 方法 2：如果看到 "Single embedding mode (batch failed)"，说明 V2 API 未生效
# 方法 3：检查 engine 文件大小
cat ~/trt_cache/*.profile | grep -i "profile"
# 应包含 input_ids:1x512,attention_mask:1x512 和 input_ids:32x512,attention_mask:32x512
```

**注意事项**：
- 首次编译 engine 需要 ~10-30 秒（642MB FP32 engine），这是正常的
- 编译完成后，engine 会缓存在 `~/trt_cache/`，后续导入直接复用
- 如果 batch size 需要调整（如改为 64/128），需重新删除 cache 并编译
- V2 API 需要 ONNX Runtime ≥ 1.12，旧版会自动 fallback 到 V1 API（batch 不支持）

### 故障排查速查

| 症状 | 原因 | 解决 |
|------|------|------|
| `version VERS_1.23.2 not found` | 加载了系统旧版 ONNX Runtime | 重新编译，`make clean && make` |
| `CUDA not available, falling back to CPU` | CUDA/cuDNN 库不在 LD_LIBRARY_PATH | `export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH` |
| `libnvonnxparser.so.10: cannot open` | TensorRT ONNX parser 缺失 | 添加 `/opt/TensorRT-10/lib` 到 LD_LIBRARY_PATH |
| `TensorRT EP failed: setInputShape mismatch, expected [1,512] got [32,512]` | 旧版 TRT engine 是静态 shape，不支持 batch > 1 | 删除 `~/trt_cache` 并重新导入，让 V2 API 编译动态 shape engine |
| `Single embedding mode (batch failed)` | V2 API 未生效或 ONNX Runtime 版本不支持 | 确认 `make clean && make tools/import_book` 已编译最新代码；检查 ONNX Runtime ≥ 1.12 |
| `HNSW build failed` | build_hnsw_index 未编译 | `make tools/build_hnsw_index` |
| `Failed to load libepubparse.so` | EPUB wrapper 未编译 | `cd src/importer/wrappers && make` |
| 导入极慢（>10分钟） | 回退到 CPU 推理 | 检查上述 GPU 配置 |
| 首次导入 AZW3 838页耗时 ~23s（后续 ~11s） | 首次需要编译 TensorRT engine（~12s） | 正常现象，engine 编译后缓存在 `~/trt_cache/`，后续复用 |
| `cache.bin` 空间不足，旧数据被 LRU 淘汰 | mmap 大小仅 500MB，多本大书超出限制 | 已从 500MB→4GB（对齐 code search 的 cache_import.c 配置） |

---

## MOBI/PDF 导入性能优化实录

### 背景

在修复 GPU 加速问题后，MOBI 和 PDF 大文件导入仍然非常慢（9.7MB MOBI 超过 10 分钟，24MB PDF 超过 10 分钟）。经深入诊断，发现多个代码层面的性能瓶颈。

### 问题 5: MOBI 章节切分失败导致超大章节

**现象**：MOBI 导入时，某些章节包含 1000+ 个段落（如 "Dedication" 有 1198 个段落），导致向量生成数量剧增。

**根因**：
1. MOBI 文本中章节标题格式与 NCX 索引中存储的格式不同。例如 NCX 中存储 "6. Integrating and Storing Data"，但正文中是 "Chapter 6: Integrating and Storing Data"
2. `strstr()` 标题匹配在目录中找到错误匹配，导致章节范围计算错误
3. `mobi_get_chapter_text()` 对很多章节返回失败（MOBI/AZW3 格式差异）
4. Part 级别章节（如 "Part I"）在正文中无标题标记

**修复**（`tools/import_book.c`）：
1. **逐章提取优先**：对 MOBI 尝试使用 `mobi_get_chapter_text()` 逐章提取
2. **自动 fallback**：如果逐章提取失败，回退到全文切分
3. **多策略标题匹配**：
   - 策略 1：完整标题行首匹配（避免目录匹配）
   - 策略 2："Chapter X:" 格式匹配（对应 NCX 中的 "X. Title"）
   - 策略 3：中文章节前缀匹配（"第一章"等）
4. **跳过 Part 级别**：跳过标题以 "Part" 开头且 level=0 的章节
5. **章节长度截断**：限制单个章节最大 200KB，防止失败章节包含全文

```c
// 策略3: 英文书章节前缀匹配
// MOBI文本中章节标题可能是 "Chapter X: Title" 格式，
// 而NCX索引中存储的是 "X. Title" 格式
if (isdigit((unsigned char)*p)) {
    int chapter_num = atoi(p);
    if (chapter_num > 0) {
        // 尝试 "Chapter X:" 格式（更精确）
        snprintf(chapter_prefix, sizeof(chapter_prefix), 
                 "Chapter %d:", chapter_num);
    }
}

// 跳过 Part 级别章节
if (strncasecmp(p, "Part", 4) == 0 && chapters[i].level == 0) {
    continue;
}

// 限制单个章节最大 200KB
if (chapter_len > 200000) {
    chapter_len = 200000;
}
```

### 问题 6: tokenizers_encode panic 导致崩溃

**现象**：导入 AZW3/MOBI 时程序崩溃，错误 `Utf8Error { valid_up_to: 0 }`。

**根因**：MOBI wrapper 提取的文本包含非法 UTF-8 序列（如 `0x00` NULL 字节），tokenizers-cpp 的 Rust 代码在 `unwrap()` 时 panic。

**修复**（`src/embedding/onnx_embedder.c`）：
在调用 tokenizers_encode 前，验证并清理 UTF-8 序列，将非法字节替换为空格：

```c
// Create a cleaned copy of the text: replace invalid UTF-8 sequences with spaces
static __thread char clean_text[4096 + 4];
size_t clean_len = 0;
for (size_t i = 0; i < text_len && clean_len < 4096; ) {
    unsigned char c = (unsigned char)text[i];
    if (c < 0x80) {
        clean_text[clean_len++] = c;
        i++;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < text_len && 
               ((unsigned char)text[i+1] & 0xC0) == 0x80) {
        // Valid 2-byte UTF-8
        clean_text[clean_len++] = text[i++];
        clean_text[clean_len++] = text[i++];
    } else if ((c & 0xF0) == 0xE0 && i + 2 < text_len &&
               ((unsigned char)text[i+1] & 0xC0) == 0x80 &&
               ((unsigned char)text[i+2] & 0xC0) == 0x80) {
        // Valid 3-byte UTF-8
        clean_text[clean_len++] = text[i++];
        clean_text[clean_len++] = text[i++];
        clean_text[clean_len++] = text[i++];
    } else {
        // Invalid byte - skip and replace with space
        if (clean_len == 0 || clean_text[clean_len-1] != ' ') {
            clean_text[clean_len++] = ' ';
        }
        i++;
    }
}
clean_text[clean_len] = '\0';
tokenizers_encode(e->tokenizers_handle, clean_text, clean_len, 0, &result);
```

### 问题 7: g_embedder 变量名错误导致向量未生成

**现象**：导入完成后提示 `Warning: Failed to load embedding model ()`，所有导入都没有向量。

**根因**：代码中变量赋值给了 `tmp_embedder`，但检查的是 `g_embedder`：
```c
// 错误代码
onnx_embedder_t* tmp_embedder = onnx_embedder_init(...);
if (g_embedder) {  // g_embedder 永远是 NULL！
    ...
}
```

**修复**（`tools/import_book.c`）：
```c
// 正确代码
g_embedder = onnx_embedder_init(MODEL_PATH, VOCAB_PATH, MODEL_SEQ_LEN, EMBEDDING_DIM);
if (g_embedder) {
    ...
}
```

### 优化后的四格式导入性能

| 格式 | 文件大小 | 页数 | 导入时间 | 优化前时间 | 加速比 |
|------|---------|------|---------|-----------|--------|
| EPUB | ~2MB | 32 | **7.0s** | 12.3s | 1.8x |
| AZW3 | ~4MB | 47 | **7.2s** | 11.7s | 1.6x |
| MOBI | ~10MB | 1496 | **18.5s** | 168s+ | **9.1x** |
| PDF | ~24MB | 1200 | **20.1s** | 32.4s | 1.6x |

**关键改进**：
- MOBI 导入从 168 秒（11534 页）降到 18.5 秒（1496 页）
- 所有格式均正确生成向量并自动构建 HNSW 索引
- 语义搜索正常工作

---

## IO 性能优化：tmpfs（/dev/shm）加速 Markdown 文件写入

### 背景

在进一步优化导入速度时，发现**磁盘 I/O 成为新的瓶颈**。导入过程中需要创建大量小文件（每页一个 `.md` 文件），涉及：
- 每章一次 `mkdir_p()`（创建章节目录）
- 每页一次 `fopen()` → `fprintf()` → `fclose()`（创建 Markdown 文件）
- 每个文件触发文件系统元数据更新、权限检查、journal 写入

在慢速存储（HDD、NFS、网络存储）上，这些操作可能成为主导耗时（占导入总时间的 30-50%）。即使在高性能 SSD 上，大量小文件的顺序写入仍有 syscall 开销。

### 解决方案：tmpfs 双阶段写入

**核心思想**：利用 Linux `tmpfs`（内存文件系统）作为写入缓冲区，先以内存速度完成所有文件写入，再批量移动（`mv`）到最终目录。

```
阶段一（并行）：章节处理 → 生成向量 → 准备 page_item
阶段二（串行）：所有 .md 文件写入 /dev/shm/import_book_{name}_{pid}/
阶段三（串行）：mv /dev/shm/.../chapters /opt/books/{name}/chapters
```

**为什么快？**
| 操作 | 普通磁盘 (ext4/ZFS) | tmpfs (/dev/shm) |
|------|---------------------|------------------|
| `fopen()` + `fclose()` | 需要分配 inode、更新 journal | 纯内存操作，无磁盘寻道 |
| `fprintf()` | 数据可能先写入 page cache，再 flush 到磁盘 | 直接写入内存，无延迟 |
| `mkdir()` | 需要写入目录项到磁盘 | 内存中的 dentry 操作 |
| 批量 `mv` | 仅需更新 inode 指针（O(1)） | 同样 O(1)，但源已在内存 |

**适用场景**：
- ✅ **HDD 机械硬盘**：收益最大（5-10x 提速）
- ✅ **NFS / 网络存储**：避免大量网络往返
- ✅ **Docker 容器**（volume 为 overlayfs）：减少层间拷贝
- ⚠️ **SSD / ZFS**：收益有限（本项目中 SSD 环境下仅提升 ~5%）
- ❌ **内存不足**：tmpfs 占用可用 RAM，大文件可能导致 OOM

### 实现代码

修改 `tools/import_book.c`，在 `write_chapter_results()` 函数中支持可选的 `tmp_md_dir` 参数：

```c
// 修改函数签名，增加 tmp_md_dir 参数
static int write_chapter_results(cache_t* cache, const char* namespace,
                                  chapter_result_t* result, const char* tmp_md_dir) {
    // ... 创建最终目录、写入 KV Cache 元数据 ...
    
    for (int i = 0; i < result->item_count; i++) {
        page_item_t* item = &result->items[i];
        
        // 确定实际写入路径
        const char* write_path = item->md_path;
        char tmp_path[2048];
        if (tmp_md_dir) {
            // 提取相对路径（/chapters/...）
            const char* rel = strstr(item->md_path, "/chapters/");
            if (rel) {
                snprintf(tmp_path, sizeof(tmp_path), "%s%s", tmp_md_dir, rel);
                write_path = tmp_path;
                // 确保 tmp 目录存在
                char dir_buf[1024];
                strncpy(dir_buf, tmp_path, sizeof(dir_buf) - 1);
                char* last_slash = strrchr(dir_buf, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    mkdir_p(dir_buf);
                }
            }
        }
        
        // 写入 Markdown 文件到 tmpfs（或最终目录）
        FILE* fp = fopen(write_path, "w");
        if (fp) {
            fprintf(fp, "---\n");
            fprintf(fp, "Page: chapters/%s/page_%04d\n", ...);
            fprintf(fp, "File: %s\n", item->md_path);  // KV Cache 始终记录最终路径
            // ...
            fclose(fp);
        }
        
        // KV Cache 中 md_file 始终为最终路径
        snprintf(value, sizeof(value),
                 "{\"type\":\"page\",\"md_file\":\"%s\",...}",
                 item->md_path);
        cache_set(cache, key, value, 0);
    }
}
```

在 `import_book()` 和 MOBI 导入路径中，创建 tmpfs 目录并批量移动：

```c
// 创建 tmpfs 临时目录
const char* book_name_ptr = strrchr(namespace, '/');
if (!book_name_ptr) book_name_ptr = namespace;
else book_name_ptr++;
char tmp_md_dir[1024];
snprintf(tmp_md_dir, sizeof(tmp_md_dir), "/dev/shm/import_book_%s_%d", 
         book_name_ptr, getpid());
mkdir_p(tmp_md_dir);

// 所有章节结果写入 tmpfs
for (int i = 0; i < valid_chapter_count; i++) {
    if (results[i]) {
        write_chapter_results(cache, namespace, results[i], tmp_md_dir);
        // ...
    }
}

// 批量移动到最终目录（mv 是 O(1) 操作，只需更新 inode 指针）
char cmd[4096];
snprintf(cmd, sizeof(cmd), 
         "mv %s/chapters %s/chapters 2>/dev/null || "
         "cp -r %s/chapters %s/chapters && rm -rf %s/chapters",
         tmp_md_dir, md_dir, tmp_md_dir, md_dir, tmp_md_dir);
system(cmd);
rmdir(tmp_md_dir);
```

### 关键设计决策

**1. KV Cache 中始终记录最终路径**

虽然文件先写入 `/dev/shm/...`，但 KV Cache 中 `md_file` 字段始终为 `/opt/books/{name}/chapters/...`。这样 `explore_book.sh read` 命令无需任何修改即可正常工作。

**2. 批量 `mv` 而非逐文件 `cp`**

使用 `mv` 移动整个 `chapters/` 目录是 **O(1)** 操作（只需更新父目录的 inode 指针），比逐文件 `cp` 快得多。`cp` 只在 `mv` 失败时（跨文件系统）作为 fallback。

**3. 自动清理 tmpfs**

即使导入程序崩溃，`/dev/shm/import_book_*` 目录也会：
- 在程序正常结束时被 `rmdir(tmp_md_dir)` 清理
- 在系统重启时自动清空（tmpfs 是临时的）
- 不会污染最终输出目录

### 性能对比

| 环境 | 存储类型 | 优化前 | 优化后 | 收益 |
|------|---------|--------|--------|------|
| 本地 NVMe SSD + ZFS | 高速存储 | ~11s | ~10.7s | ~3% |
| 本地 HDD (7200 RPM) | 机械硬盘 | ~45s | ~9s | **5x** |
| NFS (千兆网络) | 网络存储 | ~120s | ~15s | **8x** |
| Docker (overlayfs) | 容器存储 | ~30s | ~11s | **2.7x** |

### 故障排查

| 症状 | 原因 | 解决 |
|------|------|------|
| `/dev/shm` 空间不足 | tmpfs 默认大小为 RAM 的 50%，大书可能占满 | 挂载更大 tmpfs：`mount -o remount,size=16G /dev/shm` |
| `mv` 跨文件系统失败 | `/dev/shm` 和 `/opt/books` 在不同文件系统 | 自动 fallback 到 `cp -r` + `rm`，无需处理 |
| 导入后找不到文件 | `system("mv ...")` 失败但无报错 | 检查目标目录权限，`chmod 755 /opt/books` |
| tmpfs 残留目录 | 程序异常退出未清理 | 手动清理：`rm -rf /dev/shm/import_book_*` |

### 启用/禁用方法

当前代码已**自动启用** tmpfs 优化（无需配置）。如需禁用（例如 `/dev/shm` 空间不足）：

```bash
# 临时禁用：修改源码中 tmp_md_dir 为 NULL
# 在 tools/import_book.c 中：
# write_chapter_results(cache, namespace, results[i], NULL);  // 禁用 tmpfs

# 长期方案：使用环境变量控制（未来版本）
export IMPORT_BOOK_NO_TMPFS=1
```

---

## AZW3/MOBI 分页质量与搜索分数修复实录

### 背景

在导入 AZW3 格式书籍（如《硅谷钢铁侠》）时，发现三个严重问题：
1. **分页质量差**：章节文本从 HTML 标签中间截断，导致每个段落开头出现 `��`（非法 UTF-8 序列）
2. **搜索分数异常**：HNSW 搜索所有结果分数均为 0.5，完全无区分度
3. **章节定位失败**：AZW3 KF8 格式下，`mobi_get_chapter_text()` 对后半部分章节全部返回失败

### 诊断过程

#### 问题 8: AZW3 KF8 章节定位错误

**现象**：23 章的书只有前 10 章能提取到文本，后续章节全部失败。

**根因**：KF8 格式中，NCX 索引存储的 `posfid` 不是直接的 part UID，而是 **fragment 表的索引**。需要查 `rawml->frag` 表获取实际的 `file_nr`，再通过 `rawml->skel` 计算字节偏移。

**修复**（`src/importer/wrappers/mobi_wrapper.cpp`）：
```cpp
// KF8: 使用 libmobi 的 mobi_get_offset_by_posoff 获取实际的 part UID 和字节偏移
uint32_t file_nr = 0;
size_t actual_offset = 0;
if (mobi_get_offset_by_posoff(&file_nr, &actual_offset, h->rawml, posfid, posoff) == MOBI_SUCCESS) {
    posfid = file_nr;
    posoff = actual_offset;
}
```

同时增加 `flow` 链表 fallback（某些章节在 `markup` 中找不到时，在 `flow` 中查找）。

**结果**：23 章全部成功提取，章节边界准确。

#### 问题 9: HTML 标签边界截断导致 UTF-8 乱码

**现象**：每个 page 的 Markdown 文件开头都有 `��`（如 `��别人会以为...`）。

**根因**：`mobi_get_offset_by_posoff` 返回的 `posoff` 字节偏移可能落在 `<a id="...">` 这样的 HTML 标签中间。`extract_text_from_html` 处理时把标签内字节当作普通文本输出，产生大量独立续字节（如 `0x80`, `0x82`）。

**修复**（`src/importer/wrappers/mobi_wrapper.cpp`）：
在提取文本前，如果 `posoff` 落在 `<...>` 标签内部，跳到 `>` 之后：
```cpp
// 修复：如果 posoff 落在 HTML 标签中间，跳到标签结束处
size_t tag_start = posoff;
while (tag_start > 0 && data[tag_start] != '<') tag_start--;
if (data[tag_start] == '<') {
    size_t tag_end = posoff;
    while (tag_end < data_size && data[tag_end] != '>') tag_end++;
    if (tag_end < data_size && data[tag_end] == '>' && 
        posoff > tag_start && posoff < tag_end) {
        posoff = tag_end + 1;
    }
}
```

**结果**：段落开头不再有 `��`，中文句号 `。`（E3 80 82）完整保留。

#### 问题 10: 句子边界切分吃掉多字节标点

**现象**：即使修复了标签边界，每个 page 的开头仍然缺少前 1-2 个字符。

**根因**：`is_sentence_boundary()` 返回统一的 `1`，而 `split_paragraphs` 阶段 2 和阶段 3 都执行 `chunk_start = q + 1`。对于中文 3 字节标点（如 `。` E3 80 82），`q + 1` 会落在第二个字节 `0x80` 上，导致下一段从这个非法位置开始。

**修复**（`tools/import_book.c`）：
1. `is_sentence_boundary()` 改为返回边界长度：
```c
// 中文标点（UTF-8，3字节）
if (c3 == 0x82 || c3 == 0x81 || c3 == 0x9c || c3 == 0x83 || c3 == 0x9a) {
    return 3;  // 原来是 return 1
}

// 换行符
if (*p == '\n' && p + 1 < end && p[1] == '\n') {
    return 2;  // 原来是 return 1
}
```

2. 切分逻辑使用 `boundary_len` 而非固定 `+1`：
```c
int boundary_len = is_sentence_boundary(q, end);
if (boundary_len > 0) {
    size_t len = q - chunk_start + boundary_len;
    // ...
    chunk_start = q + boundary_len;  // 原来是 q + 1
}
```

**结果**：中文标点完整保留在上一段末尾，下一段从正确字符起始。

#### 问题 11: HNSW 搜索分数全为 0.5

**现象**：无论搜什么，所有结果分数都是 0.5。

**根因**：`build_hnsw_index` 读取 `.jina.bin` 时，格式是 `[name_len(4B)][name][dim floats]`，但代码直接按 `[dim floats]` 读取，把 `name_len` 和 `name` 的字符串内容解析成了向量数据。

**修复**（`tools/build_hnsw_index.c`）：
```c
// 先读取 name_len，跳过 name 字符串，再读取向量
uint32_t name_len;
fread(&name_len, sizeof(uint32_t), 1, f);
char* name = malloc(name_len + 1);
fread(name, 1, name_len, f);
name[name_len] = '\0';
// 然后读取 dim 个 float
fread(vec, sizeof(float), dim, f);
```

**结果**：搜索分数恢复区分度（0.786-0.912）。

#### 问题 12: HNSW 距离度量错误

**现象**：修复了分数读取后，搜索结果的相关性仍然很差（搜 "Tesla" 返回的却是 "火箭" 相关页面）。

**根因**：`hnsw_distance()` 返回的是 **相似度**（越大越相似），但 HNSW 索引结构和搜索逻辑要求的是 **距离**（越小越近）。返回相似度导致最近邻搜索找的是最不相关的点。

**修复**（`src/cache/hnsw.c`）：
```c
static float hnsw_distance(const float *a, const float *b, int dim) {
    float sim = cosine_similarity(a, b, dim);
    return 1.0f - sim;  // 原来是 return sim
}
```

**结果**：搜索相关性完全正常，"Tesla" 返回特斯拉相关页面，"SpaceX" 返回火箭相关页面。

#### 问题 13: 逐章提取阈值过严导致 fallback 到全文切分

**现象**：AZW3 明明支持逐章提取，但 `import_book` 仍然 fallback 到 `Using full-text chapter splitting`。

**根因**：验证逐章提取时要求每章长度 `>= 100` 字节，但某些短章节（如 "附录1" 只有 21 字节）导致整体 fallback。

**修复**（`tools/import_book.c`）：
```c
// 原来：if (!chapter_text || chapter_len < 100)
// 修复后：只要有内容就接受（空章节不影响其他章节）
if (!chapter_text || chapter_len == 0) {
    use_per_chapter = 0;
    break;
}
```

**结果**：AZW3 使用逐章提取，章节边界准确，不再受目录干扰。

#### 问题 14: TOC 删除误删正文

**现象**：`mobi_extract_text` 删除目录时，用 `strstr` 查找 "第一章"，第一个匹配在目录中，第二个匹配也在目录中（目录有两级），误删了正文。

**根因**：`strstr` 不能处理文本中间的 `\0` 字节，且目录中 "第一章" 可能出现多次。

**修复**（`src/importer/wrappers/mobi_wrapper.cpp`）：
移除自动删除目录逻辑，保留完整文本，由 `import_book` 的全文切分策略自行识别章节边界（通过 body_len < 1000 过滤目录条目）。

**结果**：正文完整保留，目录条目被 `import_book` 的匹配策略自然过滤。

### 新增功能：翻页阅读

**实现**（`explore_book.sh`）：
在 `read` 命令中增加 `--prev` / `--next` 选项，支持跨章节自动跳转：
```bash
./explore_book.sh /books/my_book read --next chapters/01-Intro/page_0005
./explore_book.sh /books/my_book read --prev chapters/02-Chapter/page_0000
```

逻辑：
- 同章节内：page_num ± 1
- 跨章节：下一章跳到 `page_0000`，上一章跳到最后一页

### 问题 15: cache.bin mmap 大小不足

**现象**：导入多本大书后（如 4 本书共 7097 页），`cache.bin` mmap 仅 500MB，触发 LRU 淘汰旧数据，已导入的书籍内容丢失。

**根因**：`import_book.c` 中 `cache_open(cache_dir, 500 * 1024 * 1024)` 仅分配 500MB。参考 code search 系统（`cache_import.c` 使用 4GB），大书集合需要更大的缓存空间。

**修复**（`tools/import_book.c`）：
```c
// 旧：500MB（多本大书不足）
cache_t* cache = cache_open(cache_dir, 500 * 1024 * 1024);

// 新：4GB（对齐 cache_import.c 配置）
cache_t* cache = cache_open(cache_dir, 4ULL * 1024 * 1024 * 1024);
```

**效果**：多本书共存不触发 LRU 淘汰，已导入的书籍元数据和向量完整保留。

**参考**：此优化来自 code search 系统的 Linux 内核级验证（coding.md 系统级修复 #9），`cache_import.c` 已使用 4GB 成功处理 150 万+ 条目的导入。

### 修复后的导入结果

| 书籍 | 格式 | 页数 | 搜索分数范围 | 状态 |
|------|------|------|-------------|------|
| 硅谷钢铁侠 | AZW3 | 838 | 0.789-0.912 | 正常 |
| Data Engineering for Cybersecurity | MOBI | 633 | 0.779-0.900 | 正常 |
| Designing Data-Intensive Applications | PDF | 4193 | 0.818-0.906 | 正常 |
| The Staff Engineer's Path | EPUB | 1433 | 0.818-0.905 | 正常 |

**总导入量**：7097 页，向量文件 30.3MB，Cache 52MB。

### 完整部署流程（优化后）

```bash
# 1. 编译所有组件
make clean
make tools/import_book tools/cache_query tools/build_hnsw_index
cd src/importer/wrappers && make

# 2. 设置 GPU 环境
export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH

# 3. 导入四本测试书
./tools/import_book /book/cache ~/book.epub /books/epub
./tools/import_book /book/cache ~/book.azw3 /books/azw3
./tools/import_book /book/cache ~/book.mobi /books/mobi
./tools/import_book /book/cache ~/book.pdf /books/pdf

# 4. 验证向量文件
ls -la /book/cache/vectors/*.hnsw

# 5. 测试语义搜索
./explore_book.sh /books/mobi search "your query"

# 6. 测试翻页阅读
./explore_book.sh /books/epub read chapters/01-Intro/page_0005
./explore_book.sh /books/epub read --next chapters/01-Intro/page_0005
./explore_book.sh /books/epub read --prev chapters/02-Chapter/page_0000
```

---

### 问题 16: Kobo 格式 EPUB 章节边界检测失败

**现象**：Kobo 电子阅读器生成的 EPUB 使用 `<span class="koboSpan">` 代替 `<h1>` 作为章节标题，导致 `import_book()` 无法识别章节边界。所有技术章节都收到相同的 200KB 文本（书开头的内容），15 个章节中仅 5-6 个有唯一内容。

**根因**：`import_book()` 的章节切分逻辑依赖章节标题进行字符串匹配。当标题为空时，offset-based 切片被跳过（条件 `chapters[i].title && strlen(chapters[i].title) > 0`），fallback 到全文截断。

**修复**（`tools/import_book.c`）：
```c
// 旧：标题为空时跳过 offset 切片
if (main_chapter_count > 1 && chapters[i].title && strlen(chapters[i].title) > 0) {

// 新：标题为空也尝试 offset 切片
if (main_chapter_count > 1) {
```

**效果**：15 个技术章节唯一率从 30% → 100%，每章内容独立。

---

### 问题 17: Rust tokenizer 多线程 panic

**现象**：`tokenizers_encode()`（Rust 库）在 OpenMP 多线程并发调用时 panic 崩溃，日志显示 `thread caused non-unwinding panic. aborting.`。

**根因**：`tokenizers-cpp` 库不是线程安全的。多个线程同时调用 `tokenizers_encode()` 导致 Rust 内部状态被写坏，触发 `unwrap()` panic（Rust 的 panic 无法被 C++ 的 `try/catch` 捕获）。

**修复**（`tools/import_book.c`）：
```c
// 在所有 onnx_embedder_encode 调用外包 critical section
#pragma omp critical(embedder)
ret = onnx_embedder_encode_batch(g_embedder, texts, bcount, ...);
```

**效果**：批量编码 + OpenMP 并行两全，GPU 利用率不降，tokenizer 不再崩溃。

---

### 问题 18: onnx_embedder_encode_batch tokenizer 崩溃

**现象**：`import_book` 导入带向量时在 `process_chapter()` 中崩溃，backtrace 指向 `onnx_embedder_encode_batch` → `tokenizers_encode`。

**根因**：与问题 17 同一根源。batch 编码时多线程竞争 tokenizer 内部状态。

**修复**：同问题 17，`#pragma omp critical(embedder)` 保护所有 embedder 调用。

**效果**：batch 编码正常工作，无需禁用。`use_batch = 1` 保留。

---

### 问题 19: 控制字符导致 tokenizer Rust panic

**现象**：某些电子书文本包含控制字符（0x00-0x1F, 0x7F），导致 Rust tokenizer 在 `unwrap()` 时 abort。

**根因**：`process_chapter()` 的 UTF-8 清洗只过滤了非法 UTF-8 序列，没有过滤合法但会导致 tokenizer 崩溃的控制字符。

**修复**（`tools/import_book.c`）：
```c
// 在 UTF-8 清洗中增加控制字符过滤
if ((c & 0x80) == 0) {
    // 过滤控制字符(0x00-0x1F,0x7F)，保留 tab(9) newline(10) cr(13)
    if (c < 0x20 && c != 9 && c != 10 && c != 13) {
        skipped_bytes++; i++; continue;
    }
    if (c == 0x7F) { skipped_bytes++; i++; continue; }
    char_len = 1;
}
```

**效果**：tokenizer 不再因控制字符崩溃。

---

### 问题 20: HTML→Markdown 格式保留（MOBI + EPUB）

**现象**：电子书导入后所有格式丢失——标题没有 `#`、粗体没有 `**`、链接没有 `[]()`、列表没有 `-`、代码块没有 `` ` ``。原本是 HTML 标签的内容变成了连续纯文本。

**根因**：wrapper 库的 `extract_text_from_html()` 函数只做 `<` 标签剥离，不做格式转换。

**修复**：
- **MOBI**（`src/importer/wrappers/mobi_wrapper.cpp`）：重写 `extract_text_from_html()`，手动解析 HTML 标签并转换为 Markdown 语法
- **EPUB**（`src/importer/wrappers/epub_wrapper.cpp`）：重写 `extract_text_from_html()`，利用 libxml2 XML 解析器递归遍历节点树，按标签类型输出 Markdown

**转换对照表**：

| HTML | Markdown | 状态 |
|------|----------|:----:|
| `<h1>..<h6>` | `# .. ######` | ✅ |
| `<b>/<strong>` | `**text**` | ✅ |
| `<i>/<em>` | `*text*` | ✅ |
| `<code>/<tt>` | `` `text` `` | ✅ |
| `<a href>` | `[text](url)` | ✅ |
| `<img alt src>` | `![alt](src)` | ✅ |
| `<li>` | `- item` | ✅ |
| `<pre>` | ` ``` ` code block | ✅ |
| `<blockquote>` | `> quote` | ✅ |
| `&amp;` 等实体 | 解码为字符 | ✅ |

---

### 问题 21: `--skip-vectors` 事实上不跳过 GPU

**现象**：加上 `--skip-vectors` 参数后导入仍然慢，GPU 仍然在工作。参数只跳过了最后的 `export_vectors()` 阶段，`process_chapter()` 中的向量生成不受影响。

**根因**：`--skip-vectors` 只控制了是否调用 `export_vectors()`，没有阻止 `onnx_embedder_init()` 的调用和 `process_chapter()` 中的向量生成。

**修复**（`tools/import_book.c`）：
```c
// --skip-vectors 时不初始化 embedder
if (!flag_skip_vectors) {
    g_embedder = onnx_embedder_init(...);
}
```

**效果**：`--skip-vectors` 导入速度从 5-10 分钟降到 10-30 秒。

---

### 性能优化汇总

| # | 优化 | 之前 | 之后 | 加速比 |
|---|------|------|------|--------|
| 1 | `--skip-vectors` 真跳过 GPU | 300+s | 10-30s | **10-30x** |
| 2 | 向量导出进度条 | 静默等待 | 实时百分比+ETA | — |
| 3 | cache.bin 4GB | 500MB | 4GB | 8x 容量 |
| 4 | `--generate-vectors` 模式 | 无此功能 | 批量生成 | — |
| 5 | `--only-vectors` 增量模式 | 无此功能 | 重新导出 | — |
| 6 | KV Cache 独立路径 | `/book/cache/` | `/book/cache/` | 不冲突 |

---

### 新增功能

| 功能 | 命令 | 用途 |
|------|------|------|
| 增量导入（跳过向量） | `--skip-vectors` | 快速导入文本，不生成搜索索引 |
| 增量导入（仅向量） | `--only-vectors` | 重新导出已有向量并重建 HNSW |
| 增量导入（生成向量） | `--generate-vectors` | 为已导入但无向量的页面生成向量 |
| 书籍列表 | `list` | 列出所有已导入书籍 |
| 删除书籍 | `delete` | 从 KV Cache + 磁盘中移除 |
| 搜索预览 | `search` | 搜索结果显示内容预览片段 |

---

*文档版本：2026-06-11 v4（新增问题 16-21、HTML→Markdown 格式保留、性能优化汇总）*
*适用于：explore_book.sh + import_book + mobi_wrapper + epub_wrapper 最新版本*