#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "cache.h"
#include "onnx_embedder.h"

#define DIM 768
#define BATCH_SIZE_MPNET 64
#define BATCH_SIZE_JINA 32
#define MAX_TEXT_LEN 8192
#define MAX_ITEMS_INITIAL 10000

#define VERSION "2.0.0"

typedef struct {
    char name[256];
    char text[MAX_TEXT_LEN];
} item_t;

static void build_text(const char* name, const char* json, char* text, int max_len);
static void extract_name(const char* key, char* name, int max_len);
static void namespace_to_filename(const char* ns, char* out, size_t out_len);
static int file_exists(const char* path);
static int detect_namespace(const char* cache_dir, char* ns_out, size_t ns_out_len);

static void print_usage(const char* prog) {
    printf("AI Agent Vector Generator v%s\n", VERSION);
    printf("Generate semantic vectors for code repositories.\n\n");
    printf("Usage: %s [options] <cache_dir> [namespace]\n", prog);
    printf("\nOptions:\n");
    printf("  --model <type>   Model type: mpnet (default) or jina\n");
    printf("  --help, -h      Show this help\n");
    printf("\nExamples:\n");
    printf("  %s /memory\n", prog);
    printf("  %s --model jina /memory /code/local/my-project\n", prog);
    printf("\nModels:\n");
    printf("  mpnet - all-mpnet-base-v2 (general text, fast C tokenizer)\n");
    printf("  jina  - jina-embeddings-v2-base-code (code retrieval, BPE tokenizer)\n");
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

static int detect_namespace(const char* cache_dir, char* ns_out, size_t ns_out_len) {
    cache_t* cache = cache_open(cache_dir, 1024 * 1024 * 1024);
    if (!cache) return -1;
    
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    int found = 0;
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        if (strncmp(key, "/code/", 6) == 0) {
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
    
    if (found) {
        size_t len = strlen(ns_out);
        if (len > 0 && ns_out[len - 1] == '/') {
            ns_out[len - 1] = '\0';
        }
    }
    
    return found ? 0 : -1;
}

int main(int argc, char** argv) {
    const char* model_type = "mpnet";
    const char* cache_dir = NULL;
    const char* namespace = NULL;
    
    // Parse options
    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "--model") == 0 && arg_idx + 1 < argc) {
            model_type = argv[++arg_idx];
            arg_idx++;
        } else if (strcmp(argv[arg_idx], "--help") == 0 || strcmp(argv[arg_idx], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[arg_idx]);
            return 1;
        }
    }
    
    if (arg_idx >= argc) {
        print_usage(argv[0]);
        return 1;
    }
    cache_dir = argv[arg_idx++];
    if (arg_idx < argc) {
        namespace = argv[arg_idx];
    }
    
    // Validate model type
    int use_jina = (strcmp(model_type, "jina") == 0);
    
    struct stat st;
    if (stat(cache_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: cache_dir does not exist or is not a directory: %s\n", cache_dir);
        return 1;
    }
    
    char ns[512];
    if (namespace) {
        strncpy(ns, namespace, sizeof(ns) - 1);
        ns[sizeof(ns) - 1] = '\0';
    } else {
        printf("Auto-detecting namespace...\n");
        if (detect_namespace(cache_dir, ns, sizeof(ns)) != 0) {
            fprintf(stderr, "Error: Could not auto-detect namespace.\n");
            return 1;
        }
        printf("Detected namespace: %s\n", ns);
    }
    
    printf("Opening cache: %s\n", cache_dir);
    cache_t* cache = cache_open(cache_dir, 1024 * 1024 * 1024);
    if (!cache) {
        fprintf(stderr, "Failed to open cache\n");
        return 1;
    }
    
    // Initialize embedder
    onnx_embedder_t* embedder = NULL;
    if (use_jina) {
        printf("Loading Jina embedder...\n");
        embedder = onnx_embedder_init(
            "/opt/models/jina-embeddings-v2-base-code/model.onnx",
            "/opt/models/jina-embeddings-v2-base-code/vocab.json",
            512, DIM
        );
        if (!embedder) {
            fprintf(stderr, "Failed: %s\n", onnx_embedder_error());
            cache_close(cache);
            return 1;
        }
    } else {
        printf("Loading MPNet embedder...\n");
        embedder = onnx_embedder_init(
            "/opt/models/all-mpnet-base-v2/model.onnx",
            "/opt/models/all-mpnet-base-v2/vocab.txt",
            128, DIM
        );
        if (!embedder) {
            fprintf(stderr, "Failed: %s\n", onnx_embedder_error());
            cache_close(cache);
            return 1;
        }
    }
    
    // Collect chunks
    printf("Collecting chunks from namespace: %s\n", ns);
    int capacity = MAX_ITEMS_INITIAL;
    item_t* items = malloc(capacity * sizeof(item_t));
    if (!items) {
        fprintf(stderr, "Failed to allocate memory\n");
        if (embedder) onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    int item_count = 0;
    char chunk_prefix[512];
    int n = snprintf(chunk_prefix, sizeof(chunk_prefix), "%s/chunks/", ns);
    if (n < 0 || n >= (int)sizeof(chunk_prefix)) {
        fprintf(stderr, "Error: namespace too long\n");
        free(items);
        if (embedder) onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    size_t prefix_len = strlen(chunk_prefix);
    
    cache_iter_t* iter = cache_iter_create(cache);
    const char* key;
    const char* value;
    
    // Noise directory filters
    const char* noise_dirs[] = {
        "thirdparty/", "3rdparty/", "third_party/", "third-party/",
        "external/", "deps/", "dependencies/", "vendor/",
        "build/", "cmake-build/", "CMakeFiles/", "out/", "bin/", "obj/",
        "test/", "tests/", "testing/", "gtest/", "googletest/",
        "examples/", "demo/", "demos/", "sample/", "samples/",
        "docs/", "doc/", "documentation/", "website/",
        "scripts/", "tools/", "utils/", "benchmark/", "benchmarks/",
        ".git/", "node_modules/", "__pycache__/", ".pytest_cache/",
        NULL
    };
    
    while (cache_iter_next(iter, &key, &value) == 1) {
        if (strncmp(key, chunk_prefix, prefix_len) != 0) continue;
        
        // Extract file path from key: {namespace}/chunks/{filepath}/{name}
        const char* file_path = key + prefix_len;
        
        // Skip noise directories
        int is_noise = 0;
        for (int d = 0; noise_dirs[d]; d++) {
            if (strstr(file_path, noise_dirs[d])) {
                is_noise = 1;
                break;
            }
        }
        if (is_noise) continue;
        
        char name[256];
        extract_name(key, name, sizeof(name));
        
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
        
        build_text(name, value, items[item_count].text, MAX_TEXT_LEN);
        strncpy(items[item_count].name, name, sizeof(items[item_count].name) - 1);
        items[item_count].name[sizeof(items[item_count].name) - 1] = '\0';
        
        if (strlen(items[item_count].text) > 50) {
            item_count++;
        }
    }
    cache_iter_destroy(iter);
    
    printf("Found %d items to encode\n", item_count);
    
    if (item_count == 0) {
        printf("Nothing to do\n");
        free(items);
        if (embedder) onnx_embedder_free(embedder);
        cache_close(cache);
        return 0;
    }
    
    // Encode
    printf("Encoding with %s...\n", use_jina ? "Jina" : "MPNet");
    float* vectors = malloc(item_count * DIM * sizeof(float));
    if (!vectors) {
        fprintf(stderr, "Failed to allocate vectors memory\n");
        free(items);
        if (embedder) onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    int processed = 0;
    clock_t encode_start = clock();
    
    // Use C embedder with batching
    int batch_size = use_jina ? BATCH_SIZE_JINA : BATCH_SIZE_MPNET;
    const char** texts = malloc(batch_size * sizeof(char*));
    if (!texts) {
        free(vectors);
        free(items);
        onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    for (int i = 0; i < item_count; i += batch_size) {
        int batch_end = i + batch_size;
        if (batch_end > item_count) batch_end = item_count;
        int batch_count = batch_end - i;
        
        for (int j = 0; j < batch_count; j++) {
            texts[j] = items[i + j].text;
        }
        
        int ret = onnx_embedder_encode_batch(embedder, texts, batch_count, vectors + i * DIM);
        if (ret == 0) {
            processed += batch_count;
        } else {
            for (int j = 0; j < batch_count; j++) {
                if (onnx_embedder_encode(embedder, items[i + j].text, vectors + (i + j) * DIM) == 0) {
                    processed++;
                }
            }
        }
        
        if ((i / batch_size) % 10 == 0 && i > 0) {
            printf("  Progress: %d/%d\n", i, item_count);
        }
    }
    free(texts);
    
    double encode_time = (double)(clock() - encode_start) / CLOCKS_PER_SEC;
    printf("Encoded %d/%d items in %.1fs (%.1f items/s)\n", 
           processed, item_count, encode_time, processed / encode_time);
    
    // Store vectors
    printf("Storing vectors to binary file...\n");
    clock_t store_start = clock();
    
    char vec_dir[512];
    snprintf(vec_dir, sizeof(vec_dir), "%s/vectors", cache_dir);
    if (mkdir(vec_dir, 0755) != 0) {
        // May already exist
    }
    
    char safe_ns[256];
    namespace_to_filename(ns, safe_ns, sizeof(safe_ns));
    
    char vec_file[1024], idx_file[1024];
    const char* model_suffix = use_jina ? "jina" : "mpnet";
    int n1 = snprintf(vec_file, sizeof(vec_file), "%s/%s.%s.bin", vec_dir, safe_ns, model_suffix);
    int n2 = snprintf(idx_file, sizeof(idx_file), "%s/%s.%s.idx", vec_dir, safe_ns, model_suffix);
    if (n1 >= (int)sizeof(vec_file) || n2 >= (int)sizeof(idx_file)) {
        fprintf(stderr, "Error: Path too long\n");
        free(items);
        free(vectors);
        if (embedder) onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    FILE* fp = fopen(vec_file, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to create %s\n", vec_file);
        free(items);
        free(vectors);
        if (embedder) onnx_embedder_free(embedder);
        cache_close(cache);
        return 1;
    }
    
    uint32_t count = item_count;
    uint32_t dim = DIM;
    fwrite(&count, 4, 1, fp);
    fwrite(&dim, 4, 1, fp);
    fwrite(vectors, sizeof(float), item_count * DIM, fp);
    fclose(fp);
    
    // Write index
    size_t offset = 8;
    typedef struct { char name[256]; int count; } name_count_t;
    name_count_t* name_table = calloc(item_count, sizeof(name_count_t));
    int name_table_size = 0;
    char json_escaped[512];
    
    FILE* idx_fp = fopen(idx_file, "w");
    if (idx_fp) {
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
            
            int je = 0;
            for (int k = 0; unique_name[k] && je < 510; k++) {
                char c = unique_name[k];
                if (c == '"' || c == '\\') {
                    json_escaped[je++] = '\\';
                    json_escaped[je++] = c;
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
    }
    free(name_table);
    
    printf("Binary file: %s\n", vec_file);
    printf("Index file: %s\n", idx_file);
    
    double store_time = (double)(clock() - store_start) / CLOCKS_PER_SEC;
    printf("Stored %d vectors in %.1fs (%.1f items/s)\n",
           item_count, store_time, item_count / store_time);
    
    if (stat(vec_file, &st) == 0) {
        size_t expected = 8 + item_count * DIM * sizeof(float);
        printf("File size: %ld bytes (expected: %zu) OK=%d\n", 
               st.st_size, expected, st.st_size == (long)expected);
    }
    
    printf("Done! Total: %.1fs\n", encode_time + store_time);
    
    free(items);
    free(vectors);
    if (embedder) onnx_embedder_free(embedder);
    cache_close(cache);
    
    return 0;
}

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

static void build_text(const char* name, const char* json, char* text, int max_len) {
    char sig[512] = {0};
    char doc[1024] = {0};
    char content[4096] = {0};
    char lang[32] = {0};
    
    extract_field(json, "signature", sig, sizeof(sig));
    extract_field(json, "docstring", doc, sizeof(doc));
    extract_field(json, "content", content, sizeof(content));
    extract_field(json, "language", lang, sizeof(lang));
    
    char lines[10][256];
    int line_count = 0;
    char* p = content;
    while (*p && line_count < 10) {
        while (*p && (*p == ' ' || *p == '\t')) p++;
        
        int i = 0;
        while (*p && *p != '\n' && i < 255) {
            lines[line_count][i++] = *p++;
        }
        lines[line_count][i] = '\0';
        
        if (i > 0) {
            line_count++;
        }
        if (*p == '\n') p++;
    }
    
    int n = 0;
    if (name[0]) {
        n += snprintf(text + n, max_len - n, "%s ", name);
    }
    if (lang[0]) {
        n += snprintf(text + n, max_len - n, "(%s) ", lang);
    }
    if (sig[0]) {
        n += snprintf(text + n, max_len - n, "%s ", sig);
    }
    if (doc[0]) {
        n += snprintf(text + n, max_len - n, "%s ", doc);
    }
    for (int i = 0; i < line_count && n < max_len - 1; i++) {
        n += snprintf(text + n, max_len - n, "%s ", lines[i]);
    }
    
    if (n > 0 && text[n-1] == ' ') {
        text[n-1] = '\0';
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
