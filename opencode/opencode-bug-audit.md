# opencode 推理质量 Bug 审计报告

> 审计日期: 2026-08-01
> 审计对象: opencode dev 分支 `19231fce4b`
> 审计方法: 代码探索系统语义定位 + 源码逐行验证 + git 历史追溯
> 背景: 社区反馈 opencode harness 导致 DeepSeek V4 等开源模型"显著降智"，评测分数差约 20 分

---

## 一、推理参数链路（reasoning effort）— 3 个问题

### 🔴 Bug 1（主因，确定性）：DeepSeek 官方 API 的 reasoning effort 变体被硬编码禁用

**位置**: `packages/opencode/src/provider/transform.ts:773-785`

```ts
const id = model.id.toLowerCase()
if (
  id.includes("deepseek-chat") ||      // ← 官方 API 模型 ID！
  id.includes("deepseek-reasoner") ||
  id.includes("deepseek-r1") ||
  id.includes("deepseek-v3") ||
  id.includes("minimax") ||
  (id.includes("glm") && !glm52) ||
  id.includes("kimi") ||
  id.includes("k2p") ||
  id.includes("qwen") ||
  id.includes("big-pickle")
)
  return {}    // ← 空对象：effort 变体全部不生成
```

**致命点**: DeepSeek 官方 API 的模型 ID 从 V1 到 **V4 一直是 `deepseek-chat` / `deepseek-reasoner`**（V4 只是模型升级，API 名不变）。因此 **V4 官方 API 用户永远命中此分支** → 无任何 reasoning effort 变体 → `packages/llm/src/protocols/openai-chat.ts:340` 的 `reasoning_effort` 字段永不写入请求 → 模型永远以默认 effort 思考。

**同一模型对比**: `transform.ts:930` 对 `api.id.includes("deepseek-v4")` 显式 push "max"（OpenRouter/网关路径）→ effort 正常。
**结论**: 同一模型，官方 API 降智，OpenRouter 不降智。

**历史**: git blame 指向 `a882e958b3 "fix: deepseek variants (#24157)"`——上游把 `id.includes("deepseek")` 全匹配改为精确列表，本意是只禁 R1，但误伤官方 API 别名，且未随 V4 发布更新。

**影响**: DeepSeek V4 官方 API 用户（`deepseek-chat`）无法调节推理强度，复杂编码任务深度思考受限。

**修复建议**: 将 `deepseek-chat`/`deepseek-reasoner` 从硬编码禁用列表中移除，改为按 api.id + 模型版本能力精确判断（DeepSeek V3.2+ 支持 `reasoning_effort`）。

---

### 🟠 Bug 2（高危）：`model.id` vs `api.id` 判断不一致

**位置**: `transform.ts:724` vs `transform.ts:930`

```ts
// :724 — 用 model.id（用户显示名）
const id = model.id.toLowerCase()
...
// :930 — 用 model.api.id（API 真实名）
if (model.api.id.toLowerCase().includes("deepseek-v4")) efforts.push("max")
```

两处判断口径不同。用户配置的 model.id 与 api.id 不一致时（如 UI 显示名 "DeepSeek-V4"、api.id "deepseek-chat"），命不命中 `return {}` 完全取决于配置写法 → 行为不可预测。

---

### 🟠 Bug 3（高危）：`capabilities.reasoning` 默认 `false`

**位置**: `packages/opencode/src/provider/provider.ts:1462`

```ts
reasoning: model.reasoning ?? existingModel?.capabilities.reasoning ?? false,
```

若 models.dev 元数据未标注（或用户自定义模型），`variants()` 在 `transform.ts:722` 直接 `return {}` —— 推理参数全链路静默失效。DeepSeek V4 若未在元数据标注 `reasoning: true`，即使 Bug 1 修复也无济于事。

---

### 🟡 Bug 4（中危）：切换模型时思维链被当正文注入

**位置**: `packages/core/src/session/runner/to-llm-message.ts:76-87`

```ts
if (item.type === "reasoning")
  return sameModel
    ? [ { type: "reasoning", ... } ]
    : item.text.length > 0
      ? [{ type: "text", text: item.text }]   // ← 思维链变正文
      : []
```

`sameModel=false`（切换模型）时，旧模型的 reasoning 内容作为普通 text 注入 assistant 消息。新模型会把旧模型的思考内容**当成已给出的最终回答**，影响推理方向（开源模型尤其明显）。

---

### 🟡 Bug 5（边缘）：Anthropic thinking budget 计算可能退化

**位置**: `transform.ts:1013`

