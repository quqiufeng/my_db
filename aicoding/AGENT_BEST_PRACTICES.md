# AI Agent 工程最佳实践

> 基于 aicoding 项目实践，结合 Prompt Engineering、Context Engineering、Harness Engineering 三层框架，以及 OpenAI Codex 智能体优先工程经验整理。

---

## 核心观点

构建可靠、可维护、可迭代的 AI Agent 应用，不能只关注 Prompt。真正决定上限的是三层基础设施的系统性设计：

- **Prompt Engineering**：模型这一次要做什么。
- **Context Engineering**：模型此刻知道什么。
- **Harness Engineering**：模型如何可靠地做，并持续变好。

Prompt 决定任务，Context 决定知识，Harness 决定可靠性。

---

## 一、Prompt Engineering：指令层

### 1.1 好的 Prompt 包含六个要素

| 要素 | 说明 | 在 aicoding 中的体现 |
|------|------|---------------------|
| **Persona（角色）** | 定义模型扮演的角色 | `prompts/default.lua` 中定义 expert coding assistant |
| **Purpose（目标）** | 明确任务要达成什么 | 系统 prompt 要求模型完成用户编程任务 |
| **Process（流程）** | 按什么步骤完成 | 先读取、再编辑、再验证的默认工作流 |
| **Policy（约束）** | 边界、风格、格式限制 | 工具使用规则、记忆规则、上下文边界 |
| **Presentation（输出）** | 输出结构 | tool call 使用 JSON schema，代码块带语言标记 |
| **Proof（自检）** | 输出前检查逻辑 | 要求模型用工具验证，而不是空口断言 |

### 1.2 Prompt 设计的最佳实践

- **精简系统 prompt，把 schema 放在 `tools` 数组里**。
  aicoding 不把完整工具说明塞进 prompt，而是让 API 单独传递 schema，减少 token 占用。

- **项目指令通过 `AGENTS.md` / `instructions.md` 注入**。
  项目根目录或 `.opencode/` 下的这些文件会自动加入系统 prompt，实现按项目定制。

- **避免一个巨大无比的 AGENTS.md**。
  参考 OpenAI Codex 经验：AGENTS.md 应该是一份地图（~100 行），指向 `docs/` 中结构化的真实来源。

- **prompt 版本可回滚**。
  由于 aicoding 的 prompt 是 Lua 模块，天然支持 git 版本管理。每次改动应能对比效果。

### 1.3 常见错误

- 目标不清楚，一句话丢给模型期待完成复杂任务。
- 约束不明确，模型不知道什么不能做。
- 输出格式不稳定，下游解析困难。
- 没有自检步骤，模型容易幻觉。

---

## 二、Context Engineering：上下文层

### 2.1 上下文层的核心问题

模型不能只靠内置知识回答。它需要结合：

- 用户输入
- 历史对话
- 知识库与文档
- 代码库
- 工具执行结果
- 短期记忆与长期记忆

Context Engineering 的关键不是塞更多信息，而是**给模型刚好需要的信息**。

### 2.2 aicoding 的上下文基础设施

aicoding 的 Context Infra 由三部分组成：

```
Context 基础设施
├── 记忆系统（KV Cache）
│   ├── 会话历史
│   ├── 事实/偏好
│   ├── 计划/任务状态
│   └── agent 运行记录
├── coding search（代码语义索引）
└── 文档/web 索引（web_fetch 等）
```

#### 2.2.1 记忆系统

由 `memory.lua` + `libmydb.so` 实现：

- **层级命名空间**：`/code/...`、`/agent/{session}/...`
- **磁盘持久化**：mmap + `msync`，进程重启不丢失
- **多维搜索**：前缀、范围、正则、标签、语义
- **TTL 生命周期**：支持过期
- **工具暴露**：`kv_get` / `kv_set` / `kv_search` / `kv_context`

**最佳实践**：

- 学到的重要事实，主动用 `kv_set` 写入 `/agent/{session}/facts/`。
- 读完代码后，把关键结论写成原子化事实，避免重复读取。
- 需要旧信息时，主动用 `kv_search` / `kv_get` 召回，而不是依赖 prompt。
- Plan/Build Agent 的运行记录自动写入 KV，实现跨 agent 共享状态。

#### 2.2.2 有界上下文窗口

