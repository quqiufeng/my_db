#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "onnx_embedder.h"

#define DIM 768
#define MAX_SEQ 512
#define BATCH_SIZE 128
#define MAX_TEXT_LEN 8192

typedef struct {
    char name[256];
    char text[MAX_TEXT_LEN];
} item_t;

static int load_items(const char* text_file, const char* meta_file, item_t** items, int* count) {
    FILE* text_fp = fopen(text_file, "r");
    FILE* meta_fp = fopen(meta_file, "r");
    if (!text_fp || !meta_fp) {
        if (text_fp) fclose(text_fp);
        if (meta_fp) fclose(meta_fp);
        return -1;
    }

    int capacity = 10000;
    *items = malloc(capacity * sizeof(item_t));
    if (!*items) {
        fclose(text_fp); fclose(meta_fp);
        return -1;
    }

    char* text_line = NULL;
    char* meta_line = NULL;
    size_t text_len = 0, meta_len = 0;
    ssize_t text_read, meta_read;
    int n = 0;

    while ((text_read = getline(&text_line, &text_len, text_fp)) != -1 &&
           (meta_read = getline(&meta_line, &meta_len, meta_fp)) != -1) {
        if (n >= capacity) {
            capacity *= 2;
            item_t* new_items = realloc(*items, capacity * sizeof(item_t));
            if (!new_items) break;
            *items = new_items;
        }

        // Extract name from meta JSON
        const char* p = strstr(meta_line, "\"name\":\"");
        if (p) {
            p += 8;
            int i = 0;
            while (*p && *p != '"' && i < 255) {
                (*items)[n].name[i++] = *p++;
            }
            (*items)[n].name[i] = '\0';

            // Copy text (strip newline, truncate to MAX_TEXT_LEN-1)
            if (text_read > 0 && text_line[text_read-1] == '\n') text_line[text_read-1] = '\0';
            if (text_read > 0 && text_line[text_read-1] == '\r') text_line[text_read-1] = '\0';
            strncpy((*items)[n].text, text_line, MAX_TEXT_LEN - 1);
            (*items)[n].text[MAX_TEXT_LEN - 1] = '\0';

            n++;
        }
    }

    free(text_line);
    free(meta_line);
    fclose(text_fp); fclose(meta_fp);
    *count = n;
    return 0;
}

