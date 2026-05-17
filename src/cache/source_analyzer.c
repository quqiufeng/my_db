#include "cache_internal.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <regex.h>
#include <ctype.h>

// ====== 语言检测 ======

typedef enum {
    LANG_UNKNOWN = 0,
    LANG_C,
    LANG_CPP,
    LANG_PYTHON,
    LANG_JAVASCRIPT,
    LANG_JAVA,
    LANG_GO,
    LANG_RUST,
} language_t;

static language_t detect_language(const char* filename) {
    if (!filename) return LANG_UNKNOWN;
    
    const char* ext = strrchr(filename, '.');
    if (!ext) return LANG_UNKNOWN;
    
    if (strcmp(ext, ".c") == 0) return LANG_C;
    if (strcmp(ext, ".h") == 0) return LANG_C;
    if (strcmp(ext, ".cpp") == 0) return LANG_CPP;
    if (strcmp(ext, ".cc") == 0) return LANG_CPP;
    if (strcmp(ext, ".hpp") == 0) return LANG_CPP;
    if (strcmp(ext, ".cxx") == 0) return LANG_CPP;
    if (strcmp(ext, ".py") == 0) return LANG_PYTHON;
    if (strcmp(ext, ".js") == 0) return LANG_JAVASCRIPT;
    if (strcmp(ext, ".ts") == 0) return LANG_JAVASCRIPT;
    if (strcmp(ext, ".java") == 0) return LANG_JAVA;
    if (strcmp(ext, ".go") == 0) return LANG_GO;
    if (strcmp(ext, ".rs") == 0) return LANG_RUST;
    
    return LANG_UNKNOWN;
}

// ====== 简单的 regex 匹配辅助函数 ======

typedef struct {
    char** matches;
    size_t count;
    size_t capacity;
} match_list_t;

static void match_list_init(match_list_t* list) {
    list->matches = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void match_list_free(match_list_t* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->matches[i]);
    }
    free(list->matches);
    list->matches = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int match_list_add(match_list_t* list, const char* str, size_t len) {
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? 8 : list->capacity * 2;
        char** new_matches = realloc(list->matches, sizeof(char*) * new_cap);
        if (!new_matches) return -1;
        list->matches = new_matches;
        list->capacity = new_cap;
    }
    
    list->matches[list->count] = malloc(len + 1);
    if (!list->matches[list->count]) return -1;
    memcpy(list->matches[list->count], str, len);
    list->matches[list->count][len] = '\0';
    list->count++;
    return 0;
}

// 使用 regex 提取所有匹配
static int regex_extract_all(const char* text, const char* pattern, 
                             int group_idx, match_list_t* out_list) {
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) return -1;
    
    regmatch_t matches[10];
    const char* ptr = text;
    
    while (regexec(&regex, ptr, 10, matches, 0) == 0) {
        if (matches[group_idx].rm_so != -1) {
            size_t len = matches[group_idx].rm_eo - matches[group_idx].rm_so;
            if (match_list_add(out_list, ptr + matches[group_idx].rm_so, len) < 0) {
                regfree(&regex);
                return -1;
            }
        }
        ptr += matches[0].rm_eo;
        if (matches[0].rm_eo == 0) ptr++;  // 避免无限循环
    }
    
    regfree(&regex);
    return 0;
}

// ====== AST 节点类型 ======

typedef enum {
    AST_FUNCTION = 0,
    AST_CLASS = 1,
    AST_STRUCT = 2,
    AST_VARIABLE = 3,
    AST_IMPORT = 4,
    AST_COMMENT = 5,
} internal_ast_node_type_t;

typedef struct {
    internal_ast_node_type_t type;
    char* name;
    char* signature;  // 函数签名或类型信息
    size_t line_start;
    size_t line_end;
} ast_node_t;

// 内部 AST 树结构（对应 public 的 cache_ast_tree_t）
struct cache_ast_tree {
    ast_node_t* nodes;
    size_t count;
    size_t capacity;
};

