# PHP 源码架构分析报告

> 生成方式: 基于已索引的 PHP 源码（`/opt/php/src/`）通过语义搜索 + context 查询完成的 AI Agent 分析（零直接文件读取）
> 分析日期: 2026-06-09

---

## 一、整体架构概览

PHP 源码分为四大层级：

```
┌──────────────────────────────────────────────────────┐
│                    SAPI Layer                          │
│  (CLI / PHP-FPM / Apache mod_php / phpdbg / Embed)   │
├──────────────────────────────────────────────────────┤
│              Zend Engine (核心)                        │
│  ┌────────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │  Compiler   │  │    VM    │  │  Memory Manager  │  │
│  │(zend_compile│  │(zend_vm_ │  │  (zend_alloc.c)  │  │
│  │   .c)       │  │execute.h)│  │                  │  │
│  └────────────┘  └──────────┘  └──────────────────┘  │
│  ┌────────────┐  ┌──────────┐  ┌──────────────────┐  │
│  │  Data types │  │  Object  │  │  Extension API   │  │
│  │(zend_types │  │  System  │  │  (zend_API.c)    │  │
│  │   .h/.c)   │  │(zend_obj │  │                  │  │
│  │            │  │  ects.c) │  │                  │  │
│  └────────────┘  └──────────┘  └──────────────────┘  │
├──────────────────────────────────────────────────────┤
│                 Extension Layer                        │
│  (standard / pdo / mbstring / curl / gd / ... 80+)    │
├──────────────────────────────────────────────────────┤
│              Main / SAPI Glue Layer                    │
│  (main/SAPI.h, main/main.c, main/php.h, ...)         │
└──────────────────────────────────────────────────────┘
```

**源码规模**: 2375 个 C 文件, 61866 个代码块（函数/方法/类定义）
**核心目录**:
- `Zend/` — Zend 引擎（189 文件）：编译器、VM、内存管理、核心类型
- `main/` — PHP 核心基础设施（79 文件）：SAPI 接口、INI 解析、请求生命周期
- `sapi/` — 服务器接口实现（148 文件）：CLI、FPM、Apache2Handler、phpdbg、Embed
- `ext/` — 扩展（1677 文件）：80+ 标准扩展

---

## 二、核心数据结构

### 2.1 zval — 通用值类型

**文件**: `Zend/zend_types.h:341`

```c
struct _zval_struct {
    zend_value        value;         // 实际值（联合体）
    union {
        uint32_t type_info;          // 类型信息（高位=flags, 低位=type）
        struct {
            uint8_t    type;         // IS_NULL, IS_LONG, IS_STRING, IS_ARRAY...
            uint8_t    type_flags;
            uint16_t   extra;
        } v;
    } u1;
    union {
        uint32_t     next;           // 哈希冲突链
        uint32_t     cache_slot;     // 缓存槽
        uint32_t     opline_num;     // opcode 编号
        uint32_t     lineno;         // AST 节点行号
        uint32_t     fe_pos;         // foreach 位置
        uint32_t     guard;          // 递归/属性保护
        uint32_t     constant_flags; // 常量标志
    } u2;
};
```

### 2.2 zend_string — 字符串

**文件**: `Zend/zend_types.h:379`

```c
struct _zend_string {
    zend_refcounted_h gc;    // 引用计数 + GC 信息
    zend_ulong        h;     // 哈希值（预计算，加速 HashTable 查找）
    size_t            len;   // 字符串长度（二进制安全）
    char              val[1];// 内联数据（柔性数组，实际分配 len+1）
};
```

**搜索命中**: `zend_string_init` (score=0.8309, `zend_string.h:202`)
- 引用计数 + COW
- `h` 字段预计算 DJB33A 哈希值
- `val[1]` 柔性数组技巧

### 2.3 HashTable — 有序字典

**文件**: `Zend/zend_types.h:394`

```c
struct _zend_array {
    zend_refcounted_h gc;
    uint32_t          flags;         // 打包数组/只读
    uint32_t          nTableMask;    // 哈希掩码
    union {
        uint32_t     *arHash;        // 哈希索引表（指向 Bucket 索引）
        Bucket       *arData;        // Bucket 数组（有序）
        zval         *arPacked;      // 打包数组优化
    };
    uint32_t          nNumUsed;      // 已用 Bucket 数
    uint32_t          nNumOfElements;// 有效元素数
    uint32_t          nTableSize;    // 容量（2 的幂）
    uint32_t          nInternalPointer; // foreach 内部指针
    zend_long         nNextFreeElement;
    dtor_func_t       pDestructor;
};
```

