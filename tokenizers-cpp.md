# tokenizers-cpp 集成指南

## 概述

本项目使用 **tokenizers-cpp**（HuggingFace 官方 C++ tokenizer 绑定）替代手写 BPE tokenizer，实现 CodeBERT 等模型的纯 C 推理。

**优势**：
- 支持所有 HuggingFace 模型（BPE、WordPiece、SentencePiece、Unigram）
- 无需手写 JSON 解析器或 BPE 合并规则
- 与 Python `transformers` 库 100% 兼容
- 零 Python 依赖，适合生产部署

## 依赖说明

本项目使用 ONNX Runtime 进行 GPU 推理加速，依赖以下外部库：

| 库 | 版本 | 用途 | 大小 |
|---|---|---|---|
| **tokenizers-cpp** | 0.1.0 | HuggingFace Tokenizer C++绑定 | ~38MB |
| cuDNN | 9.22.0 | NVIDIA 深度神经网络库 | ~2.2GB |
| TensorRT | 10.16.1 | NVIDIA 推理优化器 | ~910MB |
| ONNX Runtime | 1.14.1 (GPU) | 推理运行时 | ~480MB |

> **tokenizers-cpp** 是必需依赖，用于替代手写 BPE tokenizer，支持所有 HuggingFace 模型（BPE、WordPiece、SentencePiece、Unigram）。
> 官方仓库：https://github.com/mlc-ai/tokenizers-cpp

## 构建 tokenizers-cpp

tokenizers-cpp 需要从源码构建（Rust + C++）。

### 环境要求

- **Rust**: `rustc >= 1.70`，`cargo` 可用
- **CMake**: `>= 3.16`
- **GCC/G++**: `>= 9.0`（支持 C++17）
- **Git**: 用于克隆子模块

### 一键构建脚本

```bash
#!/bin/bash
set -e

PROJECT_DIR="/home/dministrator/my_db"
TOKENIZERS_DIR="$PROJECT_DIR/third_party/tokenizers-cpp"

echo "=== 构建 tokenizers-cpp ==="

# 1. 克隆仓库
mkdir -p "$TOKENIZERS_DIR"
cd "$TOKENIZERS_DIR"

if [ ! -d "tokenizers-cpp" ]; then
    git clone --depth 1 https://github.com/mlc-ai/tokenizers-cpp.git
    cd tokenizers-cpp
    # 初始化子模块（msgpack + sentencepiece）
    git submodule update --init --recursive
else
    cd tokenizers-cpp
fi

# 2. 创建构建目录
mkdir -p build
cd build

# 3. CMake 构建
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo "✓ tokenizers-cpp 构建完成"

# 4. 复制库文件到项目目录
cd "$PROJECT_DIR"
mkdir -p lib/tokenizers-cpp
mkdir -p include/tokenizers-cpp

cp "$TOKENIZERS_DIR/tokenizers-cpp/build/libtokenizers_c.a" lib/tokenizers-cpp/
cp "$TOKENIZERS_DIR/tokenizers-cpp/build/libtokenizers_cpp.a" lib/tokenizers-cpp/
cp "$TOKENIZERS_DIR/tokenizers-cpp/build/release/build/onig_sys-*/out/libonig.a" lib/tokenizers-cpp/
cp "$TOKENIZERS_DIR/tokenizers-cpp/build/sentencepiece/src/libsentencepiece.a" lib/tokenizers-cpp/

# 复制头文件
cp "$TOKENIZERS_DIR/tokenizers-cpp/include/tokenizers_c.h" include/tokenizers-cpp/
cp "$TOKENIZERS_DIR/tokenizers-cpp/include/tokenizers_cpp.h" include/tokenizers-cpp/

echo "✓ tokenizers-cpp 已安装到项目目录"
ls -lh lib/tokenizers-cpp/
```

### 手动构建步骤

