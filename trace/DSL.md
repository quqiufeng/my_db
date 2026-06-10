# Trace DSL 规格 v1

> 探针描述语言规格定义。
> 本文件只定义语法和语义，语言实现（解析器、编译器）属于其他项目。

---

## 一、设计原则

1. **声明式**：用户描述"要采集什么"，不描述"怎么采"
2. **后端无关**：同一份探针可编译到 perf / eBPF / SystemTap / DTrace
3. **渐进式**：简单场景 3 行搞定，复杂场景可加过滤和聚合

---

## 二、文件结构

一个 `.trace` 文件包含一个或多个 `probe` 定义块。

```
// 注释用 // 或 /* */
probe <target> {
    <clause>
    <clause>
    ...
}
```

---

## 三、探针目标 `<target>`

### 3.1 CPU 热点

```
probe cpu {
    ...
}
```

采集 CPU 调用栈分布。

### 3.2 函数

```
probe "malloc" {
    ...
}

probe "lj_gc_step" {
    ...
}
```

在指定函数的入口/出口设置探针。

### 3.3 函数 + 库

```
probe "malloc@libc.so.6" {
    ...
}
```

限定共享库中的函数。

### 3.4 内核函数

```
probe "k:__alloc_pages_slowpath" {
    ...
}
```

`k:` 前缀表示内核函数探针 (kprobe)。

### 3.5 用户态函数

```
probe "u:ngx_event_accept" {
    ...
}
```

`u:` 前缀表示用户态函数探针 (uprobe)，可省略。

### 3.6 tracepoint

```
probe "t:sched:sched_switch" {
    ...
}
```

`t:` 前缀表示内核 tracepoint。

---

## 四、子句

### 4.1 采样频率 `freq`

```
probe cpu {
    freq 99         // 每秒采样 99 次
}
```

仅用于 `probe cpu`。默认 99，范围 1-1000。

### 4.2 采样时长 `duration`

```
probe cpu {
    duration 300    // 采集 300 秒
}
```

单位秒。默认 60。

### 4.3 数据采集 `collect`

```
probe "lj_gc_step" {
    collect(call_count)         // 调用次数
    collect(duration_ns)        // 每次耗时（纳秒）
    collect(duration_ns: hist)  // 耗时直方图分布
    collect(caller)             // 调用者函数名
    collect(backtrace: depth 8) // 调用栈（最多 8 层）
    collect(backtrace: frame 3) // 调用栈的第 3 层
    collect(arg0)               // 第一个参数值
    collect(arg1)               // 第二个参数值
    collect(return)             // 返回值
}
```

支持的聚合方式：

| 聚合 | 说明 |
|------|------|
| `call_count` | 调用次数计数 |
| `duration_ns` | 每次耗时（原始值） |
| `duration_ns: hist` | 耗时直方图（对数分布） |
| `duration_ns: sum` | 耗时总和 |
| `duration_ns: avg` | 平均耗时 |
| `bytes: sum` | 字节数总和 |
| `bytes: max` | 最大字节数 |
| `arg0`..`arg9` | 第 N 个参数值 |
| `return` | 返回值 |
| `caller` | 调用者函数名 |
| `backtrace` | 完整调用栈 |
| `backtrace: depth N` | 指定深度的调用栈 |
| `backtrace: frame N` | 调用栈第 N 层 |

### 4.4 过滤条件 `when`

```
probe "malloc" when bytes > 4096 {
    collect(call_count)
    collect(caller)
    collect(bytes: sum)
}

probe "lj_gc_step" when duration_ns > 1000000 {
    // 只记录耗时超过 1ms 的 GC 步进
    collect(caller)
    collect(duration_ns: hist)
}
```

支持的比较操作：`>` `<` `>=` `<=` `==` `!=`。

### 4.5 入口/出口控制

```
probe "lj_gc_step" {
    on enter {           // 函数入口处采集
        collect(arg0)
        collect(caller)
    }
    on exit {            // 函数返回处采集
        collect(return)
        collect(duration_ns)
    }
}
```

