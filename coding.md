# 代码探索记忆系统 - 永不丢失的代码知识大脑

> [← 返回项目总览](README.md)

---

## 📖 Code Search 使用方法（权威速查 · 必读）

> **完整方法论**（Phase 映射 / 证据分级 / 报告模板）见 **[code_search_best_practices.md](code_search_best_practices.md)**。
> 探索任何项目：**先 `check` → 再「整体架构 → 子模块」→ 结论标 `[static]/[verified]`**。

### 0. 前置检查（必做）

```bash
./ai_code_search.sh check <cache_dir> [project]
# 校验：chunks / 向量 / HNSW 新鲜度 / call_graph / dataflow / word_freq / KV 是否导入
```

- `check` 通过才可探索；**KV 未导入** → `context/symbol` 不可用（只能语义搜索）。
- 导入 KV：`./tools/cache_import <cache_dir> /code/<project> --cache-dir /memory`（含 chunks+callgraph+dataflow+**symbols**）。

### 1. 统一入口：`ai_code_search.sh`

| 子命令 | 用途 |
|---|---|
| `check <cache> [project]` | 探索前置检查 |
| `index <repo> <cache> [workers] [--exclude-dir a,b]` | 索引（CPU，**串行**） |
| `vector <cache> [name]` | 词频→向量→调用图→数据流→HNSW 验收（GPU） |
| `search <cache> "<自然语言>" [n] [--rich\|--callgraph\|--kind\|--lang\|--file\|--json]` | 语义搜索，**默认 brief**（`name+score+Location`），`--rich` 给完整定义 |
| `snippet <cache> <file> [n]` | 代码片段相似实现 |
| `dataflow <cache> <var>` | 变量/字段级数据流 |
| `callgraph <cache>` | 构建/查看调用图 |
| `cross-search "<query>" [n] [proj...]` | 跨项目搜索（并发限流+超时） |

```bash
./ai_code_search.sh check /opt/code_caches/nginx_cache nginx
./ai_code_search.sh search /opt/code_caches/nginx_cache "event loop epoll" 8
./ai_code_search.sh search /opt/code_caches/nginx_cache "ngx_palloc" 3 --rich
```

### 2. 精确符号 / 上下文：`tools/cache_query`（走 KV，**不需向量**）

```bash
CQ=./tools/cache_query
$CQ <name>  --repo /code/<project> --type symbol            # 精确定位
$CQ <name>  --repo /code/<project> --type context --depth N # +caller/callee/调用链/dataflow
$CQ <name>  --repo /code/<project> --type context --brief   # 精简：符号+caller/callee 名
$CQ /code/<project>/chunks/... --type exact                 # 原始 key
```

- **stdout 是纯 JSON**（`[CACHE]`/embedder 日志已改到 stderr），可直接管道 `jq`/python。
- `context` 的 `callers/callees` 是真实调用关系（已修反向图 bug）。

### 3. 脚本化探索（agent 首选）

**`tools/ctx.py`**——批量 context，只回吐「符号 + caller/callee 名」，省 context：

```bash
./tools/ctx.py /code/linux_723 vfs_read ksys_read filemap_read --max 8
./tools/ctx.py /code/openresty ngx_http_lua_run_thread --depth 2
./tools/ctx.py /code/linux_723 call_rcu --json        # + --json 走管道
```

**探索骨架**（与最佳实践 §4/§6 对应）：

```bash
# ① 架构扫描：一批自然语言 query 定位子系统入口
for q in "scheduler pick next task" "virtual file system inode" "network socket tcp"; do
  ./ai_code_search.sh search <cache> "$q" 5            # 默认 brief，直接看 file:line
done
# ② 批量取调用关系（脚本）：确认主线/多态/横切
./tools/ctx.py /code/<project> <fn1> <fn2> <fn3> --max 10
# ③ 数据流：追踪关键变量/字段
./ai_code_search.sh dataflow <cache> <var>
# ④ 跨项目对照（已在 /memory 的项目）
./ai_code_search.sh cross-search "memory pool allocator"
```

核心：**CLI 返回结构化 JSON → 脚本只提取所需字段 → 少 token、可聚合**。单次查询直接命令即可，批量/聚合用 `ctx.py`。

### 4. 证据纪律

- `search`/`callgraph`/`dataflow`/`context` 结果**一律是 `[static]`**（静态证据），不等于运行时真相。
- 承重结论须经真实 build + 断点/测试验证后才能标 `[verified]`。
- 引用统一用 `file:symbol`，行号仅参考。

### 5. 改动后回归（真实项目验证）

```bash
./tests/validate_tools.sh     # 内置 openresty + linux_723；向量/HNSW/search 分数/context callees/ dataflow 不变量
```

> 经验：本项目关键缺陷（UTF-8 abort、cache_import O(n²)、namespace 去重爆炸、dataflow 内存上限、symbol 未导入、日志污染 stdout、callees 自指）**全部由真实项目暴露**，务必用真项目跑回归。

---

## ⚙️ 运行依赖（GPU 加速必装 TensorRT）

**向量化（`batch_embedder`）的加速依赖 TensorRT**，缺它可用但**明显变慢**（自动降级，不报错）：

| 依赖 | 作用 | 缺失后果 |
|---|---|---|
| **NVIDIA 驱动 + CUDA** | GPU 基础 | 回落到 CPU（极慢） |
| **TensorRT 10**（`/opt/TensorRT-10`） | 推理加速（实测内核 69 万 chunks **~17 分钟**） | 回落 CUDA EP（慢得多） |
| **cuDNN** | CUDA EP 依赖 | CUDA 不可用 |
| **ONNX Runtime GPU 1.20.1**（`/data/venv/onnxruntime-linux-x64-gpu-1.20.1`） | ONNX 推理引擎 | 无法加载模型 |
| **Jina v2 模型** `/opt/models/jina-embeddings-v2-base-code/` | 嵌入模型 | 搜索失效 |

**TRT 配置要点**（已在代码内固定）：
- **动态 shape profile**：`input_ids/attention_mask: 1x64 … 32x512`（配合"按 batch 实际长度 padding + 长度分桶"）。
  ⚠️ profile 若写成固定 `512`，变长输入会 `setInputShape` 失败 → 该批跳过 → 大量丢向量。改 profile 后**必须删 `$HOME/trt_cache` 重建 engine**。
- `BATCH_SIZE ≤ 32`（TRT profile 上限；超限失败并**非零退出**，绝不写零向量）。
- 首次运行在 `$HOME/trt_cache` 构建 engine（约十几秒），之后复用。

**环境变量**（`ai_code_search.sh` 已自动拼接；单独调试 `batch_embedder`/`cache_query` 时需手动设）：
```bash
export LD_LIBRARY_PATH="/opt/my_db:/opt/TensorRT-10/lib:/opt/cuda/lib64:\
/data/venv/lib/python3.12/site-packages/nvidia/cudnn/lib:\
/data/venv/onnxruntime-linux-x64-gpu-1.20.1/lib:${LD_LIBRARY_PATH:-}"
```

> **实测加速比（linux 内核 693,026 chunks）**：优化前 140 items/s（82 分钟）→ 优化后 **684 items/s（约 17 分钟）= ~4.9x**。

> **HNSW 构建提速**：768 维点积加 `#pragma omp simd`（`-O2` 默认不向量化归约）+ `build_hnsw_index` 新增
> `--m/--ef-construction/--ef-search`。openresty 35k 向量：34.9s → **17.6s（SIMD）/ 13.3s（ef=100）= ~2.6x**；
> 内核 HNSW（原 ~15 分钟）预计 **~6 分钟**。

### GPU 可调参数（按显卡/显存调整）

> **当前机器：NVIDIA GeForce RTX 3080（20GB）**。
> 以下值均为**源码硬编码**（暂不支持 env 覆盖），改后需 `make` 重编译对应库/工具；
> **换卡（尤其换架构）后建议 `rm -rf $HOME/trt_cache`** 让 TensorRT 重建 engine。

| 参数 | 当前值 | 位置 | 调整建议（按显存） |
|---|---|---|---|
| `device_id` | `0` | `src/embedding/onnx_embedder.c:592`(TRT-V2) / `:627`(TRT-legacy) / `:644`(CUDA) | 多卡时改成目标 GPU 序号 |
| `trt_max_workspace_size` | **12 GB** | `onnx_embedder.c:593`(V2) / `:630`(legacy) | ≥24GB 可提到 16–20GB；10–12GB 降到 4–6GB；8GB 降到 ~2–3GB（**过大可能建 engine 时 OOM**） |
| `cuda gpu_mem_limit` | **16 GB** | `onnx_embedder.c:647` | 设为显存 ~80%（3080 20GB → 16GB 合适） |
| TRT 动态 profile | `1x64 … 32x512` | `onnx_embedder.c:599-601` | batch 上限(=32)、seq 上限(512) 可按显存/模型调；改后删 trt_cache |
| `BATCH_SIZE` | `32` | `tools/batch_embedder.c:15` | 不得超过 profile 的 batch 上限(32)；小显存可降到 16 |
| `MAX_SEQ` | `512` | `tools/batch_embedder.c:14` | Jina 模型上限 512，一般不动 |
| `DIM` | `768` | `tools/batch_embedder.c:13` | Jina v2 固定 768，**不可改** |
| TRT engine 缓存 | `$HOME/trt_cache` | — | 换卡/改 profile 后删除以重建 |

**当前 3080 20GB 的取值即现状，无需改动**：`device_id=0`、`trt_max_workspace_size=12GB`、`gpu_mem_limit=16GB`、`BATCH_SIZE=32`。

**换卡参考**：
- **RTX 4090D（24GB）**：现有 `trt_max_workspace_size=12GB`、`gpu_mem_limit=16GB` **正是当初按 4090D 写的，可直接用**；想更充分利用可把 `gpu_mem_limit` 提到 ~19GB、workspace 提到 16GB（对 Jina 这种小模型收益有限，12GB 已足够）。其余参数（profile/BATCH_SIZE/DIM）不变。
- **≤12GB 的卡**：至少把 `trt_max_workspace_size` 降到 4–6GB、`gpu_mem_limit` 降到 ~8–10GB，否则 TRT 建 engine 可能 OOM。
- **换任意卡后**：`rm -rf $HOME/trt_cache` 让 TRT 按新 GPU 重建 engine（跨架构 engine 不通用）。

---

## 🚀 开发与部署流程（固定）

> **本地开发、远程使用**：本地改源码 → 本地 `make` → `scp` 二进制到远程 → 远程直接用。
> （远程 `/opt/my_db` **不是 git clone**，含 onnx/cudnn 大库不进 git，**不要指望远程 `git pull`**。）
> 源码历史在 GitHub **`quqiufeng/my_db`** 留存。

```bash
# ① 本地改源码（本地 /opt/my_db = git 仓库 quqiufeng/my_db）
cd /opt/my_db
vim tools/vector_search.c                 # 例：改某个工具
git add tools/vector_search.c && git commit -m "..." && git push

# ② 本地编译（不要在远程编译）
make tools/vector_search                   # 单工具；或 make 全量

# ③ 传二进制到远程
sshpass -p '<pw>' scp -P <port> tools/vector_search root@<host>:/opt/my_db/tools/

# ④ 远程直接用（无需编译）
./ai_code_search.sh search <cache_dir> "<query>" <k>
```

> ⚠️ 索引任务必须**串行**（code_indexer 的 worker 临时文件是固定 `/tmp` 名，并行会交叉污染）。

---

## 🎯 功能与作用

**作用**：把任意代码仓库变成**可自然语言查询的记忆库**，给 LLM 做 **RAG（检索增强）**——
定位实现、取**完整类型定义**、看调用关系/数据流、找同族实现与用法。
用途：**读项目 / 改项目 / 写代码**（补 LLM 不知道未见项目类型系统的短板 = 信息对等）。

**核心能力**：

| 能力 | 命令 | 说明 |
|---|---|---|
| **语义搜索** | `ai_code_search.sh search <cache> "<自然语言>" [k]` | 功能描述 → 定位 `file:line`；**默认 brief**（name+score+Location），`--rich` 输出完整定义 `FullDef` |
| 代码片段 | `snippet <cache> <file>` | 按文件/片段匹配 |
| 变量数据流 | `dataflow <cache> <var>` | 字段级 + 跨函数的 定义/赋值/使用 |
| 调用关系 | `callgraph <cache>`（search 自带 callers） | 谁调用谁（调用图） |
| 索引 | `index <repo> <cache> [workers]` | 提取 chunks（函数/结构体/宏/typedef） |
| 向量 | `vector <cache> [name]` | 词频 + Jina 向量 + 调用图 + 数据流 四步 |
| 词频 | `word_freq` | TF-IDF 重排（辅助搜索） |

