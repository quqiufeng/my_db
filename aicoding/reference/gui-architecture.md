# GUI 架构：LuaJIT + Rust/gpui-component

本项目采用 **Lua 驱动业务逻辑、Rust 做 GPU 加速渲染** 的分层架构。两者通过 **稳定的 C ABI** 隔离——Rust 编译为 `.so` 动态库，LuaJIT 通过 FFI 加载调用。界面响应与 LLM 交互完全在 Lua 层完成，修改行为不需要重编译 Rust。

---

## 一、整体分层

```
┌────────────────────────────────────────────────────────────┐
│                    Lua 业务逻辑层                            │
│  main.lua (run_gui)                                         │
│  - 创建 GUI 实例、注册用户消息回调                            │
│  - Coroutine 调度：LLM 请求、流式输出、工具执行               │
│  - 组装 HTTP 请求头、解析响应                                │
│  - 调用 gui.* FFI 更新界面                                  │
├────────────────────────────────────────────────────────────┤
│                  LuaJIT FFI 绑定层                           │
│  gui.lua                                                    │
│  - ffi.load("aicoding_gui", true) 加载 .so                  │
│  - ffi.cdef 声明 Rust 导出的 C 函数                          │
│  - ffi.cast 将 Lua 闭包转为 C 函数指针传递给 Rust            │
│  - 管理回调生命周期，防止 GC 回收                             │
├────────────────────────────────────────────────────────────┤
│                    C 桥接层                                   │
│  lua_engine.c / gui_tick.c                                  │
│  - opencode.gui_set_tokens/input/submit (C → Lua binding)   │
│  - dlsym 动态解析 Rust 导出的 GUI 符号                       │
│  - gui_tick.c: Rust 定时器 → 调用 Lua gui_tick() 恢复协程   │
│  - gui_tick.c: Rust 双击复制 → 调用 Lua gui_on_copy()       │
├────────────────────────────────────────────────────────────┤
│                  Rust 渲染引擎层                              │
│  gui_gpui/src/lib.rs                                        │
│  - gpui-component 窗口、输入框、按钮、布局                    │
│  - syntect 代码语法高亮                                     │
│  - Markdown / Diff / Reasoning 块渲染                       │
│  - 60fps 定时器驱动 Lua tick + 界面刷新                      │
└────────────────────────────────────────────────────────────┘
```

---

## 二、代码文件清单

| 文件 | 层 | 职责 |
|------|-----|------|
| `gui_gpui/src/lib.rs` | Rust | 窗口创建、消息列表、输入框、状态栏、右侧面板、代码高亮、Diff 视图 |
| `gui.lua` | Lua FFI | `cdef` 声明、`ffi.load`、回调类型转换、`create`/`append_message`/`stream_delta` |
| `lua_engine.c` | C | `l_gui_set_tokens`/`l_gui_set_input`/`l_gui_submit` 注册到 `opencode.` 表 |
| `gui_tick.c` | C | `opencode_gui_tick`（定时器驱动协程）、`opencode_gui_notify_copy`（双击复制回调 Lua） |
| `main.lua` | Lua | `run_gui()`——GUI 主入口，协程内的 LLM 调用、流式输出、工具执行 |
| `async_http.lua` | Lua | 非阻塞 HTTP（配合协程 yield，避免 GUI 卡死） |

---

## 三、C ABI 契约（LuaJIT ↔ Rust）

所有跨语言调用都通过 `.so` 导出的 C 符号完成。

### 3.1 Rust 导出的 C 函数（被 Lua 调用）

