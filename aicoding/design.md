# opencode 复刻设计方案

> 基于 my_db KV Cache 的永久记忆 Agent 上下文系统

## 1. 设计目标

复刻 opencode 的核心交互体验，但解决两个原生痛点：

1. **上下文爆炸**：长会话或大项目时，messages 数组不断增长，token 消耗失控。
2. **第三方代码误读**：Agent 经常无意义地读取 `node_modules/`、`vendor/`、`.venv/` 等外部依赖。

核心解决思路：

> **所有上下文和参考代码全部存入 KV Cache，按 namespace 组织，支持搜索。Prompt 只携带当前最关键的信息，其他内容需要时随时从 KV Cache 读取。**

这样带来三个结果：

- Prompt 大小始终可控，不会随会话增长。
- 历史、项目知识、代码参考全部落盘，是永久记忆。
- 第三方代码只返回预索引的精华摘要，不被完整读入 prompt。

## 2. 核心理念

### 2.1 Prompt = 索引，KV Cache = 数据库

把传统 opencode 的"把所有信息堆进 messages"模式，改为：

> **Prompt = 索引，KV Cache = 完整数据库。**

模型像查询数据库一样按需拉取上下文，而不是把数据库全背在身上。

因为所有内容块（chunk）大小固定，需要时直接对 KV Cache 做读写即可，不需要像 opencode 那样频繁 compaction。

### 2.2 模拟人类记忆

本方案本质上在模拟人类记忆的工作方式：

> 人脑不会记住所有细节，只记住最核心的概念、经验、结论。需要具体信息时，会去查书（读 md 文档）、去查资料库（查 KV Cache）。

Agent 也应该这样工作。

**人类 vs Agent 记忆对照：**

| 人类 | Agent |
|------|-------|
| 瞬时记忆 | prompt 中当前几条关键信息 |
| 工作记忆 | `/session/{id}/tasks/` + `/facts/` |
| 长期记忆 | `/project/` + `/knowledge/` + `/code/` |
| 查书/笔记 | 读项目 `*.md` 文档 |
| 查资料库 | 搜索 KV Cache |

**Agent 只记精华：**

- 不记完整对话
- 不记完整文件内容
- 不记第三方代码全文
- 只记：任务、关键事实、决策、错误模式、架构要点、代码模式

**需要细节时去查：**

- 查 md 文档 → 读项目 README、设计文档、API 文档
- 查 KV Cache → 语义搜索代码、符号上下文、历史记录

### 2.3 三层记忆模型

```
┌─────────────────────────────────────┐
│  瞬时记忆（在 prompt 中）            │
│  - 当前任务一句话                    │
│  - 刚查到的关键代码片段              │
│  - 本轮对话最后 1-2 轮               │
│  容量：极小，只放最核心              │
└─────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────┐
│  工作记忆（KV Cache，TTL 短）        │
│  /session/{id}/tasks/               │
│  /session/{id}/facts/               │
│  /session/{id}/errors/              │
│  容量：当前会话相关                  │
│  生命周期：几小时到几天              │
└─────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────┐
│  长期记忆（KV Cache，永久）          │
│  /project/{name}/architecture       │
│  /project/{name}/conventions        │
│  /knowledge/{domain}/concept/       │
│  /code/{project}/...                │
│  容量：项目知识 + 通用知识           │
│  生命周期：永久，可更新              │
└─────────────────────────────────────┘
              │
              ▼
┌─────────────────────────────────────┐
│  外部资料（需要时读取）              │
│  *.md 文档                          │
│  源码文件                           │
│  第三方代码摘要                     │
│  按需加载，不常驻记忆                │
└─────────────────────────────────────┘
```

### 2.4 记忆的衰减与强化

- 不重要的信息设置短 TTL，自然过期
- 反复用到的信息提升重要性 `i`，转化为长期记忆
- 关键决策永久保存
- 错误模式记录，避免重复犯错

## 3. KV Cache 落盘保证

本方案依赖 my_db KV Cache 的持久化能力：

- `cache.bin` 通过 `mmap` 映射到磁盘文件。
- 每次写入后调用 `msync(MS_SYNC)` 强制落盘。
- 进程重启、系统关机后数据不丢失。
- 下次启动 `cache_open()` 时自动加载。

因此，Agent 的记忆是**永久性**的：

| 场景 | 效果 |
|------|------|
| 对话中断 | 重新打开后，历史、任务、事实都在 |
| 项目分析一次 | 终身可用，无需重复索引 |
| 跨会话 | 上周聊过的设计决策，今天仍能召回 |
| 多 Agent | 一个 Agent 写入，其他 Agent 立即可读 |

## 4. Namespace 规范

所有数据按层级 namespace 组织，避免冲突，便于搜索。

```
/agent/{agent_id}/
  /config              # Agent 配置（模型、性格、权限）
  /stats               # 运行统计

/session/{session_id}/
  /turns/{turn_id}           # 每轮对话原文
  /summaries/{turn_id}       # 每轮压缩摘要
  /facts/{fact_id}           # 提取的关键事实
  /tasks/{task_id}           # 任务记录
  /files/{relative_path}     # 会话中读/写过的文件快照
  /symbols/{name}            # 当前会话关注的符号
  /errors/{error_id}         # 遇到的错误及解决方案

/project/{project_name}/
  /meta/readme               # README 摘要
  /meta/architecture         # 架构说明
  /meta/conventions          # 代码规范
  /meta/dependencies         # 依赖列表
  /files/{path}              # 项目文件摘要/索引
  /symbols/{name}            # 项目符号索引
  /todos                     # 项目级 TODO

/code/{project}/             # 第三方/参考项目（复用现有索引）
  /chunks/{file}/{function}
  /symbols/{name}
  /callers/{name}
  /callees/{name}
  /dataflow/vars/{name}
  /_meta/info

/knowledge/{domain}/         # 通用知识
  /concept/{name}
  /pattern/{name}
  /faq/{name}

/search/
  /recent/{query_hash}       # 最近搜索缓存
  /hot/{namespace}           # 热门查询
```

## 5. Value 统一 JSON 格式

所有存入 KV Cache 的 value 使用统一 JSON 结构：