```bash
# 1. 克隆仓库
git clone --depth 1 https://github.com/mlc-ai/tokenizers-cpp.git
cd tokenizers-cpp

# 2. 初始化子模块
git submodule update --init --recursive

# 3. 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 4. 复制产物
cp build/libtokenizers_c.a /path/to/project/lib/tokenizers-cpp/
cp build/libtokenizers_cpp.a /path/to/project/lib/tokenizers-cpp/
cp build/release/build/onig_sys-*/out/libonig.a /path/to/project/lib/tokenizers-cpp/
cp include/tokenizers_c.h /path/to/project/include/tokenizers-cpp/
```

### 构建产物说明

| 文件 | 大小 | 说明 |
|---|---|---|
| `libtokenizers_c.a` | ~35MB | Rust tokenizer 核心（静态库） |
| `libtokenizers_cpp.a` | ~166KB | C++ 包装层（静态库） |
| `libonig.a` | ~1.1MB | 正则表达式引擎（静态库） |
| `libsentencepiece.a` | ~2.2MB | SentencePiece tokenizer（静态库） |

## 快速部署（推荐）

将 **所有** 依赖库复制到项目根目录，无需设置 `LD_LIBRARY_PATH`：

```bash
# 进入项目目录
cd /home/dministrator/my_db

# 1. 确保 tokenizers-cpp 已构建（如果尚未构建）
if [ ! -f "lib/tokenizers-cpp/libtokenizers_c.a" ]; then
    echo "请先构建 tokenizers-cpp（见上文构建说明）"
    exit 1
fi

# 2. 删除旧版 cuDNN（如果存在）
rm -f libcudnn*.so*

# 3. 复制完整的 cuDNN 9.22.0（包含所有子库）
CUDNN_SRC=/opt/cudnn-linux-x86_64-9.22.0.52_cuda12/lib

# 主库
cp $CUDNN_SRC/libcudnn.so.9.22.0 .
ln -sf libcudnn.so.9.22.0 libcudnn.so.9
ln -sf libcudnn.so.9 libcudnn.so

# 子库（cuDNN 9 采用模块化设计，必须全部复制）
cp $CUDNN_SRC/libcudnn_ops.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_cnn.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_adv.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_engines_precompiled.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_engines_runtime_compiled.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_engines_tensor_ir.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_ext.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_graph.so.9.22.0 .
cp $CUDNN_SRC/libcudnn_heuristic.so.9.22.0 .

# 创建子库符号链接
for lib in libcudnn_ops libcudnn_cnn libcudnn_adv libcudnn_engines_precompiled \
           libcudnn_engines_runtime_compiled libcudnn_engines_tensor_ir \
           libcudnn_ext libcudnn_graph libcudnn_heuristic; do
  ln -sf ${lib}.so.9.22.0 ${lib}.so.9
done

# 3. 复制 TensorRT 10 库（如尚未复制）
TRT_SRC=/opt/TensorRT-10/lib

cp $TRT_SRC/libnvinfer.so.10.16.1 .
ln -sf libnvinfer.so.10.16.1 libnvinfer.so.10
ln -sf libnvinfer.so.10 libnvinfer.so

cp $TRT_SRC/libnvonnxparser.so.10.16.1 .
ln -sf libnvonnxparser.so.10.16.1 libnvonnxparser.so.10
ln -sf libnvonnxparser.so.10 libnvonnxparser.so

# TensorRT builder资源（SM86 = RTX 3080/3090/A4000）
cp $TRT_SRC/libnvinfer_builder_resource_sm86.so.10.16.1 .
ln -sf libnvinfer_builder_resource_sm86.so.10.16.1 libnvinfer_builder_resource_sm86.so.10

# 4. 验证所有依赖已就绪
ldd libonnxruntime_providers_tensorrt.so | grep "not found" || echo "✓ TensorRT provider OK"
ldd libonnxruntime_providers_cuda.so | grep "not found" || echo "✓ CUDA provider OK"
```

## 完整复制脚本

一键执行：

