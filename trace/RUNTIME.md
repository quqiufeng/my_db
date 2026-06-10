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

实现路径:
  Step 1: 通过 GDB 找到 Isolate 指针
    - V8 嵌入时通常有全局变量保存 Isolate*
    - Node.js: node::IsolateData 或直接 node::Environment
    - 或通过 pthread_getspecific 找线程局部存储的 Isolate
  
  Step 2: 访问 Isolate->code_cache
    - code_cache 是 std::unordered_map<CodeDesc, Code*>
    - 遍历所有 Code 对象
    
  Step 3: 从 Code 对象提取映射
    Code* (v8/src/codegen/code.h)
      ├── InstructionStart():  Address    ← 机器码起始地址
      ├── InstructionSize():   int        ← 机器码大小
      └── SourcePositionInfo():
            ├── script->name()->ToString() ← 文件名 (app.js)
            ├── line_offset()             ← 行号偏移
            └── function_name()           ← 函数名 (sortData)
  
  Step 4: 构建映射表输出
    tracec extract v8 <pid>
    → JSON: [{start, end, file, line, function}, ...]

  GDB Python 关键代码:
    isolate = gdb.parse_and_eval("v8::Isolate::GetCurrent()")
    code_cache = isolate['code_cache']
    for entry in code_cache:
        code = entry['code']
        script = code['script']
        file = script['name']['value']
        start = int(code['InstructionStart']())
        size  = int(code['InstructionSize']())
```

### 3.4 CPython 实现（待实现）

```
CPython 映射表位置:  PyThreadState->frame->f_code

实现路径:
  Step 1: 找到 PyThreadState
    Python.h: PyThreadState *PyThreadState_Get()
    或通过全局变量 _PyRuntime 访问
  
  Step 2: 遍历 frame 链表
    PyThreadState->frame → PyFrameObject
      ├── f_code → PyCodeObject
      │     ├── co_filename:    PyObject*    ← 文件名字符串
      │     ├── co_firstlineno: int           ← 函数起始行号
      │     └── co_name:        PyObject*    ← 函数名
      ├── f_lineno:  int         ← 当前执行行号
      └── f_back:    PyFrameObject*  ← 上一帧 (调用者)
    
    frame 链表就是当前线程的 Python 调用栈
  
  Step 3: 遍历所有线程
    每个线程有自己的 PyThreadState
    需要遍历所有线程才能拿到完整的运行时调用关系
  
  Step 4: 构建映射表输出
    tracec extract cpython <pid>
    → JSON: [{file, line, function, thread_id}, ...]

  GDB Python 关键代码:
    tstate = gdb.parse_and_eval("PyThreadState_Get()")
    frame = tstate['frame']
    while frame:
        code = frame['f_code']
        name = code['co_name']['ob_type']['tp_name']
        file = code['co_filename']['ob_type']['tp_name']
        line = int(frame['f_lineno'])
        frames.append({file, line, function: name})
        frame = frame['f_back']
```

### 3.5 PHP 实现（待实现）

```
PHP 映射表位置:  zend_executor_globals->opline

实现路径:
  Step 1: 找到 executor_globals
    PHP 维护全局变量 executor_globals (zend_executor_globals*)
    编译 PHP 时保留此符号即可访问
  
  Step 2: 读取当前执行状态
    zend_executor_globals (Zend/zend_globals.h)
      ├── opline:     zend_op*      ← 当前执行的字节码指令
      ├── function_state:
      │     └── function: zend_function
      │           ├── common.function_name ← 函数名
      │           └── op_array: zend_op_array
      │                 ├── filename:  char*  ← 文件名
      │                 └── line_start: int    ← 起始行号
      └── prev_execute_data → zend_execute_data
              └── call 链 = PHP 调用栈
    
  Step 3: 遍历调用栈
    zend_execute_data->call 链就是 PHP 的调用栈
    每层可以读出: 函数名、文件、当前行号
    opline->lineno 是当前执行的行号
  
  Step 4: 构建映射表输出
    tracec extract php <pid>
    → JSON: [{file, line, function, ...}, ...]

  GDB Python 关键代码:
    eg = gdb.parse_and_eval("executor_globals")
    opline = eg['opline']
    line = int(opline['lineno'])
    func = eg['function_state']['function']['common']['function_name']
```

### 3.6 JVM 实现（待实现）

```
JVM 映射表位置:  Klass->vtable

实现路径:
  JVM 的情况不同 — JVM 本身有完善的工具接口
  优先使用标准工具而非 GDB 提取:
  
  方式 A: 使用 JMX + ThreadMXBean
    - 通过 JMX 连接目标 JVM
    - ThreadMXBean.dumpAllThreads() + CPU 时间
    - 不需要 GDB
  
  方式 B: 使用 perf + perf-map-agent
    - perf record -F 99 -g -p $PID
    - perf-map-agent 读取 JIT 编译的 /tmp/perf-<pid>.map
    - JVM 在 -XX:+PreserveFramePointer 下产出 perf 可直接读的符号
  
  方式 C: 直接读 JVM 堆
    - 每个 Java 线程的 JavaThread->vframe()
    - 遍历 vframe 链 = Java 调用栈
    - 读出 method->name, bci→行号
  
  推荐走方式 B (标准工具链，不需要 GDB)
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

## 五、各运行时的实现优先级

| 优先级 | 运行时 | 命令 | 难度 | 理由 |
|--------|--------|------|------|------|
| ✅ 已实现 | LuaJIT | `extract luajit` | 中 | 结构体固定，trace[] 数组遍历简单 |
| ⬜ 优先 | V8/Node.js | `extract v8` | 中 | Isolate->code_cache 需要遍历 std::map |
| ⬜ 次优先 | CPython | `extract cpython` | 低 | frame 链表遍历最简单 |
| ⬜ 按需 | PHP | `extract php` | 中 | zend_executor_globals 结构复杂 |
| ⬜ 按需 | JVM | `extract jvm` | 低 | 走 perf-map-agent 标准工具 |

每个运行时的 extract 实现大约 100-200 行 GDB Python 代码，结构完全一致：

```
gdb 附加进程 → 找到运行时核心结构体 → 遍历映射表 → 输出 JSON
```

当需要分析某个语言的性能时，按需实现即可。

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
