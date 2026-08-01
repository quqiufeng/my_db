# aicoding — AI Coding Agent

一个面向开发者的 AI Coding Agent，**UI 使用 Rust 客户端 agent-rs**（终端 REPL，后续演进为 TUI/Web），核心 AI 能力（无限上下文、代码搜索、Plan/Build Agent、权限系统）基于 C + LuaJIT + my_db KV Cache 实现。

> 📚 **相关基础设施文档**：
> - [`/opt/my_db/coding.md`](../coding.md) — 代码探索记忆系统：语义搜索、调用图、数据流追踪。
> - [`/opt/my_db/kvCache.md`](../kvCache.md) — my_db KV Cache：层级命名空间、持久化、多维搜索。

---

## 架构

```
┌───────────────────────────────────────────────────────────┐
│  agent-rs (Rust 客户端, agent-rs/)                        │
│  ┌─────────────────────────────────────────────────────┐  │
│  │  crates/cli  终端 REPL (流式文本/工具状态渲染)        │  │
│  │  crates/acp  ACP 协议客户端 (JSON-RPC over stdio)    │  │
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
| **UI** | agent-rs (Rust) | 终端 REPL（流式文本、工具调用可视化），后续演进为 TUI/Web |
| **协议** | ACP (Agent Client Protocol) | JSON-RPC over stdio，agent-rs ↔ aicoding 通信 |
| **AI 引擎** | C + LuaJIT | LLM 调用、工具执行、上下文管理、Agent 逻辑 |
| **持久化** | my_db (C) | mmap 零拷贝 KV Cache，向量索引，代码搜索 |

### 为什么这样设计

| 传统方案 | 本方案 |
|----------|--------|
| Electron + Node.js 运行时 | C + LuaJIT 单二进制 |
| 自建完整前端（编辑器+UI 全栈） | **引擎/UI 分离：C 引擎 + Rust 客户端，通过 ACP 协议解耦** |
| 上下文靠 messages 数组无限增长 | 有界窗口 + LRU 归档 + KV Cache 按需召回 |
| 工具硬编码在 TypeScript 里 | Lua 脚本定义，热更新 |
| 依赖 npm 生态 | 只依赖系统已有的 `libmydb.so` / `cjson.so` |

---

## 核心特性

| 特性 | 说明 |
|------|------|
| **Rust 客户端** | agent-rs 通过 ACP 协议连接引擎，终端流式渲染，引擎/UI 完全解耦 |
| **独立二进制** | 编译成单个 `aicoding`，无 Node.js / Bun / Electron 依赖 |
| **无限上下文** | 记忆无限（KV Cache 永不丢失），视野宽度可调，默认 64K 只决定单次请求装载量 |
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

> aicoding 的所有会话数据、记忆、知识库保存在项目根目录的 `.opencode/` 下，与 opencode 兼容。

### 2. 配置 LLM 密钥

aicoding 在启动时按以下优先级加载配置（后加载的覆盖前面的）：

1. `/etc/aicoding/.env` — 系统级配置
2. `~/.aicoding/.env` — 用户全局配置（推荐）
3. `./.env` 或 `--env FILE` — 项目级配置
4. 环境变量

推荐使用用户全局配置：

```bash
mkdir -p ~/.aicoding
cat > ~/.aicoding/.env << 'EOF'
# DeepSeek（默认，配合 --model deepseek-v4-flash）
DEEPSEEK_API_KEY=sk-...
DEEPSEEK_BASE_URL=https://api.deepseek.com

# 或使用 Kimi
# OPENAI_API_KEY=sk-kimi-...
# OPENAI_BASE_URL=https://api.kimi.com/coding/v1
# LLM_USER_AGENT=KimiCLI/1.0.0

# 或使用 OpenAI
# OPENAI_API_KEY=sk-...
# OPENAI_BASE_URL=https://api.openai.com/v1