```json
{
  "t": "chunk|symbol|summary|fact|task|file|error|concept",
  "c": "精华内容/摘要",
  "s": "源文件路径或来源",
  "p": "位置（line:42-80, page:15）",
  "i": 5,
  "tags": ["tag1", "tag2"],
  "v": [],
  "ts": 1718400000,
  "ttl": 0
}
```

字段说明：

| 字段 | 含义 |
|------|------|
| `t` | 类型 |
| `c` | 内容/摘要 |
| `s` | 来源 |
| `p` | 位置 |
| `i` | 重要性 1-5 |
| `tags` | 标签，用于交叉搜索 |
| `v` | 可选向量 |
| `ts` | 创建时间戳 |
| `ttl` | 过期时间（毫秒，0 表示永久） |

### 5.1 源码位置索引原则

> **记忆存精华，位置做索引。**

KV Cache 中不存完整源码，只存：

- 函数/类是做什么的
- 关键逻辑一句话
- 设计亮点
- 文件路径 + 行号

Agent 想看完整代码时：

1. 查 KV Cache → 拿到摘要和位置
2. 根据 `path:line` 去读原文件
3. 把需要的片段放入 prompt

示例：

```json
{
  "t": "symbol",
  "c": "ngx_palloc: 从内存池分配内存，优先使用当前块剩余空间，不足时分配新块。",
  "s": "src/core/ngx_palloc.c",
  "p": "line:15-80",
  "i": 5,
  "tags": ["memory-pool", "nginx"]
}
```

Agent 需要详细看实现时：

```python
read_file("src/core/ngx_palloc.c", offset=15, limit=80)
```

## 6. 核心原则

| 原则 | 说明 |
|------|------|
| 能入 cache 就不放 prompt | 历史、文件、错误、任务全部持久化到 KV Cache |
| 只召回相关片段 | LLM 调用前按需搜索，不塞完整历史 |
| namespace 隔离 | 会话、项目、代码库严格分开，避免污染 |
| 带 TTL 分层 | 原始对话短期，事实/架构长期，知识永久 |
| 搜索优先于遍历 | 用 prefix/regex/vector/tag 定位，不用 glob 扫全量 |

## 7. Prompt 组成

每次调用大模型 API 时，prompt 中只包含以下三类信息：

1. **系统提示**：固定、精简。
2. **当前任务 + 关键事实**：从 KV Cache 召回的最相关片段。
3. **本轮需要的代码/上下文**：通过搜索实时获取。

以下内容**不直接放入 prompt**，而是留在 KV Cache：

- 完整对话历史
- 之前读过的所有文件
- 项目全部代码
- 第三方依赖完整内容

## 8. 当前记忆容量控制

### 8.1 记忆上限

Agent 的当前工作记忆有明确上限，防止膨胀：

```python
MAX_WORKING_MEMORY = 64 * 1024  # 64KB 文本
MAX_FACTS = 20
MAX_TASKS = 10
MAX_RECENT_TURNS = 5
MAX_PROMPT_TOKENS = 8000
```

### 8.2 丢弃策略

当工作记忆超过上限时：

1. 先丢原始对话，只保留摘要
2. 再丢低重要性事实（`i < 3`）
3. 再丢已完成任务
4. 永久记忆不丢（`/project/`、`/knowledge/`、`/code/`）

### 8.3 为什么能丢

因为原始信息都有持久化：

| 信息类型 | 持久化位置 |
|---------|-----------|
| 完整对话 | 可归档到冷存储，但通常不需要 |
| 项目架构 | `/project/{name}/architecture` |
| 代码细节 | `/code/{project}/chunks/...` |
| 设计文档 | 项目 `*.md` 文件 |
| 第三方代码 | `/code/npm/{package}/...` |

### 8.4 记忆重建

```
Agent 发现当前记忆不够用了
  │
  ▼
丢弃旧的 / 摘要化
  │
  ▼
需要时根据当前 query 重新搜索
  ├─ 搜 KV Cache → /project/ /code/ /knowledge/
  ├─ 读项目 md 文档
  └─ 必要时读源码文件
  │
  ▼
重新装入当前记忆
```

### 8.5 一句话总结

> **Agent 的脑子很小，只装当下最需要的；背后的 KV Cache 和 md 文档是无限的外脑，随时能查。**

## 9. 模型可用工具

Agent 通过工具按需从 KV Cache 读取信息：

```python
[
  {
    "name": "kv_search",
    "description": "从 KV Cache 搜索相关代码或历史信息",
    "parameters": {
      "query": "用户当前问题",
      "namespace": "/code/nginx or /session/xxx",
      "type": "semantic|prefix|regex|symbol"
    }
  },
  {
    "name": "kv_get",
    "description": "精确读取 KV Cache 中某个 key 的内容",
    "parameters": {
      "key": "/session/xxx/tasks/001"
    }
  },
  {
    "name": "kv_context",
    "description": "获取某个符号的完整上下文（caller/callee/dataflow）",
    "parameters": {
      "symbol": "ngx_palloc",
      "repo": "/code/nginx"
    }
  }
]
```

模型自己决定什么时候查 cache、查什么。

## 9. 远程大模型 API 的工具调用

把 KV Cache 和 md 文档的搜索能力作为工具暴露给远程 API，让模型自己决定查什么、怎么查。

### 9.1 暴露给 API 的工具列表

```json
[
  {
    "name": "md_read",
    "description": "读取项目中的 markdown 文档，获取设计说明、API 文档、架构信息",
    "parameters": {
      "path": "docs/architecture.md",
      "offset": 0,
      "limit": 100
    }
  },
  {
    "name": "md_search",
    "description": "在所有 md 文档中搜索关键词",
    "parameters": {
      "query": "memory pool",
      "project": "mydb"
    }
  },
  {
    "name": "kv_get",
    "description": "精确读取 KV Cache 中某个 key 的内容",
    "parameters": {
      "key": "/project/mydb/meta/architecture"
    }
  },
  {
    "name": "kv_search",
    "description": "在 KV Cache 中语义搜索或前缀搜索",
    "parameters": {
      "query": "memory pool allocator",
      "namespace": "/code/nginx",
      "type": "semantic"
    }
  },
  {
    "name": "kv_context",
    "description": "获取某个符号的完整上下文：caller/callee/dataflow",
    "parameters": {
      "symbol": "ngx_palloc",
      "repo": "/code/nginx",
      "depth": 2
    }
  },
  {
    "name": "source_read",
    "description": "根据 KV Cache 中记录的位置读取源码片段",
    "parameters": {
      "path": "src/core/ngx_palloc.c",
      "line_start": 15,
      "line_end": 80
    }
  },
  {
    "name": "code_index",
    "description": "使用 coding.md 工具链扫描代码库并写入 KV Cache 记忆系统，供后续搜索使用",
    "parameters": {
      "source": "/opt/nginx 或 https://github.com/redis/redis",
      "namespace": "/code/nginx",
      "options": {
        "skip_vectors": false,
        "skip_callgraph": false,
        "skip_dataflow": false,
        "jobs": 4
      }
    }
  }
]
```

