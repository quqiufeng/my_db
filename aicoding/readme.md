# aicoding

一个面向开发者、对齐 [opencode](https://opencode.ai) 体验的 AI Coding Agent。核心定位是**小而精、可编译成独立二进制**——不依赖 Node.js / Bun / Python 运行时，一个可执行文件即可运行。

> 当前仓库：`quqiufeng/my_db/aicoding`
>
> 📐 **GUI 架构说明**：[Rust + LuaJIT FFI 的 GUI 开发模式](reference/gui-architecture.md) —— Rust/gpui-component 渲染引擎编译为 `.so`，LuaJIT 通过 C ABI 驱动界面，内容更新与 LLM 交互全部在 Lua 层完成。
>
> 📚 **相关基础设施文档**：
> - [`/opt/my_db/coding.md`](../coding.md) —— 代码探索记忆系统：语义搜索、调用图、数据流追踪、超大代码库索引。
> - [`/opt/my_db/kvCache.md`](../kvCache.md) —— my_db KV Cache 存储系统：层级命名空间、持久化、多维搜索。

---

## 一句话介绍

`aicoding` 是一个复刻 opencode 核心工作流的本地 AI 编程助手：读取项目、编辑文件、运行 bash/git、管理权限，全部通过大模型工具调用完成。与原生 opencode 的本质区别在于上下文系统——它**解决了**上下文窗口无限增长的问题：用 **my_db KV Cache** 替代不断增长的 `messages` 数组，把长期知识转移到本地持久化记忆中，实现按需召回。

---

## 底层基础设施

本 agent 不是从零构建，而是直接建立在 `/opt/my_db` 已完成的两个核心系统之上：

### 1. 代码探索记忆系统 [`coding.md`](../coding.md)

把任意代码仓库（从几百文件的小项目到 Linux 内核级别的超大项目）转换为可自然语言查询的智能记忆库。

- **语义搜索**：基于 Jina embeddings + HNSW 近似索引，用自然语言找代码。
- **调用图分析**：自动构建 caller/callee 关系，支持符号上下文查询。
- **数据流追踪**：字段级 + 跨函数变量生命周期追踪。
- **超大项目支持**：Linux 内核（5万+文件）已预索引到 `/opt/code_caches/linux_cache`。
- **完全本地运行**：代码不离开本地机器，GPU 加速（TensorRT + CUDA）。

agent 中的 `kv_search` / `kv_context` / `code_index` 工具直接依赖这套系统。

### 2. KV Cache 记忆存储 [`kvCache.md`](../kvCache.md)

专为 AI Agent 设计的层级化记忆存储系统。

- **层级命名空间**：如 `/code/linux/symbols/schedule`、`/agent/default/tools`，像文件系统一样组织知识。
- **磁盘持久化**：mmap + `msync(MS_SYNC)`，进程重启不丢失。
- **多维搜索**：前缀、范围、正则、标签、模糊（Levenshtein）搜索。
- **TTL 生命周期**：支持记忆过期，区分工作记忆 / 短期记忆 / 长期记忆。
- **大文件引用**：精华存 KV，原始资料存文件路径，自动关联。

agent 运行时的所有项目知识、代码索引、会话事实都落盘在这个 KV Cache 中。

---

## 核心特性

| 特性 | 说明 |
|------|------|
| **独立二进制** | 编译成单个 `aicoding`，无需 Node.js / Bun / Electron |
| **KV Cache 记忆** | 项目知识、代码索引、会话事实全部落盘，进程重启不丢失 |
| **工具调用** | 读文件、编辑文件、运行 bash/git、搜索代码记忆、编译验证 |
| **权限系统** | 兼容 opencode 的 `permissions` 规则，支持 `allow`/`deny`/`ask`，默认保护系统与用户目录 |
| **统一工具基础模块** | `shell.lua` / `json.lua` / `tokens.lua` 统一处理命令引用、JSON 编解码、token 估算 |
| **项目类型约定注入** | 自动检测 linux_kernel/cargo/npm/python/go/cmake 等项目类型，注入对应的最佳实践 workflow、检查清单、常见错误 |
| **安全护栏** | 默认禁止写/删 `/usr`、`/etc`、`/bin`、`/sbin`、`/lib*`、`/opt/my_db`、`~/*`；拦截危险 bash 模式 |
| **LuaJIT 脚本层** | Prompt、工具定义、调度逻辑、Agent 全部用 Lua 编写，改逻辑不重编译 |
| **实验性 GUI** | Rust/gpui-component 编译为 `.so`，LuaJIT FFI 驱动，可弹出聊天窗口 |
| **代码语法高亮** | GUI 代码块使用 syntect 按语言着色 |
| **Markdown 基础渲染** | GUI 文本消息支持标题、加粗、斜体、行内代码、无序列表 |
| **Diff 分栏视图** | `diff` 代码块自动解析为左右 side-by-side，红删绿增 |
| **搜索结果本地重排序** | `kv_search` 返回的结果经 `rerank.lua` 本地重排，可用 `min_score` 过滤噪声 |
| **自动会话摘要** | 每轮对话结束自动提取摘要和关键事实写入 KV Cache |
| **项目级知识记忆** | `.opencode/knowledge/` 下的文件自动注入系统提示并同步到 KV Cache，跨会话共享 |
| **符号上下文深度控制** | `kv_context` 支持 `depth` 控制调用链展开深度，`batch` 批量查询多个符号 |
| **结构化执行轨迹** | 自动记录 agent_start / llm_request / tool_call / tool_result / error 到 `.opencode/traces/{session}.jsonl` |
| **轨迹查询工具** | `trace_query` 工具让模型/开发者自省本轮执行过程 |
| **回归测试套件** | `make regression` 一键运行 C 单元测试 + 全部 GUI 自动化测试 |
| **自主 Plan Agent** | `/plan <desc>` 自动拆解任务、逐步执行、失败重规划 |
| **自主 Build Agent** | `/build [goal]` 触发编译-诊断-修复循环，失败自动重试 |
| **Web Fetch 工具** | 抓取外部文档/API/RFC，供模型实时参考 |
| **可编程 GUI 测试** | 自动填输入、触发提交、断言 LLM 输出，支持批量回归测试 |
| **预索引第三方代码** | 用现有 `analyze_repo.sh` / `typescript-indexer` 索引进 KV Cache，避免误读依赖 |

---

## 技术架构：小而精

```
┌─────────────────────────────────────────┐
│  aicoding (单个二进制)               │
│  - C 内核                                │
│  - LuaJIT 脚本层                         │
└─────────────────────────────────────────┘
           │
           ▼
┌─────────────────────────────────────────┐
│  C 内核                                  │
│  - KV Cache 读写 (libmydb.so)            │
│  - Session namespace 管理                │
│  - OpenAI-compatible LLM HTTP client     │
│  - LuaJIT 宿主                            │
└─────────────────────────────────────────┘
           │
           ▼
┌─────────────────────────────────────────┐
│  LuaJIT 脚本层 (动态，用户可改)           │
│  - prompts/default.lua  系统提示组装      │
│  - context.lua          上下文窗口压缩    │
│  - memory.lua           记忆系统封装      │
│  - rerank.lua           KV 搜索结果重排序 │
│  - summarize.lua        会话摘要与事实提取 │
│  - knowledge.lua        项目级知识记忆     │
│  - trace.lua            结构化执行轨迹     │
│  - shell.lua            POSIX shell 引用与安全命令执行 │
│  - json.lua             统一 JSON 编解码与错误处理 │
│  - tokens.lua           统一 token 估算器 │
│  - conventions.lua      项目类型自动检测与最佳实践注入 │
│  - tools/default.lua    工具定义与调度    │
│  - permissions.lua      权限规则解析与安全护栏 │
│  - agents/build.lua     自主 Build Agent │
│  - agents/plan.lua      自主 Plan Agent  │
│  - gui.lua              GUI FFI 封装      │
└─────────────────────────────────────────┘
           │
           ▼
┌─────────────────────────────────────────┐
│  libaicoding_gui.so (可选，Rust/gpui)    │
│  - 聊天窗口                               │
│  - 消息列表 / 输入框 / 工具输出           │
└─────────────────────────────────────────┘
```

### 为什么这样设计

| 传统方案 | 本方案 |
|----------|--------|
| Electron + Node.js 运行时，启动慢、体积大 | C + LuaJIT，编译成单个二进制 |
| 上下文靠 `messages` 数组，需要复杂 compaction | 滑动窗口 + LRU 归档 + KV Cache 永久记忆 |
| 工具/schema 硬编码在 TypeScript 里 | Lua 脚本定义，热更新 |
| Agent 能力需要改主程序 | Lua 插件即可新增 agent，热加载 |
| 依赖 npm 生态 | 只依赖系统已存在的 `libmydb.so` / `cjson.so` |

---

## 快速开始

### 1. 编译

```bash
cd /opt/my_db/aicoding
make
```

生成：

- `aicoding` — 主程序
- `libaicoding_agent.a` — C 内核静态库

### 2. 配置

复制 `.env.example` 为 `.env`，填写你的 LLM 密钥：

```bash
LLM_PROTOCOL=openai
OPENAI_API_KEY=sk-...
OPENAI_BASE_URL=https://api.kimi.com/coding/v1
OPENAI_MODEL=kimi-latest
LLM_USER_AGENT=opencode/1.17.6 ai-sdk/provider-utils/4.0.27 runtime/bun/1.3.14
LLM_EXTRA_HEADER=x-opencode-version: 1.17.6
LLM_TEMPERATURE=1.0
```

> `.env` 已被 `.gitignore` 排除，不会提交。

### 3. 运行 CLI

```bash
./aicoding --project /path/to/your/repo
```

### 4. 运行 GUI（实验性）

```bash
# 先编译 GUI .so
cd gui_gpui && cargo build --release && cd ..
cp gui_gpui/target/release/libopencode_gui.so ./libaicoding_gui.so

# 启动 GUI 模式
OPENCODE_GUI=1 ./aicoding --project /path/to/your/repo
```

---

## 目录结构

```
aicoding/
├── cli.c              # 主入口 / REPL / GUI 分支
├── session.c/h        # Session namespace on KV Cache
├── lua_engine.c/h     # LuaJIT 宿主 + C 绑定
├── llm_client.c/h     # OpenAI/Anthropic HTTP client
├── main.lua           # Lua 运行时入口
├── memory.lua         # 记忆系统封装（read/write/search/context）
├── permissions.lua    # 权限规则引擎
├── prompts/
│   └── default.lua    # 系统提示组装
├── tools/
│   └── default.lua    # 工具定义与调度
├── agents/
│   └── build.lua      # 自主 Build-Fix Agent
├── gui.lua            # LuaJIT FFI 封装 GUI
├── gui_gpui/          # Rust/gpui-component GUI 后端
│   ├── Cargo.toml
│   └── src/lib.rs
├── tests/             # 可编程 GUI 自动化测试
├── design.md          # 详细设计文档
└── README.md          # 使用说明（本文件）
```

---

## 主要工具

| 工具 | 用途 |
|------|------|
| `read` | 读取文件、目录、图片；支持 `offset`/`limit` 分页；自动检测二进制 |
| `edit` | 精确替换文件内容（旧字符串必须唯一匹配） |
| `write` | 创建或覆盖文件；支持 `create_only`/`append`/`expected_hash` 条件写入 |
| `apply_patch` | 应用 unified diff 补丁 |
| `glob` | 按 glob 模式列出文件 |
| `grep` | 用 ripgrep 搜索文件内容 |
| `file_delete` | 删除文件，支持 `expected_hash` 校验 |
| `bash` | 执行 shell 命令 |
| `git` | 执行 git 命令 |
| `diff` | 查看 git diff |
| `build` | 自动检测构建系统并编译；失败时把输出反馈给 LLM 修复 |
| `web_fetch` | 抓取外部 URL 内容（文档/API/RFC），返回 text/html/markdown |
| `kv_search` | 搜索 KV Cache；支持 `semantic`/`prefix`/`regex`/`tag`，可对已索引代码库做自然语言语义搜索；结果本地重排，可设 `min_score` 过滤 |
| `kv_get` | 读取精确 KV 键 |
| `kv_set` | 写入记忆（事实、历史、中间结论），支持 TTL |
| `kv_context` | 获取符号上下文（caller/callee/call-paths），支持 `depth` 和 `batch` |
| `knowledge_read` | 读取 `.opencode/knowledge/` 下的项目级知识文件 |
| `knowledge_write` | 写入项目级知识文件并同步到 KV Cache |
| `knowledge_search` | 对项目级知识做语义搜索 |
| `trace_query` | 查询本轮会话的结构化执行轨迹 |
| `plugin_create` | 创建 Lua 插件，动态添加新工具 |
| `plugin_load` | 热加载插件，无需重启 |
| `plugin_list` | 列出可用插件 |
| `code_index` | 索引新代码库到 KV Cache |

---

## 上下文窗口：问题已解决

与原生 opencode 的本质不同：原生 opencode 仍然受限于一个不断增长的 `messages` 数组，最终必须压缩、截断、丢失信息。aicoding **从根本上解决了这个问题**——LLM API 只接收一个**有界**的上下文窗口，所有长期知识都保存在本地的 KV Cache 或源文件中，需要时通过 tool call 召回。

### 核心设计

- **上下文有界**：默认 16k tokens（可用 `OPENCODE_CONTEXT_TOKENS` 调整），永远不会膨胀。
- **系统提示精简**：只放当前任务必须遵守的规则和少量关键事实。
- **近期对话保留**：至少保留最近 4 轮完整的 user/assistant/tool 对话组。
- **旧消息归档**：更老的对话自动移入 KV Cache `/agent/{session}/history/`，永久保存。
- **按需召回**：被移出窗口的信息不会丢失，模型通过 `kv_search` / `kv_get` / `read` 随时读取。
- **自动摘要**：每轮对话结束自动提取摘要和关键事实到 KV Cache，供未来会话回忆。
- **项目知识共享**：`.opencode/knowledge/` 下的文档自动进入系统提示，并可通过 `knowledge_search` 召回。

### 这不是缓解，是解决

原生 opencode 的 `messages` 会增长 → 压缩 → 失忆 → 用户重复说明。  
aicoding 的 `messages` 永远 bounded → 旧信息去 KV Cache → 需要时召回 → 不丢失、不重复。

### 模型需要知道的记忆规则

系统 prompt 已明确指示模型：

1. 不要依赖 prompt 记住跨越多轮的信息。
2. 学到重要事实时，主动用 `kv_set` 写回 `/agent/{session}/facts`。
3. 需要旧信息时，主动用 `kv_search` / `kv_get` 从 KV Cache 召回。
4. 读完代码后，把关键结论写成原子化事实，避免重复读取。

这些规则让模型自己参与记忆管理，而不是被动依赖一个越来越长的 prompt。

---

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `OPENAI_API_KEY` | — | LLM API 密钥 |
| `OPENAI_BASE_URL` | `https://api.openai.com/v1` | API 基础地址 |
| `OPENAI_MODEL` | `kimi-latest` | 模型名称 |
| `LLM_PROTOCOL` | `openai` | `openai` 或 `anthropic` |
| `LLM_USER_AGENT` | — | HTTP User-Agent，Kimi 接口需要设置为 `KimiCLI/1.0.0` |
| `LLM_EXTRA_HEADER` | — | 额外请求头，如 `x-opencode-version: 1.17.6` |
| `LLM_TEMPERATURE` | `1.0` | 采样温度 |
| `OPENCODE_CONTEXT_TOKENS` | `16384` | 上下文窗口上限 |
| `OPENCODE_COMPRESS_PROMPT` | 自动 | `1` 强制启用 system prompt 压缩，`0` 禁用 |
| `OPENCODE_AUTO_SUMMARIZE` | — | 设置为 `0` 禁用每轮结束后的自动 LLM 摘要 |
| `OPENCODE_GUI` | — | 设置为 `1` 启用 GUI 模式 |
| `OPENCODE_ALLOW_ALL` | — | 设置为 `1` 自动允许所有权限询问（测试用） |
| `OPENCODE_GUI_TEST_MSG` | — | GUI 启动后自动发送的测试消息 |
| `OPENCODE_GUI_TEST_SCRIPT` | — | 可编程 GUI 测试脚本路径 |
| `OPENCODE_SESSION` | `default` | Session ID |

> 环境变量前缀保留 `OPENCODE_` 以兼容 opencode 生态。

---

## LLM Token Compression（可选）

除了把长期知识转移到 KV Cache 之外，本项目还支持对超长 system prompt 做 **LLM Token Compression**：用固定简写词表把 system prompt 编码，前面附带解码算法和码表，让模型在收到后自行解码。

### 为什么有用

- 上下文越大，固定码表的摊销成本越低，压缩率越明显。
- 对包含大量工具 schema、代码示例、重复技术术语的 system prompt 效果尤其好。
- 短 prompt 自动跳过，不会额外增加 overhead。

### 开关

```bash
# 自动：system prompt 超过 8k tokens 时启用
./aicoding --project /path/to/repo

# 强制启用
OPENCODE_COMPRESS_PROMPT=1 ./aicoding --project /path/to/repo

# 禁用
OPENCODE_COMPRESS_PROMPT=0 ./aicoding --project /path/to/repo
```

---

## CLI 命令

在 CLI 模式下，以下斜杠命令可用：

| 命令 | 说明 |
|------|------|
| `/quit` / `/exit` | 退出程序 |
| `/task <desc>` | 设置当前任务描述 |
| `/plan <desc>` | 运行自主 Plan Agent，自动拆解任务并逐步执行 |
| `/build [goal]` | 运行自主 Build-Fix Agent，默认目标为 `make the project compile` |
| `/lua <code>` | 直接执行 Lua 代码 |

示例：

```bash
> /plan add a greet function to lib.lua and verify it builds
> /build
> /build fix compilation warnings and run tests
```

---

## Lua 插件热更新

本 agent 基于 LuaJIT，prompt、tools、permissions、context、agents 都是 Lua 模块。如果某个 tool 缺失，模型可以在对话中自己创建并热加载：

```bash
"创建一个 hello 工具，输入 name，返回 greeting"
```

模型会调用：

1. `plugin_create(name="hello_plugin", code=...)` —— 写 Lua 文件。
2. `plugin_load(name="hello_plugin")` —— 热加载，立即生效。

之后 `hello` 就会出现在可用工具里。无需重启 agent，无需改 TypeScript/编译。

---

## Agent 工作流

aicoding 支持两种内置 Agent，均作为 Lua 插件实现，可通过 `/plan` 和 `/build` 命令触发。

### Plan Agent

`/plan <desc>` 让模型先把任务拆成可执行步骤，再按依赖顺序逐个完成，并把中间结果写入 KV Cache。

工作流程：

1. LLM 生成 JSON 格式的计划（`[{description, depends_on, verify}, ...]`）。
2. 按依赖拓扑排序执行。
3. 每步调用主 `chat_once` 循环完成具体修改。
4. 如果步骤标记 `verify=true`，自动调用 Build Agent 验证。
5. 失败时重新生成剩余计划，尝试恢复。
6. 最终结果以总结形式返回。

示例：

```bash
> /plan add a greet function to lib.lua and verify it builds
```

### Build Agent

`/build [goal]` 进入“编译-诊断-修复”循环：

1. 运行 `build` 工具。
2. 成功 → 结束。
3. 失败 → 把错误输出给 LLM，生成修复 tool calls。
4. 执行修复后回到第 1 步。
5. 最多重试 3 次。

示例：

```bash
> /build
> /build fix all warnings and run tests
```

### Plan + Build 组合

Plan Agent 在执行标记为 `verify` 的步骤时会自动调用 Build Agent，形成：

```
plan → execute step → build verify → fix if needed → next step
```

这是处理“实现一个功能并确保编译通过”类任务的标准流程。

---

## Web Fetch 工具

`web_fetch` 工具让 agent 能抓取外部 URL 内容，用于查 API 文档、RFC、StackOverflow、依赖库最新用法等。

参数：

| 参数 | 类型 | 说明 |
|------|------|------|
| `url` | string | 必填，仅支持 `http`/`https` |
| `format` | string | 可选：`text`（默认）、`html`、`markdown` |
| `timeout` | integer | 可选，默认 30 秒 |
| `max_length` | integer | 可选，默认 8000 字符 |

示例：

```bash
> 查一下 https://example.com 的文档
```

等价 `/lua` 调用：

```lua
local tools = require("tools.default")
local cjson = require("cjson")
local result = tools.dispatch({
    name = "web_fetch",
    arguments = cjson.encode({
        url = "https://example.com",
        format = "text",
        max_length = 4000
    })
})
print(result.content)
```

实现细节：优先使用 `lynx`/`w3m` 把 HTML 转成可读文本；如果系统没有这些工具，则返回原始 HTML 或用 `sed` 简单去标签后的文本。抓取结果会写入 KV Cache `/agent/{session}/web_fetches/{timestamp}` 供后续引用。

---

## 记忆系统使用

底层记忆能力封装在 `memory.lua`，对 LLM 暴露为 `kv_*` 系列工具，对开发者可随时在 `/lua` 命令里调用。

### 1. 自然语言搜索已索引代码库

`kv_search` 支持 `search_type=semantic`，直接对 `/opt/code_caches/{repo}_cache` 做向量语义搜索：

```bash
# 在 LLM 对话中说：
"用 kv_search 以语义搜索方式在 /code/local/linux 里找内存分配相关函数"
```

等价 `/lua` 调用：

```lua
local mem = require("memory")
local results = mem.search("memory allocation", {
    namespace = "/code/local/linux",
    search_type = "semantic",
    top_k = 5
})
for _, r in ipairs(results) do
    print(r.name, r.file, r.line_start, r.score)
end
```

已预索引的代码库包括 Linux Kernel、OpenResty、llama.cpp、LuaJIT、CPython、PHP、opencode 等，见 [`/opt/my_db/coding.md`](../coding.md)。

### 2. 写入/读取会话记忆

```lua
local mem = require("memory")

-- 写入一个事实（当前 session 命名空间下）
mem.fact("user_preference", "uses 2-space indentation")

-- 读取
print(mem.recall("user_preference"))

-- 写入带 TTL 的历史记录（3600 秒后过期）
mem.history("last_action", "refactored kv_search", 3600)

-- 任意 key/value
mem.write("/agent/default/plan/step1", "implement semantic search")
```

### 3. 符号上下文查询

支持调用链展开深度和批量符号查询：

```lua
local ok, out = mem.context("schedule", "/code/local/linux", { depth = 2 })
print(out)

-- 批量查询多个符号
local ok, out = mem.context({"schedule", "wake_up_process"}, "/code/local/linux", { batch = true })
print(out)
```

等价工具调用：

```json
{ "symbol": "schedule", "repo": "/code/local/linux", "depth": 2 }
```

批量查询：

```json
{ "symbol": "schedule,wake_up_process", "repo": "/code/local/linux", "batch": true }
```

### 4. 命名空间约定

| 命名空间 | 用途 | 读写 |
|----------|------|------|
| `/code/local/{repo}` | 代码库语义记忆 | 只读（由 `analyze_repo.sh` 导入） |
| `/agent/{session}/facts` | 当前会话事实 | 读写 |
| `/agent/{session}/history` | 当前会话历史动作 | 读写 |
| `/agent/{session}/plan` | 当前计划/待办 | 读写 |
| `/agent/{session}/build` | Build Agent 运行记录 | 读写 |
| `/project/{basename}/knowledge` | 项目级知识文件 | 读写 |

### 5. 项目级知识记忆

把跨会话都需要的设计决策、架构约定、API 用法等写入 `.opencode/knowledge/`（如 `architecture.md`、`decisions.md`）。

- 启动时自动读取并注入系统 prompt。
- 同步到 KV Cache `/project/{basename}/knowledge/`，支持语义搜索。

```bash
# 对话中
用 knowledge_write 把"本项目的所有工具 handler 都必须返回 {ok, ...}"写入 decisions.md
```

等价 `/lua` 调用：

```lua
local tools = require("tools.default")
tools.dispatch({
    name = "knowledge_write",
    arguments = {
        name = "decisions.md",
        content = "## Tool handler return format\nAll tool handlers must return a table with `ok` field."
    }
})
```

### 6. 结构化执行轨迹

每轮对话的 LLM 请求、工具调用、错误等信息自动写入 `.opencode/traces/{session}.jsonl`。可用 `trace_query` 工具或 `/lua` 查询：

```lua
local tools = require("tools.default")
local r = tools.dispatch({
    name = "trace_query",
    arguments = { event = "tool_call", tool = "edit", limit = 5 }
})
print(require("cjson").encode(r))
```

### 7. 索引新项目到记忆系统

如果要用 agent 探索一个尚未索引的项目，调用 `code_index` 工具即可。底层会调用 `/opt/my_db/analyze_repo.sh`（Node.js/TypeScript 项目则调用 `analyze_nodejs_repo.sh`）完成：代码分块、语义向量生成、调用图/数据流分析、导入 KV Cache。

**对话示例**：

```bash
"索引 /opt/redis 到 /code/local/redis"
```

等价 `/lua` 调用：

```lua
local tools = require("tools.default")
local result = tools.tool_handlers.code_index({
    source = "/opt/redis",
    namespace = "/code/local/redis",
    language = "auto"
})
print(result.output)
```

完成后即可用 `kv_search search_type=semantic namespace=/code/local/redis` 对新项目做自然语言搜索。

### 8. CLI 调试命令

启动 CLI 后可用 `/lua <code>` 直接执行 Lua：

```bash
> /lua local mem = require("memory"); print(mem.recall("user_preference"))
```

Plan 和 Build Agent 也会把执行计划、步骤结果、失败重试记录写入 KV Cache：

```lua
print(mem.read("/agent/default/plan/plan"))      -- 当前计划 JSON
print(mem.read("/agent/default/plan/status"))    -- running / success / failed
print(mem.read("/agent/default/build/status"))   -- build agent 状态
```

---

## 项目类型约定自动注入

启动时，`conventions.lua` 会根据项目根目录的文件特征自动识别项目类型，并将对应的最佳实践注入系统 prompt。

已内置的项目类型：

| 类型 | 识别特征 |
|------|---------|
| `linux_kernel` | `init/main.c`、`scripts/checkpatch.pl`、`Kconfig` 等 |
| `cargo` | `Cargo.toml` |
| `npm` | `package.json` |
| `python` | `pyproject.toml`、`setup.py`、`requirements.txt` |
| `go` | `go.mod` |
| `cmake` | `CMakeLists.txt` |
| `generic` | 不匹配以上任何类型时的默认约定 |

注入的内容包括：

- **Workflow**：该类项目的标准开发步骤
- **Checklist**：完成前应验证的事项
- **Common mistakes**：该类项目最容易犯的错误
- **Useful commands**：常用的构建/测试/检查命令

用户可以通过以下方式覆盖：

- `.opencode/conventions.md` — 完全替换自动检测到的约定
- `.opencode/conventions/{type}.md` — 覆盖特定类型的约定

---

## 项目指令自动注入

启动时，如果项目根目录存在以下文件，会自动读取并注入到系统 prompt 中：

| 文件 | 说明 |
|------|------|
| `AGENTS.md` | 通用 agent 指令 |
| `instructions.md` 或 `.opencode/instructions.md` | 项目专属指令 |
| `claude.md` | Claude 模型专属指令 |

这些文件的内容会作为 `# Project Instructions` 附加到每次 LLM 调用的系统 prompt 里，无需手动复制到对话中。

---

## 权限配置

支持项目级 `.opencode/config.json` 和全局 `~/.config/opencode/config.json`。

```json
{
  "permissions": [
    { "action": "apply_edit", "resource": "*", "effect": "deny" },
    { "action": "git", "resource": "*", "effect": "allow" },
    { "action": "bash", "resource": "*", "effect": "ask" },
    { "action": "read", "resource": "*", "effect": "allow" },
    { "action": "*", "resource": "*", "effect": "ask" }
  ]
}
```

- `allow`：直接执行
- `deny`：拒绝
- `ask`：终端提示确认

---

## 与原生 opencode 的差异

| 维度 | 原生 opencode | 本 opencode |
|------|--------------|-------------|
| 运行时 | Bun/Node.js + Electron | C + LuaJIT 单二进制 |
| 上下文 | 不断增长，依赖 compaction | **有界窗口 + KV Cache 永久存储，按需召回** |
| 持久化 | SQLite / 日志 | mmap KV Cache |
| 工具定义 | TypeScript 硬编码 | Lua 脚本，热更新 |
| Agent 扩展 | 改主程序 | Lua 插件，热加载 |
| 第三方代码 | 直接读取 | 预索引，只读摘要 |
| GUI | SolidJS TUI | Rust/gpui-component `.so` + FFI |

更完整的差距清单见 [`design.md`](./design.md) 第 19 章。

---

## 设计哲学

1. **小而精**：只保留 aicoding 最核心的编码 Agent 能力，不贪大求全。
2. **可编译**：最终交付物是一个独立二进制，部署简单。
3. **脚本化**：用 LuaJIT 承载 prompt、工具、权限、agents 等易变逻辑，避免频繁重编译。
4. **记忆外置**：Agent 脑子小，KV Cache 和 md 文档是无限外脑。

---

## 自动化测试

aicoding 自带可编程 GUI 测试框架，可以自动驱动 GUI 与 LLM 交互，无需人工点击窗口。

### 快速运行全部测试

```bash
cd /opt/my_db/aicoding
make regression
```

这会先运行 C 单元测试，然后按顺序执行所有 GUI 自动化测试（需要 GUI 显示和 LLM API 密钥）。

### 编译与运行

```bash
cd /opt/my_db/aicoding
make
cd gui_gpui && cargo build --release && cd ..
cp gui_gpui/target/release/libopencode_gui.so ./libaicoding_gui.so

OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
  ./aicoding --project /code/current --gui-test-script tests/gui_hi.lua
```

### 测试脚本环境

测试脚本在协程中运行，可用的全局变量：

| 变量 | 说明 |
|------|------|
| `_G` | 全局表，可访问 `run_build_agent` 等全局函数 |
| `app` | GUI 应用句柄 |
| `gui` | `gui.lua` 模块，提供 `set_input`/`submit`/`get_messages` 等 |
| `session_id` | 当前 session id |
| `project_ns` | 当前 project namespace |
| `opencode` | C 绑定库（变量名保留以兼容旧测试脚本） |
| `require` | Lua require |

常用 API：

| 函数 | 说明 |
|------|------|
| `gui.set_input(app, text)` | 把文本写入输入框 |
| `gui.submit(app)` | 触发提交，等效于按回车 |
| `gui.get_messages(app)` | 返回消息列表 `[{role, text}, ...]` |
| `gui.set_tokens(app, total, prompt, completion)` | 更新 token 显示 |

### 快速测试清单

| 测试 | 覆盖点 |
|------|--------|
| `tests/gui_hi.lua` | 基本对话循环 |
| `tests/gui_markdown.lua` | Markdown 渲染 + diff 分栏视图 |
| `tests/gui_write_edit.lua` | `write` / `edit` / 自动创建目录 |
| `tests/gui_mid_project.lua` | `read` / `edit` / `bash` / `git diff` 集成 |
| `tests/gui_build.lua` | `build` 工具自动检测 |
| `tests/gui_build_agent.lua` | 自主 Build-Fix Agent 完整循环 |
| `tests/gui_plan.lua` | 自主 Plan Agent 拆解任务并执行 |
| `tests/gui_web_fetch.lua` | `web_fetch` 工具抓取外部内容 |

### 运行示例

#### 基础对话测试

```bash
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
  ./aicoding --project /code/current --gui-test-script tests/gui_hi.lua
```

#### Markdown 与 Diff 渲染测试

```bash
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
  ./aicoding --project /code/current --gui-test-script tests/gui_markdown.lua
```

#### Plan Agent 测试

```bash
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
  ./aicoding --project /opt/my_db/aicoding --gui-test-script tests/gui_plan.lua
```

该测试会让 Plan Agent 自动拆解任务、修改 `lib.lua`、调用 Build Agent 验证。

#### Build 工具测试

```bash
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
  ./aicoding --project /opt/my_db/aicoding --gui-test-script tests/gui_build.lua
```

`build` 工具自动检测规则：

| 构建文件 | 默认命令 |
|---------|---------|
| `Cargo.toml` | `cargo build --release` |
| `package.json` | `npm run build` |
| `Makefile` / `makefile` | `make` |
| `CMakeLists.txt` | `cmake --build build` |
| `go.mod` | `go build ./...` |
| `setup.py` / `pyproject.toml` | `python -m build` |

#### Build-Fix Agent 测试

```bash
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
  ./aicoding --project /opt/my_db/aicoding --gui-test-script tests/gui_build_agent.lua
```

该测试会构造一个缺分号的 C 文件，触发 `/build`，验证 agent 能自动诊断、修复、重新编译成功。

#### Web Fetch 测试

```bash
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
  ./aicoding --project /opt/my_db/aicoding --gui-test-script tests/gui_web_fetch.lua
```

验证 `web_fetch` 能抓取 `https://example.com` 并返回内容。

#### 中型项目集成测试

```bash
# 1. 准备一个真实 git 仓库
cd /tmp
rm -rf midtest_project
git init midtest_project
cd midtest_project
git config user.email "test@example.com"
git config user.name "Test"
echo '# Mid Project' > README.md
echo 'module Mid' > lib.lua
git add .
git commit -m 'init'

# 2. 运行集成测试
cd /opt/my_db/aicoding
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
OPENCODE_ALLOW_ALL=1 \
  ./aicoding --project /tmp/midtest_project \
                 --gui-test-script tests/gui_mid_project.lua
```

#### 新建文件 + 编辑文件测试

```bash
# 1. 准备一个带 main.lua 的 git 仓库
cd /tmp
rm -rf writedit_project
git init writedit_project
cd writedit_project
git config user.email "test@example.com"
git config user.name "Test"
echo 'print("old")' > main.lua
git add .
git commit -m 'init'

# 2. 运行专项测试
cd /opt/my_db/aicoding
OPENCODE_GUI=1 \
LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
OPENCODE_ALLOW_ALL=1 \
  ./aicoding --project /tmp/writedit_project \
                 --gui-test-script tests/gui_write_edit.lua
```

### 批量测试

```bash
for t in tests/gui_*.lua; do
    echo "Running $t..."
    OPENCODE_GUI=1 LD_LIBRARY_PATH=/opt/my_db:/usr/local/luajit/lib:/opt/my_db/aicoding \
        timeout 120 ./aicoding --project /code/current --gui-test-script "$t"
    echo "exit=$?"
done
```

---

## 下一步

- [x] 工具对齐：`edit`/`write`/`apply_patch`/`glob`/`grep`/`build`
- [x] `read` 工具增强：分页、图片、目录、二进制检测
- [x] 文件修改增强：BOM/行尾保留、条件写入、自动创建父目录
- [x] 权限系统与 checkpoint/undo
- [x] 可编程 GUI 自动化测试（含 write/edit 专项、mid-project 集成、markdown 渲染、build 验证、build agent）
- [x] GUI 代码块语法高亮
- [x] GUI Markdown 基础渲染（标题/加粗/斜体/行内代码/列表）
- [x] GUI diff 代码块左右分栏视图
- [x] GUI 中文字体与字体大小优化
- [x] GUI 左侧信息流自动滚动到底部
- [x] 编辑文件后自动附加 git diff 输出
- [x] `build` 编译验证工具（自动检测构建系统，错误反馈给 LLM）
- [x] `build` 自主 Agent：编译失败 → LLM 诊断 → 修改代码 → 再次编译，循环直到成功
- [x] `plan` 自主 Agent：任务拆解 → 按依赖执行 → Build Agent 验证 → 失败重规划
- [x] `web_fetch` 外部文档抓取工具
- [x] `rerank.lua` 本地搜索结果重排序，降低检索噪声
- [x] `summarize.lua` 自动会话摘要 + 关键事实提取
- [x] `knowledge.lua` 项目级 `.opencode/knowledge/` 跨会话知识记忆
- [x] `kv_context` 深度控制与批量符号查询
- [x] `trace.lua` 结构化 agent 执行轨迹 + `trace_query` 工具
- [x] `make regression` 一键回归测试套件
- [ ] Agent 系统：`explore` 内置 agent
- [ ] GUI 编辑文件 side-by-side diff 视图（已支持消息内 diff 块）
- [x] `AGENTS.md` / `instructions` 自动注入
- [x] 迁移 GUI 到 GPUI + gpui-component
- [x] 项目重命名为 `aicoding`，默认 KV Cache 路径改为 `~/aicoding/<project_basename>`
- [ ] GUI 编辑文件 side-by-side diff 视图（已支持消息内 diff 块）

---

*基于 my_db KV Cache 构建。*

---

## 下一步：把 aicoding 基座打扎实（面向 Linux 内核级开发）

> 目标：让 aicoding 成为市面上第一个真正可用于 Linux 内核开发、不需要担心上下文窗口的 AI Coding Agent。
> 以下清单基于 AGENT_BEST_PRACTICES.md 的 Prompt / Context / Harness 三层框架，结合现有代码逐项复盘得出。

### 一、Context Engineering：让模型在长周期内核任务中始终"记得住、找得到"

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| C1 | **token 估算器校准** | ✅ 已完成：`tokens.lua` 已创建并被 context/compress/summarize 共享（commit `3ee5758e`） | 上下文压缩边界不准确，可能提前归档或超限；成本预测不准 | 提供一个 `tokens.lua`，统一基于 tiktoken 近似或字符统计，被 context/compress/summarize 共享；单元测试覆盖中英文、代码块、工具调用 |
| C2 | **归档内容可召回** | `context.lua` 把旧消息写入 `/agent/{session}/history/`，但系统 prompt 没有提示模型去召回 | 被移出窗口的信息理论上存在，但模型不知道在何时、如何读取 | 在系统 prompt 中加入"如需历史信息，用 `kv_search` 搜索 `/agent/{session}/history/`"；并提供一个 `recall_turns(query)` 辅助函数 |
| C3 | **跨 session 记忆预热** | 新 session 启动时只读取 facts/summaries，不会主动搜索相关历史 | 换一个 session 后，模型对之前分析过的模块一无所知 | 启动时根据当前 task/current 自动 `kv_search` 相关 facts + summaries + knowledge，把最相关的 N 条注入 prompt |
| C4 | **facts 质量与去重** | `prompts/default.lua` 取最近 10 条 facts，按 importance 排序；`rerank.lua` 按查询重排 | facts 可能重复、过期、互相矛盾；没有垃圾回收 | 给 fact 增加 source/action/evidence 字段；定期合并重复 fact，标记过期；提供 `fact_gc` 工具 |
| C5 | **代码检索结果可信度** | `rerank.lua` 用 token 重叠打分，没有利用代码结构信息 | 对长函数、宏、Kconfig 等召回可能不准 | 重排时加入 kind（function/struct/macro）、调用图热度、文件路径匹配、签名匹配等信号；对内核专用符号做 boost |
| C6 | **调用图深度与循环控制** | `kv_context` 已支持 depth/batch，但 `cache_query` 的输出是原始 JSON，未针对 LLM 阅读优化 | 深调用链输出冗长，容易塞爆上下文 | 对 `kv_context` 结果做摘要：每层只保留关键调用点，检测循环，支持 `max_nodes` 限制 |
| C7 | **项目级知识自动维护** | `knowledge.lua` 支持读写和注入 prompt，但靠模型自觉 | 模型可能不写、写乱、写重复 | 在 Plan/Build Agent 结束时自动提取并写入 `knowledge_write`；提供 knowledge 模板（architecture.md、decisions.md、patterns.md、errors.md） |
| C8 | **外部文档持久化索引** | `web_fetch` 只把摘要写入 KV，不做向量索引 | 抓取过的 RFC/文档无法语义搜索 | 对 web_fetch 内容做分块 + 向量索引，写入 `/agent/{session}/web_index/`，支持 `kv_search search_type=semantic` |

### 二、Harness Engineering：让模型在内核这种高风险场景下可靠执行

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| H1 | **权限系统更细粒度** | ✅ 已完成：内置 deny 规则覆盖 `/usr`、`/etc`、`/bin`、`/sbin`、`/lib*`、`/opt/my_db`、`~/*`；增加 `is_dangerous_bash()` 危险命令检测（commit `3ee5758e`） | 无法限制"只能改 .c/.h"、"不能删文件"、"bash 不能执行 rm -rf /" | 增加 action 细分：edit_filetype、delete、bash_command；resource 支持 glob 和否定模式；默认规则对危险操作更保守 |
| H2 | **敏感操作二次确认** | 依赖 permissions 的 ask 模式，但没有针对"大面积删除/网络/格式化"的特殊提示 | 模型可能在长任务中误触高破坏操作 | 对 `file_delete`、`bash` 中 rm/dd/mkfs、`write` 覆盖多文件等触发二次确认，并记录到 trace |
| H3 | **插件沙箱与签名** | `plugin_create`/`plugin_load` 直接加载任意 Lua，没有沙箱 | 恶意或被误导的插件可执行任意系统命令 | 插件加载前可选沙箱：限制 `os.execute`/`io.popen`、禁止 require C 模块、只开放白名单 API；提供 `plugin_validate` |
| H4 | **Agent 状态机持久化** | Plan/Build Agent 状态存在 KV Cache，但只保留最近一条 status | 进程崩溃后无法准确恢复执行到哪一步 | 每条 agent 事件写入 trace + KV；提供 `agent_resume` 命令从最近 checkpoint 恢复 |
| H5 | **Build Agent 针对内核** | 当前 build 工具只检测 cargo/npm/make/cmake/go/python | Linux 内核用 kbuild，需要 `make menuconfig`、`make -j$(nproc)`、处理 .config | 扩展 build 检测：识别 `Kconfig`、`Makefile`（内核风格）、`scripts/kconfig/`；支持指定 target 和 defconfig；失败时只返回前 N 个 error 和对应文件 |
| H6 | **Plan Agent 自省** | Plan Agent 生成计划后执行，但计划本身不经过可行性评估 | 内核任务可能计划不可行（如修改不存在的子系统） | 在生成计划后增加"plan_review"步骤：检查涉及文件/符号是否存在、依赖是否合理、是否需要配置变更 |
| H7 | **错误恢复与熔断** | chat_once 8 次迭代后返回"too many tool iterations"，没有分类错误 | 网络错误、API 限流、解析错误、工具失败混为一谈 | 对错误分类：retry（网络/限流）、replan（工具链失败）、abort（权限/安全）；支持指数退避重试 LLM 请求 |
| H8 | **结构化日志级别** | 只有 `opencode.log_info`，没有 debug/warn/error | 排查问题时日志 noise 大，关键错误被淹没 | 增加 `opencode.log_debug/warn/error`，受 `OPENCODE_LOG_LEVEL` 控制；关键路径统一使用 |
| H9 | **可观测性查询** | `trace_query` 已支持事件过滤 | 缺少成本、延迟、token 使用、工具成功率的聚合 | 在 trace 中记录 token usage、latency；提供 `metric_summary` 工具输出 scorecard |
| H10 | **评估 benchmark** | 只有 8 个 GUI 冒烟测试 | 无法量化 prompt/context/agent 改动的影响 | 建立 `benchmarks/`：内核符号问答、已知 bug 修复、新驱动函数添加、跨文件重构；输出 pass / cost / turns |

### 三、Prompt / System：让模型知道如何像一个内核开发者一样工作

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| P1 | **内核专用系统 prompt** | 默认 prompt 是通用 coding assistant | 模型不了解 Linux 内核编码风格、Kconfig、头文件约定、许可证头 | 检测项目为内核源码时，注入 `prompts/linux_kernel.lua`：包含 CodingStyle、checkpatch、Kconfig、include 规则 |
| P2 | **任务模板** | `/task` 只保存一句话描述 | 复杂内核任务（如"给某子系统加接口"）缺少标准分析路径 | 提供任务模板：探索子系统 → 读 Kconfig/Makefile → 找同类实现 → 修改 → 编译 → 运行 checkpatch |
| P3 | **工作流强制** | 系统 prompt 列出 6 条 workflow，但靠模型自觉 | 模型可能跳过搜索直接编辑，导致破坏 | 在 prompt 中增加"必须完成 checklist"：每次编辑前说明依据、编辑后运行 build/diff、关键结论写 KV |
| P4 | **输出格式稳定** | 工具调用由 schema 保证，但自然语言输出格式多变 | 长回复中关键信息（文件列表、测试结果）难以解析 | 要求模型在总结时使用结构化格式：```yaml status: ... files: ... tests: ...``` |

### 四、Tool / Agent：补齐内核开发必备工具

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| T1 | **跨文件批量编辑** | `edit` 一次只能替换一处，`apply_patch` 需要完整 patch | 内核修改常涉及多文件同模式替换 | 提供 `multi_edit` 工具：按 glob 批量应用相同模式替换，返回每个文件结果；提供 `sed_safe` 模式 |
| T2 | **内核代码搜索增强** | `kv_search` 语义搜索已可用，`grep` 用 ripgrep | 缺少按子系统、按配置项、按 commit 搜索 | 增加 `kconfig_search`、`grep_commit`（git log -S）、`grep_defconfig` 工具 |
| T3 | **checkpatch / sparse 集成** | build 工具不识别 checkpatch | 内核代码需要过风格检查 | 增加 `checkpatch` 工具：对修改文件运行 `scripts/checkpatch.pl`；`sparse` 工具静态检查 |
| T4 | **配置系统交互** | 没有 Kconfig 相关工具 | 修改驱动时可能需要改 Kconfig/Makefile | 提供 `kconfig_read`（读 Kconfig 描述）、`kconfig_set`（修改 .config）工具 |
| T5 | **测试执行工具** | `build` 只做编译，没有测试 | 内核修改后需要运行 qemu/boot test | 提供 `test` 工具：检测项目类型，运行 `make test` / `pytest` / `kselftest` / `qemu`（可配置） |
| T6 | **Explore Agent** | roadmap 中标记为未完成 | 模型探索大型代码库时缺少导航 agent | 实现 `agents/explore.lua`：接受"了解某子系统"任务，自动搜索入口文件、核心结构体、关键函数，输出架构报告 |
| T7 | **Review Agent** | 无 | 编辑后缺少代码审查 | 实现 `agents/review.lua`：对修改文件做静态检查、风格检查、架构约束检查，输出 review report |

### 五、Observability / Traceability：让开发者能复盘 agent 行为

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| O1 | **trace 持久化与轮转** | `trace.lua` 写入单文件，没有大小限制 | 长时间运行后 trace 文件无限增长 | 按天轮转，限制单文件大小；提供 `trace_prune` 工具 |
| O2 | **trace 与 KV 关联** | trace 和 KV 分别存储 | 难以从一次 LLM 请求追踪到产生的 facts/checkpoints | trace 中记录相关 KV key、checkpoint_id、修改文件路径 |
| O3 | **运行时 metric 看板** | 无 | 无法直观看到 agent 成本和成功率 | 在 GUI/CLI 中显示本次会话的 turns、tokens、tool calls、success rate |
| O4 | **失败回放** | 无 | 线上问题难以本地复现 | 提供 `trace_replay`：读取 trace 文件，按顺序重放 tool calls（LLM 调用可 mock） |

### 六、Testing / Evaluation：把回归变成基础设施

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| E1 | **C 层单元测试覆盖** | 只有 `test/test_session.c` 一个冒烟测试 | C 核心（cache 绑定、llm_client、http_async）无单元测试 | 为 `lua_engine.c` 的绑定函数、`llm_client.c` 的请求构建、`http_async.c` 增加测试 |
| E2 | **Lua 层单元测试** | 无 | rerank、context、permissions 等核心逻辑无独立测试 | 使用 busted 或纯 Lua 测试框架，覆盖 rerank、context.compress、permissions.check、checkpoint |
| E3 | **回归测试并行化** | `make regression` 串行运行 GUI 测试 | 时间长，依赖 LLM，不稳定 | 支持非 GUI 的 CLI 回归；对不依赖 LLM 的模块单独跑；关键路径加 mock LLM |
| E4 | **内核基准任务** | 无 | 无法证明 aicoding 能做内核开发 | 在 `benchmarks/linux/` 中准备 3-5 个真实任务：添加 sysfs 属性、修复已知 simple bug、refactor 一个 helper |

### 七、Safety / Guardrails：让 agent 不会被自己或模型搞坏

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| S1 | **内容安全过滤** | ✅ 已完成：`permissions.is_dangerous_bash()` 拦截 rm -rf /、mkfs、dd、fork bomb、重定向到 /dev、curl | sh 等模式（commit `3ee5758e`） | 模型可能输出或执行有害内容 | 对 LLM 输出做关键词/命令黑名单检查；对 bash 命令做危险命令检测 |
| S2 | **写保护目录** | ✅ 已完成：`permissions.lua` 默认 deny 对 `/usr`、`/etc`、`/bin`、`/sbin`、`/lib*`、`/opt/my_db`、`~/*` 的 write/file_create/file_delete/delete（commit `3ee5758e`） | 模型可能误写 `/usr`、`/etc`、内核源码外的路径 | 默认 deny 对 `/usr`、`/etc`、`/opt/my_db`（除项目目录外）、`~` 根目录的 write/delete |
| S3 | **资源限制** | 无 | 长循环或大量搜索可能耗尽资源 | 对 tool calls / LLM calls / bash timeout / 文件读取大小设置全局限制；超限时熔断 |
| S4 | **checkpoint 自动策略** | 只在 edit/write 前备份 | delete、apply_patch、bash 也可能破坏 | 对 delete、apply_patch、file_create 也触发 checkpoint；大修改前自动备份整个项目 |

### 八、Linux 内核开发专属：让 aicoding 真正懂内核

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| L1 | **内核索引预加载** | `/opt/code_caches/linux_cache` 已存在 | 但 aicoding 启动时不会自动把它作为可用 repo 提示给模型 | 系统 prompt 自动列出 `/opt/code_caches` 中可访问的 repo；对 `/code/local/linux` 做特殊别名 |
| L2 | **内核文件类型识别** | `read` 工具对所有文本文件一视同仁 | 对 Kconfig、.config、dts、Makefile、头文件没有特殊处理 | 在 read 结果中返回文件类型提示；提供 `kconfig_read`、`dts_read` 等专用工具 |
| L3 | **头文件/导出符号追踪** | `kv_context` 有 caller/callee，但缺少 include/export 关系 | 内核修改常需要加 include、EXPORT_SYMBOL、头文件声明 | 扩展 cache_query 输出 include 关系、EXPORT_SYMBOL 列表；工具提示模型检查 |
| L4 | **编译-配置联动** | build 工具对内核对 .config 变化不敏感 | 修改 Kconfig 后需要重新配置，否则编译失败 | build 工具检测到 Kconfig 变化时提示/运行 `make oldconfig`；支持保存/恢复 .config |
| L5 | **版本与分支感知** | 无 | 模型不知道当前内核版本、目标分支 | 从 Makefile 读取版本注入 prompt；git branch/tag 注入 prompt |
| L6 | **checkpatch 风格内化** | 无 | 模型生成的补丁风格不符，每次靠 checkpatch 后返工 | 在 prompt 中注入常见 checkpatch 规则；edit 后自动调用 checkpatch 并反馈 |

### 九、工程债务：把代码本身整理成 agent 能维护的样子

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| D1 | **统一 shell_quote** | ✅ 已完成：`shell.lua` 提供 `quote`/`quote_args`/`run`，`tools/default.lua`、`knowledge.lua`、`trace.lua` 已迁移（commit `3ee5758e`） | 新增模块容易漏引或写错 | 抽一个 `shell.lua` 模块，所有 os.execute/io.popen 统一使用；回归测试覆盖含空格/单引号路径 |
| D2 | **统一 JSON 处理** | ✅ 已完成：`json.lua` 封装 decode/encode，统一 `(val, err)` 返回签名；`main.lua`、agents、checkpoints、prompts 已迁移（commit `3ee5758e`） | 错误处理不一致 | 提供 `json.lua` 封装 decode/encode，统一错误处理 |
| D3 | **减少全局函数** | `main.lua` 暴露多个全局函数供 C 调用，但没有命名空间 | 命名冲突风险 | 把全局入口整理到 `aicoding.*` 命名空间，或在文档中明确列出 |
| D4 | **错误码与日志统一** | tool 返回 `{ok, error}`，但 error 信息格式不一 | 下游解析困难 | 定义 error taxonomy：FILE_NOT_FOUND、PERMISSION_DENIED、LLM_ERROR、TIMEOUT 等 |
| D5 | **模块依赖图清晰** | 模块间 require 关系隐含 | 新增循环依赖风险 | 绘制模块依赖图；禁止 tools -> main 反向依赖；lint 检查 |

### 十、文档与仓库即记录系统

| # | 事项 | 现状 | 风险/差距 | 期望的"完成标准" |
|---|------|------|-----------|------------------|
| Doc1 | **AGENTS.md 变成地图** | 目前没有项目级 AGENTS.md | 新会话模型不知道项目结构 | 创建 `.opencode/AGENTS.md`，指向 `docs/ARCHITECTURE.md`、`docs/WORKFLOW.md`、`docs/CONVENTIONS.md` |
| Doc2 | **架构约束文件** | 无 `.opencode/architecture.json` | 模型可能写出破坏依赖方向的代码 | 定义 `aicoding` 自身模块的层级和依赖规则，并提供 lint 工具验证 |
| Doc3 | **决策记录模板** | `knowledge.lua` 已支持 knowledge_write | 但缺少标准模板 | 提供 `.opencode/knowledge/decisions.md` 模板和 `knowledge_write` 最佳实践 |
| Doc4 | **更新本 README 底部 TODO** | 本次追加 | 后续每完成一项应更新状态 | 把本 TODO 改为 `.opencode/ROADMAP.md`，README 只保留链接；每项完成后在 ROADMAP 中标记并附提交 hash |