```bash
#!/bin/bash
set -e

PROJECT_DIR="/home/dministrator/my_db"
CUDNN_SRC="/opt/cudnn-linux-x86_64-9.22.0.52_cuda12/lib"
TRT_SRC="/opt/TensorRT-10/lib"

cd "$PROJECT_DIR"

echo "=== 部署 GPU 依赖库到项目目录 ==="

# cuDNN 9.22.0 - 必须复制所有子库
rm -f libcudnn*.so*

cp "$CUDNN_SRC"/libcudnn.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_ops.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_cnn.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_adv.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_engines_precompiled.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_engines_runtime_compiled.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_engines_tensor_ir.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_ext.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_graph.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_heuristic.so.9.22.0 .

# 创建符号链接
ln -sf libcudnn.so.9.22.0 libcudnn.so.9
ln -sf libcudnn.so.9 libcudnn.so

for lib in libcudnn_ops libcudnn_cnn libcudnn_adv libcudnn_engines_precompiled \
           libcudnn_engines_runtime_compiled libcudnn_engines_tensor_ir \
           libcudnn_ext libcudnn_graph libcudnn_heuristic; do
  ln -sf ${lib}.so.9.22.0 ${lib}.so.9
done

# TensorRT
cp "$TRT_SRC/libnvinfer.so.10.16.1" .
ln -sf libnvinfer.so.10.16.1 libnvinfer.so.10
ln -sf libnvinfer.so.10 libnvinfer.so

cp "$TRT_SRC/libnvonnxparser.so.10.16.1" .
ln -sf libnvonnxparser.so.10.16.1 libnvonnxparser.so.10
ln -sf libnvonnxparser.so.10 libnvonnxparser.so

cp "$TRT_SRC/libnvinfer_builder_resource_sm86.so.10.16.1" .
ln -sf libnvinfer_builder_resource_sm86.so.10.16.1 libnvinfer_builder_resource_sm86.so.10

echo "✓ 部署完成"
echo "cuDNN + TensorRT 总计: $(du -sh . | cut -f1)"
```

## 不同 GPU 架构说明

TensorRT builder 资源文件按 GPU 架构（SM）分发：

| 文件 | 对应 GPU |
|---|---|
| `libnvinfer_builder_resource_sm75.so.10` | RTX 2060/2070/2080, T4 |
| `libnvinfer_builder_resource_sm80.so.10` | A100, RTX 3090 |
| `libnvinfer_builder_resource_sm86.so.10` | **RTX 3080/3090 Ti, A4000** |
| `libnvinfer_builder_resource_sm89.so.10` | RTX 4080/4090 |
| `libnvinfer_builder_resource_sm90.so.10` | H100 |

如目标服务器 GPU 不同，复制对应的 SM 版本即可。

## 验证 GPU 加速

```bash
# 无需设置 LD_LIBRARY_PATH
./tests/test_codebert_embedder

# 应输出：
#   Using TensorRT GPU acceleration (FP16)
```

## 故障排除

### 问题：`libcudnn.so.9: file too short`

原因：cuDNN 9 的 `.so` 文件是文本链接脚本（GNU ld script），不是 ELF 文件。

解决：必须使用实际的 ELF 文件（如 `libcudnn.so.9.22.0`）创建符号链接：
```bash
# 错误方式（复制文本脚本）
cp /opt/cudnn9/lib/libcudnn.so.9 .

# 正确方式（复制实际 ELF 文件）
cp /opt/cudnn9/lib/libcudnn.so.9.22.0 .
ln -sf libcudnn.so.9.22.0 libcudnn.so.9
```

### 问题：`CUDA not available, falling back to CPU`

常见原因及解决：

1. **cuDNN 子库缺失**
   ```bash
   # cuDNN 9 采用模块化设计，必须复制所有子库
   # 仅复制 libcudnn.so.9 主库不够！
   ldd libonnxruntime_providers_tensorrt.so | grep "not found"
   ```

2. **cuDNN 版本不匹配**
   - ONNX Runtime 1.14.1 编译时链接了 `libcudnn.so.9`
   - 必须使用 cuDNN 9.x，不能用 cuDNN 8.x
   - 版本参考：`strings libonnxruntime_providers_cuda.so | grep libcudnn.so`

