#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>
#include "onnx_embedder.h"

#define DIM 768
#define MAX_SEQ 512
#define BATCH_SIZE 16
#define MAX_TEXT_LEN 16384

typedef struct {
    char* name;
    char* text;
} item_t;

// Free an item batch
static void free_items(item_t* items, int count) {
    for (int i = 0; i < count; i++) {
        free(items[i].name);
        free(items[i].text);
    }
}

// Load up to `max_items` items from the text/meta file pair.
// Returns number of items loaded. 0 means EOF or error.
static int load_items_batch(FILE* text_fp, FILE* meta_fp,
                             item_t* items, int max_items) {
    char* text_line = NULL;
    char* meta_line = NULL;
    size_t text_len = 0, meta_len = 0;
    int n = 0;

    while (n < max_items) {
        ssize_t text_read = getline(&text_line, &text_len, text_fp);
        ssize_t meta_read = getline(&meta_line, &meta_len, meta_fp);
        if (text_read == -1 || meta_read == -1) break;

        // Extract name from meta JSON: "name":"..."
        const char* p = strstr(meta_line, "\"name\":\"");
        if (!p) continue;
        p += 8;
        const char* p_end = strchr(p, '"');
        if (!p_end) continue;

        size_t name_len = p_end - p;
        if (name_len == 0 || name_len > 255) continue;

        items[n].name = malloc(name_len + 1);
        if (!items[n].name) break;
        memcpy(items[n].name, p, name_len);
        items[n].name[name_len] = '\0';

        // Copy text (strip trailing newline/cr)
        if (text_read > 0 && text_line[text_read-1] == '\n') text_line[--text_read] = '\0';
        if (text_read > 0 && text_line[text_read-1] == '\r') text_line[--text_read] = '\0';
        size_t copy_len = (text_read < MAX_TEXT_LEN) ? text_read : (MAX_TEXT_LEN - 1);
        items[n].text = malloc(copy_len + 1);
        if (!items[n].text) { free(items[n].name); break; }
        memcpy(items[n].text, text_line, copy_len);
        items[n].text[copy_len] = '\0';

        n++;
    }

    free(text_line);
    free(meta_line);
    return n;
}

