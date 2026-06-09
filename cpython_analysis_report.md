# CPython 源码架构分析报告

> 生成方式: 基于已索引的 CPython 源码（`/opt/cpython/src/`）通过 code search 完成
> 分析日期: 2026-06-09

---

## 一、整体架构概览

CPython 源码分为四大层级:

```
┌──────────────────────────────────────────────────────┐
│              Python/compiler/ (编译器)                │
│  Parser → AST → Symbol Table → CFG → Bytecode       │
├──────────────────────────────────────────────────────┤
│              Python/ceval.c (VM 解释器)               │
│  _PyEval_EvalFrameDefault → opcode dispatch loop     │
├──────────────────────────────────────────────────────┤
│              Objects/ (内置类型)                      │
│  PyObject / PyLong / PyDict / PyList / PyUnicode     │
├──────────────────────────────────────────────────────┤
│              Modules/ (标准库 C 扩展)                 │
│  sys / io / json / re / math / _thread / ...         │
├──────────────────────────────────────────────────────┤
│              Include/ (C API 头文件)                  │
│  object.h / dictobject.h / longobject.h / ceval.h    │
└──────────────────────────────────────────────────────┘
```

**源码规模**: 2165 个 C/H 文件, 67590 个代码块
**关键目录**:
- `Python/` — 编译器 + VM（104 个 C 文件）
- `Objects/` — 内置对象实现（73 个 C 文件）
- `Modules/` — C 扩展模块（248 个 C 文件）
- `Include/` — 295 个头文件

---

## 二、核心数据结构

### 2.1 PyObject — 所有对象的基类

**文件**: `Include/object.h`

CPython 中所有 Python 对象都以 `PyObject` 开头。
`PyObject_HEAD` 宏展开为:

```c
// PyObject_HEAD 展开:
Py_ssize_t ob_refcnt;      // 引用计数
PyTypeObject *ob_type;     // 类型对象
```

### 2.2 PyLongObject — 整数

**文件**: `Objects/longobject.c`

```c
struct _longobject {
    PyObject_HEAD
    _PyLongValue long_value;  // 变长大整数
};
```

**搜索命中**: `_longobject` (struct, score=0.784)
- Python 的整数是变长大整数（类似 Chez Scheme 的 bignum）
- 小整数（-5 到 257）在 small_ints 缓存中预分配

### 2.3 PyDictObject — 字典

**文件**: `Objects/dictobject.c`

**搜索命中**: `PyDict_SetItem` (score=0.868)

```c
// dict 的核心插入操作
PyDict_SetItem(PyObject *op, PyObject *key, PyObject *value)
{
    assert(key);
    assert(value);
    if (!PyDict_Check(op)) {
        if (PyFrozenDict_Check(op)) {
            frozendict_does_not_support("assignment");
        }
        // ...
    }
}
```

**设计要点**:
- 使用 `PyDictKeysObject` 分离键和值（indices + entries 数组）
- 采用 combined table 和 split table 双重模式
- 保留插入顺序（Python 3.7+ 保证 dict 有序）

### 2.4 PyListObject — 列表

**文件**: `Objects/listobject.c`

```c
// 列表核心: PyObject* 指针数组
typedef struct {
    PyObject_VAR_HEAD
    PyObject **ob_item;    // 底层 C 数组
    Py_ssize_t allocated;  // 已分配容量
} PyListObject;
```

- 动态数组实现，当超出容量时 realloc（2 倍扩容）
- `list.append` 和 `list.pop` 均摊 O(1)

### 2.5 PyUnicodeObject — 字符串

**文件**: `Objects/unicodeobject.c`

Python 字符串使用多种内部表示（flexible string representation）:
- 1 字节（ASCII/Latin-1）
- 2 字节（UCS-2）
- 4 字节（UCS-4/UTF-32）

自动根据内容选择最紧凑的表示。

### 2.6 PyFrameObject — 栈帧

**文件**: `Include/internal/pycore_interpframe_structs.h`

**搜索命中**: `_PyInterpreterFrame` (struct)

```c
struct _PyInterpreterFrame {
    _PyStackRef f_executable;        // code object
    struct _PyInterpreterFrame *previous; // 上一帧（调用链）
    _PyStackRef f_funcobj;           // 函数对象
    PyObject *f_globals;             // 全局变量
    PyObject *f_builtins;            // 内置变量
    _Py_CODEUNIT *instr_ptr;         // 当前执行的指令
    _PyStackRef *stackpointer;       // 栈指针
    char owner;                      // 帧所有权
    _PyStackRef localsplus[1];       // 局部变量 + 栈（柔性数组）
};
```

---

## 三、编译器管线

### 3.1 编译流程

```
Python 源码
    ↓
Parser (语法分析)         → Python/grammar.c + Grammar/python.gram
    ↓
AST (抽象语法树)          → Python/ast.c + Include/Python-ast.h
    ↓
符号表 (Symbol Table)     → Python/symtable.c
    ↓
CFG (控制流图)           → Python/flowgraph.c
    ↓
Bytecode 生成            → Python/compile.c → code object
    ↓
字节码指令               → Lib/opcode.py + Include/internal/pycore_opcode.h
```

### 3.2 关键编译函数

| 函数 | 文件 | 功能 |
|------|------|------|
| `_PyAST_Compile` | compile.c | AST → code object（编译入口） |
| `_PySymtable_Build` | symtable.c | 符号表构建（作用域分析） |
| `_PyFlowGraph_FromAST` | flowgraph.c | AST → CFG（控制流图） |
| `makecode` | compile.c | CFG → code object |

### 3.3 字节码指令

Python 现在使用 `opcode.py` 作为单源真值，自动生成 `.h` 文件。

