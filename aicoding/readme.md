# aicoding — AI Coding Agent for Zed Editor

一个面向开发者的 AI Coding Agent，**UI 使用 Zed 编辑器**，所有的 AI 能力（无限上下文、代码搜索、Plan/Build Agent、权限系统）基于 C + LuaJIT + my_db KV Cache 实现。

> 📚 **相关基础设施文档**：
> - [`/opt/my_db/coding.md`](../coding.md) — 代码探索记忆系统：语义搜索、调用图、数据流追踪。
> - [`/opt/my_db/kvCache.md`](../kvCache.md) — my_db KV Cache：层级命名空间、持久化、多维搜索。

---

## 架构

```
┌───────────────────────────────────────────────────────────┐
│                    Zed Editor                             │
│  ┌─────────────────────────────────────────────────────┐  │
│  │  Agent Panel (聊天界面 / 文件差异 / 工具输出)        │  │
│  └──────────────────────┬──────────────────────────────┘  │
│                         │ ACP (stdin/stdout JSON-RPC)     │
└─────────────────────────┼─────────────────────────────────┘
                          │
┌─────────────────────────┼─────────────────────────────────┐
│  aicoding ───--acp ─────┘                                 │
│                                                           │
│  ┌─────────────────────────────────────────────────────┐  │
│  │  C 内核                                              │  │
│  │  - cli.c: --acp 模式, JSON-RPC 解析/分发              │  │
│  │  - lua_engine.c: LuaJIT 宿主 + C 绑定 (acp_send 等)   │  │
│  │  - llm_client.c: OpenAI/Anthropic HTTP 客户端         │  │
│  │  - session.c: KV Cache 上的会话管理                   │  │
│  │  - memory.lua: my_db 记忆封装 (read/write/search)     │  │
│  └──────────────────────┬──────────────────────────────┘  │
│                         │                                 │
│  ┌──────────────────────┴──────────────────────────────┐  │
│  │  LuaJIT 脚本层 (动态, 不改 C 即可修改逻辑)            │  │
│  │                                                     │  │
│  │  main.lua         ACP 入口 (acp_chat)                │  │
│  │  context.lua      滑动窗口上下文压缩                  │  │
│  │  prompts/default  系统提示组装 + 事实注入             │  │
│  │  summarize.lua    自动摘要 + 关键事实提取             │  │
│  │  tools/default    工具定义与分发 (read/write/bash)    │  │
│  │  memory.lua       KV Cache 记忆系统封装               │  │
│  │  permissions.lua  权限规则引擎                        │  │
│  │  agents/          Plan / Build 自主 Agent            │  │
│  │  shell.lua        安全命令执行                        │  │
│  │  knowledge.lua    项目级跨会话知识记忆                │  │
│  │  trace.lua        结构化执行轨迹                      │  │
│  │  rerank.lua       搜索结果本地重排序                  │  │
│  │  conventions.lua  项目类型自动检测与注入              │  │
│  │  compress.lua     System prompt 压缩                 │  │
│  └──────────────────────┬──────────────────────────────┘  │
│                         │                                 │
│  ┌──────────────────────┴──────────────────────────────┐  │
│  │  持久化层                                            │  │
│  │  - my_db KV Cache (libmydb.so) → 记忆/上下文/事实    │  │
│  │  - Vector Engine (libvector_engine.so) → 语义搜索    │  │
│  │  - tools/cache_query → 符号上下文 / 调用图查询       │  │
│  └─────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────┘
```

### 分层职责

| 层 | 技术 | 职责 |
|----|------|------|
| **UI** | Zed Agent Panel | 聊天界面、文件差异预览、工具调用可视化、线程管理 |
| **协议** | ACP (Agent Client Protocol) | JSON-RPC over stdio，Zed ↔ aicoding 通信 |
| **AI 引擎** | C + LuaJIT | LLM 调用、工具执行、上下文管理、Agent 逻辑 |
| **持久化** | my_db (C) | mmap 零拷贝 KV Cache，向量索引，代码搜索 |

### 为什么这样设计

