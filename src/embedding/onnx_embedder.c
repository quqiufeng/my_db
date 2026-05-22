#include "onnx_embedder.h"
#include "tokenizers-cpp/tokenizers_c.h"
#include <onnxruntime_c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dlfcn.h>

#define MAX_VOCAB_SIZE 50000
#define MAX_TOKEN_LEN 64

// Tokenizer types
typedef enum {
    TOKENIZER_WORDPIECE,
    TOKENIZER_BPE
} tokenizer_type_t;

// Global ONNX Runtime API pointer
static const OrtApi* g_ort = NULL;

// Vocab hash table entry
typedef struct {
    char token[MAX_TOKEN_LEN];
    int id;
    int occupied;
} vocab_hash_entry_t;

// Embedder state
struct onnx_embedder {
    OrtEnv* env;
    OrtSession* session;
    OrtMemoryInfo* memory_info;
    
    // Tokenizer
    tokenizer_type_t tokenizer_type;
    vocab_hash_entry_t* vocab_hash;
    int vocab_hash_size;
    int vocab_size;
    TokenizerHandle tokenizers_handle;
    
    int max_seq_length;
    int dim;
    int unk_id;
    int cls_id;
    int sep_id;
    int pad_id;
    int bos_id;
    int eos_id;
    
    // ONNX model output name
    const char* output_name;
    
    // Use pooled output (CLS token) instead of mean pooling
    int use_pooled_output;
    
    // Working buffers
    int64_t* input_ids;
    int64_t* attention_mask;
};

static __thread char g_error_msg[256] = {0};

static void set_error(const char* msg) {
    strncpy(g_error_msg, msg, sizeof(g_error_msg) - 1);
    g_error_msg[sizeof(g_error_msg) - 1] = '\0';
}

const char* onnx_embedder_error(void) {
    return g_error_msg;
}

// Initialize ONNX Runtime API
// Use RTLD_GLOBAL so provider libraries can find symbols in the main library
static int init_ort_api(void) {
    if (g_ort) return 0;
    
    // Load ONNX Runtime with RTLD_GLOBAL for provider symbol visibility
    void* handle = dlopen("libonnxruntime.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        // Try alternate name
        handle = dlopen("libonnxruntime_gpu.so", RTLD_NOW | RTLD_GLOBAL);
    }
    // Note: handle intentionally not stored - library remains loaded for session lifetime
    
    const OrtApiBase* base = OrtGetApiBase();
    if (!base) {
        set_error("Failed to get ONNX API base");
        return -1;
    }
    
    g_ort = base->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        set_error("Failed to get ONNX API");
        return -1;
    }
    
    return 0;
}

// Simple djb2 hash for strings
static unsigned int hash_str(const char* str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// Load vocab into hash table
static int load_vocab(const char* vocab_path, vocab_hash_entry_t** out_hash, int* out_hash_size,
                      int* out_vocab_size, int* unk_id, int* cls_id, int* sep_id, int* pad_id) {
    FILE* f = fopen(vocab_path, "r");
    if (!f) {
        set_error("Failed to open vocab file");
        return -1;
    }
    
    // First pass: count tokens
    char line[256];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < MAX_VOCAB_SIZE) {
        count++;
    }
    rewind(f);
    
    // Allocate hash table (2x size for low collision)
    int hash_size = count * 2 + 1;
    vocab_hash_entry_t* hash_table = calloc(hash_size, sizeof(vocab_hash_entry_t));
    if (!hash_table) {
        fclose(f);
        set_error("Failed to allocate vocab hash table");
        return -1;
    }
    
    count = 0;
    while (fgets(line, sizeof(line), f) && count < MAX_VOCAB_SIZE) {
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 1 && line[len-2] == '\r') line[len-2] = '\0';
        
        if (strcmp(line, "[UNK]") == 0) *unk_id = count;
        if (strcmp(line, "[CLS]") == 0) *cls_id = count;
        if (strcmp(line, "[SEP]") == 0) *sep_id = count;
        if (strcmp(line, "[PAD]") == 0) *pad_id = count;
        
        // Insert into hash table
        unsigned int h = hash_str(line) % hash_size;
        while (hash_table[h].occupied) {
            h = (h + 1) % hash_size;
        }
        strncpy(hash_table[h].token, line, MAX_TOKEN_LEN - 1);
        hash_table[h].token[MAX_TOKEN_LEN - 1] = '\0';
        hash_table[h].id = count;
        hash_table[h].occupied = 1;
        
        count++;
    }
    fclose(f);
    
    *out_hash = hash_table;
    *out_hash_size = hash_size;
    *out_vocab_size = count;
    return 0;
}

