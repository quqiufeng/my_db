# LuaJIT AOT: Lua BC → Chez Scheme → ELF

> AI Agent 工作规格书。读完即可开始编码。

---

## 一、方案总览

```
Lua 源码  →  LuaJIT 解析器  →  BC 指令  →  Scheme S-expr  →  Chez AOT  →  ELF
               (复用)          (~70种)     (~1500行翻译器)    (45年后端)
```

**你写**:  一个 BC 指令 → Scheme S-expr 的翻译器
**不写**:  词法分析、语法分析、GC、寄存器分配、指令选择、异常处理、链接器

**等价对比 PHP 方案**:
- PHP:  AST 节点 (树状, ~50种) → Scheme, 控制流天然在树里
- Lua:  BC 指令 (扁平, ~70种) → Scheme, **需要先恢复控制流结构**

---

## 二、一周工期规划

```
Day 1:  BC 解码器           解析 luajit -bl 输出的 BC 文本格式
Day 2:  常量/算术/转移指令    KSHORT/KNUM/ADDVV/MOV/JMP → Scheme
Day 3:  控制流恢复            从 ISLT+JMP 模式重建 if/while/for/repeat
Day 4:  函数与表操作           CALL/RET/TGETV/TSETV/TNEW → Scheme + FFI
Day 5:  FFI 桥接              foreign-procedure 绑定 lj_tab.c/lj_str.c
Day 6:  闭包与上值             FNEW/UGET/USETV → lambda 闭包环境
Day 7:  集成 Chez AOT         compile-file + gcc -static, 跑通端到端
```

---

## 三、BC 指令格式

### 3.1 指令编码 (lj_bc.h)

```
32-bit 指令, 字段布局:

+--------+--------+--------+--------+     ← MSB (byte 3) ... LSB (byte 0)
|   B    |   C    |   A    |   OP   |     Format ABC: 3 个 8-bit 操作数
+--------+--------+--------+--------+
|     D (16-bit)    |   A    |   OP   |     Format AD:  1 个 16-bit 操作数
+--------+--------+--------+--------+
```

```c
// 字段提取 (全部宏定义)
bc_op(i) = i & 0xFF                       // 操作码
bc_a(i)  = (i >> 8) & 0xFF                // 目标寄存器 A
bc_b(i)  = (i >> 24)                      // 操作数 B
bc_c(i)  = (i >> 16) & 0xFF               // 操作数 C
bc_d(i)  = (i >> 16)                      // 操作数 D (16-bit)
bc_j(i)  = (int16_t)bc_d(i) - 0x8000      // 跳转偏移量 (有符号)
```

### 3.2 输入格式约定

使用 `luajit -bl` 的输出作为翻译器输入，每行一条指令：

```
0001    KSHORT   0   1         ; R(0) = 1
0002    KSHORT   1   2         ; R(1) = 2
0003    ADDVV    0   0   1     ; R(0) = R(0) + R(1)
0004    GGET     2   0         ; R(2) = _G["print"]
0005    MOV      3   0         ; R(3) = R(0)
0006    CALL     2   1   2     ; R(2) = print(R(3))
0007    RET0     0   1         ; return

格式: 偏移(4位hex)  TAB  指令名  TAB  操作数(空格分隔)  [;注释]
```

---

## 四、完整 BC 指令表（AI 可直接抄）

### 4.1 常量加载 (6 条)

```
指令        格式       操作数                           Scheme 翻译
────────────────────────────────────────────────────────────────────────
KSTR       dst,str     A=目标, C=字符串常量索引           (set! R(A) "str")
KNUM       dst,num     A=目标, C=数字常量索引             (set! R(A) num)
KSHORT     dst,lits    A=目标, C=int16_t 立即数           (set! R(A) C)
KPRI       dst,pri     A=目标, C=0:nil/1:false/2:true    (set! R(A) '()/#f/#t)
KNIL       base,base   A=起始, B=结束                     (set! R(A) '()) ... 批量
KCDATA     dst,cdata   A=目标, C=cdata 索引              (set! R(A) cdata)
```

