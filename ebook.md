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
./tools/import_book <cache_dir> <book_file> [namespace] [output_dir]
```

**参数说明**：
| 参数 | 必需 | 说明 |
|------|------|------|
| `cache_dir` | 是 | KV Cache 目录，如 `./ai_code_memory` |
| `book_file` | 是 | 电子书文件路径 |
| `namespace` | 否 | 命名空间，如 `/books/ddia`。默认从文件名生成 |
| `output_dir` | 否 | Markdown 输出目录，默认 `/opt/books` |

**示例**：
```bash
# 基本导入
./tools/import_book ./ai_code_memory ~/book.mobi

# 指定命名空间
./tools/import_book ./ai_code_memory ~/book.mobi /books/my_book

# 指定输出目录
./tools/import_book ./ai_code_memory ~/paper.pdf /books/paper /data/books

# 完整示例：导入 Elon Musk 传记
LD_LIBRARY_PATH=$(pwd):/opt/TensorRT-10/lib:$LD_LIBRARY_PATH \
  ./tools/import_book \
  ./ai_code_memory \
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
ai_code_memory/
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
  --analysis-dir ./ai_code_memory \
  --max-results 10
```

---

## 已导入书籍

当前系统已导入以下书籍：

| 命名空间 | 书名 | 格式 | 章节数 | 向量数 |
|----------|------|------|--------|--------|
| `/books/ddia` | Designing Data-Intensive Applications | PDF | 210 | 10,918 |
| `/books/cybersec` | Cybersecurity | MOBI | 10 | 5,323 |
| `/books/elon_musk` | 硅谷钢铁侠 | AZW3 | 23 | 144 |
| `/books/staff_engineer` | The Staff Engineer's Path | EPUB | 21 | 82 |

**查看所有命名空间**：
```bash
strings ai_code_memory/cache.bin | grep "^/books/" | sort -u
```

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
ls -la ai_code_memory/vectors/*.hnsw

# 检查 GPU 是否可用
./tools/cache_query "test" --type search --analysis-dir ./ai_code_memory
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
| `models/jina-embeddings-v2-base-code/` | Jina v2 嵌入模型 |
| `ai_code_memory/` | KV Cache 数据目录 |
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

**现象**：TensorRT 工作空间仅 6GB，缓存路径是相对路径 `./trt_cache`，在服务器环境中不可靠。

**修复**（`src/embedding/onnx_embedder.c`）：
```c
// RTX 4090D: 24GB VRAM, allocate 12GB workspace
OrtTensorRTProviderOptions trt_options;
memset(&trt_options, 0, sizeof(trt_options));
trt_options.trt_max_workspace_size = 12ULL * 1024 * 1024 * 1024;  // 12GB

// Use absolute path for engine cache
static char trt_cache_path[512];
snprintf(trt_cache_path, sizeof(trt_cache_path), "%s/trt_cache",
         getenv("HOME") ? getenv("HOME") : "/tmp");
trt_options.trt_engine_cache_path = trt_cache_path;
```

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

| 指标 | RTX 3080 (本地) | RTX 4090D (修复前) | RTX 4090D (修复后) |
|------|----------------|-------------------|-------------------|
| TensorRT 状态 | ✅ 正常 | ❌ 加载失败 | ✅ 正常 |
| ONNX Runtime | 1.23.2 | 1.20.1（错误） | 1.23.2 |
| GPU 推理 | ✅ | ❌ CPU fallback | ✅ |
| AZW3 导入 (144页) | ~12s | ~60s+ | **~11.7s** |
| HNSW 索引构建 | ✅ 自动 | ❌ 失败 | ✅ 自动 |
| 语义搜索 | ✅ | ❌ 不可用 | ✅ 8s |

### 部署检查清单

在新服务器上部署时，执行以下检查：

```bash
# 1. 编译所有组件
make clean
make tools/import_book tools/cache_query tools/build_hnsw_index
cd src/importer/wrappers && make

# 2. 验证 GPU 库链接
ldd tools/import_book | grep onnx
# 应显示项目目录下的 libonnxruntime.so.1，而非系统路径

# 3. 验证 TensorRT 可用
export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH
./tools/import_book ./ai_code_memory ~/test.epub /books/test
# 应看到："Using TensorRT GPU acceleration (FP32 for Jina)"

# 4. 验证 HNSW 索引生成
ls -la ai_code_memory/vectors/*.hnsw
# 应存在 .hnsw 文件

# 5. 验证语义搜索
./explore_book.sh /books/test search "test query"
# 应返回相关结果，而非 "Vector engine not initialized"
```

### 故障排查速查

| 症状 | 原因 | 解决 |
|------|------|------|
| `version VERS_1.23.2 not found` | 加载了系统旧版 ONNX Runtime | 重新编译，`make clean && make` |
| `CUDA not available, falling back to CPU` | CUDA/cuDNN 库不在 LD_LIBRARY_PATH | `export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH` |
| `libnvonnxparser.so.10: cannot open` | TensorRT ONNX parser 缺失 | 添加 `/opt/TensorRT-10/lib` 到 LD_LIBRARY_PATH |
| `HNSW build failed` | build_hnsw_index 未编译 | `make tools/build_hnsw_index` |
| `Failed to load libepubparse.so` | EPUB wrapper 未编译 | `cd src/importer/wrappers && make` |
| 导入极慢（>10分钟） | 回退到 CPU 推理 | 检查上述 GPU 配置 |

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

### 完整部署流程（优化后）

```bash
# 1. 编译所有组件
make clean
make tools/import_book tools/cache_query tools/build_hnsw_index
cd src/importer/wrappers && make

# 2. 设置 GPU 环境
export LD_LIBRARY_PATH=/opt/TensorRT-10/lib:$(pwd):$LD_LIBRARY_PATH

# 3. 导入四本测试书
./tools/import_book ./ai_code_memory ~/book.epub /books/epub
./tools/import_book ./ai_code_memory ~/book.azw3 /books/azw3
./tools/import_book ./ai_code_memory ~/book.mobi /books/mobi
./tools/import_book ./ai_code_memory ~/book.pdf /books/pdf

# 4. 验证向量文件
ls -la ai_code_memory/vectors/*.hnsw

# 5. 测试语义搜索
./explore_book.sh /books/mobi search "your query"
```

---

*文档版本：2026-05-22*
*适用于：explore_book.sh + import_book 最新版本*