```rust
// lib.rs: #[no_mangle] pub extern "C" fn ...
void*   gui_app_create(const char* config_json);
void    gui_app_free(void* app);
void    gui_on_user_message(void* app, callback_fn, void* userdata);
void    gui_on_tool_call(void* app, callback_fn, void* userdata);
int     gui_run(void* app, void* lua_state);

void    gui_stream_delta(void* app, const char* session_id, const char* delta);
void    gui_append_message(void* app, const char* session_id, const char* role, const char* text);
void    gui_tool_output(void* app, const char* session_id, const char* tool_id, const char* output);
void    gui_set_tokens(void* app, int total, int prompt, int completion);
void    gui_set_input_value(void* app, const char* text);
void    gui_submit_input(void* app);
char*   gui_get_messages(void* app);
void    gui_add_todo(void* app, const char* text);
void    gui_set_todo_done(void* app, const char* text, int done);
void    gui_clear_todos(void* app);
```

### 3.2 C 层导出的函数（被 Rust 调用）

```c
// gui_tick.c: 可在 GPUI 主线程安全调用
void opencode_gui_tick(void* lua_state);       // 恢复 Lua pending coroutines
void opencode_gui_notify_copy(void* lua_state, const char* text);  // 双击复制通知 Lua
```

### 3.3 Lua 端注册的全局函数（被 Rust/C 回调）

```lua
-- main.lua 定义的全局函数，被 gui_tick.c 通过 lua_getglobal 查找：
function gui_tick()      -- 60fps 定时器每帧调用一次，恢复所有 pending coroutines
function gui_on_copy(text)  -- 用户双击代码块/文本时调用
```

---

## 四、核心数据流

### 4.1 启动流程

```
cli.c main()
  │
  ├─ 加载 .env → 模型选择器 → configure_llm()
  │
  └─ opencode.llm_protocol() + llm_complete_raw (CLI 模式)
     或
     run_gui() (GUI 模式，默认)
        │
        ├─ gui = require("gui")
        ├─ gui.create({title, project_root, model, version})
        │     └─ Rust gui_app_create() → 初始化 GuiApp 结构体
        │
        ├─ gui.append_message(welcome)
        ├─ gui.on_user_message(handler)
        │     └─ Rust gui_on_user_message() → 存储回调指针
        │
        ├─ gui.run(app)
        │     └─ Rust gui_run() → gpui_platform::application().run()
        │           ├─ 创建窗口 → ChatView
        │           ├─ 60fps 定时器 → opcode_gui_tick → Lua gui_tick()
        │           └─ 阻塞直到窗口关闭
        │
        └─ gui.free(app)
```

### 4.2 用户输入 → LLM 响应 → 流式输出

```
用户输入文本 → 回车
  │
  ├─ Rust: gui_submit_input()
  │     ├─ 写入 messages: "user", text
  │     └─ 调用 on_user_message 回调 (C 函数指针)
  │           │
  │           └─ Lua: gui.on_user_message 注册的 handler
  │                 │
  │                 ├─ gui.append_message(app, sid, "user", text)   → 界面显示用户消息
  │                 ├─ gui.clear_todos(app)
  │                 ├─ trace.agent_start(text)
  │                 └─ 创建协程:
  │                       coroutine.create(function()
  │                         1. 组装 request_body (system + messages + tools)
  │                         2. async_http.request(url, headers, body_json)
  │                              ├─ opencode.http_request() → C 异步 HTTP
  │                              └─ opencode.http_poll() + coroutine.yield() 循环
  │                         3. 解析响应 → content + reasoning + tool_calls
  │                         4. while content 未发送完:
  │                              ├─ gui.stream_delta(app, sid, chunk)  → Rust 追加到 messages
  │                              └─ coroutine.yield()                 → 让出给定时器
  │                         5. 如果有 reasoning → gui.append_message("reasoning", ...)
  │                         6. 如果有 tool_calls:
  │                              ├─ gui.tool_output(app, sid, name, "running...")
  │                              ├─ tools.dispatch(tc) → 执行工具
  │                              ├─ gui.tool_output(app, sid, name, result)
  │                              └─ 转到步骤 1（下一轮 LLM 调用）
  │                         7. 完成
  │                       end)
  │
  └─ coroutine.resume(co)
        └─ 插入 pending_coroutines 表
```

### 4.3 定时器驱动协程