### 9.2 API 决策流程

```
用户提问
  │
  ▼
发送轻量 prompt + 工具列表
  │
  ▼
远程 API 判断：
  ├─ 这个问题需要查架构吗？ → md_read / kv_get
  ├─ 需要找相关代码吗？ → kv_search
  ├─ 提到具体函数了吗？ → kv_context
  └─ 需要看实现细节吗？ → source_read
  │
  ▼
API 返回 tool_calls
  │
  ▼
本地执行工具，返回结果
  │
  ▼
API 基于结果生成最终回答
```

### 9.3 给 API 的系统提示

```markdown
You have access to a local knowledge base:

- Project docs: *.md files under the project root
- Long-term memory: KV Cache at /memory/
  - /project/{name}/ — project architecture, conventions, todos
  - /code/{project}/ — indexed third-party and reference code
  - /knowledge/{domain}/ — general programming knowledge
  - /session/{id}/ — current session tasks and facts

Rules:
- Only load information relevant to the current task.
- Prefer kv_search over reading full files.
- Use kv_context when analyzing a specific symbol.
- Use source_read only when you need implementation details.
- Current working memory is limited; old items may be dropped.

You can also extend the knowledge base:

- Use `code_index` to scan any code repository into KV Cache.
- This uses the coding.md toolchain: index → vectorize → call graph → dataflow → cache_import.
- After indexing, the code becomes searchable via kv_search / kv_context / source_read.

When to use code_index:
- User asks about a dependency not yet indexed.
- User references an open-source project you don't recognize.
- Existing index is outdated and needs refresh.
```

### 9.4 优势

| 优势 | 说明 |
|------|------|
| API 更智能 | 模型自己决定查什么，比我们硬塞更精准 |
| 减少无用 token | 只传模型主动要的信息 |
| 多轮查询 | API 可以连环调用多个工具，逐步深入 |
| 可解释 | 模型查了什么一目了然 |
| 通用 | 同样的工具接口适配任何支持 function calling 的 API |

## 10. 工作流程

```
用户提问
  │
  ▼
发送轻量 prompt + 工具列表
  │
  ▼
远程 API 判断需要查什么
  → kv_search("memory pool", "/code/nginx")
  → kv_context("ngx_palloc", "/code/nginx")
  → source_read("src/core/ngx_palloc.c", 15, 80)
  │
  ▼
本地执行工具，返回结果
  │
  ▼
远程 API 基于结果生成回答/执行 edit/bash
```

## 11. 关键场景处理

### 10.1 用户发消息

```python
def on_user_message(session, query):
    # 1. 记录用户输入
    session.add_turn(next_turn_id(), "user", query)

    # 2. 判断是否需要搜索代码
    if needs_code_context(query):
        code_results = session.search_code(query)
        if is_symbol(query):
            context = get_symbol_context(query, repo=session.project)
            session.add_fact(f"ctx_{query}", context, important=True)

    # 3. 组装 prompt
    prompt = session.build_prompt(query)

    # 4. 调用 LLM
    response = llm.complete(prompt)

    # 5. 记录助手回复
    session.add_turn(next_turn_id(), "assistant", response)

    return response
```

### 10.2 读取文件

```python
def read_file(session, path):
    # 项目文件：直接读取 + 摘要入 cache
    content = fs.read(path)
    summary = summarize(content)
    session.add_file(path, summary, hash(content))

    # 第三方文件：查 cache，不直接读
    if is_third_party(path):
        cached = find_in_code_cache(path)
        return cached or {
            "error": "第三方代码默认不直接读取",
            "suggestion": f"使用 search --third-party {symbol} 查询摘要"
        }

    return content
```

### 10.3 每次 LLM 调用前

只从 KV Cache 中搜索并召回相关片段，不携带完整历史。

## 11. 第三方代码预索引

需要用到第三方代码库时，提前用 coding.md 工具链处理一遍，存入 KV Cache 记忆系统。Agent 运行时只查记忆，不直接读第三方源码。

### 11.1 预索引流程

```
1. 项目启动前
   │
   ▼
识别项目依赖
   ├─ package.json → node_modules/*
   ├─ requirements.txt → .venv/site-packages/*
   ├─ Cargo.toml → cargo registry/*
   ├─ go.mod → $GOPATH/pkg/*
   └─ 其他 vendor/ third_party/
   │
   ▼
用 coding.md 工具链索引每个依赖
   ├─ ./analyze_repo.sh /path/to/dependency /code/npm/{package}
   ├─ 或 ./ai_code_search.sh analyze ...
   └─ 生成 chunks / symbols / vectors / call_graph / dataflow
   │
   ▼
cache_import 写入 /memory/
   │
   ▼
2. Agent 运行时
   │
   ▼
Agent 需要查第三方 API
   ├─ kv_search("debounce throttle", namespace="/code/npm/lodash")
   ├─ kv_context("useEffect", repo="/code/npm/react")
   └─ kv_get("/code/npm/express/src/router.js")
   │
   ▼
只返回精华摘要，不读完整源码
```

### 11.2 第三方代码的记忆结构

```
/code/npm/{package}/
  /_meta/
    /readme              # README 摘要
    /version             # 版本号
    /language            # 主要语言
    /dependencies        # 依赖关系
    /structure           # 目录结构
  /src/{entry_file}
    /src/index.js        # 入口文件摘要
    /src/{module}.js     # 各模块摘要
  /symbols/{export_name}
    /symbols/debounce    # debounce 函数摘要 + 签名
    /symbols/useEffect   # useEffect hook 摘要 + 签名
  /types/{type_name}     # TypeScript 类型定义
  /examples/{usage}      # 典型用法
```