// Find token id in vocab hash table
static int vocab_lookup(vocab_hash_entry_t* hash_table, int hash_size, const char* token) {
    unsigned int h = hash_str(token) % hash_size;
    int start = h;
    while (hash_table[h].occupied) {
        if (strcmp(hash_table[h].token, token) == 0) {
            return hash_table[h].id;
        }
        h = (h + 1) % hash_size;
        if (h == start) break;
    }
    return -1;
}

// UTF-8 character classification
static int is_cjk_utf8(const char* p) {
    unsigned char c = (unsigned char)*p;
    // CJK Unified Ideographs: U+4E00-U+9FFF (UTF-8: E4 B8 80 - E9 BF BF)
    // CJK Extension A: U+3400-U+4DBF (UTF-8: E3 90 80 - E4 B6 BF)
    // CJK Extension B: U+20000-U+2A6DF (UTF-8: F0 A0 80 80 - F0 AA 9B 9F)
    if (c == 0xE4 || c == 0xE5 || c == 0xE6 || c == 0xE7 || c == 0xE8 || c == 0xE9) {
        return 3; // Most common CJK
    }
    if (c == 0xE3) {
        unsigned char c2 = (unsigned char)*(p+1);
        if (c2 >= 0x90) return 3; // Extension A
    }
    if (c == 0xF0) {
        unsigned char c2 = (unsigned char)*(p+1);
        if (c2 >= 0xA0 && c2 <= 0xAA) return 4; // Extension B
    }
    return 0;
}

static int is_punctuation_utf8(const char* p) {
    unsigned char c = (unsigned char)*p;
    // ASCII punctuation
    if (c < 0x80) {
        return c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' ||
               c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
               c == '"' || c == '\'' || c == '`' || c == '-' || c == '_' ||
               c == '+' || c == '=' || c == '*' || c == '/' || c == '\\' ||
               c == '<' || c == '>' || c == '|' || c == '&' || c == '%' || c == '$' ||
               c == '#' || c == '@' || c == '^' || c == '~';
    }
    // CJK punctuation (UTF-8 3-byte: E3/EF/EF...)
    if (c == 0xE3) {
        unsigned char c2 = (unsigned char)*(p+1);
        unsigned char c3 = (unsigned char)*(p+2);
        // 。，、；：？！""''（）【】《》…—～·
        if (c2 == 0x80) {
            return c3 == 0x80 || c3 == 0x81 || c3 == 0x82 || c3 == 0x83 || c3 == 0x84 ||
                   c3 == 0x85 || c3 == 0x86 || c3 == 0x87 || c3 == 0x88 || c3 == 0x89 ||
                   c3 == 0x8C || c3 == 0x8D || c3 == 0x8E || c3 == 0x8F || c3 == 0x90 ||
                   c3 == 0x94 || c3 == 0x98 || c3 == 0x99 || c3 == 0x9A || c3 == 0x9B ||
                   c3 == 0x9C || c3 == 0x9D || c3 == 0x9E || c3 == 0x9F || c3 == 0xA0 ||
                   c3 == 0xA1;
        }
    }
    return 0;
}

// Extract next token from text
// Returns bytes consumed, fills token buffer
static int extract_next_token(const char* text, char* token, int max_len) {
    if (!text || !*text) return 0;
    
    const char* p = text;
    
    // Skip whitespace
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return p - text;
    
    // CJK character: extract one character (3 or 4 bytes)
    int cjk_len = is_cjk_utf8(p);
    if (cjk_len > 0) {
        if (cjk_len < max_len) {
            memcpy(token, p, cjk_len);
            token[cjk_len] = '\0';
        }
        return p - text + cjk_len;
    }
    
    // Punctuation: skip it (don't tokenize)
    int punct_len = is_punctuation_utf8(p);
    if (punct_len > 0) {
        return p - text + punct_len;
    }
    
    // ASCII word: extract until whitespace/punctuation/CJK
    int wlen = 0;
    while (*p && wlen < max_len - 1) {
        if (isspace((unsigned char)*p)) break;
        int cl = is_cjk_utf8(p);
        if (cl > 0) break;
        int pl = is_punctuation_utf8(p);
        if (pl > 0) break;
        token[wlen++] = *p;
        p++;
    }
    token[wlen] = '\0';
    
    return p - text;
}

