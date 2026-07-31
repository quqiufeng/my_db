# opencode 代码级分析报告

> 版本: `19231fce4b`（dev 分支，2026-08-01）
> 分析方式: 代码探索系统（12418 chunks / 4764 函数 / 17532 调用边 / 2000 变量）
> 分析命令: `./analyze_nodejs_repo.sh /opt/opencode /code/opencode`

---

## 1. 项目总览

opencode 是一个 **TypeScript 全栈 AI 编码 Agent**，采用 monorepo 结构（`packages/*`，32 个子包）。核心引擎在 `packages/opencode/src`（运行时/工具/Agent）与 `packages/core/src`（领域模型/存储/调度），前端 UI 在 `packages/app`、`packages/tui`、`packages/session-ui`。

**技术栈**：Effect-TS（核心并发/依赖注入）、AI SDK v5（模型接入）、drizzle-orm + SQLite（持久化）、SolidJS（桌面/TUI UI）、Bun（构建/运行时）。

## 2. 架构分层

```
┌─ CLI 层       packages/opencode/src/cli/     (run/tui/serve/cmd 子命令)
├─ HTTP 服务层  packages/opencode/src/server/   (effect HttpApi + WebSocket + OpenAPI)
├─ 应用内核     packages/opencode/src/          (session/agent/tool/permission/provider/plugin)
├─ 领域核心     packages/core/src/              (schema/session/database/location/github-copilot)
├─ 协议/SDK     packages/sdk/js, packages/client, packages/protocol, packages/schema
└─ 存储         SQLite (drizzle, 38 个 migration) + 文件系统
```

## 3. 核心执行管线（Agent 主循环）

```
opencode run [prompt]
  → packages/opencode/src/cli/cmd/run.ts:1   (非交互/--mini 交互/--attach 三模式)
    → runInteractiveRuntime / runQueue       (runtime.ts:537, 从 stdin 读指令排队)
      → SessionExecution.run (core/src/session/execution/local.ts:31)
        → SessionRunCoordinator (active/interrupt/resume/wake 四态调度)
          → SessionRunner.run                 (core/src/session/runner/)
            → SessionProcessor.create         (opencode/src/session/processor.ts:98)
              → LLM.stream → native-runtime.ts / ai-sdk.ts
              → 工具执行: ToolRegistry.tools → Permission.ask → 工具结果回写
              → 循环直到: stop / continue / compact (DOOM_LOOP_THRESHOLD=3)
```

**关键设计**（processor.ts:102）：LLM 流开始**之前**就捕获文件快照（`snapshot.track()`），因为 AI SDK 可能在 emit start-step 前内部执行工具，防止快照太晚。

## 4. 关键子系统

### 4.1 Agent 系统（agent/agent.ts:35）

- `Info` 是严格 Schema：name/mode(`subagent`/`primary`/`all`)/permission ruleset/model/variant/steps
- 支持 **自动 Agent 生成**（`generate()`：描述 → LLM 生成 identifier/whenToUse/systemPrompt）
- 内置权限模板：外部目录默认 `"*": "ask"`，白名单（临时目录/skills/references）`allow`（agent.ts:108-117）
- 子 Agent 通过 `task` 工具 + 独立权限隔离（subagent-permissions.ts）

### 4.2 Provider/模型层（provider/provider.ts:107）

- **25+ 供应商懒加载**：`BUNDLED_PROVIDERS` 记录表（OpenAI/Anthropic/Bedrock/Vertex/XAI/Mistral/Groq/Cohere/Perplexity/OpenRouter/Copilot/venice 等），首次使用才 `import()`
- **双执行路径**：原生运行时 `session/llm/native-runtime.ts`（`statusWithFetch` 校验 provider ∈ {openai, anthropic, opencode*}，走 `@opencode-ai/llm` 直连）；通用路径走 AI SDK。字段透传采用 OpenAI 官方 wire 字段名（**identity 而非 translation**，native-runtime.ts:79-88 注释明示设计原则）
- OAuth 认证要求 provider fetch override（native-runtime.ts:60），支持 `auth.ts`

### 4.3 工具系统（tool/registry.ts:86）

