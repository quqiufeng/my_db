#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_NAME_LEN 256
#define MAX_FUNC 50000

typedef struct func_body {
    char name[MAX_NAME_LEN];
    char* content;
    struct func_body* next;
} func_body_t;

static func_body_t* g_bodies = NULL;
static int g_body_count = 0;

static char** g_func_names = NULL;
static int g_func_count = 0;

static void add_func_name(const char* name) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_func_names[i], name) == 0) return;
    }
    g_func_names[g_func_count] = strdup(name);
    g_func_count++;
}

static int is_func_known(const char* name) {
    for (int i = 0; i < g_func_count; i++) {
        if (strcmp(g_func_names[i], name) == 0) return 1;
    }
    return 0;
}

static void add_body(const char* name, const char* content) {
    func_body_t* b = malloc(sizeof(func_body_t));
    if (!b) return;
    strncpy(b->name, name, MAX_NAME_LEN - 1);
    b->name[MAX_NAME_LEN - 1] = '\0';
    b->content = strdup(content);
    b->next = g_bodies;
    g_bodies = b;
    g_body_count++;
}

static int contains_call(const char* content, const char* func_name) {
    int len = strlen(func_name);
    const char* p = content;
    while ((p = strstr(p, func_name)) != NULL) {
        if (p > content) {
            char c = *(p - 1);
            if (isalnum(c) || c == '_' || c == ':') {
                p += len;
                continue;
            }
        }
        char after = p[len];
        if (after == '(' || after == ' ' || after == '\t' || after == '\n') {
            return 1;
        }
        p += len;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Call Graph Builder\n");
        printf("Usage: %s <cache_dir>\n", argv[0]);
        return 1;
    }
    
    const char* cache_dir = argv[1];
    char meta_file[512], out_file[512];
    snprintf(meta_file, sizeof(meta_file), "%s/chunks_meta.jsonl", cache_dir);
    snprintf(out_file, sizeof(out_file), "%s/call_graph.json", cache_dir);
    
    g_func_names = malloc(MAX_FUNC * sizeof(char*));
    if (!g_func_names) return 1;
    
    printf("Phase 1: Loading function names...\n");
    
    FILE* fp = fopen(meta_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", meta_file);
        return 1;
    }
    
    char* line = NULL;
    size_t line_len = 0;
    
    while (getline(&line, &line_len, fp) != -1) {
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
    
    while (getline(&line, &line_len, fp) != -1) {
        char* name_p = strstr(line, "\"name\":\"");
        char* kind_p = strstr(line, "\"kind\":\"");
        char* content_p = strstr(line, "\"content\":\"");
        if (!name_p || !kind_p || !content_p) continue;
        
        name_p += 8;
        kind_p += 8;
        content_p += 11;
        
        char kind[32] = {0};
        int i = 0;
        while (*kind_p && *kind_p != '"' && i < 31) kind[i++] = *kind_p++;
        
        if (strcmp(kind, "function") != 0 && strcmp(kind, "method") != 0) continue;
        
        char name[256] = {0};
        i = 0;
        while (*name_p && *name_p != '"' && i < 255) name[i++] = *name_p++;
        
        // Skip if already have body for this function (avoid duplicates from chunking)
        static char processed[MAX_FUNC];
        int found = 0;
        for (int j = 0; j < g_func_count; j++) {
            if (strcmp(g_func_names[j], name) == 0) {
                if (processed[j]) { found = 1; break; }
                processed[j] = 1;
                break;
            }
        }
        if (found) continue;
        
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
        
        add_body(name, content);
        free(content);
    }
    fclose(fp);
    printf("  Loaded %d function bodies\n", g_body_count);
    
    printf("Phase 3: Building call graph...\n");
    
    FILE* out = fopen(out_file, "w");
    if (!out) {
        fprintf(stderr, "Failed to create %s\n", out_file);
        goto cleanup;
    }
    
    fprintf(out, "{\n");
    int first = 1;
    
    for (int f = 0; f < g_func_count; f++) {
        const char* callee = g_func_names[f];
        int callee_len = strlen(callee);
        if (callee_len < 2) continue; // Skip short names
        
        char callers[8192] = {0};
        int caller_count = 0;
        
        func_body_t* b = g_bodies;
        while (b) {
            if (strcmp(b->name, callee) != 0 && b->content) {
                if (contains_call(b->content, callee)) {
                    if (caller_count > 0) strcat(callers, ",");
                    strcat(callers, "\"");
                    strcat(callers, b->name);
                    strcat(callers, "\"");
                    caller_count++;
                    if (caller_count >= 50) break;
                }
            }
            b = b->next;
        }
        
        if (caller_count > 0) {
            if (!first) fprintf(out, ",\n");
            first = 0;
            fprintf(out, "  \"%s\": [%s]", callee, callers);
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