static void ast_tree_init(cache_ast_tree_t* tree) {
    tree->nodes = NULL;
    tree->count = 0;
    tree->capacity = 0;
}

static void ast_tree_free(cache_ast_tree_t* tree) {
    if (!tree) return;
    for (size_t i = 0; i < tree->count; i++) {
        free(tree->nodes[i].name);
        free(tree->nodes[i].signature);
    }
    free(tree->nodes);
    tree->nodes = NULL;
    tree->count = 0;
    tree->capacity = 0;
}

static int ast_tree_add(cache_ast_tree_t* tree, internal_ast_node_type_t type, const char* name, 
                        const char* signature, size_t line_start, size_t line_end) {
    if (tree->count >= tree->capacity) {
        size_t new_cap = tree->capacity == 0 ? 16 : tree->capacity * 2;
        ast_node_t* new_nodes = realloc(tree->nodes, sizeof(ast_node_t) * new_cap);
        if (!new_nodes) return -1;
        tree->nodes = new_nodes;
        tree->capacity = new_cap;
    }
    
    ast_node_t* node = &tree->nodes[tree->count];
    node->type = type;
    node->name = name ? strdup(name) : NULL;
    node->signature = signature ? strdup(signature) : NULL;
    node->line_start = line_start;
    node->line_end = line_end;
    tree->count++;
    return 0;
}

// ====== 各语言提取器 ======

// C/C++ 提取器
static void extract_c_cpp(const char* source, cache_ast_tree_t* tree) {
    // 提取函数定义: return_type func_name(args)
    match_list_t funcs;
    match_list_init(&funcs);
    regex_extract_all(source, 
        "([a-zA-Z_][a-zA-Z0-9_]*\\s+)+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\([^)]*\\)\\s*\\{", 
        2, &funcs);
    for (size_t i = 0; i < funcs.count; i++) {
        ast_tree_add(tree, AST_FUNCTION, funcs.matches[i], NULL, 0, 0);
    }
    match_list_free(&funcs);
    
    // 提取结构体: struct name {
    match_list_t structs;
    match_list_init(&structs);
    regex_extract_all(source, "struct\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\{", 1, &structs);
    for (size_t i = 0; i < structs.count; i++) {
        ast_tree_add(tree, AST_STRUCT, structs.matches[i], NULL, 0, 0);
    }
    match_list_free(&structs);
    
    // 提取类: class name {
    match_list_t classes;
    match_list_init(&classes);
    regex_extract_all(source, "class\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*[:\\{]", 1, &classes);
    for (size_t i = 0; i < classes.count; i++) {
        ast_tree_add(tree, AST_CLASS, classes.matches[i], NULL, 0, 0);
    }
    match_list_free(&classes);
    
    // 提取 include
    match_list_t includes;
    match_list_init(&includes);
    regex_extract_all(source, "#include\\s*[\"<]([^\">]+)[\">]", 1, &includes);
    for (size_t i = 0; i < includes.count; i++) {
        ast_tree_add(tree, AST_IMPORT, includes.matches[i], NULL, 0, 0);
    }
    match_list_free(&includes);
}

// Python 提取器
static void extract_python(const char* source, cache_ast_tree_t* tree) {
    // 提取函数定义: def name(args):
    match_list_t funcs;
    match_list_init(&funcs);
    regex_extract_all(source, "def\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\(", 1, &funcs);
    for (size_t i = 0; i < funcs.count; i++) {
        ast_tree_add(tree, AST_FUNCTION, funcs.matches[i], NULL, 0, 0);
    }
    match_list_free(&funcs);
    
    // 提取类定义: class name:
    match_list_t classes;
    match_list_init(&classes);
    regex_extract_all(source, "class\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*[:\\(]", 1, &classes);
    for (size_t i = 0; i < classes.count; i++) {
        ast_tree_add(tree, AST_CLASS, classes.matches[i], NULL, 0, 0);
    }
    match_list_free(&classes);
    
    // 提取 import
    match_list_t imports;
    match_list_init(&imports);
    regex_extract_all(source, "(?:from|import)\\s+([a-zA-Z_][a-zA-Z0-9_.]*)", 1, &imports);
    for (size_t i = 0; i < imports.count; i++) {
        ast_tree_add(tree, AST_IMPORT, imports.matches[i], NULL, 0, 0);
    }
    match_list_free(&imports);
}

