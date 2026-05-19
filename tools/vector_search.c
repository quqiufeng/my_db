#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include "cache.h"
#include "onnx_embedder.h"

#define DIM 768
#define MAX_RESULTS 10
#define MAX_QUERY_LEN 1024
#define MAX_NAMESPACES 32
#define MAX_RESULT_DETAILS 100

typedef struct {
    char name[256];
    float score;
} search_result_t;

typedef struct {
    char namespace[256];
    char vec_file[512];
    char idx_file[512];
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
} result_detail_t;

static int output_json = 0;
static int output_rich = 0;

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
        
        char idx_file[1024];
        snprintf(idx_file, sizeof(idx_file), "%s/%s", vec_dir, entry->d_name);
        idx_file[strlen(idx_file) - 4] = '\0';
        strncat(idx_file, ".idx", sizeof(idx_file) - strlen(idx_file) - 1);
        
        struct stat st;
        if (stat(idx_file, &st) != 0) continue;
        
        char ns[256] = {0};
        strncpy(ns, entry->d_name, len - 4);
        
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
    }
    out[i] = '\0';
    return i;
}

// Get rich details from cache
static int get_result_details(cache_t* cache, const char* namespace, const char* name,
                              result_detail_t* detail) {
    memset(detail, 0, sizeof(result_detail_t));
    strncpy(detail->name, name, sizeof(detail->name) - 1);
    
    // Try to find chunk by searching all file subdirectories
    // Format: {namespace}/chunks/{file}/{name}
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    
    char chunk_prefix[512];
    snprintf(chunk_prefix, sizeof(chunk_prefix), "%s/chunks/", namespace);
    size_t prefix_len = strlen(chunk_prefix);
    
    int found = 0;
    while (cache_iter_next(iter, &key, &value) == 1) {
        if (strncmp(key, chunk_prefix, prefix_len) != 0) continue;
        
        const char* last_slash = strrchr(key, '/');
        if (last_slash && strcmp(last_slash + 1, name) == 0) {
            extract_json_field(value, "file", detail->file, sizeof(detail->file));
            extract_json_field(value, "signature", detail->signature, sizeof(detail->signature));
            extract_json_field(value, "docstring", detail->docstring, sizeof(detail->docstring));
            extract_json_field(value, "content", detail->content, sizeof(detail->content));
            extract_json_field(value, "language", detail->language, sizeof(detail->language));
            
            char line_buf[32];
            if (extract_json_field(value, "line_start", line_buf, sizeof(line_buf)) > 0) {
                detail->line_start = atoi(line_buf);
            }
            if (extract_json_field(value, "line_end", line_buf, sizeof(line_buf)) > 0) {
                detail->line_end = atoi(line_buf);
            }
            found = 1;
            break;
        }
    }
    cache_iter_destroy(iter);
    return found;
}

static int compare_results(const void* a, const void* b) {
    float sa = ((search_result_t*)a)->score;
    float sb = ((search_result_t*)b)->score;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return 0;
}

static int search_source(const vector_source_t* source, const float* query_vec, 
                         float query_norm, int max_results, 
                         search_result_t** out_results, int* out_count) {
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
    
    int show = (max_results < parsed) ? max_results : parsed;
    *out_results = malloc(show * sizeof(search_result_t));
    if (*out_results) {
        memcpy(*out_results, results, show * sizeof(search_result_t));
        *out_count = show;
    }
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
    
    // Parse arguments
    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "--json") == 0) {
            output_json = 1;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "--rich") == 0) {
            output_rich = 1;
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "--help") == 0 || strcmp(argv[arg_idx], "-h") == 0) {
            printf("AI Agent Semantic Code Search\n");
            printf("Usage: %s [options] <cache_dir> <query> [max_results] [namespace]\n", argv[0]);
            printf("\nOptions:\n");
            printf("  --json       Output results as JSON\n");
            printf("  --rich       Include full code context (file, line, signature, content)\n");
            printf("  -h, --help   Show this help\n");
            printf("\nExamples:\n");
            printf("  %s ./ai_code_memory \"generate image\" 5\n", argv[0]);
            printf("  %s --rich ./ai_code_memory \"CUDA kernel\" 10\n", argv[0]);
            printf("  %s --json --rich ./ai_code_memory \"memory allocation\" 5 /code/local/project\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[arg_idx]);
            return 1;
        }
    }
    
    if (arg_idx >= argc) {
        fprintf(stderr, "Error: cache_dir required\n");
        fprintf(stderr, "Usage: %s [options] <cache_dir> <query> [max_results] [namespace]\n", argv[0]);
        return 1;
    }
    cache_dir = argv[arg_idx++];
    
    if (arg_idx >= argc) {
        fprintf(stderr, "Error: query required\n");
        return 1;
    }
    query = argv[arg_idx++];
    
    if (arg_idx < argc) {
        max_results = atoi(argv[arg_idx]);
        arg_idx++;
    }
    if (arg_idx < argc) {
        target_ns = argv[arg_idx];
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
    
    // Filter by namespace
    int active_sources[MAX_NAMESPACES];
    int num_active = 0;
    for (int i = 0; i < num_sources; i++) {
        if (!target_ns || strcmp(sources[i].namespace, target_ns) == 0 ||
            strstr(sources[i].namespace, target_ns)) {
            active_sources[num_active++] = i;
        }
    }
    if (num_active == 0) {
        fprintf(stderr, "No matching namespace found for: %s\n", target_ns);
        return 1;
    }
    
    // Open cache for rich mode
    cache_t* cache = NULL;
    if (output_rich) {
        cache = cache_open(cache_dir, 1024 * 1024 * 1024);
        if (!cache) {
            fprintf(stderr, "Warning: Failed to open cache, rich mode disabled\n");
            output_rich = 0;
        }
    }
    
    // Load embedder
    if (!output_json) {
        printf("Loading embedder...\n");
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
    
    float query_vec[DIM];
    if (onnx_embedder_encode(embedder, query, query_vec) != 0) {
        fprintf(stderr, "Failed to encode query\n");
        onnx_embedder_free(embedder);
        if (cache) cache_close(cache);
        return 1;
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
                          max_results * 2, &src_results, &src_count) == 0) {
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
    
    onnx_embedder_free(embedder);
    
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
            
            if (output_rich && cache) {
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
                
                if (output_rich && cache) {
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