**内存布局**:
```
┌─────────────────────────┐  ← arData 向下
│ HT_HASH(ht, -1)          │    哈希索引表
│ ...                      │
│ HT_HASH(ht, -nTableMask) │
├─────────────────────────┤  ← arData
│ Bucket[0]               │    {zval, hash, key}
│ ...                      │
│ Bucket[nTableSize-1]    │
└─────────────────────────┘
```

### 2.4 zend_object — 对象

**文件**: `Zend/zend_types.h:562`

```c
struct _zend_object {
    zend_refcounted_h gc;
    uint32_t          handle;
    uint32_t          extra_flags;
    zend_class_entry *ce;
    const zend_object_handlers *handlers; // 虚函数表
    HashTable        *properties;
    zval              properties_table[1];
};
```

### 2.5 zend_class_entry — 类

**文件**: `Zend/zend.h:151`

```c
struct _zend_class_entry {
    zend_class_type type;      // class/interface/trait/enum
    zend_string *name;
    uint32_t ce_flags;
    HashTable function_table;  // 方法表
    HashTable properties_info; // 属性信息
    HashTable constants_table; // 常量表
    zend_function *constructor, *destructor, *clone;
    zend_function *__get, *__set, *__call, *__tostring;
    const zend_object_handlers *default_object_handlers;
};
```

---

## 三、内存管理 (Zend MM)

**文件**: `Zend/zend_alloc.c` (3589 行)

```c
struct _zend_mm_heap {
    zend_mm_free_slot *free_slot[ZEND_MM_BINS]; // 小对象空闲链表
    zend_mm_huge_list *huge_list;                // 大块分配
    zend_mm_chunk     *main_chunk;               // 主 chunk
    zend_mm_chunk     *cached_chunks;            // 缓存 chunk
};
```

**分配路径**: `_emalloc` (score=0.9277) → `_zend_mm_alloc` (score=0.9224) → `zend_mm_alloc_heap` (score=0.8909)

分配策略: 小对象(free_slot) → 中等(chunk页面) → 大对象(mmap)

---

## 四、编译器管线

**文件**: `Zend/zend_compile.c` (12705 行)

```
PHP 源码 → 词法分析(Lexer) → 语法分析(Bison) → AST
         → zend_compile_stmt() → zend_op_array → VM 执行
```

### 关键入口函数

| 函数 | 位置 | score | 功能 |
|------|------|-------|------|
| `init_compiler` | zend_compile.c:453 | — | 初始化编译器 |
| `zend_compile_stmt` | zend_compile.c:11981 | 0.8589 | AST→opcodes 主分派 |
| `zend_compile_expr_inner` | zend_compile.c:12109 | 0.8530 | 表达式编译 |
| `zend_compile_dim` | zend_compile.c:3149 | — | 数组下标编译 |
| `zend_compile_prop` | zend_compile.c:3235 | — | 属性访问编译 |
| `zend_compile_assign` | zend_compile.c:103 | — | 赋值编译 |

```c
// init_compiler: 创建编译器 arena
void init_compiler(void) {
    CG(arena) = zend_arena_create(64 * 1024);
    CG(active_op_array) = NULL;
    zend_init_compiler_data_structures();
    zend_init_rsrc_list();
    zend_stream_init();
}
```

```c
// zend_compile_stmt: 按 AST 节点类型分派
static void zend_compile_stmt(zend_ast *ast) {
    switch (ast->kind) {
        case ZEND_AST_STMT_LIST: zend_compile_stmt_list(ast); break;
        case ZEND_AST_RETURN:    zend_compile_return(ast);    break;
        case ZEND_AST_ECHO:      zend_compile_echo(ast);      break;
        case ZEND_AST_IF:        zend_compile_if(ast);        break;
        // ... 50+ 种节点类型
    }
}
```

---

## 五、Zend VM 执行

**文件**: `Zend/zend_vm_execute.h` (~123000 行，自动生成)

使用 **标签指针分派**（`goto *label` 直接跳转）：

```c
#define ZEND_OPCODE_HANDLER_ARGS  zend_execute_data *execute_data, const zend_op *opline
#define ZEND_VM_ENTER()  execute_data = EG(current_execute_data); LOAD_OPLINE(); ...
#define ZEND_VM_LEAVE()  return (zend_op*)((uintptr_t)opline | ZEND_VM_ENTER_BIT)
```

