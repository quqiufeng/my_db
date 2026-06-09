/*
 * cache_import.c — C implementation of code_to_memory.py
 * 
 * Imports analysis results into KV Cache using jansson for JSON parsing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <jansson.h>

#define MAX_LINE (1024 * 1024)
#define SYNC_INTERVAL 5000
#define MAX_VALUE_LEN (1024 * 1024)

// Cache API from libmydb.so
extern void* cache_open(const char* dir, size_t max_size);
extern void cache_close(void* cache);
extern int cache_sync(void* cache);
extern int cache_set(void* cache, const char* key, const char* value, uint64_t ttl);
extern size_t cache_memory_used(void* cache);

static void* g_cache = NULL;
static int g_total_keys = 0;

static void log_info(const char* msg) {
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", tm);
    printf("[%s] %s\n", time_str, msg);
}

static void log_warn(const char* msg) {
    fprintf(stderr, "[WARN] %s\n", msg);
}

// Safe cache set with size check and periodic sync
static int safe_set_json(const char* key, const char* value) {
    size_t len = strlen(value);
    if (len >= MAX_VALUE_LEN) {
        log_warn("Value too large, skipping");
        return 0;
    }
    int ret = cache_set(g_cache, key, value, 0);
    if (ret == 0) {
        g_total_keys++;
        if (g_total_keys % SYNC_INTERVAL == 0) {
            cache_sync(g_cache);
            char msg[64];
            snprintf(msg, sizeof(msg), "Synced at %d keys", g_total_keys);
            log_info(msg);
        }
    }
    return ret == 0;
}

// Trim leading '/' from filepath
static void trim_slash(char* s) {
    if (s && s[0] == '/') {
        memmove(s, s + 1, strlen(s));
    }
}

// Build chunk JSON value from jansson object
static int import_chunk(const char* namespace, json_t* chunk) {
    const char* name = json_string_value(json_object_get(chunk, "name"));
    const char* file = json_string_value(json_object_get(chunk, "file"));
    const char* kind = json_string_value(json_object_get(chunk, "kind"));
    
    if (!name || !file) return 0;
    if (kind && (strcmp(kind, "header") == 0 || strcmp(kind, "file") == 0)) return 0;
    
    // Skip non-meaningful kinds
    const char* meaningful[] = {"function", "method", "class", "struct", 
        "namespace", "macro", "typedef", "enum", "interface", "prototype"};
    int is_meaningful = 0;
    if (kind) {
        for (size_t i = 0; i < sizeof(meaningful)/sizeof(meaningful[0]); i++) {
            if (strcmp(kind, meaningful[i]) == 0) { is_meaningful = 1; break; }
        }
    }
    if (!is_meaningful) return 0;
    
    char filepath[512];
    strncpy(filepath, file, sizeof(filepath) - 1);
    filepath[sizeof(filepath) - 1] = '\0';
    trim_slash(filepath);
    
    json_int_t line_start = json_integer_value(json_object_get(chunk, "line_start"));
    json_int_t line_end = json_integer_value(json_object_get(chunk, "line_end"));
    const char* language = json_string_value(json_object_get(chunk, "language"));
    const char* signature = json_string_value(json_object_get(chunk, "signature"));
    const char* docstring = json_string_value(json_object_get(chunk, "docstring"));
    const char* content = json_string_value(json_object_get(chunk, "content"));
    
    if (content && strlen(content) == 0) return 0;  // Skip empty content
    
    // Truncate content to first 50 lines / 16KB
    char content_trunc[16384] = {0};
    if (content) {
        int line_count = 0;
        size_t j = 0;
        for (size_t i = 0; content[i] && j < sizeof(content_trunc) - 1; i++) {
            if (content[i] == '\n') line_count++;
            if (line_count >= 50) break;
            content_trunc[j++] = content[i];
        }
        content_trunc[j] = '\0';
    }
    
    // Build key
    char key[1024];
    snprintf(key, sizeof(key), "%s/chunks/%s/%s", namespace, filepath, name);
    
    // Build JSON value using jansson
    char id_buf[1024];
    snprintf(id_buf, sizeof(id_buf), "%s:%s:%s", namespace, filepath, name);
    
    json_t* value = json_object();
    json_object_set_new(value, "id", json_string(id_buf));
    json_object_set_new(value, "name", json_string(name));
    json_object_set_new(value, "file", json_string(filepath));
    json_object_set_new(value, "kind", json_string(kind ? kind : ""));
    json_object_set_new(value, "line_start", json_integer(line_start));
    json_object_set_new(value, "line_end", json_integer(line_end));
    json_object_set_new(value, "language", json_string(language ? language : ""));
    json_object_set_new(value, "signature", json_string(signature ? signature : ""));
    json_object_set_new(value, "docstring", json_string(docstring ? docstring : ""));
    json_object_set_new(value, "content", json_string(content_trunc));
    
    // Tags array
    json_t* tags = json_array();
    if (signature) json_array_append_new(tags, json_string(signature));
    if (language) json_array_append_new(tags, json_string(language));
    if (kind) json_array_append_new(tags, json_string(kind));
    json_object_set_new(value, "tags", tags);
    
    char* json_str = json_dumps(value, JSON_COMPACT);
    json_decref(value);
    
    if (!json_str) {
        fprintf(stderr, "DEBUG: json_dumps failed for %s\n", name);
        return 0;
    }
    
    int ret = safe_set_json(key, json_str);
    free(json_str);
    return ret;
}

// Import chunks_meta.jsonl
static void import_chunks(const char* analysis_dir, const char* namespace) {
    char path[512];
    snprintf(path, sizeof(path), "%s/chunks_meta.jsonl", analysis_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) {
        log_warn("chunks_meta.jsonl not found");
        return;
    }
    
    log_info("Phase 1: Importing chunks...");
    
    char* line = malloc(MAX_LINE);
    int line_count = 0;
    int valid = 0;
    int stored = 0;
    
    // Build symbol index while importing chunks
    json_t* symbol_map = json_object();  // name -> array of symbol info
    
    while (fgets(line, MAX_LINE, fp)) {
        line_count++;
        json_error_t error;
        json_t* chunk = json_loads(line, 0, &error);
        if (!chunk) continue;
        
        valid++;
        if (import_chunk(namespace, chunk)) {
            stored++;
            
            // Build symbol index
            const char* name = json_string_value(json_object_get(chunk, "name"));
            const char* file = json_string_value(json_object_get(chunk, "file"));
            const char* kind = json_string_value(json_object_get(chunk, "kind"));
            json_int_t line_start = json_integer_value(json_object_get(chunk, "line_start"));
            const char* signature = json_string_value(json_object_get(chunk, "signature"));
            const char* language = json_string_value(json_object_get(chunk, "language"));
            
            if (name && file) {
                json_t* sym_arr = json_object_get(symbol_map, name);
                if (!sym_arr) {
                    sym_arr = json_array();
                    json_object_set_new(symbol_map, name, sym_arr);
                }
                json_t* sym = json_object();
                json_object_set_new(sym, "name", json_string(name));
                json_object_set_new(sym, "kind", json_string(kind ? kind : ""));
                json_object_set_new(sym, "file", json_string(file));
                json_object_set_new(sym, "line", json_integer(line_start));
                json_object_set_new(sym, "signature", json_string(signature ? signature : ""));
                json_object_set_new(sym, "language", json_string(language ? language : ""));
                json_array_append_new(sym_arr, sym);
            }
        }
        json_decref(chunk);
    }
    
    free(line);
    fclose(fp);
    
    // Store symbol index
    const char* sym_name;
    json_t* sym_arr;
    int symbols_stored = 0;
    json_object_foreach(symbol_map, sym_name, sym_arr) {
        char key[1024];
        snprintf(key, sizeof(key), "%s/symbols/%s", namespace, sym_name);
        char* str = json_dumps(sym_arr, JSON_COMPACT);
        if (str) {
            safe_set_json(key, str);
            free(str);
            symbols_stored++;
        }
    }
    json_decref(symbol_map);
    
    char msg[256];
    snprintf(msg, sizeof(msg), "Stored %d chunks, %d symbols out of %d valid", stored, symbols_stored, valid);
    log_info(msg);
}

// Import call_graph.json
static void import_callgraph(const char* analysis_dir, const char* namespace) {
    char path[512];
    snprintf(path, sizeof(path), "%s/call_graph.json", analysis_dir);
    
    json_error_t error;
    json_t* root = json_load_file(path, 0, &error);
    if (!root) {
        log_warn("call_graph.json not found or invalid");
        return;
    }
    
    log_info("Phase 2: Importing call graph...");
    
    const char* func_name;
    json_t* func_data;
    int stored_callers = 0;
    int stored_callees = 0;
    
    json_object_foreach(root, func_name, func_data) {
        json_t* calls = json_object_get(func_data, "calls");
        if (!calls || !json_is_array(calls)) continue;
        
        // Build callers and callees maps
        json_t* callers = json_array();
        json_t* callees = json_array();
        
        size_t idx;
        json_t* call;
        json_array_foreach(calls, idx, call) {
            const char* caller = json_string_value(json_object_get(call, "function"));
            const char* file = json_string_value(json_object_get(call, "file"));
            json_int_t line = json_integer_value(json_object_get(call, "line"));
            const char* args = json_string_value(json_object_get(call, "arguments"));
            
            if (!caller) continue;
            
            json_t* site = json_object();
            json_object_set_new(site, "name", json_string(caller));
            json_object_set_new(site, "file", json_string(file ? file : ""));
            json_object_set_new(site, "line", json_integer(line));
            json_object_set_new(site, "args", json_string(args ? args : ""));
            json_array_append_new(callers, site);
            
            json_t* site2 = json_object();
            json_object_set_new(site2, "name", json_string(func_name));
            json_object_set_new(site2, "file", json_string(file ? file : ""));
            json_object_set_new(site2, "line", json_integer(line));
            json_object_set_new(site2, "args", json_string(args ? args : ""));
            json_array_append_new(callees, site2);
        }
        
        // Store callers: who calls this function
        if (json_array_size(callers) > 0) {
            char key[512];
            snprintf(key, sizeof(key), "%s/callers/%s", namespace, func_name);
            json_t* val = json_object();
            json_object_set_new(val, "count", json_integer(json_array_size(callers)));
            json_object_set_new(val, "callers", callers);
            char* str = json_dumps(val, JSON_COMPACT);
            if (str) {
                safe_set_json(key, str);
                free(str);
                stored_callers++;
            }
            json_decref(val);
        } else {
            json_decref(callers);
        }
        
        // Store callees: what this function calls
        if (json_array_size(callees) > 0) {
            char key[512];
            snprintf(key, sizeof(key), "%s/callees/%s", namespace, func_name);
            json_t* val = json_object();
            json_object_set_new(val, "count", json_integer(json_array_size(callees)));
            json_object_set_new(val, "callees", callees);
            char* str = json_dumps(val, JSON_COMPACT);
            if (str) {
                safe_set_json(key, str);
                free(str);
                stored_callees++;
            }
            json_decref(val);
        } else {
            json_decref(callees);
        }
    }
    
    json_decref(root);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Stored %d caller entries, %d callee entries", stored_callers, stored_callees);
    log_info(msg);
}

// Import dataflow.json
static void import_dataflow(const char* analysis_dir, const char* namespace) {
    char path[512];
    snprintf(path, sizeof(path), "%s/dataflow.json", analysis_dir);
    
    json_error_t error;
    json_t* root = json_load_file(path, 0, &error);
    if (!root) {
        log_warn("dataflow.json not found or invalid");
        return;
    }
    
    log_info("Phase 3: Importing dataflow...");
    
    const char* var_name;
    json_t* var_data;
    int stored_vars = 0;
    
    json_object_foreach(root, var_name, var_data) {
        json_t* occurrences = json_object_get(var_data, "occurrences");
        if (!occurrences || !json_is_array(occurrences)) continue;
        
        // Store variable occurrences
        char key[512];
        snprintf(key, sizeof(key), "%s/dataflow/vars/%s", namespace, var_name);
        json_t* val = json_object();
        json_object_set_new(val, "count", json_integer(json_array_size(occurrences)));
        json_object_set_new(val, "occurrences", json_incref(occurrences));
        
        char* str = json_dumps(val, JSON_COMPACT);
        if (str) {
            safe_set_json(key, str);
            free(str);
            stored_vars++;
        }
        json_decref(val);
        
        // Group by function
        json_t* func_vars = json_object();
        size_t idx;
        json_t* occ;
        json_array_foreach(occurrences, idx, occ) {
            const char* func = json_string_value(json_object_get(occ, "func"));
            if (!func) func = "unknown";
            json_t* arr = json_object_get(func_vars, func);
            if (!arr) {
                arr = json_array();
                json_object_set_new(func_vars, func, arr);
            }
            json_array_append(arr, occ);
        }
        
        // Store function-level data
        const char* func_name;
        json_t* vars;
        json_object_foreach(func_vars, func_name, vars) {
            char fkey[512];
            snprintf(fkey, sizeof(fkey), "%s/dataflow/func/%s", namespace, func_name);
            json_t* fval = json_object();
            json_object_set_new(fval, "function", json_string(func_name));
            json_object_set_new(fval, "variable_count", json_integer(json_array_size(vars)));
            json_object_set_new(fval, "variables", json_incref(vars));
            
            char* fstr = json_dumps(fval, JSON_COMPACT);
            if (fstr) {
                safe_set_json(fkey, fstr);
                free(fstr);
            }
            json_decref(fval);
        }
        
        json_decref(func_vars);
    }
    
    json_decref(root);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Stored %d variables", stored_vars);
    log_info(msg);
}

// Register vector file reference
static void register_vectors(const char* analysis_dir, const char* namespace) {
    DIR* dir = opendir(analysis_dir);
    if (!dir) return;
    
    struct dirent* entry;
    char vector_file[512] = {0};
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".bin") && strstr(entry->d_name, "jina")) {
            snprintf(vector_file, sizeof(vector_file), "%s/%s", analysis_dir, entry->d_name);
            break;
        }
    }
    closedir(dir);
    
    if (vector_file[0]) {
        struct stat st;
        if (stat(vector_file, &st) == 0) {
            char key[512];
            snprintf(key, sizeof(key), "%s/vectors/path", namespace);
            
            json_t* val = json_object();
            json_object_set_new(val, "file", json_string(vector_file));
            json_object_set_new(val, "size_bytes", json_integer(st.st_size));
            json_object_set_new(val, "type", json_string("float32"));
            json_object_set_new(val, "description", json_string("Vector file reference"));
            
            char* str = json_dumps(val, JSON_COMPACT);
            if (str) {
                safe_set_json(key, str);
                free(str);
            }
            json_decref(val);
            
            char msg[256];
            snprintf(msg, sizeof(msg), "Vector file registered: %s", vector_file);
            log_info(msg);
        }
    }
}

// Store metadata
static void store_metadata(const char* namespace, const char* source_dir) {
    char key[512];
    snprintf(key, sizeof(key), "%s/_meta/info", namespace);
    
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    
    json_t* val = json_object();
    json_object_set_new(val, "type", json_string("code_repo"));
    json_object_set_new(val, "source_dir", json_string(source_dir));
    json_object_set_new(val, "imported_at", json_string(time_str));
    json_object_set_new(val, "total_keys", json_integer(g_total_keys));
    
    char* str = json_dumps(val, JSON_COMPACT);
    if (str) {
        safe_set_json(key, str);
        free(str);
    }
    json_decref(val);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <analysis_dir> <namespace> [--cache-dir <dir>]\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s /opt/code_caches/nginx_cache /code/nginx\n", argv[0]);
        printf("  %s ./linux_subsystems/mm_cache /code/linux/mm --cache-dir ./ai_memory\n", argv[0]);
        return 1;
    }
    
    const char* analysis_dir = argv[1];
    const char* namespace = argv[2];
    const char* cache_dir = "/memory";
    
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[i + 1];
            i++;
        }
    }
    
    log_info("Importing analysis into KV Cache");
    printf("  Namespace: %s\n", namespace);
    printf("  Source: %s\n", analysis_dir);
    printf("  Cache: %s\n", cache_dir);
    
    g_cache = cache_open(cache_dir, 2ULL * 1024 * 1024 * 1024);
    if (!g_cache) {
        fprintf(stderr, "[ERROR] Failed to open cache: %s\n", cache_dir);
        return 1;
    }
    
    import_chunks(analysis_dir, namespace);
    import_callgraph(analysis_dir, namespace);
    import_dataflow(analysis_dir, namespace);
    register_vectors(analysis_dir, namespace);
    store_metadata(namespace, analysis_dir);
    
    cache_sync(g_cache);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Import complete! Total keys: %d", g_total_keys);
    log_info(msg);
    
    snprintf(msg, sizeof(msg), "Memory used: %zu bytes", cache_memory_used(g_cache));
    log_info(msg);
    
    cache_close(g_cache);
    return 0;
}
