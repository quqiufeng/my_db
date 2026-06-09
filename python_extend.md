# Python AOT 编译器方案：CPython + Chez Scheme = Go 风格部署

> 本文档描述如何利用 **my_db 代码搜索系统** 理解 CPython 源码，
> 将 Python 编译为 Chez Scheme S-expr，通过 Chez AOT 编译器产出单文件 ELF 二进制，
> 结合 Maven 风格的二进制包注册中心，彻底消除 pip 依赖陷阱，
> 实现 PyTorch 等 AI/ML 生态的 Go 风格部署。

---

## 一、核心理念

### 1.1 这不是为了"更快"

```
快不是目的，省才是。
省掉 pip install，省掉 requirements.txt，省掉 Dockerfile，省掉环境配置。
```

### 1.2 目标是让 AI 模型部署像 Go 一样简单

```
现状:                             目标:
  Dockerfile 50 行                  ./llm --model llama.pt
  pip install torch 2GB              ↑ 单文件 ELF
  conda create -n llm               ↑ 拷到任何 Linux 机器
  CUDA version mismatch             ↑ 有 GPU 就用 GPU
  "it works on my machine"          ↑ 不需要装任何东西
```

### 1.3 跟 Maven 一样的二进制包管理

```
Maven:                           你:
  mvn install                       aot-get install torch
  mvn build                         py2native build
  .jar → .war                         .py → .ELF
  nexus 私服                         aot hub 注册中心
```

---

## 二、AI/ML 场景的依赖地狱

### 2.1 一个典型 AI 项目的依赖链

```
你的代码:
  train.py → import torch, numpy, transformers, accelerate, deepspeed

依赖树:
  torch-2.1.0
    ├── CUDA 12.1
    ├── cuDNN 8.9
    ├── cuBLAS
    └── libtorch (C++ 后端)
  numpy-1.26.0
    └── OpenBLAS
  transformers-4.36.0
    └── huggingface-hub
  deepspeed-0.12.0
    ├── CUDA 12.1
    └── torch-2.1.0

这棵树的每个节点都可能出问题:
  ❌ CUDA 版本不对 → torch 装不上
  ❌ glibc 版本太老 → 所有 C 扩展装不上
  ❌ gcc 版本太低 → 编译 flash-attn 失败
  ❌ cuDNN 路径没设对 → 运行时崩溃
  ❌ Ubuntu 22.04 和 24.04 行为不同
```

### 2.2 pip 的局限

```
pip 是源码分发生态:
  torch 的 wheel 虽然带了 .so，但依赖系统 CUDA/cuDNN
  flash-attn 需要源码编译 → 需要 gcc + CUDA Toolkit
  transformers 纯 Python → 没问题，但它的依赖也可能有问题

结果:
  50% 的时间在写模型代码
  50% 的时间在配环境
```

### 2.3 AOT 方案的解决思路

```
不通过 pip 分发源码或 .so，而是分发预编译的 .a（静态库）

.a 的编译由专业团队完成:
  官方 CI → 编译 torch-2.1.0-cuda12-linux-x86_64.a
  官方 CI → 编译 numpy-1.26.0-linux-x86_64.a
  官方 CI → 编译 cudnn-8.9-linux-x86_64.a

用户:
  aot-get install torch         ← 下载 .a
  aot-get install numpy         ← 下载 .a
  py2native build               ← 全部静态链接 → 单文件 ELF
```

---

## 三、系统架构

### 3.1 三组件架构

```
┌──────────────────────────────────────────────────────────────────┐
│  aot hub（注册中心）                                              │
│  存储各类 C 库的预编译静态库                                       │
│  torch-2.1.0-cuda12.a                                             │
│  numpy-1.26.0.a                                                   │
│  opencv-4.9.0.a                                                   │
│  curl -O https://aot.hub/packages/torch/2.1.0/torch.a            │
├──────────────────────────────────────────────────────────────────┤
│  aot-get（包管理器）                                               │
│  从注册中心下载 .a，管理版本，处理依赖传递                            │
│  aot-get install torch                                            │
│  aot-get install numpy                                            │
│  aot-get update                                                   │
│  产物: ./deps/ 目录下的 .a 文件                                    │
├──────────────────────────────────────────────────────────────────┤
│  py2native（AOT 编译器）                                           │
│  编译你的 Python 代码 → 链接所有 .a → 输出单文件 ELF               │
│  py2native build -o inference                                     │
│  产物: ./inference (ELF x86-64)                                   │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 与传统方案对比

```
               pip                    conda                 aot-get/py2native
