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

## 16. 后续实现步骤

1. 实现 `AgentContext` Python 类，对接 `libmydb.so`。
2. 提供 `kv_search` / `kv_get` / `kv_context` 工具接口。
3. 包装 `cache_query` 为模型可调用的函数。
4. 跑通端到端示例：用户提问 → 搜索代码 → 深度上下文 → 生成回答。
5. 增加第三方代码识别与拦截配置。
6. 实现第三方依赖自动预索引流程。

## 17. UI 层实现方案：GPUI 编译为 `.so` + C 调用

### 17.1 为什么选 GPUI

opencode 的核心产品线其实是 **TUI/终端交互** + **代码编辑**。原生 opencode TUI 基于 Node.js/Bun + SolidJS + 自定义终端库，启动慢、内存占用高、渲染经过 JS 事件循环。

Zed 的 **GPUI** 框架正好适合重写这条产品线：

- GPU 加速的 2D 渲染（Metal/Vulkan/DirectX）
- 响应式 UI 模型，类似 React/Solid
- 跨平台窗口和输入事件系统
- 已被 Zed 编辑器验证过

### 17.2 GPUI 不是纯 Rust，底层是 C/C++/Objective-C

GPUI 对外是 Rust crate，但底层大量依赖平台原生 API：

| 平台 | 底层技术 |
|------|---------|
| macOS | Cocoa/AppKit (Objective-C)、Metal |
| Linux | Wayland/X11 (C)、Vulkan/OpenGL |
| Windows | Win32 (C)、DirectX/DirectWrite |
| 字体 | CoreText / DirectWrite / font-kit (C/C++) |
| 输入 | 各平台原生事件 API |

所以 GPUI 的“Rust 上层 + C/C++ 底层”结构，很适合包装成 `.so` 给 C 调用。

### 17.3 现成组件库：`gpui-component`