```
Rust 60fps 定时器 (16ms)
  │
  ├─ ① opencode_gui_tick(lua_state)   [C → Lua]
  │     └─ Lua gui_tick()
  │            └─ 遍历 pending_coroutines[]
  │                  对于每个 suspended 的协程: coroutine.resume(co)
  │                  协程执行到下一个 coroutine.yield() 后挂起
  │                  协程的 stream_delta() 调用在 resume 中执行
  │
  └─ ② cx.notify()                    [Rust 重绘]
        └─ ChatView::render()
              ├─ 读取 app.messages → 消息列表
              ├─ 读取 app.todos → 待办列表
              ├─ 读取 app.token_* → 统计栏
              └─ 渲染完整的 UI 树
```

**关键：每帧先 tick 再 render。** 协程在 tick 阶段发送 stream delta 更新 Rust 共享状态，然后 render 阶段读取最新状态绘制，延迟控制在 1 帧（<16ms）。

### 4.4 双击复制

```
用户双击消息/代码块
  │
  ├─ Rust 事件 → opencode_gui_notify_copy(lua_state, text)  [C → Lua]
  │     └─ Lua gui_on_copy(text)
  │            └─ opencode.set_clipboard(text)
  │                  ├─ wl-copy / xclip / xsel
  │                  └─ log.info("copied to clipboard")
```

---

## 五、状态管理

### 5.1 Rust 侧共享状态（`GuiApp` 结构体）

```rust
struct GuiApp {
    on_user_message: Option<extern "C" fn(...)>,  // 用户消息回调
    on_tool_call:    Option<extern "C" fn(...)>,   // 工具调用回调
    messages:        Arc<Mutex<Vec<MessageRow>>>,  // 消息列表
    model:           String,                        // 模型名
    title:           String,                        // 窗口标题
    version:         String,                        // 版本
    project_root:    PathBuf,                       // 项目路径
    todos:           Arc<Mutex<Vec<TodoItem>>>,     // 待办列表
    input_buffer:    Arc<Mutex<String>>,            // 外部输入缓冲区
    token_prompt:    Arc<Mutex<usize>>,             // prompt tokens
    token_completion: Arc<Mutex<usize>>,            // completion tokens
    token_total:     Arc<Mutex<usize>>,             // 总 tokens
    lua_state:       *mut c_void,                   // LuaJIT 状态指针
    view:            Mutex<Option<WeakEntity<ChatView>>>,  // GPUI 视图弱引用
    executor:        Mutex<Option<ForegroundExecutor>>,    // GPUI 主线程执行器
}
```

### 5.2 Lua 侧协程状态

```lua
-- main.lua
local pending_coroutines = {}  -- 全局待恢复协程列表
_G.gui_mode = true              -- GUI 模式标志（控制 async_http.sleep 行为）
```

协程内部有自己的闭包状态（`messages` 表、`request_body`、`tool_calls` 等），不需要额外的状态管理。

---

## 六、UI 组件结构

```
窗口
├── 主区域 (Main Area)
│   ├── 消息列表 (Message List) — 可滚动
│   │   ├── User 消息（蓝色标签）
│   │   ├── Assistant 消息（紫色标签）
│   │   │   ├── Markdown 渲染（标题/列表/加粗/行内代码）
│   │   │   └── 代码块（syntect 语法高亮）
│   │   │       └── diff 检测 → side-by-side 分栏对比
│   │   ├── Thinking 块（灰色标签，默认折叠）— reasoning_content
│   │   └── Tool 消息（黄色标签，可折叠）
│   ├── 输入栏 (Input Bar)
│   │   └── gpui-component Input（多行文本、Shift+Enter 换行、Enter 发送）
│   └── 状态栏 (Status Bar)
│       ├── 模型名 · 版本
│       ├── token 使用量 / 上限 (xx%) 进度条
│       └── 项目名:main
└── 右侧面板 (Right Panel)
    ├── Session — 会话开始时间
    ├── Context — token 使用详情 + 进度条
    ├── LSP — LSP 状态（当前禁用）
    └── Todo — Agent 待办列表（可点击切换完成状态）
```