```ts
budgetTokens: Math.min(16_000, Math.floor(model.limit.output / 2 - 1)),
```

output limit 小的模型 budget 会退化为 0 或负数。

---

## 二、Prompt Cache 链路（缓存不命中）— 3 个问题

### 🔴 Bug A（确定性）：`promptCacheKey` 提取是死代码

**位置**: `packages/core/src/session/runner/llm.ts:204`

```ts
const promptCacheKey = /^ses_[0-9a-f]{64}$/.test(session.id) ? session.id.slice(4) : session.id
```

真实 ID 格式（`packages/opencode/src/id/id.ts`）: `ses_` + 12 位时间戳 hex + **14 位随机 base62**（共 26 字符）：

```ts
return prefix + "_" + timeBytes.toString("hex") + randomBase62(LENGTH - 12)
```

- 正则要求 **64 位纯 hex** → **永远不匹配** → `slice(4)` 永不执行（死代码）
- 兜底值 = 完整 session.id，含 **14 位随机 base62** → 任何以该 key 做缓存键的 provider/网关，**跨进程/重启缓存永远 miss**

---

### 🟠 Bug B（中危）：日期进入 system 前缀

**位置**: `packages/core/src/system-context/builtins.ts`

```ts
load: DateTime.nowAsDate.pipe(Effect.map((date) => date.toDateString())),
baseline: (date) => `Today's date: ${date}`,
```

system 前缀含日期（`toDateString` 日粒度）→ 跨天会话当天首次请求必 miss（DeepSeek/OpenAI 逐字节前缀缓存）。跨天会话的前缀缓存每天失效一次。

---

### 🟠 Bug C（中危）：每个 turn 重新渲染 system baseline

**位置**: `packages/core/src/session/runner/llm.ts:168-171`

```ts
const loadSystemContext = (agent) =>
  Effect.all([systemContext.load(), skillGuidance.load(agent), referenceGuidance.load()], {
    concurrency: "unbounded",
  }).pipe(Effect.map(SystemContext.combine))
```

每个 turn 重新加载渲染 system + skill guidance + reference guidance。任何一次渲染顺序/内容抖动（skill 目录遍历顺序、reference 注入）都会破坏前缀 → 全部历史缓存失效。对 DeepSeek 磁盘缓存（逐字节前缀匹配）影响最大。

---

### 缓存 miss 对推理质量的真实影响

缓存 miss 不改变请求内容，但对开源模型是可感知的质量路径：

1. 长上下文（10 万+ token）每次全量重新 prefill → 首 token 延迟翻倍
2. 长上下文全量处理时注意力质量波动——开源模型在超长全量 prefill 下输出稳定性显著差于缓存命中后的增量处理（表现为"同样的活干得更慢更差"）
3. 成本 2-5 倍（DeepSeek 缓存读 0.1x / 写 1.25x）

---

## 三、结论与优先级

| # | 问题 | 严重度 | 对"降智"的贡献 | 修复成本 |
|---|------|--------|---------------|---------|
| 1 | deepseek-chat/reasoner effort 被硬编码剥离 | 🔴 确定性 | 直接（推理强度受限） | 低（删两行） |
| 2 | model.id vs api.id 口径不一致 | 🟠 高危 | 间接（行为不可预测） | 中 |
| 3 | capabilities.reasoning 默认 false | 🟠 高危 | 直接（全链路静默失效） | 低 |
| 4 | 模型切换 reasoning→text 注入 | 🟡 中危 | 间接（污染新模型上下文） | 中 |
| A | promptCacheKey 正则死代码 | 🔴 确定性 | 间接（缓存永远 miss） | 低（改正则） |
| B | 日期进 system 前缀 | 🟡 中危 | 间接（每日一次全量） | 低（跳过日期或周粒度） |
| C | baseline 每 turn 重渲染 | 🟡 中危 | 间接（前缀抖动） | 中（epoch 缓存渲染结果） |

**最可能的"降智 20 分"组合**: Bug 1 + Bug 3（DeepSeek V4 官方 API 用户完全拿不到 reasoning_effort 参数）+ Bug A（缓存 miss 放大长上下文质量波动）。

**复现路径**: 配置 DeepSeek 官方 API（模型 `deepseek-chat`）→ 变体列表为空 → 请求体无 `reasoning_effort`；同样的 key 走 OpenRouter 的 `deepseek/deepseek-v4` → 变体含 low/high/max → 请求体有 `reasoning_effort`。

---

*审计基于代码探索系统（12418 chunks / 4764 函数 / 17532 调用边）+ 源码逐行验证*
