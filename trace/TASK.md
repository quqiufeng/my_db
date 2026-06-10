# Trace 开发任务清单

> 范围限定：`tracec` — 将探针 DSL 编译为后端追踪脚本的编译器。
> DSL 语言设计属于其他项目，不在此清单中。

---

## Phase 1：`tracec` 核心编译器

**目标**：接收探针 DSL 输入，输出各后端追踪脚本。

### 1.1 DSL 解析

| 任务 | 状态 | 说明 |
|------|------|------|
| DSL AST 定义 | ✅ | 内部 AST 数据结构，支持 CPU/函数/内存/锁探针 |
| DSL 解析器 | ✅ | 递归下降解析器，支持 // 和 /* */ 注释 |

### 1.2 后端代码生成

| 任务 | 状态 | 说明 |
|------|------|------|
| perf 后端 | ✅ | CPU + uprobe + tracepoint 探针 |
| SystemTap 后端 | ✅ | `.stp` 脚本，profile + process.function + tracepoint |
| eBPF (bpftrace) 后端 | ✅ | profile + uprobe/uretprobe + 内存聚合 |
| DTrace 后端 | ✅ | `.d` 脚本（macOS/FreeBSD） |
| GDB Python 后端 | ✅ | bash 包装脚本，输出 JSON |

### 1.3 输出处理

| 任务 | 状态 | 说明 |
|------|------|------|
| JSON schema 定义 | ✅ | 顶层 + 每探针结果结构的完整 schema |
| tracec parse 命令 | ✅ | 解析 perf report 输出 → JSON |
| GDB JSON 输出 | ✅ | GDB 采样脚本直接输出 JSON |

### 1.4 工具框架

| 任务 | 状态 | 说明 |
|------|------|------|
| CLI 入口 | ✅ | `tracec detect | compile | parse | extract` |
| 自动目标检测 | ✅ | `auto` 模式按 stap → ebpf → perf → dtrace → gdb 优先级 |
| 错误处理与提示 | ✅ | 后端不可用时 perror + 返回非零 |

### 1.5 运行时提取（新增）

| 任务 | 状态 | 说明 |
|------|------|------|
| tracec extract 框架 | ✅ | `extract <runtime> <pid>` 命令结构 |
| LuaJIT trace 提取 | ✅ | 读 jit_State->trace[] 输出 JSON 映射表 |
| 安全退出机制 | ✅ | 非目标进程不崩溃，输出空表 |
| V8/Node.js 提取 | ⬜ | 读 Isolate->code_cache |
| CPython 提取 | ⬜ | 读 PyThreadState->frame |
| PHP 提取 | ⬜ | 读 zend_executor_globals |

---

## Phase 2：语言绑定（可选）

| 任务 | 状态 | 说明 |
|------|------|------|
| 嵌入 ReScheme FFI | ⬜ | 通过 `foreign-procedure` 调 `tracec` 编译器 |
| 嵌入 Python | ⬜ | Python `subprocess` 调 `tracec` |
| 嵌入 Shell | ⬜ | Shell 命令行包装 |

---

## Phase 3：验证测试

| 任务 | 状态 | 说明 |
|------|------|------|
| 后端输出验证 | ✅ | 5 个后端均正确生成语法有效的脚本 |
| 全量编译测试 | ✅ | 10 探针全部正确解析 |
| 自动降级测试 | ⬜ | `auto` 模式下按优先级正确选择可用后端 |
| 端到端执行测试 | ⬜ | 在有 perf 的环境跑一次完整调用链 |

---

> **范围说明**：本 TASK.md 仅涵盖 `tracec` 编译器本身的实现。
> DSL 语言设计、探针语义、分析方法论、AI Agent 集成等属于其他项目。
> 架构细节见 `trace/README.md`。