| 传统方案 | 本方案 |
|----------|--------|
| Electron + Node.js 运行时 | C + LuaJIT 单二进制 |
| 自己实现 UI (TUI/GUI) | **直接用 Zed 的 Agent Panel**，零 UI 工作量 |
| 上下文靠 messages 数组无限增长 | 有界窗口 + LRU 归档 + KV Cache 按需召回 |
| 工具硬编码在 TypeScript 里 | Lua 脚本定义，热更新 |
| 依赖 npm 生态 | 只依赖系统已有的 `libmydb.so` / `cjson.so` |

---

## 核心特性

| 特性 | 说明 |
|------|------|
| **Zed 原生集成** | `--acp` 模式通过 ACP 协议接入 Zed Agent Panel，无需额外 UI |
| **独立二进制** | 编译成单个 `aicoding`，无 Node.js / Bun / Electron 依赖 |
| **无限上下文** | 滑动窗口 + KV Cache 归档，默认 16K 窗口永不膨胀 |
| **代码语义搜索** | 基于 Jina embeddings + HNSW，自然语言搜索已索引代码库 |
| **符号上下文** | 调用链展开 (caller/callee)，支持深度控制和批量查询 |
| **工具调用** | read/write/edit/glob/grep/bash/build/git/web_fetch 等 24+ 工具 |
| **权限系统** | allow/deny/ask 三级，默认保护系统目录和用户目录 |
| **自主 Agent** | Plan Agent (任务拆解执行) + Build Agent (编译-诊断-修复循环) |
| **自动摘要** | 每轮对话结束自动提取摘要和事实写入 KV Cache |
| **项目级知识** | `.opencode/knowledge/` 跨会话共享，自动注入系统提示 |
| **Lua 热更新** | 工具、prompt、agent 全部用 Lua 编写，改逻辑不重编译 |
| **项目类型检测** | 自动识别 linux_kernel/cargo/npm/python/go/cmake，注入最佳实践 |
| **安全护栏** | 默认禁止危险路径写入和危险 bash 命令 |
| **Web Fetch** | 抓取外部文档/API/RFC 供模型实时参考 |
| **结构化轨迹** | 工具调用、LLM 请求、错误等自动记录，支持 `trace_query` 自省 |
| **预索引代码库** | Linux Kernel / Nginx / Redis / CPython / LuaJIT 等已预索引 |

---

## 快速开始

### 1. 编译

```bash
cd /opt/my_db/aicoding
make
```

生成 `aicoding` 二进制和 `libaicoding_agent.a` 静态库。

### 2. 配置

复制 `.env.example` 为 `.env`，填写 LLM 密钥：

```bash
OPENAI_API_KEY=sk-...
OPENAI_BASE_URL=https://api.openai.com/v1
OPENAI_MODEL=gpt-4o
```

支持所有 OpenAI/Anthropic 兼容 API（Kimi、DeepSeek、Claude 等），见 `.env.example`。

### 3. 接入 Zed

在 Zed 的 `settings.json` 中添加：

```json
{
  "agent_servers": {
    "aicoding": {
      "command": "/opt/my_db/aicoding/aicoding.sh",
      "args": ["--acp", "--project", "/your/project"],
      "env": {
        "OPENAI_API_KEY": "sk-...",
        "OPENAI_MODEL": "gpt-4o"
      }
    }
  }
}
```

然后在 Zed 中打开 Agent Panel（`cmd-alt-o` / `ctrl-alt-o` 或命令面板 → `agent: new thread`），选择 aicoding，开始对话。

### 4. 快速验证

```bash
# ACP 模式手动测试
echo '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":1}}' \
  | ./aicoding.sh --acp --project /tmp
```

---

## 上下文管理

与原生 opencode 的本质区别：aicoding **从根本上解决了**上下文窗口无限增长的问题。

### 核心设计

- **上下文有界**：默认 16K tokens（`OPENCODE_CONTEXT_TOKENS` 调整），永不膨胀。
- **近期对话保留**：至少保留最近 4 轮完整的 user/assistant/tool 对话组。
- **旧消息归档**：更老的对话自动移入 KV Cache `/agent/{session}/history/`，7 天 TTL。
- **按需召回**：被移出窗口的信息不丢失，模型通过 `kv_search` / `kv_get` 随时读取。
- **自动摘要**：每轮对话结束自动提取摘要和关键事实到 KV Cache，跨会话可回忆。
- **跨 session 预热**：新 session 启动时自动读取上次的摘要和 facts。

### 模型需要知道的记忆规则

