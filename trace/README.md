# Trace — 开源动态追踪 & 性能分析框架

> 对标 OpenResty XRay 的开源替代方案
> 给任意二进制程序"拍 X 光"，5 分钟找到热点代码行
>
> 本文档深受章亦春（agentzh）《动态追踪技术漫谈》启发：
> https://blog.openresty.com.cn/cn/dynamic-tracing

---

## 一、一句话

**对一个正在运行的二进制程序采样 5 分钟，直接告诉你最热的代码在哪个文件的哪一行。**

不需要预索引源码、不需要静态分析、不需要事先准备。

```
perf record -F 99 -g -p $PID -- sleep 300
    ↓
perf report --stdio
    ↓
25.3%  lj_gc_step  lj_gc.c:724    ← 直接定位
```

---

## 二、动态追踪的核心思想

本章节内容提炼自章亦春(agentzh)的《动态追踪技术漫谈》。

### 2.1 活体分析：把运行中的程序当数据库查

动态追踪的本质是**"活体分析"**。程序仍然在线上处理真实请求时，就可以从外部对它进行分析，就像查询一个只读数据库一样。这个"数据库"的信息源就是**正在运行的软件系统本身**——它包含了绝大部分的宝贵信息。

操作系统内核扮演着"造物主"的角色，拥有绝对权限，能确保查询不会影响到系统本身的正常运行。

### 2.2 探针机制：针灸式诊断

在软件系统的关键"穴位"上安置探针，每个探针上定义自定义的"传感器"，自由采集所需的关键信息。

这种追踪涉及两个维度：
- **时间维度**：程序持续运行，有时间线上的连续变化
- **空间维度**：跨多个进程，包含内核进程，可以纵向（跨软件层次）和横向（跨进程空间）获取信息

### 2.3 核心优点

- **非侵入式**：不修改代码、不重启、不配置，像给奔跑的人拍 X 光
- **热插拔**：随时运行、随时采样、随时结束
- **低开销**：精心编写的探针对系统极限性能影响在 5% 以下，只发生在采样期间
- **按需采集**：不上线就不知道需要什么数据，动态追踪实现了"随时随地，按需采集"

### 2.4 调试符号：二进制世界的灯塔

动态追踪依赖 **DWARF 格式的调试符号**——它将二进制中的地址映射回源码的函数名、变量名、行号。没有调试符号，就像在黑暗中摸黑前行。

- GCC 4.5 之前生成的调试符号质量较差，4.5 之后有长足进步
- 开源软件栈（内核 + 系统软件 + 应用）全开源时，动态追踪的威力最大化
- 大部分发行版提供 debuginfo/DBG 包，无需自己编译

---

## 三、技术选型：DTrace vs SystemTap vs eBPF vs perf

### 3.1 历史演进

| 阶段 | 时间 | 技术 | 说明 |
|------|------|------|------|
| 鼻祖 | 2000s | **DTrace** (Solaris) | Sun 发明，D 语言脚本，内核 VM 常驻 |
| Linux 继承 | 2000s | **SystemTap** (Red Hat) | 功能最强，用户态符号自动加载，有循环 |
| 内核机制 | 2005 | kprobes | 内核函数入口/出口设置探针 |
| 用户态探针 | 2012 | uprobes (Linux 3.5) | 用户态函数入口探针 |
| 返回探针 | 2013 | uretprobes (Linux 3.10) | 用户态函数返回探针 |
| 新一代VM | 2014+ | eBPF (Linux 3.15+) | 内核内虚拟机，LLVM 编译 C→eBPF 字节码 |
| 产品化 | 2017+ | **OpenResty XRay** | Y 语言 + Stap+/eBPF+/GDB/ODB 多后端 |

### 3.2 DTrace：缺循环和用户态符号

- **优点**：与内核紧密集成，D 语言 VM 常驻内核，启动极快
- **缺点**：没有循环结构（官方担心过热，但 agentzh 认为可以在 VM 级别限制）；用户态符号需手工声明，不自动加载
- **移植**：macOS 自带、FreeBSD 有移植、Linux 移植一直未达生产级别

### 3.3 SystemTap：功能最强大的 Linux 方案

agentzh 认为 **SystemTap 是目前 Linux 世界功能最强大、最实用的动态追踪框架**。

- **优点**：用户态调试符号自动加载；有循环结构，可编写复杂分析逻辑；agentzh 本人贡献过重要补丁（支持任意探针上下文访问用户态全局变量）
- **缺点**：不是 Linux 内核的一部分，需追赶内核变化；脚本被编译为内核模块 C 源码，需要内核头文件和 C 编译器，启动慢
- **stap++**：agentzh 对 SystemTap 的宏语言扩展，封装了常用模式

### 3.4 eBPF：有严重的限制

