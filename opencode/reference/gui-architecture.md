# OpenCode GUI 架构：Rust + LuaJIT FFI

这是一个值得参考的 GUI 程序开发模式：**用 Rust 写渲染引擎和平台适配，用 LuaJIT 写业务逻辑和 UI 内容驱动**。两者通过稳定的 C ABI 隔离，Rust 编译为动态库，Lua 通过 FFI 加载并调用。

## 整体分层

```
┌─────────────────────────────────────────────────────────────┐
│                         Lua 脚本层                           │
│  main.lua  /  prompts/default.lua  /  tools/default.lua      │
│  permissions.lua  /  gui.lua                                  │
│  - 组装 LLM 请求                                              │
│  - 调用 C HTTP 客户端发送请求                                  │
│  - 解析 SSE/JSON 响应                                         │
│  - 调用 GUI FFI 更新界面                                       │
│  - 调度工具、检查权限、管理 Todo                              │
├─────────────────────────────────────────────────────────────┤
│                      LuaJIT FFI 绑定层                         │
│  gui.lua 中通过 ffi.load("opencode_gui") 加载 libopencode_gui.so│
│  暴露：gui.create / append_message / stream_delta /           │
│        tool_output / add_todo / set_todo_done 等              │
├─────────────────────────────────────────────────────────────┤
│                       Rust GUI 引擎层                          │
│  opencode/gui_gpui/src/lib.rs                                 │
│  - GPUI / gpui-component 实现窗口、渲染、输入、事件            │
│  - 暴露 #[no_mangle] C ABI 函数                               │
│  - 维护 App 状态（messages / todos / callbacks）               │
│  - 定时轮询刷新，把 C 回调的更新同步到 GPUI 主线程             │
├─────────────────────────────────────────────────────────────┤
│                        C 核心层                               │
│  session.h/c, lua_engine.h/c, llm_client.h/c                  │
│  - KV Cache 会话管理                                          │
│  - LuaJIT 宿主 + C 函数注册                                   │
│  - OpenAI/Anthropic HTTP 客户端                               │
└─────────────────────────────────────────────────────────────┘
```

## 为什么这样设计

### 1. Rust 做 GUI 引擎

- **跨平台渲染**：GPUI 提供一致的 GPU 加速 UI
- **内存安全**：窗口状态、跨线程通信不容易出 UB
- **稳定 ABI**：`#[no_mangle] pub extern "C"` 导出固定符号，LuaJIT FFI 可以直接调用
- **高性能重绘**：C 回调更新共享状态后，GPUI 定时轮询触发 `cx.notify()`

### 2. LuaJIT 写业务逻辑

- **热更新**：修改 prompt、工具、交互逻辑不需要重新编译 Rust/C
- **表达力强**：JSON 组装、字符串处理、权限规则都很方便
- **体积小**：Lua 脚本轻量，适合作为 agent 的“大脑”
- **生态借力**：可以用 `cjson.so` 等现有 Lua/C 扩展

### 3. C ABI 作为边界

```rust
// Rust 端导出的 C 函数
#[no_mangle]
pub extern "C" fn gui_append_message(
    app: *mut c_void,
    role: *const c_char,
    text: *const c_char,
);
```

```c
// C 头端看到的签名（概念上）
void gui_append_message(void* app, const char* role, const char* text);
```

```lua
-- Lua 端通过 FFI 调用
local gui = ffi.load("opencode_gui")
gui.gui_append_message(app, "assistant", delta)
```

三者共享同一套 C ABI，Rust 和 Lua 都不需要知道对方的具体实现。

## 数据流示例：LLM 回复更新到 GUI

```
用户按 Enter
    ↓
Lua send_message → C llm_complete_raw
    ↓
C HTTP 客户端流式读取 SSE
    ↓
每收到一个 delta，C 调用 Lua 回调
    ↓
Lua 调用 gui.stream_delta("assistant", delta)
    ↓
Rust GUI 引擎追加到 messages，刷新 UI
    ↓
GPUI 定时轮询触发 cx.notify()，窗口重绘
```

## 关键技巧：跨线程安全刷新

Rust GUI 引擎不能在 C/Lua 回调线程直接调用 `cx.notify()`。本项目的做法：

1. C 回调更新共享状态（`messages`、`todos`）
2. Rust 引擎在 `ChatView` 创建时启动一个 GPUI 内部定时器：

```rust
cx.spawn(async move |this, cx| {
    loop {
        cx.background_executor()
            .timer(Duration::from_millis(100))
            .await;
        this.update(cx, |_this, cx| cx.notify()).ok();
    }
})
.detach();
```

3. 每 100ms 在 GPUI 主线程上触发一次重绘

这样避免了外部 `mpsc` 通道与 GPUI 渲染冲突导致的窗口透明问题。

## 可复用模式

如果你想做一个类似的 GUI 程序，可以照搬这个结构：

| 部分 | 技术 | 职责 |
|------|------|------|
| 渲染引擎 | Rust + GPUI/egui/tao | 窗口、绘制、输入、定时刷新 |
| 业务逻辑 | LuaJIT | 数据获取、状态机、UI 内容生成 |
| 绑定层 | LuaJIT FFI + C ABI | 稳定的跨语言接口 |
| 核心库 | C/Rust | 文件、网络、数据库等底层能力 |

## 本项目相关文件

- `opencode/gui_gpui/src/lib.rs`：Rust GUI 引擎
- `opencode/gui.lua`：Lua FFI 绑定
- `opencode/main.lua`：主循环、LLM 交互、GUI 内容驱动
- `opencode/prompts/default.lua`：系统 prompt 组装
- `opencode/tools/default.lua`：工具定义与调度
- `opencode/permissions.lua`：权限规则
- `opencode/reference/opencode-ui-layout.md`：UI 布局参考
