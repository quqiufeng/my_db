#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "cache.h"
#include "onnx_embedder.h"
#include "cache_hnsw.h"

#define DIM 768
#define MAX_RESULTS 10
#define MAX_QUERY_LEN 1024
#define MAX_NAMESPACES 32
#define MAX_RESULT_DETAILS 100
#define META_HASH_SIZE 262144  // 256K buckets for fast lookup

// Simple in-memory metadata entry (no cache needed)
typedef struct meta_entry {
    char name[256];
    char file[512];
    char kind[32];
    char language[32];
    char signature[512];
    char content[4096];
    int line_start;
    int line_end;
    struct meta_entry* next;  // hash collision chain
} meta_entry_t;

// Global metadata hash table
static meta_entry_t** g_meta_hash = NULL;
static int g_meta_loaded = 0;

// Word frequency table for TF-IDF
#define WORD_HASH_SIZE 65536
typedef struct idf_entry {
    char word[64];
    float idf;
    struct idf_entry* next;
} idf_entry_t;
static idf_entry_t** g_idf_table = NULL;
static int g_idf_loaded = 0;

// Caller count table for PageRank-like weighting
#define CALLER_HASH_SIZE 65536
typedef struct caller_count_entry {
    char name[256];
    int count;
    struct caller_count_entry* next;
} caller_count_entry_t;
static caller_count_entry_t** g_caller_table = NULL;
static int g_caller_count_loaded = 0;

// FNV-1a hash for name lookup
static uint64_t hash_name(const char* name) {
    uint64_t h = 0xcbf29ce484222325;
    for (const char* p = name; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 0x100000001b3;
    }
    return h;
}

typedef struct {
    char name[256];
    float score;
} search_result_t;

typedef struct {
    char namespace[256];
    char vec_file[512];
    char idx_file[512];
    char hnsw_file[512];
    char model[32];
    uint32_t count;
} vector_source_t;

typedef struct {
    char name[256];
    char file[512];
    int line_start;
    int line_end;
    char signature[512];
    char docstring[1024];
    char content[4096];
    char language[32];
    char kind[32];
} result_detail_t;

typedef struct {
    char kind_filter[32];
    char lang_filter[32];
    char file_filter[256];
} search_filter_t;

static int output_json = 0;
static int output_rich = 0;
static int show_callgraph = 0;

// Call graph support
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
        // Parse "function_name": ["caller1", "caller2"]
        char* key_start = strchr(line, '"');
        if (!key_start) continue;
        key_start++;
        char* key_end = strchr(key_start, '"');
        if (!key_end) continue;
        *key_end = '\0';
        
        char func_name[256];
        strncpy(func_name, key_start, sizeof(func_name) - 1);
        func_name[sizeof(func_name) - 1] = '\0';
        
        // Find array start
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
                // Store in hash table (simplified - no chaining shown but using open addressing)
                g_call_graph[h] = entry;
            }
        }
        
        // Parse callers from array
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

static int get_caller_count(const char* func_name) {
    if (!g_callgraph_loaded || !g_call_graph) return 0;
    uint64_t h = hash_callgraph(func_name);
    callgraph_entry_t* entry = g_call_graph[h];
    while (entry) {
        if (strcmp(entry->func_name, func_name) == 0) return entry->caller_count;
        entry = entry->next;
    }
    return 0;
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

// Load word frequencies for TF-IDF
static unsigned int hash_word_simple(const char* word) {
    unsigned int h = 5381;
    while (*word) h = ((h << 5) + h) + *word++;
    return h % WORD_HASH_SIZE;
}

static void load_word_freq(const char* cache_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/word_freq.json", cache_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) return;
    
    g_idf_table = calloc(WORD_HASH_SIZE, sizeof(idf_entry_t*));
    if (!g_idf_table) { fclose(fp); return; }
    
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char* key_start = strchr(line, '"');
        if (!key_start) continue;
        key_start++;
        char* key_end = strchr(key_start, '"');
        if (!key_end) continue;
        *key_end = '\0';
        
        if (key_start[0] == '_') continue; // Skip metadata
        
        char* idf_p = strstr(key_end + 1, "\"idf\":");
        if (!idf_p) continue;
        float idf = atof(idf_p + 6);
        
        unsigned int h = hash_word_simple(key_start);
        idf_entry_t* entry = malloc(sizeof(idf_entry_t));
        if (entry) {
            strncpy(entry->word, key_start, sizeof(entry->word) - 1);
            entry->word[sizeof(entry->word) - 1] = '\0';
            entry->idf = idf;
            entry->next = g_idf_table[h];
            g_idf_table[h] = entry;
        }
    }
    
    fclose(fp);
    g_idf_loaded = 1;
}

