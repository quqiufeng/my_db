#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_NAME_LEN 256
#define MAX_FUNC 300000
#define HASH_SIZE 524287
static char** g_func_names = NULL;
static int g_func_count = 0;


static int g_name_hash[HASH_SIZE];  // -1 = empty
#define MAX_ARGS 20

typedef struct func_body {
    char name[MAX_NAME_LEN];
    char file[512];
    int line_start;
    char* content;
    struct func_body* next;
} func_body_t;

static func_body_t* g_bodies = NULL;
static int g_body_count = 0;



// DJB2 hash
static unsigned int hash_str(const char* s) {
    unsigned int h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h % HASH_SIZE;
}

static void add_func_name(const char* name) {
    unsigned int h = hash_str(name);
    while (g_name_hash[h] >= 0) {
        if (strcmp(g_func_names[g_name_hash[h]], name) == 0) return;
        h = (h + 1) % HASH_SIZE;
    }
    g_func_names[g_func_count] = strdup(name);
    g_name_hash[h] = g_func_count;
    g_func_count++;
}

static int is_func_known(const char* name) {
    unsigned int h = hash_str(name);
    while (g_name_hash[h] >= 0) {
        if (strcmp(g_func_names[g_name_hash[h]], name) == 0) return 1;
        h = (h + 1) % HASH_SIZE;
    }
    return 0;
}

static void add_body(const char* name, const char* file, int line, const char* content) {
    func_body_t* b = malloc(sizeof(func_body_t));
    if (!b) return;
    strncpy(b->name, name, MAX_NAME_LEN - 1);
    b->name[MAX_NAME_LEN - 1] = '\0';
    strncpy(b->file, file, sizeof(b->file) - 1);
    b->file[sizeof(b->file) - 1] = '\0';
    b->line_start = line;
    b->content = strdup(content);
    b->next = g_bodies;
    g_bodies = b;
    g_body_count++;
}

