#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>

#define MAX_LINE 131072
#define MAX_VARS 1000
#define MAX_OCCURS 100

// 变量出现记录
typedef struct {
    char file[512];
    char func[256];
    int line;
    int col;
    char context[256];
    int is_definition;  // 1=定义, 2=赋值, 3=使用
} occurrence_t;

// 变量记录
typedef struct {
    char name[128];
    char type[128];     // 变量类型（如果知道）
    occurrence_t occurs[MAX_OCCURS];
    int occur_count;
} var_record_t;

static var_record_t g_vars[MAX_VARS];
static int g_var_count = 0;

// 查找或创建变量记录
static var_record_t* find_or_create_var(const char* name) {
    for (int i = 0; i < g_var_count; i++) {
        if (strcmp(g_vars[i].name, name) == 0) return &g_vars[i];
    }
    if (g_var_count >= MAX_VARS) return NULL;
    strncpy(g_vars[g_var_count].name, name, sizeof(g_vars[g_var_count].name) - 1);
    g_vars[g_var_count].name[sizeof(g_vars[g_var_count].name) - 1] = '\0';
    g_vars[g_var_count].type[0] = '\0';
    g_vars[g_var_count].occur_count = 0;
    return &g_vars[g_var_count++];
}

// 添加出现记录
static void add_occurrence(var_record_t* var, const char* file, const char* func, 
                           int line, int col, const char* context, int is_def) {
    if (!var || var->occur_count >= MAX_OCCURS) return;
    occurrence_t* occ = &var->occurs[var->occur_count++];
    strncpy(occ->file, file, sizeof(occ->file) - 1);
    strncpy(occ->func, func, sizeof(occ->func) - 1);
    occ->line = line;
    occ->col = col;
    strncpy(occ->context, context, sizeof(occ->context) - 1);
    occ->is_definition = is_def;
}

// 检测 C 类型关键字
static int is_c_type(const char* word) {
    static const char* types[] = {
        "int", "char", "float", "double", "void", "auto", "static", "const",
        "struct", "union", "enum", "unsigned", "signed", "long", "short",
        "size_t", "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int8_t", "int16_t", "int32_t", "int64_t", "bool", "ggml_tensor",
        "ggml_context", "ngx_str_t", "ngx_int_t", "ngx_uint_t", "ngx_flag_t",
        "ngx_http_request_t", "ngx_connection_t", "ngx_cycle_t", "ngx_pool_t",
        "sd_image_t", "sd_ctx_t", "ggml_backend_t", "ggml_cgraph",
        NULL
    };
    for (int i = 0; types[i]; i++) {
        if (strcmp(word, types[i]) == 0) return 1;
    }
    return 0;
}

// 简单词法分析：提取 token
static void tokenize_line(const char* line, char tokens[][128], int* token_count, int max_tokens) {
    *token_count = 0;
    const char* p = line;
    while (*p && *token_count < max_tokens) {
        // 跳过空白
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        
        // 跳过注释
        if (*p == '/' && *(p+1) == '/') break;
        
        // 标识符或关键字
        if (isalpha((unsigned char)*p) || *p == '_') {
            int i = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < 127) {
                tokens[*token_count][i++] = *p++;
            }
            tokens[*token_count][i] = '\0';
            (*token_count)++;
        }
        // 数字
        else if (isdigit((unsigned char)*p)) {
            int i = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '.') && i < 127) {
                tokens[*token_count][i++] = *p++;
            }
            tokens[*token_count][i] = '\0';
            (*token_count)++;
        }
        // 字符串
        else if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) p++;
            if (*p) p++;
        }
        // 操作符
        else {
            int i = 0;
            tokens[*token_count][i++] = *p++;
            // 多字符操作符
            if (*p && ((*p == '=' && strchr("=+-*/%<>!|&", *(p-1))) ||
                        (*p == '+' && *(p-1) == '+') ||
                        (*p == '-' && *(p-1) == '-') ||
                        (*p == '>' && *(p-1) == '-') ||
                        (*p == '>' && *(p-1) == '>') ||
                        (*p == '<' && *(p-1) == '<'))) {
                tokens[*token_count][i++] = *p++;
            }
            tokens[*token_count][i] = '\0';
            (*token_count)++;
        }
    }
}