# 通用
LLM_PROTOCOL=openai
LLM_TEMPERATURE=1.0
EOF
```

支持的所有提供商见 `.env.example`。

### 3. 使用 agent-rs（Rust 客户端）

#### 3.1 编译客户端

```bash
cd /opt/my_db/aicoding/agent-rs
cargo build --release
```

#### 3.2 启动

```bash
AICODING_ENV_FILE=~/.aicoding/deepseek.env AICODING_MODEL=deepseek-v4-flash \
  ./agent-rs/target/release/agent-cli --project <项目路径>
```

agent-rs 启动引擎进程（`--acp` 模式），握手后进入交互 REPL：

- 输入消息回车发送，assistant 回复流式渲染
- 工具调用显示为 `tool[id] name (kind) ...` → `tool[id] -> completed`
- `Ctrl-C` 取消当前回合（再按一次强制退出），`/quit` 退出

`AICODING_ENV_FILE` 指定引擎的 LLM 配置文件（`--env` 参数），`AICODING_MODEL` 指定模型（必须存在于 `models.json`）。

#### 3.3 常用命令

| 输入 | 效果 |
|------|------|
| `帮我看一下这个项目的架构` | aicoding 会读取文件、搜索代码、分析结构 |
| `/plan 给 main.go 添加一个 HTTP 路由` | 运行 Plan Agent，自动拆解任务并逐步执行 |
| `/build 修复所有编译警告` | 运行 Build Agent，编译-诊断-修复循环 |
| `用 kv_search 搜索 memory pool 相关的函数` | 在已索引的代码库中做语义搜索 |

### 4. 快速验证（直接测试引擎协议）

```bash
echo '{"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":1}}' \
  | /opt/my_db/aicoding/aicoding.sh --acp --project /tmp
```

正常输出应包含 `"protocolVersion":1` 和 `"agentInfo"`。

### 5. 多模型切换

aicoding 内置 2 个模型（`/opt/my_db/aicoding/models.json`）：`deepseek-v4-flash`（快速免费）和 `deepseek-v4-pro`（高质量）。可在 `~/.aicoding/models.json` 中添加自定义模型：

```json
[
  { "name": "my-model", "provider": "openai", "model": "gpt-4o" }
]
```

启动时通过 `--model` 指定（agent-rs 用 `AICODING_MODEL` 环境变量传入）：

```bash
./aicoding.sh --acp --project . --model deepseek-v4-flash
```

---

## 记忆系统

aicoding 有两层存储，分工不同：

### 1. 会话记忆 — 项目本地

保存在 `<项目根>/.opencode/` 的 KV Cache 中，大小只受磁盘限制：

| 组件 | 位置 | 容量 |
|------|------|------|
| 对话历史 + facts + summaries | `.opencode/cache.bin` + `index.bin` | **无上限**，只受磁盘限制 |
| 旧消息归档 | 同上，按 namespace 划分 | 自动管理 |

**与 opencode 的本质区别：**

opencode 的上下文依赖一个不断增长的 `messages` 数组。项目越大、对话越长，数组就越接近模型上限（128K-200K）。到了上限就必须压缩截断，丢失信息。**大项目开发到一半就卡住了，因为之前的上下文全部丢失。**

aicoding 的上下文**本质上没有限制**——全部内容都在 KV Cache 记忆里。所谓"上下文窗口"只是一个**数字**（`OPENCODE_CONTEXT_TOKENS`，默认 64K），它决定的不是"能记住多少"，而是"每次请求的视野宽度"——当前视野之外的记忆仍在，随时按需召回：

```text
每次 LLM 请求：
+------------------------------------------+
|  视野宽度 (可调数字, 默认 64K)             |
|  +---------+----------+--------------+   |
|  | system  | 最近 N 轮 | 召回的内容    |   |
|  | prompt  | 对话      |              |   |
|  +---------+----------+--------------+   |
|       视野外的内容 → 仍在 KV Cache 中     |
+------------------------------------------+
                      ▲
                      │ 按需召回 (kv_search / kv_get / read)
                      │
  KV Cache (.opencode/, 无大小限制)  ← 完整记忆
  +----------------------------------+
  | 全部历史对话 / facts / 归档       |
  | 项目知识 / 会话摘要 / 代码索引     |
  +----------------------------------+
