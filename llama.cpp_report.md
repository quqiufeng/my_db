# llama.cpp 代码级分析报告

> 基于代码探索记忆系统自动分析生成 | 2026-06-10

---

## 1. 项目概览

| 属性 | 值 |
|------|-----|
| 源码路径 | `/opt/llama.cpp` |
| 源文件总数 | 783 (C++/C/H/CUDA) |
| 索引 Chunks | **25829** |
| 唯一函数数 | **10801** |
| 调用边数 | **1784** |
| 追踪变量 | 2000 (696 字段级) |
| 语义向量 | 25829 条 (768 维) |

**目录结构：**

| 目录 | 文件数 | 说明 |
|------|--------|------|
| `src/` | 28 (.cpp) | 核心推理引擎 (llama.h, llama.cpp) |
| `ggml/src/` | 8 (.cpp) | GGML 张量计算库 |
| `common/` | 26 (.cpp) | 公用工具 |
| `include/` | 2 (.h) | 公共头文件 |
| `examples/` | — | 示例程序和绑定 |
| `gguf-py/` | — | GGUF 格式 Python 工具 |
| `tests/` | — | 测试用例 |

---

## 2. 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                      llama.cpp                                │
│                                                               │
│  ┌───────────────────────────────────────────────────────┐   │
│  │              llama.h / llama.cpp (核心 API)             │   │
│  │  llama_model_load  llama_decode  llama_encode          │   │
│  │  llama_create_context  llama_set_seed  llama_sampling  │   │
│  └──────────────────────┬────────────────────────────────┘   │
│                         │                                      │
│  ┌──────────────────────▼────────────────────────────────┐   │
│  │              GGML (张量计算库)                           │   │
│  │  ggml_mul_mat  ggml_compute_forward  ggml_graph        │   │
│  │  ggml_cpu  ggml_cuda  ggml_metal  ggml_vulkan         │   │
│  └──────────────────────┬────────────────────────────────┘   │
│                         │                                      │
│  ┌──────────────────────▼────────────────────────────────┐   │
│  │            Backend (硬件加速后端)                         │   │
│  │  CUDA (NVIDIA)  |  Metal (Apple)  |  Vulkan (通用)    │   │
│  │  SYCL (Intel)   |  WebGPU (浏览器)  |  CPU (fallback) │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

## 3. 核心推理引擎

### 3.1 模型加载

| 函数 | Score | 说明 |
|------|-------|------|
| `llama_model_load_from_file_impl` | **0.9075** | 模型文件加载主入口 |
| `llama_load_model_from_file` | 0.8866 | 公开 API 接口 |
| `llama_model_xverse::load_arch_tensors` | 0.8799 | 特定架构张量加载 |

### 3.2 推理执行

| 函数 | Score | 说明 |
|------|-------|------|
| `llama_decode` | **0.9101** | 推理执行主入口 |
| `llama_context::decode` | 0.8044 | 内部 decode 实现 |

### 3.3 采样

| 函数 | Score | 说明 |
|------|-------|------|
| `llama_sampler_top_k_impl` | **0.8805** | Top-K 采样 |
| `llama_sampler_top_k_apply` | 0.8623 | Top-K 应用 |
| `llama_sampler_init_top_k` | 0.7884 | Top-K 初始化 |

## 4. GGML 张量库

| 函数 | Score | 说明 |
|------|-------|------|
| `ggml_mul_mat` | **0.9016** | 矩阵乘法核心 |
| `ggml_compute_forward_mul_mat` | 0.8843 | 矩阵乘法前向计算 |
| `ggml_quantize_init` | 0.8971 | 量化初始化 |
| `dequantize` | 0.8521 | 反量化操作 |

### 量化支持

GGML 支持多种量化格式以压缩模型权重：

- **Q4_0**: 4-bit 块量化（块内共享 scale）
- **Q8_0**: 8-bit 块量化
- **Q5_1**: 5-bit 高精度量化
- **IQ4_NL**: 4-bit 非对称量化

## 5. KV Cache

| 函数 | Score | 说明 |
|------|-------|------|
| `llama_kv_cache_context` | 0.8096 | KV Cache 上下文 |
| `llama_kv_cache_context::apply` | 0.7775 | 缓存应用 |

KV Cache 存储注意力层的 Key 和 Value 矩阵，通过 **paged attention** 管理显存，避免推理过程中的重复计算。

## 6. 硬件加速后端

语义搜索检测到的后端：

| 后端 | 文件 | 状态 |
|------|------|------|
| CUDA | `ggml-cuda.cu` | NVIDIA GPU |
| Metal | `ggml-metal.m` | Apple Silicon |
| Vulkan | `ggml-vulkan.cpp` | 跨平台 GPU |
| SYCL | `ggml-sycl.cpp` | Intel GPU |
| WebGPU | `ggml-webgpu.cpp` | 浏览器 |
| CPU | `ggml-cpu.cpp` | fallback |

---

## 7. 分析数据

| 数据文件 | 路径 |
|---------|------|
| 代码内容 | `/opt/code_caches/llama.cpp_cache/chunks_text.txt` |
| 元数据 | `/opt/code_caches/llama.cpp_cache/chunks_meta.jsonl` |
| 调用关系 | `/opt/code_caches/llama.cpp_cache/call_graph.json` (1784 边) |
| 数据流 | `/opt/code_caches/llama.cpp_cache/dataflow.json` (2000 变量) |
| 语义向量 | `/opt/code_caches/llama.cpp_cache/vectors/code_local_llama.cpp.jina.bin` |
| KV 命名空间 | `/code/llama.cpp` (55103 keys) |

```bash
# 查询示例
sudo /opt/my_db/tools/cache_query llama_decode --repo /code/llama.cpp --type context --depth 2
sudo /opt/my_db/ai_code_search.sh search /opt/code_caches/llama.cpp_cache "quantize Q4_0" 5
sudo /opt/my_db/ai_code_search.sh search /opt/code_caches/llama.cpp_cache "attention mask transformer" 5
```

---

*报告由代码探索记忆系统自动分析生成 | llama.cpp | 2026-06-10*