// 分析单行代码中的变量
typedef struct {
    char file[512];
    char func[256];
    int abs_line;  // 文件中的绝对行号
} ctx_info_t;

static void analyze_line(const char* line, int line_no, ctx_info_t* ctx) {
    char tokens[64][128];
    int token_count = 0;
    tokenize_line(line, tokens, &token_count, 64);
    
    // 扫描：查找定义、赋值、使用
    for (int i = 0; i < token_count; i++) {
        // 1. 变量定义：类型 变量名 [ = ... ] ;
        // 模式：type var_name [= ...];
        if (is_c_type(tokens[i]) && i + 1 < token_count) {
            // 检查后面跟着的是否是标识符（不是关键字）
            char* next = tokens[i + 1];
            // 跳过指针星号
            if (strcmp(next, "*") == 0 && i + 2 < token_count) {
                next = tokens[i + 2];
            }
            if (isalpha((unsigned char)next[0]) || next[0] == '_') {
                var_record_t* var = find_or_create_var(next);
                if (var && var->occur_count == 0) {
                    strncpy(var->type, tokens[i], sizeof(var->type) - 1);
                    add_occurrence(var, ctx->file, ctx->func, ctx->abs_line, 0, line, 1);
                }
            }
        }
        
        // 2. 赋值：var_name = ...
        if (strcmp(tokens[i], "=") == 0 && i > 0) {
            char* prev = tokens[i - 1];
            // 跳过 ++ -- 等
            if (strcmp(prev, "+") != 0 && strcmp(prev, "-") != 0 &&
                strcmp(prev, "*") != 0 && strcmp(prev, "&") != 0) {
                var_record_t* var = find_or_create_var(prev);
                if (var) add_occurrence(var, ctx->file, ctx->func, ctx->abs_line, 0, line, 2);
            }
        }
        
        // 3. 使用：token 出现在右侧（简单启发式）
        // 如果 token 是标识符，且不是类型，且不在赋值左侧
        if ((isalpha((unsigned char)tokens[i][0]) || tokens[i][0] == '_') &&
            !is_c_type(tokens[i])) {
            // 检查是否是赋值左侧
            int is_lvalue = 0;
            if (i + 1 < token_count && strcmp(tokens[i + 1], "=") == 0) {
                is_lvalue = 1;
            }
            // 检查是否是 ++ -- 左侧
            if (i + 1 < token_count && 
                (strcmp(tokens[i + 1], "++") == 0 || strcmp(tokens[i + 1], "--") == 0)) {
                is_lvalue = 1;
            }
            if (!is_lvalue) {
                var_record_t* var = find_or_create_var(tokens[i]);
                if (var && var->occur_count > 0) {
                    // 确保不是重复记录同一行
                    int dup = 0;
                    for (int j = 0; j < var->occur_count; j++) {
                        if (var->occurs[j].line == ctx->abs_line) { dup = 1; break; }
                    }
                    if (!dup) {
                        add_occurrence(var, ctx->file, ctx->func, ctx->abs_line, 0, line, 3);
                    }
                }
            }
        }
    }
}

// 处理函数体内容
static void process_function_content(const char* content, ctx_info_t* ctx_base) {
    char line[1024];
    const char* p = content;
    int rel_line = 0;
    
    while (*p) {
        int i = 0;
        while (*p && *p != '\n' && i < sizeof(line) - 1) {
            line[i++] = *p++;
        }
        line[i] = '\0';
        if (*p == '\n') p++;
        rel_line++;
        
        ctx_info_t ctx = *ctx_base;
        ctx.abs_line = ctx_base->abs_line + rel_line - 1;
        analyze_line(line, rel_line, &ctx);
    }
}