3. **GPU 驱动问题**
   ```bash
   nvidia-smi  # 检查驱动和 CUDA 版本
   ```

4. **CUDA 库缺失**
   - 需要 `libcublas.so.12`、`libcudart.so.12`
   - 通常随 CUDA toolkit 安装，路径：`/usr/local/cuda/lib64`


## 环境要求

- **GPU**: NVIDIA RTX 3080 或更高（支持 FP16）
- **驱动**: 535+ (CUDA 12 compatible)
- **系统库**: `libstdc++.so.6`, `libgcc_s.so.1` (通常已安装)

## 性能基准

RTX 3080 + TensorRT FP16 实测：

| 指标 | 数值 |
|---|---|
| 单文本延迟 | ~3.0 ms |
| 吞吐量 | ~340 texts/sec |
| 首次启动 | ~15s（TensorRT engine 编译） |
| 后续启动 | ~2s（engine 缓存） |

对比 CPU（i7-12700）：
- CPU: ~20 texts/sec
- **GPU: ~340 texts/sec（17x 加速）**

## 库文件清单（部署后）

项目根目录应包含：

```
libonnxruntime.so.1              → libonnxruntime_gpu.so
libonnxruntime_gpu.so            ONNX Runtime GPU 运行时
libonnxruntime_providers_cuda.so CUDA Execution Provider
libonnxruntime_providers_tensorrt.so TensorRT Execution Provider
libonnxruntime_providers_shared.so   Provider 共享库

# cuDNN 9.22.0（模块化设计，主库 + 9个子库）
libcudnn.so.9                    → libcudnn.so.9.22.0
libcudnn.so.9.22.0               cuDNN 主库（stub loader）
libcudnn_ops.so.9                → libcudnn_ops.so.9.22.0
libcudnn_cnn.so.9                → libcudnn_cnn.so.9.22.0
libcudnn_adv.so.9                → libcudnn_adv.so.9.22.0
libcudnn_engines_precompiled.so.9
libcudnn_engines_runtime_compiled.so.9
libcudnn_engines_tensor_ir.so.9
libcudnn_ext.so.9
libcudnn_graph.so.9
libcudnn_heuristic.so.9

# TensorRT 10.16.1
libnvinfer.so.10                 → libnvinfer.so.10.16.1
libnvinfer.so.10.16.1            TensorRT 推理引擎
libnvonnxparser.so.10            → libnvonnxparser.so.10.16.1
libnvonnxparser.so.10.16.1       TensorRT ONNX 解析器
libnvinfer_builder_resource_sm86.so.10  TensorRT SM86 资源（RTX 3080）
```

总计约 **3.5GB**（cuDNN 2.2GB + TensorRT 910MB + ONNX Runtime 480MB + tokenizers-cpp 38MB）。

## tokenizers-cpp C API 使用示例

### 初始化 BPE Tokenizer

```c
#include "tokenizers-cpp/tokenizers_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 从 vocab.json + merges.txt 创建 tokenizer
TokenizerHandle create_codebert_tokenizer(const char* vocab_path, const char* merges_path) {
    // 读取 vocab.json
    FILE* f = fopen(vocab_path, "rb");
    fseek(f, 0, SEEK_END);
    long vlen = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* vocab = malloc(vlen + 1);
    fread(vocab, 1, vlen, f);
    vocab[vlen] = '\0';
    fclose(f);
    
    // 读取 merges.txt
    f = fopen(merges_path, "rb");
    fseek(f, 0, SEEK_END);
    long mlen = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* merges = malloc(mlen + 1);
    fread(merges, 1, mlen, f);
    merges[mlen] = '\0';
    fclose(f);
    
    // 创建 byte-level BPE tokenizer
    TokenizerHandle tok = byte_level_bpe_tokenizers_new_from_str(
        vocab, vlen, merges, mlen, NULL, 0
    );
    
    free(vocab);
    free(merges);
    
    return tok;
}
```

### 编码文本