aicoding 从根本上解决了上下文无限增长问题：

- 默认 16k tokens 上下文上限（`OPENCODE_CONTEXT_TOKENS` 可调）。
- 旧消息归档到 KV Cache `/agent/{session}/history/`。
- 系统 prompt 精简，只保留必要规则。
- 需要时通过 tool call 召回，而不是堆在 prompt 里。

这比原生 opencode 不断压缩 messages 的方式更可持续。

#### 2.2.3 coding search

第三方代码库预索引到 `/opt/code_caches/{repo}_cache`：

- 语义搜索：Jina embeddings + HNSW
- 调用图分析：caller/callee 关系
- 符号上下文：`kv_context(symbol, repo)`

**最佳实践**：

- 不要直接把几万文件塞进 prompt。
- 用 `kv_search search_type=semantic` 先做语义召回。
- 对关键符号用 `kv_context` 获取调用关系。
- 新代码库先用 `code_index` 索引，再让 agent 探索。

#### 2.2.4 web_fetch 外部知识

`web_fetch` 工具让 agent 能抓取外部 URL：

- API 文档、RFC、StackOverflow
- 依赖库最新用法
- 返回 text / html / markdown

**最佳实践**：

- 外部抓取结果写入 KV Cache `/agent/{session}/web_fetches/`，供后续引用。
- 抓取内容截断到合理长度，避免污染上下文。
- 仅允许 `http`/`https` URL，防止本地命令注入。

### 2.3 Context Engineering 的常见错误

- 不给背景信息，模型只能猜。
- 检索结果噪声太大，没有过滤排序。
- 上下文太长，没有压缩和排序。
- 关键信息顺序混乱，无关信息干扰。
- 记忆机制缺失，长期知识不更新。

### 2.4 下一步可加强

- **自动摘要归档**：每次对话结束自动总结并存入 facts。
- **记忆召回提示**：新会话自动搜索相关历史事实注入 prompt。
- **项目级记忆**：把项目通用知识存到 `.opencode/knowledge/`，跨 session 可用。
- **rerank 步骤**：`kv_search` 结果增加二次筛选，减少噪声。

---

## 三、Harness Engineering：牵引层

Harness 不是单一规范，而是一套工程实践的总称。它解决的是：**模型如何可靠地执行任务，并持续变好**。

### 3.1 Harness 的组成部分

| 组件 | 说明 | aicoding 现状 |
|------|------|--------------|
| **Agent Loop** | 思考 → 行动 → 观察 → 再思考 | `chat_once` / `run_gui` 的迭代循环 |
| **工具调用系统** | 让模型安全操作外部世界 | `tools.default.lua` 统一调度 |
| **权限控制** | 哪些能做、哪些需确认 | `permissions.lua` 支持 allow/deny/ask；内置对 `/usr`、`/etc`、`/bin`、`/sbin`、`/lib*`、`/opt/my_db`、`~/*` 的写/删保护；`OPENCODE_ALLOW_ALL=1` 可全局放行（测试/自动化场景） |
| **错误处理** | 失败如何恢复 | tool 返回 `{ok=false, error=...}` |
| **日志追踪** | 模型做了什么、为什么做 | `log.lua` 统一模块 + `OPENCODE_LOG_LEVEL` 控制；`trace.lua` 结构化 JSONL 轨迹；关键事件已埋点 |
| **Guardrails** | 内容安全、敏感操作限制 | `permissions.is_dangerous_bash()` 拦截 rm -rf /、mkfs、dd、fork bomb、curl\|sh 等危险命令；默认写保护系统目录 |
| **评估系统** | 标准任务衡量效果 | GUI 测试脚本覆盖核心路径 |
| **版本管理** | prompt/agent 可回滚 | Lua 模块天然 git 管理 |
| **成本控制** | token、API 费用控制 | 上下文有界本身就是成本控制 |
| **部署发布** | 如何上线更新 | 单二进制，热更新 Lua 模块 |

### 3.2 aicoding 的 Harness 优势

#### 3.2.1 热更新 + Lua 插件 = 开了天窗

这是 aicoding 最大的差异化设计。

| 传统方式 | aicoding |
|---------|---------|
| 改 prompt 要重新部署 | Lua 模块热加载 |
| 加工具要改主程序 | `plugin_create` + `plugin_load` |
| 加 agent 要发版 | 写 `agents/xxx.lua` 即可 |
| 模型只能按固定能力工作 | 模型可以自己扩展自己的能力 |