static void save_vectors(const char* bin_file, const char* idx_file,
                         item_t* items, float** vectors, int count) {
    FILE* bin_fp = fopen(bin_file, "wb");
    FILE* idx_fp = fopen(idx_file, "w");
    if (!bin_fp || !idx_fp) {
        if (bin_fp) fclose(bin_fp);
        if (idx_fp) fclose(idx_fp);
        return;
    }

    // Header: count + dim
    uint32_t h_count = count;
    uint32_t h_dim = DIM;
    fwrite(&h_count, 4, 1, bin_fp);
    fwrite(&h_dim, 4, 1, bin_fp);

    size_t offset = 8;
    for (int i = 0; i < count; i++) {
        // Write vector: name_len(4) + name + dim floats
        uint32_t name_len = strlen(items[i].name);
        fwrite(&name_len, 4, 1, bin_fp);
        fwrite(items[i].name, 1, name_len, bin_fp);

        // Write index entry: "name":offset (pointing to vec data, not name_len)
        fprintf(idx_fp, "\"%s\":%zu\n", items[i].name, offset + 4 + name_len);

        // Normalize vector before saving
        float vec[DIM];
        memcpy(vec, vectors[i], DIM * sizeof(float));
        float norm = 0;
        for (int j = 0; j < DIM; j++) norm += vec[j] * vec[j];
        norm = sqrtf(norm);
        if (norm > 1e-12) {
            for (int j = 0; j < DIM; j++) vec[j] /= norm;
        }

        fwrite(vec, sizeof(float), DIM, bin_fp);
        offset += 4 + name_len + DIM * sizeof(float);
    }

    fclose(bin_fp); fclose(idx_fp);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("C Batch Vector Generator (Jina/MPNet)\n");
        printf("Usage: %s <cache_dir> [--model mpnet|jina] [--name project_name]\n", argv[0]);
        return 1;
    }

    const char* cache_dir = argv[1];
    const char* model_type = "jina";
    const char* custom_name = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_type = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            custom_name = argv[++i];
        }
    }

    char text_file[512], meta_file[512];
    snprintf(text_file, sizeof(text_file), "%s/chunks_text.txt", cache_dir);
    snprintf(meta_file, sizeof(meta_file), "%s/chunks_meta.jsonl", cache_dir);

    printf("Loading items from %s...\n", cache_dir);
    item_t* items = NULL;
    int count = 0;
    if (load_items(text_file, meta_file, &items, &count) != 0 || count == 0) {
        fprintf(stderr, "Failed to load items\n");
        return 1;
    }
    printf("Loaded %d items\n", count);

    // Init embedder
    printf("Loading %s embedder...\n", model_type);
    onnx_embedder_t* embedder;
    if (strcmp(model_type, "jina") == 0) {
        embedder = onnx_embedder_init(
            "models/jina-embeddings-v2-base-code/model.onnx",
            "models/jina-embeddings-v2-base-code/vocab.json",
            MAX_SEQ, DIM
        );
    } else {
        embedder = onnx_embedder_init(
            "models/all-mpnet-base-v2/model.onnx",
            "models/all-mpnet-base-v2/vocab.txt",
            128, DIM
        );
    }
    if (!embedder) {
        fprintf(stderr, "Failed to load embedder: %s\n", onnx_embedder_error());
        free(items);
        return 1;
    }

    // Allocate vectors
    float** vectors = malloc(count * sizeof(float*));
    for (int i = 0; i < count; i++) {
        vectors[i] = malloc(DIM * sizeof(float));
    }

    // Batch encode
    printf("Encoding...\n");
    clock_t start = clock();

    int batch_size = (strcmp(model_type, "jina") == 0) ? 64 : 128;
    int processed = 0;

    for (int i = 0; i < count; i += batch_size) {
        int bsize = (i + batch_size <= count) ? batch_size : (count - i);

        for (int j = 0; j < bsize; j++) {
            if (onnx_embedder_encode(embedder, items[i + j].text, vectors[i + j]) != 0) {
                fprintf(stderr, "Failed to encode item %d\n", i + j);
            }
        }

        processed += bsize;
        if ((processed / batch_size) % 10 == 0) {
            double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
            printf("  Progress: %d/%d (%.0f items/s)\n", processed, count, processed / elapsed);
        }
    }

    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("Done in %.1fs (%.0f items/s)\n", elapsed, count / elapsed);

    // Auto-detect repo name from cache_dir path
    // e.g., "./linux_cache" -> "linux", "./ai_code_memory" -> "ai_code_memory"
    const char* repo_name = strrchr(cache_dir, '/');
    if (repo_name) repo_name++;
    else repo_name = cache_dir;
    
    char safe_name[256];
    if (custom_name) {
        strncpy(safe_name, custom_name, sizeof(safe_name) - 1);
        safe_name[sizeof(safe_name) - 1] = '\0';
    } else {
        strncpy(safe_name, repo_name, sizeof(safe_name) - 1);
        safe_name[sizeof(safe_name) - 1] = '\0';
        char* suffix = strstr(safe_name, "_cache");
        if (suffix) *suffix = '\0';
        if (strlen(safe_name) == 0) strcpy(safe_name, "unknown");
    }
    
    // Create vectors/ directory
    char vec_dir[512];
    snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    mkdir(vec_dir, 0755);
    
    // Build filenames: code_local_<repo>.jina.bin
    char bin_file[512], idx_file[512];
    snprintf(bin_file, sizeof(bin_file), "%s/code_local_%s.jina.bin", vec_dir, safe_name);
    snprintf(idx_file, sizeof(idx_file), "%s/code_local_%s.jina.idx", vec_dir, safe_name);

    save_vectors(bin_file, idx_file, items, vectors, count);
    printf("Saved vectors to %s\n", bin_file);
    printf("Saved index to %s\n", idx_file);

    // Cleanup
    onnx_embedder_free(embedder);
    for (int i = 0; i < count; i++) free(vectors[i]);
    free(vectors);
    free(items);

    return 0;
}
