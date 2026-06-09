# Rust AOT 编译器方案：syn + rustc_driver + Chez Scheme = 有限集成

> 本文档描述如何利用 **Rust 编译器工具链（syn + rustc_driver）** 提取 Rust 代码的语义，
> 将 Rust 代码编译为 Chez Scheme S-expr，通过 Chez AOT 编译器产出单文件 ELF 二进制。
> 重点说明 Zed 编辑器的 GPUI 库集成方案（这是引入 Rust 支持的核心原因）。

---

## 一、Rust 在体系中的特殊位置

### 1.1 和 PHP/Python 的根本区别

```
PHP/Python:
  runtime = C → foreign-procedure 直调 → 全部生态可用
  探索方式: cache_query "_zend_ast" → C 源码

Rust:
  runtime = Rust → 没有 C ABI → 大部分生态不可用
  探索方式: syn + rustc_driver → 编译器内部 AST
```

### 1.2 分层支持策略

```
Rust 编译后的产物分为两层，各自处理:

┌─────────────────────────────────────────────┐
│  你写的 Rust 业务逻辑:                         │
│    fn main() { ... }                         │
│    fn process(data: &[f32]) -> f32 { ... }    │
│                                              │
│    处理: syn 提取 AST → 翻译器 → Scheme      │
│    语义: 对齐 Rust 语法规则，不对齐运行时实现   │
├─────────────────────────────────────────────┤
│  Rust C ABI 桥接层:                           │
│    #[no_mangle] pub extern "C" fn ...        │
│                                              │
│    处理: gcc 直接调                           │
│    语义: 和 C 函数完全一样                     │
├─────────────────────────────────────────────┤
│  Rust 标准库/生态 (std, tokio, serde...):    │
│                                              │
│    处理: 不能调 ❌                             │
│    替代: 直接调底层的 C API (epoll,  JSON...) │
└─────────────────────────────────────────────┘
```

---

## 二、引入 Rust 的核心原因：Zed GPUI 图形库

### 2.1 GPUI 是什么

GPUI 是 Zed 编辑器（`github.com/zed-industries/zed`）的 UI 框架。
**混合即时/保留模式，GPU 加速**，用 Rust 编写。

```
GPUI 的特点:
  - GPU 渲染（Vulkan/Metal）
  - 高性能文本布局（harfbuzz + 自定义排版引擎）
  - 低延迟输入处理
  - 原生窗口管理（X11/Wayland/macOS）
```

### 2.2 GPUI 底层是 C

GPUI 虽然是 Rust 实现的，但它底层的全部能力来自 C 库：

```
GPUI 功能             底层 C 库           直接 foreign-procedure
────────────────────────────────────────────────────────────────
窗口创建               X11 (libX11)        XOpenDisplay / XCreateWindow
                       Wayland (libwayland) wl_display_connect
GPU 渲染               Vulkan (libvulkan)   vkCreateInstance / vkCreateSwapchainKHR
文本布局               harfbuzz (libharfbuzz) hb_shape / hb_buffer_create
字体                       freetype (libfreetype)     FT_New_Face / FT_Load_Glyph
图像解码               libpng / libjpeg     png_create_read_struct
路径渲染               cosmic-text          (底层调 harfbuzz + freetype)

全部是 C ABI，全部可以 foreign-procedure。
不需要 Rust，不需要 GPUI，直接调就行。
```

### 2.3 集成方案：跳过 GPUI，直调 C

```
Zed 的 GPUI (Rust)             →  你的 Scheme 代码
  Window::new("hello")           ←   (XOpenDisplay ...)
  window.draw(scene)             ←   (vkQueuePresentKHR ...)
  text_system.layout_line(...)   ←   (hb_shape ...)
  sprite_atlas.upload(...)       ←   (vkCreateImage ...)

Rust 只是一个语法包装，底层 C 库都能直接 foreign-procedure。
不写 Rust，不翻译 GPUI，直接调 C。
```

#### 窗口创建示例