static float get_idf(const char* word) {
    if (!g_idf_loaded || !g_idf_table) return 1.0f;
    unsigned int h = hash_word_simple(word);
    idf_entry_t* entry = g_idf_table[h];
    while (entry) {
        if (strcmp(entry->word, word) == 0) return entry->idf;
        entry = entry->next;
    }
    return 1.0f; // Default IDF for unknown words
}

// Convert namespace to safe filename (replace / with _)
static void namespace_to_filename(const char* ns, char* out, size_t out_len) {
    int si = 0;
    for (int i = 0; ns[i] && si < (int)out_len - 1; i++) {
        if (ns[i] == '/') {
            if (si > 0 && out[si-1] != '_') {
                out[si++] = '_';
            }
        } else if (ns[i] == '-' || ns[i] == '.' || isalnum((unsigned char)ns[i])) {
            out[si++] = ns[i];
        } else {
            out[si++] = '_';
        }
    }
    out[si] = '\0';
}

// Discover available vector files in cache_dir/vectors/
static int discover_sources(const char* cache_dir, vector_source_t* sources, int max_sources) {
    char vec_dir[512];
    int n = snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    if (n < 0 || n >= (int)sizeof(vec_dir)) {
        fprintf(stderr, "Path too long: %s/vectors\n", cache_dir);
        return -1;
    }
    
    DIR* dir = opendir(vec_dir);
    if (!dir) {
        fprintf(stderr, "No vectors directory found: %s\n", vec_dir);
        fprintf(stderr, "Run vector_generator first.\n");
        return -1;
    }
    
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && count < max_sources) {
        size_t len = strlen(entry->d_name);
        if (len < 5 || strcmp(entry->d_name + len - 4, ".bin") != 0) continue;
        
        // Parse model suffix: name.mpnet.bin or name.jina.bin
        char base_name[256];
        strncpy(base_name, entry->d_name, len - 4);
        base_name[len - 4] = '\0';
        
        char* model_suffix = NULL;
        char* last_dot = strrchr(base_name, '.');
        if (last_dot) {
            if (strcmp(last_dot + 1, "mpnet") == 0 || 
                strcmp(last_dot + 1, "jina") == 0) {
                model_suffix = last_dot + 1;
                *last_dot = '\0';
            }
        }
        if (!model_suffix) {
            model_suffix = "mpnet"; // legacy fallback
        }
        
        char idx_file[1024];
        snprintf(idx_file, sizeof(idx_file), "%s/%s.%s.idx", vec_dir, base_name, model_suffix);
        
        struct stat st;
        if (stat(idx_file, &st) != 0) {
            // Try legacy .idx
            snprintf(idx_file, sizeof(idx_file), "%s/%s.idx", vec_dir, base_name);
            if (stat(idx_file, &st) != 0) continue;
        }
        
        char ns[256] = {0};
        strncpy(ns, base_name, sizeof(ns) - 1);
        
        if (strncmp(ns, "code_local_", 11) == 0) {
            snprintf(sources[count].namespace, sizeof(sources[count].namespace), 
                     "/code/local/%s", ns + 11);
        } else if (strncmp(ns, "code_", 5) == 0) {
            snprintf(sources[count].namespace, sizeof(sources[count].namespace), 
                     "/code/%s", ns + 5);
        } else {
            snprintf(sources[count].namespace, sizeof(sources[count].namespace), 
                     "/code/local/%s", ns);
        }
        
        snprintf(sources[count].vec_file, sizeof(sources[count].vec_file),
                 "%s/%s", vec_dir, entry->d_name);
        strncpy(sources[count].idx_file, idx_file, sizeof(sources[count].idx_file) - 1);
        sources[count].idx_file[sizeof(sources[count].idx_file) - 1] = '\0';
        strncpy(sources[count].model, model_suffix, sizeof(sources[count].model) - 1);
        sources[count].model[sizeof(sources[count].model) - 1] = '\0';
        
        // Check for HNSW index
        char hnsw_path[1024];
        snprintf(hnsw_path, sizeof(hnsw_path), "%s/%s.%s.bin.hnsw", vec_dir, base_name, model_suffix);
        struct stat hnsw_st;
        if (stat(hnsw_path, &hnsw_st) == 0) {
            strncpy(sources[count].hnsw_file, hnsw_path, sizeof(sources[count].hnsw_file) - 1);
            sources[count].hnsw_file[sizeof(sources[count].hnsw_file) - 1] = '\0';
        } else {
            sources[count].hnsw_file[0] = '\0';
        }
        
        FILE* fp = fopen(sources[count].vec_file, "rb");
        if (fp) {
            uint32_t c, d;
            if (fread(&c, 4, 1, fp) == 1 && fread(&d, 4, 1, fp) == 1) {
                sources[count].count = c;
            }
            fclose(fp);
        }
        
        count++;
    }
    closedir(dir);
    return count;
}

