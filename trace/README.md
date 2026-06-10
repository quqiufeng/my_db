# Trace — 开源动态追踪 & 性能分析框架

> 对标 OpenResty XRay 的开源替代方案
> 给任意二进制程序"拍 X 光"，5 分钟找到热点代码行

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

## 二、核心架构

### 2.1 技术栈

```
探针 DSL (统一接口)
    │
    ▼ 探针编译器 (tracec)
    │
    ├── perf          ←  Linux 默认 (内核自带，零安装)
    ├── eBPF (BCC)    ←  低开销自定义探针 (内核 ≥ 4.x)
    ├── SystemTap     ←  复杂追踪 (Red Hat 系)
    ├── DTrace        ←  macOS / FreeBSD
    └── GDB Python    ←  兜底方案 (慢但通用)
    │
    ▼
性能数据 (JSON: 函数名 + 频次 + 耗时 + 源码位置)
    │
    ▼
分析报告 (直接定位到源码行)
```

### 2.2 各后端对比

| | perf | BCC (eBPF) | SystemTap | DTrace | GDB |
|--|------|------------|-----------|--------|-----|
| 安装 | 内核自带 | 需装 BCC | 需装 + 头文件 | 系统自带 | 系统自带 |
| 开销 | 1-3% | <1% | 1-5% | <1% | 10-50% |
| 自定义逻辑 | ❌ | 有限 | 强 | 有限 (无循环) | 完全 |
| 适用 | 通用 CPU 热点 | 自定义计数 | 复杂追踪 | macOS/BSD | 兜底 |

**默认**：优先 perf → eBPF 退化 → SystemTap 退化 → GDB。用户无感。

---

## 三、技术原理：从二进制地址到源码行

### 3.1 关键链路

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

### 3.2 需要什么条件

```
必须:  二进制编译时加了 -g（保留 debug symbols）
       perf_event_paranoid ≤ 1（普通用户也能采）

可选:  有源码（修 bug 时用）
       无源码也能看热点（但只有函数名，没行号）
```

### 3.3 没有 debug symbols 怎么办

```bash
# 安装 debuginfo 包 (大部分发行版提供)
dnf debuginfo-install nginx     # RHEL/Fedora
apt install nginx-dbg           # Debian/Ubuntu

# 或者自己编译带 -g 的版本
./configure --with-debug && make -j
```

如果实在没有 debug symbols，至少 perf 能给出函数名，配合 code_search 能查到这个函数在架构里的角色——但这不是必须的。

---

## 四、探针 DSL

类似 XRay 的 Y 语言。同一份探针脚本，编译到不同后端。

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

### 编译示例

```bash
# 自动检测目标环境，选择最佳后端
tracec compile basic.trace --target auto

# 或手动指定后端
tracec compile basic.trace --target perf
tracec compile basic.trace --target ebpf
tracec compile basic.trace --target systemtap
tracec compile basic.trace --target gdb
```

---

## 五、AI Agent 使用模式

### 5.1 主动排查

```
用户: "程序变慢了"

Agent:
  Step 1: 采样 5 分钟
    tracec run basic.trace --pid 31274

  Step 2: 读报告
    热点: lj_gc_step:724 (40%)
    调用者: lj_gc_step_jit:764 (60%)
            lua_pcall (30%)

  Step 3: 继续追踪
    tracec add --probe "lj_gc_step_jit" --collect "duration_hist"

  Step 4: 结论
    JIT 编译频繁触发 → GC 阈值太激进 → 建议调整
```

### 5.2 告警自动响应

```
告警: CPU 突增 80%

Agent:
  → 自动 perf record -F 99 -g -p $PID --sleep 60
  → 对比基线火焰图
  → 发现 ngx_ssl_handshake 新增 50%
  → 追踪调用源 IP
  → 同一 IP 10000 req/s → SSL 重放攻击
  → 自动建议加 rate limit
```

### 5.3 Agent 写探针

```bash
# Agent 观察后自动生成针对性探针
cat << PROBE | tracec run --pid $PID
probe "lj_gc_step" {
    collect(call_count)
    collect(duration_ns: hist)
    collect(caller)
}
probe "lj_str_new" when bytes > 1000 {
    collect(caller)
    collect(content_preview: 32)
}
PROBE
```

---

## 六、和 XRay 的对标

### 6.1 功能对照

| 功能 | XRay | Trace |
|------|------|-------|
| **on-CPU 火焰图** | ✅ | ✅ perf + FlameGraph |
| **off-CPU 火焰图** | ✅ | ✅ perf sched |
| **内存分配热点** | ✅ | ✅ uprobe malloc |
| **锁竞争分析** | ✅ | ✅ perf lock / eBPF |
| **Crash dump** | ✅ | ✅ GDB 自动化 |
| **容器/K8s** | ✅ | ✅ host agent |
| **多语言混合栈** | ✅ | ⚠️ 限于 debug symbols 质量 |
| **自动化根因推理** | ✅ 知识库驱动 | ❌ AI Agent 在线探索代替 |
| **无符号分析** | ✅ 自研算法 | ❌ 需要调试符号 |
| **零开销常驻** | ✅ | ❌ 按需采样 |

### 6.2 核心差异

```
XRay:  15 年排障经验 → 知识库 → 自动匹配模式 → 给结论
Trace: 采样 5 分钟 → AI Agent 看不懂 → 加新探针 → 再采 → 定位
       知识库不预先写，在探索中积累
```

XRay 的壁垒是**做好的知识库**。Trace 的策略是：**不需要提前知道——AI 拿着探针去现场查**。

---

## 七、开发路线

### Phase 1: tracec 探针编译器

```
perf 后端 → 可采集 CPU + 内存 + 锁热点 → 输出 JSON 报告
```

### Phase 2: 多后端

```
eBPF + SystemTap + DTrace + GDB 后端 → 覆盖 Linux/macOS/BSD
```

### Phase 3: AI Agent 集成

```
Agent 自动生成探针、自动分析、自动积累诊断知识
```

---

## 八、和 code_search 的关系

**没有依赖关系。** 两个独立工具：

```
code_search:  你看源码时用 → 搜函数、看调用关系
Trace:        程序跑慢时用 → 采样 5 分钟 → 找到热点行

各自独立可用。不强制绑定。
```

但如果两个都用，有个可选加成：

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
> **一句话**: 给二进制拍 X 光，5 分钟定位热点代码行。