- **16+ 内置工具**：read/write/edit/apply_patch/glob/grep/bash(→shell)/task/webfetch/websearch/lsp/skill/todo/question/plan/exit_plan/code-mode
- 注册表动态组装：`tools(model, agent, permission)` 按 Agent 的 ruleset 裁剪工具集
- 插件工具经 `fromPlugin()` 适配进同一注册表；WebSearch 仅 opencode provider 或 exa/parallel 标志启用（registry.ts:58）
- 工具定义用 zod + AI SDK JSONSchema7 双向 Schema

### 4.4 权限系统（permission/index.ts:28）

- `evaluate()`：**wildcard 双向匹配** `(permission, pattern)`，`findLast` 取最后匹配规则，默认 `ask`
- 规则三态 `deny/allow/ask`：deny 立即抛 `DeniedError`；ask 走 Deferred 挂起 → publish `Event.Asked` → 用户 reply 后 `Deferred.succeed` 放行（permission/index.ts:98-107）
- reply reject 会级联拒绝同一 session 所有 pending 请求（index.ts:129-130）——防止部分拒绝导致死锁

### 4.5 会话生命周期

- **持久化**：SQLite + drizzle（core/src/database/），38 个迁移文件记录演进
- **调度**：RunCoordinator（drain/force/interrupt/wake），location 路由支持未来远程放置（execution/local.ts:10 注释）
- **上下文管理**：compaction（session/compaction.ts）+ summary + overflow 检测 + `max-steps`
- **回滚**：session/revert.ts + 快照系统（snapshot.track 于 processor 创建时）

### 4.6 协议与服务端（server/server.ts:56）

- effect HttpApi + WebSocket 双通道，OpenAPI 导出（`openapi()`），CORS/mDNS 广播支持
- **代码生成 SDK**：`packages/sdk/js/src/gen` 为生成产物（types.gen.ts 1.1 万行+），配套 httpapi-codegen、client（Effect 版）、sdk-next、codemode（OpenAPI 解释器）等新包——本次更新最大亮点
- 事件流：SSE（sdk 生成 createSseClient）+ GlobalBus（bus/global.ts，Node EventEmitter + 自动事件 ID）

### 4.7 MCP / LSP

- MCP：`mcp/`（client/server/catalog/transport），websearch 通过 MCP 集成
- LSP：`lsp/launch.ts` 的 `spawn()` + `server.ts` 含 20+ 语言服务器配置（含 Roslyn 等），`lsp` 工具提供符号/诊断能力

## 5. 数据流示例（验证查询）

`message` 变量追踪：200 处使用，定义于 `acp/service.ts:669 replayMessages()`（`message` 在 ACP 事件重放中流转）。`createSession` context 查询：被 `runInteractiveRuntime`/`runQueue`/`resolveSession`/`pickVariant` 调用（5+ caller，6 callee）。

## 6. 设计评价

| 维度 | 评价 |
|------|------|
| 架构清晰度 | ★★★★★ 分层严格（CLI→server→core），Effect 依赖注入贯穿，无循环依赖（有 import-boundaries 测试） |
| 可扩展性 | ★★★★★ 25+ provider 懒加载、插件系统（JS + Effect/Promise 双 API）、MCP/LSP 开放协议 |
| 类型安全 | ★★★★★ 全链 Schema（effect Schema）驱动：消息/事件/权限/工具定义全部 Schema-first |
| 代码生成 | ★★★★☆ 协议 SDK 全自动生成，但生成的 types.gen.ts 体积大（1.1 万+ 行） |
| 复杂度 | ★★★☆☆ Effect-TS 学习曲线陡峭；`provider.ts` 2011 行 / `processor.ts` 718 行偏大 |
| 性能 | 快照提前捕获 + 流式编码 + 原生 LLM 直连（绕开 AI SDK 中间层）有明确优化意图 |

**一句话**：opencode 是现代 Schema-first、Effect-typed 的 AI 编码 Agent 参考实现——双运行时（原生直连 + AI SDK）、三端 UI（桌面/TUI/Web）、生成式 SDK 是其最值得学习的架构决策。

---

*生成于 2026-08-01，基于代码探索系统（cache_query / dataflow / context 查询）*