// Parse JSON index file
static int parse_index(const char* idx_file, char** names, size_t* offsets, int max_entries) {
    FILE* fp = fopen(idx_file, "r");
    if (!fp) return -1;
    
    int count = 0;
    char line[4096];
    
    size_t total_len = 0;
    char* json_buf = malloc(16 * 1024 * 1024);
    if (!json_buf) {
        fclose(fp);
        return -1;
    }
    
    while (fgets(line, sizeof(line), fp) && total_len < 16*1024*1024 - 1) {
        size_t len = strlen(line);
        memcpy(json_buf + total_len, line, len);
        total_len += len;
    }
    fclose(fp);
    json_buf[total_len] = '\0';
    
    char* p = json_buf;
    while (*p && count < max_entries) {
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++;
        
        int ni = 0;
        while (*p && *p != '"' && ni < 255) {
            if (*p == '\\' && *(p+1)) {
                p++;
                names[count][ni++] = *p++;
            } else {
                names[count][ni++] = *p++;
            }
        }
        names[count][ni] = '\0';
        
        if (*p != '"') continue;
        p++;
        
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;
        
        offsets[count] = strtoull(p, &p, 10);
        count++;
    }
    
    free(json_buf);
    return count;
}

// Parse simple JSON field
static int extract_json_field(const char* json, const char* field, char* out, int max_len) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char* p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p && isspace(*p)) p++;
    
    int i = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && i < max_len - 1) {
            if (*p == '\\' && *(p+1)) {
                p++;
                switch (*p) {
                    case 'n': out[i++] = '\n'; break;
                    case 't': out[i++] = '\t'; break;
                    case 'r': out[i++] = '\r'; break;
                    default: out[i++] = *p; break;
                }
            } else {
                out[i++] = *p;
            }
            p++;
        }
    } else {
        // Handle integer values (e.g., "line_start":8)
        while (*p && (isdigit((unsigned char)*p) || *p == '-') && i < max_len - 1) {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return i;
}

// Load chunks_meta.jsonl into memory hash table
static int load_meta_jsonl(const char* cache_dir) {
    if (g_meta_loaded) return 0;
    
    char meta_path[512];
    snprintf(meta_path, sizeof(meta_path), "%s/chunks_meta.jsonl", cache_dir);
    
    FILE* fp = fopen(meta_path, "r");
    if (!fp) {
        fprintf(stderr, "Warning: No chunks_meta.jsonl found at %s\n", meta_path);
        return -1;
    }
    
    // Allocate hash table
    g_meta_hash = calloc(META_HASH_SIZE, sizeof(meta_entry_t*));
    if (!g_meta_hash) {
        fclose(fp);
        return -1;
    }
    
    char line[65536];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        meta_entry_t* entry = calloc(1, sizeof(meta_entry_t));
        if (!entry) continue;
        
        extract_json_field(line, "name", entry->name, sizeof(entry->name));
        extract_json_field(line, "file", entry->file, sizeof(entry->file));
        extract_json_field(line, "kind", entry->kind, sizeof(entry->kind));
        extract_json_field(line, "language", entry->language, sizeof(entry->language));
        extract_json_field(line, "signature", entry->signature, sizeof(entry->signature));
        extract_json_field(line, "content", entry->content, sizeof(entry->content));
        
        char line_buf[32];
        if (extract_json_field(line, "line_start", line_buf, sizeof(line_buf)) > 0) {
            entry->line_start = atoi(line_buf);
        }
        if (extract_json_field(line, "line_end", line_buf, sizeof(line_buf)) > 0) {
            entry->line_end = atoi(line_buf);
        }
        
        if (entry->name[0]) {
            uint64_t h = hash_name(entry->name) % META_HASH_SIZE;
            entry->next = g_meta_hash[h];
            g_meta_hash[h] = entry;
            count++;
        } else {
            free(entry);
        }
    }
    
    fclose(fp);
    g_meta_loaded = 1;
    printf("Loaded %d metadata entries from %s\n", count, meta_path);
    return 0;
}