**关键性质（面向 LLM/RAG）**：
- **信息对等**：`--rich` 现在返回**完整定义**（修了旧的"窗口切成 Part N、只返回一片"）。
- **精确定位**：以 `file:symbol` 为准（版本免疫），行号仅参考。
- **多维证据**：语义检索 + 调用图 + 数据流可交叉验证。
- **本地隐私**：C 引擎 + GPU 向量，代码不出机器。

---

## 🔧 索引生成流程（严格：index + vector + hnsw，**缺一不可**）

> ⚠️ **只跑 `index` → 没有向量 → 语义搜索 `search` 失效**（search 靠 Jina 向量；
> chunks 一旦重建，向量必须一起重算，否则**向量与 chunks 对不上**）。

```bash
cd /opt/my_db
# ① 抽取 chunks（CPU：ctags + AST 文本解析，workers 并行）
./ai_code_search.sh index  <repo_path> <cache_dir> [workers]
# ② 生成向量 + 调用图 + 数据流 + 校验/重建 HNSW（GPU；五步 = 词频 → 向量(含HNSW) → 调用图 → 数据流 → HNSW验收）
./ai_code_search.sh vector <cache_dir> <project_name>
# ③ 单独重建 HNSW（一般不需要；vector 已自动验收，缺失/过旧会自动重建）
./tools/build_hnsw_index <cache_dir>/vectors/code_local_<project_name>.jina.bin --threads 8
```

**纪律（血泪教训）**：
1. **索引任务必须串行**——`code_indexer` 的 worker 临时文件是固定 `/tmp` 名，并行会交叉污染缓存。
2. **`index` 是 CPU（ctags），`vector` 是 GPU（Jina/TensorRT）**——**重建索引后必须重算向量**，
   否则 `search` 的向量与 chunks 不匹配 → 语义搜索失效。
3. **HNSW 由 `vector` 自动校验/重建**（Step 5/5，`.hnsw` 缺失或旧于 `.bin` 才重跑 `build_hnsw_index`）。
4. **验收标准**：`.hnsw` 时间戳**新于** `.bin`（防旧索引冒充新索引）；`search` 冒烟命中。
5. **老缓存 root 属主** → embedder/hnsw 静默写失败留旧文件 → 先 `chown -R` 再重建。
6. **向量编码不能静默失败**：`batch_embedder` 的 `BATCH_SIZE` 必须 ≤ TensorRT 优化 profile 上限（当前 `32`）。
   超限时 `setInputShape` 报错；旧版会 `memset` 成**零向量**（搜索分数塌到 ~0.05 却不报错）。
   现已改为：跳过失败批 + 结束时**非零退出**，且 `vector` 命令校验 HNSW 失败即中止。

---

## 项目文件存储目录

所有探索开源项目生成的数据文件统一保存在 **`/code/`** 目录下，按项目名组织：

```
/code/
├── {project}/                    # 项目分析输出
│   ├── chunks_text.txt           # 代码内容（一行一个 chunk）
│   ├── chunks_meta.jsonl         # 元数据（函数名/文件/行号/签名）
│   ├── call_graph.json           # 调用关系图
│   ├── dataflow.json             # 变量数据流
│   ├── word_freq.json            # 词频统计
│   └── vectors/                  # 语义向量
│       ├── {project}.jina.bin
│       ├── {project}.jina.idx
│       └── {project}.jina.bin.hnsw
├── explore_repo.sh               # 项目探索脚本（软链接到项目根目录）
├── analyze_repo.sh               # 项目分析脚本（软链接到项目根目录）
└── analyze_nodejs_repo.sh        # Node.js/TypeScript 项目专用分析脚本
```

> **注意**: KV Cache 二进制数据（`cache.bin` + `index.bin` + `vectors/`）保存在 `/memory/` 目录下；`/code/{project}/` 下存放的是可读的分析产出（文本、JSON、向量等临时文件）。

### 待探索项目列表

| 项目 | 源码路径 | 分析输出 | 命名空间 | 状态 | 计划导入时间 |
|------|----------|----------|----------|------|-------------|
| Linux Kernel | /opt/linux/src/linux-7.0.11 | /code/linux | /code/linux | ✅ 2026-06-10 | 已完成（不含 drivers） |
| OpenResty | /opt/openresty-1.31.1.1 | /code/openresty | /code/openresty | ✅ 2026-06-10 | 已完成 |
| Redis | /opt/redis | /code/redis | /code/redis | ✅ 2026-08-28 | 已完成（10658 chunks） |
| stable-diffusion.cpp | /opt/sd | /code/sd | /code/sd | 已完成 | 2026-07-07 |
| llama.cpp | /opt/llama.cpp | /code/llama.cpp | /code/llama.cpp | ✅ 2026-06-10 | 已完成 |
| **PostgreSQL** | /opt/postgresql（git，2023+ 历史，更新至 2026-09-16） | /code/postgresql | /code/postgresql | ✅ 2026-09-16 | 56627 chunks，索引重建完成 |
| SQLite | 待下载 | /code/sqlite | /code/sqlite | 待探索 | — |
| RocksDB | 待下载 | /code/rocksdb | /code/rocksdb | 待探索 | — |
| HAProxy | 待下载 | /code/haproxy | /code/haproxy | 待探索 | — |
| libuv | 待下载 | /code/libuv | /code/libuv | 待探索 | — |
| whisper.cpp | 待下载 | /code/whisper.cpp | /code/whisper.cpp | 待探索 | — |
| CPython | /opt/cpython/src（git，更新至 2026-09-16） | /code/python | /code/python | ✅ 2026-09-16 重建 | 67590 chunks，索引重建完成 |
| LuaJIT | https://github.com/LuaJIT/LuaJIT | /code/luajit | /code/LuaJIT/LuaJIT | ✅ 2026-06-10 | 已完成 |
| HotSpot JVM | 待下载 | /code/hotspot | /code/hotspot | 待探索 | — |
| mruby | 待下载 | /code/mruby | /code/mruby | 待探索 | — |
| Zig | https://github.com/ziglang/zig | /code/zig | /code/zig | 待探索 | — |
| PHP | /opt/php/src（git，更新至 2026-09-16） | /code/php | /code/php | ✅ 2026-09-16 重建 | 65459 chunks，索引重建完成 |
| opencode | /opt/opencode | /code/opencode | /code/opencode | ✅ 2026-06-15 | 已完成（Node.js/TypeScript 验证） |
| ComfyUI | /opt/static_comfyui/ComfyUI | /code/comfyui | /code/comfyui | ✅ 2026-07-05 | 已完成 |
| **Ruby (CRuby)** | /opt/ruby（git，2023+ 历史，2026-08-31） | /code/ruby | /code/ruby | ✅ 2026-09-16 | 153005 chunks，新建索引 |
| **Perl5** | /opt/perl5（git，2023+ 历史，2026-09-15） | /code/perl5 | /code/perl5 | ✅ 2026-09-16 | 9311 chunks，新建索引 |
| **qwenimage-ncnn-vulkan** | /opt/qwenimage-ncnn-vulkan（GitHub nihui，Qwen-Image-2.1 ncnn/Vulkan） | /opt/code_caches/qwenimage_cache | 同左 | ✅ 2026-10-01 | 1145 chunks（39 文件），已探索架构 |
| **zimage-ncnn-vulkan** | /opt/zimage-ncnn-vulkan（GitHub nihui，Z-Image ncnn/Vulkan，经 gitcode.com/gh_mirrors 镜像 clone——GitHub 直连当时不通） | /opt/code_caches/zimage_cache | 同左 | ✅ 2026-10-01 | 496 chunks（18 文件），早期开发阶段 |

> **2026-09-16 批量重建说明**：CPython/PHP 源码 git 更新至 09-16 后索引全量重建（index+vector 覆盖写）；
> Ruby/Perl5/PostgreSQL 为新建索引（git 含 2023+ 提交历史，可用于 func_pairs 修复对语料）。
> 当日全部完成（chunks + vectors + hnsw + call_graph + dataflow），五语言冒烟搜索全部命中。
> ⚠️ 教训三条：
> ① code_indexer 的 worker 临时文件是固定 /tmp 名——**索引任务必须串行**，并行会交叉污染（php 的 chunks 曾混入 postgresql 缓存）。
> ② **hnsw 不在 `vector` 四步流水线里**（词频→embedder→call_graph→dataflow），需单独跑
>    `tools/build_hnsw_index <cache>/vectors/code_local_{name}.jina.bin --threads 8`。
> ③ 老缓存若是 root 属主（6 月期），embedder/hnsw 会静默写失败留旧文件——先 `chown -R` 再重建；
>    验收标准 = **.hnsw 时间戳比 .bin 新**（防旧索引冒充新索引）。


> 使用 `./analyze_repo.sh <source>` 分析新项目后，数据会自动保存到 `/code/{project}/`。

---

## 系统概述

本项目实现了一套**本地代码语义搜索与记忆系统**，能够将任意代码仓库（从几百文件的小项目到 Linux 内核级别的超大项目）转换为可自然语言查询的智能记忆库。系统基于 C 语言实现核心引擎，使用 GPU 加速语义向量生成，通过 mmap 持久化存储确保知识永不丢失。

**核心理念**：把代码库变成你的"第二大脑"——一次索引，永久记忆，随时用自然语言"回忆"起任何实现细节、调用关系和数据流路径。

### 系统特点

- **完全本地运行**：代码不离开本地机器，企业级隐私安全
- **GPU 加速**：TensorRT + cuDNN + CUDA，向量生成速度 ~100 items/s
- **持久化存储**：mmap + msync(MS_SYNC)，断电不丢数据
- **自然语言搜索**：用英文描述想找的功能，无需记住函数名
- **多维度分析**：语义搜索 + 调用图 + 数据流追踪 + 代码片段匹配
- **超大项目支持**：智能分治策略，支持 Linux 内核（5万+文件）

---

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                        用户接口层                                │
│  ┌──────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │ analyze_repo.sh  │  │ ai_code_search.sh│  │ai_code_search_ │ │
│  │  (分析主控)       │  │  (统一搜索)       │  │   large.sh     │ │
│  │                  │  │                  │  │ (超大项目分治)  │ │
│  └────────┬─────────┘  └────────┬─────────┘  └───────┬────────┘ │
└───────────┼─────────────────────┼────────────────────┼──────────┘
            │                     │                    │
            ▼                     ▼                    ▼
┌─────────────────────────────────────────────────────────────────┐
│                      核心工具链（C 实现）                         │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐            │
│  │ code_indexer │ │batch_embedder│ │ vector_search│            │
│  │   (索引器)    │ │  (向量化)     │ │   (搜索引擎)  │            │
│  └──────────────┘ └──────────────┘ └──────────────┘            │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐            │
│  │  call_graph  │ │   dataflow   │ │   word_freq  │            │
│  │  (调用图)     │ │  (数据流)     │ │  (词频统计)   │            │
│  └──────────────┘ └──────────────┘ └──────────────┘            │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      KV Cache 存储层                             │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │   cache.bin     │  │    index.bin     │  │   vectors/      │ │
│  │   (mmap数据)     │  │   (Hash+SkipList)│  │   (语义向量)     │ │
│  └─────────────────┘  └──────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
            │
            ▼