```scheme
;; 直接调 X11，跳过所有 Rust
(define x11-display
  (foreign-procedure "XOpenDisplay" (void*) void*))

(define x11-create-window
  (foreign-procedure "XCreateWindow"
    (void* void* int32 int32 int32 int32 int32 void* void* void*) void*))

(define x11-map-window
  (foreign-procedure "XMapWindow" (void* void*) int32))

(let ((dpy (x11-display null))
      (win (x11-create-window dpy (root-window dpy) 0 0 800 600 0 null null null)))
  (x11-map-window dpy win))
```

#### Vulkan 渲染示例

```scheme
;; 直接调 Vulkan，跳过 Rust 的 vulkano/ash 包装
(define vk-create-instance
  (foreign-procedure "vkCreateInstance"
    (void* void* void*) int32))

(define vk-create-device
  (foreign-procedure "vkCreateDevice"
    (void* void* void*) int32))

(define vk-create-swapchain
  (foreign-procedure "vkCreateSwapchainKHR"
    (void* void* void*) int32))

;; 渲染循环完全在 Scheme 里，直接调 GPU
(let loop ()
  (vk-acquire-next-image ...)
  (vk-queue-submit ...)
  (vk-queue-present ...)
  (loop))
```

### 2.4 可复用的 GUI 库包装

如果你不想每次都写 X11/Vulkan 的样板代码，可以写一个轻量的 Scheme GUI 库：

```scheme
;; simple-window.ss — Scheme GUI 库（类似 GPUI 但纯 Scheme + C FFI）
(define (make-window title width height)
  (let ((dpy (x11-open-display))
        (win (x11-create-window dpy width height)))
    (register-frame-callback (lambda (scene)
      (vk-present dpy win scene)))
    (window-object dpy win)))

(define (window-draw win render-fn)
  (push-frame-callback win (lambda () 
    (let ((scene (make-scene)))
      (render-fn scene)
      (vk-present (window-dpy win) (window-handle win) scene)))))
```

这份包装写一次，后续所有 GUI 项目复用。放进 aot hub 上就是 `aot-get install simple-gui`。

---

## 三、Rust 语法翻译方案

### 3.1 翻译流水线

```
Rust 源码
  ↓
syn (Rust 官方解析库)
  → 产出 Rust AST（纯语法，无语义）
  ↓
rustc_driver (Rust 编译器驱动)
  → 跑类型检查、生命周期推断、trait 解析
  → 产出带语义的完整 AST（JSON）
  ↓
你的翻译器
  → 读取 JSON AST
  → 按 Rust 语义规则翻译
  → 产出 Scheme S-expr
  ↓
Chez AOT
  → compile-file
  → ELF 二进制
```

### 3.2 AST 导出工具

```rust
// rust-ast-dump.rs — 用 syn 导出 Rust AST
use syn::{parse_file, File};
use serde_json::json;
use std::fs;

fn main() {
    let code = fs::read_to_string(std::env::args().nth(1).unwrap()).unwrap();
    let ast: File = syn::parse_file(&code).unwrap();
    
    let items: Vec<_> = ast.items.iter().map(|item| {
        match item {
            Item::Fn(f) => json!({
                "kind": "function",
                "name": f.sig.ident.to_string(),
                "inputs": f.sig.inputs.iter().map(|arg| {
                    json!({"name": format!("{:?}", arg)})
                }).collect::<Vec<_>>(),
                "body": format!("{:?}", f.block)
            }),
            _ => json!({"kind": "unknown"})
        }
    }).collect();
    
    println!("{}", serde_json::to_string_pretty(&json!({"items": items})).unwrap());
}
```

### 3.3 翻译映射表

```
Rust 构造                Scheme 生成                       备注
──────────────────────────────────────────────────────────────────────
fn foo(x: i32) -> i32    (define (foo x) ...)              基本一致
let x = 42               (let ((x 42)) ...)                基本一致
if cond { ... }          (if cond ...)                     基本一致
match x { ... }          (match x ...)                     Scheme 原生支持
loop { ... }             (let loop () ... (loop))          尾部调用优化
for x in iter {}         (for-each (λ(x) ...) iter)        迭代器
struct Point { x, y }    (define-record-type Point ...)     等同
impl Point { fn f() }    (define (point-f p ...) ...)       方法展开
Option<T>                 (or (some val) (none))            动态类型
Result<T, E>             (or (ok val) (err val))           动态类型
trait Display { ... }    (define display ...)              特化函数
#[derive(Debug)]         自动生成 (display ...)            宏展开
unsafe { ... }           无条件翻译                         不做安全检查
```