### 11.3 接入配置

```json
{
  "third_party": {
    "index_on_startup": true,
    "cache_dir": "/memory",
    "sources": {
      "npm": {
        "path": "./node_modules",
        "namespace_prefix": "/code/npm",
        "index_command": "analyze_repo.sh"
      },
      "pypi": {
        "path": ".venv/lib/python*/site-packages",
        "namespace_prefix": "/code/pypi",
        "index_command": "analyze_repo.sh"
      },
      "cargo": {
        "path": "~/.cargo/registry/src",
        "namespace_prefix": "/code/cargo",
        "index_command": "analyze_repo.sh"
      }
    }
  }
}
```

### 11.4 关键优势

| 方面 | 效果 |
|------|------|
| 不读完整第三方代码 | token 大幅下降 |
| 精华摘要 + 语义搜索 | 查得准、查得快 |
| 一次索引，永久使用 | 多个项目共享同一份记忆 |
| 版本固定 | `/code/npm/lodash@4.17.21` 可并存 |
| 不污染项目上下文 | namespace 隔离 |

### 11.5 一句话总结

> **第三方代码也是知识，提前用 coding.md 索引进 KV Cache。Agent 运行时只查记忆，不碰源码。**

## 12. 与现有工具链对接

| 已有工具 | 用途 |
|---------|------|
| `analyze_repo.sh /opt/nginx /code/nginx` | 索引第三方/参考项目 |
| `cache_query "memory pool" --repo /code/nginx --type search` | Agent 内部语义搜索 |
| `cache_query ngx_palloc --repo /code/nginx --type context` | 符号深度上下文 |
| `cache_import` | 把分析结果写入 `/code/` |
| `mydb.Cache` Python binding | Agent Runtime 直接读写 KV Cache |

## 13. 上下文不爆炸的原因

1. **Prompt 不再累积**：每次只放系统提示 + 当前任务 + 刚召回的相关片段。
2. **历史、文件、代码全部外置**：存在 KV Cache，按需拉取。
3. **模型通过工具自己查**：需要时才读，不需要就不读。
4. **第三方代码只给摘要**：不塞完整文件。
5. **模拟人类记忆**：只记精华，细节现查。

## 14. Agent 无需维护复杂上下文

本方案下，Agent 本身非常轻量，不需要维护复杂的上下文状态。

### 14.1 传统 Agent vs 本方案

| 传统 Agent | 本方案 Agent |
|-----------|-------------|
| messages 越来越长 | prompt 始终很小 |
| 自己记住所有历史 | 只记精华，历史入 KV Cache |
| 自己读完整文件 | 只读摘要，细节用 source_read |
| 自己处理第三方代码 | 第三方代码预索引，只查记忆 |
| 上下文管理逻辑复杂 | 有上限，超了就丢，能重建 |

### 14.2 Agent 真正需要维护的内容

```
prompt 里只放：
  1. 系统提示（固定）
  2. 当前任务（1-2 句话）
  3. 关键事实（最多 20 条）
  4. 最近 3-5 轮对话
  5. 刚查到的相关片段
```

其他全部外置：

| 信息类型 | 存储位置 |
|---------|---------|
| 项目架构 | `/project/{name}/` |
| 代码知识 | `/code/{project}/` |
| 通用知识 | `/knowledge/` |
| 原始文档 | 项目 `*.md` 文件 |
| 第三方代码 | 预索引进 `/code/` |

### 14.3 为什么实现更简单

1. **不用写复杂的上下文压缩算法**
2. **不用做精细的 token 预算分配**
3. **有明确上限，超了就丢**
4. **需要的信息随时从外脑召回**

### 14.4 一句话总结

> **Agent 是个轻量调度器，不是记忆仓库。记忆仓库是 KV Cache + md 文档，远程 API 自己按需去查。**

## 15. 与原生 opencode 的差异

| 维度 | 原生 opencode | 本方案复刻版 |
|------|--------------|-------------|
| 上下文管理 | compaction 摘要，可能丢失细节 | KV Cache 四级记忆，按需召回 |
| 上下文大小 | messages 不断增长，token 爆炸 | prompt 固定大小，内容块固定大小，按需读写 KV Cache |
| 第三方代码 | 无特殊处理，直接读文件 | 预索引 + 拦截 + 只读摘要 |
| 项目知识 | 依赖模型自己记住 | 显式索引到 `/code/` namespace |
| 持久化 | 会话结束后主要靠日志 | mmap KV Cache，断电不丢 |
| 搜索能力 | 基本 grep/文件列表 | 语义向量 + 调用图 + 数据流 |
| 架构复杂度 | 较高，依赖 npm 生态 | 基于现有 C 工具链，更轻量 |
| 记忆模型 | 无显式分层 | 模拟人类三层记忆 |
| Agent 复杂度 | 需要维护长上下文 | 轻量调度器，上下文极简 |

## 16. 会话与上下文管理：基于 KV Cache 重构

原生 opencode 的会话管理依赖 `messages` 数组，需要 compaction、token 预算、复杂的状态机。本方案直接用 KV Cache 替换整个会话/上下文系统。

### 16.1 核心原则

> **没有 messages 数组。会话就是 KV Cache 中的一个 namespace，prompt 是每次调用前从 KV Cache 实时组装出来的查询结果。**

| opencode | 本方案 |
|----------|--------|
| `messages: Message[]` | `/session/{id}/` namespace |
| compaction | 不需要，本来就只存摘要 |
| context window 预算 | 固定工作记忆上限 |
| 工具定义硬编码 | 工具定义也入 KV Cache |
| 历史原文 | 不保留，只保留每轮摘要 |

### 16.2 Session namespace 结构

```
/session/{session_id}/
  /meta
    /created_at          # 创建时间戳
    /updated_at          # 最后活跃时间
    /project             # 关联项目 namespace
    /model               # 当前模型配置
  /task/current          # 当前任务一句话
  /task/history/{id}     # 历史任务（已完成的）
  /facts/{fact_id}       # 关键事实、决策、错误模式
  /turns/{turn_id}       # 原始轮次（可选，TTL 短，可清理）
  /summaries/{turn_id}   # 每轮压缩摘要
  /files/{path_hash}     # 会话中关注的文件快照/摘要
  /symbols/{name}        # 当前关注的符号
  /errors/{error_id}     # 遇到的错误及解决方案
  /pending_tool          # 当前未完成的 tool call（如果有）
```

