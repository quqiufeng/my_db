#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include "cache.h"
#include "onnx_embedder.h"

#define DIM 768
#define BATCH_SIZE 512
#define MAX_TEXT_LEN 2048
#define MAX_ITEMS_INITIAL 10000

#define VERSION "1.1.0"

typedef struct {
    char name[256];
    char text[MAX_TEXT_LEN];
} item_t;

static void build_text(const char* json, char* text, int max_len);
static void extract_name(const char* key, char* name, int max_len);
static void namespace_to_filename(const char* ns, char* out, size_t out_len);
static int file_exists(const char* path);

static void print_usage(const char* prog) {
    printf("AI Agent Vector Generator v%s\n", VERSION);
    printf("Generate semantic vectors for code repositories.\n\n");
    printf("Usage: %s <cache_dir> [namespace]\n", prog);
    printf("\nArguments:\n");
    printf("  cache_dir    Path to cache directory (contains cache.bin)\n");
    printf("  namespace    Repository namespace (default: auto-detect)\n");
    printf("\nExamples:\n");
    printf("  %s ./ai_code_memory\n", prog);
    printf("  %s ./ai_code_memory /code/local/my-project\n", prog);
    printf("  %s ./ai_code_memory /code/github/redis/redis\n", prog);
    printf("\nEnvironment:\n");
    printf("  Set LD_LIBRARY_PATH to include TensorRT and CUDA libraries.\n");
}

static int file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

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