// WordPiece tokenization with Unicode support
static int wordpiece_tokenize(onnx_embedder_t* e, const char* text, int64_t* token_ids, int max_tokens) {
    if (!text || !*text) {
        token_ids[0] = e->cls_id;
        token_ids[1] = e->sep_id;
        return 2;
    }
    
    int pos = 0;
    token_ids[pos++] = e->cls_id;
    
    const char* p = text;
    char word[256];
    
    while (*p && pos < max_tokens - 1) {
        int consumed = extract_next_token(p, word, sizeof(word));
        if (consumed == 0) break;
        p += consumed;
        
        if (word[0] == '\0') continue;  // Skipped punctuation
        
        // Convert to lowercase for ASCII letters only (preserve UTF-8)
        for (int i = 0; word[i]; i++) {
            if ((unsigned char)word[i] < 0x80) {
                word[i] = tolower((unsigned char)word[i]);
            }
        }
        
        int wlen = strlen(word);
        int sub_pos = 0;
        int is_first = 1;
        
        // WordPiece subword tokenization
        while (sub_pos < wlen && pos < max_tokens - 1) {
            int best_len = 0;
            int best_id = e->unk_id;
            
            int max_try = wlen - sub_pos;
            if (!is_first && max_try > MAX_TOKEN_LEN - 3) max_try = MAX_TOKEN_LEN - 3;
            if (is_first && max_try > MAX_TOKEN_LEN - 1) max_try = MAX_TOKEN_LEN - 1;
            
            for (int try_len = max_try; try_len > 0; try_len--) {
                char try_token[MAX_TOKEN_LEN];
                if (!is_first) {
                    try_token[0] = '#';
                    try_token[1] = '#';
                    memcpy(try_token + 2, word + sub_pos, try_len);
                    try_token[2 + try_len] = '\0';
                } else {
                    memcpy(try_token, word + sub_pos, try_len);
                    try_token[try_len] = '\0';
                }
                
                int id = vocab_lookup(e->vocab_hash, e->vocab_hash_size, try_token);
                if (id >= 0) {
                    best_len = try_len;
                    best_id = id;
                    break;
                }
            }
            
            if (best_len == 0) {
                token_ids[pos++] = e->unk_id;
                sub_pos++;
            } else {
                token_ids[pos++] = best_id;
                sub_pos += best_len;
            }
            is_first = 0;
        }
    }
    
    token_ids[pos++] = e->sep_id;
    return pos;
}

// Mean pooling + L2 normalization
static void mean_pool_and_normalize(float* output, int64_t* attention_mask, 
                                     int seq_len, int dim, float* result) {
    // Initialize result to zero
    memset(result, 0, sizeof(float) * dim);
    
    // Sum valid tokens (attention_mask == 1)
    int valid_count = 0;
    for (int i = 0; i < seq_len; i++) {
        if (attention_mask[i] == 1) {
            for (int j = 0; j < dim; j++) {
                result[j] += output[i * dim + j];
            }
            valid_count++;
        }
    }
    
    if (valid_count > 0) {
        // Mean
        for (int j = 0; j < dim; j++) {
            result[j] /= valid_count;
        }
    }
    
    // L2 normalize
    float norm = 0.0f;
    for (int j = 0; j < dim; j++) {
        norm += result[j] * result[j];
    }
    norm = sqrtf(norm);
    
    if (norm > 1e-6f) {
        for (int j = 0; j < dim; j++) {
            result[j] /= norm;
        }
    }
}

// ====== Public API ======