// JavaScript 提取器
static void extract_javascript(const char* source, cache_ast_tree_t* tree) {
    // 提取函数: function name(args) 或 const name = (args) =>
    match_list_t funcs;
    match_list_init(&funcs);
    regex_extract_all(source, "function\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\(", 1, &funcs);
    regex_extract_all(source, "const\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*=\\s*\\([^)]*\\)\\s*=>", 1, &funcs);
    for (size_t i = 0; i < funcs.count; i++) {
        ast_tree_add(tree, AST_FUNCTION, funcs.matches[i], NULL, 0, 0);
    }
    match_list_free(&funcs);
    
    // 提取类: class name {
    match_list_t classes;
    match_list_init(&classes);
    regex_extract_all(source, "class\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\{", 1, &classes);
    for (size_t i = 0; i < classes.count; i++) {
        ast_tree_add(tree, AST_CLASS, classes.matches[i], NULL, 0, 0);
    }
    match_list_free(&classes);
    
    // 提取 import
    match_list_t imports;
    match_list_init(&imports);
    regex_extract_all(source, "import\\s+.*?\\s+from\\s+['\"]([^'\"]+)['\"]", 1, &imports);
    for (size_t i = 0; i < imports.count; i++) {
        ast_tree_add(tree, AST_IMPORT, imports.matches[i], NULL, 0, 0);
    }
    match_list_free(&imports);
}

// Java 提取器
static void extract_java(const char* source, cache_ast_tree_t* tree) {
    // 提取方法
    match_list_t methods;
    match_list_init(&methods);
    regex_extract_all(source, 
        "(?:public|private|protected|static|final|abstract|synchronized)\\s+"
        "(?:<[^>]+>\\s+)?"
        "[a-zA-Z_][a-zA-Z0-9_<>,\\s]*\\s+"
        "([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\([^)]*\\)\\s*(?:throws\\s+[^{]+)?\\s*\\{",
        1, &methods);
    for (size_t i = 0; i < methods.count; i++) {
        ast_tree_add(tree, AST_FUNCTION, methods.matches[i], NULL, 0, 0);
    }
    match_list_free(&methods);
    
    // 提取类
    match_list_t classes;
    match_list_init(&classes);
    regex_extract_all(source, "class\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*[\\{\\u003c]", 1, &classes);
    for (size_t i = 0; i < classes.count; i++) {
        ast_tree_add(tree, AST_CLASS, classes.matches[i], NULL, 0, 0);
    }
    match_list_free(&classes);
    
    // 提取 import
    match_list_t imports;
    match_list_init(&imports);
    regex_extract_all(source, "import\\s+([a-zA-Z_][a-zA-Z0-9_.]*(?:\\.\\*)?);", 1, &imports);
    for (size_t i = 0; i < imports.count; i++) {
        ast_tree_add(tree, AST_IMPORT, imports.matches[i], NULL, 0, 0);
    }
    match_list_free(&imports);
}

// Go 提取器
static void extract_go(const char* source, cache_ast_tree_t* tree) {
    // 提取函数: func name(args)
    match_list_t funcs;
    match_list_init(&funcs);
    regex_extract_all(source, "func\\s+(?:[a-zA-Z_][a-zA-Z0-9_]*\\s*\\([^)]*\\)\\s*\\.)?"
                              "([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\(", 1, &funcs);
    for (size_t i = 0; i < funcs.count; i++) {
        ast_tree_add(tree, AST_FUNCTION, funcs.matches[i], NULL, 0, 0);
    }
    match_list_free(&funcs);
    
    // 提取结构体
    match_list_t structs;
    match_list_init(&structs);
    regex_extract_all(source, "type\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s+struct\\s*\\{", 1, &structs);
    for (size_t i = 0; i < structs.count; i++) {
        ast_tree_add(tree, AST_STRUCT, structs.matches[i], NULL, 0, 0);
    }
    match_list_free(&structs);
    
    // 提取 import
    match_list_t imports;
    match_list_init(&imports);
    regex_extract_all(source, "import\\s+[\"']([^\"']+)[\"']", 1, &imports);
    for (size_t i = 0; i < imports.count; i++) {
        ast_tree_add(tree, AST_IMPORT, imports.matches[i], NULL, 0, 0);
    }
    match_list_free(&imports);
}

