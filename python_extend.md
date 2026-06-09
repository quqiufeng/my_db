# Python AOT 编译器方案：CPython + Chez Scheme = Go 风格部署

> 本文档描述如何利用 **my_db 代码搜索系统** 理解 CPython 源码，
> 将 Python 编译为 Chez Scheme S-expr，通过 Chez AOT 编译器产出单文件 ELF 二进制，
> 彻底消除 pip 依赖陷阱，实现 PyTorch/numpy 等生态的 Go 风格部署。

---

## 一、核心理念

### 1.1 这个方案不是为了"更快"

```
快不是目的，省才是。
省掉 pip install，省掉 requirements.txt，省掉 Dockerfile，省掉环境配置。

你用 Python 写 PyTorch 模型，90% 的计算量在 C++/CUDA 层。
Python 本身只是胶水——但就是这层胶水，带来了整个 pip 依赖地狱。
```

### 1.2 目标

```
传统部署:                          AOT 部署:
  pip install torch                ./py2native train.py -o train
  pip install numpy                scp train server:~/
  pip install scipy                ssh server ./train
  pip install -r requirements.txt  ↑ 完事了
  Dockerfile 5GB+
  构建 10 分钟
  "it works on my machine"

AOT 方案 = Python 代码 → 单文件 ELF 二进制
          运行时通过 FFI 静态链接 libtorch/numpy/CUDA
          零依赖，零配置，零环境问题
```

### 1.3 和 PHP 方案完全一样

```
PHP → Chez AOT:               Python → Chez AOT:
  Zend/ (zval/HashTable)         Objects/ (PyDict/PyList/PyUnicode)
  ext/ (标准函数)                 Modules/ (标准模块 + PyTorch/numpy)
  FFI 直接调 C 运行时             FFI 直接调 C++ 运行时
  翻译器只处理控制流              翻译器只处理控制流

没有本质区别。换一套 C 库的名字而已。
```

---

## 二、架构

### 2.1 整体链路

```
Python 源码
  ↓
_pypaser (CPython 的 Parser)    → Python/peg_api.c
  ↓
AST                             → Python/ast.c
  ↓
你的翻译器 (AST → Scheme S-expr)  ← 你写 ~2500 行
  ↓
Scheme S-expr
  ↓
Chez AOT compile-file           ← Chez 45 年打磨的后端
  ↓
ELF 二进制
  + 静态链接 libpython 运行时
  + 静态链接 libtorch / numpy
  + 静态链接 CUDA runtime
  ↓
./app     ← 单文件，零依赖
```

### 2.2 和 PHP 的关键对照

```
层次               PHP                          CPython
──────────────────────────────────────────────────────────────────────
Parser             zend_language_parser.y       PEG parser (Python/pegen.c)
AST                _zend_ast                    AST 节点 (Python/ast.c)
编译器             zend_compile.c               _PyAST_Compile (score=0.896)
值类型             zval (16 字节内联)            PyObject* (堆分配指针)
整数               zend_long (64 位)             PyLongObject (变长大整数)
字符串             zend_string                   PyUnicodeObject (1/2/4 字节)
哈希表             HashTable                     PyDictObject
VM 分派            goto *label                   computed goto
函数调用栈         zend_execute_data 链           _PyInterpreterFrame 链
                   
差异点:
GC                纯 refcount                    refcount + 分代回收
GIL               无                             有 (但 free-threaded 解决中)
异常处理          zend_exception                 C longjmp + PyErr_Set*
Generator         zend_generator                 yield + async/await
```

---

## 三、AI Agent 使用 Code Search 的工作流

### 3.1 第一步：定位编译器入口

```bash
cache_query "_PyAST_Compile" --repo /code/python --type context
# → Python/compile.c:1523
#   AST → Code Object 的编译入口，等价 PHP 的 zend_compile_file
```

### 3.2 第二步：获取 AST 节点类型

```bash
cache_query "AST node types Python" --repo /code/python --type search \
  --analysis-dir /opt/code_caches/python_cache
# → 搜索 AST 节点枚举定义
```

### 3.3 第三步：定位关键数据结构

```bash
# 值系统
cache_query "_longobject" --repo /code/python --type context
cache_query "_PyUnicodeObject" --repo /code/python --type context

# 哈希表 (Python 的内置 dict)
cache_query "PyDict_SetItem" --repo /code/python --type context

# VM 执行入口
cache_query "_PyEval_EvalFrame" --repo /code/python --type context

# 栈帧结构
cache_query "_PyInterpreterFrame" --repo /code/python --type context
```

