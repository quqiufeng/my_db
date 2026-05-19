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
// Returns number of sources found, or -1 on error
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
        
        // Check corresponding .idx file exists
        char idx_file[1024];
        snprintf(idx_file, sizeof(idx_file), "%s/%s", vec_dir, entry->d_name);
        idx_file[strlen(idx_file) - 4] = '\0';  // remove .bin
        strncat(idx_file, ".idx", sizeof(idx_file) - strlen(idx_file) - 1);
        
        struct stat st;
        if (stat(idx_file, &st) != 0) continue;
        
        // Extract namespace from filename: code_local_repo-name.bin -> /code/local/repo-name
        char ns[256] = {0};
        strncpy(ns, entry->d_name, len - 4);  // remove .bin
        ns[len - 4] = '\0';
        
        // Try to reconstruct namespace (best effort)
        // Pattern: code_local_XXX -> /code/local/XXX
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
        
        // Read count from header
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

// Simple JSON index parser: extracts name->offset pairs
static int parse_index(const char* idx_file, char** names, size_t* offsets, int max_entries) {
    FILE* fp = fopen(idx_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open index: %s\n", idx_file);
        return -1;
    }
    
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
    if (!fp) {
        fprintf(stderr, "Cannot open: %s\n", source->vec_file);
        return -1;
    }
    
    uint32_t count, dim;
    if (fread(&count, 4, 1, fp) != 1 || fread(&dim, 4, 1, fp) != 1) {
        fprintf(stderr, "Failed to read header: %s\n", source->vec_file);
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

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("AI Agent Semantic Code Search\n");
        printf("Usage: %s <cache_dir> <query> [max_results] [namespace]\n", argv[0]);
        printf("\nExamples:\n");
        printf("  %s ./ai_code_memory \"generate image\" 5\n", argv[0]);
        printf("  %s ./ai_code_memory \"CUDA kernel\" 10 /code/local/my-project\n", argv[0]);
        printf("\nIf namespace is omitted, searches all available projects.\n");
        return 1;
    }
    
    const char* cache_dir = argv[1];
    const char* query = argv[2];
    int max_results = (argc > 3) ? atoi(argv[3]) : MAX_RESULTS;
    const char* target_ns = (argc > 4) ? argv[4] : NULL;
    
    if (max_results <= 0) max_results = MAX_RESULTS;
    if (max_results > 10000) {
        fprintf(stderr, "Warning: max_results capped at 10000\n");
        max_results = 10000;
    }
    
    if (strlen(query) > MAX_QUERY_LEN) {
        fprintf(stderr, "Error: query too long (max %d chars)\n", MAX_QUERY_LEN);
        return 1;
    }
    
    printf("Cache: %s\n", cache_dir);
    printf("Query: \"%s\"\n", query);
    if (target_ns) printf("Namespace: %s\n", target_ns);
    printf("Max results: %d\n\n", max_results);
    
    // Discover vector sources
    vector_source_t sources[MAX_NAMESPACES];
    int num_sources = discover_sources(cache_dir, sources, MAX_NAMESPACES);
    if (num_sources < 0) {
        return 1;
    }
    if (num_sources == 0) {
        fprintf(stderr, "No vector files found in %s/vectors/\n", cache_dir);
        fprintf(stderr, "Run vector_generator first.\n");
        return 1;
    }
    
    printf("Found %d vector source(s):\n", num_sources);
    for (int i = 0; i < num_sources; i++) {
        printf("  [%d] %s (%u vectors)\n", i + 1, sources[i].namespace, sources[i].count);
    }
    printf("\n");
    
    // Filter by namespace if specified
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
    
    // Load embedder
    printf("Loading embedder...\n");
    onnx_embedder_t* embedder = onnx_embedder_init(
        "models/all-mpnet-base-v2/model.onnx",
        "models/all-mpnet-base-v2/vocab.txt",
        128, DIM
    );
    if (!embedder) {
        fprintf(stderr, "Failed to load embedder: %s\n", onnx_embedder_error());
        return 1;
    }
    
    // Encode query
    printf("Encoding query...\n");
    float query_vec[DIM];
    if (onnx_embedder_encode(embedder, query, query_vec) != 0) {
        fprintf(stderr, "Failed to encode query\n");
        onnx_embedder_free(embedder);
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
        printf("Searching %s...\n", sources[src_idx].namespace);
        
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
        printf("No results found.\n");
        free(all_results);
        return 0;
    }
    
    // Global sort
    qsort(all_results, total_results, sizeof(search_result_t), compare_results);
    
    int show = (max_results < total_results) ? max_results : total_results;
    printf("\nTop %d results across %d source(s):\n", show, num_active);
    printf("%-50s %s\n", "Name", "Score");
    printf("%-50s %s\n", "----", "-----");
    
    for (int i = 0; i < show; i++) {
        printf("%-50s %.4f\n", all_results[i].name, all_results[i].score);
    }
    
    free(all_results);
    return 0;
}
