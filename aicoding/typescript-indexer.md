# 代码索引插件：TypeScript/Node.js 解析增强

> coding.md 的第一个专用语言索引插件

## 1. 为什么做成插件

coding.md 的 `code_indexer` 基于 universal-ctags，对 C/C++/Python 效果很好。但现代语言（TypeScript、Rust、Go 等）有各自复杂的语法特性，ctags 难以全面覆盖。

把语言增强做成**插件模式**的好处：

| 优势 | 说明 |
|------|------|
| 解耦 | 核心索引器保持通用，语言特化逻辑交给插件 |
| 可扩展 | 未来加 Rust、Go、Java 插件，走同一套接口 |
| 可独立迭代 | TS 插件升级不需要改 `code_indexer` |
| 可组合 | 一个插件处理多种相近语言（如 `.ts/.tsx/.js/.jsx`） |
| 可禁用 | 不需要时直接移除插件配置 |

TypeScript/Node.js 是第一个专用语言插件，因为：

- npm 是世界上最大的开源代码生态
- 现代 AI Agent 大量基于 TypeScript/Node.js
- opencode 本身就是 TypeScript/Effect-TS 项目

## 2. 插件接口设计

### 2.1 插件注册文件

`code_indexer` 启动时读取插件注册表：

```json
{
  "$schema": "https://opencode.ai/coding-indexer-plugin-schema.json",
  "plugins": [
    {
      "name": "typescript",
      "version": "1.0.0",
      "description": "Enhanced TypeScript/JavaScript parser using tree-sitter",
      "extensions": [".ts", ".tsx", ".js", ".jsx"],
      "languages": ["typescript", "tsx", "javascript", "jsx"],
      "command": ["/opt/my_db/plugins/typescript-indexer/bin/typescript-indexer"],
      "env": {
        "TS_PARSER_MAX_FILE_SIZE": "1048576"
      },
      "priority": 100
    }
  ]
}
```

### 2.2 插件定位

```
plugins/
├── typescript-indexer/
│   ├── plugin.json          # 插件元数据
│   ├── Makefile             # 构建脚本
│   ├── src/
│   │   ├── main.c           # 入口与参数解析
│   │   ├── parser.c         # tree-sitter 解析
│   │   ├── extractor.c      # 符号提取
│   │   ├── callgraph.c      # 调用图提取
│   │   └── import_export.c  # 导入导出解析
│   └── bin/
│       └── typescript-indexer  # 编译后的二进制
```

### 2.3 code_indexer 调用方式

```bash
code_indexer 遇到 .ts 文件时
  │
  ▼
检查 plugin.json：是否有匹配 .ts 的插件？
  │
  ├─ 有 → 调用插件处理该文件
  │         插件输出 JSON Lines 到 stdout
  │         code_indexer 读取并合并到 chunks_meta.jsonl
  │
  └─ 无 → 回退到 ctags 通用解析
```

### 2.4 插件输入

通过命令行参数传入：

```bash
/opt/my_db/plugins/typescript-indexer/bin/typescript-indexer \
  --file /opt/opencode/packages/core/src/session/message.ts \
  --project /opt/opencode
```

### 2.5 插件输出格式

插件输出 **JSON Lines**，每行一条记录。支持两种类型：

#### chunk 记录

```json
{
  "type": "chunk",
  "name": "toLLMMessage",
  "kind": "ts_function",
  "file": "packages/core/src/session/runner/to-llm-message.ts",
  "line_start": 93,
  "line_end": 145,
  "language": "typescript",
  "signature": "function toLLMMessage(message: SessionMessage.Message, model: Model): Message[]",
  "content": "function toLLMMessage(message: SessionMessage.Message, model: Model): Message[] {\n  switch (message.type) {\n    ...\n  }\n}",
  "tags": ["export"],
  "is_export": true,
  "is_default_export": false
}
```

#### call_edge 记录

```json
{
  "type": "call_edge",
  "caller": "toLLMMessage",
  "caller_file": "packages/core/src/session/runner/to-llm-message.ts",
  "caller_line": 102,
  "callee": "Message.make",
  "callee_file": null,
  "callee_line": null,
  "kind": "direct"
}
```

#### import_edge 记录

```json
{
  "type": "import_edge",
  "source_file": "packages/core/src/session/runner/to-llm-message.ts",
  "target_file": "packages/core/src/session/message.ts",
  "symbols": ["SessionMessage"],
  "kind": "named_import"
}
```

#### metadata 记录（可选，每个文件输出一次）