// Get rich details from memory hash table (O(1))
static int get_result_details(cache_t* cache, const char* namespace, const char* name,
                              result_detail_t* detail) {
    (void)cache; (void)namespace;  // unused
    memset(detail, 0, sizeof(result_detail_t));
    strncpy(detail->name, name, sizeof(detail->name) - 1);
    
    if (!g_meta_hash) return 0;
    
    uint64_t h = hash_name(name) % META_HASH_SIZE;
    meta_entry_t* entry = g_meta_hash[h];
    
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            strncpy(detail->file, entry->file, sizeof(detail->file) - 1);
            strncpy(detail->kind, entry->kind, sizeof(detail->kind) - 1);
            strncpy(detail->language, entry->language, sizeof(detail->language) - 1);
            strncpy(detail->signature, entry->signature, sizeof(detail->signature) - 1);
            strncpy(detail->content, entry->content, sizeof(detail->content) - 1);
            detail->line_start = entry->line_start;
            detail->line_end = entry->line_end;
            return 1;
        }
        entry = entry->next;
    }
    return 0;
}

static int compare_results(const void* a, const void* b) {
    float sa = ((search_result_t*)a)->score;
    float sb = ((search_result_t*)b)->score;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return 0;
}

// Check if query keywords appear in name for hybrid boost
static float keyword_boost(const char* query, const char* name) {
    char qcopy[256];
    strncpy(qcopy, query, sizeof(qcopy) - 1);
    qcopy[sizeof(qcopy) - 1] = '\0';
    
    float boost = 0.0f;
    char* token = strtok(qcopy, " _-.,;:!?()[]{}<>\"'\t\n");
    while (token) {
        if (strlen(token) >= 3) {
            // Check if token appears in name (case-insensitive)
            const char* p = name;
            size_t tlen = strlen(token);
            while (*p) {
                if (strncasecmp(p, token, tlen) == 0) {
                    // TF-IDF weighting: rare words get higher boost
                    float idf = get_idf(token);
                    // Normalize IDF to ~0.01-0.08 range (max IDF ~9 for rare words)
                    float weight = 0.01f + (idf / 9.0f) * 0.07f;
                    boost += weight;
                    break;
                }
                p++;
            }
        }
        token = strtok(NULL, " _-.,;:!?()[]{}<>\"'\t\n");
    }
    
    return boost > 0.3f ? 0.3f : boost;  // Cap at 0.3
}