onnx_embedder_t* onnx_embedder_init(const char* model_path, const char* vocab_path,
                                     int max_seq_length, int dim) {
    if (init_ort_api() != 0) {
        return NULL;
    }
    
    onnx_embedder_t* e = calloc(1, sizeof(onnx_embedder_t));
    if (!e) {
        set_error("Failed to allocate embedder");
        return NULL;
    }
    
    e->max_seq_length = max_seq_length;
    e->dim = dim;
    
    // Detect tokenizer type from vocab file extension and path
    size_t vocab_len = strlen(vocab_path);
    int is_jina = (strstr(vocab_path, "jina") != NULL);
    
    if (vocab_len > 5 && strcmp(vocab_path + vocab_len - 5, ".json") == 0) {
        // BPE tokenizer (Jina / RoBERTa)
        e->tokenizer_type = TOKENIZER_BPE;
        e->output_name = "last_hidden_state";
        e->use_pooled_output = 0;
        e->bos_id = 0;   // <s>
        e->eos_id = 2;   // </s>
        e->unk_id = 3;   // <unk>
        e->pad_id = 1;   // <pad> = 1
        
        // Load vocab.json into memory
        FILE* f = fopen(vocab_path, "rb");
        if (!f) {
            set_error("Failed to open vocab.json");
            free(e);
            return NULL;
        }
        
        fseek(f, 0, SEEK_END);
        long vocab_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        char* vocab_data = malloc(vocab_size + 1);
        if (!vocab_data) {
            fclose(f);
            set_error("Failed to allocate vocab buffer");
            free(e);
            return NULL;
        }
        
        if (fread(vocab_data, 1, vocab_size, f) != (size_t)vocab_size) {
            free(vocab_data);
            fclose(f);
            set_error("Failed to read vocab.json");
            free(e);
            return NULL;
        }
        vocab_data[vocab_size] = '\0';
        fclose(f);
        
        // Derive merges path from vocab path (replace vocab.json with merges.txt)
        char merges_path[512];
        strncpy(merges_path, vocab_path, sizeof(merges_path) - 1);
        merges_path[sizeof(merges_path) - 1] = '\0';
        
        // Find last '/' or '\'
        char* last_slash = strrchr(merges_path, '/');
        char* last_backslash = strrchr(merges_path, '\\');
        char* base = (last_slash > last_backslash) ? last_slash : last_backslash;
        if (!base) base = merges_path;
        else base++;
        
        strcpy(base, "merges.txt");
        
        // Load merges.txt into memory
        f = fopen(merges_path, "rb");
        if (!f) {
            free(vocab_data);
            set_error("Failed to open merges.txt");
            free(e);
            return NULL;
        }
        
        fseek(f, 0, SEEK_END);
        long merges_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        
        char* merges_data = malloc(merges_size + 1);
        if (!merges_data) {
            free(vocab_data);
            fclose(f);
            set_error("Failed to allocate merges buffer");
            free(e);
            return NULL;
        }
        
        if (fread(merges_data, 1, merges_size, f) != (size_t)merges_size) {
            free(vocab_data);
            free(merges_data);
            fclose(f);
            set_error("Failed to read merges.txt");
            free(e);
            return NULL;
        }
        merges_data[merges_size] = '\0';
        fclose(f);
        
        // Create byte-level BPE tokenizer
        e->tokenizers_handle = byte_level_bpe_tokenizers_new_from_str(
            vocab_data, vocab_size,
            merges_data, merges_size,
            NULL, 0  // no added tokens
        );
        
        free(vocab_data);
        free(merges_data);
        
        if (!e->tokenizers_handle) {
            set_error("Failed to initialize tokenizers-cpp");
            free(e);
            return NULL;
        }
        
        printf("Loaded tokenizers-cpp from %s\n", vocab_path);
    } else {
        // WordPiece tokenizer (MPNet / BERT)
        e->tokenizer_type = TOKENIZER_WORDPIECE;
        e->output_name = "token_embeddings";
        e->use_pooled_output = 0;
        
        // Load vocab
        if (load_vocab(vocab_path, &e->vocab_hash, &e->vocab_hash_size, &e->vocab_size,
                       &e->unk_id, &e->cls_id, &e->sep_id, &e->pad_id) < 0) {
            free(e);
            return NULL;
        }
        
        printf("Loaded WordPiece vocab: %d tokens (UNK=%d, CLS=%d, SEP=%d, PAD=%d)\n",
               e->vocab_size, e->unk_id, e->cls_id, e->sep_id, e->pad_id);
    }
    
    // Allocate working buffers
    e->input_ids = calloc(max_seq_length, sizeof(int64_t));
    e->attention_mask = calloc(max_seq_length, sizeof(int64_t));
    
    if (!e->input_ids || !e->attention_mask) {
        set_error("Failed to allocate buffers");
        onnx_embedder_free(e);
        return NULL;
    }
    
    // Initialize ONNX Runtime
    OrtStatus* status;
    
    status = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "embedder", &e->env);
    if (status != NULL) {
        set_error("Failed to create ONNX env");
        g_ort->ReleaseStatus(status);
        onnx_embedder_free(e);
        return NULL;
    }
    
    OrtSessionOptions* session_options;
    status = g_ort->CreateSessionOptions(&session_options);
    if (status != NULL) {
        set_error("Failed to create session options");
        g_ort->ReleaseStatus(status);
        onnx_embedder_free(e);
        return NULL;
    }
    
    // 尝试添加 TensorRT Execution Provider (RTX 4090D - 24GB VRAM)
    // TensorRT 比 CUDA 更快（通常 2-5x）
    OrtTensorRTProviderOptions trt_options;
    memset(&trt_options, 0, sizeof(trt_options));
    trt_options.device_id = 0;
    trt_options.trt_max_partition_iterations = 1000;
    trt_options.trt_min_subgraph_size = 1;
    // RTX 4090D 有 24GB 显存，分配更多 workspace 提升性能
    trt_options.trt_max_workspace_size = 12ULL * 1024 * 1024 * 1024;  // 12GB workspace
    // Jina model requires FP32 precision (FP16 causes embedding quality issues)
    trt_options.trt_fp16_enable = is_jina ? 0 : 1;
    trt_options.trt_int8_enable = 0;
    trt_options.trt_engine_cache_enable = 1;  // 缓存 engine，下次启动更快
    // 使用绝对路径避免相对路径问题
    static char trt_cache_path[512];
    snprintf(trt_cache_path, sizeof(trt_cache_path), "%s/trt_cache", getenv("HOME") ? getenv("HOME") : "/tmp");
    trt_options.trt_engine_cache_path = trt_cache_path;
    trt_options.trt_dump_subgraphs = 0;
    
    status = g_ort->SessionOptionsAppendExecutionProvider_TensorRT(session_options, &trt_options);
    if (status != NULL) {
        // TensorRT 不可用，回退到 CUDA
        g_ort->ReleaseStatus(status);
        
        OrtCUDAProviderOptions cuda_options;
        memset(&cuda_options, 0, sizeof(cuda_options));
        cuda_options.device_id = 0;
        cuda_options.arena_extend_strategy = 0;
        // RTX 4090D: 24GB VRAM, allow up to 16GB for CUDA
        cuda_options.gpu_mem_limit = 16ULL * 1024 * 1024 * 1024;
        cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchHeuristic;
        cuda_options.do_copy_in_default_stream = 1;
        
        status = g_ort->SessionOptionsAppendExecutionProvider_CUDA(session_options, &cuda_options);
        if (status != NULL) {
            fprintf(stderr, "  [ERROR] CUDA not available, falling back to CPU. Check LD_LIBRARY_PATH for CUDA/cuDNN libraries.\n");
            g_ort->ReleaseStatus(status);
        } else {
            printf("  Using CUDA GPU acceleration\n");
        }
    } else {
        if (is_jina) {
            printf("  Using TensorRT GPU acceleration (FP32 for Jina)\n");
        } else {
            printf("  Using TensorRT GPU acceleration (FP16)\n");
        }
    }
    
    status = g_ort->CreateSession(e->env, model_path, session_options, &e->session);
    g_ort->ReleaseSessionOptions(session_options);
    
    if (status != NULL) {
        set_error("Failed to create ONNX session");
        g_ort->ReleaseStatus(status);
        onnx_embedder_free(e);
        return NULL;
    }
    
    status = g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &e->memory_info);
    if (status != NULL) {
        set_error("Failed to create memory info");
        g_ort->ReleaseStatus(status);
        onnx_embedder_free(e);
        return NULL;
    }
    
    printf("ONNX embedder initialized: model=%s, seq_len=%d, dim=%d\n",
           model_path, max_seq_length, dim);
    
    return e;
}