```

记忆是无限的，视野是当前快照。项目再大、对话再长，**永远不会丢失**——因为上下文从来不是"装不下的东西"，只是"此刻没看而已"。

### 2. 代码搜索 — 全局共享

保存在 `/opt/code_caches/` 和 `/memory/` 中，由 `analyze_repo.sh` 预索引：

| 组件 | 位置 | 说明 |
|------|------|------|
| 语义向量 + HNSW | `/opt/code_caches/xxx_cache/vectors/` | GPU 加速，自然语言搜代码 |
| 代码 chunks/符号 | `/memory/cache.bin` | 所有已索引项目的全量代码 |

### 视野宽度机制 (context.lua)

视野宽度默认 64K tokens，由 `OPENCODE_CONTEXT_TOKENS` 调整——它只是**每次请求装载多少记忆**的参数，不是记忆上限：

- **视野可调**：随模型上下文能力自由调整（128K 模型可设 128K），只影响成本不影响记忆
- **保留最近**：至少保留最近 4 轮完整的 user/assistant/tool 对话组
- **自动归档**：视野之外的消息仍在 KV Cache `/agent/{session}/history/`（7 天 TTL），**不删除**
- **按需召回**：模型通过 `kv_search` / `kv_get` / `read` 随时把记忆装回视野
- **自动摘要**：每轮对话结束自动提取摘要和关键事实到 KV Cache
- **跨 session 预热**：新 session 启动时自动读取上次的摘要和 facts

### 模型需要遵守的记忆规则

系统 prompt 已明确指示模型：

1. 不要依赖 prompt 记住跨越多轮的信息
2. 学到重要事实时，主动用 `kv_set` 写回 `/agent/{session}/facts`
3. 需要旧信息时，主动用 `kv_search` / `kv_get` 从 KV Cache 召回
4. 读完代码后，把关键结论写成原子化事实，避免重复读取

这些规则让模型自己参与记忆管理，而不是被动依赖一个越来越长的 prompt。
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
| `OPENAI_MODEL` | `deepseek-v4-flash` | 模型名称 |
| `LLM_PROTOCOL` | `openai` | `openai` 或 `anthropic` |
| `LLM_TEMPERATURE` | `1.0` | 采样温度 |
| `OPENCODE_CONTEXT_TOKENS` | `16384` | 单次请求视野宽度（非记忆上限） |
| `OPENCODE_SESSION` | `default` | Session ID |
| `OPENCODE_LOG_LEVEL` | `info` | 日志级别 |
| `ACP_CWD` | `$HOME` | ACP 模式工作目录 |

---

## 与原生 opencode 的差异

| 维度 | 原生 opencode | aicoding |
|------|--------------|----------|
| 运行时 | Bun/Node.js + Electron + TUI | C + LuaJIT 单二进制引擎 + Rust 客户端 |
| UI | 自建 TUI (SolidJS) | **agent-rs (Rust)：终端 REPL → TUI/Web** |
| 上下文 | 不断增长，依赖 compaction | **记忆无限 + 可调视野 + KV Cache 按需召回** |
| 持久化 | SQLite | mmap KV Cache |
| 工具定义 | TypeScript 硬编码 | Lua 脚本，热更新 |
| 协议 | 内部 | **ACP 标准协议，引擎/UI 解耦** |
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
├── models.json        # 内置模型列表
├── agent-rs/          # Rust 客户端
│   ├── crates/
│   │   ├── acp/       # ACP 协议类型 + 进程客户端 (JSON-RPC / 事件流)
│   │   └── cli/       # 终端 REPL (流式渲染、工具状态、取消)
└── design.md          # 详细设计文档
```

---

## 参考文档

### ACP 协议 (Agent Client Protocol)

aicoding 的 `--acp` 模式实现了 ACP 协议的服务端，通信基于 **JSON-RPC 2.0 over stdio**，agent-rs 是参考该协议实现的客户端：

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

*基于 my_db KV Cache + C/LuaJIT 引擎 + agent-rs Rust 客户端 + ACP 协议构建。*
