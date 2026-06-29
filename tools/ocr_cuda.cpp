/**
 * Pure C++ OCR binary using Unlimited-OCR model.
 * No Python dependency at runtime.
 *
 * Links against:
 *   - libtorch (for ML inference)
 *   - libmupdf (for PDF page rendering)
 *
 * Compile with -D_GLIBCXX_USE_CXX11_ABI=0 to match libtorch ABI.
 *
 * Usage:
 *   ./ocr_cuda <pdf_path> [--dpi 200]
 *
 * Output (stdout):
 *   ---OCR_CHAPTERS:{json}---
 *   OCR text content
 */

#include <torch/torch.h>
#include <torch/script.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <cstdint>

// MuPDF (header already has extern "C" guards)
#include <mupdf/fitz.h>

// ─── Configuration ───
static constexpr int64_t HIDDEN_SIZE = 1280;
static constexpr int64_t NUM_LAYERS = 12;
static constexpr int64_t NUM_HEADS = 10;
static constexpr int64_t NUM_KV_HEADS = 10;
static constexpr int64_t HEAD_DIM = 128;
static constexpr int64_t MOE_INTERMEDIATE = 896;
static constexpr int64_t NUM_EXPERTS = 64;
static constexpr int64_t TOP_K = 6;
static constexpr int64_t NUM_SHARED = 2;
static constexpr int64_t VOCAB_SIZE = 129280;
static constexpr double RMS_EPS = 1e-6;
static constexpr double ROPE_THETA = 10000.0;
static constexpr int64_t EOS_ID = 1;
static constexpr int64_t IMAGE_TOKEN_ID = 128815;
static constexpr int IMAGE_SIZE = 640;
static constexpr int NUM_QUERIES = 10;
static constexpr int64_t IMG_TOKENS_PER_IMAGE = (NUM_QUERIES + 1) * NUM_QUERIES + 1; // 111

// TensorPack format constants
static constexpr uint32_t TP_MAGIC = 0x5052434f; // "OCRP" little-endian
static constexpr uint32_t TP_VERSION = 1;

// ─── Dtype codes ───
enum TP_Dtype : uint32_t {
    TP_F32 = 0, TP_F64 = 1, TP_I32 = 2, TP_I64 = 3,
    TP_BF16 = 4, TP_U8 = 5, TP_I8 = 6, TP_F16 = 7
};

// ─── Weights ───
struct LayerW {
    torch::Tensor input_norm, post_norm;
    torch::Tensor qw, kw, vw, ow;
    bool is_moe = false;
    // Dense MLP
    torch::Tensor mlp_gate, mlp_up, mlp_down;
    // MoE
    torch::Tensor gate_w; // router
    std::vector<torch::Tensor> expert_gate, expert_up, expert_down;
    torch::Tensor sh_gate, sh_up, sh_down;
};

struct Weights {
    torch::Tensor embed_w, head_w;
    torch::Tensor final_norm_w;  // model.model.norm.weight
    std::vector<LayerW> layers;
};

struct Vocab {
    std::vector<std::string> table;
    Vocab() : table(VOCAB_SIZE) {}
    void load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "[FATAL] Cannot open vocab: %s\n", path.c_str()); exit(1); }
        for (int i = 0; i < VOCAB_SIZE; i++) {
            uint32_t len; f.read((char*)&len, 4);
            if (len > 0) { std::string s(len, '\0'); f.read(&s[0], len); table[i] = s; }
        }
    }
    std::string decode(const std::vector<int64_t>& ids) {
        std::string out;
        for (auto id : ids) {
            if (id >= 0 && id < (int64_t)VOCAB_SIZE && !table[id].empty()) out += table[id];
        }
        // Replace BPE space marker Ġ (UTF-8: 0xC4 0xA0)
        size_t p;
        while ((p = out.find("\xc4\xa0")) != std::string::npos) out.replace(p, 2, " ");
        return out;
    }
};