### 3.4 语义对齐的限制

```
Rust 有但 Scheme 没有的概念          影响
──────────────────────────────────────────
所有权转移                           忽略，翻译成 let 绑定
借用 &mut                            忽略，翻译成 set!
生命周期 'a                          忽略，不检查
trait 约束                          展开成具体类型
泛型 T: Display                      翻译时特化具体调用
Pin<&mut Self>                      忽略，通过引用传递
Send + Sync                         忽略
drop checker                        忽略

编译后的代码能跑，但不保证 Rust 的内存安全。
翻译器只翻译语义，不翻译 Rust 编译器的安全检查。
```

---

## 四、Rust 生态支持决策

### 4.1 生态可用性分层

```
┌──────────────────────────────────────────────────────────┐
│  完全可用 (有 C ABI):                                      │
│    Vulkan (libvulkan.so / .a)                              │
│    X11 (libX11)                                            │
│    Wayland (libwayland)                                     │
│    freetype (libfreetype)                                   │
│    harfbuzz (libharfbuzz)                                   │
│    OpenSSL (libssl / libcrypto)                             │
│    zlib                                                     │
│    cURL (libcurl)                                           │
│    ... 一切 C 库                                            │
│                                                            │
│  可用 (有 extern "C" 包装):                                 │
│    你自己写的 Rust 模块                                     │
│    别人给你写了 C ABI 的 Rust crate                          │
│                                                            │
│  不可用 (纯 Rust，无 C ABI):                                │
│    tokio / async-std                                        │
│    axum / actix-web                                         │
│    serde (序列化逻辑)                                       │
│    rayon                                                    │
│    clap                                                     │
│    ... 大部分 crates.io                                     │
│                                                            │
│  替代方案:                                                  │
│    tokio → epoll C API                                      │
│    serde → 手写 JSON 解析，或者调 jansson C 库              │
│    clap  → Scheme 自己的参数解析库                           │
└──────────────────────────────────────────────────────────┘
```

### 4.2 为什么 GPUI 是例外

GPUI 虽然是 Rust 写的，但因为它底层是 Vulkan/X11，你**不需要通过 Rust 去调它**：

```
GPUI (Rust) →  vulkano (Rust) →  libvulkan (C) → GPU
              ash (Rust) →
              winit (Rust) →  X11 (C)     → 窗口
  
你的路径:
              libvulkan (C) ← foreign-procedure
              X11 (C)       ← foreign-procedure
              freetype (C)  ← foreign-procedure

跳过 Rust，直调 C。和 GPUI 走一样的路，只是从不同的入口。
```

**你不是"集成了 GPUI"，你是"重新实现了 GPUI 底层那层 C 调用"。**

---

## 五、推荐实践

### 5.1 使用 Rust 的场景

```
适合 Rust 的场景:
  你想用 Rust 语法写一段逻辑，然后编译进二进制
  → 用 syn + rustc_driver 导出 AST
  → 翻译器处理
  → 正常工作

不适合 Rust 的场景:
  你想用 tokio / serde / axum / rayon / ... 这些依赖库
  → 没有 C ABI
  → 翻译器不知道怎么调
  → 找 C 替代方案，或者手写

折中方案:
  你的 Rust 代码 → 翻译器 → Scheme
  需要的系统能力 → foreign-procedure 调 C
  需要的库能力  → 找 C 替代，或者自己写
```

### 5.2 GUI 开发推荐栈

```
你最方便做 GUI 的路径:

  Scheme (你写)
    ↓
  simple-gui 库 (你写一次，永久复用)
    ├── XOpenDisplay (FFI)         ← 窗口
    ├── vkCreateInstance (FFI)     ← 渲染
    ├── vkCreateSwapchainKHR (FFI) ← 交换链
    ├── hb_shape (FFI)             ← 文本
    └── FT_Load_Glyph (FFI)        ← 字体
  
  不需要 Rust。
  不需要 GPUI。
  只需要 C library + foreign-procedure。
```

### 5.3 构建命令