### 16.3 Prompt 组装流程

每次调用 LLM 前，C 侧 `session_build_prompt()` 按以下顺序从 KV Cache 读取：

1. **System prompt**：从 `/agent/{id}/prompts/system` 读取固定模板。
2. **工具列表**：从 `/agent/{id}/tools/` 读取可用工具 schema。
3. **当前任务**：`/session/{id}/task/current`。
4. **关键事实**：`/session/{id}/facts/*`，按重要性 `i` 排序，最多 20 条。
5. **最近摘要**：`/session/{id}/summaries/*`，按 turn_id 逆序，最多 5 条。
6. **相关代码片段**：根据当前 query 调用 `kv_search` 实时召回，最多 5 段。

所有内容固定格式为 markdown 文本块，最终拼成一个 `char*` 传入 LLM client。

### 16.4 不保留完整对话

- 用户/助手的完整原文可以临时写入 `/session/{id}/turns/{turn_id}`，但 TTL 短（如 24 小时）。
- 每轮结束后立即生成摘要写入 `/session/{id}/summaries/{turn_id}`，长期保留。
- 摘要格式：

```json
{
  "t": "summary",
  "c": "用户要求添加 TypeScript 插件支持；助手创建了 C 插件并更新了 code_indexer。",
  "tags": ["typescript", "plugin", "code_indexer"],
  "i": 4,
  "ts": 1718400000
}
```

### 16.5 工具定义也入 KV Cache

不再把 tool schema 硬编码在 C 代码里，而是作为 KV 记录：

```
/agent/{agent_id}/tools/
  /bash                # bash 工具 schema
  /source_read         # 读文件工具 schema
  /kv_search           # 搜索代码记忆
  /kv_context          # 符号上下文
  /code_index          # 索引新项目
  /md_read             # 读 markdown 文档
```

每条 value 是 JSON：

```json
{
  "t": "tool",
  "name": "kv_search",
  "description": "从 KV Cache 搜索相关代码或历史信息",
  "parameters": {
    "query": {"type": "string", "required": true},
    "namespace": {"type": "string", "required": false},
    "type": {"type": "string", "enum": ["semantic", "prefix", "regex", "symbol"]}
  }
}
```

启动时 C 侧加载这些记录，动态构建发送给 LLM 的 `tools` 数组。

### 16.6 事实提取与强化

每次助手回复后，可选调用一次轻量 LLM 让模型自己提取关键事实：

```
输入：本轮对话原文
输出：JSON 数组 [{"fact": "...", "importance": 4, "tags": [...]}]
```

写入 `/session/{id}/facts/{fact_id}`。

反复被引用或搜索到的事实可以提升 `i` 值，最终升级为 `/project/{name}/facts/` 长期记忆。

### 16.7 C Session Manager API

最小 C API 设计：

```c
typedef struct session session_t;

session_t* session_create(const char* session_id, const char* project_ns, const char* model);
void       session_free(session_t* s);

int session_set_task(session_t* s, const char* task);
int session_add_fact(session_t* s, const char* fact, int importance, const char** tags, int ntags);
int session_add_summary(session_t* s, int turn_id, const char* summary);
int session_add_error(session_t* s, const char* error, const char* solution);
int session_add_file(session_t* s, const char* path, const char* summary);
int session_add_symbol(session_t* s, const char* name);

char* session_build_prompt(session_t* s, const char* user_query);
```

实现直接调用 `libmydb.so` 的 `cache_set` / `cache_get` / `cache_search`。

### 16.8 与 opencode 提示词的对接

opencode 的 system prompt、tool schema 描述、响应格式要求可以直接复用。迁移方式：

1. 把 opencode 的 prompt 文本整理成 markdown 模板文件，放在 `opencode/prompts/`。
2. C 启动时加载这些模板。
3. 模板中的变量（如 `{tools}`、`{task}`、`{facts}`、`{summaries}`）由 `session_build_prompt()` 用 KV Cache 查询结果填充。

这样 opencode 的提示词工程成果保留，但底层 context 管理完全替换为 KV Cache。

### 16.9 为什么更简单

| 传统方案 | KV Cache 方案 |
|----------|--------------|
| 维护 messages 数组 | 直接读写 KV Cache |
| compaction 算法 | 只存摘要，自然瘦身 |
| token 预算分配 | 固定上限，超了按策略丢 |
| 硬编码工具 schema | 工具定义也是 KV 记录 |
| 历史原文拖累 prompt | 历史只以摘要形式召回 |
| 上下文状态机复杂 | 每次重新组装，状态极简 |

一句话总结：

> **opencode 的提示词可以复用，但它的会话/context 系统完全不需要复刻。用 KV Cache 重构后，Agent 只要一个 session_id 和一个 prompt 组装函数。**

## 17. LuaJIT 脚本层：C 内核 + Lua 动态扩展

为了不改 C 代码就能调整 prompt、工具、响应解析逻辑，引入 LuaJIT 作为脚本层。C 只做最小内核（KV Cache、LLM HTTP client、session namespace），动态能力全部交给 Lua。

### 17.1 为什么加 LuaJIT

| 纯 C 方案 | C + LuaJIT 方案 |
|-----------|----------------|
| 改 prompt 要重编译 | 改 Lua 脚本即可 |
| 加 tool 要改 C 代码 | Lua 里注册新 tool |
| JSON 用 C 库容易出错 | Lua cjson 成熟稳定 |
| 插件系统要自己设计 | Lua `require` 就是插件系统 |
| 响应解析写 C 很繁琐 | Lua 模式匹配/JSON 极方便 |

### 17.2 职责划分

```
┌─────────────────────────────────────────┐
│  Lua 脚本层（动态，用户可改）              │
│  - prompt 组装                            │
│  - tool schema 定义                       │
│  - tool dispatch                          │
│  - LLM 响应解析                           │
│  - 插件加载 (require)                     │
│  - JSON 处理 (cjson)                      │
└─────────────────────────────────────────┘
                    ↑↓ C ABI / Lua bindings
┌─────────────────────────────────────────┐
│  C 内核（稳定，性能关键）                  │
│  - KV Cache 读写                          │
│  - session namespace 管理                 │
│  - OpenAI-compatible LLM client           │
│  - SSE 流式解析                           │
│  - 文件读取 (source_read)                 │
│  - LuaJIT 宿主                            │
└─────────────────────────────────────────┘
```