```c
// 编码单个文本
void encode_text(TokenizerHandle tok, const char* text) {
    TokenizerEncodeResult result;
    tokenizers_encode(tok, text, strlen(text), 1, &result);
    
    printf("Tokens (%zu): ", result.len);
    for (size_t i = 0; i < result.len; i++) {
        printf("%d ", result.token_ids[i]);
    }
    printf("\n");
    
    // 释放结果
    tokenizers_free_encode_results(&result, 1);
}

// 批量编码
void encode_batch(TokenizerHandle tok, const char** texts, int count) {
    TokenizerEncodeResult* results = malloc(count * sizeof(TokenizerEncodeResult));
    size_t* lens = malloc(count * sizeof(size_t));
    
    for (int i = 0; i < count; i++) {
        lens[i] = strlen(texts[i]);
    }
    
    tokenizers_encode_batch(tok, texts, lens, count, 1, results);
    
    for (int i = 0; i < count; i++) {
        printf("Text %d: %d tokens\n", i, (int)results[i].len);
    }
    
    tokenizers_free_encode_results(results, count);
    free(results);
    free(lens);
}
```

### 释放资源

```c
// 使用完毕后释放 tokenizer
tokenizers_free(tok);
```

### 与 ONNX Embedder 集成

本项目已将 tokenizers-cpp 集成到 `libonnx_embedder.so` 中：

```c
#include "onnx_embedder.h"

// 自动检测 tokenizer 类型（BPE/WordPiece）
onnx_embedder_t* embedder = onnx_embedder_init(
    "models/codebert-base/model.onnx",   // ONNX 模型
    "models/codebert-base/vocab.json",   // vocab.json（BPE）
    512,                                 // max_seq_length
    768                                  // dim
);

// 推理
float vec[768];
onnx_embedder_encode(embedder, "def hello():", vec);

// 释放
onnx_embedder_free(embedder);
```

**注意**：使用 `.json` 后缀的 vocab 路径时，会自动使用 tokenizers-cpp 的 BPE tokenizer；使用 `.txt` 后缀时，使用内置的 WordPiece tokenizer（兼容 MPNet）。

## 支持的 Tokenizer 类型

tokenizers-cpp 支持 HuggingFace 生态系统中的所有主流 tokenizer：

| 类型 | 代表模型 | 创建函数 |
|---|---|---|
| **Byte-level BPE** | GPT-2, CodeBERT, RoBERTa | `byte_level_bpe_tokenizers_new_from_str()` |
| **BPE** | GPT, GPT-2 | `tokenizers_new_from_str()` (JSON配置) |
| **WordPiece** | BERT, MPNet | 内置 C 实现 |
| **SentencePiece** | ALBERT, XLNet, T5 | `tokenizers_new_from_str()` (JSON配置) |
| **Unigram** | XLNet, ALBERT | `tokenizers_new_from_str()` (JSON配置) |

## 完整复制脚本（含 tokenizers-cpp）

一键执行所有步骤（构建 + 部署）：