---

## 七、关键设计细节

### 7.1 Callback 生命周期管理

```lua
-- gui.lua
M._apps[handle] = {
    handle = handle,
    user_cb = nil,   -- C 函数指针，防止 GC
    tool_cb = nil,   -- C 函数指针，防止 GC
}
```

Lua 的 `ffi.cast` 产生的函数指针如果被 GC 回收，C 侧调用时会崩溃。`gui.lua` 用 `M._apps` 表持有所有回调引用，确保在 GUI 生命周期内指针有效。

### 7.2 流式输出 UTF-8 安全分块

```lua
-- main.lua: 流式输出 chunk
local chunk_size = 20
local pos = 1
while pos <= #content do
    local next_pos = math.min(pos + chunk_size, #content + 1)
    -- 如果 next_pos 在多字节 UTF-8 字符中间，回退到字符起始
    while next_pos > pos do
        local byte = content:byte(next_pos)
        if byte == nil or byte < 128 or byte >= 192 then break end
        next_pos = next_pos - 1
    end
    local chunk = content:sub(pos, next_pos - 1)
    gui.stream_delta(app, sid, chunk)
    pos = next_pos
    coroutine.yield()
end
```

### 7.3 工具回调内存安全

```lua
-- 返回给 Rust 的字符串必须分配在 C 堆上
local cstr = ffi.C.malloc(#out + 1)
ffi.copy(cstr, out)
ffi.cast("char*", cstr)[#out] = 0  -- null-terminate
return cstr  -- Rust 侧通过 CString::from_raw 接管所有权
```

### 7.4 GUI 中的 LLM 请求：异步非阻塞

GUI 模式不走 C 层的 `llm_complete_raw`（同步阻塞），而是走 `async_http.lua`：

```lua
-- main.lua / async_http.lua
local req = opencode.http_request(url, method, headers, body)
while true do
    local status = opencode.http_poll(req)  -- 非阻塞
    if status == 1 then return opencode.http_response(req) end
    if status == -1 then return nil end
    coroutine.yield()  -- 让出给 60fps 定时器
end
```

### 7.5 线程安全

- 所有跨语言调用都在 **GPUI 主线程** 上执行（LuaJIT 运行在主线程上，FFI 调用同步）
- `Arc<Mutex<T>>` 保护 Rust 侧共享状态
- **不需要** `mpsc` 通道或 `Send + Sync` 标记以外的同步机制

---

## 八、性能特性

| 指标 | 值 |
|------|-----|
| UI 刷新率 | 60fps（16ms 定时器） |
| 流式输出延迟 | <16ms（从 Lua yield 到界面显示） |
| 代码高亮 | syntect（Rust 原生，比 browser-based 快） |
| LLM HTTP | C 层 curl-multi 异步，不阻塞渲染 |
| 二进制体积 | ~40MB（Rust .so）+ 2MB（aicoding 二进制） |

---

## 九、编译与运行

```bash
# 编译 Rust GUI
cd gui_gpui && cargo build --release
cp target/release/libopencode_gui.so ../libaicoding_gui.so

# 编译 C 内核
cd .. && make

# 安装全局
sudo make install

# 运行 GUI（默认模式）
aicoding --project /path/to/repo

# 或强制 CLI
OPENCODE_GUI=0 aicoding --project /path/to/repo
```

---

## 十、架构图（简化调用链路）