### 17.3 C 暴露给 Lua 的 API

```c
// KV Cache
opencode.cache_get(key) -> string|nil
opencode.cache_set(key, value, ttl_ms) -> bool
opencode.cache_search_prefix(prefix, max_results) -> [{key, value, score}]
opencode.cache_search_tag(tag, max_results) -> [{key, value, score}]

// Files / code
opencode.source_read(path, line_start, line_end) -> string|nil, err

// LLM
opencode.llm_complete(system_prompt, user_prompt, tools_json) -> response_json|nil, err

// Logging
opencode.log_info(msg)
```

### 17.4 Lua 脚本结构

```
opencode/
  main.lua                 # 入口：加载 prompts/tools，注册工具
  prompts/
    default.lua            # build_prompt(session_id, project_ns, query)
  tools/
    default.lua            # tool 定义 + dispatch
  plugins/
    *.lua                  # 用户自定义插件
```

### 17.5 示例：prompt 组装在 Lua 里

```lua
-- prompts/default.lua
local cjson = require("cjson")

local M = {}
M.MAX_FACTS = 20
M.MAX_SUMMARIES = 5

function M.build_prompt(session_id, project_ns, user_query)
    local session_prefix = "/session/" .. session_id .. "/"
    local parts = {}

    -- System
    table.insert(parts, "# System\nYou are an expert coding assistant...")

    -- Tools (discovered from KV Cache)
    local tools = opencode.cache_search_prefix("/agent/default/tools/", 32)
    for _, t in ipairs(tools) do
        local tool = cjson.decode(t.value)
        table.insert(parts, "- `" .. tool.name .. "`: " .. tool.description)
    end

    -- Current task
    local task_json = opencode.cache_get(session_prefix .. "task/current")
    if task_json then
        local task = cjson.decode(task_json)
        table.insert(parts, "\n## Current Task\n" .. task.c)
    end

    -- Facts, summaries, user query...
    table.insert(parts, "\n## User\n" .. user_query)
    table.insert(parts, "\n## Assistant\n")

    return table.concat(parts, "\n")
end

return M
```

### 17.6 示例：tool 定义和 dispatch 在 Lua 里

```lua
-- tools/default.lua
local cjson = require("cjson")

local M = {}

M.tools = {
    {
        name = "kv_search",
        description = "Search KV Cache",
        parameters = {
            query = { type = "string", required = true },
            namespace = { type = "string", required = false }
        },
        handler = function(args)
            local prefix = args.namespace or ""
            if prefix ~= "" and not prefix:match("/$") then prefix = prefix .. "/" end
            local results = opencode.cache_search_prefix(prefix, 10)
            return { ok = true, results = results }
        end
    },
    {
        name = "source_read",
        description = "Read source file lines",
        parameters = {
            path = { type = "string", required = true },
            line_start = { type = "integer", required = false },
            line_end = { type = "integer", required = false }
        },
        handler = function(args)
            local content = opencode.source_read(args.path, args.line_start or 1, args.line_end or 0)
            return { ok = true, content = content }
        end
    }
}

function M.register_tools(agent_ns)
    for _, t in ipairs(M.tools) do
        local schema = { t = "tool", name = t.name, description = t.description, parameters = t.parameters }
        local key = agent_ns .. "/tools/" .. t.name
        opencode.cache_set(key, cjson.encode(schema), 0)
    end
end

function M.dispatch(tool_call)
    for _, t in ipairs(M.tools) do
        if t.name == tool_call.name then
            return t.handler(tool_call.arguments)
        end
    end
    return { ok = false, error = "unknown tool" }
end

return M
```

### 17.7 为什么用 /usr/local/lualib/cjson.so

系统已经通过 OpenResty/LuaJIT 生态预置了 `cjson.so`，LuaJIT 可以直接 `require("cjson")`。不需要额外安装 npm 包，也不需要 C 代码里处理 JSON 转义。

### 17.8 安全边界

- **危险操作（bash）** 目前由 Lua 实现；后续可加 C 侧白名单/沙箱。
- Lua 脚本运行在主线程，tool handler 中不要执行长时间阻塞操作；需要的话后续改成 C 侧线程池 + Lua callback。

### 17.9 一句话总结

> **C 写内核和 LLM client，LuaJIT 写 prompt、tool、插件。改逻辑不用重新编译，JSON 全部交给 cjson，扩展能力用 `require`。**

## 18. LLM Client 设计

C 侧实现一个轻量 OpenAI-compatible HTTP client，支持同步和流式两种模式。

### 18.1 配置

```c
typedef struct {
    char* base_url;       /* e.g. "https://api.openai.com/v1" */
    char* api_key;
    char* model;
    double temperature;
    int    max_tokens;
} llm_config_t;
```

实际密钥从环境变量读取：

```bash
export OPENAI_BASE_URL=https://api.openai.com/v1
export OPENAI_API_KEY=sk-...
export OPENAI_MODEL=gpt-4o-mini
```

### 18.2 同步调用

```c
llm_client_t* c = llm_client_create(&cfg);
char* resp = llm_complete(c, system_prompt, user_prompt, tools_json);
// resp is full JSON from /chat/completions
```

### 18.3 流式调用

```c
int on_delta(const char* delta, void* userdata) {
    printf("%s", delta);
    return 0;  // continue
}

llm_complete_stream(c, system, prompt, tools, on_delta, NULL);
```

SSE 解析在 C 侧完成，每解析出一个 `content` delta 就调一次 callback。callback 可以把文本回传给 UI 或 Lua。

### 18.4 工具调用流程

```
Lua: build_prompt() -> C: llm_complete(system, prompt, tools_json)
                           -> API returns tool_calls
                           -> Lua: handle_tool_call() -> C tool dispatch
                           -> C: send tool result back to API
                           -> API returns final text
                           -> Lua: save summary + facts
```

### 18.5 一句话总结

> **LLM client 是 C 侧一个小型 libcurl 封装，只做 HTTP + SSE 解析。请求体组装、响应解析、tool 循环全部交给 Lua。**

## 19. 与原生 opencode 尚未对齐的功能清单