系统 prompt 已明确指示模型：

1. 不要依赖 prompt 记住跨越多轮的信息。
2. 学到重要事实时，主动用 `kv_set` 写回 `/agent/{session}/facts`。
3. 需要旧信息时，主动用 `kv_search` / `kv_get` 召回。
4. 读完代码后，把关键结论写成原子化事实，避免重复读取。

---

## Agent 工作流

### Plan Agent

自主拆解任务 → 按依赖顺序执行 → Build Agent 验证 → 失败重规划。

```
/plan 为 lib.lua 添加一个 greet 函数并确保编译通过
```

流程：
1. LLM 生成 JSON 格式的计划（`[{description, depends_on, verify}, ...]`）
2. 按依赖拓扑排序执行
3. 每步标记 `verify=true` 的步骤自动调用 Build Agent
4. 失败时重新生成剩余计划，尝试恢复

### Build Agent

编译-诊断-修复循环：

```
/build
/build 修复所有警告并运行测试
```

流程：
1. 自动检测构建系统（cargo/npm/make/cmake/go）
2. 运行编译
3. 失败 → 把错误给 LLM → 生成修复 → 重新编译（最多 3 次）

---

## 记忆系统

底层基于 my_db KV Cache，对 LLM 暴露为 `kv_*` 系列工具。

### 命名空间约定

| 命名空间 | 用途 | 读写 |
|----------|------|------|
| `/code/local/{repo}` | 代码库语义记忆 | 只读（由 `analyze_repo.sh` 导入） |
| `/agent/{session}/facts` | 当前会话事实 | 读写 |
| `/agent/{session}/history` | 历史消息归档 | 读写 |
| `/agent/{session}/summaries` | 会话摘要 | 读写 |
| `/project/{basename}/knowledge` | 项目级知识文件 | 读写 |

### 代码语义搜索

已预索引的代码库：Linux Kernel、Nginx、Redis、CPython、LuaJIT、PHP、OpenResty、llama.cpp、ComfyUI 等。

```lua
-- 在 Lua 中搜索
local mem = require("memory")
mem.search("memory allocation", {
    namespace = "/code/local/linux",
    search_type = "semantic",
    top_k = 5
})
```

### 符号上下文查询

```lua
local ok, out = mem.context("schedule", "/code/local/linux", { depth = 2 })
```

---

## 主要工具

| 工具 | 用途 |
|------|------|
| `read` | 读取文件/目录/图片，支持 `offset`/`limit` 分页 |
| `edit` | 精确替换文件内容 |
| `write` | 创建或覆盖文件 |
| `apply_patch` | 应用 unified diff 补丁 |
| `glob` | 按 glob 模式列出文件 |
| `grep` | 搜索文件内容 |
| `bash` | 执行 shell 命令 |
| `build` | 自动检测构建系统并编译 |
| `web_fetch` | 抓取外部 URL 内容 |
| `kv_search` | 语义/前缀/正则/标签搜索 KV Cache |
| `kv_get` / `kv_set` | 精确读写 KV Cache |
| `kv_context` | 符号上下文 (caller/callee/call-paths) |
| `knowledge_read/write/search` | 项目级知识文件 |
| `trace_query` | 查询执行轨迹 |
| `plugin_create/load/list` | Lua 插件热加载 |
| `code_index` | 索引新代码库到 KV Cache |

---

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `OPENAI_API_KEY` | — | LLM API 密钥 |
| `OPENAI_BASE_URL` | `https://api.openai.com/v1` | API 基础地址 |
| `OPENAI_MODEL` | `kimi-latest` | 模型名称 |
| `LLM_PROTOCOL` | `openai` | `openai` 或 `anthropic` |
| `LLM_TEMPERATURE` | `1.0` | 采样温度 |
| `OPENCODE_CONTEXT_TOKENS` | `16384` | 上下文窗口上限 |
| `OPENCODE_SESSION` | `default` | Session ID |
| `OPENCODE_LOG_LEVEL` | `info` | 日志级别 |
| `ACP_CWD` | `$HOME` | ACP 模式工作目录 |

---

## 与原生 opencode 的差异