典型指令:
```
LOAD_CONST    → 加载常量
LOAD_FAST     → 加载局部变量
STORE_FAST    → 存储局部变量
LOAD_GLOBAL   → 加载全局变量
BINARY_OP     → 二元运算 (+ - * / 等)
CALL          → 函数调用
RETURN_VALUE  → 返回值
```

---

## 四、VM 执行引擎

### 4.1 执行入口

**搜索命中**: `_PyEval_EvalFrame` (score=0.863)

```c
_PyEval_EvalFrame(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag)
{
    EVAL_CALL_STAT_INC(EVAL_CALL_TOTAL);
    if (tstate->interp->eval_frame == NULL) {
        return _PyEval_EvalFrameDefault(tstate, frame, throwflag);
    }
    // 支持自定义 eval_frame hook（用于调试器、JIT 等）
    return tstate->interp->eval_frame(tstate, frame, throwflag);
}
```

### 4.2 主循环

VM 在 `Python/ceval.c` 的 `_PyEval_EvalFrameDefault` 中实现。
核心调度是 computed goto (GCC) 或 switch-case:

```c
// 简化版主循环
PyObject* _PyEval_EvalFrameDefault(PyThreadState *tstate, ...) {
    goto main_loop;
    
    main_loop:
        switch (opcode) {
            case TARGET(LOAD_CONST): {
                PyObject *value = GETITEM(consts, oparg);
                Py_INCREF(value);
                PUSH(value);
                goto main_loop;
            }
            case TARGET(BINARY_OP): {
                PyObject *rhs = PEEK(1);
                PyObject *lhs = PEEK(2);
                PyObject *res = PyNumber_Add(lhs, rhs);  // 或减/乘等
                // ...
                goto main_loop;
            }
            case TARGET(CALL): {
                // 函数调用: 创建新帧、递归 eval
                // ...
            }
            case TARGET(RETURN_VALUE): {
                // 返回: 弹出当前帧，继续上一帧
                // ...
            }
        }
}
```

### 4.3 函数调用机制

当 `CALL` 指令执行时:
1. 创建 `_PyInterpreterFrame`（在 C 栈上分配）
2. 设置 `previous` 指向当前帧
3. 递归调用 `_PyEval_EvalFrame`
4. 返回时从 `previous` 恢复

这与 PHP 的 `zend_execute_data` 链异曲同工。

---

## 五、内存管理

### 5.1 引用计数

```c
// Include/object.h
#define Py_INCREF(op) ((op)->ob_refcnt++)
#define Py_DECREF(op) if (--(op)->ob_refcnt == 0) _Py_Dealloc(op)
```

* 所有 `PyObject*` 操作都通过 INCREF/DECREF 管理
* 循环引用由 GC（`gcmodule.c`）处理

### 5.2 垃圾回收

```c
// Python 内置的 GC 是分代回收 + 引用计数混合
// gc.collect() 触发循环引用检测
```

---

## 六、内置对象操作

### 6.1 对象初始化

**搜索命中**: `PyObject_Init` (score=0.847)

```c
PyObject_Init(PyObject *op, PyTypeObject *tp)
{
    if (op == NULL) return PyErr_NoMemory();
    _PyObject_Init(op, tp);
    return op;
}
```

### 6.2 Fork 处理

**搜索命中**: `PyOS_BeforeFork` (score=0.828)

```c
PyOS_BeforeFork(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    run_at_forkers(interp->before_forkers, 1);
    _PyImport_AcquireLock(interp);
    _PyEval_StopTheWorldAll(&_PyRuntime);
    HEAD_LOCK(&_PyRuntime);
}
```

---

## 七、CPython vs PHP 架构对比

| 层次 | CPython | PHP (Zend) |
|------|---------|------------|
| **值类型** | `PyObject*` (指针) | `zval` (16 字节内联) |
| **整数** | `PyLongObject` (变长) | `zend_long` (64 位) |
| **字符串** | `PyUnicodeObject` (1/2/4 字节) | `zend_string` (len+hash+val) |
| **哈希表** | `PyDictObject` (indices+entries) | `HashTable` (arData+arHash) |
| **列表** | `PyListObject` (PyObject* 数组) | `HashTable.packed` (zval 数组) |
| **引用计数** | `ob_refcnt` | `zend_refcounted_h.gc.refcount` |
| **GC** | 分代回收 (gcmodule.c) | 纯 refcount（无循环检测） |
| **VM 分派** | computed goto / switch | `goto *label` (标签指针) |
| **编译产物** | code object (bytecode) | zend_op_array (opcode) |
| **执行入口** | `_PyEval_EvalFrame` | `zend_execute` |
| **函数调用栈** | `_PyInterpreterFrame` 链 | `zend_execute_data` 链 |

---

## 八、搜索系统快速参考

```bash
# 获取 struct 定义
cache_query "_longobject" --repo /code/python --type context
cache_query "_PyInterpreterFrame" --repo /code/python --type context
cache_query "_frame" --repo /code/python --type context

# 获取函数源码
cache_query "PyDict_SetItem" --repo /code/python --type context
cache_query "PyObject_Init" --repo /code/python --type context
cache_query "_PyEval_EvalFrame" --repo /code/python --type context
cache_query "PyOS_BeforeFork" --repo /code/python --type context

# 语义搜索（需要等向量命名空间修复后可用）
cache_query "Python dictionary implementation" --repo /code/python --type search
```

---

> *基于 `/opt/cpython/src/` 源码, 使用 `cache_query --type context` 获取所有 struct 和函数源码*
> *向量搜索暂不可用（`code_local_cpython.jina.bin` 命名空间需对齐到 `/code/python`)*