> 以下功能来自对 `/opt/opencode` 真实源码的分析，优先级按实现难度和用户体验排序。`skill` 工具不在本复刻范围内。

### 19.1 工具系统差距

#### 19.1.1 缺少的核心工具

| 工具 | opencode 行为 | 本方案现状 | 优先级 |
|------|--------------|-----------|--------|
| `edit` | 精确文本替换，支持 `oldString`/`newString`/`replaceAll`，保留 BOM/行尾符 | 有 `apply_edit`，功能类似但命名和输出格式不同 | 高 |
| `write` | 创建或覆盖单个文件，保留 BOM | 拆成 `file_create`，没有统一 `write` | 高 |
| `apply_patch` | 用 `*** Begin Patch` 格式批量增删改文件 | 缺失 | 高 |
| `glob` | 按 glob 模式搜索文件，返回相对路径列表 | 只有 `file_list`（ls 包装） | 高 |
| `grep` | 正则搜索文件内容，返回文件/行号/预览 | 缺失 | 高 |
| `question` | 向用户提问（单选/多选/自定义），获取结构化答案 | 缺失 | 中 |
| `todowrite` | 创建和维护结构化任务列表 | 缺失 | 中 |
| `webfetch` | HTTP/HTTPS 抓取，返回 text/markdown/html | 缺失 | 中 |
| `websearch` | 网络搜索（Exa/Parallel） | 缺失 | 低 |
| `lsp` | 语言服务器协议交互 | 缺失 | 低 |

#### 19.1.2 现有工具需对齐

| 我们的工具 | 需对齐项 |
|-----------|---------|
| `apply_edit` | 改名为 `edit` 或加别名；支持 `replaceAll`；输出 diff 预览；BOM/行尾处理 |
| `file_create` | 合并为 `write`，支持覆盖已存在文件；返回 `{ operation, target, resource, existed }` |
| `file_delete` | 建议通过 `apply_patch` delete 实现，不再单独暴露 |
| `source_read` | 改名为 `read`；支持 offset/limit 分页；支持图片 base64；支持目录列表；二进制文件检测 |
| `bash` | 增加 `workdir`、`timeout`、`description`；stdout/stderr 分离输出；超时检测 |
| `git` | opencode 不单独暴露 `git` 工具，应通过 `bash` 执行 git 命令 |
| `diff` | opencode 不单独暴露 `diff` 工具，通过 `bash` git diff 或文件系统状态获取 |
| `kv_*` | 保留，但输出格式应更紧凑，接近 opencode `ToolOutput` 结构 |

#### 19.1.3 工具输出格式

- opencode 工具返回 `{ structured, content: [{type, text} | {type, file}] }`，再转成模型可见文本。
- 当前方案返回 `{ ok, output/content/error }` 自定义 JSON，后续应统一为结构化输出。

### 19.2 权限系统差距

| 项 | opencode | 本方案现状 | 是否需改 |
|---|---|---|---|
| 匹配语义 | **last-match-wins**（`findLast`） | 当前 first-match-wins | 是 |
| 无规则默认 | 默认 `deny`（`missingAgentPermissions = [{ "*", "*", "deny" }]`） | 默认 `ask` | 是 |
| 持久化授权 | 用户 `always` 后写入 `PermissionSaved` 表，跨进程保留 | 仅 session 内存 allow | 是 |
| 错误类型 | `DeniedError` / `RejectedError` / `CorrectedError` | 字符串错误 | 中 |
| Agent 级权限 | 每个 agent 可独立配置 `permissions` | 全局 + 项目级 | 中 |
| 资源数组 | `resources: string[]`，逐资源 evaluate | 单 resource 字符串 | 低 |
| 配置键 | `permissions`（复数数组） | 同 v2，但兼容 legacy `permission` | 可保留 |

### 19.3 Session / 消息系统差距

| 项 | opencode | 本方案 |
|---|---|---|
| 消息类型 | `User`/`Assistant`/`Shell`/`System`/`Synthetic`/`Compaction`/`AgentSwitched`/`ModelSwitched` | 简单 user/assistant/tool |
| Compaction | 一级消息类型，自动/手动触发 summary + recent context | 无 |
| 持久化 | SQLite event store、session inbox、input promotion | 只有 KV Cache index 持久化 |
| Agent 切换 | `AgentSwitched`/`ModelSwitched`、Context Epoch 替换 | 单一 agent |
| 多 session/project | 支持 | 仅单 session/project |
| 会话分享 | `share` 配置 | 无 |

### 19.4 Agent 系统差距

| 项 | opencode | 本方案 |
|---|---|---|
| 内置 agent | `build`/`plan`/`explore`/`title`/`summary`/`compaction` | 无 |
| 自定义 agent | 用户可配置 `agents` | 无 |
| Agent 字段 | `model`/`variant`/`system`/`mode`/`steps`/`color`/`permissions` | 无 |
| 子代理 | `createSubagentData`/`snapshotSubagentData` | 无 |

### 19.5 上下文 / Instruction 差距

| 项 | opencode | 本方案 |
|---|---|---|
| `AGENTS.md` | 自动从项目根向上发现并注入系统上下文 | 未实现 |
| `instructions` | 配置额外指令文件/glob/URL | 未实现 |
| `references` | 本地目录/Git 仓库作为 `@alias/path` 外部上下文 | 未实现 |
| System Context Registry | `SystemContext`/`Context Epoch`/`Mid-Conversation System Message` | 只有静态 system prompt |

### 19.6 Config 配置差距

| 配置项 | opencode | 本方案 |
|---|---|---|
| `agents` | 支持 | 无 |
| `permissions` | 支持 | 部分 |
| `providers` | 多 provider/model/variant/headers/body | 仅 env |
| `mcp.servers` | 支持 local/remote MCP | 无 |
| `skills` | 支持 | 不做 |
| `instructions` | 支持 | 无 |
| `references` | 支持 | 无 |
| `formatter` / `lsp` | 支持 | 无 |
| `snapshots` | 文件系统快照/undo | 无 |
| `tool_output` | 输出截断阈值 | 硬编码 |
| `compaction` | 上下文压缩配置 | 不需要 |
| `shell` | 默认 shell | 无 |
| `share` / `enterprise` / `username` | 支持 | 无 |

### 19.7 Git / 文件系统差距