agentzh 的评价："eBPF 在设计上一直有严重的限制，使得那些基于 eBPF 开发的动态追踪工具始终停留在较为简单的水平上，用我的话来说，还停留在'石器时代'。"

- eBPF 作为新一代内核 VM，可用于构建类似 DTrace 的常驻追踪框架
- BCC (BPF Compiler Collection) 用 LLVM 把 C 代码编译为 eBPF 字节码
- 但 eBPF 的能力上限远低于 SystemTap

### 3.5 各后端能力对比

| | perf | eBPF (BCC) | SystemTap | DTrace | GDB |
|--|------|------------|-----------|--------|-----|
| 安装 | 内核自带 | 需装 BCC | 需装 + 头文件 | macOS 自带 | 系统自带 |
| 开销 | 1-3% | <1% | 1-5% | <1% | 10-50% |
| 用户态符号 | ✅ 自动 | ⚠️ 部分 | ✅ 自动 | ⚠️ 需声明 | ✅ 完全 |
| 自定义逻辑 | ❌ | 有限 | **强** | 有限(无循环) | 完全 |
| 循环结构 | ❌ | ❌ | ✅ | ❌ | ✅ |
| 启动速度 | 极快 | 快（需 LLVM） | 慢（编译内核模块）| 极快 | 快 |
| 适用场景 | 通用 CPU 热点 | 自定义计数器 | **复杂追踪** | macOS/BSD | 兜底 |

**Trace 默认选择策略**：优先 SystemTap（功能最强）→ 退化 eBPF（低开销）→ 退化 perf（零安装）→ 退化 GDB（兜底）。用户无感。

---

## 四、核心架构

### 4.1 统一探针 DSL

类似 XRay 的 Y 语言 / agentzh 的 stap++。同一份探针脚本，编译到不同后端。

```
// basic.trace — 5 分钟采样脚本

// CPU 热点
probe cpu {
    freq 99                 // 99 Hz 采样
    duration 300            // 5 分钟
    collect_stack           // 收集调用栈
    collect_caller          // 记录调用者
}

// 内存分配
probe memory {
    on "malloc"             // uprobe libc:malloc
    on "free"
    collect(alloc_count)
    collect(bytes_total: sum(size))
    collect(high_water_mark)
}

// 锁竞争
probe lock {
    on "pthread_mutex_lock"
    on "futex"
    collect(contention_count)
    collect(wait_time: hist)
}
```

### 4.2 编译器架构

```
探针 DSL
    │
    ▼ 探针编译器 (tracec)
    │
    ├── SystemTap ← 首选 (功能最强)
    ├── eBPF/BCC  ← 次选 (低开销)
    ├── perf      ← 兜底 (内核自带)
    ├── DTrace    ← macOS/FreeBSD
    └── GDB Python ← 最终兜底 (慢但通用)
    │
    ▼
性能数据 (JSON: 函数名 + 频次 + 耗时 + 源码位置)
    │
    ▼
分析报告 (直接定位到源码行)
```

---

## 五、技术原理：从二进制地址到源码行

### 5.1 关键链路

```
perf 采到 RIP = 0x7f3a8c1b2408
    │
    ▼ 查 ELF .debug_info 段 (DWARF 格式)
函数名 + 偏移:  lj_gc_step + 0x84
    │
    ▼ addr2line / perf annotate
源码行:  lj_gc.c:724
    │
    ▼ (可选) 带上调用关系
perf report -g callers  →  谁调了 lj_gc_step
```

### 6.1 什么是火焰图
### 5.2 需要什么条件

```
基本要求:  perf_event_paranoid ≤ 1（普通用户也能采）
可选:     有调试符号或已索引到 code_search（用于地址→函数名）
```

### 5.3 符号重建 vs 运行时感知

perf 采到裸地址后，要翻译成 `文件:行号`，涉及两个层次。

#### 第一层：符号重建（地址 → 函数名）

```
perf 采到 0x5555000a1b40
    ↓ 查 ELF .symtab / .dynsym
lj_gc_step                ← 函数名
    ↓ cache_query "lj_gc_step" --repo /code/LuaJIT/LuaJIT
lj_gc.c:724               ← 代码位置
```

对于 C/C++/Rust/Zig 编译的二进制，ELF 符号表就够了。即使被 strip，`.dynsym` 通常保留。
对于已经用 `analyze_repo.sh` 索引过的项目，`code_search` 可以直接从源码定位，不需要 DWARF。

#### 第二层：运行时感知（C 引擎栈 → 脚本层函数）

这是真正的技术壁垒。

对于 JavaScript、Lua、Python、PHP 等动态语言，perf 只能看到**运行时引擎的 C 代码**，看不到脚本层的函数。因为这些语言的 JIT 编译器或解释器把脚本代码编译为机器码后，存在**进程堆内存里**，不在 ELF 段里。

