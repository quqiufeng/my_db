#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cache_hnsw.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Build HNSW index from vector binary file\n");
        printf("Usage: %s <vec_file>\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s ./ai_code_memory/vectors/code_local_llama.cpp.jina.bin\n", argv[0]);
        return 1;
    }
    
    const char* vec_file = argv[1];
    
    // Read vectors
    FILE* fp = fopen(vec_file, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open %s\n", vec_file);
        return 1;
    }
    
    uint32_t count, dim;
    if (fread(&count, 4, 1, fp) != 1 || fread(&dim, 4, 1, fp) != 1) {
        fprintf(stderr, "Failed to read header\n");
        fclose(fp);
        return 1;
    }
    
    printf("Building HNSW index: %u vectors, %u dims\n", count, dim);
    
    hnsw_index_t* hnsw = hnsw_create(dim);
    if (!hnsw) {
        fprintf(stderr, "Failed to create HNSW index\n");
        fclose(fp);
        return 1;
    }
    
    // Configure for code search (smaller M for faster build, larger ef for accuracy)
    hnsw_set_m(hnsw, 16);
    hnsw_set_ef_construction(hnsw, 200);
    hnsw_set_ef_search(hnsw, 64);
    
    float* vec = malloc(dim * sizeof(float));
    if (!vec) {
        fprintf(stderr, "Failed to allocate vector buffer\n");
        hnsw_destroy(hnsw);
        fclose(fp);
        return 1;
    }
    
    printf("Inserting vectors...\n");
    for (uint32_t i = 0; i < count; i++) {
        if (fread(vec, sizeof(float), dim, fp) != dim) {
            fprintf(stderr, "Failed to read vector %u\n", i);
            break;
        }
        
        if (hnsw_insert(hnsw, i, vec) != 0) {
            fprintf(stderr, "Failed to insert vector %u\n", i);
        }
        
        if ((i + 1) % 1000 == 0 || i == count - 1) {
            printf("  Inserted %u/%u\n", i + 1, count);
        }
    }
    
    free(vec);
    fclose(fp);
    
    printf("Index built: %zu vectors\n", hnsw_count(hnsw));
    printf("Memory usage: %.2f MB\n", hnsw_memory_usage(hnsw) / (1024.0 * 1024.0));
    
    // Save index
    char hnsw_file[1024];
    snprintf(hnsw_file, sizeof(hnsw_file), "%s.hnsw", vec_file);
    
    size_t size = hnsw_serialize_size(hnsw);
    void* buf = malloc(size);
    if (!buf) {
        fprintf(stderr, "Failed to allocate serialize buffer\n");
        hnsw_destroy(hnsw);
        return 1;
    }
    
    size_t written = hnsw_serialize(hnsw, buf, size);
    if (written == 0) {
        fprintf(stderr, "Failed to serialize index\n");
        free(buf);
        hnsw_destroy(hnsw);
        return 1;
    }
    
    FILE* out = fopen(hnsw_file, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create %s\n", hnsw_file);
        free(buf);
        hnsw_destroy(hnsw);
        return 1;
    }
    
    fwrite(buf, 1, written, out);
    fclose(out);
    
    printf("Saved HNSW index: %s (%.2f MB)\n", hnsw_file, written / (1024.0 * 1024.0));
    
    free(buf);
    hnsw_destroy(hnsw);
    
    return 0;
}
