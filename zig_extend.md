# Zig AOT 编译方案：第一公民 C ABI = 0 桥接层

> 本文档描述如何利用 Zig 编写高性能模块，通过 native C ABI 直接链接到 Chez Scheme，
> 无需翻译、无需桥接、零开销。Zig 是整个体系中"手写性能模块"的最佳选择。

---

## 一、Zig 在体系中的位置

### 1.1 和其他语言的根本区别

```
PHP/Python/LuaJIT:
  运行时是 C → 探索 → 翻译 → Scheme
  输入是脚本，输出是机器码

Zig:
  运行时不存在 → 编译直接出 .a
  不需要翻译 → 直接 foreign-procedure
  输入是 C ABI 兼容的模块，输出是链接进 ELF 的 .a

Zig 不是"翻译输入语言"——它是"手工性能模块的编译工具"。
```

### 1.2 和 Rust 的对比

```
Rust:                       Zig:
  运行时 = Rust，没有 C ABI    运行时 = 不存在，C ABI 是第一公民
  生态 = crates.io            生态 = C 库全兼容
  桥接 = 需要 extern "C" 包装  桥接 = export fn 就是 C ABI
  翻译 = 需要 syn + 翻译器     不翻译 = 直接编译 .a

Zig 不需要"集成"——它天生就在你的体系里。
你的体系: foreign-procedure 调一切 C ABI。
Zig:      编译产物就是 C ABI。
```

---

## 二、使用方式

### 2.1 写一个 Zig 模块

```zig
// math.zig — 你的高性能模块
export fn process_tensor(data: [*]f32, len: usize) f32 {
    var sum: f32 = 0;
    for (data[0..len]) |val| {
        sum += val * val;
    }
    return sum;
}
```

### 2.2 编译成 .a

```bash
zig build-lib math.zig -target x86_64-linux-musl \
    --name math -dynamic
# → libmath.a (标准静态库)
```

### 2.3 Scheme 直接调

```scheme
;; 不需要翻译器，直接 foreign-procedure
(define process-tensor
  (foreign-procedure "process_tensor"
    (void* int64) float32))

(process-tensor data-ptr len)
```

### 2.4 链接进 ELF

```bash
g++ -static -o app main.o \
    libmath.a \          # Zig 编译的
    libpython.a \        # Python 运行库
    libvulkan.a \        # GUI 渲染库
    -lchezscheme
```

---

## 三、Zig 独特的价值

### 3.1 零桥接层

```
Rust 写高性能模块:
  #[no_mangle] pub extern "C" fn ...   ← 每写一个函数加一次
  unsafe { ... }                        ← 跨 FFI 不安全
  需要手动 Pin / 手动管理内存            ← 麻烦

Zig 写高性能模块:
  export fn ...                          ← 就是 C ABI，不用加标记
  指针就是指针，内存就是内存              ← 不需要 unsafe
  C 库 #include 直接可用                 ← 不需要包装
```

### 3.2 替代 Rust 的手写性能层

```
你之前想在体系里加 Rust 的原因:
  Rust 可以写高性能模块编译成 .a
  但 Rust 的运行时没有 C ABI，桥接麻烦

Zig 解决这个问题:
  同样的性能（编译到 LLVM IR 做优化）
  同样的 .a 输出
  但不需要桥接层——export fn 就是 C ABI
  标准库也是 C ABI 兼容的（用 musl libc）
```

---

## 四、总结

```
Zig 在你的体系里的定位:

  不是"翻译输入语言"
  是"手工高性能模块的编译工具"

  Python/PHP 写业务逻辑 → 翻译器 → Scheme
  Zig 写性能热点        → 直接 .a  → foreign-procedure
  C 库做通用功能        → 直接 .a  → foreign-procedure

  三者的关系:
    Zig 补 Python/PHP 的性能短板
    Zig 补 Rust 的 C ABI 缺失
    Zig 本身是你体系的原住民——不需要任何适配。
```

> **文档版本**: 2026-06-09