// Append vectors to existing .bin / .idx files
static void append_vectors(const char* bin_file, const char* idx_file,
                            item_t* items, float** vectors, int count,
                            int is_first) {
    FILE* bin_fp = fopen(bin_file, is_first ? "wb" : "ab");
    FILE* idx_fp = fopen(idx_file, is_first ? "w" : "a");
    if (!bin_fp || !idx_fp) {
        if (bin_fp) fclose(bin_fp);
        if (idx_fp) fclose(idx_fp);
        return;
    }

    if (is_first) {
        // Write header: count + dim (will be updated at end)
        uint32_t h_count = 0;
        uint32_t h_dim = DIM;
        fwrite(&h_count, 4, 1, bin_fp);
        fwrite(&h_dim, 4, 1, bin_fp);
    }

    // Seek to end of existing data (after header for first batch)
    fseek(bin_fp, 0, SEEK_END);
    size_t offset = ftell(bin_fp);
    if (offset < 8) offset = 8; // minimum: header size

    for (int i = 0; i < count; i++) {
        uint32_t name_len = strlen(items[i].name);
        fwrite(&name_len, 4, 1, bin_fp);
        fwrite(items[i].name, 1, name_len, bin_fp);

        fprintf(idx_fp, "\"%s\":%zu\n", items[i].name, offset + 4 + name_len);

        // Normalize
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

    fclose(bin_fp);
    fclose(idx_fp);
}

// Rewrite header with final count
static void update_header(const char* bin_file, int total_count) {
    FILE* fp = fopen(bin_file, "r+b");
    if (fp) {
        fseek(fp, 0, SEEK_SET);
        fwrite(&total_count, 4, 1, fp);
        fclose(fp);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("C Batch Vector Generator (streaming, low-memory)\n");
        printf("Usage: %s <cache_dir> [--model mpnet|jina] [--name project_name]\n", argv[0]);
        return 1;
    }

    const char* cache_dir = argv[1];
    const char* model_type = "jina";
    const char* custom_name = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) model_type = argv[++i];
        else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) custom_name = argv[++i];
    }

    char text_file[512], meta_file[512];
    snprintf(text_file, sizeof(text_file), "%s/chunks_text.txt", cache_dir);
    snprintf(meta_file, sizeof(meta_file), "%s/chunks_meta.jsonl", cache_dir);

    FILE* text_fp = fopen(text_file, "r");
    FILE* meta_fp = fopen(meta_file, "r");
    if (!text_fp || !meta_fp) {
        if (text_fp) fclose(text_fp);
        if (meta_fp) fclose(meta_fp);
        fprintf(stderr, "Failed to open %s or %s\n", text_file, meta_file);
        return 1;
    }

    // Init embedder
    printf("Loading %s embedder...\n", model_type);
    onnx_embedder_t* embedder;
    if (strcmp(model_type, "jina") == 0) {
        embedder = onnx_embedder_init(
            "/opt/models/jina-embeddings-v2-base-code/model.onnx",
            "/opt/models/jina-embeddings-v2-base-code/vocab.json",
            MAX_SEQ, DIM);
    } else {
        embedder = onnx_embedder_init(
            "/opt/models/all-mpnet-base-v2/model.onnx",
            "/opt/models/all-mpnet-base-v2/vocab.txt", 128, DIM);
    }
    if (!embedder) {
        fprintf(stderr, "Failed to load embedder: %s\n", onnx_embedder_error());
        fclose(text_fp); fclose(meta_fp);
        return 1;
    }

    // Auto-detect project name
    const char* repo_name = strrchr(cache_dir, '/');
    repo_name = repo_name ? repo_name + 1 : cache_dir;
    char safe_name[256];
    if (custom_name) {
        strncpy(safe_name, custom_name, sizeof(safe_name) - 1);
    } else {
        strncpy(safe_name, repo_name, sizeof(safe_name) - 1);
        char* suffix = strstr(safe_name, "_cache");
        if (suffix) *suffix = '\0';
        if (strlen(safe_name) == 0) strcpy(safe_name, "unknown");
    }
    safe_name[sizeof(safe_name) - 1] = '\0';

    // Create vectors/ directory
    char vec_dir[512];
    snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    mkdir(vec_dir, 0755);

    char bin_file[512], idx_file[512];
    snprintf(bin_file, sizeof(bin_file), "%s/code_local_%s.jina.bin", vec_dir, safe_name);
    snprintf(idx_file, sizeof(idx_file), "%s/code_local_%s.jina.idx", vec_dir, safe_name);

    // Streaming: process in batches
    printf("Encoding (streaming, batch_size=%d)...\n", BATCH_SIZE);
    item_t* batch = malloc(BATCH_SIZE * sizeof(item_t));
    if (!batch) { fprintf(stderr, "OOM\n"); return 1; }

    clock_t start = clock();
    int total_processed = 0;
    int is_first = 1;
    int batch_id = 0;

    while (1) {
        int n = load_items_batch(text_fp, meta_fp, batch, BATCH_SIZE);
        if (n == 0) break;
        batch_id++;

        // Allocate vector storage for this batch (contiguous; 批量推理)
        const char** texts = malloc(n * sizeof(char*));
        float* vecbuf = malloc((size_t)n * DIM * sizeof(float));
        float** vectors = malloc(n * sizeof(float*));
        if (!vectors || !texts || !vecbuf) { free(texts); free(vecbuf); free(vectors); free_items(batch, n); break; }
        for (int i = 0; i < n; i++) {
            texts[i] = batch[i].text;
            vectors[i] = vecbuf + (size_t)i * DIM;
        }

        // Encode（一次批量推理，替代逐条）
        if (onnx_embedder_encode_batch(embedder, texts, n, vecbuf) != 0) {
            fprintf(stderr, "Warning: batch encode failed\n");
            memset(vecbuf, 0, (size_t)n * DIM * sizeof(float));
        }

        // Append to files
        append_vectors(bin_file, idx_file, batch, vectors, n, is_first);
        is_first = 0;

        // Cleanup batch
        free(texts);
        free(vecbuf);
        free(vectors);
        free_items(batch, n);

        total_processed += n;

        // Progress
        double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
        printf("  Progress: %d items (%.0f items/s, batch %d)\n",
               total_processed, total_processed / elapsed, batch_id);
    }

    free(batch);
    fclose(text_fp);
    fclose(meta_fp);

    // Update header with final count
    update_header(bin_file, total_processed);

    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    printf("Done in %.1fs (%.0f items/s)\n", elapsed, total_processed / elapsed);
    printf("Saved vectors to %s\n", bin_file);
    printf("Saved index to %s\n", idx_file);

    // Fork HNSW index build
    printf("Starting HNSW index build in background...\n");
    pid_t pid = fork();
    if (pid == 0) {
        execl("./tools/build_hnsw_index", "build_hnsw_index", bin_file, (char*)NULL);
        execlp("tools/build_hnsw_index", "build_hnsw_index", bin_file, (char*)NULL);
        fprintf(stderr, "[HNSW] Failed to start index builder\n");
        _exit(1);
    } else if (pid > 0) {
        printf("HNSW index build started in background (PID: %d)\n", (int)pid);
    }

    onnx_embedder_free(embedder);
    return 0;
}
