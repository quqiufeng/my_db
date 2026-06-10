# Trace 开发任务清单

> 基于 `trace/README.md` 定义的架构拆解为可执行任务。
> 状态：⬜ 待办 / 🔄 进行中 / ✅ 完成

---

## Phase 1：探针编译器 `tracec` — SystemTap 后端

**目标**：实现 `tracec` 编译器，支持探针 DSL → SystemTap 脚本 → 性能数据 JSON → 分析报告。

### 1.1 探针 DSL 定义

| 任务 | 状态 | 说明 |
|------|------|------|
| DSL 语法设计 | ⬜ | 定义探针语言语法（probe、collect、when、freq、duration 等关键字） |
| DSL AST 定义 | ⬜ | 定义内部 AST 数据结构（支持 CPU/内存/锁三种探针类型） |
| DSL 解析器 | ⬜ | 实现 DSL 文本 → AST 的解析器 |
| DSL 语义检查 | ⬜ | 类型检查、作用域检查、错误提示 |

### 1.2 SystemTap 代码生成

| 任务 | 状态 | 说明 |
|------|------|------|
| CPU 热点探针生成 | ⬜ | probe cpu → `perf record -F 99 -g` 或 SystemTap `profile-{hz,tick}` |
| 内存分配探针生成 | ⬜ | probe memory → SystemTap `process.*.function("malloc").call` |
| 锁竞争探针生成 | ⬜ | probe lock → SystemTap `process.*.function("pthread_mutex_lock").call` |
| 调用栈采集 | ⬜ | `print_ubacktrace()` + 符号解析 |
| 耗时直方图采集 | ⬜ | `@hist_log` 聚合耗时分布 |
| caller 追踪 | ⬜ | 通过调用栈获取调用者函数名 |

### 1.3 性能数据输出

| 任务 | 状态 | 说明 |
|------|------|------|
| JSON 输出格式定义 | ⬜ | 定义标准 JSON schema：{函数名, 频次, 耗时, 源码位置, 调用者} |
| SystemTap 输出 → JSON | ⬜ | 解析 SystemTap stdout 为结构化 JSON |
| 采样数据聚合 | ⬜ | 合并多次采样数据，去重、排序 |

### 1.4 分析报告生成

| 任务 | 状态 | 说明 |
|------|------|------|
| 热点函数排序 | ⬜ | 按 CPU 占比降序排列 TOP N |
| 源码位置解析 | ⬜ | 通过 addr2line / debug symbols 将地址翻译为文件:行号 |
| 调用关系附加 | ⬜ | 从采样数据中提取 caller/callee 关系 |
| 报告输出 | ⬜ | 生成可读的文本报告（终端输出格式） |

### 1.5 验证测试

| 任务 | 状态 | 说明 |
|------|------|------|
| 对一个已知程序采样 | ⬜ | 写一个测试程序（如 fibonacci 循环），验证热点定位准确性 |
| 验证 CPU 热点 TOP 1 | ⬜ | 确认报告中的 TOP 1 与预期一致 |
| 验证空载开销 | ⬜ | 系统空闲时采样，确认开销 < 3% |
| 端到端集成测试 | ⬜ | DSL → SystemTap → JSON → 报告 全链路 |

---

## Phase 2：多后端支持

**目标**：将 `tracec` 扩展到 eBPF/perf/DTrace/GDB 多个后端。

### 2.1 eBPF/BCC 后端

| 任务 | 状态 | 说明 |
|------|------|------|
| eBPF CPU 热点探针 | ⬜ | BCC `profile.py` 封装，99Hz 采样 |
| eBPF 内存分配探针 | ⬜ | BCC `mallocstacks.py` 封装 |
| eBPF 锁竞争探针 | ⬜ | BCC `lockstat.py` 封装 |
| eBPF 输出 → JSON | ⬜ | 统一 JSON schema |
| 自动降级策略 | ⬜ | 检测内核版本，eBPF 不可用时自动切到 SystemTap |

### 2.2 perf 后端

| 任务 | 状态 | 说明 |
|------|------|------|
| perf CPU 热点采样 | ⬜ | `perf record -F 99 -g -p $PID --sleep $DURATION` |
| perf 输出解析 | ⬜ | `perf report --stdio` 解析为 JSON |
| perf annotate 指令级热点 | ⬜ | `perf annotate` 精确到指令行的热点分布 |
| perf sched off-CPU | ⬜ | `perf record -e sched:sched_switch -g` 采集 off-CPU |

### 2.3 DTrace 后端（macOS/FreeBSD）