模型可以在对话中自己创建并热加载工具：

```bash
"创建一个 hello 工具，输入 name，返回 greeting"
```

模型会调用 `plugin_create` 写文件，再调用 `plugin_load` 热加载，无需重启 agent。

**最佳实践**：

- 插件目录分全局 `/opt/my_db/aicoding/plugins/` 和项目级 `.opencode/plugins/`。
- 高风险插件在加载前经过权限检查。
- 插件行为应通过结构测试约束，避免无限扩张。

#### 3.2.2 Agent 闭环

aicoding 已实现两类 Agent：

**Build Agent**：编译 → 失败 → LLM 诊断 → 修改 → 重试

```
/build [goal]
```

**Plan Agent**：任务拆解 → 按依赖执行 → Build 验证 → 失败重规划

```
/plan <desc>
```

两者组合形成：

```
plan → execute step → build verify → fix if needed → next step
```

#### 3.2.3 Checkpoint / Undo

每次 `edit` / `write` / `apply_patch` / `file_delete` 前自动备份到 `.opencode/checkpoints/{session}/{timestamp}/`。

- 支持 `checkpoint_list`、`undo_last`、`rollback_to`。
- 失败时可以快速恢复，降低 agent 破坏风险。

#### 3.2.4 自动化 GUI 测试

aicoding 自带可编程 GUI 测试框架：

- `gui.set_input`、`gui.submit`、`gui.get_messages`
- 覆盖对话、编辑、build、plan、web_fetch
- 无需人工点击窗口

这是 Harness 中评估系统的雏形。

### 3.3 Harness 的常见错误

- 没有工具调用闭环，模型只会说不会做。
- 没有评估机制，改动效果不可知。
- 没有日志和可观测性，出问题无法定位。
- 没有错误恢复，一次失败就卡住。
- 没有成本和权限控制，生产环境不敢用。
- 没有自动化测试，每次迭代靠手点。

### 3.4 下一步可加强

参考 OpenAI Codex 的 Harness 经验：

| 优先级 | 事项 | 价值 |
|--------|------|------|
| P0 | 结构化项目知识库 `docs/`，让 AGENTS.md 变地图 | 仓库即记录系统 |
| P1 | 运行时可观测性（结构化日志、指标查询） | 让智能体能验证运行时行为 |
| P2 | UI 驱动验证（截图、DOM、录屏） | 让智能体能验证前端行为 |
| P3 | 架构约束与自定义 lint | 保证智能体代码一致性 |
| P4 | 自动评估基准（benchmark + scorecard） | 量化改动影响 |
| P5 | 持续重构/垃圾回收 agent | 长期控制熵增 |

---

## 四、仓库即记录系统

OpenAI Codex 实验最重要的经验之一是：**把代码仓库作为记录系统**。

### 4.1 为什么

智能体在运行时无法访问的内容，对它来说就是不存在的。

- Google Docs 里的决策 → 不存在
- Slack 讨论 → 不存在
- 人脑中的经验 → 不存在
- 仓库中的 Markdown / 代码 / 计划 → 存在

### 4.2 推荐结构

```
docs/
├── design-docs/
│   ├── index.md
│   ├── core-beliefs.md
│   └── ...
├── exec-plans/
│   ├── active/
│   ├── completed/
│   └── tech-debt-tracker.md
├── generated/
│   └── db-schema.md
├── product-specs/
│   ├── index.md
│   └── ...
├── references/
│   ├── design-system-reference-llms.txt
│   └── ...
├── ARCHITECTURE.md
├── DESIGN.md
├── PLANS.md
├── QUALITY_SCORE.md
└── RELIABILITY.md
```

### 4.3 AGENTS.md 应该是一份地图

不要写成一个巨大的百科全书。理想形态：

```markdown
# 项目指南

## 架构
参见 [ARCHITECTURE.md](./ARCHITECTURE.md)

## 代码规范
参见 [docs/quality.md](./docs/quality.md)

## 活跃计划
参见 [docs/exec-plans/active/](./docs/exec-plans/active/)

## 技术债务
参见 [docs/exec-plans/tech-debt-tracker.md](./docs/exec-plans/tech-debt-tracker.md)
```