| 维度 | 原生 opencode | aicoding |
|------|--------------|----------|
| 运行时 | Bun/Node.js + Electron + TUI | C + LuaJIT 单二进制 |
| UI | 自建 TUI (SolidJS) | **Zed Agent Panel (零 UI 工作量)** |
| 上下文 | 不断增长，依赖 compaction | **有界窗口 + KV Cache 按需召回** |
| 持久化 | SQLite | mmap KV Cache |
| 工具定义 | TypeScript 硬编码 | Lua 脚本，热更新 |
| 协议 | 内部 | **ACP 标准协议，接入任何 ACP 编辑器** |
| Agent 扩展 | 改主程序 | Lua 插件，热加载 |

---

## 目录结构

```
aicoding/
├── cli.c              # 主入口：--acp ACP 模式 / REPL
├── session.c/h        # Session namespace on KV Cache
├── lua_engine.c/h     # LuaJIT 宿主 + C 绑定 (含 acp_send)
├── llm_client.c/h     # OpenAI/Anthropic HTTP 客户端
├── main.lua           # ACP 入口 (acp_chat) + 聊天循环
├── memory.lua         # 记忆系统封装
├── context.lua        # 滑动窗口上下文压缩
├── prompts/
│   └── default.lua    # 系统提示组装 + 事实注入
├── tools/
│   └── default.lua    # 工具定义与调度
├── agents/
│   ├── build.lua      # 自主 Build-Fix Agent
│   └── plan.lua       # 自主 Plan Agent
├── summarize.lua      # 自动摘要 + 事实提取
├── permissions.lua    # 权限规则引擎
├── knowledge.lua      # 项目级知识记忆
├── shell.lua          # 安全 shell 执行
├── json.lua           # JSON 编解码
├── tokens.lua         # Token 估算
├── rerank.lua         # 搜索结果重排序
├── compress.lua       # System prompt 压缩
├── conventions.lua    # 项目类型自动检测
├── trace.lua          # 结构化执行轨迹
├── aicoding.sh        # 启动 wrapper (设 LD_LIBRARY_PATH)
└── design.md          # 详细设计文档
```

---

## 参考文档

### Zed 集成相关

aicoding 通过 **Custom External Agent** 方式接入 Zed，无需开发 Rust/WASM 扩展：

- [Zed External Agents 文档](https://zed.dev/docs/ai/external-agents) — 如何在 Zed 中注册外部 ACP agent
- [Zed Agent Settings](https://zed.dev/docs/ai/agent-settings) — `agent_servers` 配置详解
- [Zed Agent Panel](https://zed.dev/docs/ai/agent-panel) — Agent Panel 使用方法
- [Zed Extensions 开发文档](https://zed.dev/docs/extensions/developing-extensions) — 如需发布为 Zed 扩展，参考此文档

### ACP 协议 (Agent Client Protocol)

aicoding 的 `--acp` 模式实现了 ACP 协议的服务端，通信基于 **JSON-RPC 2.0 over stdio**：

- [ACP 协议概述](https://agentclientprotocol.com/get-started/introduction) — ACP 是什么、为什么这么设计
- [ACP 协议规范](https://agentclientprotocol.com/protocol/v1/overview) — 协议详细说明
- [初始化](https://agentclientprotocol.com/protocol/v1/initialization) — `initialize` 握手与能力协商
- [Session 管理](https://agentclientprotocol.com/protocol/v1/session-setup) — `session/new` / `session/prompt` / `session/cancel`
- [Tool Calls](https://agentclientprotocol.com/protocol/v1/tool-calls) — 工具调用生命周期（pending → in_progress → completed）
- [File System](https://agentclientprotocol.com/protocol/v1/file-system) — 文件读写方法
- [Terminals](https://agentclientprotocol.com/protocol/v1/terminals) — 终端执行方法
- [Schema](https://agentclientprotocol.com/protocol/v1/schema) — 完整的 JSON Schema 定义
- [ACP Registry](https://agentclientprotocol.com/get-started/registry) — ACP Agent 注册中心

### my_db 基础设施

- [`/opt/my_db/coding.md`](../coding.md) — 代码探索记忆系统：语义搜索、调用图、数据流追踪、超大代码库索引
- [`/opt/my_db/kvCache.md`](../kvCache.md) — my_db KV Cache：层级命名空间、持久化、多维搜索

---

*基于 my_db KV Cache + Zed Agent Panel + ACP 协议构建。*