// ─── TensorPack loader ───
static std::unordered_map<std::string, torch::Tensor> load_tensorpack(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { fprintf(stderr, "[FATAL] Cannot open %s\n", path.c_str()); exit(1); }
    
    // Read header
    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || *(uint32_t*)magic != TP_MAGIC) {
        fprintf(stderr, "[FATAL] Invalid TensorPack magic\n"); exit(1);
    }
    uint32_t version;
    if (fread(&version, 4, 1, f) != 1 || version != TP_VERSION) {
        fprintf(stderr, "[FATAL] Unsupported TensorPack version\n"); exit(1);
    }
    uint32_t num_tensors;
    if (fread(&num_tensors, 4, 1, f) != 1) {
        fprintf(stderr, "[FATAL] Failed to read tensor count\n"); exit(1);
    }
    
    // Helper: checked fread
    auto must_read = [](void* buf, size_t size, size_t count, FILE* f) {
        if (fread(buf, size, count, f) != count) {
            perror("[FATAL] fread failed");
            exit(1);
        }
    };
    
    std::unordered_map<std::string, torch::Tensor> result;
    result.reserve(num_tensors);
    
    for (uint32_t i = 0; i < num_tensors; i++) {
        // Read name
        uint32_t name_len;
        must_read(&name_len, 4, 1, f);
        std::string name(name_len, '\0');
        must_read(&name[0], 1, name_len, f);
        
        // Read shape
        uint32_t ndim;
        must_read(&ndim, 4, 1, f);
        std::vector<int64_t> shape(ndim);
        for (uint32_t d = 0; d < ndim; d++) {
            int64_t dim_val;
            must_read(&dim_val, 8, 1, f);
            shape[d] = dim_val;
        }
        
        // Read dtype
        uint32_t dtype_code;
        must_read(&dtype_code, 4, 1, f);
        
        // Map dtype
        torch::ScalarType st;
        uint32_t elem_size;
        switch (dtype_code) {
            case TP_F32:  st = torch::kFloat32; elem_size = 4; break;
            case TP_F64:  st = torch::kFloat64; elem_size = 8; break;
            case TP_I32:  st = torch::kInt32;   elem_size = 4; break;
            case TP_I64:  st = torch::kInt64;   elem_size = 8; break;
            case TP_BF16: st = torch::kBFloat16; elem_size = 2; break;
            case TP_U8:   st = torch::kUInt8;   elem_size = 1; break;
            case TP_I8:   st = torch::kInt8;    elem_size = 1; break;
            case TP_F16:  st = torch::kFloat16; elem_size = 2; break;
            default:
                fprintf(stderr, "[FATAL] Unknown dtype %u\n", dtype_code);
                exit(1);
        }
        
        // Read data size
        uint64_t data_size;
        must_read(&data_size, 8, 1, f);
        
        // Check consistency
        int64_t expected = 1;
        for (auto s : shape) expected *= s;
        expected *= elem_size;
        if ((uint64_t)expected != data_size) {
            fprintf(stderr, "[FATAL] Size mismatch for '%s': expected %ld, got %lu\n",
                    name.c_str(), expected, data_size);
            exit(1);
        }
        
        // Read data
        int64_t numel = 1;
        for (auto s : shape) numel *= s;
        
        auto tensor = torch::empty(shape, torch::TensorOptions().dtype(st));
        must_read(tensor.data_ptr(), 1, data_size, f);
        
        result[name] = tensor;
        
        if ((i + 1) % 100 == 0 || i + 1 == num_tensors) {
            fprintf(stderr, "\r  [OCR] Loading tensors %u/%u", i + 1, num_tensors);
        }
    }
    fclose(f);
    fprintf(stderr, "\n");
    return result;
}