// Auto-detect namespace from cache metadata
static int detect_namespace(const char* cache_dir, char* ns_out, size_t ns_out_len) {
    cache_t* cache = cache_open(cache_dir, 1024 * 1024 * 1024);
    if (!cache) return -1;
    
    // Look for /code/* namespaces
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    int found = 0;
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        if (strncmp(key, "/code/", 6) == 0) {
            // Extract /code/owner/repo part
            const char* p = key + 6;
            int slashes = 0;
            size_t len = 6;
            for (; *p && slashes < 2 && len < ns_out_len - 1; p++, len++) {
                if (*p == '/') slashes++;
            }
            if (slashes >= 1) {
                strncpy(ns_out, key, len);
                ns_out[len] = '\0';
                found = 1;
                break;
            }
        }
    }
    cache_iter_destroy(iter);
    cache_close(cache);
    
    return found ? 0 : -1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    
    const char* cache_dir = argv[1];
    
    // Validate cache_dir
    struct stat st;
    if (stat(cache_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: cache_dir does not exist or is not a directory: %s\n", cache_dir);
        return 1;
    }
    
    // Determine namespace
    char namespace[512];
    if (argc > 2) {
        strncpy(namespace, argv[2], sizeof(namespace) - 1);
        namespace[sizeof(namespace) - 1] = '\0';
    } else {
        printf("Auto-detecting namespace...\n");
        if (detect_namespace(cache_dir, namespace, sizeof(namespace)) != 0) {
            fprintf(stderr, "Error: Could not auto-detect namespace.\n");
            fprintf(stderr, "Please specify namespace explicitly:\n");
            fprintf(stderr, "  %s %s /code/local/your-project\n", argv[0], cache_dir);
            return 1;
        }
        printf("Detected namespace: %s\n", namespace);
    }
    
    printf("Opening cache: %s\n", cache_dir);
    cache_t* cache = cache_open(cache_dir, 1024 * 1024 * 1024);
    if (!cache) {
        fprintf(stderr, "Failed to open cache\n");
        return 1;
    }
    
    printf("Loading embedder...\n");
    onnx_embedder_t* embedder = onnx_embedder_init(
        "models/all-mpnet-base-v2/model.onnx",
        "models/all-mpnet-base-v2/vocab.txt",
        128, DIM
    );
    if (!embedder) {
        fprintf(stderr, "Failed: %s\n", onnx_embedder_error());
        cache_close(cache);
        return 1;
    }
    
    // 收集 chunks (dynamic array)
    printf("Collecting chunks from namespace: %s\n", namespace);
    int capacity = MAX_ITEMS_INITIAL;
    item_t* items = malloc(capacity * sizeof(item_t));
    if (!items) {
        fprintf(stderr, "Failed to allocate memory for items\n");
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    int item_count = 0;
    
    char chunk_prefix[512];
    int n = snprintf(chunk_prefix, sizeof(chunk_prefix), "%s/chunks/", namespace);
    if (n < 0 || n >= (int)sizeof(chunk_prefix)) {
        fprintf(stderr, "Error: namespace too long\n");
        free(items);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    size_t prefix_len = strlen(chunk_prefix);
    
    // Check if vector files already exist (for resume support)
    char vec_dir[512];
    snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    
    char safe_ns[256];
    namespace_to_filename(namespace, safe_ns, sizeof(safe_ns));
    
    char existing_vec[1024], existing_idx[1024];
    snprintf(existing_vec, sizeof(existing_vec), "%s/%s.bin", vec_dir, safe_ns);
    snprintf(existing_idx, sizeof(existing_idx), "%s/%s.idx", vec_dir, safe_ns);
    
    // Build set of existing vector names for incremental generation
    // (simple approach: just check if files exist, skip if both exist)
    int skip_existing = 0;
    if (file_exists(existing_vec) && file_exists(existing_idx)) {
        printf("Note: Vector files already exist. Use --force to regenerate.\n");
        printf("  %s\n", existing_vec);
        // Still continue to check for new chunks
    }
    
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        if (strncmp(key, chunk_prefix, prefix_len) != 0) continue;
        
        char name[256];
        extract_name(key, name, sizeof(name));
        
        // Expand array if needed
        if (item_count >= capacity) {
            int new_capacity = capacity * 2;
            item_t* new_items = realloc(items, new_capacity * sizeof(item_t));
            if (!new_items) {
                fprintf(stderr, "Failed to expand items array\n");
                break;
            }
            items = new_items;
            capacity = new_capacity;
        }
        
        build_text(value, items[item_count].text, MAX_TEXT_LEN);
        strncpy(items[item_count].name, name, sizeof(items[item_count].name) - 1);
        items[item_count].name[sizeof(items[item_count].name) - 1] = '\0';
        
        if (strlen(items[item_count].text) > 10) {
            item_count++;
        }
    }
    cache_iter_destroy(iter);
    
    printf("Found %d items to encode\n", item_count);
    
    if (item_count == 0) {
        printf("Nothing to do\n");
        free(items);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 0;
    }
    
    // 编码
    printf("Encoding with batch size %d...\n", BATCH_SIZE);
    float* vectors = malloc(item_count * DIM * sizeof(float));
    if (!vectors) {
        fprintf(stderr, "Failed to allocate vectors memory\n");
        free(items);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    int processed = 0;
    clock_t encode_start = clock();
    
    const char** texts = malloc(BATCH_SIZE * sizeof(char*));
    if (!texts) {
        fprintf(stderr, "Failed to allocate texts buffer\n");
        free(vectors);
        free(items);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    for (int i = 0; i < item_count; i += BATCH_SIZE) {
        int batch_end = i + BATCH_SIZE;
        if (batch_end > item_count) batch_end = item_count;
        int batch_count = batch_end - i;
        
        for (int j = 0; j < batch_count; j++) {
            texts[j] = items[i + j].text;
        }
        
        int ret = onnx_embedder_encode_batch(embedder, texts, batch_count, vectors + i * DIM);
        if (ret == 0) {
            processed += batch_count;
        } else {
            // Fallback to single encoding
            for (int j = 0; j < batch_count; j++) {
                if (onnx_embedder_encode(embedder, items[i + j].text, vectors + (i + j) * DIM) == 0) {
                    processed++;
                }
            }
        }
        
        if ((i / BATCH_SIZE) % 10 == 0 && i > 0) {
            printf("  Progress: %d/%d\n", i, item_count);
        }
    }
    
    free(texts);
    double encode_time = (double)(clock() - encode_start) / CLOCKS_PER_SEC;
    printf("Encoded %d/%d items in %.1fs (%.1f items/s)\n", 
           processed, item_count, encode_time, processed / encode_time);
    
    // 存储到二进制文件
    printf("Storing vectors to binary file...\n");
    clock_t store_start = clock();
    
    if (mkdir(vec_dir, 0755) != 0) {
        // Directory may already exist, that's fine
    }
    
    char vec_file[1024], idx_file[1024];
    int n1 = snprintf(vec_file, sizeof(vec_file), "%s/%s.bin", vec_dir, safe_ns);
    int n2 = snprintf(idx_file, sizeof(idx_file), "%s/%s.idx", vec_dir, safe_ns);
    if (n1 >= (int)sizeof(vec_file) || n2 >= (int)sizeof(idx_file)) {
        fprintf(stderr, "Error: Path too long\n");
        free(items);
        free(vectors);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    // Write binary file: [count:4][dim:4][vectors...]
    FILE* fp = fopen(vec_file, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to create %s\n", vec_file);
        free(items);
        free(vectors);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    uint32_t count = item_count;
    uint32_t dim = DIM;
    size_t written = 0;
    written += fwrite(&count, 4, 1, fp);
    written += fwrite(&dim, 4, 1, fp);
    written += fwrite(vectors, sizeof(float), item_count * DIM, fp);
    fclose(fp);
    
    if (written != (size_t)(2 + item_count * DIM)) {
        fprintf(stderr, "Warning: Incomplete write to %s\n", vec_file);
    }
    
    // Write index file: JSON mapping name -> offset
    size_t offset = 8;  // skip header
    
    typedef struct {
        char name[256];
        int count;
    } name_count_t;
    
    name_count_t* name_table = calloc(item_count, sizeof(name_count_t));
    if (!name_table) {
        fprintf(stderr, "Failed to allocate name table\n");
        free(items);
        free(vectors);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    int name_table_size = 0;
    char json_escaped[512];
    
    FILE* idx_fp = fopen(idx_file, "w");
    if (!idx_fp) {
        fprintf(stderr, "Failed to create %s\n", idx_file);
        free(name_table);
        free(items);
        free(vectors);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    fprintf(idx_fp, "{");
    for (int i = 0; i < item_count; i++) {
        int dup_count = 0;
        for (int j = 0; j < name_table_size; j++) {
            if (strcmp(name_table[j].name, items[i].name) == 0) {
                dup_count = ++name_table[j].count;
                break;
            }
        }
        
        char unique_name[256];
        if (dup_count == 0) {
            strncpy(name_table[name_table_size].name, items[i].name, 255);
            name_table[name_table_size].name[255] = '\0';
            name_table[name_table_size].count = 0;
            name_table_size++;
            strncpy(unique_name, items[i].name, sizeof(unique_name) - 1);
            unique_name[sizeof(unique_name) - 1] = '\0';
        } else {
            snprintf(unique_name, sizeof(unique_name), "%s_%d", items[i].name, dup_count);
        }
        
        // Escape for JSON
        int je = 0;
        for (int k = 0; unique_name[k] && je < 510; k++) {
            char c = unique_name[k];
            if (c == '"' || c == '\\') {
                json_escaped[je++] = '\\';
                json_escaped[je++] = c;
            } else if (c == '\b') {
                json_escaped[je++] = '\\'; json_escaped[je++] = 'b';
            } else if (c == '\f') {
                json_escaped[je++] = '\\'; json_escaped[je++] = 'f';
            } else if (c == '\n') {
                json_escaped[je++] = '\\'; json_escaped[je++] = 'n';
            } else if (c == '\r') {
                json_escaped[je++] = '\\'; json_escaped[je++] = 'r';
            } else if (c == '\t') {
                json_escaped[je++] = '\\'; json_escaped[je++] = 't';
            } else if ((unsigned char)c < 0x20) {
                je += snprintf(json_escaped + je, 512 - je, "\\u%04x", (unsigned char)c);
            } else {
                json_escaped[je++] = c;
            }
        }
        json_escaped[je] = '\0';
        
        if (i > 0) fprintf(idx_fp, ",");
        fprintf(idx_fp, "\"%s\":%zu", json_escaped, offset);
        offset += DIM * sizeof(float);
    }
    fprintf(idx_fp, "}\n");
    fclose(idx_fp);
    free(name_table);
    
    printf("Binary file: %s\n", vec_file);
    printf("Index file: %s\n", idx_file);
    
    double store_time = (double)(clock() - store_start) / CLOCKS_PER_SEC;
    printf("Stored %d vectors in %.1fs (%.1f items/s)\n",
           item_count, store_time, item_count / store_time);
    
    // Verify file size
    if (stat(vec_file, &st) == 0) {
        size_t expected = 8 + item_count * DIM * sizeof(float);
        printf("File size: %ld bytes (expected: %zu) OK=%d\n", 
               st.st_size, expected, st.st_size == (long)expected);
    }
    
    printf("Done! Total: %.1fs\n", encode_time + store_time);
    
    free(items);
    free(vectors);
    onnx_embedder_free(embedder);
    cache_close(cache);
    
    return 0;
}

// 从 JSON 提取字段
static int extract_field(const char* json, const char* field, char* out, int max_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char* p = strstr(json, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p && isspace(*p)) p++;
    if (*p != '"') return 0;
    p++;
    
    int i = 0;
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
    out[i] = '\0';
    return i;
}

static void build_text(const char* json, char* text, int max_len) {
    char sig[512] = {0};
    char doc[512] = {0};
    char content[1024] = {0};
    
    extract_field(json, "signature", sig, sizeof(sig));
    extract_field(json, "docstring", doc, sizeof(doc));
    extract_field(json, "content", content, sizeof(content));
    
    char lines[5][256];
    int line_count = 0;
    char* p = content;
    while (*p && line_count < 5) {
        int i = 0;
        while (*p && *p != '\n' && i < 255) {
            lines[line_count][i++] = *p++;
        }
        lines[line_count][i] = '\0';
        line_count++;
        if (*p == '\n') p++;
    }
    
    int n = 0;
    if (sig[0]) n += snprintf(text + n, max_len - n, "%s\n", sig);
    if (doc[0]) n += snprintf(text + n, max_len - n, "%s\n", doc);
    for (int i = 0; i < line_count && n < max_len - 1; i++) {
        n += snprintf(text + n, max_len - n, "%s\n", lines[i]);
    }
}

static void extract_name(const char* key, char* name, int max_len) {
    const char* last_slash = strrchr(key, '/');
    if (last_slash) {
        strncpy(name, last_slash + 1, max_len - 1);
        name[max_len - 1] = '\0';
    } else {
        strncpy(name, key, max_len - 1);
        name[max_len - 1] = '\0';
    }
}