### 4.2 转移与比较 (12 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
ISLT       var,var     if R(B) >= R(C) → skip D         (if (>= R(B) R(C)) (skip))
ISGE       var,var     if R(B) <  R(C) → skip D         (if (<  R(B) R(C)) (skip))
ISLE       var,var     if R(B) >  R(C) → skip D         (if (>  R(B) R(C)) (skip))
ISGT       var,var     if R(B) <= R(C) → skip D         (if (<= R(B) R(C)) (skip))
ISEQV      var,var     if R(B) != R(C) → skip D         (if (not (eqv? R(B) R(C))) (skip))
ISNEV      var,var     if R(B) == R(C) → skip D         (if (eqv? R(B) R(C)) (skip))
ISEQS      var,str     if R(B) != Kst(C) → skip D       同 ISEQV, 操作数类型不同
ISEQN      var,num     同上, 比较数字                     (if (not (= R(B) Knum(C))) (skip))
ISEQP      var,pri     同上, 比较原始类型                 同上模式
IST        var         if not R(B) → skip D             (if (not R(B)) (skip))
ISF        var         if R(B) → skip D                 (if R(B) (skip))
JMP        rbase,jump 无条件跳转 D                      (goto label)
```

**关键**: 所有 ISxx 指令的语义都是 **条件为假时跳转**, 跳转目标 = 当前 PC + 1 + D。

### 4.3 一元运算 (5 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
MOV        dst,var     R(A) = R(B)                      (set! R(A) R(B))
NOT        dst,var     R(A) = not R(B)                  (set! R(A) (not R(B)))
UNM        dst,var     R(A) = -R(B)                     (set! R(A) (- R(B)))
LEN        dst,var     R(A) = #R(B)                     (set! R(A) (length R(B)))
ISTC       dst,var     if R(B) then R(A)=true else skip  (set! R(A) R(B)) + skip
ISFC       dst,var     同上, 取反                        (set! R(A) (not R(B))) + skip
```

### 4.4 算术运算 (18 条)

操作数模式: `VV`=var+var, `VN`=var+num, `NV`=num+var

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
ADDVV      dst,var,var R(A) = R(B) + R(C)              (set! R(A) (+ R(B) R(C)))
SUBVV      dst,var,var R(A) = R(B) - R(C)              (set! R(A) (- R(B) R(C)))
MULVV      dst,var,var R(A) = R(B) * R(C)              (set! R(A) (* R(B) R(C)))
DIVVV      dst,var,var R(A) = R(B) / R(C)              (set! R(A) (/ R(B) R(C)))
MODVV      dst,var,var R(A) = R(B) % R(C)              (set! R(A) (remainder R(B) R(C)))
ADDVN/ADDNV     操作数 C 是数字常量                     用 Knum(C) 替换即可
SUBVN/SUBNV     同上                                    同上
MULVN/MULNV     同上                                    同上
DIVVN/DIVNV     同上                                    同上
MODVN/MODNV     同上                                    同上
POW        dst,var,var R(A) = R(B) ^ R(C)              (set! R(A) (expt R(B) R(C)))
CAT        dst,rbase   R(A) = R(B)..R(C)               (set! R(A) (string-append R(B) R(C)))
```

### 4.5 函数调用 (6 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
CALL       base,lit    R(A) = R(A)(R(A+1..A+B))        (set! R(A) (R(A) arg1 arg2 ...))
CALLM      base,lit    同上, 可变参数                    (set! R(A) (apply R(A) args))
CALLT      base,lit    尾部调用                          (R(A) arg1 arg2 ...)   ← 尾部位置!
CALLMT     base,lit    尾部调用, 可变参数                同上, 尾部
ITERC      base,lit    泛型 for 迭代器调用               (call-with-values iterator ...)
ITERN      base,lit    泛型 for 迭代器, 下一组           (call-with-values iterator next ...)
```

**B 操作数含义**: B=0 → 从栈顶取参数数量; B>0 → 参数个数
**C 操作数含义**: C=0 → 接受所有返回值; C>0 → 固定返回值个数

### 4.6 返回 (4 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
RET0       rbase,lit   return 无值                       (values)
RET1       rbase,lit   return R(A)                      (values R(A))
RET        rbase,lit   return R(A..A+C-1)               (values R(A) R(A+1) ...)
RETM       base,lit    return 可变参数                    (apply values args)
```

### 4.7 表操作 (11 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
TNEW       dst,lit     R(A) = new_table                  (set! R(A) (lua-table-new asize hsize))
TDUP       dst,tab     R(A) = copy_table(template)       (set! R(A) (lua-table-copy R(C)))
TGETV      dst,var,var R(A) = R(B)[R(C)]                 (set! R(A) (lua-table-ref R(B) R(C)))
TGETS      dst,var,str R(A) = R(B)["str"]               (set! R(A) (lua-table-ref R(B) 'str))
TGETB      dst,var,lit R(A) = R(B)[C]                    (set! R(A) (lua-table-ref-int R(B) C))
TGETR      dst,var,var R(A) = R(B)[R(C)]  (record)      (set! R(A) (lua-record-ref R(B) R(C)))
TSETV      var,var,var R(A)[R(C)] = R(B)                 (lua-table-set! R(A) R(C) R(B))
TSETS      var,var,str R(A)["str"] = R(B)               (lua-table-set! R(A) 'str R(B))
TSETB      var,var,lit R(A)[C] = R(B)                    (lua-table-set-int! R(A) C R(B))
TSETM      base,num    R(A)[R(A)+i] = R(B)[i]            (lua-table-set-multi! ...)
TSETR      var,var,var record 写入                        (lua-record-set! ...)
```

