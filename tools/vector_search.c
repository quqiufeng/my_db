/*
 * vector_search.c — CLI wrapper around libvector_engine.so
 * 
 * All heavy lifting (embedder, HNSW, metadata, TF-IDF, call graph) is done by the library.
 * This file only handles: argument parsing, output formatting, callgraph display.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include "vector_engine.h"

#define MAX_RESULTS 100
#define MAX_QUERY_LEN 1024

static int output_json = 0;
static int output_rich = 0;
static int show_callgraph = 0;

// Call graph support (loaded separately for display)
typedef struct caller_node {
    char name[256];
    struct caller_node* next;
} caller_node_t;

typedef struct callgraph_entry {
    char func_name[256];
    caller_node_t* callers;
    int caller_count;
    struct callgraph_entry* next;
} callgraph_entry_t;

static callgraph_entry_t** g_call_graph = NULL;
static int g_callgraph_loaded = 0;
#define CALLGRAPH_HASH_SIZE 65536

static uint64_t hash_callgraph(const char* name) {
    uint64_t h = 0xcbf29ce484222325;
    for (const char* p = name; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 0x100000001b3;
    }
    return h % CALLGRAPH_HASH_SIZE;
}

static void load_call_graph(const char* cache_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/call_graph.json", cache_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) return;
    
    g_call_graph = calloc(CALLGRAPH_HASH_SIZE, sizeof(callgraph_entry_t*));
    if (!g_call_graph) { fclose(fp); return; }
    
    char line[131072];
    while (fgets(line, sizeof(line), fp)) {
        char* key_start = strchr(line, '"');
        if (!key_start) continue;
        key_start++;
        char* key_end = strchr(key_start, '"');
        if (!key_end) continue;
        *key_end = '\0';
        
        char func_name[256];
        strncpy(func_name, key_start, sizeof(func_name) - 1);
        func_name[sizeof(func_name) - 1] = '\0';
        
        char* array_start = strchr(key_end + 1, '[');
        if (!array_start) continue;
        array_start++;
        
        uint64_t h = hash_callgraph(func_name);
        callgraph_entry_t* entry = g_call_graph[h];
        while (entry) {
            if (strcmp(entry->func_name, func_name) == 0) break;
            entry = entry->next;
        }
        
        if (!entry) {
            entry = calloc(1, sizeof(callgraph_entry_t));
            if (entry) {
                strncpy(entry->func_name, func_name, sizeof(entry->func_name) - 1);
                entry->func_name[sizeof(entry->func_name) - 1] = '\0';
                entry->next = g_call_graph[h];
                g_call_graph[h] = entry;
            }
        }
        
        char* p = array_start;
        while (*p && *p != ']') {
            if (*p == '"') {
                p++;
                char caller_name[256];
                int i = 0;
                while (*p && *p != '"' && i < 255) {
                    caller_name[i++] = *p++;
                }
                caller_name[i] = '\0';
                if (*p == '"') p++;
                
                if (entry && caller_name[0]) {
                    caller_node_t* node = malloc(sizeof(caller_node_t));
                    if (node) {
                        strncpy(node->name, caller_name, sizeof(node->name) - 1);
                        node->name[sizeof(node->name) - 1] = '\0';
                        node->next = entry->callers;
                        entry->callers = node;
                        entry->caller_count++;
                    }
                }
            } else {
                p++;
            }
        }
    }
    
    fclose(fp);
    g_callgraph_loaded = 1;
}

static void print_callers(const char* func_name) {
    if (!g_callgraph_loaded || !g_call_graph) return;
    
    uint64_t h = hash_callgraph(func_name);
    callgraph_entry_t* entry = g_call_graph[h];
    while (entry) {
        if (strcmp(entry->func_name, func_name) == 0) {
            if (entry->caller_count > 0) {
                printf("    Called by (%d):", entry->caller_count);
                int printed = 0;
                caller_node_t* node = entry->callers;
                while (node && printed < 5) {
                    printf(" %s", node->name);
                    node = node->next;
                    printed++;
                }
                if (entry->caller_count > 5) printf(" ...");
                printf("\n");
            }
            return;
        }
        entry = entry->next;
    }
}

static void free_call_graph(void) {
    if (!g_call_graph) return;
    for (int i = 0; i < CALLGRAPH_HASH_SIZE; i++) {
        callgraph_entry_t* entry = g_call_graph[i];
        while (entry) {
            caller_node_t* node = entry->callers;
            while (node) {
                caller_node_t* next = node->next;
                free(node);
                node = next;
            }
            callgraph_entry_t* next_entry = entry->next;
            free(entry);
            entry = next_entry;
        }
    }
    free(g_call_graph);
    g_call_graph = NULL;
    g_callgraph_loaded = 0;
}

// JSON escape string
static void json_escape(const char* src, char* dst, size_t dst_size) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        char c = src[i];
        if (c == '"' || c == '\\') {
            if (j < dst_size - 2) { dst[j++] = '\\'; dst[j++] = c; }
        } else if (c == '\n') {
            if (j < dst_size - 3) { dst[j++] = '\\'; dst[j++] = 'n'; }
        } else if (c == '\r') {
            if (j < dst_size - 3) { dst[j++] = '\\'; dst[j++] = 'r'; }
        } else if (c == '\t') {
            if (j < dst_size - 3) { dst[j++] = '\\'; dst[j++] = 't'; }
        } else if ((unsigned char)c < 0x20) {
            int n = snprintf(dst + j, dst_size - j, "\\u%04x", (unsigned char)c);
            if (n > 0) j += n;
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

// ---- 从源码取【完整定义】（修：chunks 被窗口切成 Part N，检索只返回一片）----
static int def_brace_end(const char* s, int n, int open) {
    int depth = 0;
    for (int i = open; i < n; i++) {
        if (s[i] == '{') depth++;
        else if (s[i] == '}') { if (--depth == 0) return i; }
    }
    return -1;
}

static int def_find(const char* s, int n, const char* name, const char* kind,
                    int* ps, int* pe) {
    int nl = (int)strlen(name);
    if (nl == 0) return 0;
    for (int i = 0; i + nl <= n; i++) {
        if (strncmp(s + i, name, nl) != 0) continue;
        if (i > 0 && (isalnum((unsigned char)s[i-1]) || s[i-1] == '_')) continue;
        int j = i + nl;
        if (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) continue;
        int k = j;
        while (k < n && (s[k]==' '||s[k]=='\t'||s[k]=='\n'||s[k]=='\r')) k++;
        int open = -1;
        if (k < n && s[k] == '{') {
            open = k;                                   // struct/union/enum NAME {
        } else if (k < n && s[k] == '(') {              // function NAME(...) {
            int d = 0, m = k;
            for (; m < n; m++) {
                if (s[m] == '(') d++;
                else if (s[m] == ')' && --d == 0) break;
            }
            if (m >= n) continue;
            int t = m + 1;
            while (t < n && (s[t]==' '||s[t]=='\t'||s[t]=='\n'||s[t]=='\r')) t++;
            if (t < n && s[t] == '{') open = t;
        }
        if (open < 0) continue;
        int close = def_brace_end(s, n, open);
        if (close < 0) continue;
        int st = i;
        while (st > 0 && s[st-1] != '\n') st--;          // 行首
        if (kind == NULL || strcmp(kind, "function") == 0) {
            if (st > 0) {                                // 带上返回类型那一行
                int prev = st - 2;
                while (prev > 0 && s[prev-1] != '\n') prev--;
                int punct = 0;
                for (int x = prev; x < st - 1; x++)
                    if (strchr(";{}(),", s[x])) { punct = 1; break; }
                if (!punct && st - 1 - prev > 0) st = prev;
            }
        }
        *ps = st; *pe = close;
        return 1;
    }
    return 0;
}

static void print_full_def(const char* file, const char* name, const char* kind) {
    FILE* fp = fopen(file, "r");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 8L*1024*1024) { fclose(fp); return; }
    char* buf = malloc(sz + 1);
    if (!buf) { fclose(fp); return; }
    size_t rd = fread(buf, 1, sz, fp);
    buf[rd] = '\0';
    fclose(fp);
    int st, en;
    if (def_find(buf, (int)rd, name, kind, &st, &en)) {
        printf("    FullDef:\n");
        for (int i = st; i <= en; i++) {
            if (i == st || buf[i-1] == '\n') printf("      | ");
            putchar(buf[i]);
        }
        if (buf[en] != '\n') printf("\n");
    }
    free(buf);
}

int main(int argc, char** argv) {
    const char* cache_dir = NULL;
    const char* query = NULL;
    int max_results = 10;
    const char* target_ns = NULL;
    
    const char* model_type = "jina";
    vector_search_opts_t opts = {0};
    
    // Parse arguments
    int arg_idx = 1;
    while (arg_idx < argc) {
        if (argv[arg_idx][0] == '-') {
            if (strcmp(argv[arg_idx], "--json") == 0) {
                output_json = 1;
                arg_idx++;
            } else if (strcmp(argv[arg_idx], "--rich") == 0) {
                output_rich = 1;
                arg_idx++;
            } else if (strcmp(argv[arg_idx], "--callgraph") == 0) {
                show_callgraph = 1;
                arg_idx++;
            } else if (strcmp(argv[arg_idx], "--snippet") == 0 && arg_idx + 1 < argc) {
                FILE* fp = fopen(argv[arg_idx + 1], "r");
                if (fp) {
                    static char snippet_buf[32768];
                    size_t n = fread(snippet_buf, 1, sizeof(snippet_buf) - 1, fp);
                    snippet_buf[n] = '\0';
                    fclose(fp);
                    query = snippet_buf;
                } else {
                    fprintf(stderr, "Failed to read snippet file: %s\n", argv[arg_idx + 1]);
                    return 1;
                }
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--model") == 0 && arg_idx + 1 < argc) {
                model_type = argv[arg_idx + 1];
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--kind") == 0 && arg_idx + 1 < argc) {
                opts.kind_filter = argv[arg_idx + 1];
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--lang") == 0 && arg_idx + 1 < argc) {
                opts.lang_filter = argv[arg_idx + 1];
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--file") == 0 && arg_idx + 1 < argc) {
                opts.file_filter = argv[arg_idx + 1];
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--help") == 0 || strcmp(argv[arg_idx], "-h") == 0) {
                printf("AI Agent Semantic Code Search\n");
                printf("Usage: %s [options] <cache_dir> <query> [max_results] [namespace]\n", argv[0]);
                printf("\nOptions:\n");
                printf("  --json       Output results as JSON\n");
                printf("  --rich       Include full code context\n");
                printf("  --callgraph  Show function call relationships\n");
                printf("  --snippet    Search by code snippet file\n");
                printf("  --model      Model type: jina (default) or mpnet\n");
                printf("  --kind       Filter by symbol kind\n");
                printf("  --lang       Filter by language\n");
                printf("  --file       Filter by filename pattern\n");
                printf("  -h, --help   Show this help\n");
                return 0;
            } else {
                fprintf(stderr, "Unknown option: %s\n", argv[arg_idx]);
                return 1;
            }
        } else {
            if (!cache_dir) {
                cache_dir = argv[arg_idx];
            } else if (!query) {
                query = argv[arg_idx];
            } else if (max_results == 10) {
                max_results = atoi(argv[arg_idx]);
                if (max_results <= 0) max_results = 10;
            } else {
                target_ns = argv[arg_idx];
            }
            arg_idx++;
        }
    }
    
    if (!cache_dir) {
        fprintf(stderr, "Error: cache_dir required\n");
        return 1;
    }
    
    if (!query) {
        fprintf(stderr, "Error: query required\n");
        return 1;
    }
    
    if (max_results <= 0) max_results = 10;
    if (max_results > 10000) max_results = 10000;
    
    if (strlen(query) > MAX_QUERY_LEN) {
        fprintf(stderr, "Error: query too long (max %d chars)\n", MAX_QUERY_LEN);
        return 1;
    }
    
    // Open vector engine (loads metadata, word_freq, call_graph internally)
    vector_engine_t* engine = vector_engine_open(cache_dir, model_type);
    if (!engine) {
        fprintf(stderr, "Failed to open vector engine: %s\n", vector_engine_error());
        return 1;
    }
    
    // Load call graph separately for display (library loads it too, but we need it for --callgraph)
    if (show_callgraph || output_rich) {
        load_call_graph(cache_dir);
    }
    
    // Enable boosts
    opts.use_keyword_boost = 1;
    opts.use_caller_boost = 1;
    
    // Search
    vector_result_t* results = malloc(max_results * sizeof(vector_result_t));
    if (!results) {
        fprintf(stderr, "Out of memory\n");
        vector_engine_close(engine);
        return 1;
    }
    
    int n = vector_engine_search_ex(engine, query, max_results, target_ns, &opts, results);
    
    if (n <= 0) {
        if (output_json) {
            printf("{\"query\":\"%s\",\"results\":[],\"total\":0}\n", query);
        } else {
            printf("No results found.\n");
        }
        free(results);
        vector_engine_close(engine);
        free_call_graph();
        return 0;
    }
    
    if (output_json) {
        char escaped_query[2048];
        json_escape(query, escaped_query, sizeof(escaped_query));
        
        printf("{\n");
        printf("  \"query\": \"%s\",\n", escaped_query);
        printf("  \"results_count\": %d,\n", n);
        printf("  \"results\": [\n");
        
        for (int i = 0; i < n; i++) {
            char escaped_name[512], escaped_file[1024], escaped_sig[1024], escaped_content[8192];
            json_escape(results[i].name, escaped_name, sizeof(escaped_name));
            json_escape(results[i].file, escaped_file, sizeof(escaped_file));
            json_escape(results[i].signature, escaped_sig, sizeof(escaped_sig));
            json_escape(results[i].content, escaped_content, sizeof(escaped_content));
            
            printf("    {\n");
            printf("      \"name\": \"%s\",\n", escaped_name);
            printf("      \"score\": %.4f", results[i].score);
            
            if (output_rich) {
                printf(",\n");
                printf("      \"file\": \"%s\",\n", escaped_file);
                printf("      \"line_start\": %d,\n", results[i].line_start);
                printf("      \"line_end\": %d,\n", results[i].line_end);
                printf("      \"language\": \"%s\",\n", results[i].language);
                printf("      \"kind\": \"%s\",\n", results[i].kind);
                printf("      \"signature\": \"%s\",\n", escaped_sig);
                printf("      \"content\": \"%s\"", escaped_content);
            }
            
            printf("\n    }%s\n", (i < n - 1) ? "," : "");
        }
        
        printf("  ]\n");
        printf("}\n");
    } else {
        if (output_rich) {
            printf("\nTop %d results:\n\n", n);
            for (int i = 0; i < n; i++) {
                printf("─");
                for (int k = 0; k < 69; k++) printf("─");
                printf("\n");
                
                printf("[%d] %s (%.4f)\n", i + 1, results[i].name, results[i].score);
                
                if (results[i].signature[0]) {
                    printf("    Signature: %s\n", results[i].signature);
                }
                if (results[i].file[0]) {
                    printf("    Location:  %s:%d\n", results[i].file, results[i].line_start);
                }
                if (results[i].language[0]) {
                    printf("    Language:  %s\n", results[i].language);
                }
                // 优先从源码取【完整定义】（修 chunks 被窗口切碎、只返回一片的问题）
                if (results[i].file[0] && results[i].name[0]) {
                    print_full_def(results[i].file, results[i].name, results[i].kind);
                }
                if (results[i].content[0]) {
                    printf("    Code(window):\n");
                    char* p = results[i].content;
                    int line_no = 0;
                    while (*p && line_no < 6) {
                        printf("      %c ", '|');
                        while (*p && *p != '\n') putchar(*p++);
                        printf("\n");
                        if (*p == '\n') p++;
                        line_no++;
                    }
                    if (*p) printf("      ...\n");
                }
                if (show_callgraph || output_rich) {
                    print_callers(results[i].name);
                }
                printf("\n");
            }
        } else {
            printf("\nTop %d results:\n", n);
            printf("%-50s %s\n", "Name", "Score");
            printf("%-50s %s\n", "----", "-----");
            for (int i = 0; i < n; i++) {
                printf("%-50s %.4f\n", results[i].name, results[i].score);
            }
        }
    }
    
    if (n > 0 && results[0].score < 0.30f) {
        fprintf(stderr,
            "\n[WARN] 低置信度: top1=%.4f < 0.30。索引可能已损坏或与查询不匹配；\n"
            "       请确认 vectors/*.jina.bin.hnsw 新于 .bin，必要时重跑: ai_code_search.sh vector <cache>\n",
            results[0].score);
    }

    free(results);
    vector_engine_close(engine);
    free_call_graph();
    return 0;
}