// Rust 提取器
static void extract_rust(const char* source, cache_ast_tree_t* tree) {
    // 提取函数: fn name(args)
    match_list_t funcs;
    match_list_init(&funcs);
    regex_extract_all(source, "fn\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\(", 1, &funcs);
    for (size_t i = 0; i < funcs.count; i++) {
        ast_tree_add(tree, AST_FUNCTION, funcs.matches[i], NULL, 0, 0);
    }
    match_list_free(&funcs);
    
    // 提取结构体
    match_list_t structs;
    match_list_init(&structs);
    regex_extract_all(source, "struct\\s+([a-zA-Z_][a-zA-Z0-9_]*)\\s*\\{", 1, &structs);
    for (size_t i = 0; i < structs.count; i++) {
        ast_tree_add(tree, AST_STRUCT, structs.matches[i], NULL, 0, 0);
    }
    match_list_free(&structs);
    
    // 提取 use
    match_list_t uses;
    match_list_init(&uses);
    regex_extract_all(source, "use\\s+([a-zA-Z_][a-zA-Z0-9_:]*);", 1, &uses);
    for (size_t i = 0; i < uses.count; i++) {
        ast_tree_add(tree, AST_IMPORT, uses.matches[i], NULL, 0, 0);
    }
    match_list_free(&uses);
}

// ====== 主提取函数 ======

cache_ast_tree_t* cache_analyze_source(const char* filename, const char* source) {
    if (!filename || !source) return NULL;
    
    cache_ast_tree_t* tree = malloc(sizeof(cache_ast_tree_t));
    if (!tree) return NULL;
    
    ast_tree_init(tree);
    
    language_t lang = detect_language(filename);
    
    switch (lang) {
        case LANG_C:
        case LANG_CPP:
            extract_c_cpp(source, tree);
            break;
        case LANG_PYTHON:
            extract_python(source, tree);
            break;
        case LANG_JAVASCRIPT:
            extract_javascript(source, tree);
            break;
        case LANG_JAVA:
            extract_java(source, tree);
            break;
        case LANG_GO:
            extract_go(source, tree);
            break;
        case LANG_RUST:
            extract_rust(source, tree);
            break;
        default:
            // 未知语言，尝试通用提取
            extract_c_cpp(source, tree);
            break;
    }
    
    return tree;
}

// ====== AST 序列化为 JSON ======

char* cache_ast_to_json(const cache_ast_tree_t* tree, const char* filename) {
    if (!tree) return NULL;
    
    // 计算所需缓冲区大小
    size_t buf_size = 4096;
    char* buf = malloc(buf_size);
    if (!buf) return NULL;
    
    int pos = snprintf(buf, buf_size, 
        "{\"file\":\"%s\",\"language\":\"%s\",\"nodes\":[",
        filename ? filename : "unknown",
        tree->count > 0 ? "detected" : "unknown");
    
    for (size_t i = 0; i < tree->count && pos < (int)buf_size - 256; i++) {
        const ast_node_t* node = &tree->nodes[i];
        const char* type_str = "unknown";
        switch (node->type) {
            case AST_FUNCTION: type_str = "function"; break;
            case AST_CLASS: type_str = "class"; break;
            case AST_STRUCT: type_str = "struct"; break;
            case AST_VARIABLE: type_str = "variable"; break;
            case AST_IMPORT: type_str = "import"; break;
            case AST_COMMENT: type_str = "comment"; break;
        }
        
        pos += snprintf(buf + pos, buf_size - pos,
            "%s{\"type\":\"%s\",\"name\":\"%s\"}",
            i > 0 ? "," : "",
            type_str,
            node->name ? node->name : "");
    }
    
    pos += snprintf(buf + pos, buf_size - pos, "]}");
    
    return buf;
}

