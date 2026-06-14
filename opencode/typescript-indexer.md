# TypeScript/Node.js 代码索引增强设计

> 为 coding.md 代码探索系统增加对 TypeScript/JavaScript 生态的深度解析能力

## 1. 为什么要增强

Node.js/npm 是世界上最大的开源代码生态之一：

- npm 注册表有超过 200 万个包
- 大量现代项目使用 TypeScript/JavaScript
- AI Agent 的开发场景大量涉及 Node.js 项目
- opencode 本身就是 TypeScript/Effect-TS 项目，解析质量直接影响分析效果

当前 `code_indexer` 基于 universal-ctags 做通用符号提取，对 C/C++/Python 效果很好，但对 TypeScript 有以下短板：

| 问题 | 影响 |
|------|------|
| 箭头函数识别不完整 | `const handler = () => {}` 被识别为变量而非函数 |
| 接口/类型别名噪音大 | `.d.ts` 和类型定义产生大量无执行逻辑的 chunks |
| Effect-TS 等工厂函数识别差 | `Layer.effect`、`Context.Service` 等模式抓不到清晰边界 |
| ES Module 调用图弱 | `import/export` 跨文件关系提取不充分 |
| JSX/TSX 组件边界不清 | React/Vue 组件的 props/state 关系难以提取 |

因此，需要为 TypeScript/JavaScript 增加专用解析器，显著提升索引质量。

## 2. 目标

1. 准确识别 TypeScript/JavaScript 各类符号：函数、类、方法、接口、类型别名、变量、装饰器
2. 正确提取箭头函数、异步函数、生成器函数
3. 提取 import/export 关系和跨文件调用边
4. 过滤 `.d.ts` 噪音，降低类型定义对语义搜索的干扰
5. 支持 `.ts`、`.tsx`、`.js`、`.jsx` 文件
6. 与现有 C 工具链兼容，统一输出 `chunks_meta.jsonl`

## 3. 技术选型：tree-sitter

### 3.1 为什么选 tree-sitter

| 方案 | 优点 | 缺点 |
|------|------|------|
| universal-ctags | 轻量、多语言、已集成 | 对 TS 现代语法支持有限 |
| TypeScript Compiler API | 最准确、有类型信息 | 需要 Node.js 运行时、解析慢、内存大、和 C 工具链风格不一致 |
| **tree-sitter** | 快速、准确、多语言统一、适合做符号和调用图 | 需要引入 parser 依赖 |

选择 **tree-sitter** 作为主要增强方案：

- 官方有 `tree-sitter-typescript`，支持 TS/TSX
- 解析速度快，适合大规模代码库
- 能产出完整 AST，支持符号提取、调用图、scope 分析
- 后续可扩展到其他语言（Rust/Go/Python 都有 tree-sitter grammar）

### 3.2 解析范围

```
.ts     TypeScript 源码
.tsx    TypeScript + JSX
.js     JavaScript 源码
.jsx    JavaScript + JSX
.d.ts   TypeScript 声明文件（降低权重或跳过）
```

## 4. 增强后的符号类型

在现有 kind 基础上，新增 TypeScript 专用 kind：

| Kind | 说明 | 示例 |
|------|------|------|
| `ts_function` | 函数声明、函数表达式、箭头函数 | `function foo() {}`、`const bar = () => {}` |
| `ts_method` | 类/对象方法 | `class A { foo() {} }` |
| `ts_class` | 类声明 | `class Foo {}` |
| `ts_interface` | 接口 | `interface Foo {}` |
| `ts_type_alias` | 类型别名 | `type Foo = {}` |
| `ts_enum` | 枚举 | `enum Color {}` |
| `ts_variable` | 变量声明 | `const foo = ...` |
| `ts_property` | 类/对象属性 | `class A { foo: number }` |
| `ts_import` | 导入 | `import { foo } from './bar'` |
| `ts_export` | 导出 | `export function foo()` |
| `ts_decorator` | 装饰器 | `@Component` |
| `ts_namespace` | 命名空间 | `namespace Foo {}` |
| `ts_jsx_component` | JSX 组件 | `function Component() {}` |

## 5. 调用图增强

### 5.1 提取 CallExpression

```typescript
// 识别以下调用形式
foo()                           → callee: foo
obj.bar()                       → callee: obj.bar
this.baz()                      → callee: this.baz
lib.fn(arg1, arg2)              → callee: lib.fn
someObj.method?.()              → callee: someObj.method
```

### 5.2 提取 Import/Export 关系

```typescript
// import { foo } from './bar'
// → 添加 cross-file edge: current_file → ./bar → foo

// import * as lib from './lib'
// → 添加 cross-file edge: current_file → ./lib → *

// export function foo() {}
// → 标记 foo 为 module export

// export { foo } from './bar'
// → 添加 re-export edge
```

### 5.3 跨文件解析

结合 `tsconfig.json` 或 package.json 的 `exports`/`main`，解析相对路径和路径别名：

```typescript
// tsconfig.json
{
  "compilerOptions": {
    "paths": {
      "@/*": ["./src/*"]
    }
  }
}

// import { foo } from '@/utils'
// → 解析为 ./src/utils.ts 或 ./src/utils/index.ts
```