| 项 | opencode | 本方案 |
|---|---|---|
| BOM 保留 | `edit`/`write` 保留 UTF-8 BOM | 无 |
| 行尾符处理 | 自动检测/转换 `\n` / `\r\n` | 无 |
| 条件写入 | `writeIfUnchanged` 防脏写 | 无 |
| 外部目录权限 | `external_directory` 单独授权 | 无 |
| 快照/undo | `snapshot` 配置支持 revert/unrevert | 无 |
| Git 工具 | 不单独暴露，通过 `bash` 执行 | 有独立 `git` 工具 |

### 19.8 UI / CLI 差距

| 项 | opencode | 本方案 |
|---|---|---|
| 默认形态 | TUI（`runTui`） | stdin REPL |
| 权限提示 | TUI 弹窗 `once`/`always`/`reject` | tty 文本 `y/N/a/d` |
| 流式输出 | 支持 SSE 增量渲染 | 整段返回 |
| 多 session | 支持 | 不支持 |
| 命令体系 | `serve`/`service`/`migrate`/`debug` | 仅 `--session`/`--project` |

### 19.9 LLM / Provider 差距

| 项 | opencode | 本方案 |
|---|---|---|
| 多 provider | `providers` 配置 | 单 env |
| per-agent model | 支持 | 无 |
| model variants | 支持 | 无 |
| request overrides | headers/body/aisdk options | 无 |
| tool choice | `toolChoice` | 无 |
| response format | structured output / json schema | 无 |
| 协议支持 | OpenAI Chat/Responses, Anthropic, Gemini, Bedrock | OpenAI-compatible only |

### 19.10 MCP / 插件差距

| 项 | opencode | 本方案 |
|---|---|---|
| MCP server | `mcp.servers` local/remote | 无 |
| 插件系统 | server/tui 插件 | 静态 Lua 脚本 |

### 19.11 实现优先级建议

按依赖顺序，建议分阶段补齐：

1. **权限系统对齐**：改回 last-match-wins；持久化 `always` 授权。
2. **工具改名/补齐**：`edit`/`write`/`apply_patch`/`glob`/`grep`。
3. **`read` 增强**：分页、图片、目录、二进制检测。
4. **文件修改增强**：BOM/行尾/条件写入。
5. **`question` / `todowrite` 工具**。
6. **Agent 系统**：至少内置 `build`/`plan`/`explore`。
7. **`AGENTS.md` / `instructions` 自动注入**。
8. **Session 结构化持久化**。
9. **TUI / 流式输出**。
10. **MCP / 多 provider / 插件**。

### 19.12 GUI 当前实现

早期实现过一个最小可用 GUI 原型（`egui` + `eframe` 编译为 `libaicoding_gui.so`，通过 C ABI + LuaJIT FFI 集成，`OPENCODE_GUI=1` 启动）。该路线已废弃——UI 与引擎的耦合方式改为 **ACP 协议解耦**，见第 20 章。

---

## 20. UI 层实现方案：agent-rs（Rust ACP 客户端）

### 20.1 架构决策：引擎/UI 分离

引擎（C + LuaJIT）通过 `--acp` 模式提供 **ACP 协议**（JSON-RPC 2.0 over stdio），UI 是纯协议消费方。早期方案（GPUI fork Zed 源码 + C ABI `.so` 桥接）已废弃，原因：

- GPUI 没有独立发布，必须 fork Zed 百万行源码并长期跟进，维护成本极高
- 依赖 Zed 的能力边界，UI 演进受制于人
- 引擎内嵌 UI 使两者无法独立升级

正确路线：**Rust 客户端走 ACP 协议连接引擎**。引擎零改动，UI 完全独立，可随时替换（终端/TUI/Web/任何 ACP 客户端）。

### 20.2 agent-rs 结构

```
agent-rs/
├── crates/
│   ├── acp/       # ACP 协议类型 + 进程客户端
│   │              # - 完整协议类型（initialize/session/*/事件流）
│   │              # - spawn 引擎、请求/响应 id 匹配、broadcast 事件流
│   └── cli/       # 终端 REPL（当前）
```

### 20.3 当前实现（M1：终端 REPL，已完成）

- 协议握手（initialize）、会话生命周期（new/prompt/close/cancel）
- 流式文本渲染（`agent_message_chunk` 事件逐块输出）
- 工具调用状态显示（`tool_call` → `tool_call_update`，in_progress → completed）
- Ctrl-C 取消当前回合、会话结束自动归档（引擎侧 summarize）

### 20.4 演进路线

| 阶段 | 内容 | 状态 |
|------|------|------|
| M1 | ACP 客户端 + 终端 REPL（流式/工具状态/取消） | ✅ 完成 |
| M2 | TUI（ratatui）：会话列表、输入框、流式面板、工具面板 | 计划 |
| M3 | Web UI（axum + SSE）：浏览器访问，对标 opencode app | 计划 |
| M4 | 渐进接管引擎：`llm_client.c` → reqwest、Lua 逻辑 → Rust trait | 计划 |

### 20.5 与 my_db 工具链的整合

| 已有组件 | 在 UI 中的作用 |
|---------|---------------|
| `libmydb.so` / KV Cache | 引擎侧记忆系统（`kv_*` 工具由模型自主调用，UI 无需实现） |
| `cache_query` / 语义向量 | 代码搜索（通过引擎工具暴露） |
| 会话摘要 | 引擎每轮自动摘要，UI 可展示跨会话恢复 |

UI 层（agent-rs）负责体验：记忆操作可视化（"写入记忆 X"）、新会话预热上次摘要（引擎已支持，UI 展示）。

### 20.6 关键协议要点（agent-rs 已验证）

- 请求/响应：`{"jsonrpc":"2.0","id":N,"method":...,"params":...}`，字段 camelCase
- 流式事件：无 id 的 `session/update` notification（broadcast 给所有订阅者）
- 引擎 stdout 混有 `[CACHE]` 等日志行，客户端需过滤非 JSON 行
- 引擎 `load_env_file` 用 `setenv(...,1)` 无条件覆盖环境变量，LLM 配置注入必须走 `--env FILE` 参数

### 20.7 一句话总结

> **C + LuaJIT 引擎不变，agent-rs 作为独立的 Rust ACP 客户端提供 UI（REPL → TUI → Web），引擎/UI 通过 ACP 协议完全解耦。**