// ─── Load weights ───
static Weights load_weights(const std::string& dir) {
    auto data = load_tensorpack(dir + "/weights.tpack");
    fprintf(stderr, "[OCR] Loaded %zu tensors\n", data.size());
    
    auto gt = [&](const std::string& k) -> torch::Tensor {
        auto it = data.find(k);
        if (it == data.end()) { fprintf(stderr, "[FATAL] Missing: %s\n", k.c_str()); exit(1); }
        return it->second;
    };
    
    Weights w;
    w.embed_w = gt("embed_tokens.weight");
    w.head_w = gt("lm_head.weight");
    w.final_norm_w = gt("model.norm.weight");
    w.layers.resize(NUM_LAYERS);
    
    for (int i = 0; i < NUM_LAYERS; i++) {
        auto& l = w.layers[i];
        auto p = "layers." + std::to_string(i) + ".";
        l.input_norm = gt(p + "input_layernorm.weight");
        l.post_norm = gt(p + "post_attention_layernorm.weight");
        l.qw = gt(p + "self_attn.q_proj.weight");
        l.kw = gt(p + "self_attn.k_proj.weight");
        l.vw = gt(p + "self_attn.v_proj.weight");
        l.ow = gt(p + "self_attn.o_proj.weight");
        
        std::string gk = p + "moe.gate.weight";
        if (data.find(gk) != data.end()) {
            l.is_moe = true;
            l.gate_w = gt(gk);
            l.expert_gate.resize(NUM_EXPERTS);
            l.expert_up.resize(NUM_EXPERTS);
            l.expert_down.resize(NUM_EXPERTS);
            for (int e = 0; e < NUM_EXPERTS; e++) {
                auto ep = p + "moe.experts." + std::to_string(e) + ".";
                l.expert_gate[e] = gt(ep + "gate_proj.weight");
                l.expert_up[e] = gt(ep + "up_proj.weight");
                l.expert_down[e] = gt(ep + "down_proj.weight");
            }
            l.sh_gate = gt(p + "moe.shared_experts.gate_proj.weight");
            l.sh_up = gt(p + "moe.shared_experts.up_proj.weight");
            l.sh_down = gt(p + "moe.shared_experts.down_proj.weight");
        } else {
            l.is_moe = false;
            l.mlp_gate = gt(p + "mlp.gate_proj.weight");
            l.mlp_up = gt(p + "mlp.up_proj.weight");
            l.mlp_down = gt(p + "mlp.down_proj.weight");
        }
    }
    return w;
}

// ─── Tensor ops ───
static torch::Tensor rms_norm(const torch::Tensor& x, const torch::Tensor& w, double eps) {
    auto x32 = x.to(torch::kFloat32);
    auto var = x32.pow(2).mean(-1, true);
    return (w.to(torch::kFloat32) * (x32 * torch::rsqrt(var + eps))).to(x.scalar_type());
}

static torch::Tensor precompute_cos_sin(int64_t max_len, int64_t dim) {
    // inv_freq[i] = 1.0 / (ROPE_THETA ** (i / dim)) for i in {0, 2, 4, ..., dim-2}
    auto ids = torch::arange(0, dim, 2, torch::kFloat32); // [dim/2]
    auto inv_freq = 1.0 / torch::pow(ROPE_THETA, ids / (double)dim);
    auto t = torch::arange(max_len, torch::kFloat32).reshape({max_len, 1});
    auto angles = t * inv_freq; // [max_len, dim/2]
    auto freqs = torch::cat({angles.cos(), angles.sin()}, -1); // [max_len, dim]
    return freqs;
}

static void apply_rope(torch::Tensor& q, torch::Tensor& k,
                        const torch::Tensor& cs, int64_t offset) {
    int64_t D = q.size(-1), H = D / 2;
    int64_t S = q.size(-2);
    auto cos = cs.slice(0, offset, offset + S).slice(1, 0, H).reshape({1, 1, S, H});
    auto sin = cs.slice(0, offset, offset + S).slice(1, H, D).reshape({1, 1, S, H});
    auto q1 = q.slice(-1, 0, H), q2 = q.slice(-1, H, D);
    auto k1 = k.slice(-1, 0, H), k2 = k.slice(-1, H, D);
    q.copy_(torch::cat({q1 * cos - q2 * sin, q1 * sin + q2 * cos}, -1));
    k.copy_(torch::cat({k1 * cos - k2 * sin, k1 * sin + k2 * cos}, -1));
}