### 3.4 第四步：定位运行时函数（FFI 绑定用）

```bash
# 对象操作
cache_query "PyObject_Init" --repo /code/python --type context
cache_query "PyNumber_Add" --repo /code/python --type context

# 列表操作
cache_query "PyList_Append" --repo /code/python --type context

# 迭代器协议
cache_query "PyIter_Next" --repo /code/python --type context

# 异常处理
cache_query "PyErr_SetString" --repo /code/python --type context
```

### 3.5 第五步：搜索 PyTorch FFI 边界

```bash
# 搜索 libtorch 的 C 接口
cache_query "torch forward tensor" --repo /code/python --type search
# → 找到 PyTorch 绑定的 C 函数声明

# 定位 C++ → C 包装层
cache_query "extern torch Tensor" --repo /code/python --type search
```

---

## 四、翻译器实现

### 4.1 核心翻译函数

```scheme
;; python->scheme.ss — Python AST → Scheme S-expr 翻译器

(define (compile-stmt ast)
  (match ast
    ;; Python 的 if 语句
    ((If test body orelse)
     `(if ,(compile-expr test)
          ,(compile-block body)
          ,(compile-block orelse)))
    
    ;; Python 的 for 循环
    ((For target iter body . _)
     ;; Python: for x in iter: body
     ;; Scheme: (py-for-each (lambda (x) body) iter)
     `(,(ffi "PyIter_ForEach")
        (lambda (,(compile-expr target))
          ,(compile-block body))
        ,(compile-expr iter)))
    
    ;; 函数定义
    ((FunctionDef name args body . _)
     `(define (,name ,@(compile-args args))
        ,@(compile-block body)))
    
    ;; 函数调用 (包括 PyTorch 的 tensor 操作)
    ((Call func args)
     `(,(compile-expr func)
       ,@(map compile-expr args)))))
```

### 4.2 FFI 桥接（CPython 运行时）

```scheme
;; python-runtime.ss — CPython C 库的 FFI 绑定

;; 对象系统
(define py-incref
  (foreign-procedure "_Py_IncRef" (void*) void*))

(define py-decref
  (foreign-procedure "_Py_DecRef" (void*) void*))

;; 数值运算
(define py-number-add
  (foreign-procedure "PyNumber_Add" (void* void*) void*))

(define py-number-subtract
  (foreign-procedure "PyNumber_Subtract" (void* void*) void*))

;; 列表操作
(define py-list-append
  (foreign-procedure "PyList_Append" (void* void*) int))

;; 字典操作
(define py-dict-set-item
  (foreign-procedure "PyDict_SetItem" (void* void* void*) int))

;; 迭代器协议
(define py-iter-next
  (foreign-procedure "PyIter_Next" (void*) void*))

;; 字符串操作
(define py-unicode-from-string
  (foreign-procedure "PyUnicode_FromString" (string) void*))
```

### 4.3 FFI 桥接（PyTorch / libtorch）

```scheme
;; pytorch-ffi.ss — PyTorch C++ API 的 FFI 绑定
;; 通过 extern "C" 包装后的接口

(define torch-from-blob
  (foreign-procedure "torch_from_blob"
    (void* void* int) void*))     ;; float* data, int64* shape, int ndim

(define torch-forward
  (foreign-procedure "torch_forward"
    (void* void*) void*))          ;; Module* model, Tensor* input

(define torch-save
  (foreign-procedure "torch_save"
    (void* string) int))           ;; Tensor t, string path

(define torch-load
  (foreign-procedure "torch_load"
    (string) void*))               ;; string path → Tensor*
```

---

## 五、Python 特有的挑战与解决方案

### 5.1 挑战：值类型是堆分配指针

```
PHP:  zval 是 16 字节值，可直接嵌入栈帧
      Chez: (let ((a 42)) ...)  ← 值在寄存器里

Python: PyObject* 是指针，所有操作都要解引用
        Chez: (let ((a (py-long-from-int 42))) ...)
              每次访问 a 都要 FFI 调用
              开销比 PHP 大，但 Python 本来就不追求极致性能
              
        且 PyTorch 场景下，主要计算在 C++ 层，
        Python 的胶水代码开销相比毫秒级的 tensor 操作可以忽略。
```

### 5.2 挑战：GIL

```
PHP: 没有 GIL，多线程直接翻译到 Scheme native threads
Python: 有 GIL——

  方案 A: 启用 CPython 的 free-threaded 构建
          (Py_GIL_DISABLED，Python 3.13+，实验性)
          所有 C 扩展需要线程安全适配

  方案 B: 只编译单线程代码 + FFI 调用时获取 GIL
          (foreign-procedure 内部自动 PyGILState_Ensure)
          兼容现有 C 扩展，但失去并行性

  推荐方案 B —— PyTorch 场景下，计算密集型代码在 C++ 层，
  Python 层的 GIL 争用不是瓶颈。
```

### 5.3 挑战：Generator / async

```scheme
;; Python:  def gen(): yield 1; yield 2
;;
;; 翻译到 Scheme 的 continuation:
(define (gen)
  (lambda (c)
    (c 1)   ;; yield 1
    (c 2))) ;; yield 2

;; 调用:
(define g (gen))
(g (lambda (val) (display val)))  ;; 每次 yield 调这个回调

;; async/await 同理——coroutine = 带状态的 continuation
```

### 5.4 挑战：极端动态性

```
class Foo:
    pass

Foo.x = 42           # 运行时加属性
Foo.__class__ = ...   # 运行时改元类

解决方案: 混合模式
- 静态可分析的部分 → Chez AOT 编译
- 动态 eval/exec/monkey-patch → CPython 的 eval_frame hook 回调

实际上 PyTorch 场景不需要动态特性：
  model = torch.load("model.pt")  ← 纯静态
  output = model(input)           ← 纯静态
  loss = loss_fn(output, target)  ← 纯静态
  不需要 eval()，不需要 monkey-patch。
```

---

## 六、部署场景

### 6.1 PyTorch 推理服务

```bash
$ cat inference.py
import torch
import torchvision.transforms as T

model = torch.load("resnet50.pt")
model.eval()

def predict(image_path):
    img = T.ToTensor()(PIL.Image.open(image_path))
    with torch.no_grad():
        return model(img.unsqueeze(0))

# 使用: ./inference server --port 8080

$ py2native inference.py -o inference --link-libtorch
$ ./inference server --port 8080
  → 单文件二进制
  → 不需要 Python / torch 环境
  → scp 到任何 Linux 服务器直接跑
```

### 6.2 产物对比

```
传统方式:                          AOT 方式:
/inference_env/                   /inference
  python3.11                        ↑ 单文件
  lib/python3.11/site-packages/      ELF x86-64
    torch/
    torchvision/
    numpy/
    PIL/
    ...
  2000+ 文件
  ~5GB
```

### 6.3 静态链接方式

```bash
# 编译方案代码到 .so
chez --compile inference.sls --optimize-level 3 --output inference.so

# 静态链接 libtorch + CUDA + numpy + Python 运行时
g++ -static -o inference \
    inference.so \
    /opt/cpython/lib/libpython3.a \
    /opt/libtorch/lib/libtorch.a \
    /opt/libtorch/lib/libc10.a \
    /opt/cuda/lib64/libcudart.a \
    -lchezscheme -lpthread -ldl -lm -lstdc++

# 产出 ≈ 50-200MB (主要是 libtorch)
# 但这是单文件——scp 到任何 Linux 就能跑
```

---

## 七、搜索系统参考

```bash
# 获取 AST 编译入口
cache_query "_PyAST_Compile" --repo /code/python --type context

# 获取对象结构
cache_query "_longobject" --repo /code/python --type context
cache_query "_PyInterpreterFrame" --repo /code/python --type context

# 获取运行时函数签名
cache_query "PyDict_SetItem" --repo /code/python --type context
cache_query "PyList_Append" --repo /code/python --type context
cache_query "PyIter_Next" --repo /code/python --type context

# 验证调用链
cache_query "PyDict_SetItem" --repo /code/python --type context --depth 1

# 跨项目搜索
cross_search.sh "tensor operation" 5 python php
```

---

## 八、总结

和 PHP 完全一样。换一套 C 库的名字而已。

```
PHP:  ext/ + Zend/    → FFI → Chez AOT → ELF → ./app
Python: Modules/ + Objects/ + libtorch → FFI → Chez AOT → ELF → ./inference
                           ↑
                    多了一个 PyTorch，但思想完全一样
```

**搜索系统 + Chez Scheme + C 运行时 = 任何 C 实现的脚本语言的 Go 风格部署。**

> **文档版本**: 2026-06-09
> **相关文件**: php_extend.md (同方案 PHP 版), cpython_analysis_report.md (CPython 源码分析)
> **搜索命令示例**: 本文档中所有 cache_query 命令均可直接复制执行