```json
{
  "type": "metadata",
  "file": "packages/core/src/session/runner/to-llm-message.ts",
  "language": "typescript",
  "stats": {
    "functions": 2,
    "classes": 0,
    "interfaces": 0,
    "imports": 5
  }
}
```

### 2.6 回退机制

如果插件进程退出非零、输出为空或解析失败，`code_indexer` 自动回退到 ctags。

## 3. 插件生命周期

```
1. code_indexer 启动
   │
   ▼
2. 读取 plugin registry（默认 ~/.config/coding/indexer-plugins.json）
   │
   ▼
3. 扫描源码目录，按扩展名匹配插件
   │
   ▼
4. 对每个匹配文件 fork 子进程调用插件
   │
   ▼
5. 插件输出 JSON Lines → code_indexer 解析
   │
   ▼
6. 合并到统一的 chunks_meta.jsonl / chunks_text.txt
   │
   ▼
7. 无插件匹配的文件走 ctags 默认流程
```

## 4. TypeScript 插件实现

### 4.1 技术选型：tree-sitter C API

插件使用 `tree-sitter` C 核心库 + `tree-sitter-typescript` 原生 parser。不依赖 Node.js / npm，运行时仅需要 `libjansson.so.4` 与 `libc`。

```c
// plugins/typescript-indexer/src/parser.c
#include <tree_sitter/api.h>

extern const TSLanguage *tree_sitter_typescript(void);
extern const TSLanguage *tree_sitter_tsx(void);

TSParser *create_parser(const char *path) {
  TSParser *parser = ts_parser_new();
  if (strstr(path, ".tsx"))
    ts_parser_set_language(parser, tree_sitter_tsx());
  else if (strstr(path, ".ts"))
    ts_parser_set_language(parser, tree_sitter_typescript());
  return parser;
}
```

构建时把 `libtree-sitter.a` 静态链接进二进制，避免目标机器安装 `libtree-sitter.so`：

```makefile
LDFLAGS += -L/opt/tree-sitter -l:libtree-sitter.a -ljansson
```

### 4.2 提取的符号类型

| Kind | tree-sitter 节点 | 说明 |
|------|-----------------|------|
| `ts_function` | `function_declaration`、`function_expression`、`arrow_function` | 函数 |
| `ts_method` | `method_definition` | 类/对象方法 |
| `ts_class` | `class_declaration` | 类 |
| `ts_interface` | `interface_declaration` | 接口 |
| `ts_type_alias` | `type_alias_declaration` | 类型别名 |
| `ts_enum` | `enum_declaration` | 枚举 |
| `ts_variable` | `variable_declarator` | 变量 |
| `ts_property` | `property_definition`、`public_field_definition` | 属性 |
| `ts_import` | `import_declaration` | 导入 |
| `ts_export` | `export_statement` | 导出 |
| `ts_decorator` | `decorator` | 装饰器 |
| `ts_namespace` | `module_declaration` | 命名空间 |
| `ts_jsx_component` | 函数/类且返回 JSX | React/Vue 组件 |

### 4.3 调用图提取

遍历 `call_expression` 节点，记录当前函数与 callee 名称：

```c
// plugins/typescript-indexer/src/callgraph.c
void extract_calls(TSNode node, const char *current_func) {
  if (strcmp(ts_node_type(node), "call_expression") == 0) {
    TSNode callee = ts_node_child_by_field_name(node, "function", 8);
    const char *callee_name = get_callee_name(callee);
    printf("{\"type\":\"call_edge\",\"caller\":\"%s\",\"callee\":\"%s\"}\n",
           current_func, callee_name);
  }
  uint32_t count = ts_node_named_child_count(node);
  for (uint32_t i = 0; i < count; i++) {
    extract_calls(ts_node_named_child(node, i), current_func);
  }
}
```

### 4.4 Import/Export 解析

```c
// plugins/typescript-indexer/src/import_export.c
void extract_import(TSNode node, const char *current_file) {
  TSNode source = ts_node_child_by_field_name(node, "source", 6);
  char module[256];
  get_node_text(source, module, sizeof(module));

  uint32_t count = ts_node_named_child_count(node);
  for (uint32_t i = 0; i < count; i++) {
    TSNode spec = ts_node_named_child(node, i);
    if (strcmp(ts_node_type(spec), "import_specifier") == 0) {
      const char *name = get_identifier(spec);
      printf("{\"type\":\"import_edge\",\"source_file\":\"%s\","
             "\"target_file\":\"%s\",\"symbols\":[\"%s\"]}\n",
             current_file, resolve_import_path(module, current_file), name);
    }
  }
}
```