// ─── Attention forward (with KV cache append) ───
static std::tuple<torch::Tensor, torch::Tensor, torch::Tensor>
attn_forward(const torch::Tensor& h, const LayerW& w,
             const torch::Tensor& cs, const torch::Tensor& k_cache,
             const torch::Tensor& v_cache, int64_t pos, bool prefill) {
    int64_t B = h.size(0), Q = h.size(1);
    
    auto q = torch::nn::functional::linear(h, w.qw)
                .reshape({B, Q, NUM_HEADS, HEAD_DIM}).transpose(1, 2);
    auto k = torch::nn::functional::linear(h, w.kw)
                .reshape({B, Q, NUM_KV_HEADS, HEAD_DIM}).transpose(1, 2);
    auto v = torch::nn::functional::linear(h, w.vw)
                .reshape({B, Q, NUM_KV_HEADS, HEAD_DIM}).transpose(1, 2);
    
    apply_rope(q, k, cs, pos);
    
    torch::Tensor nk, nv;
    if (prefill || k_cache.numel() == 0) {
        nk = k; nv = v;
    } else {
        nk = torch::cat({k_cache, k}, -2);
        nv = torch::cat({v_cache, v}, -2);
    }
    
    int64_t T = nk.size(-2);
    double scale = 1.0 / std::sqrt((double)HEAD_DIM);
    auto attn = torch::matmul(q, nk.transpose(-2, -1)) * scale;
    
    if (Q > 1 && T > Q) {
        // Prefill: causal mask, last Q rows of T-length lower triangular
        auto full_mask = torch::tril(torch::ones({T, T}, torch::kFloat32).to(h.device()));
        auto mask = full_mask.slice(0, T-Q, T);
        attn = attn + (1.0 - mask).to(h.dtype()) * -1e10;
    } else if (Q > 1) {
        // Q == T (first prefill): lower triangular
        auto mask = torch::tril(torch::ones({Q, T}, torch::kFloat32).to(h.device()));
        attn = attn + (1.0 - mask).to(h.dtype()) * -1e10;
    }
    // When Q == 1 (single-token decode), no causal mask needed:
    // the new token attends to all positions in the KV cache
    
    auto aw = torch::softmax(attn.to(torch::kFloat32), -1).to(h.dtype());
    auto out = torch::matmul(aw, nv).transpose(1, 2).reshape({B, Q, HIDDEN_SIZE});
    out = torch::nn::functional::linear(out, w.ow);
    return {out, nk, nv};
}

// ─── MoE forward ───
static torch::Tensor moe_forward(const torch::Tensor& x, const LayerW& w) {
    auto sizes = x.sizes();
    auto flat = x.reshape({-1, HIDDEN_SIZE});
    int64_t T = flat.size(0);
    
    // Routing in float32 (matching Python: hidden_states.type(torch.float32) on the gate input)
    auto flat32 = flat.to(torch::kFloat32);
    auto logits = torch::nn::functional::linear(flat32, w.gate_w.to(torch::kFloat32));
    auto weights = torch::softmax(logits, -1); // float32
    auto topk = torch::topk(weights, TOP_K, -1);
    auto topw = std::get<0>(topk);  // raw softmax scores, NOT normalized (norm_topk_prob=False)
    auto topi = std::get<1>(topk);  // [T, K]
    
    // Accumulate in float32, then convert back to match Python:
    //   new_x.view(*topk_ids.shape, -1).type(topk_weight.dtype).mul_(topk_weight.unsqueeze(dim=-1)).sum(dim=1)
    auto result = torch::zeros({T, HIDDEN_SIZE},
        torch::TensorOptions().dtype(torch::kFloat32).device(x.device()));
    for (int e = 0; e < NUM_EXPERTS; e++) {
        auto mask = (topi == e);
        auto idx = mask.nonzero(); // [S, 2] (token_idx, k_idx)
        if (idx.size(0) == 0) continue;
        auto ti = idx.select(1, 0);
        auto ki = idx.select(1, 1);
        
        auto inp = flat.index_select(0, ti);  // keep bf16 for the expert compute
        auto wgt = topw.index({ti, ki}).unsqueeze(-1); // float32
        
        auto gate = torch::nn::functional::linear(inp, w.expert_gate[e]);
        auto up = torch::nn::functional::linear(inp, w.expert_up[e]);
        auto hidden = torch::nn::functional::silu(gate) * up;
        auto out = torch::nn::functional::linear(hidden, w.expert_down[e]).to(torch::kFloat32);
        result.index_add_(0, ti, out * wgt);
    }
    
    if (w.sh_gate.numel() > 0) {
        auto g = torch::nn::functional::linear(flat, w.sh_gate);
        auto u = torch::nn::functional::linear(flat, w.sh_up);
        auto so = torch::nn::functional::silu(g) * u;
        so = torch::nn::functional::linear(so, w.sh_down).to(torch::kFloat32);
        result = result + so;
    }
    return result.to(x.scalar_type()).reshape(sizes);
}