```
  Lua                           C                        Rust
─────                         ───                       ────
main.lua                      gui_tick.c                lib.rs (gpui)
  │                             │                         │
  ├─ gui.run(app) ──────────────┼──────────────────────→ gui_run()
  │                             │                         │
  │                             │                    gpui::application().run()
  │                             │                     ├─ Window → ChatView
  │                             │                     ├─ 60fps timer ───┐
  │                             │                     │                 │
  │  gui_tick() ←─── opencode_gui_tick() ←───────────-┘                 │
  │    ├─ resume coroutine ──→ stream_delta() ───────→ messages.push()  │
  │    └─ 返回                                                           │
  │                                  cx.notify() ←──────────────────────┘
  │                                    │
  │                                    └─ ChatView::render()
  │                                         ├─ messages → 消息列表
  │                                         ├─ todos → 待办面板
  │                                         └─ tokens → 状态栏
  │
  ├─ gui.on_user_message(handler) ← Rust: 用户回车
  │     │
  │     └─ create coroutine {
  │           1. async_http.request() ──→ C curl-multi
  │           2. parse response
  │           3. stream_delta() + yield()
  │           4. tool_call → tools.dispatch()
  │           5. repeat
  │        }
  │
  └─ gui.free(app) ──────────────────→ gui_app_free()
```

---

## 十一、与其他方案的对比

| 方案 | 体积 | 启动速度 | 可热更新 | GPU 加速 |
|------|------|---------|---------|---------|
| Electron + React | ~150MB | 慢（需要 Node.js） | 否 | 浏览器引擎 |
| Tauri + Rust | ~5MB | 快 | 否（前端代码可热更新） | WebView |
| **本方案 (LuaJIT + gpui)** | **~42MB** | **快（直链动态库）** | **是（Lua 全量热更新）** | **原生 GPU** |
| 纯 Rust（egui/imgui） | ~2MB | 快 | 否 | 原生 GPU |

本方案的优势：**Lua 层（prompt、工具、GUI 交互逻辑）全部支持热更新**，不需要重编译 Rust。Rust 层只负责渲染，业务逻辑全部在 Lua 中定义。

---

## 十二、通用模式总结

这套架构可以抽象为一个**通用的 Linux GUI 开发模式**：

> **Rust/gpui 做 GPU 加速渲染 + LuaJIT FFI 做业务逻辑 + C ABI 做跨语言边界**

```
任何 GUI 程序
├── Rust/gpui: 窗口、输入、布局、GPU 绘制
├── LuaJIT FFI: 桥接层，封装 C/Rust API 为 Lua 调用
└── Lua: 业务逻辑、状态管理、UI 内容生成
```

### 为什么这适合 Linux

1. **LuaJIT FFI 可以直接封装任意 C 库**
   - 你不需要 Rust 绑定，也不需要 C 扩展——`ffi.cdef` + `ffi.load` 就能调用现有的 `.so`
   - 比如 `cjson.so`、`libcurl.so`、`libmydb.so`、OpenGL 库等
   - 这意味着可以用 Lua 写 GUI 业务逻辑，同时复用 Linux 生态里几乎所有的 C 库

2. **Rust 只负责 GPU 渲染**
   - gpui-component 提供窗口、按钮、输入框、滚动等基础组件
   - Rust 层是稳定的——编译一次，Lua 层随便改
   - 界面布局、交互逻辑、数据获取全部在 Lua 中热更新

3. **适合的场景**
   - AI agent 聊天窗口（本项目的用途）
   - 系统监控仪表盘（GPU 加速 + 实时数据）
   - 开发者工具（Lua 热更新意味着快速迭代）
   - 嵌入式 GUI（Rust 无运行时 + LuaJIT 轻量）

4. **不适合的场景**
   - 移动端或 Web 端（gpui 目前主要是 Linux/ macOS）
   - 需要复杂动画/过渡效果的 UI（gpui-component 基础组件有限）
   - 团队全员是前端/JS 开发者（需要理解 LuaJIT FFI）

### 关键门槛

这套方案只需要三样东西：
- **一个 Rust 编译环境**：`cargo build --release` 生成 `.so`
- **一个 LuaJIT 运行时**：项目自带
- **一份 C ABI 头文件**（或者像本项目直接在 `ffi.cdef` 中声明）

没有 Node.js、没有 Electron、没有 WebView，一个 ~42MB 的二进制就可以跑一个 GPU 加速的 GUI 程序。