// ====== AST 语义搜索 ======

int cache_search_ast(cache_t* cache, const char* query, cache_ast_node_type_t type_filter,
                     cache_search_options_t* options,
                     cache_result_t** out_results, size_t* out_count) {
    (void)type_filter;  // TODO: implement type filtering
    
    if (!cache || !query || !out_results || !out_count) return CACHE_ERR_INVAL;
    
    *out_results = NULL;
    *out_count = 0;
    
    size_t query_len = strlen(query);
    if (query_len == 0) return CACHE_ERR_INVAL;
    
    uint64_t now = cache_now_ms();
    int max_results = options ? options->max_results : 100;
    const char* ns_filter = options ? options->ns_filter : NULL;
    
    cache_result_t* results = NULL;
    size_t result_count = 0;
    size_t result_cap = 0;
    
    // 遍历所有 entry，查找包含 query 的 AST JSON
    for (size_t i = 0; i < cache->sorted.count; i++) {
        size_t offset = cache_sorted_get(cache, i);
        if (!offset) continue;
        
        if (!entry_is_valid(cache, offset, now)) continue;
        if (!ns_filter_match(cache, offset, ns_filter)) continue;
        
        size_t value_len;
        const char* value = entry_value(cache, offset, &value_len);
        
        // 检查是否是 AST JSON（简单检查是否包含 '"type":"'）
        if (strstr(value, "\"type\":\"") == NULL) continue;
        
        // 检查是否包含 query
        if (strstr(value, query) != NULL) {
            if (append_result(&results, &result_count, &result_cap, cache, offset, 1.0) < 0) {
                free(results);
                return CACHE_ERR_NOMEM;
            }
            if (max_results > 0 && result_count >= (size_t)max_results) break;
        }
    }
    
    *out_results = results;
    *out_count = result_count;
    return CACHE_OK;
}

// ====== 清理函数 ======

void cache_ast_free(cache_ast_tree_t* tree) {
    if (!tree) return;
    ast_tree_free(tree);
    free(tree);
}

// ====== 从 cache entry 提取 AST 标签 ======

int cache_ast_extract_tags(const char* filename, const char* source, 
                           char** out_tags, size_t* out_tag_count) {
    if (!filename || !source || !out_tags || !out_tag_count) return CACHE_ERR_INVAL;
    
    *out_tags = NULL;
    *out_tag_count = 0;
    
    cache_ast_tree_t* tree = cache_analyze_source(filename, source);
    if (!tree) return CACHE_ERR_NOMEM;
    
    if (tree->count == 0) {
        cache_ast_free(tree);
        return CACHE_OK;
    }
    
    // 分配标签缓冲区
    size_t buf_size = 4096;
    char* tags = malloc(buf_size);
    if (!tags) {
        cache_ast_free(tree);
        return CACHE_ERR_NOMEM;
    }
    
    size_t pos = 0;
    tags[0] = '\0';
    
    for (size_t i = 0; i < tree->count && pos < buf_size - 256; i++) {
        const ast_node_t* node = &tree->nodes[i];
        if (!node->name) continue;
        
        const char* type_str = "";
        switch (node->type) {
            case AST_FUNCTION: type_str = "func:"; break;
            case AST_CLASS: type_str = "class:"; break;
            case AST_STRUCT: type_str = "struct:"; break;
            case AST_IMPORT: type_str = "import:"; break;
            default: continue;
        }
        
        pos += snprintf(tags + pos, buf_size - pos, "%s%s ", type_str, node->name);
    }
    
    cache_ast_free(tree);
    
    *out_tags = tags;
    *out_tag_count = pos > 0 ? pos - 1 : 0;  // 去掉末尾空格
    return CACHE_OK;
}
