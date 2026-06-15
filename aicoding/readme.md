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
| **权限系统** | 兼容 opencode 的 `permissions` 规则，支持 `allow`/`deny`/`ask` |
| **LuaJIT 脚本层** | Prompt、工具定义、调度逻辑、Agent 全部用 Lua 编写，改逻辑不重编译 |
| **实验性 GUI** | Rust/gpui-component 编译为 `.so`，LuaJIT FFI 驱动，可弹出聊天窗口 |
| **代码语法高亮** | GUI 代码块使用 syntect 按语言着色 |
| **Markdown 基础渲染** | GUI 文本消息支持标题、加粗、斜体、行内代码、无序列表 |
| **Diff 分栏视图** | `diff` 代码块自动解析为左右 side-by-side，红删绿增 |
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
│  - tools/default.lua    工具定义与调度    │
│  - permissions.lua      权限规则解析      │
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
| `kv_search` | 搜索 KV Cache；支持 `semantic`/`prefix`/`regex`/`tag`，可对已索引代码库做自然语言语义搜索 |
| `kv_get` | 读取精确 KV 键 |
| `kv_set` | 写入记忆（事实、历史、中间结论），支持 TTL |
| `kv_context` | 获取符号上下文（caller/callee） |
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

```lua
local ok, out = mem.context("schedule", "/code/local/linux")
print(out)
```

等价工具调用：

```json
{ "symbol": "schedule", "repo": "/code/local/linux" }
```

### 4. 命名空间约定

| 命名空间 | 用途 | 读写 |
|----------|------|------|
| `/code/local/{repo}` | 代码库语义记忆 | 只读（由 `analyze_repo.sh` 导入） |
| `/agent/{session}/facts` | 当前会话事实 | 读写 |
| `/agent/{session}/history` | 当前会话历史动作 | 读写 |
| `/agent/{session}/plan` | 当前计划/待办 | 读写 |
| `/agent/{session}/build` | Build Agent 运行记录 | 读写 |

### 3. Plan Agent 与 Build Agent

Plan 和 Build Agent 会把执行计划、步骤结果、失败重试记录写入 KV Cache：

```lua
local mem = require("memory")
print(mem.read("/agent/default/plan/plan"))      -- 当前计划 JSON
print(mem.read("/agent/default/plan/status"))    -- running / success / failed
print(mem.read("/agent/default/build/status"))   -- build agent 状态
```

### 4. 索引新项目到记忆系统

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

### 5. CLI 调试命令

启动 CLI 后可用 `/lua <code>` 直接执行 Lua：

```bash
> /lua local mem = require("memory"); print(mem.recall("user_preference"))
```

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
- [ ] Agent 系统：`explore` 内置 agent
- [x] `AGENTS.md` / `instructions` 自动注入
- [x] 迁移 GUI 到 GPUI + gpui-component
- [x] 项目重命名为 `aicoding`，默认 KV Cache 路径改为 `~/aicoding/<project_basename>`
- [ ] GUI 编辑文件 side-by-side diff 视图（已支持消息内 diff 块）

---

*基于 my_db KV Cache 构建。*