### 4.5 噪音过滤

| 来源 | 策略 |
|------|------|
| `.d.ts` | 默认跳过，或标记 `kind: ts_declaration` 且降低搜索权重 |
| 测试文件 | 跳过 `*.test.ts`、`*.spec.ts`、`**/__tests__/**` |
| 类型定义 | 保留但单独标记 kind，语义搜索时降低权重 |
| 极小函数 | 少于 3 行的 getter/setter 可合并 |
| node_modules | 由外层 exclude 规则控制 |

## 5. 实现步骤

### Phase 1：插件框架 MVP

1. 定义 `plugin.json` schema
2. 修改 `code_indexer` 读取 registry 并 fork 插件进程
3. 解析插件 JSON Lines 输出
4. 无插件时回退 ctags
5. 输出统一格式到 `chunks_meta.jsonl`

### Phase 2：TypeScript 插件 MVP

1. 创建 `plugins/typescript-indexer/`
2. 用 tree-sitter 实现基本符号提取
3. 输出 chunk 记录
4. 验证 opencode 项目

### Phase 3：调用图与导入导出

1. 实现 `call_edge` 输出
2. 实现 `import_edge` 输出
3. code_indexer 合并到 `call_graph.json`

### Phase 4：噪音过滤与权重

1. `.d.ts` 过滤/降权
2. 测试文件跳过
3. 类型定义单独标记

### Phase 5：性能优化

1. 插件内批量处理（一次处理多个文件）
2. 多进程并行
3. 使用 C 原生 tree-sitter 绑定（已实现）
4. 静态链接 tree-sitter 核心，简化部署

## 6. 与现有工具链的集成

```
analyze_repo.sh /opt/opencode /code/opencode
  │
  ▼
code_indexer 扫描文件
  ├─ .ts/.tsx/.js/.jsx → 调用 typescript-indexer 插件
  ├─ .c/.h/.cpp/.py   → 走 ctags
  │
  ▼
统一生成 chunks_meta.jsonl + chunks_text.txt
  │
  ▼
batch_embedder 生成向量
  │
  ▼
call_graph / dataflow 分析
  │
  ▼
cache_import 写入 /memory/
```

## 7. 验证计划

用 opencode 做 A/B 测试：

| 指标 | ctags | typescript-plugin | 提升 |
|------|-------|-------------------|------|
| 函数识别数 | 2667 | ? | 预期 +30~50% |
| 箭头函数识别 | 低 | 高 | 明显提升 |
| 调用图边 | 966 | ? | 预期 +3~5x |
| chunks 数 | 31691 | ? | 可能减少（过滤噪音） |
| 语义搜索命中率 | ? | ? | 主观评估提升 |

验证命令：

```bash
# 启用插件
/opt/my_db/ai_code_search.sh index /opt/opencode /opt/code_caches/opencode_ts_cache 4 \
  --plugins /opt/my_db/plugins/typescript-indexer/plugin.json

# 对比查询
./tools/cache_query "arrow function handler" --repo /code/opencode_ts --type search
./tools/cache_query "toToolKind" --repo /code/opencode_ts --type context --depth 2
```

## 8. 对其他语言插件的启示

TypeScript 插件成功后，可以用同样接口扩展：

| 语言 | 插件名 | 解析器 |
|------|--------|--------|
| Rust | rust-indexer | tree-sitter-rust |
| Go | go-indexer | tree-sitter-go |
| Java | java-indexer | tree-sitter-java |
| Ruby | ruby-indexer | tree-sitter-ruby |
| C# | csharp-indexer | tree-sitter-c-sharp |

所有插件统一遵循：

- `plugin.json` 注册
- 输入：文件路径
- 输出：JSON Lines（chunk / call_edge / import_edge / metadata）
- 失败回退 ctags

## 9. 与复刻 opencode 的关系

这个插件设计本身也服务于复刻 opencode：

1. **更准地理解 opencode 源码**：Effect-TS 工厂函数、Layer/Service 模式能被识别
2. **Agent 可参考 opencode 实现**：调用图更清晰，能追踪模块依赖
3. **第三方依赖索引**：用户项目中的 `node_modules/` 可以高质量索引
4. **验证插件架构**：第一个真实场景就是分析 opencode，为后续语言插件打样

## 10. 一句话总结

> **把 TypeScript/Node.js 索引增强做成 coding.md 的第一个语言插件，用统一插件接口连接 tree-sitter 和 code_indexer，未来任何语言都可以按同样模式扩展。**