```
perf 采到 0x5555001c2408
    ↓ 查 ELF 符号表
v8::Array::New               ← V8 引擎的 C++ 函数
    ↓ 真正在执行的 JS 代码
sortData (app.js:42)         ← 看不到！在 V8 堆里
```

每个运行时内部都维护着一张 **"机器码地址 → 源码位置"** 的映射表，但存放位置和格式完全不同：

```
运行时       映射表位置                             关键结构体
──────────────────────────────────────────────────────────────────
V8           Isolate->code_cache                   v8::internal::Isolate
LuaJIT       jit_State->trace[].mcode              GCtrace, MCode
CPython      PyThreadState->frame->f_code          PyFrameObject
PHP          zend_executor_globals->opline          zend_op_array
JVM          Klass->vtable                          java_lang_ClassLoader
```

要拿到映射数据，需要：

```
V8 运行时感知:
  1. 读 V8 源码 src/execution/isolate.h → 找到 Isolate 结构体布局
  2. 找到 code_cache 的存储格式 (Builtins + Code 对象)
  3. 遍历所有已编译代码，提取 地址范围 → (文件名, 行号, 函数名)
  4. perf 采到地址后反向查询此映射表

LuaJIT 运行时感知:
  1. 读 lj_jit.h → 找到 jit_State + GCtrace 结构体
  2. 遍历 GCtrace 链表，读 mcode 字段（编译后的机器码地址）
  3. 通过 lj_debug.c 提取对应的行号信息
  4. 构建地址范围 → 函数名映射表
```

这就是 XRay 真正的技术壁垒——不是探针 DSL，不是符号重建，而是 **对每个运行时内部数据结构的深入理解**。每接入一种语言，就要读透它的运行时源码，找到这张映射表，实现提取器。每个运行时的版本更新都可能改变结构体布局。

```
              符号重建                             运行时感知
────────────────────────────────────────────────────────────────────
问题          地址 → 函数名                         C 引擎栈 → 脚本层函数
数据来源      ELF .symtab / .dynsym               运行时的堆内存数据结构
解决方案      code_search + ELF 符号表              读源码 + 实现遍历器
覆盖语言      C/C++/Rust/Zig (ELF 格式)            JS/Lua/Python/PHP/JVM
难度          ✅ 已有方案                           🔲 每个语言独立实现
```

### 5.4 没有调试符号的兜底

```bash
# 安装 debuginfo 包（大部分发行版提供）
dnf debuginfo-install nginx     # RHEL/Fedora
apt install nginx-dbg           # Debian/Ubuntu

# 或者自己编译带 -g 的版本
./configure --with-debug && make -j

# 对于已索引的项目，code_search 直接定位
cache_query "lj_gc_step" --repo /code/LuaJIT/LuaJIT --type context
```

由 Brendan Gregg 发明，是给软件系统拍的 X 光照片。它将时间和空间两个维度的信息融合在一张图上，直观反映性能方面的定量统计规律。

### 6.2 火焰图种类

| 类型 | 采集方式 | 诊断目标 |
|------|---------|---------|
| **on-CPU 火焰图** | 采 CPU 上的调用栈 | CPU 热点函数 |
| **off-CPU 火焰图** | 采进程休眠时的调用栈 | 锁竞争、I/O 阻塞、调度延迟 |
| **内存火焰图** | uprobe malloc/free | 内存分配热点、泄漏 |

**off-CPU 火焰图**是 agentzh 的创新贡献。他首次在 Nginx（单线程模型）上成功应用，相比 Brendan 在多线程程序上的尝试效果更好——Nginx 的 off-CPU 火焰图只有 `epoll_wait` 一个噪音点，很容易识别并忽略。

**内存泄漏火焰图**也是 agentzh 的实战成果：成功定位了 Nginx 核心中 Valgrind 和 AddressSanitizer 都无法捕捉的微妙泄漏（发生在 Nginx 自己的内存池中）。

### 6.3 生成命令

```bash
# on-CPU 火焰图
perf record -F 99 -g -p $PID --sleep 60
perf script | stackcollapse-perf.pl | flamegraph.pl > on-cpu.svg

# off-CPU 火焰图
perf record -e sched:sched_switch -g -p $PID --sleep 60
perf script | stackcollapse-perf.pl | flamegraph.pl > off-cpu.svg
```

---

## 七、方法论（agentzh & Brendan Gregg）

### 7.1 小步推进，连续求问

不要指望一次编写一个庞大的工具采集所有信息然后解决问题。应该把最终问题分解成一系列小假设，逐步验证、逐步修正方向。

好处：
- 每步工具足够简单，工具本身不会引入 bug
- 引入的探针少，对生产系统开销小
- 每个工具可复用

### 7.2 拒绝大数据