// 从 meta 文件提取信息并分析
static int analyze_meta(const char* meta_file) {
    FILE* fp = fopen(meta_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", meta_file);
        return 1;
    }
    
    char* line = NULL;
    size_t line_len = 0;
    int func_count = 0;
    
    while (getline(&line, &line_len, fp) != -1) {
        // 提取 name, file, kind, line_start, content
        char name[256] = {0}, file[512] = {0}, kind[32] = {0};
        char content[32768] = {0};
        int line_start = 0;
        
        char* name_p = strstr(line, "\"name\":\"");
        char* file_p = strstr(line, "\"file\":\"");
        char* kind_p = strstr(line, "\"kind\":\"");
        char* line_p = strstr(line, "\"line_start\":");
        char* content_p = strstr(line, "\"content\":\"");
        
        if (!name_p || !file_p || !kind_p || !content_p) continue;
        
        name_p += 8;
        file_p += 8;
        kind_p += 8;
        line_p += 13;
        content_p += 11;
        
        int i = 0;
        while (*name_p && *name_p != '"' && i < 255) name[i++] = *name_p++;
        
        i = 0;
        while (*file_p && *file_p != '"' && i < 511) file[i++] = *file_p++;
        
        i = 0;
        while (*kind_p && *kind_p != '"' && i < 31) kind[i++] = *kind_p++;
        line_start = atoi(line_p);
        
        // 只分析函数和方法
        if (strcmp(kind, "function") != 0 && strcmp(kind, "method") != 0) continue;
        
        // 提取 content
        i = 0;
        const char* cp = content_p;
        while (*cp && i < sizeof(content) - 1) {
            if (*cp == '\\' && *(cp+1)) {
                content[i++] = *(++cp);
                cp++;
            } else if (*cp == '"') {
                break;
            } else {
                content[i++] = *cp++;
            }
        }
        content[i] = '\0';
        
        ctx_info_t ctx;
        strncpy(ctx.file, file, sizeof(ctx.file) - 1);
        strncpy(ctx.func, name, sizeof(ctx.func) - 1);
        ctx.abs_line = line_start;
        
        process_function_content(content, &ctx);
        func_count++;
    }
    
    fclose(fp);
    free(line);
    printf("Analyzed %d functions, found %d unique variables\n", func_count, g_var_count);
    return 0;
}

// 输出变量数据流报告
static void print_dataflow(const char* var_name) {
    var_record_t* var = NULL;
    for (int i = 0; i < g_var_count; i++) {
        if (strcmp(g_vars[i].name, var_name) == 0) {
            var = &g_vars[i];
            break;
        }
    }
    
    if (!var) {
        printf("Variable '%s' not found in analyzed code.\n", var_name);
        return;
    }
    
    printf("\n╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║ Variable Data Flow: %-45s ║\n", var_name);
    if (var->type[0]) {
        printf("║ Type: %-58s ║\n", var->type);
    }
    printf("║ Total occurrences: %-46d ║\n", var->occur_count);
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");
    
    // 分类显示
    printf("📌 DEFINITIONS:\n");
    printf("─────────────────────────────────────────────────────────────────\n");
    int found = 0;
    for (int i = 0; i < var->occur_count; i++) {
        if (var->occurs[i].is_definition == 1) {
            printf("  %s:%d  in %s()\n", 
                   var->occurs[i].file, var->occurs[i].line, var->occurs[i].func);
            printf("  %s\n\n", var->occurs[i].context);
            found++;
        }
    }
    if (!found) printf("  (no explicit definition found)\n\n");
    
    printf("✏️  ASSIGNMENTS (Value Changes):\n");
    printf("─────────────────────────────────────────────────────────────────\n");
    found = 0;
    for (int i = 0; i < var->occur_count; i++) {
        if (var->occurs[i].is_definition == 2) {
            printf("  %s:%d  in %s()\n", 
                   var->occurs[i].file, var->occurs[i].line, var->occurs[i].func);
            printf("  %s\n\n", var->occurs[i].context);
            found++;
        }
    }
    if (!found) printf("  (no assignments found)\n\n");
    
    printf("👁️  USAGES (Value Reads):\n");
    printf("─────────────────────────────────────────────────────────────────\n");
    found = 0;
    for (int i = 0; i < var->occur_count; i++) {
        if (var->occurs[i].is_definition == 3) {
            printf("  %s:%d  in %s()\n", 
                   var->occurs[i].file, var->occurs[i].line, var->occurs[i].func);
            printf("  %s\n\n", var->occurs[i].context);
            found++;
        }
    }
    if (!found) printf("  (no usages found)\n\n");
}