## 6. 噪音过滤策略

| 来源 | 策略 |
|------|------|
| `.d.ts` 文件 | 默认跳过，或标记 `kind: ts_declaration` 降低搜索权重 |
| 测试文件 | 跳过 `*.test.ts`、`*.spec.ts`、`**/__tests__/**` |
| 类型定义 | 保留但标记为 `ts_interface`/`ts_type_alias`，语义搜索时降低权重 |
| 第三方代码 | 仍由 `node_modules/` 过滤规则排除 |
| 极小函数 | 行数少于 3 行的 getter/setter 可合并或跳过 |

## 7. 实现方案

### 7.1 Phase 1：快速验证（推荐先走这一步）

用 Node.js + tree-sitter 写一个独立 parser：

```typescript
// tools/ts_parser.ts
import Parser from "tree-sitter"
import TypeScript from "tree-sitter-typescript"

const parser = new Parser()
parser.setLanguage(TypeScript.typescript)

export function parseFile(path: string, source: string) {
  const tree = parser.parse(source)
  return extractChunks(tree.rootNode, path)
}

function extractChunks(node: Parser.SyntaxNode, path: string) {
  const chunks = []
  for (const child of node.children) {
    if (isFunctionNode(child)) {
      chunks.push({
        name: getFunctionName(child),
        kind: "ts_function",
        file: path,
        line_start: child.startPosition.row,
        line_end: child.endPosition.row,
        signature: child.text.slice(0, 200),
      })
    }
    // ... class, interface, method, import, etc.
  }
  return chunks
}
```

输出格式与现有 `chunks_meta.jsonl` 一致，直接复用 `batch_embedder` 和 `cache_import`。

### 7.2 Phase 2：集成到 code_indexer

在 `code_indexer.c` 中：

1. 检测文件扩展名 `.ts/.tsx/.js/.jsx`
2. 对这些文件调用 `ts_parser`（通过 fork + exec 或动态库）
3. 其他语言继续用 ctags
4. 统一输出 `chunks_meta.jsonl` 和 `chunks_text.txt`

```c
// 伪代码
if (is_typescript_or_javascript(file)) {
    chunks = ts_parser_extract(file);
} else {
    chunks = ctags_extract(file);
}
```

### 7.3 Phase 3：C 语言原生集成（可选优化）

如果 Node.js wrapper 性能不够，可将 tree-sitter parser 编译为 C 库，直接链接到 `code_indexer`：

```c
#include "tree_sitter/api.h"
#include "tree_sitter/typescript/parser.h"

TSParser* parser = ts_parser_new();
ts_parser_set_language(parser, tree_sitter_typescript());
```

优点：
- 无 Node.js 启动开销
- 多 worker 并行更轻量
- 和现有 C 工具链风格一致

缺点：
- 编译复杂
- 维护成本高

## 8. 输出格式

与现有 `chunks_meta.jsonl` 保持一致，新增字段可选：

```json
{
  "name": "toToolKind",
  "kind": "ts_function",
  "file": "packages/opencode/src/acp/tool.ts",
  "line_start": 38,
  "line_end": 71,
  "language": "typescript",
  "signature": "export function toToolKind(toolName: string): ToolKind",
  "tags": ["export"],
  "is_export": true,
  "imports": ["@agentclientprotocol/sdk"],
  "called_by": [],
  "calls": []
}
```

## 9. 验证计划

用 opencode 项目做前后对比：

| 指标 | ctags 旧版 | tree-sitter 新版 | 提升 |
|------|-----------|------------------|------|
| chunks 数 | 31691 | ? | 预期更合理（过滤噪音） |
| 函数识别数 | 2667 | ? | 预期显著提升 |
| 箭头函数识别 | 低 | 高 | 明显提升 |
| 调用图边 | 966 | ? | 预期提升 2-5x |
| 数据流变量 | 2000 | ? | 预期提升 |
| 语义搜索 Top-5 命中率 | ? | ? | 主观评估 |

验证命令：

```bash
# 旧版
/opt/my_db/ai_code_search.sh index /opt/opencode /opt/code_caches/opencode_ctags_cache 4

# 新版
/opt/my_db/ai_code_search.sh index /opt/opencode /opt/code_caches/opencode_ts_cache 4 --ts-parser

# 对比查询
./tools/cache_query "arrow function handler" --repo /code/opencode_ctags --type search
./tools/cache_query "arrow function handler" --repo /code/opencode_ts --type search
```

## 10. 与复刻 opencode 的关系

TypeScript 解析增强不仅服务于 coding.md 通用分析能力，也直接支持我们复刻 opencode：

1. **深度理解 opencode 源码**：更准确的调用图和符号索引
2. **复刻实现参考**：可以精确提取 opencode 的模块边界、工具注册流程、消息状态机
3. **第三方依赖索引**：用户项目中的 `node_modules/` 可以高质量索引
4. **Agent 工具设计**：参考 opencode 的 `toToolKind` 分类，设计我们自己的工具权限模型

## 11. 一句话总结

> **为 TypeScript/JavaScript 增加 tree-sitter 专用解析器，把 coding.md 从"能分析 TS"提升到"深度理解 TS"，这是覆盖现代 AI Agent 开发场景的关键投入。**