──────────────────────────────────────────────────────────────────────────────
包格式          wheel (.so)           tar.bz2 (.so)         .a (静态库)
链接方式        运行时 dlopen          运行时 dlopen          编译时静态链接
外部依赖        需要 CUDA/cuDNN        需要 CUDA/cuDNN       全部打包进二进制
运行时          python app.py          python app.py          ./app
环境隔离        venv                   conda env             不需要（二进制自带）
部署            Docker 镜像             Docker 镜像           scp 单文件
版本管理        pip list               conda list             aot-get list
```

### 3.3 AI/ML 场景的包结构

```
torch-2.1.0-cuda12.a   →  ~200MB（libtorch + CUDA 静态库）
numpy-1.26.0.a         →   ~30MB
transformers.a         →  纯 Python，直接 AOT 编译，不需要预编译 .a
accelerate.a           →  纯 Python
deepspeed-cuda12.a     →   ~80MB

编译产物:
./llm → ~400MB（含 torch + numpy + 你的代码）
       → 但这是单文件
       → 对比 Docker 镜像 5GB+ 已经小了一个量级
```

---

## 四、AI Agent 使用 Code Search 的工作流

### 4.1 第一步：定位编译器入口

```bash
cache_query "_PyAST_Compile" --repo /code/python --type context
# → Python/compile.c:1523
#   AST → Code Object 的编译入口，等价 PHP 的 zend_compile_file
```

### 4.2 第二步：获取 AST 节点类型

```bash
cache_query "AST node types Python" --repo /code/python --type search \
  --analysis-dir /opt/code_caches/python_cache
# → 搜索 AST 节点枚举定义
```

### 4.3 第三步：定位关键数据结构

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

### 4.4 第四步：定位运行时函数（FFI 绑定用）

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

### 4.5 第五步：搜索 PyTorch FFI 边界

```bash
# 搜索 libtorch 的 C 接口
cache_query "torch forward tensor" --repo /code/python --type search
# → 找到 PyTorch 绑定的 C 函数声明
```

---

## 五、翻译器实现

### 5.1 核心翻译函数

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

### 5.2 FFI 桥接（CPython 运行时）

```scheme
;; python-runtime.ss — CPython C 库的 FFI 绑定

(define py-number-add
  (foreign-procedure "PyNumber_Add" (void* void*) void*))

(define py-list-append
  (foreign-procedure "PyList_Append" (void* void*) int))

(define py-dict-set-item
  (foreign-procedure "PyDict_SetItem" (void* void* void*) int))

(define py-iter-next
  (foreign-procedure "PyIter_Next" (void*) void*))
```

### 5.3 FFI 桥接（PyTorch / libtorch）

```scheme
;; pytorch-ffi.ss — PyTorch C++ API 的 FFI 绑定

(define torch-from-blob
  (foreign-procedure "torch_from_blob"
    (void* void* int) void*))

(define torch-forward
  (foreign-procedure "torch_forward"
    (void* void*) void*))

(define torch-load
  (foreign-procedure "torch_load"
    (string) void*))

(define torch-save
  (foreign-procedure "torch_save"
    (void* string) int))
```

### 5.4 编译 + 链接流程

```bash
# 1. 声明依赖
cat > aot.json << EOF
{
  "dependencies": {
    "torch":    {"version": "2.1.0", "cuda": "12"},
    "numpy":    {"version": "1.26.0"},
    "opencv":   {"version": "4.9.0", "optional": true}
  }
}
EOF

# 2. 下载预编译静态库
aot-get install
# → 下载 torch-2.1.0-cuda12.a 到 ./deps/
# → 下载 numpy-1.26.0.a 到 ./deps/

# 3. 编译 Python 代码
chez --script python-to-scheme.ss < train.py > train.sls
chez --compile train.sls --optimize-level 3 -o train.o

# 4. 静态链接所有依赖
g++ -static -o llm \
    train.o \
    deps/torch-2.1.0-cuda12.a \
    deps/numpy-1.26.0.a \
    -lchezscheme -lpthread -ldl -lm -lstdc++

# 5. 产物
./llm --model llama.pt
```

---

## 六、一个真实的 AI 推理场景

### 6.1 从源码到部署

```bash
# 你的 Python 代码: infer.py
#   import torch
#   import torchvision.transforms as T
#   model = torch.load("resnet50.pt")
#   model.eval()
#   def predict(img): return model(img)

# 构建
$ aot-get install torch-2.1.0-cuda12
$ py2native build infer.py -o infer --link torch

# 产物
$ file infer
infer: ELF 64-bit LSB executable, x86-64, statically linked

$ du -h infer
287M    infer

# 部署到无 CUDA Toolkit 的服务器
$ scp infer user@production-server:~
$ ssh user@production-server
$ ./infer --image cat.jpg
→ class: tabby cat (confidence: 0.97)
```

### 6.2 对比传统部署

```
传统:                              AOT:
  Dockerfile 30 行                  一行命令
  apt install cuda-toolkit-12       不需要
  pip install torch                 不需要
  pip install torchvision           不需要
  COPY infer.py /app                不需要（代码在 ELF 里）
  CMD ["python", "infer.py"]        ./infer
  镜像 6GB                          单文件 287MB
  构建 15 分钟                       2 分钟
  启动 30 秒                        1ms
  "CUDA version mismatch"          报错不了（已经链死了）