不必从零写 GPUI 组件，可直接使用 [longbridge/gpui-component](https://github.com/longbridge/gpui-component)：

- 60+ 跨平台桌面 UI 组件
- Dock 布局系统（面板、分割、标签页、自由拖拽）
- 虚拟化 Table / List，支持大数据量
- 内置 Markdown / 简单 HTML 渲染
- 代码编辑器（支持 LSP、Tree-sitter 语法高亮，标称 20 万行稳定）
- 图表、按钮、输入框、弹窗、菜单、主题系统
- 最小二进制约 12MB
- 已用于 Longbridge Pro 等实际产品

对 opencode 复刻版最有价值的组件：

| 组件 | 用途 |
|------|------|
| `Dock` | 会话列表面板 + 聊天面板 + 代码编辑器 + 工具输出 |
| `Editor` | 代码编辑 + 语法高亮 + LSP |
| `Markdown` | 渲染 LLM 回复、文档 |
| `List` / `Table` | 文件树、搜索结果显示 |
| `Button` / `Input` / `Modal` | 常规交互 |
| `Theme` | 暗色/亮色主题 |

### 17.4 方案：Rust 侧编译 `libgpui_app.so`，暴露最小 C ABI

不暴露 GPUI 全部 API（太复杂，且依赖 Rust trait/闭包/生命周期），只在 Rust 侧实现一个稳定的应用骨架，暴露少量 C ABI：

```rust
// Rust side (crate-type = ["cdylib"])
#[no_mangle]
pub extern "C" fn gpui_app_create(config_json: *const c_char) -> *mut AppState;

#[no_mangle]
pub extern "C" fn gpui_app_run(app: *mut AppState);

#[no_mangle]
pub extern "C" fn gpui_app_quit(app: *mut AppState);

#[no_mangle]
pub extern "C" fn gpui_window_open(app: *mut AppState, title: *const c_char);

#[no_mangle]
pub extern "C" fn gpui_panel_set_chat(
    app: *mut AppState,
    session_id: *const c_char,
    messages_json: *const c_char,
);

#[no_mangle]
pub extern "C" fn gpui_panel_set_code_context(
    app: *mut AppState,
    file_path: *const c_char,
    line_start: i32,
    line_end: i32,
    content: *const c_char,
);

#[no_mangle]
pub extern "C" fn gpui_on_user_message(
    app: *mut AppState,
    callback: extern "C" fn(*const c_char, *mut c_void),
    userdata: *mut c_void,
);

#[no_mangle]
pub extern "C" fn gpui_on_tool_call(
    app: *mut AppState,
    callback: extern "C" fn(*const c_char, *mut c_void) -> *mut c_char,
    userdata: *mut c_void,
);

#[no_mangle]
pub extern "C" fn gpui_send_stream_delta(
    app: *mut AppState,
    session_id: *const c_char,
    delta: *const c_char,
);

#[no_mangle]
pub extern "C" fn gpui_send_tool_output(
    app: *mut AppState,
    tool_id: *const c_char,
    output: *const c_char,
);
```

```c
// C side
#include "gpui_app.h"

int main() {
    gpui_app_t* app = gpui_app_create("{\"theme\":\"dark\"}");
    gpui_window_open(app, "opencode-rs");

    gpui_on_user_message(app, on_user_message, NULL);
    gpui_on_tool_call(app, on_tool_call, NULL);

    gpui_app_run(app);   // blocks, runs event loop
    return 0;
}
```

### 17.5 Rust 侧负责什么

- GPU 窗口创建与管理
- 主题、布局、渲染
- 键盘/鼠标事件路由
- 组件树：会话列表、聊天面板、代码编辑器视图、文件树、工具输出面板
- 把用户输入、命令提交通过 C 回调传给 C 端
- 使用 `gpui-component` 的 `Dock`、`Editor`、`Markdown`、`List`、`Theme` 等组件

### 17.6 C 侧负责什么

- 主程序入口
- Agent 核心逻辑
- LLM 调用（流式输出回传给 Rust UI）
- 工具执行（bash、文件读写、git）
- KV Cache 查询（调用 `libmydb.so`）
- 代码索引（调用 `typescript-indexer` 等）
- 会话状态管理

### 17.7 与现有 my_db 工具链的整合

| 已有组件 | 在新 UI 中的作用 |
|---------|---------------|
| `libmydb.so` / KV Cache | 代码记忆、语义搜索、会话持久化 |
| `typescript-indexer` | 索引项目代码 |
| `analyze_nodejs_repo.sh` | 一键索引用户项目 |
| `cache_query` | 符号上下文查询 |
| `call_graph` | 调用关系展示 |
| 语义向量/HNSW | 自然语言找代码 |

### 17.8 为什么这个方案合理

1. **借力 GPUI + gpui-component 的 GPU 渲染和现成组件**，不用自己写 Metal/Vulkan/Win32，也不用从零造编辑器。
2. **保留 C 核心技术栈**，Agent 逻辑、LLM、工具、KV Cache 继续用 C 写。
3. **Rust 只当 UI 驱动**，职责清晰。
4. **C ABI 稳定后**，以后换其他 UI 框架也容易。
5. **避免 Electron/Node.js 运行时**，启动快、内存低。
6. **Zed 本身就是最好的 demo**，编辑器、LSP、多面板、文件树都有现成参考实现。

### 17.9 风险与应对

| 风险 | 应对 |
|------|------|
| GPUI API 还在快速演进 | 只依赖少量稳定概念：window、div、text、list、editor；使用 gpui-component 进一步隔离 |
| GPUI 没有独立发布 | 基于 Zed 源码 + gpui-component 构建，可 fork 固定版本 |
| C ABI 设计复杂 | 先暴露 5-10 个函数，跑通后再扩展 |
| 编译产物大 | Rust + GPUI 产物确实大，但运行内存小，可接受 |

### 17.10 最小可验证原型

1. Rust 侧：一个 GPUI 窗口，左侧会话列表，右侧聊天面板，基于 `gpui-component` 的 `Dock` + `List`。
2. C 侧：收到用户消息后，把消息 echo 回 UI。
3. 跑通后，再加入：LLM 流式输出、代码编辑器上下文面板、Markdown 渲染、工具输出。
4. 最终接入 `libmydb.so` 查询，实现自然语言找代码 + 代码上下文展示。

### 17.11 一句话总结

> **用 GPUI + gpui-component 做 GPU 渲染和现成 UI，用 C 写 Agent 核心，通过稳定的 C ABI 桥接。既享受现代 GPU UI 框架和编辑器组件，又保留 my_db 的 C 工具链优势。**