**策略**: 表操作全部走 FFI 调 LuaJIT 的 C 运行时, 不做 Scheme 重实现。

### 4.8 全局与上值 (8 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
GGET       dst,str     R(A) = _G["str"]                 (set! R(A) (lua-global-ref 'str))
GSET       var,str     _G["str"] = R(A)                 (lua-global-set! 'str R(A))
UGET       dst,uv      R(A) = upvalue[B]                 (set! R(A) (closure-ref B))
USETV       uv,var     upvalue[A] = R(B)                 (closure-set! A R(B))
USETS       uv,str     upvalue[A] = "str"               (closure-set! A 'str)
USETN       uv,num     upvalue[A] = num                  (closure-set! A num)
USETP       uv,pri     upvalue[A] = primitive             (closure-set! A pri)
UCLO       rbase,jump 关闭上值范围                        (closure-capture! ...)
```

### 4.9 循环 (10 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
FORI       base,jump  for 循环初始化  (for i=R(A),..,R(A+2))  (let ((i init) (limit end) (step inc)) ...)
JFORI      base,jump  同上 (JIT 版本, 直接翻译成相同的 Scheme)

FORL       base,jump  for 循环体                          (when (<= i limit) ... body ... (set! i (+ i step)) (loop))
IFORL      base,jump  同上 (解释器版本)
JFORL      base,lit   同上 (JIT 版本)

ITERL      base,jump  泛型 for 迭代                        迭代器调用
IITERL     base,jump  同上 (解释器版本)
JITERL     base,lit   同上 (JIT 版本)

LOOP       rbase,jump 循环头                              (let loop () ... (loop))
ILOOP      rbase,jump 同上 (解释器)
JLOOP      rbase,lit  同上 (JIT)
```

**策略**: 所有 `Ixxx`/`Jxxx` 变体与基础版本翻译相同, 区别只在 LuaJIT 解释器/JIT 分支, Scheme 层面一样处理。

### 4.10 函数头 (7 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
FUNCF      rbase      函数头 (固定参数)                    (lambda (R0 R1 ...) body)
IFUNCF     rbase      同上 (解释器)
JFUNCF     rbase,lit  同上 (JIT)
FUNCV      rbase      函数头 (可变参数)                    (lambda args body)
IFUNCV     rbase      同上 (解释器)
JFUNCV     rbase,lit  同上 (JIT)
FUNCC      rbase      C 函数头                           (foreign-procedure ...)
FUNCCW     rbase      C 函数包装                          (foreign-procedure ...)
```

### 4.11 其他 (4 条)

```
指令        格式       语义                              Scheme 翻译
────────────────────────────────────────────────────────────────────────
FNEW       dst,func   R(A) = closure                     (set! R(A) (lua-closure KProto(C)))
VARG       base,lit   可变参数读取                        (apply ...)
ISNEXT     base,jump  泛型 for 优化检查                   (if (is-next-iter R(A)) ... 跳过)
ISTYPE     var,lit    类型检查                            (when (not (lua-type? R(A) C)) (error ...))
ISNUM      var,lit    数字类型检查                        (when (not (number? R(A))) (error ...))
```

---

## 五、控制流恢复算法

这是最核心的部分。BC 指令是扁平的，需要还原成结构化控制流。

### 5.1 算法概述

```
输入: BC 指令序列 (顺序, 含 JMP/ISLT 等跳转指令)
输出: Scheme S-expr 树 (含 if/let/loop/begin)
```

```
Pass 1: 标记基本块边界
  - 跳转目标 → 基本块入口
  - 跳转指令后一条 → 基本块入口
  
Pass 2: 构建 CFG (控制流图)
  - 基本块之间建立 predecessor/successor 边

Pass 3: 识别结构化模式
  - if-then:     B1 → ISLT → JMP → B2 → B3      识别为 (if (cond) B2)
  - if-then-else: B1 → ISLT → JMP → B2 → JMP → B3  识别为 (if (cond) B2 B3)
  - while:       LOOP → body → ISLT → JMP → back  识别为 (let loop () (when cond) body (loop))
  - for:         FORI → FORL → body → JMP → back  识别为 (do ((i init (+ i step))) ((> i limit)) body)

Pass 4: 生成 Scheme
  - 递归遍历结构化基本块
  - 每个基本块内按顺序翻译 BC 指令
  - 翻译引用标签为嵌套 let/goto
```

### 5.2 模式识别细节

**if-then 模式**:
```
模式:    ISLT    R0 R1 → JMP +3
         JMP     +1
         <then-branch>
         JMP     +2
         <else-branch>      (可选)

识别:   ISLT/JMP 组合, 跳转目标在后方
输出:   (if (< R0 R1) then-branch else-branch)
```

**while 循环模式**:
```
模式:    LOOP                  ← 循环头
         ...
         ISLT    R0 R1 → JMP +3   ← 出口条件
         JMP     +1
         <loop-body>
         JMP     back to LOOP  ← 循环回边

识别:   LOOP 标记 + JMP 向后跳转到 LOOP
输出:   (let loop () (when (< R0 R1) ... body ... (loop)))
```

**for 循环模式**:
```
模式:    FORI    R0 → JMP +5       ← for init
         ...
         FORL    R0 → JMP +3       ← for check
         <loop-body>
         JMP     back to FORL      ← loop back edge

识别:   FORI + FORL 配对
输出:   (let ((i init) (limit end) (step inc))
            (let loop ()
              (when (if (>= step 0) (<= i limit) (>= i limit))
                ... body ...
                (set! i (+ i step))
                (loop))))
```

### 5.3 伪代码

```python
def reconstruct_cfg(bc_insns):
    """Pass 1 + 2: 构建 CFG"""
    # 标记基本块边界
    block_starts = {0}  # 第一条指令是入口
    for i, ins in enumerate(bc_insns):
        if is_branch(ins.op):
            target = ins.pc + 1 + ins.d  # 跳转目标
            block_starts.add(target)
            block_starts.add(i + 1)  # 跳转的下一条
        if ins.op in ('FORI', 'FUNCF', 'FUNCV', 'LOOP'):
            block_starts.add(i)  # 结构入口
    
    # 构建基本块
    blocks = []
    for start in sorted(block_starts):
        end = next_break(bc_insns, start, block_starts)
        blocks.append(Block(start, end, bc_insns[start:end]))
    
    return blocks

def structure_blocks(blocks):
    """Pass 3: 识别结构化模式"""
    # 按顺序遍历, 匹配模式
    if match_if_then(blocks):
        return IfThen(condition, then_block, else_block)
    elif match_while(blocks):
        return While(condition, body_block)
    elif match_for(blocks):
        return For(init, limit, step, body_block)
    else:
        return Sequence(blocks)

def translate_to_scheme(ast):
    """Pass 4: 生成 S-expr"""
    match ast:
        case IfThen(cond, then, else):
            return f'(if {expr(cond)} {block(then)} {block(else)})'
        case While(cond, body):
            return f'(let loop () (when {expr(cond)} {block(body)} (loop)))'
        case For(init, limit, step, body):
            return f'(do ((i {init} (+ i {step}))) ((> i {limit})) {block(body)})'
        case Sequence(insns):
            return '(begin ' + ' '.join(translate_ins(i) for i in insns) + ')'
```

---

## 六、FFI 桥接规格

### 6.1 必需绑定的运行时函数

```scheme
;; lua-runtime.ss — LuaJIT C 运行时 FFI 绑定

;; 表操作 (lj_tab.c)
(define lua-table-new
  (foreign-procedure "lj_tab_new" (lua_State* unsigned-32 unsigned-32) GCtab*))

(define lua-table-dup
  (foreign-procedure "lj_tab_dup" (lua_State* GCtab*) GCtab*))

(define lua-table-ref
  (foreign-procedure "lj_tab_get" (GCtab* TValue*) TValue*))

(define lua-table-set
  (foreign-procedure "lj_tab_set" (lua_State* GCtab* TValue*) TValue*))

(define lua-table-ref-int
  (foreign-procedure "lj_tab_getinth" (GCtab* int32) TValue*))

(define lua-table-set-int
  (foreign-procedure "lj_tab_setinth" (lua_State* GCtab* int32) TValue*))

(define lua-table-newkey
  (foreign-procedure "lj_tab_newkey" (lua_State* GCtab* TValue*) TValue*))

(define lua-table-reasize
  (foreign-procedure "lj_tab_reasize" (lua_State* GCtab* unsigned-32) void))

;; 字符串操作 (lj_str.c)
(define lua-str-new
  (foreign-procedure "lj_str_new" (lua_State* string int) GCstr*))

;; 函数/闭包 (lj_func.c)
(define lua-func-new-c
  (foreign-procedure "lj_func_newC" (lua_State* unsigned-32 GCtab*) GCfunc*))

(define lua-func-new-lua
  (foreign-procedure "lj_func_newL_empty" (lua_State* GCproto* GCtab*) GCfunc*))

;; GC (lj_gc.c)
(define lua-gc-step
  (foreign-procedure "lj_gc_step" (lua_State*) int))

;; 状态管理 (lj_state.c)
(define lua-state-new
  (foreign-procedure "lj_state_newstate" (void* void*) lua_State*))
```

### 6.2 值类型映射

```
C 类型                  Scheme 表示                  用途
────────────────────────────────────────────────────────
lua_State*              (foreign) LuaJIT 状态         全部运行时函数的第一个参数
GCtab*                  (foreign) Lua 表              表操作
TValue*                 (foreign) Lua 值              值传递
GCstr*                  (foreign) Lua 字符串           字符串操作
GCfunc*                 (foreign) Lua 函数             函数操作
GCproto*                (foreign) Lua 原型             闭包创建
uint32_t                unsigned-32                    表尺寸/哈希位
int32_t                 int32                          整数键
string                  string                         Scheme 字符串传给 C
```

---

## 七、编译与构建

### 7.1 构建 LuaJIT 静态库

```bash
cd /opt/luajit/src
make clean && make BUILDMODE=static CC=gcc
# 产出: libluajit.a
```

### 7.2 翻译流程

```bash
# Step 1: 生成 BC 指令文本
luajit -bl app.lua > app.bc.txt

# Step 2: 翻译 BC → Scheme
chez --script bc-to-scheme.ss < app.bc.txt > app.sls

# Step 3: AOT 编译 Scheme → .so
chez --compile app.sls --optimize-level 3

# Step 4: 静态链接
gcc -static -o app app.so /opt/luajit/src/libluajit.a \
    -lchezscheme -lpthread -ldl -lm

# Step 5: 运行
./app
```

### 7.3 调试命令

```bash
# 查看 BC 指令序列
luajit -bl test.lua

# 查看 BC + 寄存器分配
luajit -bld test.lua

# 只翻译特定的函数
luajit -bl test.lua | head -20 > test_subset.bc

# 验证 Chez Scheme 输出
chez --script app.sls    # 解释执行
chez --compile app.sls   # AOT 编译
```

---

## 八、质量检查清单

```
[ ] lj_bc.h 中的全部 ~70 种 BC 指令已枚举
[ ] 每条指令的格式 (ABC/AD) 和操作数字段已确认
[ ] 控制流恢复通过以下测试:
    [ ] if-then
    [ ] if-then-else
    [ ] while 循环
    [ ] repeat-until
    [ ] 数值 for 循环
    [ ] 泛型 for 循环
    [ ] 嵌套 if + 循环
[ ] FFI 绑定的运行时函数签名已从 C 源码验证
[ ] 以下最小程序端到端通过:
    [ ] 1+1: local a=1+2; print(a)          → 3
    [ ] if:   if a>b then print('ok') end   → ok
    [ ] for:  for i=1,3 do print(i) end     → 1 2 3
    [ ] table: local t={}; t.x=1; print(t.x) → 1
    [ ] func: function f(a,b) return a+b end; print(f(1,2)) → 3
    [ ] closure: local function f() return function() return 1 end end
[ ] Chez compile-file + gcc -static 成功
[ ] ELF 二进制运行零依赖
```

---

## 九、搜索命令速查

```bash
# BC 指令格式定义
cache_query "BCIns BC format" --repo /code/LuaJIT/LuaJIT --type context

# BC opcode 枚举 (完整指令清单)
cache_query "BCOp enum" --repo /code/LuaJIT/LuaJIT --type context

# 具体指令语义
cache_query "BC_CALL" --repo /code/LuaJIT/LuaJIT --type context

# 运行时函数签名
cache_query "lj_tab_set" --repo /code/LuaJIT/LuaJIT --type context
cache_query "lj_str_new" --repo /code/LuaJIT/LuaJIT --type context
cache_query "lj_func_newL_empty" --repo /code/LuaJIT/LuaJIT --type context

# 语义搜索 (找不到具体函数时)
cache_query "table set element" --repo /code/LuaJIT/LuaJIT --type search \
  --analysis-dir /opt/code_caches/LuaJIT_cache
```

---

> **文档版本**: 2026-06-10
> **相关文件**: luajit_analysis_report.md (架构分析), php_extend.md (同系列 PHP 方案)