static int search_source(const vector_source_t* source, const float* query_vec, 
                         float query_norm, int max_results, cache_t* cache,
                         const search_filter_t* filter, const char* query,
                         search_result_t** out_results, int* out_count) {
    // Try HNSW index first
    if (source->hnsw_file[0]) {
        FILE* hnsw_fp = fopen(source->hnsw_file, "rb");
        if (hnsw_fp) {
            fseek(hnsw_fp, 0, SEEK_END);
            long hnsw_size = ftell(hnsw_fp);
            fseek(hnsw_fp, 0, SEEK_SET);
            
            void* hnsw_buf = malloc(hnsw_size);
            if (hnsw_buf) {
                fread(hnsw_buf, 1, hnsw_size, hnsw_fp);
                fclose(hnsw_fp);
                
                hnsw_index_t* hnsw = hnsw_deserialize(hnsw_buf, hnsw_size);
                if (hnsw) {
                    size_t* ids = NULL;
                    float* scores = NULL;
                    int top_k = max_results * 4; // Get more for filtering
                    
                    size_t found = hnsw_search(hnsw, query_vec, top_k, &ids, &scores);
                    
                    // Load name index for mapping
                    FILE* fp = fopen(source->vec_file, "rb");
                    uint32_t count = 0, dim_check = 0;
                    if (fp) {
                        fread(&count, 4, 1, fp);
                        fread(&dim_check, 4, 1, fp); // dim
                        fclose(fp);
                    }
                    
                    char** names = malloc(count * sizeof(char*));
                    size_t* offsets = malloc(count * sizeof(size_t));
                    for (uint32_t i = 0; i < count; i++) names[i] = malloc(256);
                    parse_index(source->idx_file, names, offsets, count);
                    
                    search_result_t* results = malloc(found * sizeof(search_result_t));
                    int result_count = 0;
                    
                    for (size_t i = 0; i < found; i++) {
                        if (ids[i] >= count) continue;
                        
                        float sim = scores[i];
                        
                        // Hybrid keyword boost
                        if (query && query[0]) {
                            sim += keyword_boost(query, names[ids[i]]);
                            if (sim > 1.0f) sim = 1.0f;
                        }
                        
                        int pass = 1;
                        // Skip anonymous symbols
                        if (strncmp(names[ids[i]], "__anon", 6) == 0) pass = 0;
                        
                        if (pass && filter && (filter->kind_filter[0] || filter->lang_filter[0] || filter->file_filter[0])) {
                            result_detail_t detail;
                            if (get_result_details(cache, source->namespace, names[ids[i]], &detail)) {
                                if (filter->kind_filter[0] && strcasecmp(detail.kind, filter->kind_filter) != 0) pass = 0;
                                if (filter->lang_filter[0] && strcasecmp(detail.language, filter->lang_filter) != 0) pass = 0;
                                if (filter->file_filter[0] && !strstr(detail.file, filter->file_filter)) pass = 0;
                            } else {
                                pass = 0;
                            }
                        }
                        
                        if (pass) {
                            strncpy(results[result_count].name, names[ids[i]], 255);
                            results[result_count].name[255] = '\0';
                            results[result_count].score = sim;
                            result_count++;
                            if (result_count >= max_results) break;
                        }
                    }
                    
                    *out_results = malloc(result_count * sizeof(search_result_t));
                    if (*out_results) {
                        memcpy(*out_results, results, result_count * sizeof(search_result_t));
                        *out_count = result_count;
                    }
                    
                    free(results);
                    if (ids) free(ids);
                    if (scores) free(scores);
                    for (uint32_t i = 0; i < count; i++) free(names[i]);
                    free(names);
                    free(offsets);
                    hnsw_destroy(hnsw);
                    free(hnsw_buf);
                    
                    return 0;
                }
                free(hnsw_buf);
            } else {
                fclose(hnsw_fp);
            }
        }
    }
    
    // Fallback to brute force search
    FILE* fp = fopen(source->vec_file, "rb");
    if (!fp) return -1;
    
    uint32_t count, dim;
    if (fread(&count, 4, 1, fp) != 1 || fread(&dim, 4, 1, fp) != 1) {
        fclose(fp);
        return -1;
    }
    
    if (dim != DIM) {
        fprintf(stderr, "Dimension mismatch in %s: expected %d, got %u\n", 
                source->vec_file, DIM, dim);
        fclose(fp);
        return -1;
    }
    
    char** names = malloc(count * sizeof(char*));
    size_t* offsets = malloc(count * sizeof(size_t));
    if (!names || !offsets) {
        free(names); free(offsets);
        fclose(fp);
        return -1;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        names[i] = malloc(256);
        if (!names[i]) {
            for (uint32_t j = 0; j < i; j++) free(names[j]);
            free(names); free(offsets);
            fclose(fp);
            return -1;
        }
    }
    
    int parsed = parse_index(source->idx_file, names, offsets, count);
    if (parsed < 0) {
        for (uint32_t i = 0; i < count; i++) free(names[i]);
        free(names); free(offsets);
        fclose(fp);
        return -1;
    }
    
    search_result_t* results = malloc(parsed * sizeof(search_result_t));
    float* vec = malloc(DIM * sizeof(float));
    if (!results || !vec) {
        free(results); free(vec);
        for (uint32_t i = 0; i < count; i++) free(names[i]);
        free(names); free(offsets);
        fclose(fp);
        return -1;
    }
    
    for (int i = 0; i < parsed; i++) {
        if (fseek(fp, offsets[i], SEEK_SET) != 0) continue;
        if (fread(vec, sizeof(float), DIM, fp) != DIM) continue;
        
        float dot = 0.0f;
        float norm_v = 0.0f;
        for (int j = 0; j < DIM; j++) {
            dot += query_vec[j] * vec[j];
            norm_v += vec[j] * vec[j];
        }
        norm_v = sqrtf(norm_v);
        
        float sim = 0.0f;
        if (query_norm > 0 && norm_v > 0) {
            sim = dot / (query_norm * norm_v);
        }
        
        // Hybrid keyword boost with TF-IDF
        if (query && query[0]) {
            sim += keyword_boost(query, names[i]);
            if (sim > 1.0f) sim = 1.0f;
        }
        
        // PageRank-like weighting: functions called by many others get boost
        int caller_count = get_caller_count(names[i]);
        if (caller_count > 0) {
            float pagerank_boost = logf(caller_count + 1) * 0.02f; // Max ~0.1 for 100+ callers
            sim += pagerank_boost;
            if (sim > 1.0f) sim = 1.0f;
        }
        
        strncpy(results[i].name, names[i], 255);
        results[i].name[255] = '\0';
        results[i].score = sim;
    }
    
    free(vec);
    fclose(fp);
    for (uint32_t i = 0; i < count; i++) free(names[i]);
    free(names);
    free(offsets);
    
    qsort(results, parsed, sizeof(search_result_t), compare_results);
    
    // Apply filters
    int filtered_count = 0;
    search_result_t* filtered = malloc(parsed * sizeof(search_result_t));
    
    for (int i = 0; i < parsed && filtered_count < max_results; i++) {
        int pass = 1;
        
        // Skip anonymous symbols (ctags-generated __anon*)
        if (strncmp(results[i].name, "__anon", 6) == 0) {
            pass = 0;
        }
        
        if (filter && (filter->kind_filter[0] || filter->lang_filter[0] || filter->file_filter[0])) {
            result_detail_t detail;
            if (get_result_details(cache, source->namespace, results[i].name, &detail)) {
                if (filter->kind_filter[0] && strcasecmp(detail.kind, filter->kind_filter) != 0) {
                    pass = 0;
                }
                if (filter->lang_filter[0] && strcasecmp(detail.language, filter->lang_filter) != 0) {
                    pass = 0;
                }
                if (filter->file_filter[0] && !strstr(detail.file, filter->file_filter)) {
                    pass = 0;
                }
            } else {
                pass = 0; // Can't verify, skip
            }
        }
        
        if (pass) {
            memcpy(&filtered[filtered_count++], &results[i], sizeof(search_result_t));
        }
    }
    
    *out_results = malloc(filtered_count * sizeof(search_result_t));
    if (*out_results) {
        memcpy(*out_results, filtered, filtered_count * sizeof(search_result_t));
        *out_count = filtered_count;
    }
    
    free(filtered);
    free(results);
    
    return 0;
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

int main(int argc, char** argv) {
    const char* cache_dir = NULL;
    const char* query = NULL;
    int max_results = MAX_RESULTS;
    const char* target_ns = NULL;
    
    const char* model_type = "mpnet";
    search_filter_t filter = {0};
    int use_filter = 0;
    
    // Parse arguments - first pass: collect all options
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
                // Read code snippet from file
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
                strncpy(filter.kind_filter, argv[arg_idx + 1], sizeof(filter.kind_filter) - 1);
                use_filter = 1;
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--lang") == 0 && arg_idx + 1 < argc) {
                strncpy(filter.lang_filter, argv[arg_idx + 1], sizeof(filter.lang_filter) - 1);
                use_filter = 1;
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--file") == 0 && arg_idx + 1 < argc) {
                strncpy(filter.file_filter, argv[arg_idx + 1], sizeof(filter.file_filter) - 1);
                use_filter = 1;
                arg_idx += 2;
            } else if (strcmp(argv[arg_idx], "--help") == 0 || strcmp(argv[arg_idx], "-h") == 0) {
                printf("AI Agent Semantic Code Search\n");
                printf("Usage: %s [options] <cache_dir> <query> [max_results] [namespace]\n", argv[0]);
                printf("\nOptions:\n");
                printf("  --json       Output results as JSON\n");
                printf("  --rich       Include full code context (file, line, signature, content)\n");
                printf("  --callgraph  Show function call relationships\n");
                printf("  --snippet    Search by code snippet file instead of text query\n");
                printf("  --model      Model type: mpnet (default) or jina\n");
                printf("  --kind       Filter by symbol kind: function, struct, class, macro, typedef\n");
                printf("  --lang       Filter by language: cpp, c, python, javascript, go, rust, java\n");
                printf("  --file       Filter by filename pattern (substring match)\n");
                printf("  -h, --help   Show this help\n");
                printf("\nExamples:\n");
                printf("  %s ./ai_code_memory \"generate image\" 5\n", argv[0]);
                printf("  %s --model jina ./ai_code_memory \"upscale image\" 5\n", argv[0]);
                printf("  %s --kind function --lang cpp ./ai_code_memory \"memory allocation\" 5\n", argv[0]);
                printf("  %s --file cuda ./ai_code_memory \"gpu kernel\" 5\n", argv[0]);
                printf("  %s --json --rich ./ai_code_memory \"memory allocation\" 5 /code/local/project\n", argv[0]);
                return 0;
            } else {
                fprintf(stderr, "Unknown option: %s\n", argv[arg_idx]);
                return 1;
            }
        } else {
            // First positional argument is cache_dir
            if (!cache_dir) {
                cache_dir = argv[arg_idx];
            } else if (!query) {
                query = argv[arg_idx];
            } else if (max_results == MAX_RESULTS) {
                max_results = atoi(argv[arg_idx]);
                if (max_results <= 0) max_results = MAX_RESULTS;
            } else {
                target_ns = argv[arg_idx];
            }
            arg_idx++;
        }
    }
    
    if (!cache_dir) {
        fprintf(stderr, "Error: cache_dir required\n");
        fprintf(stderr, "Usage: %s [options] <cache_dir> <query> [max_results] [namespace]\n", argv[0]);
        return 1;
    }
    
    if (!query) {
        fprintf(stderr, "Error: query required\n");
        return 1;
    }
    
    if (max_results <= 0) max_results = MAX_RESULTS;
    if (max_results > 10000) max_results = 10000;
    
    if (strlen(query) > MAX_QUERY_LEN) {
        fprintf(stderr, "Error: query too long (max %d chars)\n", MAX_QUERY_LEN);
        return 1;
    }
    
    // Discover vector sources
    vector_source_t sources[MAX_NAMESPACES];
    int num_sources = discover_sources(cache_dir, sources, MAX_NAMESPACES);
    if (num_sources < 0) return 1;
    if (num_sources == 0) {
        fprintf(stderr, "No vector files found in %s/vectors/\n", cache_dir);
        return 1;
    }
    
    // Filter by namespace and model
    int active_sources[MAX_NAMESPACES];
    int num_active = 0;
    for (int i = 0; i < num_sources; i++) {
        int ns_match = !target_ns || strcmp(sources[i].namespace, target_ns) == 0 ||
                       strstr(sources[i].namespace, target_ns);
        int model_match = strcmp(sources[i].model, model_type) == 0;
        if (ns_match && model_match) {
            active_sources[num_active++] = i;
        }
    }
    if (num_active == 0) {
        if (target_ns) {
            fprintf(stderr, "No matching namespace found for: %s (model: %s)\n", target_ns, model_type);
        } else {
            fprintf(stderr, "No vector files found for model: %s\n", model_type);
        }
        fprintf(stderr, "Available sources:\n");
        for (int i = 0; i < num_sources; i++) {
            fprintf(stderr, "  %s (%s, %u vectors)\n", sources[i].namespace, sources[i].model, sources[i].count);
        }
        return 1;
    }
    
    // Load metadata from JSONL (fast O(1) lookup, no cache needed)
    int meta_ok = (load_meta_jsonl(cache_dir) == 0);
    if (!meta_ok && (output_rich || use_filter)) {
        fprintf(stderr, "Warning: No chunks_meta.jsonl found, rich mode and filtering disabled\n");
        output_rich = 0;
        use_filter = 0;
    }
    
    // Load call graph if available
    if (show_callgraph || output_rich) {
        load_call_graph(cache_dir);
    }
    
    // Load word frequencies for TF-IDF
    load_word_freq(cache_dir);
    
    // Open cache only as fallback (optional)
    cache_t* cache = NULL;
    
    // Load embedder based on model type
    float query_vec[DIM];
    
    if (strcmp(model_type, "jina") == 0) {
        // Jina: use C embedder with BPE tokenizer
        if (!output_json) {
            printf("Loading Jina embedder...\n");
        }
        onnx_embedder_t* embedder = onnx_embedder_init(
            "models/jina-embeddings-v2-base-code/model.onnx",
            "models/jina-embeddings-v2-base-code/vocab.json",
            512, DIM
        );
        if (!embedder) {
            fprintf(stderr, "Failed to load Jina embedder: %s\n", onnx_embedder_error());
            if (cache) cache_close(cache);
            return 1;
        }
        
        if (onnx_embedder_encode(embedder, query, query_vec) != 0) {
            fprintf(stderr, "Failed to encode query with Jina\n");
            onnx_embedder_free(embedder);
            if (cache) cache_close(cache);
            return 1;
        }
        onnx_embedder_free(embedder);
    } else {
        // MPNet: use C embedder (default)
        if (!output_json) {
            printf("Loading MPNet embedder...\n");
        }
        onnx_embedder_t* embedder = onnx_embedder_init(
            "models/all-mpnet-base-v2/model.onnx",
            "models/all-mpnet-base-v2/vocab.txt",
            128, DIM
        );
        if (!embedder) {
            fprintf(stderr, "Failed to load embedder: %s\n", onnx_embedder_error());
            if (cache) cache_close(cache);
            return 1;
        }
        
        if (onnx_embedder_encode(embedder, query, query_vec) != 0) {
            fprintf(stderr, "Failed to encode query\n");
            onnx_embedder_free(embedder);
            if (cache) cache_close(cache);
            return 1;
        }
        onnx_embedder_free(embedder);
    }
    
    float query_norm = 0.0f;
    for (int i = 0; i < DIM; i++) {
        query_norm += query_vec[i] * query_vec[i];
    }
    query_norm = sqrtf(query_norm);
    
    // Search all active sources
    search_result_t* all_results = NULL;
    int total_results = 0;
    
    for (int s = 0; s < num_active; s++) {
        int src_idx = active_sources[s];
        search_result_t* src_results = NULL;
        int src_count = 0;
        if (search_source(&sources[src_idx], query_vec, query_norm, 
                          max_results * 2, cache, use_filter ? &filter : NULL, query,
                          &src_results, &src_count) == 0) {
            search_result_t* new_all = realloc(all_results, 
                (total_results + src_count) * sizeof(search_result_t));
            if (new_all) {
                all_results = new_all;
                memcpy(all_results + total_results, src_results, 
                       src_count * sizeof(search_result_t));
                total_results += src_count;
            }
            free(src_results);
        }
    }
    
    if (total_results == 0) {
        if (output_json) {
            printf("{\"query\":\"%s\",\"results\":[],\"total\":0}\n", query);
        } else {
            printf("No results found.\n");
        }
        free(all_results);
        if (cache) cache_close(cache);
        return 0;
    }
    
    qsort(all_results, total_results, sizeof(search_result_t), compare_results);
    
    int show = (max_results < total_results) ? max_results : total_results;
    
    if (output_json) {
        // JSON output
        char escaped_query[2048];
        json_escape(query, escaped_query, sizeof(escaped_query));
        
        printf("{\n");
        printf("  \"query\": \"%s\",\n", escaped_query);
        printf("  \"total_vectors_searched\": %u,\n", 
               sources[active_sources[0]].count);  // simplified
        printf("  \"results_count\": %d,\n", show);
        printf("  \"results\": [\n");
        
        for (int i = 0; i < show; i++) {
            char escaped_name[512];
            json_escape(all_results[i].name, escaped_name, sizeof(escaped_name));
            
            printf("    {\n");
            printf("      \"name\": \"%s\",\n", escaped_name);
            printf("      \"score\": %.4f", all_results[i].score);
            
                if (output_rich && (cache || g_meta_hash)) {
                result_detail_t detail;
                if (get_result_details(cache, sources[active_sources[0]].namespace, 
                                       all_results[i].name, &detail)) {
                    char escaped_file[1024], escaped_sig[1024], escaped_doc[2048], escaped_content[8192];
                    json_escape(detail.file, escaped_file, sizeof(escaped_file));
                    json_escape(detail.signature, escaped_sig, sizeof(escaped_sig));
                    json_escape(detail.docstring, escaped_doc, sizeof(escaped_doc));
                    json_escape(detail.content, escaped_content, sizeof(escaped_content));
                    
                    printf(",\n");
                    printf("      \"file\": \"%s\",\n", escaped_file);
                    printf("      \"line_start\": %d,\n", detail.line_start);
                    printf("      \"line_end\": %d,\n", detail.line_end);
                    printf("      \"language\": \"%s\",\n", detail.language);
                    printf("      \"signature\": \"%s\",\n", escaped_sig);
                    printf("      \"docstring\": \"%s\",\n", escaped_doc);
                    printf("      \"content\": \"%s\"", escaped_content);
                }
            }
            
            printf("\n    }%s\n", (i < show - 1) ? "," : "");
        }
        
        printf("  ]\n");
        printf("}\n");
    } else {
        // Human-readable output
        if (output_rich) {
            printf("\nTop %d results:\n\n", show);
            for (int i = 0; i < show; i++) {
                printf("─");
                for (int k = 0; k < 69; k++) printf("─");
                printf("\n");
                
                printf("[%d] %s (%.4f)\n", i + 1, all_results[i].name, all_results[i].score);
                
            if (output_rich && (cache || g_meta_hash)) {
                    result_detail_t detail;
                    if (get_result_details(cache, sources[active_sources[0]].namespace,
                                           all_results[i].name, &detail)) {
                        if (detail.signature[0]) {
                            printf("    Signature: %s\n", detail.signature);
                        }
                        if (detail.file[0]) {
                            printf("    Location:  %s:%d\n", detail.file, detail.line_start);
                        }
                        if (detail.language[0]) {
                            printf("    Language:  %s\n", detail.language);
                        }
                        if (detail.docstring[0]) {
                            printf("    Doc:\n      %s\n", detail.docstring);
                        }
                        if (detail.content[0]) {
                            printf("    Code:\n");
                            char* p = detail.content;
                            int line_no = 0;
                            while (*p && line_no < 20) {
                                printf("      %c ", '|');
                                while (*p && *p != '\n') {
                                    putchar(*p++);
                                }
                                printf("\n");
                                if (*p == '\n') p++;
                                line_no++;
                            }
                            if (*p) printf("      ...\n");
                        }
                        if (show_callgraph || output_rich) {
                            print_callers(all_results[i].name);
                        }
                    }
                }
                printf("\n");
            }
        } else {
            printf("\nTop %d results:\n", show);
            printf("%-50s %s\n", "Name", "Score");
            printf("%-50s %s\n", "----", "-----");
            for (int i = 0; i < show; i++) {
                printf("%-50s %.4f\n", all_results[i].name, all_results[i].score);
            }
        }
    }
    
    free(all_results);
    if (cache) cache_close(cache);
    return 0;
}