默认行为：
- 函数探针无 `on` 子句时，仅入口采集
- `collect(duration_ns)` 自动需要出口探针
- `collect(return)` 自动需要出口探针

---

## 五、完整示例

### 5.1 CPU 热点分析（3 行）

```
probe cpu {
    freq 99
    duration 60
}
```

### 5.2 大内存分配追踪（6 行）

```
probe "malloc" when bytes > 4096 {
    collect(call_count)
    collect(bytes: sum)
    collect(backtrace: depth 8)
    collect(caller)
}
```

### 5.3 GC 性能诊断（8 行）

```
probe "lj_gc_step" {
    on enter { collect(caller) }
    on exit  { collect(duration_ns: hist) }
}

probe "lj_gc_step" when duration_ns > 500000 {
    collect(backtrace: depth 5)
}
```

### 5.4 锁竞争分析（8 行）

```
probe "pthread_mutex_lock" {
    collect(call_count)
    collect(backtrace: depth 10)
}

probe "pthread_mutex_lock" on exit {
    collect(return)     // 0 = 获取成功, 非0 = 失败
}
```

### 5.5 完整多探针文件

```
// web-server.trace — Web 服务器性能诊断

// 1. CPU 热点
probe cpu {
    freq 99
    duration 120
}

// 2. 内存分配
probe "malloc" when bytes > 4096 {
    collect(call_count)
    collect(bytes: sum)
    collect(caller)
}

// 3. 锁竞争
probe "pthread_mutex_lock" {
    collect(call_count)
    collect(backtrace)
}

// 4. 慢请求
probe "ngx_http_process_request" on exit {
    collect(duration_ns: hist)
}

probe "ngx_http_process_request" when duration_ns > 1000000000 {
    // 记录超过 1 秒的慢请求
    collect(backtrace)
    collect(caller)
}
```

---

## 六、语义约束

| 约束 | 说明 |
|------|------|
| 同一探针目标可重复定义 | 多次 `probe "malloc"` 表示多个探针处理程序 |
| 探针定义顺序无关 | 运行时执行顺序由后端决定 |
| `freq` 仅限 `probe cpu` | 函数探针不支持此子句 |
| `collect(duration_ns)` 需要出口 | 自动绑定入口/出口两个探针 |
| 参数索引从 0 开始 | `arg0` = 第一个参数 |

---

## 七、实现建议

### 7.1 后端映射参考

```
DSL 探针                perf                    SystemTap                     eBPF
────────────────────────────────────────────────────────────────────────────────────
probe cpu { freq N }    perf record -F N        profile-{N}.hz                BCC profile.py
probe "func"            perf probe func         process.function("func")      uprobe
probe "func@lib"        perf probe lib:func     process("/lib.so").function   uprobe with path
probe "k:func"          perf probe k:func       kernel.function("func")       kprobe
probe "t:tracepoint"    perf record -e tracepoint  kernel.trace("tracepoint") tracepoint
collect(call_count)     perf report -n          @count                        COUNT
collect(duration_ns)    -                       @hist_log(tid)                @hist_log
collect(backtrace)      -g                      print_ubacktrace()            stack()
collect(caller)         -g                      print_ubacktrace()[-1]        stack()[-1]
collect(arg0)           --setup arg0=$arg0      $arg0                        args->arg0
when X > Y              perf report --percent   if (X > Y) {}                FILTER
```

### 7.2 JSON 输出 schema

```json
{
  "schema_version": 1,
  "probe": "lj_gc_step",
  "backend": "systemtap",
  "target_pid": 31274,
  "duration_sec": 60,
  "results": [
    {
      "function": "lj_gc_step",
      "file": "lj_gc.c",
      "line": 724,
      "call_count": 142000,
      "duration_ns": {
        "avg": 2340,
        "p50": 2100,
        "p90": 4500,
        "p99": 12000,
        "max": 89000
      },
      "callers": {
        "lj_gc_step_jit": 85200,
        "lua_pcall": 42600,
        "lua_gc": 14200
      }
    }
  ]
}
```

---

> **版本**: v1 (2026-06-10)
> **状态**: 草稿，待 review
> **实现**: 此语言的解析器/编译器属于其他项目
