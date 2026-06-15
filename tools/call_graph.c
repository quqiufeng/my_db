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

typedef struct call_edge {
    char caller[MAX_NAME_LEN];
    char file[512];
    int line;
    char args[1024];
    struct call_edge* next;
} call_edge_t;

typedef struct func_node {
    char name[MAX_NAME_LEN];
    call_edge_t* calls;
    struct func_node* next;
} func_node_t;

static func_node_t* g_func_nodes = NULL;

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

static func_node_t* get_func_node(const char* name, int create) {
    func_node_t* n = g_func_nodes;
    while (n) {
        if (strcmp(n->name, name) == 0) return n;
        n = n->next;
    }
    if (!create) return NULL;
    n = malloc(sizeof(func_node_t));
    if (!n) return NULL;
    strncpy(n->name, name, MAX_NAME_LEN - 1);
    n->name[MAX_NAME_LEN - 1] = '\0';
    n->calls = NULL;
    n->next = g_func_nodes;
    g_func_nodes = n;
    return n;
}

static void add_call_edge(const char* callee, const char* caller, const char* file, int line, const char* args) {
    func_node_t* node = get_func_node(callee, 1);
    if (!node) return;

    // Deduplicate identical caller/file/line
    call_edge_t* e = node->calls;
    while (e) {
        if (strcmp(e->caller, caller) == 0 && strcmp(e->file, file) == 0 && e->line == line) return;
        e = e->next;
    }

    e = malloc(sizeof(call_edge_t));
    if (!e) return;
    strncpy(e->caller, caller, MAX_NAME_LEN - 1);
    e->caller[MAX_NAME_LEN - 1] = '\0';
    strncpy(e->file, file ? file : "", sizeof(e->file) - 1);
    e->file[sizeof(e->file) - 1] = '\0';
    e->line = line;
    strncpy(e->args, args ? args : "", sizeof(e->args) - 1);
    e->args[sizeof(e->args) - 1] = '\0';
    e->next = node->calls;
    node->calls = e;
}

static int is_func_kind(const char* kind) {
    return kind[0] && (
        strcmp(kind, "function") == 0 ||
        strcmp(kind, "method") == 0 ||
        strcmp(kind, "ts_function") == 0 ||
        strcmp(kind, "ts_method") == 0
    );
}

// Extract value of a JSON string field (handles simple escaped chars)
static int json_extract_str(const char* json, const char* key, char* out, int max_len) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char* p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                case '\\': out[i++] = '\\'; break;
                case '"': out[i++] = '"'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