// 保存数据流到 JSON 文件
static void save_dataflow(const char* output_file) {
    FILE* fp = fopen(output_file, "w");
    if (!fp) return;
    
    fprintf(fp, "{\n");
    for (int v = 0; v < g_var_count; v++) {
        if (v > 0) fprintf(fp, ",\n");
        var_record_t* var = &g_vars[v];
        fprintf(fp, "  \"%s\": {\n", var->name);
        if (var->type[0]) {
            fprintf(fp, "    \"type\": \"%s\",\n", var->type);
        }
        fprintf(fp, "    \"occurrences\": [\n");
        for (int i = 0; i < var->occur_count; i++) {
            if (i > 0) fprintf(fp, ",\n");
            occurrence_t* occ = &var->occurs[i];
            const char* type_str = (occ->is_definition == 1) ? "definition" :
                                   (occ->is_definition == 2) ? "assignment" : "usage";
            fprintf(fp, "      {\"file\":\"%s\",\"func\":\"%s\",\"line\":%d,\"type\":\"%s\",\"context\":\"%s\"}",
                   occ->file, occ->func, occ->line, type_str, occ->context);
        }
        fprintf(fp, "\n    ]\n  }");
    }
    fprintf(fp, "\n}\n");
    fclose(fp);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Variable Data Flow Analyzer\n");
        printf("Usage:\n");
        printf("  %s analyze <cache_dir>              # Analyze all variables\n", argv[0]);
        printf("  %s show <cache_dir> <var_name>     # Show data flow for variable\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s analyze ./nginx_cache\n", argv[0]);
        printf("  %s show ./nginx_cache ngx_connection\n", argv[0]);
        return 1;
    }
    
    const char* cmd = argv[1];
    const char* cache_dir = argv[2];
    
    if (strcmp(cmd, "analyze") == 0) {
        if (!cache_dir) {
            fprintf(stderr, "Usage: %s analyze <cache_dir>\n", argv[0]);
            return 1;
        }
        
        char meta_file[512], out_file[512];
        snprintf(meta_file, sizeof(meta_file), "%s/chunks_meta.jsonl", cache_dir);
        snprintf(out_file, sizeof(out_file), "%s/dataflow.json", cache_dir);
        
        printf("Analyzing data flow from %s...\n", meta_file);
        if (analyze_meta(meta_file) != 0) return 1;
        
        save_dataflow(out_file);
        printf("Saved data flow to %s\n", out_file);
        printf("Found %d variables\n", g_var_count);
        
    } else if (strcmp(cmd, "show") == 0) {
        if (argc < 4) {
            fprintf(stderr, "Usage: %s show <cache_dir> <var_name>\n", argv[0]);
            return 1;
        }
        
        const char* var_name = argv[3];
        char meta_file[512];
        snprintf(meta_file, sizeof(meta_file), "%s/chunks_meta.jsonl", cache_dir);
        
        printf("Loading data flow from %s...\n", meta_file);
        if (analyze_meta(meta_file) != 0) return 1;
        
        print_dataflow(var_name);
        
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
    
    return 0;
}