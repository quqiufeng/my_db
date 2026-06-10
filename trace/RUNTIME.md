# 运行时感知架构 — 从裸地址到源码行

> 本文档描述 tracec 如何从 perf 采样裸地址出发，经过运行时映射表提取和 code_search 查询，最终定位到 `文件:行号` 的完整链路。

---

## 一、核心问题

perf 采到的是 CPU 正在执行的**机器码地址**（RIP），对于 C/C++ 代码，查 ELF 符号表就能翻译成函数名。但对于 JavaScript、Lua、Python 等动态语言，perf 只能看到运行时引擎的 C 代码（如 `v8::Array::New`），看不到脚本层的业务函数。

原因是动态语言的 JIT 编译器把脚本代码编译为机器码后，存在**进程的堆内存里**，不在 ELF 段中。需要在运行时内部的反查表才能完成翻译。

---

## 二、完整链路

```
┌─────────────────────────────────────────────────────────────────┐
│                        第一步：采样                              │
│                                                                 │
│  perf -F 99 -g -p $PID  →  每 10ms 记录一次 CPU 正在执行的 RIP  │
│                                                                 │
│  产出: raw_samples.txt                                           │
│    0x7f3a8c1b2408                                               │
│    0x7f3a8c1b2408                                               │
│    0x5555000a1b40                                               │
│    ...                                                          │
└──────────────────────────┬──────────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────────┐
│                   第二步：翻译裸地址                               │
│                                                                 │
│          ┌──────────────────┴──────────────────┐                │
│          ▼                                     ▼                │
│   C/C++ 二进制                          动态语言进程              │
│          │                                     │                │
│   ELF .symtab                          tracec extract            │
│   / .dynsym                            (详见第三章)              │
│          │                                     │                │
│   函数名 + 偏移                   运行时内部结构体                  │
│   lj_gc_step+0x84                       │                        │
│          │                    jit_State->trace[]                 │
│          ▼                    GCtrace->mcode                     │
│   addr2line                             │                        │
│          │                     匹配地址范围                       │
│          │                             │                        │
│          ▼                             ▼                        │
│   lj_gc.c:724                  lj_trace.c:499                   │
│   trace_stop                                                    │
│          │                             │                        │
│          └────────────┬────────────────┘                        │
│                       ▼                                         │
│                 统一输出:                                        │
│                 { function:"trace_stop",                         │
│                   file:"lj_trace.c",                             │
│                   line:499 }                                     │
└───────────────────────┬──────────────────────────────────────────┘
                        │
                        ▼
┌─────────────────────────────────────────────────────────────────┐
│                    第三步：code_search 增强                       │
│                                                                 │
│  拿到 function: "trace_stop"                                     │
│         │                                                       │
│   cache_query "trace_stop" --type context --depth 2              │
│         │                                                       │
│   {                                                              │
│     "file": "lj_trace.c:499",                                    │
│     "signature": "(jit_State *J)",                               │
│     "callers": [                                                 │
│       {"name": "lj_record_ins", "file": "lj_record.c:820"},      │
│       {"name": "trace_save",    "file": "lj_trace.c:580"}        │
│     ],                                                           │
│     "callees": [                                                 │
│       {"name": "trace_save",    "file": "lj_trace.c:499"},       │
│       {"name": "lj_mcode_commit", "file": "lj_mcode.c:120"}      │
│     ]                                                            │
│   }                                                              │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

---

## 三、tracec extract 详解

### 3.1 通用流程

```
tracec extract <runtime> <pid>
    │
    ▼ 步骤 1: 附加到目标进程（只读，不修改）
    GDB attach pid
    │
    ▼ 步骤 2: 找到运行时核心结构体
    尝试不同符号名，直到成功:
      lua_State *L = gdb.parse_and_eval("symbol_name")
    │
    ▼ 步骤 3: 遍历映射表
    for each entry in runtime->code_map:
        read(start_address, end_address, source_location)
    │
    ▼ 步骤 4: 构建映射表
    [ {start, end, file, line, function}, ... ]
    │
    ▼ 步骤 5: 分离进程，输出 JSON
    gdb detach
```

### 3.2 LuaJIT 实现

```
LuaJIT 映射表位置:  jit_State->trace[]
关键结构体:
  jit_State (lj_jit.h)
    └── trace[] → GCtrace (lj_jit.h)
          ├── mcode:     MCode*     ← 编译后的机器码地址
          ├── szmcode:   MSize      ← 机器码大小
          ├── traceno:   TraceNo    ← 追踪编号
          └── startpc:   BCIns*     ← 对应字节码 PC
                                      → lj_debug_line() → 行号

提取命令:
  tracec extract luajit <pid>
  → 输出 JSON: [{start, end, traceno, source: "luajit_jit"}, ...]

地址匹配:
  0x7f3a8c1b2408 落在 GCtrace.mcode .. mcode+szmcode 范围内
  → traceno = 5
  → startpc → lj_debug_line(proto, startpc) → 行号 499
  → 函数名: 通过 traceno 查找 trace 对应的函数名
```

### 3.3 V8/Node.js 实现（待实现）

```
V8 映射表位置:  Isolate->code_cache
关键结构体:
  Isolate (v8/src/execution/isolate.h)
    └── code_cache → std::unordered_map
          └── Code (v8/src/codegen/code.h)
                ├── InstructionStart():  Address   ← 机器码地址
                ├── InstructionSize():   int       ← 大小
                └── source():            Handle<Script>
                      ├── name():        String    ← 文件名
                      └── line_offset(): int       ← 行号偏移

提取命令:
  tracec extract v8 <pid>
  → 输出 JSON: [{start, end, file, line, function}, ...]
```

### 3.4 CPython 实现（待实现）

```
CPython 映射表位置:  PyThreadState->frame
关键结构体:
  PyThreadState (Include/pystate.h)
    └── frame → PyFrameObject (Include/frameobject.h)
          ├── f_code → PyCodeObject
          │     ├── co_filename:    PyObject*  ← 文件名
          │     ├── co_firstlineno: int        ← 起始行号
          │     └── co_code:        PyObject*  ← 字节码指令
          ├── f_lineno:  int        ← 当前行号
          └── f_back:    PyFrameObject*  ← 上一层调用栈

提取命令:
  tracec extract cpython <pid>
  → 遍历 frame 链表 → 输出 JSON: [{file, line, function}, ...]
```

---

## 四、与 code_search 的集成

提取映射表 + code_search = 完整的代码级定位。

```
perf 裸地址
    ↓
运行时映射表 → 函数名 + 行号
    ↓
cache_query → 调用者列表
            → 被调用者列表
            → 数据流信息
            → 函数签名
```

这种组合是 XRay 没有的——XRay 只做动态追踪，不做源码级的调用图分析。

---

## 五、适用场景

| 场景 | C/C++ | LuaJIT | V8/JS | CPython | 说明 |
|------|-------|--------|-------|---------|------|
| ELF 符号表查询 | ✅ | ✅ | ✅ | ✅ | .symtab/.dynsym |
| 运行时映射表 | ❌ 不需要 | ✅ trace[] | ✅ code_cache | ✅ frame 链 | extract 命令 |
| code_search 增强 | ✅ | ✅ | ✅ | ✅ | 可选，不强制 |
| 状态 | ✅ 已实现 | ✅ 已实现 | ⬜ 待实现 | ⬜ 待实现 | |

---

> **文档版本**: 2026-06-10
> **相关工具**: tracec extract, cache_query, perf
> **参考**: trace/README.md (架构总览), trace/DSL.md (探针语言)
