/*
 * cache_query.c — C implementation of agent_query.py
 * 
 * AI-friendly structured query interface for KV Cache.
 * 
 * Usage:
 *   ./cache_query <query> [--repo <namespace>] [--type <type>]
 * 
 * Example:
 *   ./cache_query ngx_palloc --repo /code/nginx --type context
 *   ./cache_query "memory pool" --repo /code/nginx --type search
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <getopt.h>
#include <jansson.h>

// Cache API from libmydb.so
extern void* cache_open(const char* dir, size_t max_size);
extern void cache_close(void* cache);
extern const char* cache_get(void* cache, const char* key);

static void* g_cache = NULL;

static int is_identifier(const char* s) {
    if (!s || !*s) return 0;
    if (!isalpha((unsigned char)s[0]) && s[0] != '_') return 0;
    for (size_t i = 1; s[i]; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    }
    return 1;
}

static const char* detect_query_type(const char* query, const char* repo) {
    if (strncmp(query, "/code/", 6) == 0) return "exact";
    if (strchr(query, ' ') == NULL && is_identifier(query) && repo) return "context";
    if (strchr(query, ' ') != NULL) return "search";
    return "symbol";
}

// Exact query
static json_t* do_exact(const char* query) {
    const char* value = cache_get(g_cache, query);
    if (!value) return json_array();
    
    json_error_t error;
    json_t* data = json_loads(value, 0, &error);
    if (!data) return json_array();
    
    json_t* arr = json_array();
    if (json_is_array(data)) {
        json_t* obj = json_object();
        json_object_set_new(obj, "_key", json_string(query));
        json_object_set_new(obj, "_source", json_string("exact"));
        json_object_set_new(obj, "entries", data);
        json_array_append_new(arr, obj);
    } else {
        json_object_set_new(data, "_key", json_string(query));
        json_object_set_new(data, "_source", json_string("exact"));
        json_array_append_new(arr, data);
    }
    return arr;
}

// Symbol query
static json_t* do_symbol(const char* name, const char* repo) {
    json_t* arr = json_array();
    
    char key[512];
    snprintf(key, sizeof(key), "%s/symbols/%s", repo, name);
    const char* value = cache_get(g_cache, key);
    if (!value) return arr;
    
    json_error_t error;
    json_t* symbols = json_loads(value, 0, &error);
    if (!symbols || !json_is_array(symbols)) {
        if (symbols) json_decref(symbols);
        return arr;
    }
    
    size_t idx;
    json_t* sym;
    json_array_foreach(symbols, idx, sym) {
        const char* file = json_string_value(json_object_get(sym, "file"));
        if (!file) continue;
        
        char ckey[1024];
        snprintf(ckey, sizeof(ckey), "%s/chunks/%s/%s", repo, file, name);
        const char* cvalue = cache_get(g_cache, ckey);
        if (cvalue) {
            json_t* chunk = json_loads(cvalue, 0, &error);
            if (chunk) {
                json_object_set_new(chunk, "_source", json_string("symbol"));
                json_object_set_new(chunk, "_key", json_string(ckey));
                json_array_append_new(arr, chunk);
            }
        } else {
            json_t* obj = json_object();
            json_object_set(obj, "name", json_object_get(sym, "name"));
            json_object_set(obj, "kind", json_object_get(sym, "kind"));
            json_object_set(obj, "file", json_object_get(sym, "file"));
            json_object_set(obj, "line", json_object_get(sym, "line"));
            json_object_set_new(obj, "_source", json_string("symbol"));
            json_object_set_new(obj, "_key", json_string(key));
            json_array_append_new(arr, obj);
        }
    }
    
    json_decref(symbols);
    return arr;
}

// Context query: symbol + callers + callees + call_sites + dataflow
static json_t* do_context(const char* name, const char* repo) {
    json_t* results = json_array();
    json_t* context = json_object();
    
    // Get symbol
    json_t* symbol_result = do_symbol(name, repo);
    json_t* symbol = NULL;
    if (json_array_size(symbol_result) > 0) {
        symbol = json_array_get(symbol_result, 0);
        json_incref(symbol);
    }
    json_decref(symbol_result);
    
    // Get callers
    char key[512];
    snprintf(key, sizeof(key), "%s/callers/%s", repo, name);
    const char* cvalue = cache_get(g_cache, key);
    json_t* callers = json_array();
    if (cvalue) {
        json_error_t error;
        json_t* data = json_loads(cvalue, 0, &error);
        if (data) {
            json_t* arr = json_object_get(data, "callers");
            if (arr) {
                json_decref(callers);
                callers = json_incref(arr);
            }
            json_decref(data);
        }
    }
    
    // Get callees
    snprintf(key, sizeof(key), "%s/callees/%s", repo, name);
    cvalue = cache_get(g_cache, key);
    json_t* callees = json_array();
    if (cvalue) {
        json_error_t error;
        json_t* data = json_loads(cvalue, 0, &error);
        if (data) {
            json_t* arr = json_object_get(data, "callees");
            if (arr) {
                json_decref(callees);
                callees = json_incref(arr);
            }
            json_decref(data);
        }
    }
    
    // Get dataflow
    snprintf(key, sizeof(key), "%s/dataflow/vars/%s", repo, name);
    cvalue = cache_get(g_cache, key);
    json_t* dataflow = json_array();
    if (cvalue) {
        json_error_t error;
        json_t* data = json_loads(cvalue, 0, &error);
        if (data) {
            json_t* obj = json_object();
            json_object_set_new(obj, "type", json_string("variable"));
            json_object_set_new(obj, "data", data);
            json_array_append_new(dataflow, obj);
        }
    }
    
    // Build context
    if (symbol) json_object_set_new(context, "symbol", symbol);
    json_object_set_new(context, "callers", callers);
    json_object_set_new(context, "callees", callees);
    json_object_set_new(context, "call_sites", json_array());  // TODO: implement
    json_object_set_new(context, "dataflow", dataflow);
    
    // Build stats
    json_t* stats = json_object();
    json_object_set_new(stats, "caller_count", json_integer(json_array_size(callers)));
    json_object_set_new(stats, "callee_count", json_integer(json_array_size(callees)));
    json_object_set_new(stats, "call_site_count", json_integer(0));
    json_object_set_new(stats, "dataflow_entries", json_integer(json_array_size(dataflow)));
    
    // Build result
    json_t* result = json_object();
    json_object_set_new(result, "results", json_array());
    if (symbol) {
        json_t* arr = json_object_get(result, "results");
        json_array_append(arr, symbol);
    }
    json_object_set_new(result, "context", context);
    json_object_set_new(result, "stats", stats);
    
    return result;
}

// Search query (simplified: keyword scan)
static json_t* do_search(const char* query, const char* repo) {
    json_t* arr = json_array();
    // TODO: Implement tag/prefix/fuzzy search
    // For now, return empty array
    return arr;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: %s <query> [--repo <namespace>] [--type <type>] [--cache-dir <dir>]\n", argv[0]);
        printf("\nQuery types: auto, exact, symbol, context, search\n");
        printf("\nExamples:\n");
        printf("  %s ngx_palloc --repo /code/nginx --type context\n", argv[0]);
        printf("  %s \"memory pool\" --repo /code/nginx --type search\n", argv[0]);
        printf("  %s /code/nginx/symbols/ngx_palloc --type exact\n", argv[0]);
        return 1;
    }
    
    const char* query = argv[1];
    const char* repo = NULL;
    const char* type = "auto";
    const char* cache_dir = "./ai_code_memory";
    int pretty = 0;
    
    // Parse arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            type = argv[++i];
        } else if (strcmp(argv[i], "--cache-dir") == 0 && i + 1 < argc) {
            cache_dir = argv[++i];
        } else if (strcmp(argv[i], "--pretty") == 0 || strcmp(argv[i], "-p") == 0) {
            pretty = 1;
        }
    }
    
    // Auto-detect type
    if (strcmp(type, "auto") == 0) {
        type = detect_query_type(query, repo);
    }
    
    // Open cache
    g_cache = cache_open(cache_dir, 2ULL * 1024 * 1024 * 1024);
    if (!g_cache) {
        fprintf(stderr, "[ERROR] Failed to open cache: %s\n", cache_dir);
        return 1;
    }
    
    // Build response
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    
    json_t* response = json_object();
    json_object_set_new(response, "query", json_string(query));
    json_object_set_new(response, "type", json_string(type));
    json_object_set_new(response, "repo", json_string(repo ? repo : ""));
    
    if (strcmp(type, "exact") == 0) {
        json_t* results = do_exact(query);
        json_object_set_new(response, "results", results);
        json_object_set_new(response, "context", json_object());
        json_object_set_new(response, "stats", json_object());
    } else if (strcmp(type, "symbol") == 0) {
        json_t* results = do_symbol(query, repo);
        json_object_set_new(response, "results", results);
        json_object_set_new(response, "context", json_object());
        json_object_set_new(response, "stats", json_object());
    } else if (strcmp(type, "context") == 0) {
        json_t* result = do_context(query, repo);
        json_t* results = json_object_get(result, "results");
        json_t* ctx = json_object_get(result, "context");
        json_t* stats = json_object_get(result, "stats");
        if (results) json_object_set(response, "results", results);
        if (ctx) json_object_set(response, "context", ctx);
        if (stats) json_object_set(response, "stats", stats);
        json_decref(result);
    } else {
        json_t* results = do_search(query, repo);
        json_object_set_new(response, "results", results);
        json_object_set_new(response, "context", json_object());
        json_object_set_new(response, "stats", json_object());
    }
    
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    long elapsed_ms = (ts_end.tv_sec - ts_start.tv_sec) * 1000 + 
                      (ts_end.tv_nsec - ts_start.tv_nsec) / 1000000;
    json_object_set_new(response, "timing_ms", json_integer(elapsed_ms));
    
    // Print JSON
    char* output = json_dumps(response, pretty ? JSON_INDENT(2) : JSON_COMPACT);
    if (output) {
        printf("%s\n", output);
        free(output);
    } else {
        fprintf(stderr, "[ERROR] json_dumps failed\n");
    }
    
    json_decref(response);
    cache_close(g_cache);
    return 0;
}