```bash
# 1. Rust 代码 → JSON AST
rust-ast-dump my_module.rs > my_module.ast.json

# 2. AST → Scheme S-expr
chez --script rust-to-scheme.ss < my_module.ast.json > my_module.sls

# 3. 编译 + 链接
chez --compile main.sls --optimize-level 3 -o main.o
g++ -static -o app \
    main.o \
    deps/libvulkan.a \        # Vulkan C 库
    deps/libX11.a \            # X11 C 库
    deps/libfreetype.a \       # freetype C 库
    deps/libz.a \              # zlib
    -lchezscheme -lpthread -ldl -lm

# 4. 产物
./app  ← 你的 Rust 代码 + Vulkan 渲染的 GUI 程序
```

---

## 六、总结

```
Rust 在你的体系中的定位:

  ✅ 你可以翻 Rust 语法
     借助 syn + rustc_driver 导出 AST，翻译器处理
  
  ✅ 你可以用 Rust 调用的一切 C 库
     Vulkan / X11 / Wayland / freetype / ...
     跳过 Rust，直接 foreign-procedure
  
  ❌ 你不能用 Rust 的运行时生态
     std / tokio / serde / rayon / ...
     没有 C ABI，不能调

  引入 Rust 的原因:
     不是要用 Rust 生态，是为了 GPUI 这个 GUI 库。
     但 GPUI 的底层是 C，所以实际上可以跳过 Rust。
     真正需要的不是 "Rust 集成"，是 "C 图形库集成"。
```

## 七、Rust 的真正价值：C API 索引目录

### 7.1 Rust 不是目标语言，是索引

```
Rust crate 的作用不是让你用它——是告诉你底层有什么 C 库。

你想找一个 GUI 库:
  → 搜 "rust gui framework" → 找到 GPUI
  → 看 GPUI 源码 → 发现它调了 vulkan、X11、freetype、harfbuzz
  → 不写 Rust，直接 foreign-procedure 调这些 C 库

没有 Rust，你需要:
  自己去 Vulkan 文档里找 vkCreateInstance
  自己去 X11 文档里找 XOpenDisplay
  自己去 freetype 文档里找 FT_New_Face
  花 3 个月才能凑齐一个 GUI 栈

有 Rust:
  Rust 社区已经帮你踩过坑了
  GPUI 的 Cargo.toml 已经声明了所有依赖
  你知道"要做 GUI，需要这些 C 库"
  花 3 天就能凑齐
```

### 7.2 Rust 生态帮你省掉的时间

```
没有 Rust 的索引:                  有 Rust 的索引:
  找 GUI 库: 2 周                    找 GPUI: 10 分钟
  找渲染库: 2 周                    看 Cargo.toml: 10 分钟
  找文本库: 1 周                    知道要 freetype + harfbuzz
  找窗口库: 1 周                    知道要 X11 + Wayland
  组合调试: 4 周                    自己的坑自己踩，但大方向有了
  ──────────────────                ──────────────────
  总计: 10 周                        总计: 1 周
```

Rust 社区花了 10 年把 GUI 栈踩平了。你用 Rust 的查到的每个 Cargo.toml，都是那个 crate 的作者替你走完的路。

**你不是"用 Rust"，你是"用 Rust 社区帮你画好的 C 库藏宝图"。**

### 7.3 这就是最后一块拼图

```
你写的:      Python / PHP 业务逻辑    → 控制流翻译
你调的:      GPT 告诉你的 / Rust 索引到的 C 库  → foreign-procedure
你产出的:    ./app                    → 单文件 ELF

Python 写业务逻辑没有问题。
PyTorch 的 C++ 后端直接调没有问题。
GPUI 底层的 Vulkan/X11 直接调没有问题。

唯一的问题——"我怎么知道有哪些 C 库可用？"
Google 搜 3 天，GPT 问 1 天，Rust crates.io 搜 10 分钟。

不是因为 Rust 好，是因为 Rust 社区替你走了一遍路。
```

---

> **文档版本**: 2026-06-09
> **相关文件**: php_extend.md, python_extend.md, cpython_analysis_report.md
> **搜索命令示例**: 本文档中所有 cache_query 命令均可直接复制执行