不要一次性采集尽可能全的数据。**在每一步只采集当前真正需要的信息**，基于已采集的信息指导下一步的方向。这与"全量采集、事后分析"的传统做法完全相反。

### 7.3 守株待兔

对于小概率事件（如 1% 的长尾请求），设阈值筛选，只抓取超过阈值的请求进行分析。而不是全量采集再从中筛选。

### 7.4 知识就是力量

agentzh 将动态追踪比作杨过的玄铁重剑——完全不懂武功的人使不动，但只要会一些，就可以越使越好，直至木剑也能横行天下。

**"鼓励工程师不断深入学习的工具才是有前途的好工具。"**

XRay 的商业价值就在于将 agentzh 十余年的排障经验 codified 成知识库。Trace 的策略不同：不预置知识库，而是让 AI Agent 拿着探针去现场实时探索，在探索中积累知识。

---

## 八、实战案例（来自文章）

章亦春在文章中分享了他使用 SystemTap 解决的真实线上问题，证明动态追踪的实际价值：

| 案例 | 问题 | 工具 | 发现 |
|------|------|------|------|
| 1 | Nginx CPU 异常高 | 火焰图 | 发现同事遗弃的调试代码（埋点未清理）|
| 2 | 长尾请求（秒级） | SystemTap 延时分析 | DNS 查询 CNAME 展开过慢，非 OpenResty 问题 |
| 3 | 机房 1% 网络超时 | SystemTap 超时分析 | 根本原因是硬盘配置问题（从网络到硬盘）|
| 4 | 文件操作 CPU 高 | 火焰图 | 文件句柄缓存过大 → 自旋锁抵消了缓存收益 |
| 5 | 正则编译 CPU 高 | 火焰图 | 正则缓存大小不够，超出后反复编译 |

这些案例揭示的共同模式：**没有火焰图/动态追踪时，团队只能胡乱猜测和试错；有了它，第一手数据直接给出方向。**

---

## 九、和 XRay 的对标

### 9.1 功能对照

| 功能 | XRay | Trace |
|------|------|-------|
| **on-CPU 火焰图** | ✅ | ✅ perf + FlameGraph |
| **off-CPU 火焰图** | ✅ | ✅ perf sched |
| **内存分配热点** | ✅ | ✅ uprobe malloc |
| **内存泄漏火焰图** | ✅ | ✅ 同原理可做 |
| **锁竞争分析** | ✅ | ✅ perf lock / eBPF |
| **Crash dump** | ✅ | ✅ GDB 自动化 |
| **容器/K8s 支持** | ✅ | ✅ host agent |
| **多语言混合栈** | ✅ | ⚠️ 限于 debug symbols |
| **自动化根因推理** | ✅ 知识库驱动 | ❌ AI Agent 在线探索 |
| **无符号分析** | ✅ 自研算法 | ❌ 需要调试符号 |
| **零开销常驻** | ✅ | ❌ 按需采样 |

### 9.2 核心差异

```
XRay:  15 年排障经验 → 知识库 → 自动匹配模式 → 给结论
Trace: 采样 5 分钟 → AI Agent 看不懂 → 加新探针 → 再采 → 定位
       知识库不预先写，在探索中积累
```

XRay 的壁垒是**做好的知识库**。Trace 的策略是：**不需要提前知道——AI 拿着探针去现场查**。

### 9.3 参考来源

本文档的核心技术理念来自：
- **《动态追踪技术漫谈》** — 章亦春 (agentzh) https://blog.openresty.com.cn/cn/dynamic-tracing
- **Brendan Gregg 的火焰图和性能分析方法论** — http://www.brendangregg.com
- **SystemTap 官方文档** — https://sourceware.org/systemtap
- **OpenResty XRay 产品介绍** — https://openresty.com.cn/cn/xray

---

## 十、开发路线

### Phase 1: SystemTap 后端

优先实现 SystemTap 后端（功能最强），可采集 CPU + 内存 + 锁热点，输出 JSON 报告。

### Phase 2: 多后端覆盖

eBPF + perf + DTrace + GDB 后端，覆盖 Linux/macOS/BSD。

### Phase 3: AI Agent 集成

Agent 自动生成探针、自动分析、自动积累诊断知识。

### Phase 4: 和 code_search 的可选融合

两个独立工具，不强制绑定。融合是可选加成，不是必要条件。

```
perf 告诉你热点在 lj_gc_step:724
code_search 还能告诉你:
  - 这个函数的调用链
  - 它的数据流
  - 它在架构里的角色

不是定位问题必须的，但对理解问题和修代码有帮助。
```

---

> **文档版本**: 2026-06-10
> **参考文章**: https://blog.openresty.com.cn/cn/dynamic-tracing
> **一句话**: 给二进制拍 X 光，5 分钟定位热点代码行。