┌─────────────────────────────────────────────────────────────────┐
│                      推理加速层                                  │
│  ┌─────────────────┐  ┌──────────────────┐  ┌─────────────────┐ │
│  │  model.onnx     │  │ ONNX Runtime     │  │  TensorRT       │ │
│  │  (Jina v2模型)   │  │   (推理引擎)      │  │   (GPU加速)      │ │
│  └─────────────────┘  └──────────────────┘  └─────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
```

### 数据流

1. **导入阶段**：脚本读取源码 → code_indexer 提取函数/代码块 → batch_embedder 生成语义向量 → cache_import 写入 KV Cache
2. **分析阶段**：call_graph 构建调用关系、dataflow 追踪变量、word_freq 统计词频
3. **存储阶段**：所有数据通过 mmap 持久化到磁盘，实时 msync 保证不丢失
4. **查询阶段**：自然语言查询 → 向量化 → HNSW 近似搜索 → 返回相关代码块 + 调用关系 + 数据流

---

## 核心组件详解

### 1. analyze_repo.sh — 代码分析主控脚本

**定位**：将任意代码仓库一键转换为可查询的记忆（完整 6 步流水线）

**路径**：`./analyze_repo.sh`

**功能**：
- 自动获取源码（GitHub URL 自动 clone，本地路径直接使用）
- 并行索引代码（C 多进程，ctags + AST，每秒 1000+ 文件）
- GPU 生成语义向量（TensorRT，~100 items/s）
- 构建调用图（caller/callee 关系）
- 数据流分析（变量定义/赋值/使用追踪）
- 导入 KV Cache（mmap 持久化，层级命名空间）

**用法**：
```bash
./analyze_repo.sh <source> [namespace] [options]
```

**参数说明**：
| 参数 | 必需 | 说明 |
|------|------|------|
| `source` | 是 | GitHub URL 或本地路径 |
| `namespace` | 否 | 记忆命名空间，默认自动检测 |
| `--skip-vectors` | 否 | 跳过向量生成（更快，但无语义搜索） |
| `--skip-callgraph` | 否 | 跳过调用图分析 |
| `--skip-dataflow` | 否 | 跳过数据流分析 |
| `--cache-dir <dir>` | 否 | KV Cache 目录（默认: /memory） |
| `--jobs <n>` | 否 | 并行工作进程数（默认: CPU 核心数） |
| `--name <name>` | 否 | 项目名（用于目录命名） |
| `--plugins <path>` | 否 | 插件注册表 JSON（推荐 Node.js/TypeScript 项目使用） |

**命名空间自动检测**：
- GitHub URL: `https://github.com/owner/repo` → `/code/owner/repo`
- 本地路径: `/opt/nginx` → `/code/local/nginx`

**使用示例**：
```bash
# 分析 GitHub 仓库（自动 clone）
./analyze_repo.sh https://github.com/redis/redis

# 分析本地项目
./analyze_repo.sh /opt/nginx

# 指定命名空间
./analyze_repo.sh /opt/nginx /code/nginx

# 快速分析（跳过 GPU 向量生成，适合只看代码结构）
./analyze_repo.sh /opt/redis --skip-vectors

# 仅索引+调用图（适合快速浏览）
./analyze_repo.sh /opt/sqlite --skip-vectors --skip-dataflow

# 指定并行度和缓存目录
./analyze_repo.sh /opt/linux/src/linux-7.0.11 /code/linux --jobs 8 --cache-dir /data/cache
```

### Node.js / TypeScript 项目专用分析

对于 Node.js / TypeScript 项目（包括 React/Vue/SolidJS、Bun、Effect-TS 等），推荐使用 TypeScript AST 插件替代默认 ctags，可获得更精准的函数/类型/类/接口级 chunks，避免成员变量和局部变量噪声。

**前置条件**：
```bash
# 插件已经预置在 /opt/my_db/plugins/typescript-indexer/
cd /opt/my_db/plugins/typescript-indexer
make                    # 首次编译
ldd bin/typescript-indexer   # 确认只依赖 libjansson.so.4 和 libc
```

**一键分析命令**：
```bash
./analyze_repo.sh /opt/opencode /code/opencode \
  --name opencode \
  --plugins /opt/my_db/plugins/typescript-indexer/plugin.json \
  --jobs 4
```

**便捷包装脚本**（无需记忆插件路径）：
```bash
./analyze_nodejs_repo.sh /opt/opencode /code/opencode
```

**命名空间自动检测示例**：
| 源码路径 | 自动命名空间 |
|----------|--------------|
| `/opt/opencode` | `/code/local/opencode` |
| `/opt/my-app` | `/code/local/my-app` |
| `https://github.com/anomalyco/opencode` | `/code/anomalyco/opencode` |

**与默认 ctags 对比**：
| 指标 | ctags | TS AST 插件 |
|------|-------|-------------|
| opencode chunks | 31691 | 10218 |
| 噪声（成员/字段/变量） | 高 | 低 |
| 函数/类型/类/接口 | 较少 | 主导 |
| TypeScript 泛型/类型别名 | 不支持 | 支持 |
| 导出标记 | 无 | 有（tags 含 `export`） |

**6 步流水线详解**：