// 在内容中查找函数调用，并提取参数
static int find_call_with_args(const char* content, const char* func_name,
                                char* args_out, int args_out_size) {
    int len = (int)strlen(func_name);
    const char* p = content;
    args_out[0] = '\0';
    
    while ((p = strstr(p, func_name)) != NULL) {
        if (p > content) {
            char c = *(p - 1);
            if (isalnum(c) || c == '_' || c == ':') {
                p += len;
                continue;
            }
        }
        
        const char* after = p + len;
        while (*after && isspace((unsigned char)*after)) after++;
        
        if (*after == '(') {
            // 提取参数
            after++;
            int depth = 1;
            int arg_idx = 0;
            char arg[MAX_ARGS][128];
            int arg_len = 0;
            int in_string = 0;
            char string_char = 0;
            
            while (*after && depth > 0 && arg_idx < MAX_ARGS) {
                if (!in_string && (*after == '"' || *after == '\'')) {
                    in_string = 1;
                    string_char = *after;
                    after++;
                    continue;
                }
                if (in_string && *after == string_char) {
                    in_string = 0;
                    after++;
                    continue;
                }
                if (in_string) {
                    after++;
                    continue;
                }
                
                if (*after == '(') {
                    depth++;
                } else if (*after == ')') {
                    depth--;
                    if (depth == 0) break;
                } else if (*after == ',' && depth == 1) {
                    if (arg_len > 0) {
                        arg[arg_idx][arg_len] = '\0';
                        arg_idx++;
                        arg_len = 0;
                    }
                    after++;
                    continue;
                }
                
                if (arg_len < 127 && depth == 1) {
                    arg[arg_idx][arg_len++] = *after;
                }
                after++;
            }
            
            if (arg_len > 0 && arg_idx < MAX_ARGS) {
                arg[arg_idx][arg_len] = '\0';
                arg_idx++;
            }
            
            // 格式化输出参数
            int pos = 0;
            for (int i = 0; i < arg_idx && pos < args_out_size - 1; i++) {
                // 去掉前后空格
                char* start = arg[i];
                while (*start && isspace((unsigned char)*start)) start++;
                char* end = start + strlen(start) - 1;
                while (end > start && isspace((unsigned char)*end)) *end-- = '\0';
                
                if (strlen(start) > 0) {
                    int alen = (int)strlen(start);
                    if (pos + alen + 3 < args_out_size) {
                        if (pos > 0) {
                            args_out[pos++] = ',';
                            args_out[pos++] = ' ';
                        }
                        strcpy(args_out + pos, start);
                        pos += alen;
                    }
                }
            }
            args_out[pos] = '\0';
            return 1;
        }
        
        p += len;
    }
    
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Call Graph Builder (Enhanced)\n");
        printf("Usage: %s <cache_dir>\n", argv[0]);
        return 1;
    }
    
    const char* cache_dir = argv[1];
    char meta_file[512], out_file[512];
    snprintf(meta_file, sizeof(meta_file), "%s/chunks_meta.jsonl", cache_dir);
    snprintf(out_file, sizeof(out_file), "%s/call_graph.json", cache_dir);
    
    g_func_names = malloc(MAX_FUNC * sizeof(char*));
    if (!g_func_names) return 1;
    memset(g_name_hash, -1, sizeof(g_name_hash));
    
    printf("Phase 1: Loading function names...\n");
    fflush(stdout);
    
    FILE* fp = fopen(meta_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", meta_file);
        return 1;
    }
    
    int line_count = 0;
    char* line = NULL;
    size_t line_len = 0;
    
    while (getline(&line, &line_len, fp) != -1) {
        line_count++;
        char* name_p = strstr(line, "\"name\":\"");
        char* kind_p = strstr(line, "\"kind\":\"");
        if (!name_p || !kind_p) continue;
        
        name_p += 8;
        kind_p += 8;
        
        char kind[32] = {0};
        int i = 0;
        while (*kind_p && *kind_p != '"' && i < 31) kind[i++] = *kind_p++;
        
        if (strcmp(kind, "function") != 0 && strcmp(kind, "method") != 0) continue;
        
        char name[256] = {0};
        i = 0;
        while (*name_p && *name_p != '"' && i < 255) name[i++] = *name_p++;
        
        add_func_name(name);
        if (g_func_count >= MAX_FUNC) break;
    }
    fclose(fp);
    printf("  Found %d unique functions\n", g_func_count);
    
    printf("Phase 2: Loading function bodies...\n");
    
    fp = fopen(meta_file, "r");
    if (!fp) return 1;
    
    static int processed[MAX_FUNC];
    memset(processed, 0, sizeof(processed));
    
    while (getline(&line, &line_len, fp) != -1) {
        line_count++;
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
        
        char kind[32] = {0};
        int i = 0;
        while (*kind_p && *kind_p != '"' && i < 31) kind[i++] = *kind_p++;
        
        if (strcmp(kind, "function") != 0 && strcmp(kind, "method") != 0) continue;
        
        char name[256] = {0};
        i = 0;
        while (*name_p && *name_p != '"' && i < 255) name[i++] = *name_p++;
        
        char file[512] = {0};
        i = 0;
        while (*file_p && *file_p != '"' && i < 511) file[i++] = *file_p++;
        
        int line_start = atoi(line_p);
        
        // 查找函数索引
        int func_idx = -1;
        for (int j = 0; j < g_func_count; j++) {
            if (strcmp(g_func_names[j], name) == 0) {
                func_idx = j;
                break;
            }
        }
        if (func_idx < 0) continue;
        if (processed[func_idx]) continue;
        processed[func_idx] = 1;
        
        // Extract content
        char* content = malloc(line_len);
        if (!content) continue;
        
        const char* p = content_p;
        int j = 0;
        while (*p) {
            if (*p == '\\' && *(p+1)) {
                content[j++] = *(++p);
                p++;
            } else if (*p == '"') {
                break;
            } else {
                content[j++] = *p++;
            }
        }
        content[j] = '\0';
        
        add_body(name, file, line_start, content);
        free(content);
    }
    fclose(fp);
    printf("  Loaded %d function bodies\n", g_body_count);
    
    printf("Phase 3: Building call graph with arguments...\n");
    
    FILE* out = fopen(out_file, "w");
    if (!out) {
        fprintf(stderr, "Failed to create %s\n", out_file);
        goto cleanup;
    }
    
    fprintf(out, "{\n");
    int first = 1;
    
    for (int f = 0; f < g_func_count; f++) {
        const char* callee = g_func_names[f];
        int callee_len = (int)strlen(callee);
        if (callee_len < 2) continue;
        
        int has_callers = 0;
        
        func_body_t* b = g_bodies;
        while (b) {
            if (strcmp(b->name, callee) != 0 && b->content) {
                char args[1024];
                if (find_call_with_args(b->content, callee, args, sizeof(args))) {
                    if (!has_callers) {
                        if (!first) fprintf(out, ",\n");
                        first = 0;
                        fprintf(out, "  \"%s\": {\n", callee);
                        fprintf(out, "    \"calls\": [\n");
                        has_callers = 1;
                    } else {
                        fprintf(out, ",\n");
                    }
                    
                    fprintf(out, "      {\"function\":\"%s\",\"file\":\"%s\",\"line\":%d",
                           b->name, b->file, b->line_start);
                    
                    if (args[0]) {
                        fprintf(out, ",\"arguments\":[\"");
                        // 转义参数中的特殊字符
                        const char* ap = args;
                        while (*ap) {
                            if (*ap == '"' || *ap == '\\') {
                                fputc('\\', out);
                            }
                            fputc(*ap, out);
                            ap++;
                        }
                        fprintf(out, "\"]");
                    }
                    fprintf(out, "}");
                }
            }
            b = b->next;
        }
        
        if (has_callers) {
            fprintf(out, "\n    ]\n  }");
        }
    }
    
    fprintf(out, "\n}\n");
    fclose(out);
    printf("Saved call graph to %s\n", out_file);
    
cleanup:
    for (int i = 0; i < g_func_count; i++) free(g_func_names[i]);
    free(g_func_names);
    
    func_body_t* b = g_bodies;
    while (b) {
        func_body_t* next = b->next;
        free(b->content);
        free(b);
        b = next;
    }
    free(line);
    
    return 0;
}
