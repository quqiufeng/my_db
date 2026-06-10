# Trace 开发任务清单

> 范围限定：`tracec` — 将探针 DSL 编译为后端追踪脚本的编译器。
> DSL 语言设计属于其他项目，不在此清单中。

---

## Phase 1：`tracec` 核心编译器

**目标**：接收探针 DSL 输入，输出各后端追踪脚本。

### 1.1 DSL 解析

| 任务 | 状态 | 说明 |
|------|------|------|
| DSL AST 定义 | ⬜ | 定义内部 AST（支持 CPU/内存/锁等探针类型、collect/when/freq 等子句） |
| DSL 解析器 | ⬜ | 读取 DSL 文本 → AST。解析器接口应与 DSL 定义解耦 |

### 1.2 后端代码生成

| 任务 | 状态 | 说明 |
|------|------|------|
| perf 后端生成器 | ⬜ | AST → `perf record` 命令 + `perf report` 解析脚本 |
| SystemTap 后端生成器 | ⬜ | AST → `.stp` 脚本 |
| eBPF (BCC) 后端生成器 | ⬜ | AST → BCC Python 脚本 |
| DTrace 后端生成器 | ⬜ | AST → `.d` 脚本（macOS/FreeBSD） |
| GDB Python 后端生成器 | ⬜ | AST → GDB Python 采样脚本 |

### 1.3 输出处理

| 任务 | 状态 | 说明 |
|------|------|------|
| 采样数据解析器 | ⬜ | 各后端原始输出 → 统一 JSON schema |
| JSON schema 定义 | ⬜ | 定义：{函数名, 频次, 耗时, 源码位置, 调用者} |

### 1.4 工具框架

| 任务 | 状态 | 说明 |
|------|------|------|
| CLI 入口 | ⬜ | `tracec compile input.dsl [--target perf|stap|ebpf|dtrace|gdb] [--output script]` |
| 自动目标检测 | ⬜ | `--target auto` 时自动选择可用后端（stap → ebpf → perf → dtrace → gdb） |
| 错误处理与提示 | ⬜ | 后端不可用时给出清晰的安装提示 |

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
| 后端输出验证 | ⬜ | 每个后端生成的脚本是否能正确执行 |
| 端到端测试 | ⬜ | DSL → 生成脚本 → 执行 → 解析输出 → JSON 全链路 |
| 错误降级测试 | ⬜ | 目标环境缺少某后端时自动降级正确 |

---

> **范围说明**：本 TASK.md 仅涵盖 `tracec` 编译器本身的实现。
> DSL 语言设计、探针语义、分析方法论、AI Agent 集成等属于其他项目。
> 架构细节见 `trace/README.md`。
