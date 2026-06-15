# opencode

一个面向开发者、对齐 [opencode](https://opencode.ai) 体验的 AI Coding Agent。核心定位是**小而精、可编译成独立二进制**——不依赖 Node.js / Bun / Python 运行时，一个可执行文件即可运行。

> 当前仓库：`quqiufeng/my_db/opencode`
>
> 📐 **GUI 架构说明**：[Rust + LuaJIT FFI 的 GUI 开发模式](reference/gui-architecture.md) —— Rust 渲染引擎编译为 `.so`，LuaJIT 通过 C ABI 驱动界面，内容更新与 LLM 交互全部在 Lua 层完成。

---

## 一句话介绍

`opencode` 是一个复刻 opencode 核心工作流的本地 AI 编程助手：读取项目、编辑文件、运行 bash/git、管理权限，全部通过大模型工具调用完成。与原生 opencode 的最大区别在于上下文系统——它用 **my_db KV Cache** 替代不断增长的 `messages` 数组，实现永久记忆和按需召回。

---

## 核心特性

| 特性 | 说明 |
|------|------|
| **独立二进制** | 编译成单个 `opencode_cli`，无需 Node.js / Bun / Electron |
| **KV Cache 记忆** | 项目知识、代码索引、会话事实全部落盘，进程重启不丢失 |
| **工具调用** | 读文件、编辑文件、运行 bash/git、搜索代码记忆 |
| **权限系统** | 兼容 opencode 的 `permissions` 规则，支持 `allow`/`deny`/`ask` |
| **LuaJIT 脚本层** | Prompt、工具定义、调度逻辑全部用 Lua 编写，改逻辑不重编译 |
| **实验性 GUI** | Rust/egui 编译为 `.so`，LuaJIT FFI 驱动，可弹出聊天窗口 |
| **预索引第三方代码** | 用现有 `analyze_repo.sh` / `typescript-indexer` 索引进 KV Cache，避免误读依赖 |

---

## 技术架构：小而精

```
┌─────────────────────────────────────────┐
│  opencode_cli (单个二进制)               │
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
│  - tools/default.lua    工具定义与调度    │
│  - permissions.lua      权限规则解析      │
│  - gui.lua              GUI FFI 封装      │
└─────────────────────────────────────────┘
           │
           ▼
┌─────────────────────────────────────────┐
│  libopencode_gui.so (可选，Rust/egui)    │
│  - 聊天窗口                               │
│  - 消息列表 / 输入框 / 工具输出           │
└─────────────────────────────────────────┘
```

### 为什么这样设计

| 传统方案 | 本方案 |
|----------|--------|
| Electron + Node.js 运行时，启动慢、体积大 | C + LuaJIT，编译成单个二进制 |
| 上下文靠 `messages` 数组，需要复杂 compaction | KV Cache 永久记忆，prompt 按需组装 |
| 工具/schema 硬编码在 TypeScript 里 | Lua 脚本定义，热更新 |
| 依赖 npm 生态 | 只依赖系统已存在的 `libmydb.so` / `cjson.so` |

---

## 快速开始

### 1. 编译

```bash
cd /opt/my_db/opencode
make
```

生成：

- `opencode_cli` — 主程序
- `libopencode_agent.a` — C 内核静态库

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
./opencode_cli --project /path/to/your/repo
```

### 4. 运行 GUI（实验性）

```bash
# 先编译 GUI .so
cd gui
cargo build --release
cp target/release/libopencode_gui.so ../libopencode_gui.so
cd ..

# 启动 GUI 模式
OPENCODE_GUI=1 ./opencode_cli --project /path/to/your/repo
```

---

## 目录结构

```
opencode/
├── cli.c              # 主入口 / REPL / GUI 分支
├── session.c/h        # Session namespace on KV Cache
├── lua_engine.c/h     # LuaJIT 宿主 + C bindings
├── llm_client.c/h     # OpenAI/Anthropic HTTP client
├── main.lua           # Lua 运行时入口
├── permissions.lua    # 权限规则引擎
├── prompts/
│   └── default.lua    # 系统提示组装
├── tools/
│   └── default.lua    # 工具定义与调度
├── gui.lua            # LuaJIT FFI 封装 GUI
├── gui/               # Rust/egui GUI 后端
│   ├── Cargo.toml
│   └── src/lib.rs
├── design.md          # 详细设计文档
└── README.md          # 使用说明（本文件）
```

---

## 主要工具

| 工具 | 用途 |
|------|------|
| `source_read` | 读取项目文件 |
| `apply_edit` | 精确替换文件内容 |
| `file_create` | 创建新文件 |
| `file_delete` | 删除文件 |
| `file_list` | 列出目录 |
| `bash` | 执行 shell 命令 |
| `git` | 执行 git 命令 |
| `diff` | 查看 git diff |
| `kv_search` | 搜索 KV Cache 中的代码/历史 |
| `kv_get` | 读取精确 KV 键 |
| `kv_context` | 获取符号上下文（caller/callee） |
| `code_index` | 索引新代码库到 KV Cache |

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
| 上下文管理 | `messages` + compaction | KV Cache 按需召回 |
| 持久化 | SQLite / 日志 | mmap KV Cache |
| 工具定义 | TypeScript 硬编码 | Lua 脚本 |
| 第三方代码 | 直接读取 | 预索引，只读摘要 |
| GUI | SolidJS TUI | Rust/egui .so + FFI（长期目标 GPUI） |

更完整的差距清单见 [`design.md`](./design.md) 第 19 章。

---

## 设计哲学

1. **小而精**：只保留 opencode 最核心的编码 Agent 能力，不贪大求全。
2. **可编译**：最终交付物是一个独立二进制，部署简单。
3. **脚本化**：用 LuaJIT 承载 prompt、工具、权限等易变逻辑，避免频繁重编译。
4. **记忆外置**：Agent 脑子小，KV Cache 和 md 文档是无限外脑。

---

## 下一步

- [ ] 工具对齐：`edit`/`write`/`apply_patch`/`glob`/`grep`
- [ ] `read` 工具增强：分页、图片、目录、二进制检测
- [ ] 文件修改增强：BOM/行尾保留、条件写入
- [ ] Agent 系统：`build`/`plan`/`explore` 内置 agent
- [ ] `AGENTS.md` / `instructions` 自动注入
- [ ] 迁移 GUI 到 GPUI + gpui-component

---

*基于 my_db KV Cache 构建。*