| Handler | 位置 | score | 功能 |
|---------|------|-------|------|
| `zend_vm_call_opcode_handler` | zend_vm_execute.h:123303 | 0.8928 | 通用 opcode 调用入口 |
| `ZEND_USER_OPCODE_SPEC_HANDLER` | zend_vm_execute.h:3458 | 0.8930 | 用户自定义 opcode |
| `ZEND_CONCAT_SPEC_CV_CONST_HANDLER` | zend_vm_execute.h:41168 | 0.7823 | 字符串拼接 |

---

## 六、扩展系统

**文件**: `Zend/zend_modules.h`, `Zend/zend_API.c`

```c
struct _zend_module_entry {
    const char *name;
    const zend_function_entry *functions;     // PHP 函数表
    zend_result (*module_startup_func)();     // MINIT
    zend_result (*request_startup_func)();    // RINIT
    zend_result (*request_shutdown_func)();   // RSHUTDOWN
    zend_result (*module_shutdown_func)();    // MSHUTDOWN
    const char *version;
};
```

| 阶段 | 函数 | score |
|------|------|-------|
| MINIT | `zend_startup_module` (zend_API.c:3281) | 0.9198 |
| RINIT | `zend_activate_modules` (zend_API.c:3399) | 0.8621 |

---

## 七、SAPI 层

**文件**: `main/SAPI.h:238`

```c
struct _sapi_module_struct {
    char *name;                             // "cli" / "fpm" / "apache2handler"
    int (*startup)(struct _sapi_module_struct*);
    int (*activate)(void);                  // 请求激活
    int (*deactivate)(void);
    size_t (*ub_write)(const char*, size_t); // 输出
    size_t (*read_post)(char*, size_t);     // POST 读取
    char *(*read_cookies)(void);
    void (*register_server_variables)(zval*); // $_SERVER
};
```

---

## 八、请求生命周期

```
SAPI 接收请求
  → php_request_startup()
    → zend_activate_modules() (RINIT)
    → init_compiler() (首次)
  → zend_compile_file() → zend_op_array
  → zend_execute() → opcode dispatch loop
  → php_request_shutdown()
    → zend_deactivate_modules() (RSHUTDOWN)
    → zend_mm_shutdown() (整块释放)
```

---

## 九、关键设计决策

| 特性 | 实现 | 收益 |
|------|------|------|
| **COW** | 引用计数+预修改复制 | 减少内存复制 |
| **有序哈希表** | arData顺序链+arHash索引 | 有序遍历+O(1)查找 |
| **打包数组** | 整数键→zval[]退化 | O(1)随机访问 |
| **Zend MM** | 按请求分heap+free_slot | 整块释放，无碎片 |
| **Opcode直接跳转** | goto *label | 零开销分派 |
| **JIT** | IR编译→机器码 | 热点加速 |

---

## 十、文件索引速查

| 类别 | 文件 | 行数 | 参考行 |
|------|------|------|--------|
| 类型系统 | `Zend/zend_types.h` | 1594 | zval:341, string:379, HashTable:394, object:562 |
| 内存管理 | `Zend/zend_alloc.c` | 3589 | heap:265, emalloc:2774 |
| 编译器 | `Zend/zend_compile.c` | 12705 | init:453, compile_stmt:11981 |
| VM | `Zend/zend_vm_execute.h` | ~123K | 自动生成 |
| 扩展API | `Zend/zend_API.c` | — | startup_module:3281 |
| 类 | `Zend/zend.h` | 464 | class_entry:151 |
| SAPI | `main/SAPI.h` | 346 | sapi_module:238 |

---

> **报告生成方式**: 全部 struct 定义通过 `cache_query --type context` 语义搜索获取,
> 编译器/VM 源码通过 `--type search` 定位关键函数后读取内容（不再直接 `read` 源码文件）。
>
> 可复现的搜索命令:
> ```bash
> export LD_LIBRARY_PATH="/opt/my_db:/data/cuda/lib64:\${LD_LIBRARY_PATH}"
> # 获取 struct 定义
> tools/cache_query "_zval_struct" --repo /code/php --type context
> tools/cache_query "_zend_string" --repo /code/php --type context
> tools/cache_query "_zend_array" --repo /code/php --type context
> tools/cache_query "_zend_object" --repo /code/php --type context
> tools/cache_query "_zend_mm_heap" --repo /code/php --type context
> # 获取函数源码
> tools/cache_query "init_compiler" --repo /code/php --type context
> tools/cache_query "zend_compile_stmt" --repo /code/php --type context
> # 语义搜索
> tools/cache_query "zend_compiler" --repo /code/php --type search --analysis-dir /opt/code_caches/php_cache
> tools/cross_search.sh "PHP object system" 5
> ```