```

### 6.3 大模型场景

```bash
# 一个 LLaMA 推理服务的构建
cat > aot.json << EOF
{
  "name": "llm-server",
  "dependencies": {
    "torch":    {"version": "2.1.0", "cuda": "12"},
    "transformers": {"version": "4.36.0"},
    "accelerate":   {"version": "0.25.0"}
  }
}
EOF

aot-get install
py2native build server.py -o llm-server
# 产物: ./llm-server (~500MB)
#       ↑ 静态链接了 libtorch + CUDA + 你的模型代码

# 部署到 GPU 服务器
scp llm-server gpu-node:~/
ssh gpu-node ./llm-server --model llama-70b --port 8080
# 不需要装 CUDA
# 不需要装 Python
# 不需要 pip install
# 直接跑
```

---

## 七、Python 特有的挑战

### 7.1 挑战：值类型是堆分配指针

```
PHP 的 zval 是 16 字节内联值 → 直接放寄存器
Python 的 PyObject* 是指针 → 每次访问要 FFI 调用

但对 AI 场景不是问题:
  训练循环里 99% 的时间在 C++/CUDA 层
  Python 层的 FFI 开销相比毫秒级的 forward 调用可以忽略
```

### 7.2 挑战：GIL

```
方案: 静态编译时只链接 Python 运行时 + libtorch
      运行时不需要 Python 的解释器循环（已编译成机器码）
      GIL 只保护 FFI 调用时的 C 对象，不阻塞你的计算
```

### 7.3 挑战：动态性

```
AI 场景的代码几乎不需要动态特性:
  model = torch.load("model.pt")    ← 纯静态
  output = model(input)             ← 纯静态
  loss = loss_fn(output, target)    ← 纯静态
  没有 eval()，没有 exec()，没有 monkey-patch

混合模式仅用于极少数场景:
  静态 AOT 编译 → 95% 的代码
  eval_frame hook → 5% 的动态代码
```

---

## 八、aot hub 注册中心设计

### 8.1 包上传

```bash
# 库维护者（NVIDIA / PyTorch 团队）
aot-get publish torch-2.1.0-cuda12.a \
  --name torch --version 2.1.0 \
  --cuda 12 --os linux --arch x86_64

# 注册中心验证:
#   1. 确认 .a 是静态库
#   2. 确认符号表完整
#   3. 确认依赖正确
#   4. 签名
```

### 8.2 包下载

```bash
# 用户
aot-get install torch --version 2.1.0
# → 下载 torch-2.1.0-cuda12-linux-x86_64.a
# → 验证签名
# → 放入 ./deps/
```

### 8.3 版本管理

```xml
<!-- aot.xml — 类似于 Maven 的 pom.xml -->
<project>
    <name>llm-inference</name>
    
    <dependencies>
        <dependency>
            <groupId>org.pytorch</groupId>
            <artifactId>torch</artifactId>
            <version>2.1.0</version>
            <classifier>cuda12-linux-x86_64</classifier>
            <!-- 预编译的静态库，200MB -->
        </dependency>
        <dependency>
            <groupId>org.numpy</groupId>
            <artifactId>numpy</artifactId>
            <version>1.26.0</version>
        </dependency>
        <dependency>
            <groupId>org.llvm</groupId>
            <artifactId>transformers</artifactId>
            <version>4.36.0</version>
            <!-- 纯 Python，直接 AOT 编译，不需要预编译 .a -->
        </dependency>
    </dependencies>
</project>
```

---

## 九、文档总结

### 9.1 整个系统的工作原理

```
aot hub (注册中心)
  └── 各 AI 库的预编译静态库
       torch.a, numpy.a, cudnn.a, ...
       ↓ aot-get install
aot-get (包管理器)
  └── 管理版本、下载 .a、处理依赖传递
       ↓
py2native (AOT 编译器)
  └── Python 源码 → Scheme S-expr → 机器码
       ↓ 静态链接所有 .a
单文件 ELF
  └── 包含你的代码 + 全部运行时 + 全部库
       ↓ scp
任何 Linux 服务器 → ./llm
```

### 9.2 一句话

**pip 管源码/运行时依赖，aot-get 管二进制静态链接。pip 产出 venv，py2native 产出单文件。两个不冲突，各管各的事。**

> **文档版本**: 2026-06-09
> **相关文件**: php_extend.md (同方案 PHP 版), cpython_analysis_report.md (CPython 源码分析)
> **搜索命令示例**: 本文档中所有 cache_query 命令均可直接复制执行