static int json_extract_int(const char* json, const char* key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '"') p++;
    int val = 0;
    while (*p && isdigit((unsigned char)*p)) {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
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

            int pos = 0;
            for (int i = 0; i < arg_idx && pos < args_out_size - 1; i++) {
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

static void load_plugin_edges(const char* plugin_file) {
    FILE* fp = fopen(plugin_file, "r");
    if (!fp) return;

    char* line = NULL;
    size_t line_len = 0;
    int count = 0;

    while (getline(&line, &line_len, fp) != -1) {
        char type[32] = {0};
        json_extract_str(line, "type", type, sizeof(type));
        if (strcmp(type, "call_edge") != 0) continue;

        char caller[MAX_NAME_LEN] = {0};
        char callee[MAX_NAME_LEN] = {0};
        char file[512] = {0};

        json_extract_str(line, "caller", caller, sizeof(caller));
        json_extract_str(line, "callee", callee, sizeof(callee));
        json_extract_str(line, "caller_file", file, sizeof(file));
        int line_num = json_extract_int(line, "caller_line");

        if (!caller[0] || !callee[0]) continue;

        add_func_name(callee);
        add_func_name(caller);
        add_call_edge(callee, caller, file, line_num, "");
        count++;
    }

    free(line);
    fclose(fp);
    printf("  Loaded %d call edges from plugin output\n", count);
}

static void json_escape_str(const char* src, char* dst, int max_len) {
    int j = 0;
    for (int i = 0; src[i] && j < max_len - 1; i++) {
        unsigned char c = src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 < max_len) {
                dst[j++] = '\\';
                dst[j++] = c;
            }
        } else if (c == '\n') {
            if (j + 2 < max_len) { dst[j++] = '\\'; dst[j++] = 'n'; }
        } else if (c == '\r') {
            if (j + 2 < max_len) { dst[j++] = '\\'; dst[j++] = 'r'; }
        } else if (c == '\t') {
            if (j + 2 < max_len) { dst[j++] = '\\'; dst[j++] = 't'; }
        } else if (c < 0x20) {
            // skip control chars
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Call Graph Builder (Enhanced with TypeScript plugin support)\n");
        printf("Usage: %s <cache_dir>\n", argv[0]);
        return 1;
    }

    const char* cache_dir = argv[1];
    char meta_file[512], out_file[512], plugin_file[512];
    snprintf(meta_file, sizeof(meta_file), "%s/chunks_meta.jsonl", cache_dir);
    snprintf(out_file, sizeof(out_file), "%s/call_graph.json", cache_dir);
    snprintf(plugin_file, sizeof(plugin_file), "%s/plugin_output.jsonl", cache_dir);

    g_func_names = malloc(MAX_FUNC * sizeof(char*));
    if (!g_func_names) return 1;
    memset(g_name_hash, -1, sizeof(g_name_hash));

    printf("Phase 1: Loading functions and bodies\n");
    fflush(stdout);

    FILE* fp = fopen(meta_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", meta_file);
        return 1;
    }

    char* line = NULL;
    size_t line_len = 0;
    int processed_count = 0;
    int body_count = 0;

    typedef struct func_body {
        char name[MAX_NAME_LEN];
        char file[512];
        int line_start;
        char* content;
        struct func_body* next;
    } func_body_t;

    func_body_t* bodies = NULL;

    while (getline(&line, &line_len, fp) != -1) {
        char* name_p = strstr(line, "\"name\":\"");
        char* file_p = strstr(line, "\"file\":\"");
        char* kind_p = strstr(line, "\"kind\":\"");
        char* line_p = strstr(line, "\"line_start\":");
        char* content_p = strstr(line, "\"content\":\"");
        if (!name_p || !kind_p) continue;

        name_p += 8;
        kind_p += 8;
        char kind[64] = {0};
        int i = 0;
        while (*kind_p && *kind_p != '"' && i < 63) kind[i++] = *kind_p++;
        if (!is_func_kind(kind)) continue;

        char name[256] = {0};
        i = 0;
        while (*name_p && *name_p != '"' && i < 255) name[i++] = *name_p++;
        if (!name[0]) continue;

        add_func_name(name);
        processed_count++;

        if (file_p && content_p) {
            file_p += 8;
            char file[512] = {0};
            i = 0;
            while (*file_p && *file_p != '"' && i < 511) file[i++] = *file_p++;

            int line_start = line_p ? atoi(line_p + 13) : 0;
            content_p += 11;

            char* content = malloc(line_len);
            if (content) {
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

                func_body_t* b = malloc(sizeof(func_body_t));
                strncpy(b->name, name, MAX_NAME_LEN - 1);
                b->name[MAX_NAME_LEN - 1] = '\0';
                strncpy(b->file, file, sizeof(b->file) - 1);
                b->file[sizeof(b->file) - 1] = '\0';
                b->line_start = line_start;
                b->content = content;
                b->next = bodies;
                bodies = b;
                body_count++;
            }
        }

        if (g_func_count >= MAX_FUNC) break;
    }
    fclose(fp);
    printf("  Found %d unique functions, loaded %d bodies\n", g_func_count, body_count);

    printf("Phase 2: Loading plugin call edges\n");
    load_plugin_edges(plugin_file);

    printf("Phase 3: Building call graph with arguments...\n");

    // Text-based call detection for bodies (backward compat + ctags)
    func_body_t* b = bodies;
    while (b) {
        for (int f = 0; f < g_func_count; f++) {
            const char* callee = g_func_names[f];
            if (strcmp(b->name, callee) == 0) continue;
            if (strlen(callee) < 2) continue;

            char args[1024];
            if (find_call_with_args(b->content, callee, args, sizeof(args))) {
                add_call_edge(callee, b->name, b->file, b->line_start, args);
            }
        }
        b = b->next;
    }

    FILE* out = fopen(out_file, "w");
    if (!out) {
        fprintf(stderr, "Failed to create %s\n", out_file);
        return 1;
    }

    fprintf(out, "{\n");
    int first = 1;

    func_node_t* node = g_func_nodes;
    while (node) {
        if (!node->calls) { node = node->next; continue; }

        char esc_name[MAX_NAME_LEN * 2];
        json_escape_str(node->name, esc_name, sizeof(esc_name));

        if (!first) fprintf(out, ",\n");
        first = 0;
        fprintf(out, "  \"%s\": {\n", esc_name);
        fprintf(out, "    \"calls\": [\n");

        call_edge_t* e = node->calls;
        int first_edge = 1;
        while (e) {
            char esc_caller[MAX_NAME_LEN * 2];
            char esc_file[1024];
            json_escape_str(e->caller, esc_caller, sizeof(esc_caller));
            json_escape_str(e->file, esc_file, sizeof(esc_file));

            if (!first_edge) fprintf(out, ",\n");
            first_edge = 0;
            fprintf(out, "      {\"function\":\"%s\",\"file\":\"%s\",\"line\":%d",
                    esc_caller, esc_file, e->line);
            if (e->args[0]) {
                fprintf(out, ",\"arguments\":[\"");
                const char* ap = e->args;
                while (*ap) {
                    if (*ap == '"' || *ap == '\\') fputc('\\', out);
                    fputc(*ap, out);
                    ap++;
                }
                fprintf(out, "\"]");
            }
            fprintf(out, "}");
            e = e->next;
        }

        fprintf(out, "\n    ]\n  }");
        node = node->next;
    }

    fprintf(out, "\n}\n");
    fclose(out);
    printf("Saved call graph to %s\n", out_file);

    // Cleanup
    for (int i = 0; i < g_func_count; i++) free(g_func_names[i]);
    free(g_func_names);

    while (bodies) {
        func_body_t* next = bodies->next;
        free(bodies->content);
        free(bodies);
        bodies = next;
    }

    while (g_func_nodes) {
        func_node_t* fn = g_func_nodes;
        g_func_nodes = fn->next;
        call_edge_t* e = fn->calls;
        while (e) {
            call_edge_t* next = e->next;
            free(e);
            e = next;
        }
        free(fn);
    }

    free(line);
    return 0;
}