| 步骤 | 工具 | 输入 | 输出 | 说明 |
|------|------|------|------|------|
| Step 1 | git clone | GitHub URL | 临时目录 | 自动清理临时文件 |
| Step 2 | code_indexer | 源码目录 | chunks_text.txt, chunks_meta.jsonl | 多进程并行，每秒 1000+ 文件 |
| Step 3 | batch_embedder | chunks_text.txt | vectors/*.jina.bin | TensorRT GPU，~100 items/s |
| Step 4 | call_graph | chunks_meta.jsonl | call_graph.json | 函数调用关系 |
| Step 5 | dataflow | chunks_meta.jsonl | dataflow.json | 变量数据流 |
| Step 6 | cache_import | 所有分析结果 | cache.bin, index.bin | mmap 持久化，实时落盘 |

**输出数据结构**：
```
/opt/code_caches/{project}_cache/                    # 分析目录（临时数据）
├── chunks_text.txt                   # 代码内容（一行一个 chunk）
├── chunks_meta.jsonl                 # 元数据（函数名/文件/行号/签名）
├── call_graph.json                   # 调用关系图
├── dataflow.json                     # 数据流分析
└── vectors/
    ├── code_local_{project}.jina.bin       # 语义向量（768维 float32）
    ├── code_local_{project}.jina.idx       # 名称→偏移索引
    └── code_local_{project}.jina.bin.hnsw  # HNSW 近似索引

/memory/                     # KV Cache（持久化存储）
├── cache.bin                         # mmap 数据文件
└── index.bin                         # 索引文件（Hash + Skip List）
```

**内存中的 Key 格式**：
```
/code/{project}/chunks/{file}/{function}     # 代码块
/code/{project}/symbols/{name}               # 符号索引
/code/{project}/callers/{function}           # 调用者列表
/code/{project}/callees/{function}           # 被调用者列表
/code/{project}/_meta/info                   # 项目元数据
```

**查询示例**（分析完成后）：
```bash
# 符号上下文查询（含 caller/callee）
./tools/cache_query <function> --repo /code/local/redis --type context --depth 2

# 语义搜索（自然语言）
./tools/cache_query "memory allocation" --repo /code/local/redis --type search

# 精确查找
./tools/cache_query /code/local/redis/symbols/zmalloc --type exact
```

---

### 2. ai_code_search.sh — AI Agent 统一搜索脚本

**定位**：对已索引的代码库进行查询和探索的统一入口

**路径**：`./ai_code_search.sh`

**核心思想**：100% C 语言实现核心引擎，Shell 脚本做统一命令封装，用户无需关心底层工具链。

**架构特点**：
- **用户代码层**：100% C（索引、搜索、分析、调用图、数据流）
- **推理引擎层**：C++ 库（ONNX Runtime + TensorRT 提供 GPU 加速）
- **脚本包装层**：Shell（一键命令封装）
- **模型训练层**：Python（一次性导出 ONNX，运行时不需 Python）

**子命令列表**：

| 命令 | 别名 | 功能 | 使用场景 |
|------|------|------|----------|
| `index` | `i` | 索引代码库 | 首次分析项目 |
| `vector` | `v` | 生成向量+调用图+数据流 | 索引后生成语义数据 |
| `search` | `s` | **语义搜索** | 用自然语言找代码 |
| `snippet` | `snip` | **代码片段搜索** | 找相似实现 |
| `dataflow` | `df` | **变量数据流追踪** | 追踪变量生命周期 |
| `cross-search` | `cross` / `xs` | **跨项目搜索** | 一次在所有已索引项目中搜索 |
| `callgraph` | `c` | 构建调用图 | 分析函数依赖 |
| `analyze` | `a` | **一键完整分析** | 快速开始 |
| `demo` | `d` | 演示系统能力 | 了解系统功能 |

#### 2.1 index — 索引代码库

```bash
./ai_code_search.sh index <repo_path> [cache_dir] [workers]
```

**示例**：
```bash
./ai_code_search.sh index /opt/stable-diffusion.cpp /opt/code_caches/sd_cache 4
```

**输出**：
- `chunks_text.txt` — 代码文本（用于向量生成）
- `chunks_meta.jsonl` — 元数据（用于搜索展示）

#### 2.2 vector — 生成语义向量

```bash
./ai_code_search.sh vector <cache_dir> [project_name]
```

**示例**：
```bash
./ai_code_search.sh vector /opt/code_caches/sd_cache stable-diffusion.cpp
```

**内部流程**：
1. 统计词频（TF-IDF 排序优化）
2. 生成语义向量（TensorRT GPU）
3. 构建调用关系图（含参数信息）
4. 生成变量数据流分析（字段级+跨函数）

**输出**：
- `vectors/*.jina.bin` — 语义向量
- `vectors/*.jina.idx` — 向量索引
- `call_graph.json` — 调用关系图
- `dataflow.json` — 变量数据流
- `word_freq.json` — 词频统计

#### 2.3 search — 语义搜索（核心功能）

```bash
./ai_code_search.sh search <cache_dir> "<query>" [max_results]
```

**示例**：
```bash
./ai_code_search.sh search /opt/code_caches/sd_cache "upscale image" 5
./ai_code_search.sh search /opt/code_caches/sd_cache "memory allocation buffer pool" 10
./ai_code_search.sh search /opt/code_caches/sd_cache "VAE encode latent" 5
```

**搜索技巧**：
1. 用英文自然语言描述想找的功能（如 "event loop" 而非 "事件循环"）
2. 组合关键词提高精度（如 "memory allocation buffer pool"）
3. 查看调用关系（--callgraph）理解函数在架构中的角色
4. 用 `--rich` 查看完整代码上下文
5. 用 `--kind function` 只搜索函数，过滤变量和宏

#### 2.4 snippet — 代码片段搜索

```bash
./ai_code_search.sh snippet <cache_dir> <code_file> [max_results]
```

**示例**：
```bash
./ai_code_search.sh snippet /opt/code_caches/sd_cache ./my_kernel.cpp 5
```

**原理**：将你的代码片段向量化，搜索代码库中最相似的实现。

**用途**：学习最佳实践、找到参考实现、发现更好的写法。

#### 2.5 dataflow — 变量数据流追踪

```bash
./ai_code_search.sh dataflow <cache_dir> <var_name>
```

**示例**：
```bash
# 追踪 connection 指针（含字段级和跨函数流）
./ai_code_search.sh dataflow /opt/code_caches/nginx_cache c

# 追踪内存池
./ai_code_search.sh dataflow /opt/code_caches/nginx_cache pool

# 追踪上下文指针
./ai_code_search.sh dataflow /opt/code_caches/sd_cache ctx
```

**输出示例**：
```
📌 DEFINITIONS: __inc_zone_page_state()
✏️  ASSIGNMENTS: __page_frag_cache_refill(), alloc_zpdesc()
👁️  USAGES: page_zone(), page_pgdat(), put_page_testzero()
🔍 FIELDS: page->lru, page->buddy_list, page->_mapcount
```

**功能**：
- **字段级追踪**：自动区分 `var->field`（如 `c->fd`, `c->data`）
- **跨函数流**：追踪变量在调用链中的传递路径
- **分类显示**：定义(DEF) / 赋值(SET) / 使用(USE)

#### 2.6 analyze — 一键完整分析

```bash
./ai_code_search.sh analyze <repo_path> [cache_dir]
```

**示例**：
```bash
./ai_code_search.sh analyze /opt/stable-diffusion.cpp /opt/code_caches/sd_cache
```

**流程**：index → vector → 输出使用提示

#### 2.7 demo — 演示系统能力

```bash
./ai_code_search.sh demo [cache_dir]
```

**示例**：
```bash
./ai_code_search.sh demo /opt/code_caches/sd_cache
```

**演示内容**：
1. 自然语言搜索（"upscale image"）
2. 架构理解（"text encoder embedding"）
3. 跨文件调用关系（"memory allocation buffer pool"）

---

### 3. ai_code_search_large.sh — 超大项目分治脚本

**定位**：处理 Linux 内核级别（5万+文件，100万+ chunks）的超大项目

**路径**：`./ai_code_search_large.sh`

**核心问题**：
- 直接处理 5 万文件 → 4 小时+ 向量生成，1.1GB 内存占用，可能 OOM
- 分治后：每个子系统独立处理，不会 OOM，可并行索引、串行向量化

**分治策略**：
1. **智能拆分**：自动分析项目结构，按文件数量拆分子系统
2. **大目录细分**：如 `drivers/` 按二级目录拆分为 `drivers_gpu`、`drivers_net`...
3. **小目录合并**：小目录合并到 `misc` 子系统
4. **排除非核心目录**：`testing/`、`Documentation/`、`samples/` 等

**子命令列表**：

| 命令 | 功能 | 使用场景 |
|------|------|----------|
| `init` | 初始化配置（智能分析并拆分子系统） | 首次处理超大项目 |
| `index` | 索引所有子系统 | 生成代码索引 |
| `vector` | 生成所有子系统向量 | 生成语义向量 |
| `search` | 全局搜索（聚合所有子系统） | 跨子系统搜索 |
| `search-sub` | 搜索指定子系统 | 精准搜索特定模块 |
| `dataflow` | 全局变量追踪 | 追踪变量在全局的范围 |
| `status` | 查看处理状态 | 查看进度 |

#### 3.1 init — 初始化配置

```bash
./ai_code_search_large.sh init <repo_path> [config_file]
```

**示例**：
```bash
./ai_code_search_large.sh init /opt/linux/src/linux-7.0.11
```

**自动检测结果**（Linux 内核示例）：
```
kernel/  → kernel_cache (630文件, 29,411 chunks)
mm/      → mm_cache (188文件, 11,405 chunks)
fs/      → fs_cache (2,160文件, 76,092 chunks)
net/     → net_cache (1,729文件, 66,571 chunks)
drivers/ → drivers_gpu, drivers_net 等 40+ 子系统
arch/    → arch_x86, arch_arm 等 17 子系统
排除：testing/, Documentation/, samples/ 等非核心目录
```

#### 3.2 index — 索引所有子系统

```bash
./ai_code_search_large.sh index [workers]
```

**示例**：
```bash
./ai_code_search_large.sh index 8
```

**特点**：
- 并行索引（每 4 个子系统一批，避免 fork 过多）
- 独立输出目录，互不干扰
- 自动跳过不存在的子系统

#### 3.3 vector — 生成所有子系统向量

```bash
./ai_code_search_large.sh vector [workers]
```

**示例**：
```bash
./ai_code_search_large.sh vector 1
```

**特点**：
- 串行生成（避免 GPU 显存 OOM）
- 自动跳过超过 chunk 数限制的子系统
- 同时生成调用图和数据流

#### 3.4 search — 全局搜索

```bash
./ai_code_search_large.sh search "<query>" [max_results]
```

**示例**：
```bash
./ai_code_search_large.sh search "schedule task" 10
./ai_code_search_large.sh search "page fault handler" 5
```

**特点**：
- 聚合所有子系统结果
- 自动去重
- 显示结果来源子系统

#### 3.5 search-sub — 子系统内搜索

```bash
./ai_code_search_large.sh search-sub <subsystem> "<query>" [max_results]
```

**示例**：
```bash
./ai_code_search_large.sh search-sub kernel "scheduler" 5
./ai_code_search_large.sh search-sub mm "slab allocator" 5
```

**特点**：更精准，只在指定子系统内搜索

#### 3.6 dataflow — 全局变量追踪

```bash
./ai_code_search_large.sh dataflow <var_name> [subsystem]
```

**示例**：
```bash
# 全局追踪
./ai_code_search_large.sh dataflow task_struct

# 指定子系统
./ai_code_search_large.sh dataflow task_struct kernel
```

**特点**：
- 全局搜索：在所有子系统中查找变量
- 自动跳过不包含该变量的子系统
- 显示子系统分隔线

#### 3.7 status — 查看处理状态

```bash
./ai_code_search_large.sh status
```

**输出示例**：
```
╔══════════════════════════════════════════════════════════════════╗
║ 项目: linux                                                      ║
║ 路径: /opt/linux/src/linux-7.0.11                                                 ║
╚══════════════════════════════════════════════════════════════════╝

子系统                      Chunks        向量         调用图
───────────────────────── ──────────── ──────────── ──────────
kernel                     29411        ✓            ✓
mm                         11405        ✓            ✓
fs                         76092        ✓            ✓
net                        66571        ✓            ✓
...
───────────────────────── ────────────
总计                       1469782

Cache 目录: ./linux_subsystems
```

---

## 代码探索方法论

### 以 nginx 为例的完整探索流程

**Phase 1: 索引与向量化**
```bash
./ai_code_search.sh index /opt/nginx /opt/code_caches/nginx_cache 4
./ai_code_search.sh vector /opt/code_caches/nginx_cache nginx
```

**Phase 2: 架构概览（自然语言搜索核心概念）**
```bash
./ai_code_search.sh search /opt/code_caches/nginx_cache "event loop epoll kqueue" 10
./ai_code_search.sh search /opt/code_caches/nginx_cache "HTTP request phase handler" 10
./ai_code_search.sh search /opt/code_caches/nginx_cache "master process worker process fork" 10
./ai_code_search.sh search /opt/code_caches/nginx_cache "memory pool palloc" 10
```

**Phase 3: 调用关系分析（理解架构依赖）**
```bash
./ai_code_search.sh search /opt/code_caches/nginx_cache "upstream load balancing" 10
# 关键发现：ngx_http_upstream_init_round_robin_peer 被多个算法调用
# 洞察：Round-robin 是所有负载均衡算法的基础

./ai_code_search.sh callgraph /opt/code_caches/nginx_cache
# 查看 call_graph.json，搜索某个函数的调用者列表
```

**Phase 4: 子系统深入（逐步细化）**
```bash
# 内存管理
./ai_code_search.sh search /opt/code_caches/nginx_cache "shared memory zone slab" 10
./ai_code_search.sh dataflow /opt/code_caches/nginx_cache c

# 配置解析
./ai_code_search.sh search /opt/code_caches/nginx_cache "configuration parser lexer" 10

# SSL/TLS
./ai_code_search.sh search /opt/code_caches/nginx_cache "SSL certificate handshake" 10

# 缓存系统
./ai_code_search.sh search /opt/code_caches/nginx_cache "file cache open read" 10
```

**Phase 5: 代码片段搜索（找到相似实现）**
```bash
./ai_code_search.sh snippet /opt/code_caches/nginx_cache ./my_epoll_code.cpp 5
```

**Phase 6: 变量数据流（追踪生命周期）**
```bash
./ai_code_search.sh dataflow /opt/code_caches/nginx_cache c
# 追踪 connection 指针的定义、赋值、使用位置
```

### 分析框架

对每个代码库，建议按以下框架分析：

1. **核心架构**（事件循环、进程模型、请求管线）
2. **内存管理**（分配器、内存池、垃圾回收）
3. **配置系统**（解析器、热加载、配置继承）
4. **模块系统**（初始化、加载、钩子机制）
5. **网络处理**（连接管理、协议实现、负载均衡）
6. **安全机制**（认证、加密、访问控制）
7. **缓存策略**（文件缓存、元数据缓存、缓存失效）
8. **日志系统**（分级日志、格式化、输出目标）
9. **高级特性**（重写引擎、正则表达式、新协议）
10. **源码质量评价**（架构清晰度、可扩展性、性能优化）

---

## 已验证的实战案例

### nginx（400 文件，9,174 符号）

使用本系统分析 nginx：

- ✅ **事件循环**：找到 epoll(Linux) + kqueue(BSD) 双平台实现
- ✅ **Phase Handler**：发现 11 阶段 HTTP 请求处理管线
- ✅ **内存池**：请求级别分配（create → palloc → destroy）
- ✅ **Slab Allocator**：共享内存用于 upstream 状态、rate limiting、SSL session
- ✅ **负载均衡**：Round-robin 是所有算法（least_conn/ip_hash/hash）的基础
- ✅ **配置解析**：词法分析 + 语法分析，支持 include 递归
- ✅ **多进程**：Master-Worker + Cache Manager，prefork 模型

### Linux 内核（58,373 文件，1,469,782 chunks）

使用分治策略分析 Linux 内核：

**内存管理子系统（mm/）**：
- 分治后：188 文件，11,405 chunks，2 分钟完成向量化
- 搜索 "slab allocator cache" → 找到 allocate_slab (0.9201)
- 搜索 "page allocation order zone" → 找到 prepare_alloc_pages (0.8928)
- 搜索 "memory compaction migrate" → 找到 migrate_vma_pages (0.8559)

**核心发现**：
```
内存分配三级层次：
  kmalloc/vmalloc (用户接口)
      ↓
  SLUB 分配器 (对象缓存：___slab_alloc → allocate_slab)
      ↓
  Buddy 系统 (物理页面：__alloc_pages_slowpath)

关键发现：
  - Buddy: order参数决定2^order个连续页面
  - SLUB: percpu cpu_slab → node partial → allocate_slab 三级缓存
  - OOM: __alloc_pages_may_oom → out_of_memory → oom_kill_process
  - 压缩: __alloc_pages_direct_compact → compact_zone
```

### AI Agent Linux 内核开发完整工作流

**场景**：修改 page allocation 失败时的调试信息

```bash
# 1. 理解代码（语义搜索）
./tools/vector_search ./linux_subsystems/mm_cache \
    "page allocation fail slowpath" 5 --rich
# → 找到 __alloc_pages_slowpath 是核心入口
# → 找到 prepare_alloc_pages 解析 gfp_mask
# → 找到 __alloc_pages_may_oom 触发 OOM

# 2. 分析影响范围（数据流 + 调用图）
./tools/dataflow show ./linux_subsystems/mm_cache gfp
# → gfp_mask 传递到 __alloc_pages_may_oom

./tools/vector_search ./linux_subsystems/mm_cache \
    "__alloc_pages_slowpath" 3 --callgraph
# → 被 alloc_pages, __get_free_pages 调用

# 3. 修改代码（编辑器）
vim /opt/linux/src/linux-7.0.11/mm/page_alloc.c
# 在 __alloc_pages_slowpath 中添加 printk 或 tracepoint

# 4. 编译内核
cd /opt/linux/src/linux-7.0.11
make oldconfig
make -j$(nproc)

# 5. 运行测试（QEMU）
qemu-system-x86_64 \
    -kernel arch/x86/boot/bzImage \
    -append "console=ttyS0 debug loglevel=8" \
    -serial stdio \
    -m 512M \
    -initrd rootfs.cpio.gz

# 6. 验证修改（搜索确认）
./tools/vector_search ./linux_subsystems/mm_cache \
    "your_new_debug_function" 5 --callgraph
# → 确认新函数被正确调用
```

---

## 数据存储与持久化

### KV Cache 存储结构

```
/memory/
├── cache.bin                    # mmap 数据文件（零拷贝持久化）
│   ├── 实时落盘：每次写入后 msync(MS_SYNC)
│   ├── 断电不丢：操作系统保证 mmap 数据落盘
│   └── 零拷贝：读取时直接从磁盘映射到内存
│
├── index.bin                    # 索引文件（Hash + Skip List）
│   ├── Hash 索引：O(1) 精确查找
│   ├── Skip List：O(log n) 范围查询
│   └── 自动加载：启动时从磁盘加载，无需重建
│
└── vectors/
    ├── code_local_{project}.jina.bin       # 二进制向量文件
    │   ├── 格式：每个向量 768 * 4 = 3072 字节（float32）
    │   └── 直接映射：可用于 GPU 推理
    │
    ├── code_local_{project}.jina.idx       # 偏移索引
    │   ├── 格式：名称 → (偏移, 维度) 映射
    │   └── 用途：快速定位向量位置
    │
    └── code_local_{project}.jina.bin.hnsw  # HNSW 近似索引
        ├── 算法：Hierarchical Navigable Small World
        ├── 复杂度：构建 O(n log n)，搜索 O(log n)
        └── 精度：>95% 召回率，毫秒级响应
```

### 持久化保证

- **写入即持久**：每次 `cache_set()` 后调用 `msync(MS_SYNC)`，数据立即落盘
- **断电安全**：即使系统崩溃，已写入的数据不会丢失
- **原子更新**：索引和数据文件同步更新，不会出现不一致状态
- **增量友好**：支持多次导入同一项目，更新已有条目

---

## 故障排查

### 常见问题

#### 1. 索引失败

**症状**：`code_indexer` 返回非零退出码

**解决**：
```bash
# 检查工具是否存在
ls -la tools/code_indexer

# 重新编译
make clean && make tools/code_indexer

# 检查源码目录权限
ls -la /opt/nginx
```

#### 2. 向量生成失败

**症状**：`batch_embedder` 报错或返回非零

**解决**：
```bash
# 检查 GPU 环境
nvidia-smi

# 检查 LD_LIBRARY_PATH
echo $LD_LIBRARY_PATH

# 检查模型文件
ls -la /opt/models/jina-embeddings-v2-base-code/

# 跳过向量生成（仅索引+调用图）
./analyze_repo.sh /opt/nginx --skip-vectors
```

#### 3. 搜索无结果

**症状**：语义搜索返回空结果

**解决**：
```bash
# 检查向量文件是否存在
ls -la {cache_dir}/vectors/*.hnsw

# 检查 chunks 是否生成
wc -l {cache_dir}/chunks_meta.jsonl

# 重新生成向量
./ai_code_search.sh vector {cache_dir}
```

#### 4. 调用图/数据流为空

**症状**：`call_graph.json` 或 `dataflow.json` 为空

**解决**：
```bash
# 检查 chunks_meta.jsonl 是否有内容
head -5 {cache_dir}/chunks_meta.jsonl

# 手动运行分析工具
./tools/call_graph {cache_dir}
./tools/dataflow analyze {cache_dir}
```

#### 5. 超大项目 OOM

**症状**：处理大项目时内存溢出

**解决**：
```bash
# 使用分治脚本
./ai_code_search_large.sh init /opt/linux/src/linux-7.0.11
./ai_code_search_large.sh index 4
./ai_code_search_large.sh vector 1  # 串行，避免 GPU OOM
```

---

## 与其他系统的对比

| 维度 | 本系统 | IDE 搜索 | grep/ack | Sourcegraph |
|------|--------|----------|----------|-------------|
| **搜索方式** | 自然语言语义 | 符号跳转 | 关键词正则 | 符号+正则 |
| **理解能力** | 概念级匹配 | 精确匹配 | 字面匹配 | 精确匹配 |
| **调用关系** | 自动构建 | 静态分析 | 无 | 静态分析 |
| **数据流** | 变量追踪 | 有限 | 无 | 有限 |
| **代码片段** | 相似度匹配 | 无 | 无 | 无 |
| **本地运行** | ✅ 完全本地 | ✅ | ✅ | ❌ 需服务端 |
| **隐私安全** | ✅ 不上传 | ✅ | ✅ | ❌ 上传代码 |
| **大项目** | ✅ 分治策略 | ⚠️ 慢 | ⚠️ 慢 | ✅ |
| **离线使用** | ✅ | ✅ | ✅ | ❌ |

---

## 查询类型能力对照表

系统提供 4 种核心查询类型，每种对应不同的底层机制和 AI Agent 使用场景：

| 查询类型 | 触发条件 | 底层引擎 | 需要向量？ | 响应速度 | 返回数据 | AI Agent 最适合用在哪 |
|---------|---------|---------|-----------|---------|---------|---------------------|
| `exact` | query 以 `/code/` 开头 | KV Cache 直接查 key | ❌ 不需要 | 微秒级 | 任意 key 的原始 JSON 值 | 确认索引是否完整、调试 |
| `symbol` | query 是单个标识符且有 `--repo` | KV Cache `/code/{repo}/symbols/` | ❌ 不需要 | 毫秒级 | 符号名/文件/行号/kind | 确认函数名是否正确、初步定位 |
| `context` | query 是单个标识符 + `--type context` | symbol + callers + callees + 递归调用链展开 + dataflow | ❌ 不需要 | 毫秒~秒级 | 完整上下文：符号元数据 + caller列表 + callee列表 + 调用路径树 + 变量数据流 | **理解一个函数在项目中的完整角色** |
| `search` | query 包含空格或 `--type search` | vector_engine (HNSW 近似搜索 + TF-IDF 关键词 boost + caller count boost) + 过滤 | ✅ 必须有 | 秒级 | 语义相似代码块 + name/file/line/kind/score + signature + content | **用自然语言描述功能找代码** |

### 4 种查询的 AI Agent 使用策略

```
 search（语义搜索）←─── 不知道具体函数名时的第一入口
    │  "找到相关函数名"
    ▼
 context（上下文查询）←─── 已知函数名后的深度分析
    │  "生成 caller/callee/调用链/数据流报告"
    ▼
 AI Agent 获得完整的函数角色理解 → 写代码/改代码
```

**典型链路举例**：
```
Agent 收到用户需求: "帮我优化 Nginx 的内存使用"
  → search "memory pool allocation"           # 找到相关函数
  → context ngx_palloc --repo /code/nginx --depth 2  # 深度分析
  → Agent 写出优化代码 + 标注参考来源
```

### 搜索过滤选项

所有 search/context 查询都支持以下过滤器：

| 选项 | 作用 | 示例 |
|------|------|------|
| `--kind function` | 只返回函数，过滤结构体/宏/变量 | `search "event loop" --kind function` |
| `--kind struct` | 只返回结构体定义 | `search "connection" --kind struct` |
| `--lang c` | 只搜索 C 语言代码 | `search "allocator" --lang c` |
| `--lang cpp` | 只搜索 C++ 代码 | `search "tensor" --lang cpp` |
| `--file cuda` | 只搜索文件名含 cuda 的文件 | `search "kernel" --file cuda` |
| `--no-boost` | 关闭 TF-IDF 和 caller count 加权 | 已知函数名时的精确搜索 |
| `--max-results N` | 控制返回数量（默认 10，最大 100） | `search "memory" --max-results 20` |
| `--depth N` | context 查询的调用链展开深度（0-5） | `context zmalloc --depth 3` |

### 常见查询技巧快查

| 你想做什么 | 应该用的查询 | 为什么 |
|-----------|------------|--------|
| 第一次接触一个新项目 | `search "main entry point"` | 找到 main() 或初始化入口 |
| 理解一个子系统的全貌 | `explore "connection handling"` | 自动语义搜索 + 深度分析 Top-1 函数 |
| 已知函数名想深入了解 | `symbol zmalloc --depth 2` | 完整的 caller/callee/调用链 |
| 写了一段代码想找参考 | `snippet my_code.c` | 语义相似度匹配找最接近的工业实现 |
| 对比两个项目的实现 | `compare /code/redis /code/nginx "memory pool"` | 同时搜索两个命名空间，对比输出 |
| 一次搜完所有项目 | `cross-search "slab allocator"` | 自动发现所有已索引项目，并行搜索聚合 |



## AI Agent 实战场景：16 个项目的知识网络

以下场景全部基于 16 个待探索项目的**组合效应**——项目越多，每个场景的输出越丰富。每个场景展示了 AI Agent 如何利用代码探索系统完成具体的编码任务。

---

### 场景 1：写一个内存池（跨项目参考）

**用户说**："帮我写一个内存池"

**AI Agent 执行**：
```bash
# 1. 跨项目搜索所有内存池实现
cross-search "memory pool allocator"
# → 返回:
#   Nginx    ngx_pool_t / ngx_palloc     score=0.92  [请求级别, 栈式分配]
#   Redis    zmalloc                      score=0.88  [简单封装 malloc]
#   CPython  PyMem_Malloc / arenas        score=0.85  [对象池 + arena]
#   Linux    kmem_cache / SLUB allocator  score=0.83  [per-CPU 三级缓存]
#   mruby    mrb_mem_pool / mrb_gc_alloc  score=0.80  [GC 配套分配]
#   Postgres MemoryContext / AllocSet     score=0.79  [树形内存上下文]

# 2. 对得分最高的实现做深度分析
context ngx_pool_t --repo /code/nginx --depth 2
# → 返回: 结构体定义 + 创建/分配/销毁函数 + 调用链路
```

**Agent 得到的知识**：
- Nginx 的 `ngx_pool_t` 是**栈式内存池**：create → 批量 palloc → destroy 一次性释放，零碎片
- Redis 的 `zmalloc` 是**malloc 封装**：加前缀存大小，支持 OOM handler、原子操作统计
- Linux 的 **SLUB** 是 per-CPU 对象缓存：三级缓存（cpu_slab → node partial → allocate_slab）减少锁竞争
- CPython 使用 **arena + 池化**：大块内存预先 mmap，小对象在池内分配，配合 GC 释放

**用户得到的输出**：Agent 综合各家优点，写出内存池代码，每行都带设计注释："参考 Nginx 一次性释放策略 + SLUB per-CPU 缓存思想 + zmalloc OOM handler"

---

### 场景 2："这个 epoll 循环写得对吗？"（代码审查）

**用户说**：贴了一段自定义的 epoll 事件循环代码

**AI Agent 执行**：
```bash
# 跨项目搜索相似模式
snippet /opt/code_caches/nginx_cache ./my_epoll.c 5
snippet /opt/code_caches/redis_cache ./my_epoll.c 5  
snippet /opt/code_caches/libuv_cache ./my_epoll.c 5
```

**Agent 发现的差异**：
| 检查项 | 用户代码 | Nginx | Redis | libuv |
|--------|---------|-------|-------|-------|
| EINTR 重入 | ❌ 未处理 | ✅ 检查 | ✅ 检查 | ✅ 检查 |
| ET 模式 + 非阻塞 | ⚠️ 设了但不完整 | ✅ 完整 | ✅ 完整 | ✅ 完整 |
| accept 限流 | ❌ 无限制 | ✅ 每次最多 8 个 | ✅ 每次最多 8 个 | ✅ 内置 backoff |
| EAGAIN 处理 | ❌ 缺少 | ✅ 重置 events | ✅ 继续等待 | ✅ 重新注册 |
| OOM 保护 | ❌ 无 | ✅ pool 分配失败回退 | ✅ zmalloc_oom | ✅ uv__malloc_oom |

**用户得到的输出**：Agent 标记 5 个潜在问题，每个附带工业级参考代码 + 来源标注 + 修复建议。

---

### 场景 3：设计字节码虚拟机（5 个运行时综合参考）

**用户说**："设计一个 Scheme 字节码 VM"

**AI Agent 执行**：
```bash
# 跨项目搜索字节码执行引擎
cross-search "bytecode interpreter main loop"
# → CPython:   _PyEval_EvalFrameDefault  [stack VM, 跳转表 dispatch]
# → LuaJIT:    lj_vm_asm_interpret       [register VM + trace JIT]
# → mruby:     mrb_vm_exec               [register VM, 直接线程化]
# → PHP:       zend_vm_execute            [多级 dispatch: 标签指针/跳转表]  
# → HotSpot:   TemplateInterpreter        [C++ 解释器 + C1/C2 JIT]

# 深入分析关键函数
context mrb_vm_exec --repo /code/mruby --depth 2
context _PyEval_EvalFrameDefault --repo /code/cpython --depth 2

# 搜索 GC 实现
cross-search "garbage collection mark sweep"
```

**Agent 自动生成的架构对比表**：
| 维度 | CPython | mruby | LuaJIT | HotSpot JVM | PHP |
|------|---------|-------|--------|-------------|-----|
| VM 类型 | Stack | Register | Register | Stack | Stack→Register |
| Dispatch | 跳转表 goto | 直接线程化 | 手写 asm | 模板解释器 | 标签指针 |
| 指令格式 | 变长(1-3B) | 定长(32位) | 定长(32位) | 变长 | 变长 |
| 局部变量 | stack slot | register R(i) | IR 变量 | local var table | CV 变量表 |
| GC | 引用计数+分代 | 标记-清除 | 增量 GC | G1/Parallel/ZGC | 引用计数 |
| JIT | 无 | 无 | Trace JIT | C1/C2 编译器 | 无 |
| 代码量 | ~500K 行 | ~20K 行 | ~70K 行 | ~2000K 行 | ~1000K 行 |

**用户得到的输出**：架构设计文档 + C 骨架代码 + 每项设计决策参考来源（"dispatch 方式参考 CPython 的跳转表，GC 参考 mruby 的标记-清除做简化"）。

---

### 场景 4：理解函数的完整角色（上下文分析）

**用户说**："我要改 ngx_event_accept，影响范围有多大？"

**AI Agent 执行**：
```bash
symbol ngx_event_accept --repo /code/nginx --depth 3
```

**返回的结构化数据**：
```
Symbol: ngx_event_accept  File: src/event/ngx_event_accept.c:30
  Kind: function  Signature: void ngx_event_accept(ngx_event_t *ev)

Callers (3):
  ngx_epoll_process_events → 从 epoll_wait 返回后调用
  ngx_kqueue_process_events → 从 kevent 返回后调用
  ngx_event_process_changes → 事件变化通知

Callees (8):
  ngx_get_connection → ngx_set_connection_log → ngx_ssl_create_connection
  → ngx_add_conn_to_event → ngx_http_init_connection → ...

Call Paths:
  epoll_wait → ngx_epoll_process_events → ngx_event_accept
    → ngx_get_connection → ngx_http_init_connection → HTTP 处理管线
  kevent → ngx_kqueue_process_events → ngx_event_accept → ...

Dataflow (变量: ls):
  Definition: ngx_listening_t *ls = ev->data
  Field usage: ls->addr_text, ls->handler, ls->fd, ls->pool
```

**Agent 的结论**：
- 该函数是 TCP accept 的核心入口，被 epoll 和 kqueue 两种事件驱动共用
- 修改它直接影响：所有新连接建立、SSL 握手、HTTP 初始化
- 影响范围涉及 3 个模块、8 个子函数、最终到 HTTP 请求处理管线
- 关键变量 `ls` 是全局监听套接字上下文，多 worker 间修改需互斥

---

### 场景 5：安全审计——追踪危险函数

**用户说**："检查项目里 unsafe 函数的使用情况"

**AI Agent 执行**：
```bash
cross-search "strcpy" --max-results 50 --kind function
cross-search "sprintf" --max-results 50
```

**Agent 发现**：
| 项目 | strcpy 用法 | 安全措施 | 推荐替代方案 |
|------|------------|---------|------------|
| Linux Kernel | ❌ 基本不用 | 用 `strscpy()` + GCC 沙箱检测 | `strscpy()` |
| Redis | ✅ 少量(命令行工具) | 调用前检查长度 | `sds` 动态字符串 |
| Nginx | ✅ 有使用 | 封装在 `ngx_sprintf` 内做长度控制 | `ngx_sprintf` |
| PostgreSQL | ❌ 禁止 | 静态分析强制检查 | `pg_strcpy` 封装 |
| SQLite | ❌ 几乎不用 | 使用 `sqlite3_snprintf` | `sqlite3_snprintf` |

**用户得到的输出**：安全审计报告，每个危险函数精确位置 + 调用路径 + 建议替代方案。SQLite 被标注为"最佳安全实践参考项目"。

---

### 场景 6：为 mydb 设计 B-tree 索引（数据库综合参考）

**用户说**："给我的 KV 存储加一个 B-tree 索引"

**AI Agent 执行**：
```bash
cross-search "b-tree node split insert" --kind function
context sqlite3BtreeInsert --repo /code/sqlite --depth 2
context btree_insert --repo /code/postgresql --depth 2
```

**Agent 生成的参考矩阵**：
| 特性 | SQLite | PostgreSQL | RocksDB |
|------|--------|------------|---------|
| B-tree 变体 | B+tree | B-tree | LSM-Tree |
| 页面管理 | 固定 4KB | 固定 8KB | 可变 block |
| 并发控制 | 单写者锁 | 行级锁+MVCC | 无锁 skip list |
| 分裂策略 | 提前分裂 | 先满再分裂 | compaction 合并 |
| 崩溃恢复 | journal/WAL | WAL | WAL |

**用户得到的输出**："建议参考 SQLite 的 B+tree（代码最清晰）做基础结构 + RocksDB 的 LSM 思想做写入优化"，附带关键数据结构的 C 代码骨架。

---

### 场景 7：跨项目 API 用法挖掘（libuv 最佳实践）

**用户说**："我想知道 libuv 在工业项目中怎么用的"

**AI Agent 执行**：
```bash
# 1. 找出哪些项目用了 libuv（搜索 libuv API 函数）
cross-search "uv_tcp_init uv_read_start uv_write uv_run"
# → 从 libuv 自身以及使用 libuv 的项目中返回

# 2. 统计 API 使用频率
search "uv_tcp_bind uv_listen" --kind function
```

**Agent 输出的统计**：
```
libuv API 使用热度排行（跨项目统计）:
1. uv_tcp_init        ―― 100% 项目使用 (必经初始化)
2. uv_read_start      ―― 100% 项目使用
3. uv_write           ―― 100% 项目使用
4. uv_tcp_bind        ―― 85% 项目使用
5. uv_close           ―― 100% 项目使用 (handle 生命周期管理)
6. uv_loop_init       ―― 70% 项目使用 (自定义 loop)
7. uv_timer_start     ―― 60% 项目使用 (超时管理)
8. uv_signal_start    ―― 30% 项目使用 (信号处理)
9. uv_getaddrinfo     ―― 20% 项目使用 (DNS 解析)
10. uv_poll_init      ―― 15% 项目使用 (自定义 fd 监听)
```

**常见用法模式**（Agent 自动提取）：
- Handle 生命周期：`uv_xxx_init → uv_xxx_start → ... → uv_close → free`（所有项目一致）
- Buffer 管理：`uv_buf_init` 栈上分配 vs 池化分配（不同项目有不同策略）
- 错误处理：所有项目都检查 `uv_*` 返回值，但详细程度不同

---

### 场景 8：实时 bug 模式挖掘——从已有项目中验证设计

**用户说**："这段加锁代码会不会死锁？"

**AI Agent 执行**：
```bash
# 在所有已索引项目中搜索嵌套锁的使用模式
cross-search "mutex lock nested" --kind function
cross-search "pthread_mutex_lock" --file ".c"
```

**Agent 分析**：对比用户代码与 Linux 内核、Nginx、PostgreSQL 中的锁使用模式，检查：
- 加锁顺序是否一致（所有加锁点按相同顺序？）
- 是否存在递归锁（`pthread_mutex_t` 默认不是递归的）
- 是否在信号处理函数中加锁（可能导致死锁）

**输出**：基于工业项目经验的死锁风险评估 + 具体使用建议。

---

### 场景 9：新项目架构风格匹配

**用户说**："我写的项目跟哪个已索引项目架构最像？"

**AI Agent 自动分析**新项目目录结构 + 命名风格 + 模块划分，与 16 个项目对比：

```
mydb 项目与各项目架构相似度:
┌────────────┬──────────┬──────────────────────────┐
│ 项目       │ 相似度   │ 原因                    │
├────────────┼──────────┼──────────────────────────┤
│ SQLite     │ 87%      │ 嵌入式、单进程、B-tree、C│
│ Redis      │ 72%      │ KV 存储、事件驱动       │
│ RocksDB    │ 68%      │ LSM-Tree、存储引擎      │
│ PostgreSQL │ 45%      │ 全功能 RDBMS、多进程    │
│ Nginx      │ 32%      │ 完全不同领域            │
│ Linux      │ 25%      │ 系统级、非应用          │
└────────────┴──────────┴──────────────────────────┘
```

**效果**：AI Agent 写代码时自动以 SQLite 为主要参考、Redis/RocksDB 为次要参考，不相关项目权重自动降低。

---

### 场景快查表

| 你在做的事 | 首选查询 | 最有参考价值的项目 |
|-----------|---------|------------------|
| 写内存分配器 | `cross-search "memory pool"` | Nginx, Linux, CPython, mruby |
| 设计事件循环 | `cross-search "event loop epoll"` | Nginx, Redis, libuv, HAProxy |
| 实现 GC | `cross-search "gc mark sweep"` | mruby, CPython, LuaJIT, HotSpot |
| 写 HTTP 解析器 | `search "parser state machine"` | Nginx, PHP |
| 设计字节码 VM | `cross-search "interpreter main loop"` | CPython, mruby, LuaJIT, HotSpot, PHP |
| 写 B-tree 索引 | `cross-search "btree insert"` | SQLite, PostgreSQL |
| 设计线程池 | `cross-search "thread pool worker"` | Linux, Nginx, libuv |
| 实现哈希表 | `cross-search "hash table"` | Redis(dict), CPython(PyDict), mruby(ht) |
| 检查安全风险 | `cross-search "strcpy sprintf"` | 全部项目（对比安全做法） |
| 理解一个函数 | `context <func> --depth 3` | 单个项目（调用链展开） |
| 写配置文件解析 | `cross-search "config parser"` | Nginx, Redis, PHP |
| 实现网络协议 | `cross-search "protocol parser"` | Nginx, Redis, HAProxy |
| 设计日志系统 | `cross-search "log write"` | Nginx, PostgreSQL, Linux |
| 实现模块系统 | `cross-search "module register"` | Nginx, HAProxy, LuaJIT |

---

> 提示：以上场景的丰富程度 = 已索引项目的数量。1 个项目只能单点搜索，16 个项目才能做交叉分析。
> 推荐探索顺序：mruby → SQLite → libuv → Redis → Nginx → CPython → LuaJIT → 其余项目。

---

## 踩坑记录 (实战：PHP 源码探索)

> 以下记录了探索 PHP 源码过程中遇到的全部问题及解决方案，包含完整可复现的命令。

### 1. ctags 未安装 → code_indexer 产出空文件

**现象**: code_indexer 运行后 `chunks_meta.jsonl` 和 `chunks_text.txt` 大小为 0 字节。

**原因**: code_indexer 依赖 ctags 提取函数签名。系统未安装 ctags 时，Worker 日志显示 `ctags failed: 32512`（命令未找到）。

**修复**:
```bash
sudo apt-get install -y universal-ctags
# 安装后清理重跑：
sudo rm -rf /opt/code_caches/php_cache
export LD_LIBRARY_PATH="/data/cuda/lib64:/opt/my_db:\${LD_LIBRARY_PATH}"
/opt/my_db/tools/code_indexer /opt/php/src /opt/code_caches/php_cache 6
```
结果: 2375 文件 → 61866 chunks，3.5s，680 files/s。

---

### 2. Jina 模型路径不匹配 → batch_embedder 加载失败

**现象**: batch_embedder 依次报错：
1. `Failed to open vocab.json`
2. `Failed to open merges.txt`
3. `Failed to create ONNX session`

**原因**: 硬编码路径为 `/opt/models/jina-embeddings-v2-base-code/model.onnx`，但实际模型存放在 `/opt/jina-embeddings-v2-base-code/onnx/model.onnx`。`vocab.json` 在模型根目录（不在 `onnx/` 子目录），`merges.txt` 不存在（Jina 的 merges 内嵌在 `tokenizer.json` 中）。

**修复**:
```bash
# 创建正确的目录结构（硬编码路径是 /opt/models/...）
sudo rm -rf /opt/models/jina-embeddings-v2-base-code
sudo mkdir -p /opt/models/jina-embeddings-v2-base-code
sudo ln -s /opt/jina-embeddings-v2-base-code/onnx/model.onnx \
           /opt/models/jina-embeddings-v2-base-code/model.onnx
sudo ln -s /opt/jina-embeddings-v2-base-code/vocab.json \
           /opt/models/jina-embeddings-v2-base-code/vocab.json
# 从 tokenizer.json 中提取 merges.txt
python3 -c \"
import json
with open('/opt/jina-embeddings-v2-base-code/tokenizer.json') as f:
    data = json.load(f)
merges = data['model'].get('merges', [])
with open('/opt/jina-embeddings-v2-base-code/merges.txt', 'w') as f:
    f.write('#version: 0.2\\n')
    for m in merges:
        f.write((m if isinstance(m, str) else ' '.join(m)) + '\\n')
\"
sudo ln -s /opt/jina-embeddings-v2-base-code/merges.txt \
           /opt/models/jina-embeddings-v2-base-code/merges.txt
```

---

### 3. TensorRT 加速（曾回退 CUDA，已修复）

**历史现象**: `[WARN] TensorRT V2 config failed, falling back to legacy API` + `Using CUDA GPU acceleration`。

**原因**: TRT V2 profile 曾写成固定 `1x512..32x512`，变长输入 `setInputShape` 失败。已改为**动态 profile** `1x64..32x512`，
配合「按 batch 实际长度 padding + 长度分桶」，TensorRT 现已真正生效（详见文首「运行依赖」）。

**影响 / 现状**: TensorRT 真在加速——linux 内核 693k chunks 从 82 分钟 → **~17 分钟（684 items/s）**。
若 TensorRT 缺失则回落 CUDA EP（可用但慢）。改 profile 后需删 `$HOME/trt_cache` 重建 engine。

---

### 4. HNSW 索引未自动构建 → 语义搜索返回空

**现象**: `batch_embedder` 运行完成后，`cache_query --type search` 返回 0 结果，日志显示 `Vector engine loaded` 但无匹配。

**原因**: batch_embedder 在结束时 fork 子进程调用 `build_hnsw_index` 构建近似最近邻索引，但子进程 `execl` 失败（编译产物不存在），日志: `[HNSW] Failed to start index builder`。未找到 `.hnsw` 文件的 source 会被 `search_hnsw()` 跳过（line 673: `if (!source->hnsw_file[0]) return 0;`）。

**修复**:
```bash
# 编译 HNSW 索引构建工具
cd /opt/my_db && sudo make tools/build_hnsw_index

# 为已生成向量构建 HNSW 索引
export LD_LIBRARY_PATH="/opt/my_db:\${LD_LIBRARY_PATH}"
./tools/build_hnsw_index \
  /opt/code_caches/php_cache/vectors/code_local_php.jina.bin
```
输出: 61866 vectors, 768 dims, 索引保存至 `code_local_php.jina.bin.hnsw` (198 MB)。

---

### 5. dataflow 段错误（Makefile 全局 `-shared` 污染）

**现象**: `./tools/dataflow analyze /opt/code_caches/php_cache` → `段错误（核心已转储）`

**原因**: Makefile 第 3 行 `LDFLAGS = -shared` 是全局变量。`dataflow` 没有显式构建规则，GCC 隐式规则将 `$(LDFLAGS)` 传入链接步骤，误将 dataflow 编译为**共享库**（.so）而非可执行文件，导致运行时入口点初始化错误 → 段错误。

```makefile
# Makefile L3 — 错误根源
LDFLAGS = -shared    # ← 全局，用于 libmydb.so 是正确的，但污染了所有无规则的工具
```

**修复**: 在 Makefile 中为 dataflow 添加显式构建规则，避免使用全局 `LDFLAGS`：

```makefile
TOOLS_DATAFLOW = $(TOOLS_DIR)/dataflow
$(TOOLS_DATAFLOW): $(TOOLS_DIR)/dataflow.c
	$(CC) $(CFLAGS) -o $@ $< -lm -ljansson -L. -lmydb -Wl,-rpath,. -lpthread
```

然后重新编译 & 运行：
```bash
cd /opt/my_db && sudo make tools/dataflow

export LD_LIBRARY_PATH="/opt/my_db:\${LD_LIBRARY_PATH}"
./tools/dataflow analyze /opt/code_caches/php_cache
```
结果: 40127 个函数 → 2000 个唯一变量 → 14MB dataflow.json，无段错误。

**验证**: 编译产物应为 PIE executable 而非 shared library：
```bash
file tools/dataflow
# → ELF 64-bit LSB pie executable ... (正确)
#   而非 ELF 64-bit LSB shared object ... (错误，会 segfault)
```

> **影响其他工具**: 检查了所有有显式规则的工具（code_indexer, cache_query, cache_import 等），它们都指定了 `$(CC) $(CFLAGS) -o $@ ...` 不使用 `$(LDFLAGS)`，因此不受影响。只有靠隐式规则fallthrough的工具会中招。

---

### 6. `/memory` 目录权限 → cache_import 无法打开缓存

**现象**: `cache_import` 报错 `[ERROR] Failed to open cache: /memory`

**原因**: cache_import 调用 `cache_open("/memory", 2GB)` 创建 `/memory/cache.bin`，但 `/memory` 目录属于 root，普通用户无写权限。

**修复**:
```bash
sudo chown \$(whoami) /memory
```

---

### 7. 输出缓冲 → cache_query / cache_import 无输出

**现象**: 运行 `cache_import` 或 `cache_query` 时长时间无输出，实际进程在运行但 stdout 被缓冲。

**原因**: C `printf` 默认行缓冲，管道/重定向时变为全缓冲。`log_info()` 使用 `printf` 后未 `fflush`。

**修复**: 使用 `stdbuf` 命令强制行缓冲：
```bash
export LD_LIBRARY_PATH="/opt/my_db:\${LD_LIBRARY_PATH}"
stdbuf -oL ./tools/cache_import /opt/code_caches/php_cache /code/php --cache-dir /memory
```

---

### 8. `[CACHE]` 日志输出到 stdout → 破坏 JSON 管道解析

**现象**: `cross_search.sh` 捕获 `cache_query` 输出后 JSON 解析失败，始终返回 "无结果"。

**原因**: `libmydb.so` 中的 `cache_open()` 使用 `printf` 输出 `[CACHE] ...` 信息（到 stdout），而非 `fprintf(stderr, ...)`。`cross_search.sh` 中 `output=\$("\$CACHE_QUERY" ...)` 捕获 stdout 后直接管道给 `json.load(sys.stdin)`，非 JSON 的首行导致解析失败。

**修复**（两处修改）:
```bash
# 修改 1: cross_search.sh 中过滤非 JSON 行，只将 { 开头的行传给 json.load
#        并移除 --pretty 标志（避免换行格式使单行 JSON 变多行）
#        --pretty  →  删除
#        管道的 Python 脚本改为 for line in sys.stdin: if line.startswith('{'): json.loads(line); break

# 修改 2: 核心修复建议（后续在 cache_query.c 中实施）
#         将 main 函数开头的 stdout 缓存行改为 stderr：
#         fprintf(stderr, "[CACHE] ...") 而非 printf("[CACHE] ...")
#         当前 libmydb.so 中的 printf 无法轻易修改，所以先改 shell 包装层
```

---

### 9. 重要：每次运行前需设置的环境变量

以下是最小必需的环境变量导出（脚本已自动处理，手动调试时需要）：

```bash
# C 工具链链接 ONNX Runtime + libmydb.so
export LD_LIBRARY_PATH="/opt/my_db:/data/cuda/lib64:\${LD_LIBRARY_PATH}"

# ONNX Runtime 的 CUDA 提供者库（在 ORT GPU 安装目录）
export LD_LIBRARY_PATH="/data/venv/onnxruntime-linux-x64-gpu-1.20.1/lib:\${LD_LIBRARY_PATH}"

# 一次设置永久生效（追加到 ~/.bashrc 或 ~/.profile）
cat >> ~/.bashrc << 'EOF'
# my_db 代码探索系统
export LD_LIBRARY_PATH="/opt/my_db:/data/cuda/lib64:/data/venv/onnxruntime-linux-x64-gpu-1.20.1/lib:\${LD_LIBRARY_PATH}"
EOF
```

> **提示**: `ai_code_search.sh`、`analyze_repo.sh`、`cross_search.sh` 已自动处理 GPU 库路径。仅在使用 `cache_query`、`cache_import`、`batch_embedder` 等工具**单独调试**时需要手动 export。脚本内部通过 `export LD_LIBRARY_PATH=...` 拼接了 CUDA、TensorRT、cuDNN、ONNX Runtime 路径。

---

### 10. cache_import 耗时过长 & 增量导入优化

**现象**: PHP（61866 chunks, 24212 函数）的 `cache_import` 耗时 > 30 分钟。每次重新导入需要全量重处理。

**原因分析**:
1. **`SYNC_INTERVAL=5000` 过大**: 每 5000 个 key 执行一次 `cache_sync()`（全量序列化 hash 表到 `index.bin`）。随着 cache 增大（从 0 到 80000+ keys），每次 sync 耗时从几毫秒增长到数秒，共需 ~16 次 sync。
2. **无阶段跳过**: 即使只更新了 dataflow，也必须重跑 chunks（27 分钟）和 callgraph（2 分钟）。
3. **速度衰减**: `cache_set` 在大 cache 上插入速度下降 16 倍（0.4ms/key → 6.6ms/key）。

**优化措施**:

| 优化 | 之前 | 之后 | 效果 |
|------|------|------|------|
| SYNC_INTERVAL | 5000 | 25000 | sync 次数从 ~16 降为 ~3 |
| `--skip-chunks` | ❌ 无 | ✅ 新增 | 跳过 27 分钟 chunks 阶段 |
| `--skip-callgraph` | ❌ 无 | ✅ 新增 | 跳过 2 分钟 callgraph 阶段 |
| `--skip-dataflow` | ❌ 无 | ✅ 新增 | 跳过 8 分钟 dataflow 阶段 |
| `--only-dataflow` | 已有 | 增强 | 等价于 `--skip-chunks --skip-callgraph` |
| 进度 ETA | ❌ 无 | ✅ 有 | 每阶段显示完成百分比 + 预估剩余时间 |
| 同步耗时统计 | ❌ 无 | ✅ 有 | `Synced at N keys (elapsed Ts, N keys/s)` |

**推荐策略**:
```bash
# 场景 A: 首次全量导入
export LD_LIBRARY_PATH="/opt/my_db:\${LD_LIBRARY_PATH}"
stdbuf -oL ./tools/cache_import /opt/code_caches/php_cache /code/php --cache-dir /memory

# 场景 B: dataflow 增量导入（分析完成后单独导入）
stdbuf -oL ./tools/cache_import /opt/code_caches/php_cache /code/php --cache-dir /memory --only-dataflow

# 场景 C: 仅注册/检查缓存（什么都不做，快速验证）
stdbuf -oL ./tools/cache_import /opt/code_caches/php_cache /code/php --cache-dir /memory --skip-chunks --skip-callgraph --skip-dataflow

# 场景 D: 只导入 callgraph + dataflow（跳过 chunks）
stdbuf -oL ./tools/cache_import /opt/code_caches/php_cache /code/php --cache-dir /memory --skip-chunks
```

> 注意: 缓存文件 `/memory/cache.bin` 和 `/memory/index.bin` 不会因重复导入而损坏（`cache_set` 覆盖已有 key）。如要重置，需手动删除这两文件。阶段跳过标志适合只更新了部分数据时的增量导入。

---


## 文件清单

| 文件 | 说明 |
|------|------|
| `analyze_repo.sh` | 代码分析主控脚本（6 步流水线） |
| `ai_code_search.sh` | AI Agent 统一搜索脚本 |
| `ai_code_search_large.sh` | 超大项目分治脚本 |
| `tools/code_indexer` | C 多进程代码索引器 |
| `tools/batch_embedder` | C 批量向量生成器 |
| `tools/vector_search` | C 语义搜索引擎 |
| `tools/cross_search.sh` | Shell 跨项目搜索聚合脚本（自动发现 + 并行搜索 + 去重排序） |
| `tools/call_graph` | C 调用关系分析器 |
| `tools/dataflow` | C 变量数据流追踪器 |
| `tools/word_freq` | C 词频统计器 |
| `tools/cache_import` | C KV Cache 导入工具 |
| `tools/cache_query` | C KV Cache 查询工具 |
| `/opt/models/jina-embeddings-v2-base-code/` | Jina v2 嵌入模型 |
| `/memory/` | KV Cache 数据目录 |

---

## 总结

这套系统本质上是一个**代码的「第二大脑」**：

1. **记忆**：你把代码丢进去，系统帮你建立语义记忆（向量索引 + 调用图 + 数据流）
2. **持久**：mmap 持久化保证知识永不丢失，断电也不怕
3. **回忆**：用自然语言提问，系统帮你「回忆」起相关的实现细节、调用关系和数据流路径
4. **理解**：不仅是找到代码，还能理解代码在架构中的角色、变量生命周期、影响范围

相比传统工具（grep、IDE 搜索），这套系统的核心优势是**语义理解能力**——你不需要记住函数名，只需要描述想找的功能，系统就能理解你的意图，返回最相关的代码。

**一句话描述**：这是你的私人代码知识库，一次索引，终身受用。

---

*文档版本：2026-06-11 v2*（系统级修复 9 项 + 对齐 ebook 导入 cache 大小 + tokenizer 线程安全：protected_regular / GPU扩展名 / batch_embedder流式 / chunks导入5000x / dataflow导入15x / Makefile污染 / call_graph哈希 / exclude-dir参数 / cache mmap扩容）
*适用于：analyze_repo.sh + ai_code_search.sh + ai_code_search_large.sh 最新版本*
---

## 系统级修复与性能优化记录

### TypeScript/JavaScript 索引增强（插件模式）

coding.md 的第一个专用语言索引插件。

Node.js/npm 是世界上最大的开源代码生态之一。当前 `code_indexer` 基于 universal-ctags，对 C/C++/Python 效果很好，但对 TypeScript/JavaScript 的现代语法（箭头函数、Effect-TS 工厂函数、ES Module 导入导出、JSX 组件等）解析不够深入。

详见：[opencode/typescript-indexer.md](opencode/typescript-indexer.md)

核心设计：

- **插件接口**：`code_indexer` 通过统一接口调用外部插件，插件输出 JSON Lines 格式的 chunk / call_edge / import_edge / metadata
- **插件注册**：通过 `plugin.json` 注册扩展名匹配、命令、优先级
- **失败回退**：插件失败时自动回退到 ctags
- **TS 插件内部**：使用 tree-sitter C API 解析 `.ts/.tsx/.js/.jsx`，不依赖 Node.js / npm。新增 `ts_function`、`ts_method`、`ts_class`、`ts_interface`、`ts_type_alias`、`ts_enum`、`ts_variable`、`ts_property`、`ts_import`、`ts_export`、`ts_decorator`、`ts_namespace`、`ts_jsx_component` 等符号类型
- **噪音过滤**：跳过/降权 `.d.ts`、测试文件、类型定义；减少匿名 lambda 重复；修正 `export const` 导出标记；静态链接 tree-sitter 核心以简化部署
- **构建位置**：`/opt/my_db/plugins/typescript-indexer/`，`make` 后生成 `bin/typescript-indexer`

这是 coding.md 第一次为特定语言做索引增强，插件模式保证未来 Rust、Go、Java 等语言可以按同样方式扩展。

## 系统级修复与性能优化记录

> 以下记录了在探索 OpenResty、CPython、llama.cpp、Linux Kernel 过程中发现并修复的系统性问题。与 PHP 踩坑不同，这些是基础工具链的架构缺陷，影响所有项目。

### 1. `protected_regular=2` 导致 Worker 静默失败

**现象**: 以 root 运行 `code_indexer` 时，6/8 的 worker 报 `Failed to create chunk file`，但主进程显示 `8/8 workers completed`。索引覆盖率从 1311 文件降到 327 文件，50% 数据静默丢失。

**原因**: Linux 内核 `fs.protected_regular=2`（Ubuntu 默认值）阻止 root 在 `/tmp`（sticky 目录）中截断其他用户拥有的文件。之前的 PHP 分析（以 `quqiufeng` 用户运行）在 `/tmp/` 留下了 `code_indexer_chunks_0~5.jsonl`。再次运行（root）时 `fopen("w")` 被内核拒绝。

**修复**（2 处）：

```c
// 1) 在 fopen() 前先 unlink() 删除旧文件
unlink(chunk_file);
FILE* chunk_fp = fopen(chunk_file, "w");

// 2) worker_process() 返回 int，子进程 exit(ret) 传播错误码
//    父进程检测到失败后打印详细错误
int ret = worker_process(&workers[i], i);
exit(ret);
```

```bash
# 验证修复
cat /proc/sys/fs/protected_regular  # 应为 2（启用状态）
```

**影响范围**: 所有以 root 运行 `analyze_repo.sh` 的场景。修复后 OpenResty 从 5146 chunks → 23825（4.6x）。

---

### 2. 缺失 GPU/CUDA 扩展名

**现象**: llama.cpp 的 `ggml-cuda.cu`（CUDA 后端核心，2000+ 行）完全没有被索引。`is_source_file()` 返回 0。

**原因**: `code_indexer.c` 的 `is_source_file()` 函数硬编码了文件扩展名白名单，缺少 `.cu`、`.cuh`、`.metal`。

**修复**：

```c
// 增加 3 个扩展名
strcmp(ext, ".cu") == 0 ||      // CUDA 源文件
strcmp(ext, ".cuh") == 0 ||     // CUDA 头文件
strcmp(ext, ".metal") == 0 ||   // Apple Metal shader
```

**影响**: llama.cpp 扫描文件从 783 → 1042，`ggml-cuda.cu` 从 0 chunks 恢复到 231 chunks。

---

### 3. batch_embedder 内存爆炸（流式处理 + 动态分配）

**现象**: `item_t` 结构体使用固定 8KB 缓冲区（`char text[8192]`）。对于 Linux 内核规模（1.5M chunks），1.5M × 8KB = 12.7GB 仅用于文本存储，加上向量 4.6GB，总计 ~17.3GB，超出系统可用内存。

**原因**: 原始设计用 `char text[MAX_TEXT_LEN]` 固定数组，没有考虑大规模项目的内存累积。

**修复**：

```c
// 旧: 固定大小，每个 item 8448 字节
typedef struct { char name[256]; char text[8192]; } item_t;

// 新: 动态分配，实际使用多少就占多少
typedef struct { char* name; char* text; } item_t;
```

同时改为**流式批处理**：每批加载 256 条 → encode → 写入文件 → 释放内存 → 下一批。不再一次性加载全部数据。

**效果**: 1.5M items 从 17.3GB → ~2GB 峰值，内存降低 86%。

---

### 4. KV Cache 导入性能优化（chunks 阶段 5000x）

**现象**: chunks 导入时，每条 JSON 行先 `json_loads()` 解析，然后在 `import_chunk()` 中 `json_dumps()` 重新序列化。对于 509K 条记录，这是 100 万次 JSON 序列化/反序列化。

**原因**: `import_chunk()` 接受 `json_t*` 对象，函数内部重新构建 JSON 并重新序列化。而输入数据本来就是 JSON 字符串。

**修复**: 新建 `import_chunk_raw()` 函数，使用简单的 `strstr` 从原始 JSON 行中提取 name 和 filepath，直接存储整行 JSON：

```c
static int import_chunk_raw(const char* namespace, const char* line) {
    // 从原始 JSON 行中提取 name 和 filepath
    const char* name_p = strstr(line, "\"name\":\"");
    const char* file_p = strstr(line, "\"file\":\"");
    // ... 字符串操作，无需 JSON 解析 ...
    return safe_set_json(key, line);  // 直接存原始行
}
```

同时去掉内存中的 `symbol_map` 构建（每个 symbol 额外一次 `cache_set`），改为查询时按需构建。

**效果**:

| 项目 | 之前 | 之后 |
|------|------|------|
| Linux 509K chunks | ~100 keys/s, 预估 1.5h | **~500,000 keys/s, <1s** |
| CPython 67K chunks | ~100 keys/s, ~11min | **瞬时完成** |

---

### 5. KV Cache 导入性能优化（dataflow 阶段 15-20x）

**现象**: dataflow 导入速度仅 1-2 keys/s。CPython 2000 变量需要 ~30 分钟。

**原因**: 每个变量导入时，额外为每个出现该变量的函数单独做一次 `cache_set`（内循环）。2000 个变量 × ~5 个函数 = ~12000 次 `cache_set` 调用。

**修复**: 删除 per-variable 的函数分组内循环。所有变量存储后，单次遍历构建完整的函数→变量索引，一次 `cache_set` 写入。

```c
// 旧: 2000 vars × (1 + ~5 funcs) = ~12000 cache_set 调用
// 新: 2000 vars × 1 + 1 bulk entry = ~2001 cache_set 调用
```

**效果**: CPython dataflow 从 30 分钟 → 91 秒。

---

### 6. 调用图 O(n²) 算法与硬编码上限

**现象**: `call_graph` 工具在以下场景均失败：
- 以共享库（.so）而非可执行文件被编译 → 段错误
- `MAX_FUNC=50000`，Linux 内核 300K+ 函数 → 截断 83% 的数据
- `add_func_name()` / `is_func_known()` 使用线性扫描，O(n²) → 300K 函数 × 300K 扫描 = 900 亿次 strcmp

**原因**: 
1. Makefile 中 `LDFLAGS = -shared` 是全局变量，污染了没有显式规则的工具
2. `call_graph` 没有显式 Makefile 规则，GCC 隐式规则应用了 `-shared`
3. 函数名去重和查找都使用线性搜索

**修复**：

```makefile
# Makefile: 添加显式编译规则，避免 LDFLAGS 污染
$(CALL_GRAPH): $(TOOLS_DIR)/call_graph.c
	$(CC) $(CFLAGS) -o $@ $< -lm -ljansson -L. -lmydb -Wl,-rpath,.
```

```c
// call_graph.c: 哈希表去重，O(1) 查找
#define HASH_SIZE 524287
static int g_name_hash[HASH_SIZE];  // -1 = empty

static unsigned int hash_str(const char* s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % HASH_SIZE;
}

static void add_func_name(const char* name) {
    unsigned int h = hash_str(name);
    while (g_name_hash[h] >= 0) {
        if (strcmp(g_func_names[g_name_hash[h]], name) == 0) return;
        h = (h + 1) % HASH_SIZE;
    }
    g_func_names[g_func_count] = strdup(name);
    g_name_hash[h] = g_func_count;
    g_func_count++;
}
```

额外优化：Phase 1（加载函数名）和 Phase 2（加载函数体）合并为单遍文件扫描，消除一次 300MB 的冗余文件读取。

**效果**: 从段错误/数据截断 → 198K 函数 / 250K 体 / 12 分钟完成。

---

### 7. Makefile 全局 LDFLAGS=-shared 污染

**现象**: 多个工具（`dataflow`, `call_graph`, `build_hnsw_index` 等）被编译为共享库（.so）而非可执行文件，运行时段错误。

**原因**: Makefile 第 3 行 `LDFLAGS = -shared` 用于构建 `libmydb.so` 是正确的，但它是一个全局 `make` 变量。任何没有显式编译规则的目标都会通过隐式规则继承 `$(LDFLAGS)`，导致 `-shared` 传入链接步骤。

**检查方法**:

```bash
file tools/call_graph
# 正确: ELF 64-bit LSB pie executable
# 错误: ELF 64-bit LSB shared object  ← 段错误的根源
```

**修复**: 为所有受影响的目标添加显式 Makefile 编译规则，指定 `$(CFLAGS)` 而不是 `$(LDFLAGS)`。

**受影响的工具清单**（已全部修复）:

| 工具 | 修复方式 |
|------|---------|
| `dataflow` | 显式构建规则 |
| `call_graph` | 显式构建规则 |

---

### 8. `--exclude-dir` 运行时目录排除

**现象**: `drivers/` 被硬编码加入 `is_noise_dir()` 列表以跳过 Linux 内核驱动。但这污染了通用代码，其他项目可能真的有 drivers 目录需要索引。

**原因**: 代码索引器的目录排除列表是编译期硬编码的。

**修复**: 添加 `--exclude-dir` 运行时参数：

```bash
# 用法: 排除多个目录（逗号分隔）
./analyze_repo.sh /path --exclude-dir drivers,Documentation
# 等价于 code_indexer 直接调用
./tools/code_indexer /path /cache 8 --exclude-dir drivers,Documentation
```

实现方式：在 `main()` 中解析 `--exclude-dir` 参数，将指定目录存入全局列表，`scan_directory()` 扫描时额外检查。

---

### 9. cache.bin mmap 大小不足

**现象**: `cache.bin` mmap 固定为 2GB，对于 Linux 内核规模（~1.5M 条目）可能不足，强制 LRU 淘汰旧数据。

**修复**: 在 `cache_import.c` 中将 mmap 大小调整为 4GB：

```c
g_cache = cache_open(cache_dir, 4ULL * 1024 * 1024 * 1024);
```

> **对齐**: 同一修复已同步应用到电子书导入工具 `import_book.c`（从 500MB→4GB），确保多本大书共存时不触发 LRU 淘汰。详见 [ebook.md](ebook.md) 问题 15。

---

### 10. batch_embedder 批量超过 TensorRT profile → 零向量（搜索静默失效）

**现象**: 重建 OpenResty 后 `search` 分数全在 0.02–0.05，命中函数完全不相关，但命令返回成功、无报错。

**原因**: `onnx_embedder` 的 TensorRT V2 优化 profile 为 `input_ids/attention_mask: 1x512..32x512`；
`tools/batch_embedder.c` 的 `#define BATCH_SIZE 2048` 使每批 `setInputShape` 失败。
旧失败分支 `memset(vecbuf, 0, ...)` 会把整批写成**零向量**，且仍追加进 `.bin`。

**修复**（`tools/batch_embedder.c` + `ai_code_search.sh` + `tools/vector_search.c`）:
- `BATCH_SIZE 2048 → 32`（≤ profile 上限 32）
- 编码失败**不再写零向量**：跳过该批、累计失败数，结束时**非零退出**
- HNSW 由后台 fork 改为**同步** `waitpid`，失败视为致命
- `ai_code_search.sh vector` 增加 **Step 5/5**：校验 `.hnsw` 新于 `.bin`，缺失/过旧自动重建；embedder 非零退出即中止
- `vector_search` 对 `top1 < 0.30` 输出**低置信度告警**（提示索引可能损坏）
- **TRT profile 改动态** `1x64..32x512` + 「按 batch 实际长度 padding + 长度分桶」→
  内核 693k chunks 从 82 分钟 → **~17 分钟（684 items/s，~4.9x）**（见文首「运行依赖」）

---

### 修复效果汇总

| # | 修复点 | 影响项目 | 之前 | 之后 |
|---|--------|---------|------|------|
| 1 | protected_regular + exit code | OpenResty 首次 | 丢 50% 数据 | 8/8 worker 成功 |
| 2 | 扩展名白名单 | llama.cpp | GPU 代码 0 chunks | 全量索引 |
| 3 | batch_embedder 内存 | Linux 内核 | 17.3GB → OOM | ~2GB 流式 |
| 4 | chunks 导入 json 循环 | Linux 内核 | ~100 keys/s | ~500,000 keys/s |
| 5 | dataflow 内循环 | CPython | 1-2 keys/s | 21-25 keys/s |
| 6 | call_graph O(n²) + 上限 | Linux 内核 | 段错误/截断 | 198K 函数正常 |
| 7 | Makefile -shared 污染 | 多个工具 | 段错误 | 正常执行 |
| 8 | 目录排除 | Linux 内核 | 硬编码 | 运行时参数 |
| 9 | cache mmap 大小 | Linux 内核 | 2GB | 4GB |
| 10 | batch_embedder 批量 > TRT profile | OpenResty 重建 | 零向量(搜索~0.05) | BATCH=16 + 失败非零退出 + HNSW 自动验收 |