// ─── Dense MLP ───
static torch::Tensor mlp_forward(const torch::Tensor& x, const LayerW& w) {
    auto g = torch::nn::functional::linear(x, w.mlp_gate);
    auto u = torch::nn::functional::linear(x, w.mlp_up);
    auto h = torch::nn::functional::silu(g) * u;
    return torch::nn::functional::linear(h, w.mlp_down);
}

// ─── Decoder layer ───
static std::tuple<torch::Tensor, torch::Tensor, torch::Tensor>
layer_forward(const torch::Tensor& x, const LayerW& w,
              const torch::Tensor& cs, const torch::Tensor& kc,
              const torch::Tensor& vc, int64_t pos, bool prefill) {
    auto res = x;
    auto h = rms_norm(x, w.input_norm, RMS_EPS);
    auto [ao, nk, nv] = attn_forward(h, w, cs, kc, vc, pos, prefill);
    h = res + ao;
    
    res = h;
    h = rms_norm(h, w.post_norm, RMS_EPS);
    h = w.is_moe ? moe_forward(h, w) : mlp_forward(h, w);
    h = res + h;
    return {h, nk, nv};
}



// ─── Tensor shape to string (for logging) ───
static std::string shape_str(const torch::Tensor& t) {
    std::ostringstream os;
    os << "[";
    for (int64_t i = 0; i < t.dim(); i++) {
        if (i > 0) os << ", ";
        os << t.size(i);
    }
    os << "]";
    return os.str();
}