### 4.4 在 aicoding 中落地

- 已支持 `AGENTS.md` / `instructions.md` 自动注入。
- 建议把项目级知识结构化到 `.opencode/docs/`。
- 建议用 CI 或 agent 定期检查文档是否过时。

---

## 五、智能体可读性

为了让 agent 能直接从仓库推理业务领域，需要提高仓库对智能体的可读性。

### 5.1 代码可读性

- 明确的目录结构
- 一致的命名约定
- 避免隐式依赖
- 边界处显式解析数据形状（类型校验）

### 5.2 架构约束

OpenAI 的做法：

```
Types → Config → Repo → Service → Runtime → UI
```

依赖方向由自定义 linter 机械强制执行。

aicoding 目前缺少这类约束。建议：

- 定义 `.opencode/architecture.json` 描述项目层级和依赖规则。
- 自定义 lint 检查 import/依赖方向。
- 工具执行前用 lint 验证是否符合架构。

### 5.3 可观测性可读性

agent 应该能直接读取：

- 应用日志（LogQL）
- 指标（PromQL）
- 追踪
- UI 截图 / DOM 快照

这样提示才能从"看起来对不对"进化到"运行时验证对不对"。

---

## 六、评估与迭代

### 6.1 为什么需要评估

没有评估，迭代就是盲目的。

### 6.2 推荐指标

| 指标 | 说明 |
|------|------|
| 任务完成率 | agent 是否完成目标 |
| 准确率 | 输出是否正确 |
| 幻觉率 | 是否编造信息 |
| 相关性 | 召回内容是否相关 |
| 延迟 | 响应时间 |
| 成本 | token/API 消耗 |
| 失败恢复率 | 失败时能否自动恢复 |

### 6.3 aicoding 的评估基础

已具备 8 个 GUI 自动化测试：

- `tests/gui_hi.lua`
- `tests/gui_markdown.lua`
- `tests/gui_write_edit.lua`
- `tests/gui_mid_project.lua`
- `tests/gui_build.lua`
- `tests/gui_build_agent.lua`
- `tests/gui_plan.lua`
- `tests/gui_web_fetch.lua`

建议扩展为标准化 benchmark：

- 给陌生项目加函数并编译通过
- 修复真实 bug
- 回答项目架构问题
- 查文档后完成 API 调用

输出 scorecard，用于对比 prompt/context/agent 改动效果。

---

## 七、熵与垃圾回收

完全自主的智能体会复现代码库中的模式，包括不均衡的模式。这会导致漂移。

### 7.1 黄金原则

把主观但机械的规则编码到仓库中，例如：

- 优先使用共享工具包，不重复造轮子。
- 边界处必须验证数据结构。
- 不使用 YOLO 式探测数据。

### 7.2 定期清理

运行后台 agent 扫描：

- 重复代码
- 过时文档
- 偏离架构的模式
- 死代码

发起重构 PR，小额持续偿还技术债务。

---

## 八、总结

| 层级 | 关注点 | 核心问题 | aicoding 现状 |
|------|--------|---------|--------------|
| **Prompt Engineering** | 单次调用 | 模型要做什么 | 70%，结构清晰但缺任务模板和评估 |
| **Context Engineering** | 信息供给 | 模型知道什么 | 85%，KV Cache + coding search + 有界窗口是核心优势 |
| **Harness Engineering** | 系统运行 | 模型如何可靠地做并持续变好 | 65%，骨架完整但缺可观测性、架构约束、评估 |

aicoding 已经走在前面的方向：

- Context 基础设施提前落地
- Harness 基础设施雏形完整
- 热更新 + Lua 插件让 agent 能自己进化

下一步应该把领先优势固化：

1. 仓库即记录系统：`docs/` + 轻量 AGENTS.md
2. 可观测性：日志、指标、UI 验证
3. 架构约束：自定义 lint + 依赖规则
4. 评估系统：benchmark + scorecard
5. 垃圾回收：定期重构 agent

---

## 一句话

> **Prompt 让模型听懂任务，Context 让模型掌握信息，Harness 让模型可靠执行。aicoding 已经在 Context 和 Harness 上建立了优势，接下来的竞争是谁能更好地把这三层系统化、可评估、可进化。**
