#include "onnx_embedder.h"
#include <onnxruntime_c_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define MAX_VOCAB_SIZE 50000
#define MAX_TOKEN_LEN 64

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
    vocab_hash_entry_t* vocab_hash;
    int vocab_hash_size;
    int vocab_size;
    int max_seq_length;
    int dim;
    int unk_id;
    int cls_id;
    int sep_id;
    int pad_id;
    
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
static int init_ort_api(void) {
    if (g_ort) return 0;
    
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

// Unicode character classification
static int is_cjk(char c) {
    // CJK Unified Ideographs: 0x4E00-0x9FFF
    // CJK Unified Ideographs Extension A: 0x3400-0x4DBF
    unsigned char u = (unsigned char)c;
    return (u >= 0x4E && u <= 0x9F) || (u >= 0x34 && u <= 0x4D);
}

static int is_punctuation(char c) {
    return c == '。' || c == '，' || c == '、' || c == '；' || c == '：' ||
           c == '？' || c == '！' || c == '"' || c == '"' || c == ''' || c == ''' ||
           c == '（' || c == '）' || c == '【' || c == '】' || c == '《' || c == '》' ||
           c == '…' || c == '—' || c == '～' || c == '·' ||
           c == '.' || c == ',' || c == '!' || c == '?' || c == ';' || c == ':' ||
           c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '"' || c == '\'' || c == '`' || c == '-' || c == '_' ||
           c == '+' || c == '=' || c == '*' || c == '/' || c == '\\' ||
           c == '<' || c == '>' || c == '|' || c == '&' || c == '%' || c == '$' ||
           c == '#' || c == '@' || c == '^' || c == '~';
}

// Extract next token from text
// Returns bytes consumed, fills token buffer
static int extract_next_token(const char* text, char* token, int max_len) {
    if (!text || !*text) return 0;
    
    const char* p = text;
    
    // Skip whitespace
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return p - text;
    
    char c = *p;
    
    // CJK character: extract one character
    if (is_cjk(c)) {
        token[0] = c;
        token[1] = '\0';
        return p - text + 1;
    }
    
    // Punctuation: skip it (don't tokenize)
    if (is_punctuation(c)) {
        return p - text + 1;  // Skip and return 0 tokens
    }
    
    // ASCII word: extract until whitespace/punctuation/CJK
    int wlen = 0;
    while (*p && wlen < max_len - 1) {
        char ch = *p;
        if (isspace((unsigned char)ch) || is_punctuation(ch) || is_cjk(ch)) {
            break;
        }
        token[wlen++] = ch;
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
        
        // Convert to lowercase for ASCII letters
        for (int i = 0; word[i]; i++) {
            word[i] = tolower((unsigned char)word[i]);
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
    e->unk_id = 100;
    e->cls_id = 101;
    e->sep_id = 102;
    e->pad_id = 0;
    
    // Load vocab
    if (load_vocab(vocab_path, &e->vocab_hash, &e->vocab_hash_size, &e->vocab_size,
                   &e->unk_id, &e->cls_id, &e->sep_id, &e->pad_id) < 0) {
        free(e);
        return NULL;
    }
    
    printf("Loaded vocab: %d tokens (UNK=%d, CLS=%d, SEP=%d, PAD=%d)\n",
           e->vocab_size, e->unk_id, e->cls_id, e->sep_id, e->pad_id);
    
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
        onnx_embedder_free(e);
        return NULL;
    }
    
    OrtSessionOptions* session_options;
    status = g_ort->CreateSessionOptions(&session_options);
    if (status != NULL) {
        set_error("Failed to create session options");
        onnx_embedder_free(e);
        return NULL;
    }
    
    status = g_ort->CreateSession(e->env, model_path, session_options, &e->session);
    g_ort->ReleaseSessionOptions(session_options);
    
    if (status != NULL) {
        set_error("Failed to create ONNX session");
        onnx_embedder_free(e);
        return NULL;
    }
    
    status = g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &e->memory_info);
    if (status != NULL) {
        set_error("Failed to create memory info");
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
    
    // Tokenize
    int token_count = wordpiece_tokenize(e, text, e->input_ids, e->max_seq_length);
    
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
        return -1;
    }
    
    status = g_ort->CreateTensorWithDataAsOrtValue(
        e->memory_info, e->attention_mask, e->max_seq_length * sizeof(int64_t),
        input_shape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &mask_tensor);
    if (status != NULL) {
        g_ort->ReleaseValue(input_tensor);
        set_error("Failed to create mask tensor");
        return -1;
    }
    
    // Run inference
    const char* input_names[] = {"input_ids", "attention_mask"};
    const char* output_names[] = {"token_embeddings"};
    OrtValue* output_tensor = NULL;
    OrtValue* inputs[] = {input_tensor, mask_tensor};
    
    status = g_ort->Run(e->session, NULL, input_names, (const OrtValue* const*)inputs, 2, output_names, 1, &output_tensor);
    
    g_ort->ReleaseValue(input_tensor);
    g_ort->ReleaseValue(mask_tensor);
    
    if (status != NULL) {
        set_error("ONNX inference failed");
        return -1;
    }
    
    // Get output data
    float* output_data;
    status = g_ort->GetTensorMutableData(output_tensor, (void**)&output_data);
    if (status != NULL) {
        g_ort->ReleaseValue(output_tensor);
        set_error("Failed to get output data");
        return -1;
    }
    
    // Mean pooling + normalize
    mean_pool_and_normalize(output_data, e->attention_mask, e->max_seq_length, e->dim, vector);
    
    g_ort->ReleaseValue(output_tensor);
    return 0;
}

void onnx_embedder_free(onnx_embedder_t* e) {
    if (!e) return;
    
    if (e->memory_info) g_ort->ReleaseMemoryInfo(e->memory_info);
    if (e->session) g_ort->ReleaseSession(e->session);
    if (e->env) g_ort->ReleaseEnv(e->env);
    
    free(e->vocab_hash);
    free(e->input_ids);
    free(e->attention_mask);
    free(e);
}