// ─── Main OCR pipeline ───
static void run_ocr(const std::string& pdf_path, int dpi,
                     Weights& w, Vocab& vocab,
                     torch::jit::Module& vpipe,
                     const torch::Tensor& cos_sin) {
    auto dtype = torch::kBFloat16;
    
    // Move weights to GPU
    fprintf(stderr, "[OCR] Moving weights to GPU...\n");
    auto to_gpu = [&](torch::Tensor& t) { if (!t.is_cuda()) t = t.cuda().to(dtype); };
    to_gpu(w.embed_w); to_gpu(w.head_w); to_gpu(w.final_norm_w);
    for (auto& l : w.layers) {
        to_gpu(l.input_norm); to_gpu(l.post_norm);
        to_gpu(l.qw); to_gpu(l.kw); to_gpu(l.vw); to_gpu(l.ow);
        if (l.is_moe) {
            to_gpu(l.gate_w);
            for (auto& t : l.expert_gate) to_gpu(t);
            for (auto& t : l.expert_up) to_gpu(t);
            for (auto& t : l.expert_down) to_gpu(t);
            to_gpu(l.sh_gate); to_gpu(l.sh_up); to_gpu(l.sh_down);
        } else {
            to_gpu(l.mlp_gate); to_gpu(l.mlp_up); to_gpu(l.mlp_down);
        }
    }
    
    // Open PDF
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) { fprintf(stderr, "[FATAL] Cannot create MuPDF context\n"); exit(1); }
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_try(ctx) { doc = fz_open_document(ctx, pdf_path.c_str()); }
    fz_catch(ctx) { fprintf(stderr, "[FATAL] Cannot open PDF: %s\n", pdf_path.c_str()); exit(1); }
    int npages = fz_count_pages(ctx, doc);
    fprintf(stderr, "[OCR] PDF: %d pages\n", npages);
    
    // Render pages
    float zoom = (float)dpi / 72.0f;
    fz_matrix transform = fz_scale(zoom, zoom);
    
    std::vector<torch::Tensor> page_tensors;
    for (int p = 0; p < npages; p++) {
        // Render page to pixmap
        fz_pixmap* pix = nullptr;
        fz_try(ctx) {
            pix = fz_new_pixmap_from_page_number(ctx, doc, p, transform, fz_device_rgb(ctx), 0);
        }
        fz_catch(ctx) { continue; }
        if (!pix) continue;
        
        int pw = fz_pixmap_width(ctx, pix);
        int ph = fz_pixmap_height(ctx, pix);
        int stride = fz_pixmap_stride(ctx, pix);
        unsigned char* samp = fz_pixmap_samples(ctx, pix);
        int ch = pix->n;  // number of color channels; older API: fz_pixmap_n
        
        // Convert to tensor [3, H, W] uint8
        auto img = torch::empty({ph, pw, 3}, torch::kUInt8);
        auto acc = img.accessor<uint8_t, 3>();
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++)
                for (int c = 0; c < 3; c++)
                    acc[y][x][c] = samp[y * stride + x * ch + c];
        
        fz_drop_pixmap(ctx, pix);
        
        // Preprocess: resize + normalize
        auto fimg = img.to(torch::kFloat32).div(255.0).permute({2, 0, 1}).unsqueeze(0);
        fimg = torch::nn::functional::interpolate(fimg,
            torch::nn::functional::InterpolateFuncOptions()
                .size(std::vector<int64_t>({IMAGE_SIZE, IMAGE_SIZE}))
                .mode(torch::kNearest));
        fimg = (fimg - 0.5) / 0.5; // normalize to [-1, 1]
        page_tensors.push_back(fimg.squeeze(0).to(dtype).cuda());
        fprintf(stderr, "\r  [OCR] Page %d/%d rendered (%.1f MP)", p+1, npages, (double)pw*ph/1e6);
    }
    fprintf(stderr, "\n");
    
    if (page_tensors.empty()) {
        fprintf(stderr, "[ERROR] No pages rendered\n");
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
        return;
    }
    
    // Process images one at a time through vision pipeline
    // (traced with batch=1, so we need to loop)
    fprintf(stderr, "[OCR] Running vision pipeline on %d images...\n", (int)page_tensors.size());
    std::vector<torch::Tensor> all_features;
    for (size_t i = 0; i < page_tensors.size(); i++) {
        auto single_img = page_tensors[i].unsqueeze(0); // [1, 3, 640, 640]
        torch::Tensor feats;
        { torch::NoGradGuard ng;
            feats = vpipe.forward({single_img}).toTensor();
        }
        all_features.push_back(feats);
        fprintf(stderr, "\r  [OCR] Vision %zu/%zu", i+1, page_tensors.size());
    }
    fprintf(stderr, "\n");
    auto img_features = torch::cat(all_features, 0);
    fprintf(stderr, "[OCR] Vision features shape: %s\n", shape_str(img_features).c_str());
    {
        double v_mean = img_features.mean().item().toDouble();
        double v_std = img_features.std().item().toDouble();
        fprintf(stderr, "[OCR] Vision features mean=%.4f std=%.4f\n", v_mean, v_std);
    }
    
    // Build input tokens
    std::vector<int64_t> ids;
    std::vector<uint8_t> mask; // using uint8 for bool vector (C++ compatibility)
    ids.push_back(0); mask.push_back(0); // bos
    for (int i = 0; i < npages; i++) {
        for (int j = 0; j < IMG_TOKENS_PER_IMAGE; j++) {
            ids.push_back(IMAGE_TOKEN_ID); mask.push_back(1);
        }
    }
    // "Multi page parsing." tokens
    std::vector<int64_t> after = {37460, 4366, 76466, 16};
    ids.insert(ids.end(), after.begin(), after.end());
    mask.insert(mask.end(), after.size(), 0);
    
    auto ids_t = torch::tensor(ids, torch::TensorOptions().dtype(torch::kLong).device(torch::kCUDA)).unsqueeze(0);
    auto mask_t = torch::tensor(mask, torch::TensorOptions().dtype(torch::kBool).device(torch::kCUDA)).unsqueeze(0);
    
    // Embed tokens
    torch::Tensor embed;
    { torch::NoGradGuard ng;
        embed = torch::nn::functional::embedding(ids_t, w.embed_w);
    }
    
    // Insert image features into the embedding at image token positions
    auto seq_mask = mask_t.squeeze(0); // [S] bool
    int n_img = seq_mask.sum().item<int>();
    fprintf(stderr, "[OCR] Image tokens in seq: %d, img_features rows: %d\n",
            n_img, (int)img_features.size(0));
    embed[0].masked_scatter_(seq_mask.unsqueeze(-1), img_features);
    fprintf(stderr, "[OCR] Embed shape: %s\n", shape_str(embed).c_str());
    {
        double e_mean = embed[0].mean().item().toDouble();
        double e_std = embed[0].std().item().toDouble();
        double e0_mean = embed[0][0].mean().item().toDouble();
        double e1_mean = embed[0][1].mean().item().toDouble();
        double e2_mean = embed[0][2].mean().item().toDouble();
        double e3_mean = embed[0][3].mean().item().toDouble();
        double e4_mean = embed[0][4].mean().item().toDouble();
        fprintf(stderr, "[OCR] Embed mean=%.4f std=%.4f (first 5 tokens: %.4f %.4f %.4f %.4f %.4f)\n",
                e_mean, e_std, e0_mean, e1_mean, e2_mean, e3_mean, e4_mean);
    }
    // Check specific positions: BOS, first image token, last image token, after tokens
    {
        int64_t slen = embed.size(1);
        bool mask1 = seq_mask[1].item().toBool();
        bool mask5 = seq_mask[slen-5].item().toBool();
        double bos_mean = embed[0][0].mean().item().toDouble();
        double img1_mean = mask1 ? embed[0][1].mean().item().toDouble() : -1.0;
        double img5_mean = mask5 ? embed[0][slen-5].mean().item().toDouble() : -1.0;
        fprintf(stderr, "[OCR] BOS embed mean=%.4f, first img embed mean=%.4f, last img embed mean=%.4f\n",
                bos_mean, img1_mean, img5_mean);
    }
    
    // ─── Prefill ───
    fprintf(stderr, "[OCR] Prefill...\n");
    int64_t seq_len = embed.size(1);
    std::vector<torch::Tensor> kvs; // keys and values per layer (interleaved)
    torch::Tensor hidden;
    { torch::NoGradGuard ng;
        auto h = embed;
        for (int i = 0; i < NUM_LAYERS; i++) {
            auto [o, nk, nv] = layer_forward(h, w.layers[i], cos_sin,
                                              torch::Tensor(), torch::Tensor(), 0, true);
            h = o;
            kvs.push_back(nk);
            kvs.push_back(nv);
            double lm = h.mean().item().toDouble(), ls = h.std().item().toDouble();
            // Dump first 10 values of position 0 for comparison
            fprintf(stderr, "  [OCR] Layer %d hidden: mean=%.4f std=%.4f first10=[", i+1, lm, ls);
            for (int k = 0; k < 10 && k < h.size(2); k++) {
                fprintf(stderr, "%s%.4f", k>0?",":"", h[0][0][k].item().toDouble());
            }
            fprintf(stderr, "]\n");
        }
        hidden = h;
    }
    // Apply final RMSNorm
    { torch::NoGradGuard ng;
        hidden = rms_norm(hidden, w.final_norm_w, RMS_EPS);
    }
    {
        double h_mean = hidden.mean().item().toDouble();
        double h_std = hidden.std().item().toDouble();
        fprintf(stderr, "[OCR] After prefill hidden shape: %s mean=%.4f std=%.4f\n",
                shape_str(hidden).c_str(), h_mean, h_std);
        // Dump first 10 values of last position for comparison
        int last_pos = hidden.size(1) - 1;
        fprintf(stderr, "  [OCR] Prefill final h[0][%d] first10=[", last_pos);
        for (int k = 0; k < 10 && k < hidden.size(2); k++) {
            fprintf(stderr, "%s%.4f", k>0?",":"", hidden[0][last_pos][k].item().toDouble());
        }
        fprintf(stderr, "]\n");
    }
    // Check logits at last position
    {
        auto last_h = hidden.slice(1, seq_len-1, seq_len);
        auto logits = torch::nn::functional::linear(last_h, w.head_w).to(torch::kFloat32);
        auto vals = std::get<0>(torch::topk(logits.squeeze(), 5));
        auto idxs = std::get<1>(torch::topk(logits.squeeze(), 5));
        long id0 = idxs[0].item().toLong();
        long id1 = idxs[1].item().toLong();
        long id2 = idxs[2].item().toLong();
        long id3 = idxs[3].item().toLong();
        long id4 = idxs[4].item().toLong();
        double v0 = vals[0].item().toDouble();
        double v1 = vals[1].item().toDouble();
        double v2 = vals[2].item().toDouble();
        double v3 = vals[3].item().toDouble();
        double v4 = vals[4].item().toDouble();
        fprintf(stderr, "[OCR] Prefill final logits top5: ids=[%ld,%ld,%ld,%ld,%ld] vals=[%.2f,%.2f,%.2f,%.2f,%.2f]\n",
                id0, id1, id2, id3, id4, v0, v1, v2, v3, v4);
    }
    
    // ─── Decode loop (greedy, max 4096 tokens) ───
    fprintf(stderr, "[OCR] Generating...\n");
    const int MAX_NEW = 1024;
    std::vector<int64_t> output_ids;
    int64_t pos = seq_len;
    
    for (int step = 0; step < MAX_NEW; step++) {
        torch::Tensor next_embed;
        { torch::NoGradGuard ng;
            int64_t seq_len = hidden.size(1);
            auto last_h = hidden.slice(1, seq_len - 1, seq_len); // [1, 1, 1280] bfloat16
            auto logits = torch::nn::functional::linear(last_h, w.head_w).to(torch::kFloat32);
            auto next_id = logits.squeeze().argmax().item<int64_t>();
            output_ids.push_back(next_id);
            
            if (next_id == EOS_ID) break;
            if (step < 30) {
                // Decode the token to see the text
                std::string tok_str = vocab.decode({next_id});
                if (tok_str.length() > 20) tok_str = tok_str.substr(0, 20) + "...";
                // Get top 3 logit values for debugging
                auto top3 = torch::topk(logits.squeeze(), 3);
                auto tv = std::get<0>(top3);
                auto ti = std::get<1>(top3);
                double tv0 = tv[0].item().toDouble(), tv1 = tv[1].item().toDouble(), tv2 = tv[2].item().toDouble();
                long ti0 = ti[0].item().toLong(), ti1 = ti[1].item().toLong(), ti2 = ti[2].item().toLong();
                fprintf(stderr, "  tok[%d]=%-6ld (%-20s) top3: %ld(%.1f) %ld(%.1f) %ld(%.1f)\n",
                        step, next_id, tok_str.c_str(), ti0, tv0, ti1, tv1, ti2, tv2);
            }
            
            // Embed next token
            auto nid_t = torch::tensor({next_id}, torch::TensorOptions().dtype(torch::kLong).device(torch::kCUDA)).unsqueeze(0);
            next_embed = torch::nn::functional::embedding(nid_t, w.embed_w);
            
            // Decode step
            auto h = next_embed;
            for (int i = 0; i < NUM_LAYERS; i++) {
                auto [o, nk, nv] = layer_forward(h, w.layers[i], cos_sin,
                                                  kvs[2*i], kvs[2*i+1], pos, false);
                h = o;
                kvs[2*i] = nk;
                kvs[2*i+1] = nv;
            }
            // Apply final RMSNorm
            h = rms_norm(h, w.final_norm_w, RMS_EPS);
            hidden = h;
            pos++;
        }
        if (step % 100 == 0) fprintf(stderr, "\r  [OCR] Generated %d tokens", step+1);
    }
    fprintf(stderr, "\n[OCR] Generated %zu tokens\n", output_ids.size());
    
    // Decode output
    std::string text = vocab.decode(output_ids);
    text.erase(0, text.find_first_not_of(" \t\n\r"));
    
    // Output chapters (empty - no PDF TOC extraction here, caller handles it)
    std::cout << "---OCR_CHAPTERS:{\"chapters\":[]}---" << std::endl;
    std::cout << text << std::endl;
    
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <pdf_path> [--dpi 200]" << std::endl;
        return 1;
    }
    std::string pdf_path = argv[1];
    int dpi = 200;
    for (int i = 2; i < argc; i++) {
        if (std::string(argv[i]) == "--dpi" && i + 1 < argc)
            dpi = std::atoi(argv[++i]);
    }
    
    std::string wdir = "/data/models/baidu";
    fprintf(stderr, "[OCR] Loading model from %s...\n", wdir.c_str());
    
    Weights w = load_weights(wdir);
    Vocab vocab; vocab.load(wdir + "/vocab.bin");
    
    fprintf(stderr, "[OCR] Loading vision pipeline...\n");
    auto vpipe = torch::jit::load(wdir + "/vision_pipeline.pt", torch::kCUDA);
    vpipe.eval();
    
    fprintf(stderr, "[OCR] Precomputing RoPE cos/sin...\n");
    auto cos_sin = precompute_cos_sin(4096, HEAD_DIM).to(torch::kBFloat16).cuda();
    
    fprintf(stderr, "[OCR] Starting OCR on %s...\n", pdf_path.c_str());
    run_ocr(pdf_path, dpi, w, vocab, vpipe, cos_sin);
    fprintf(stderr, "[OCR] Done.\n");
    return 0;
}