| 任务 | 状态 | 说明 |
|------|------|------|
| DTrace CPU 热点探针 | ⬜ | `profile-99` + `ustack()` |
| DTrace 符号加载 | ⬜ | 自动加载用户态调试符号 |
| DTrace 输出 → JSON | ⬜ | 统一 JSON schema |

### 2.4 GDB 后端（兜底）

| 任务 | 状态 | 说明 |
|------|------|------|
| GDB Python 采样脚本 | ⬜ | `gdb --batch -x sampler.py` |
| GDB 调用栈采集 | ⬜ | `bt` 命令批量采集调用栈 |
| GDB 输出 → JSON | ⬜ | 统一 JSON schema |

---

## Phase 3：火焰图生成

**目标**：将采样数据可视化，支持多种火焰图类型。

| 任务 | 状态 | 说明 |
|------|------|------|
| on-CPU 火焰图 | ⬜ | 调用 `stackcollapse-perf.pl` + `flamegraph.pl` |
| off-CPU 火焰图 | ⬜ | sched 事件 → 火焰图 |
| 内存火焰图 | ⬜ | malloc/free 追踪 → 内存分配火焰图 |
| 火焰图差异对比 | ⬜ | 两张火焰图做 diff，识别新增热点 |
| 自动标注问题区域 | ⬜ | 在火焰图上标出异常宽的山峰 |

---

## Phase 4：AI Agent 集成

**目标**：Agent 自动生成探针、自动分析、自动积累诊断知识。

| 任务 | 状态 | 说明 |
|------|------|------|
| Agent 探针自动生成 | ⬜ | 根据异常现象自动生成 DSL 探针脚本 |
| Agent 多轮分析 | ⬜ | 第一轮采样 → 看不懂 → 加新探针 → 再采样 → 缩小范围 |
| 历史分析记录存储 | ⬜ | 每次分析的采样数据和结论存入 KV Cache（复用 my_db 的 /memory/） |
| 基线对比 | ⬜ | 与历史正常基线对比，自动识别异常 |
| 诊断知识积累 | ⬜ | 将每次定位的问题模式存储为可复用的诊断知识 |

### 4.1 Agent 工作流示例

```
Agent 收到告警: PID 31274 CPU 突增 80%

Step 1: 自动采样 60s
  tracec run basic.trace --pid 31274 --duration 60
  → 热点: ngx_ssl_handshake (50%)

Step 2: Agent 不认识 ngx_ssl_handshake
  → 自动加探针追踪这个函数的 per-request 频次
  
Step 3: 发现 10000 req/s 来自同一 IP
  → 自动建议: SSL 重放攻击，配置 rate limit

Step 4: 记录本次诊断
  → 存储到 KV Cache，下次遇到类似模式可直接匹配
```

---

## Phase 5：工具链完善

| 任务 | 状态 | 说明 |
|------|------|------|
| `tracec` CLI 命令设计 | ⬜ | compile / run / list / help 子命令 |
| 自动目标检测 | ⬜ | 运行 `tracec` 时自动检测目标环境（内核版本、可用工具） |
| 容器/K8s 发现 | ⬜ | 从宿主机自动发现容器进程 |
| 安装脚本 | ⬜ | 一键安装依赖（perf、BCC、SystemTap、FlameGraph） |
| 开源许可证 | ⬜ | 确定并添加 LICENSE |

---

## 验证测试矩阵

### 测试程序

```
test/fib.lua         LuaJIT 斐波那契计算     → 验证 CPU 热点定位
test/alloc.lua       LuaJIT 大量内存分配     → 验证内存热点定位
test/lock.c          多线程锁竞争            → 验证锁热点定位
test/nginx.conf      nginx 实际负载          → 验证端到端集成
```

### 验收标准

```
Phase 1 完成标准:
  ✅ DSL → SystemTap → JSON → 报告 全链路跑通
  ✅ 对一个已知程序采样，TOP 1 热点正确
  ✅ 采样开销 < 5%
  ✅ 报告输出包含: 函数名、文件:行号、CPU 占比、调用者

Phase 2 完成标准:
  ✅ eBPF/perf/DTrace/GDB 后端全部可用
  ✅ 自动降级策略工作正常
  ✅ 所有后端输出统一 JSON schema

Phase 3 完成标准:
  ✅ on-CPU / off-CPU / 内存三种火焰图可生成
  ✅ 差异对比可用

Phase 4 完成标准:
  ✅ Agent 自动生成探针
  ✅ Agent 多轮分析可缩小问题范围
  ✅ 诊断知识可积累和复用
```

---

> **文档版本**: 2026-06-10
> **参考文档**: `trace/README.md` (架构与技术原理)