int onnx_embedder_encode(onnx_embedder_t* e, const char* text, float* vector) {
    if (!e || !text || !vector) {
        set_error("Invalid arguments");
        return -1;
    }
    
    // Safety: limit text length and ensure valid UTF-8
    size_t text_len = strlen(text);
    if (text_len > 4096) text_len = 4096;  // Hard limit to prevent tokenizer panic
    
    // Create a cleaned copy of the text: replace invalid UTF-8 sequences with spaces
    static __thread char clean_text[4096 + 4];
    size_t clean_len = 0;
    for (size_t i = 0; i < text_len && clean_len < 4096; ) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            // ASCII
            clean_text[clean_len++] = c;
            i++;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text_len && 
                   ((unsigned char)text[i+1] & 0xC0) == 0x80) {
            // Valid 2-byte UTF-8
            clean_text[clean_len++] = text[i++];
            clean_text[clean_len++] = text[i++];
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text_len &&
                   ((unsigned char)text[i+1] & 0xC0) == 0x80 &&
                   ((unsigned char)text[i+2] & 0xC0) == 0x80) {
            // Valid 3-byte UTF-8
            clean_text[clean_len++] = text[i++];
            clean_text[clean_len++] = text[i++];
            clean_text[clean_len++] = text[i++];
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text_len &&
                   ((unsigned char)text[i+1] & 0xC0) == 0x80 &&
                   ((unsigned char)text[i+2] & 0xC0) == 0x80 &&
                   ((unsigned char)text[i+3] & 0xC0) == 0x80) {
            // Valid 4-byte UTF-8
            clean_text[clean_len++] = text[i++];
            clean_text[clean_len++] = text[i++];
            clean_text[clean_len++] = text[i++];
            clean_text[clean_len++] = text[i++];
        } else {
            // Invalid byte - skip and replace with space
            if (clean_len == 0 || clean_text[clean_len-1] != ' ') {
                clean_text[clean_len++] = ' ';
            }
            i++;
        }
    }
    clean_text[clean_len] = '\0';
    
    if (clean_len == 0) {
        // Empty text after cleaning - return zero vector
        memset(vector, 0, sizeof(float) * e->dim);
        float norm = 0.0f;
        for (int j = 0; j < e->dim; j++) norm += vector[j] * vector[j];
        norm = sqrtf(norm);
        if (norm > 0) {
            for (int j = 0; j < e->dim; j++) vector[j] /= norm;
        }
        return 0;
    }
    
    // Tokenize
    int token_count;
    if (e->tokenizer_type == TOKENIZER_BPE) {
        TokenizerEncodeResult result;
        // Note: tokenizers-cpp byte_level_bpe does NOT add special tokens automatically
        // We need to add BOS/EOS manually
        tokenizers_encode(e->tokenizers_handle, clean_text, clean_len, 0, &result);
        
        token_count = (int)result.len + 2; // +2 for BOS and EOS
        if (token_count > e->max_seq_length) {
            token_count = e->max_seq_length;
        }
        
        // Add BOS token
        e->input_ids[0] = e->bos_id;
        
        // Copy tokens
        int max_tokens = token_count - 2;
        if (max_tokens > (int)result.len) max_tokens = (int)result.len;
        for (int i = 0; i < max_tokens; i++) {
            e->input_ids[i + 1] = result.token_ids[i];
        }
        
        // Add EOS token
        e->input_ids[token_count - 1] = e->eos_id;
        
        tokenizers_free_encode_results(&result, 1);
    } else {
        token_count = wordpiece_tokenize(e, text, e->input_ids, e->max_seq_length);
    }
    
    // Pad to max_seq_length and create attention mask
    for (int i = 0; i < e->max_seq_length; i++) {
        if (i < token_count) {
            e->attention_mask[i] = 1;
        } else {
            e->input_ids[i] = e->pad_id;
            e->attention_mask[i] = 0;
        }
    }
    
    // Create input tensors
    int64_t input_shape[] = {1, e->max_seq_length};
    OrtValue* input_tensor = NULL;
    OrtValue* mask_tensor = NULL;
    OrtStatus* status;
    
    status = g_ort->CreateTensorWithDataAsOrtValue(
        e->memory_info, e->input_ids, e->max_seq_length * sizeof(int64_t),
        input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensor);
    if (status != NULL) {
        set_error("Failed to create input tensor");
        g_ort->ReleaseStatus(status);
        return -1;
    }
    
    status = g_ort->CreateTensorWithDataAsOrtValue(
        e->memory_info, e->attention_mask, e->max_seq_length * sizeof(int64_t),
        input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &mask_tensor);
    if (status != NULL) {
        g_ort->ReleaseValue(input_tensor);
        set_error("Failed to create mask tensor");
        g_ort->ReleaseStatus(status);
        return -1;
    }
    
    // Run inference
    const char* input_names[] = {"input_ids", "attention_mask"};
    const char* output_names[] = {e->output_name};
    OrtValue* output_tensor = NULL;
    OrtValue* inputs[] = {input_tensor, mask_tensor};
    
    status = g_ort->Run(e->session, NULL, input_names, (const OrtValue* const*)inputs, 2, output_names, 1, &output_tensor);
    
    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseValue(mask_tensor);
    
    if (status != NULL) {
        set_error("ONNX inference failed");
        g_ort->ReleaseStatus(status);
        return -1;
    }
    
    // Get output data
    float* output_data;
    status = g_ort->GetTensorMutableData(output_tensor, (void**)&output_data);
    if (status != NULL) {
        g_ort->ReleaseValue(output_tensor);
        set_error("Failed to get output data");
        g_ort->ReleaseStatus(status);
        return -1;
    }
    
    if (e->use_pooled_output) {
        // Pooled output: directly copy and normalize
        memcpy(vector, output_data, sizeof(float) * e->dim);
        float norm = 0.0f;
        for (int j = 0; j < e->dim; j++) {
            norm += vector[j] * vector[j];
        }
        norm = sqrtf(norm);
        if (norm > 0) {
            for (int j = 0; j < e->dim; j++) {
                vector[j] /= norm;
            }
        }
    } else {
        // Mean pooling + normalize
        mean_pool_and_normalize(output_data, e->attention_mask, e->max_seq_length, e->dim, vector);
    }
    
    g_ort->ReleaseValue(output_tensor);
    return 0;
}

