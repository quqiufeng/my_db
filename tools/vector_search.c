#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cache.h"
#include "onnx_embedder.h"

#define DIM 768
#define MAX_RESULTS 10
#define MAX_QUERY_LEN 1024

typedef struct {
    char name[256];
    float score;
} search_result_t;

// Simple JSON index parser: extracts name->offset pairs
// Returns number of entries parsed, -1 on error
static int parse_index(const char* idx_file, char** names, size_t* offsets, int max_entries) {
    FILE* fp = fopen(idx_file, "r");
    if (!fp) {
        fprintf(stderr, "Failed to open index: %s\n", idx_file);
        return -1;
    }
    
    int count = 0;
    char line[4096];
    
    // Read entire file (it's one JSON object on one or multiple lines)
    size_t total_len = 0;
    char* json_buf = malloc(16 * 1024 * 1024);  // 16MB buffer
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
    
    // Simple parser: find "name":offset pairs
    char* p = json_buf;
    while (*p && count < max_entries) {
        // Skip to next quote
        while (*p && *p != '"') p++;
        if (!*p) break;
        p++;  // skip opening quote
        
        // Read name until closing quote
        int ni = 0;
        while (*p && *p != '"' && ni < 255) {
            if (*p == '\\' && *(p+1)) {
                p++;  // skip backslash
                names[count][ni++] = *p++;  // add escaped char
            } else {
                names[count][ni++] = *p++;
            }
        }
        names[count][ni] = '\0';
        
        if (*p != '"') continue;  // malformed, skip
        p++;  // skip closing quote
        
        // Skip to colon
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;  // skip colon
        
        // Read offset number
        offsets[count] = strtoull(p, &p, 10);
        count++;
    }
    
    free(json_buf);
    return count;
}

// Comparison function for qsort (descending by score)
static int compare_results(const void* a, const void* b) {
    float sa = ((search_result_t*)a)->score;
    float sb = ((search_result_t*)b)->score;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: %s <cache_dir> <query> [max_results]\n", argv[0]);
        printf("Example: %s ./ai_code_memory \"generate image from text\" 5\n", argv[0]);
        return 1;
    }
    
    const char* cache_dir = argv[1];
    const char* query = argv[2];
    int max_results = (argc > 3) ? atoi(argv[3]) : MAX_RESULTS;
    if (max_results <= 0) max_results = MAX_RESULTS;
    
    printf("Cache: %s\n", cache_dir);
    printf("Query: \"%s\"\n", query);
    printf("Max results: %d\n\n", max_results);
    
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
    
    // Compute query norm
    float query_norm = 0.0f;
    for (int i = 0; i < DIM; i++) {
        query_norm += query_vec[i] * query_vec[i];
    }
    query_norm = sqrtf(query_norm);
    
    // Load vector binary file
    char vec_file[512], idx_file[512];
    snprintf(vec_file, sizeof(vec_file), "%s/vectors/code_local_stable-diffusion.cpp.bin", cache_dir);
    snprintf(idx_file, sizeof(idx_file), "%s/vectors/code_local_stable-diffusion.cpp.idx", cache_dir);
    
    FILE* fp = fopen(vec_file, "rb");
    if (!fp) {
        fprintf(stderr, "Vector file not found: %s\n", vec_file);
        fprintf(stderr, "Run vector generator first.\n");
        onnx_embedder_free(embedder);
        return 1;
    }
    
    uint32_t count, dim;
    fread(&count, 4, 1, fp);
    fread(&dim, 4, 1, fp);
    printf("Vectors: %u, Dim: %u\n", count, dim);
    
    if (dim != DIM) {
        fprintf(stderr, "Dimension mismatch: expected %d, got %u\n", DIM, dim);
        fclose(fp);
        onnx_embedder_free(embedder);
        return 1;
    }
    
    // Parse index
    printf("Loading index...\n");
    char** names = malloc(count * sizeof(char*));
    size_t* offsets = malloc(count * sizeof(size_t));
    for (int i = 0; i < count; i++) {
        names[i] = malloc(256);
    }
    
    int parsed = parse_index(idx_file, names, offsets, count);
    if (parsed < 0) {
        fprintf(stderr, "Failed to parse index\n");
        fclose(fp);
        onnx_embedder_free(embedder);
        return 1;
    }
    printf("Indexed entries: %d\n\n", parsed);
    
    // Search: compute similarity for all vectors
    printf("Searching...\n");
    search_result_t* results = malloc(parsed * sizeof(search_result_t));
    
    float* vec = malloc(DIM * sizeof(float));
    for (int i = 0; i < parsed; i++) {
        fseek(fp, offsets[i], SEEK_SET);
        fread(vec, sizeof(float), DIM, fp);
        
        // Cosine similarity
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
    
    // Sort by score descending
    qsort(results, parsed, sizeof(search_result_t), compare_results);
    
    // Output top results
    printf("\nTop %d results:\n", max_results < parsed ? max_results : parsed);
    printf("%-50s %s\n", "Name", "Score");
    printf("%-50s %s\n", "----", "-----");
    
    int show = (max_results < parsed) ? max_results : parsed;
    for (int i = 0; i < show; i++) {
        printf("%-50s %.4f\n", results[i].name, results[i].score);
    }
    
    // Cleanup
    for (int i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
    free(offsets);
    free(results);
    onnx_embedder_free(embedder);
    
    return 0;
}