```bash
#!/bin/bash
set -e

PROJECT_DIR="/home/dministrator/my_db"
CUDNN_SRC="/opt/cudnn-linux-x86_64-9.22.0.52_cuda12/lib"
TRT_SRC="/opt/TensorRT-10/lib"
TOKENIZERS_DIR="$PROJECT_DIR/third_party/tokenizers-cpp"

cd "$PROJECT_DIR"

echo "=== 步骤 1: 构建 tokenizers-cpp ==="
if [ ! -f "lib/tokenizers-cpp/libtokenizers_c.a" ]; then
    mkdir -p "$TOKENIZERS_DIR"
    cd "$TOKENIZERS_DIR"
    
    if [ ! -d "tokenizers-cpp" ]; then
        git clone --depth 1 https://github.com/mlc-ai/tokenizers-cpp.git
        cd tokenizers-cpp
        git submodule update --init --recursive
    else
        cd tokenizers-cpp
    fi
    
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    
    cd "$PROJECT_DIR"
    mkdir -p lib/tokenizers-cpp include/tokenizers-cpp
    cp "$TOKENIZERS_DIR/tokenizers-cpp/build/libtokenizers_c.a" lib/tokenizers-cpp/
    cp "$TOKENIZERS_DIR/tokenizers-cpp/build/libtokenizers_cpp.a" lib/tokenizers-cpp/
    cp "$TOKENIZERS_DIR/tokenizers-cpp/build/release/build/onig_sys-"*/out/libonig.a lib/tokenizers-cpp/ 2>/dev/null || \
    cp "$TOKENIZERS_DIR/tokenizers-cpp/build/release/build/onig_sys_"*/out/libonig.a lib/tokenizers-cpp/ 2>/dev/null || \
    find "$TOKENIZERS_DIR/tokenizers-cpp/build" -name "libonig.a" -exec cp {} lib/tokenizers-cpp/ \;
    cp "$TOKENIZERS_DIR/tokenizers-cpp/include/tokenizers_c.h" include/tokenizers-cpp/
    cp "$TOKENIZERS_DIR/tokenizers-cpp/include/tokenizers_cpp.h" include/tokenizers-cpp/
    echo "✓ tokenizers-cpp 构建完成"
else
    echo "✓ tokenizers-cpp 已存在，跳过构建"
fi

cd "$PROJECT_DIR"

echo "=== 步骤 2: 部署 GPU 依赖库 ==="

# cuDNN 9.22.0
rm -f libcudnn*.so*
cp "$CUDNN_SRC"/libcudnn.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_ops.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_cnn.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_adv.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_engines_precompiled.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_engines_runtime_compiled.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_engines_tensor_ir.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_ext.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_graph.so.9.22.0 .
cp "$CUDNN_SRC"/libcudnn_heuristic.so.9.22.0 .
ln -sf libcudnn.so.9.22.0 libcudnn.so.9
ln -sf libcudnn.so.9 libcudnn.so
for lib in libcudnn_ops libcudnn_cnn libcudnn_adv libcudnn_engines_precompiled \
           libcudnn_engines_runtime_compiled libcudnn_engines_tensor_ir \
           libcudnn_ext libcudnn_graph libcudnn_heuristic; do
  ln -sf ${lib}.so.9.22.0 ${lib}.so.9
done

# TensorRT
cp "$TRT_SRC/libnvinfer.so.10.16.1" .
ln -sf libnvinfer.so.10.16.1 libnvinfer.so.10
ln -sf libnvinfer.so.10 libnvinfer.so
cp "$TRT_SRC/libnvonnxparser.so.10.16.1" .
ln -sf libnvonnxparser.so.10.16.1 libnvonnxparser.so.10
ln -sf libnvonnxparser.so.10 libnvonnxparser.so
cp "$TRT_SRC/libnvinfer_builder_resource_sm86.so.10.16.1" .
ln -sf libnvinfer_builder_resource_sm86.so.10.16.1 libnvinfer_builder_resource_sm86.so.10

echo "✓ 全部部署完成"
echo "总计占用: $(du -sh . | cut -f1)"
echo ""
echo "验证命令:"
echo "  LD_LIBRARY_PATH=. ./tests/test_codebert_embedder"
```

## 故障排除

### 问题：`tokenizers-cpp` 编译失败

**原因**：缺少 Rust 工具链或 CMake

**解决**：
```bash
# 安装 Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env

# 安装 CMake
sudo apt-get install cmake build-essential
```

### 问题：`libonig.a` 找不到

**原因**：onig_sys 的构建目录名称因平台而异

**解决**：
```bash
# 手动查找
find third_party/tokenizers-cpp/tokenizers-cpp/build -name "libonig.a"
# 然后复制到 lib/tokenizers-cpp/
```

### 问题：链接时报 `undefined reference to tokenizers_new_from_str`

**原因**：链接顺序错误或缺少 `-lstdc++`

**解决**：确保链接顺序正确：
```bash
gcc ... -L./lib/tokenizers-cpp -ltokenizers_cpp -ltokenizers_c -lonig -lstdc++
```

### 问题：`CUDA not available, falling back to CPU`

参见上文「故障排除」章节。
