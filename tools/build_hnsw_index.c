#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "cache_hnsw.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Build HNSW index from vector binary file\n");
        printf("Usage: %s <vec_file> [--threads N]\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s /memory/vectors/code_local_llama.cpp.jina.bin\n", argv[0]);
        printf("  %s /memory/vectors/code_local_llama.cpp.jina.bin --threads 4\n", argv[0]);
        return 1;
    }
    
    const char* vec_file = argv[1];
    int num_threads = 0;  // 0 = use OpenMP default
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            num_threads = atoi(argv[++i]);
        }
    }
    
    // Read vectors into memory
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
    
    printf("Loading %u vectors of dim %u into memory...\n", count, dim);
    
    float** vectors = malloc(count * sizeof(float*));
    if (!vectors) {
        fprintf(stderr, "Failed to allocate vector pointers\n");
        fclose(fp);
        return 1;
    }
    
    for (uint32_t i = 0; i < count; i++) {
        // Read name length and skip name
        uint32_t name_len;
        if (fread(&name_len, 4, 1, fp) != 1) {
            fprintf(stderr, "Failed to read name length for vector %u\n", i);
            for (uint32_t j = 0; j < i; j++) free(vectors[j]);
            free(vectors);
            fclose(fp);
            return 1;
        }
        if (fseek(fp, name_len, SEEK_CUR) != 0) {
            fprintf(stderr, "Failed to skip name for vector %u\n", i);
            for (uint32_t j = 0; j < i; j++) free(vectors[j]);
            free(vectors);
            fclose(fp);
            return 1;
        }
        
        vectors[i] = malloc(dim * sizeof(float));
        if (!vectors[i]) {
            fprintf(stderr, "Failed to allocate vector %u\n", i);
            for (uint32_t j = 0; j < i; j++) free(vectors[j]);
            free(vectors);
            fclose(fp);
            return 1;
        }
        if (fread(vectors[i], sizeof(float), dim, fp) != dim) {
            fprintf(stderr, "Failed to read vector %u\n", i);
            for (uint32_t j = 0; j <= i; j++) free(vectors[j]);
            free(vectors);
            fclose(fp);
            return 1;
        }
    }
    fclose(fp);
    
    printf("Building HNSW index: %u vectors, %u dims", count, dim);
    if (num_threads > 0) {
        printf(" (%d threads)\n", num_threads);
    } else {
        printf(" (auto threads)\n");
    }
    
    hnsw_index_t* hnsw = hnsw_create(dim);
    if (!hnsw) {
        fprintf(stderr, "Failed to create HNSW index\n");
        for (uint32_t i = 0; i < count; i++) free(vectors[i]);
        free(vectors);
        return 1;
    }
    
    // Configure for code search
    hnsw_set_m(hnsw, 16);
    hnsw_set_ef_construction(hnsw, 200);
    hnsw_set_ef_search(hnsw, 64);
    
    // Pre-allocate capacity for all nodes
    if (hnsw_reserve(hnsw, count + 64) != 0) {
        fprintf(stderr, "Failed to reserve capacity\n");
        hnsw_destroy(hnsw);
        for (uint32_t i = 0; i < count; i++) free(vectors[i]);
        free(vectors);
        return 1;
    }
    
    printf("Inserting vectors...\n");
    
    #ifdef _OPENMP
    if (num_threads > 0) {
        omp_set_num_threads(num_threads);
    }
    #pragma omp parallel for schedule(dynamic, 128)
    #endif
    for (uint32_t i = 0; i < count; i++) {
        if (hnsw_insert_parallel(hnsw, i, vectors[i]) != 0) {
            fprintf(stderr, "Failed to insert vector %u\n", i);
        }
    }
    printf("  Inserted %u/%u\n", count, count);
    
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
        for (uint32_t i = 0; i < count; i++) free(vectors[i]);
        free(vectors);
        return 1;
    }
    
    size_t written = hnsw_serialize(hnsw, buf, size);
    if (written == 0) {
        fprintf(stderr, "Failed to serialize index\n");
        free(buf);
        hnsw_destroy(hnsw);
        for (uint32_t i = 0; i < count; i++) free(vectors[i]);
        free(vectors);
        return 1;
    }
    
    FILE* out = fopen(hnsw_file, "wb");
    if (!out) {
        fprintf(stderr, "Failed to create %s\n", hnsw_file);
        free(buf);
        hnsw_destroy(hnsw);
        for (uint32_t i = 0; i < count; i++) free(vectors[i]);
        free(vectors);
        return 1;
    }
    
    fwrite(buf, 1, written, out);
    fclose(out);
    
    printf("Saved HNSW index: %s (%.2f MB)\n", hnsw_file, written / (1024.0 * 1024.0));
    
    free(buf);
    hnsw_destroy(hnsw);
    for (uint32_t i = 0; i < count; i++) free(vectors[i]);
    free(vectors);
    
    return 0;
}