// Batch encode multiple texts at once for better GPU utilization
int onnx_embedder_encode_batch(onnx_embedder_t* e, const char** texts, int count, float* vectors) {
    if (!e || !texts || !vectors || count <= 0) {
        set_error("Invalid arguments");
        return -1;
    }
    
    // Allocate batch buffers
    int64_t* batch_input_ids = (int64_t*)malloc(count * e->max_seq_length * sizeof(int64_t));
    int64_t* batch_attention_mask = (int64_t*)malloc(count * e->max_seq_length * sizeof(int64_t));
    if (!batch_input_ids || !batch_attention_mask) {
        free(batch_input_ids);
        free(batch_attention_mask);
        set_error("Failed to allocate batch buffers");
        return -1;
    }
    
    // Tokenize all texts
    for (int i = 0; i < count; i++) {
        int64_t* ids = batch_input_ids + i * e->max_seq_length;
        int64_t* mask = batch_attention_mask + i * e->max_seq_length;
        
        int token_count;
        if (e->tokenizer_type == TOKENIZER_BPE) {
            TokenizerEncodeResult result;
            // Note: tokenizers-cpp byte_level_bpe does NOT add special tokens
            tokenizers_encode(e->tokenizers_handle, texts[i], strlen(texts[i]), 0, &result);
            
            token_count = (int)result.len + 2; // +2 for BOS and EOS
            if (token_count > e->max_seq_length) {
                token_count = e->max_seq_length;
            }
            
            // Add BOS token
            ids[0] = e->bos_id;
            
            // Copy tokens
            int max_tokens = token_count - 2;
            if (max_tokens > (int)result.len) max_tokens = (int)result.len;
            for (int j = 0; j < max_tokens; j++) {
                ids[j + 1] = result.token_ids[j];
            }
            
            // Add EOS token
            ids[token_count - 1] = e->eos_id;
            
            tokenizers_free_encode_results(&result, 1);
        } else {
            token_count = wordpiece_tokenize(e, texts[i], ids, e->max_seq_length);
        }
        
        // Pad and create attention mask
        for (int j = 0; j < e->max_seq_length; j++) {
            if (j < token_count) {
                mask[j] = 1;
            } else {
                ids[j] = e->pad_id;
                mask[j] = 0;
            }
        }
    }
    
    // Create input tensors [count, max_seq_length]
    int64_t input_shape[] = {count, e->max_seq_length};
    OrtValue* input_tensor = NULL;
    OrtValue* mask_tensor = NULL;
    OrtStatus* status;
    
    status = g_ort->CreateTensorWithDataAsOrtValue(
        e->memory_info, batch_input_ids, count * e->max_seq_length * sizeof(int64_t),
        input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &input_tensor);
    if (status != NULL) {
        set_error("Failed to create batch input tensor");
        g_ort->ReleaseStatus(status);
        free(batch_input_ids);
        free(batch_attention_mask);
        return -1;
    }
    
    status = g_ort->CreateTensorWithDataAsOrtValue(
        e->memory_info, batch_attention_mask, count * e->max_seq_length * sizeof(int64_t),
        input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &mask_tensor);
    if (status != NULL) {
        g_ort->ReleaseValue(input_tensor);
        set_error("Failed to create batch mask tensor");
        g_ort->ReleaseStatus(status);
        free(batch_input_ids);
        free(batch_attention_mask);
        return -1;
    }
    
    // Run inference
    const char* input_names[] = {"input_ids", "attention_mask"};
    const char* output_names[] = {e->output_name};
    OrtValue* output_tensor = NULL;
    OrtValue* inputs[] = {input_tensor, mask_tensor};
    
    status = g_ort->Run(e->session, NULL, input_names, (const OrtValue* const*)inputs, 2, output_names, 1, &output_tensor);
    
    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseValue(mask_tensor);
    
    if (status != NULL) {
        free(batch_input_ids);
        free(batch_attention_mask);
        set_error("ONNX batch inference failed");
        g_ort->ReleaseStatus(status);
        return -1;
    }
    
    // Get output data
    float* output_data;
    status = g_ort->GetTensorMutableData(output_tensor, (void**)&output_data);
    if (status != NULL) {
        free(batch_input_ids);
        free(batch_attention_mask);
        g_ort->ReleaseValue(output_tensor);
        set_error("Failed to get batch output data");
        g_ort->ReleaseStatus(status);
        return -1;
    }
    
    // Extract vectors for each sample
    if (e->use_pooled_output) {
        // Pooled output shape: [count, dim]
        for (int i = 0; i < count; i++) {
            float* out = output_data + i * e->dim;
            memcpy(vectors + i * e->dim, out, sizeof(float) * e->dim);
            float norm = 0.0f;
            for (int j = 0; j < e->dim; j++) {
                norm += vectors[i * e->dim + j] * vectors[i * e->dim + j];
            }
            norm = sqrtf(norm);
            if (norm > 0) {
                for (int j = 0; j < e->dim; j++) {
                    vectors[i * e->dim + j] /= norm;
                }
            }
        }
    } else {
        // Output shape: [count, seq_length, dim]
        for (int i = 0; i < count; i++) {
            float* out = output_data + i * e->max_seq_length * e->dim;
            int64_t* mask = batch_attention_mask + i * e->max_seq_length;
            
            mean_pool_and_normalize(out, mask, e->max_seq_length, e->dim, vectors + i * e->dim);
        }
    }
    
    free(batch_input_ids);
    free(batch_attention_mask);
    g_ort->ReleaseValue(output_tensor);
    return 0;
}

void onnx_embedder_free(onnx_embedder_t* e) {
    if (!e) return;
    
    if (e->memory_info) g_ort->ReleaseMemoryInfo(e->memory_info);
    if (e->session) g_ort->ReleaseSession(e->session);
    if (e->env) g_ort->ReleaseEnv(e->env);
    
    if (e->tokenizer_type == TOKENIZER_BPE) {
        tokenizers_free(e->tokenizers_handle);
    } else {
        free(e->vocab_hash);
    }
    
    free(e->input_ids);
    free(e->attention_mask);
    free(e);
}
