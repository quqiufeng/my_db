/**
 * ocr_cuda — PDF → OCR 文本引擎（StaticPy 推理版）
 *
 * 旧 ocr_cuda 是手写 C++ 推理（libtorch 实现 12 层 MoE 解码器）。
 * 本版本把"图片→文本"推理替换为调用 StaticPy 编译出的 ocr_kv 二进制：
 *
 *   PDF 页 → MuPDF 渲染成 PPM → ocr_kv（vision + 12层解码器 + KV cache）
 *         → token id 流 → 本程序 vocab 解码 → <PAGE> 文本
 *
 * 保留：MuPDF PDF 渲染、outline 章节 JSON、vocab 解码、<PAGE> 输出协议
 * 移除：libtorch 推理（不再链接 torch，只依赖 ocr_kv 子进程）
 *
 * 用法:  ./ocr_cuda <pdf_path> [--dpi 200]
 * 输出:  ---OCR_CHAPTERS:{json}---  +  每页 "<PAGE>\n<text>\n"
 */

#include <mupdf/fitz.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <iostream>
#include <functional>

static constexpr int VOCAB_SIZE = 129280;
static constexpr int64_t EOS_ID = 128814;

// ─── Vocab（同旧实现：BPE 解码 + 标签/坐标清理）───
struct Vocab {
    std::vector<std::string> table;
    Vocab() : table(VOCAB_SIZE) {}
    void load(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) { fprintf(stderr, "[FATAL] Cannot open vocab: %s\n", path.c_str()); exit(1); }
        for (int i = 0; i < VOCAB_SIZE; i++) {
            uint32_t len;
            if (fread(&len, 4, 1, f) != 1) break;
            if (len > 0) {
                std::string s(len, '\0');
                if (fread(&s[0], 1, len, f) != len) break;
                table[i] = s;
            }
        }
        fclose(f);
    }
    std::string decode(const std::vector<int64_t>& ids) {
        std::string out;
        for (auto id : ids)
            if (id >= 0 && id < (int64_t)VOCAB_SIZE && !table[id].empty()) out += table[id];
        { size_t p; while ((p = out.find("\xc4\xa0")) != std::string::npos) out.replace(p, 2, " "); }
        { size_t p; while ((p = out.find("\xc4\x8a")) != std::string::npos) out.replace(p, 2, "\n"); }
        auto strip = [&](const std::string& o, const std::string& c) {
            size_t p; while ((p = out.find(o)) != std::string::npos) {
                size_t q = out.find(c, p);
                if (q != std::string::npos) out.erase(p, q - p + c.size()); else break;
            }
        };
        strip("<|", "|>");
        strip("<\xef\xbd\x9c", "\xef\xbd\x9c>");
        { size_t i = 0;
          while (i < out.size()) {
              size_t nl = out.find('\n', i), br = out.find('[', i);
              if (br != std::string::npos && (nl == std::string::npos || br < nl)) {
                  size_t br2 = out.find(']', br);
                  if (br2 != std::string::npos && (nl == std::string::npos || br2 < nl)) {
                      out.erase(br, br2 - br + 1); continue;
                  }
              }
              i = (nl == std::string::npos) ? out.size() : nl + 1;
          }
        }
        return out;
    }
};

static std::string find_ocr_kv(const char* argv0) {
    std::string self(argv0);
    size_t slash = self.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : self.substr(0, slash);
    // ocr_cuda 在 tools/ 下，ocr_kv 在项目根 ocrdec/
    std::string cand = dir + "/ocrdec/ocr_kv";
    if (access(cand.c_str(), X_OK) == 0) return cand;
    cand = dir + "/../ocrdec/ocr_kv";
    if (access(cand.c_str(), X_OK) == 0) return cand;
    cand = dir + "/ocr_kv";
    if (access(cand.c_str(), X_OK) == 0) return cand;
    cand = "/opt/ReScheme/ocrdec/ocr_kv";
    if (access(cand.c_str(), X_OK) == 0) return cand;
    return "ocr_kv";
}

static std::string render_page_ppm(fz_context* ctx, fz_document* doc, int p, int dpi) {
    float zoom = (float)dpi / 72.0f;
    fz_matrix transform = fz_scale(zoom, zoom);
    fz_pixmap* pix = nullptr;
    fz_try(ctx) { pix = fz_new_pixmap_from_page_number(ctx, doc, p, transform, fz_device_rgb(ctx), 0); }
    fz_catch(ctx) { return ""; }
    if (!pix) return "";
    int pw = fz_pixmap_width(ctx, pix), ph = fz_pixmap_height(ctx, pix);
    int stride = fz_pixmap_stride(ctx, pix), ch = pix->n;
    unsigned char* samp = fz_pixmap_samples(ctx, pix);

    char tmpl[256];
    snprintf(tmpl, sizeof(tmpl), "/tmp/ocr_page_%d_%d.ppm", getpid(), p);
    FILE* f = fopen(tmpl, "wb");
    if (!f) { fz_drop_pixmap(ctx, pix); return ""; }
    fprintf(f, "P6\n%d %d\n255\n", pw, ph);
    for (int y = 0; y < ph; y++)
        fwrite(samp + (size_t)y * stride, 1, (size_t)pw * ch, f);
    fclose(f);
    fz_drop_pixmap(ctx, pix);
    return tmpl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <pdf_path> [--dpi 200]\n", argv[0]);
        return 1;
    }
    std::string pdf_path = argv[1];
    int dpi = 200;
    for (int i = 2; i < argc; i++)
        if (std::string(argv[i]) == "--dpi" && i + 1 < argc) dpi = std::atoi(argv[++i]);

    Vocab vocab; vocab.load("/data/models/baidu/vocab.bin");

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) { fprintf(stderr, "[FATAL] no MuPDF ctx\n"); return 1; }
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_try(ctx) { doc = fz_open_document(ctx, pdf_path.c_str()); }
    fz_catch(ctx) { fprintf(stderr, "[FATAL] Cannot open PDF: %s\n", pdf_path.c_str()); return 1; }
    int npages = fz_count_pages(ctx, doc);

    // outline → 章节 JSON（协议头，先输出）
    std::string chapters_json = "[]";
    fz_outline* outline = nullptr;
    fz_try(ctx) { outline = fz_load_outline(ctx, doc); } fz_catch(ctx) {}
    if (outline) {
        std::string json = "[";
        bool first = true;
        std::function<void(fz_outline*, int)> walk = [&](fz_outline* node, int depth) {
            while (node) {
                if (node->title && strlen(node->title) > 0) {
                    if (!first) json += ","; first = false;
                    std::string t;
                    for (const char* p = node->title; *p; p++) {
                        if (*p == '"' || *p == '\\') { t += '\\'; t += *p; }
                        else if (*p == '\n') t += "\\n"; else if (*p == '\r') t += "\\r";
                        else if (*p == '\t') t += "\\t";
                        else if ((unsigned char)*p < 0x20) {}
                        else t += *p;
                    }
                    json += "{\"title\":\"" + t + "\",\"level\":" + std::to_string(depth)
                          + ",\"page\":" + std::to_string(node->page.page) + "}";
                }
                if (node->down) walk(node->down, depth + 1);
                node = node->next;
            }
        };
        walk(outline, 1);
        json += "]"; chapters_json = json;
        fz_drop_outline(ctx, outline);
    }
    std::cout << "---OCR_CHAPTERS:{\"chapters\":" << chapters_json << "}---" << std::endl;

    // 渲染所有页为 PPM
    std::vector<std::string> ppm_files;
    for (int p = 0; p < npages; p++) {
        std::string f = render_page_ppm(ctx, doc, p, dpi);
        if (!f.empty()) ppm_files.push_back(f);
        else std::cout << "<PAGE>" << std::endl << std::endl;
    }

    // 调 ocr_kv：一次进程处理全部页（参数过长则每页单独调）
    std::string okv = find_ocr_kv(argv[0]);
    std::string cmd = "\"" + okv + "\"";
    for (auto& f : ppm_files) cmd += " \"" + f + "\"";
    fprintf(stderr, "[OCR] engine: %s (%d pages)\n", okv.c_str(), (int)ppm_files.size());

    // ocr_kv 运行时需要 libtorch_std_helper.so（ReScheme 工具链目录）+ torch 运行时
    const char* torch_lib = "/data/venv/lib/python3.12/site-packages/torch/lib";
    const char* helper_dir = "/opt/ReScheme";  // libtorch_std_helper.so 所在
    const char* ldpath = getenv("LD_LIBRARY_PATH");
    std::string newlp = std::string(torch_lib) + ":" + helper_dir + (ldpath ? (":" + std::string(ldpath)) : "");
    setenv("LD_LIBRARY_PATH", newlp.c_str(), 1);

    // 预读 ocr_kv 输出：每页 token
    std::vector<std::vector<int64_t>> pages_tokens;
    bool need_popen = true;
    std::vector<int64_t> cur;
    pages_tokens.push_back(cur);  // 占位：第一个 PAGE_BREAK 前
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) {
        // popen 失败：每页单独尝试
        need_popen = false;
    }
    if (need_popen) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) {
            std::string l(buf);
            while (!l.empty() && (l.back()=='\n'||l.back()=='\r')) l.pop_back();
            if (l == "PAGE_BREAK") { pages_tokens.push_back(std::vector<int64_t>()); continue; }
            if (l.empty()) continue;
            char* end; long v = strtol(l.c_str(), &end, 10);
            if (end != l.c_str() && v >= 0 && v < VOCAB_SIZE)
                pages_tokens.back().push_back(v);
        }
        pclose(fp);
    } else {
        for (auto& f : ppm_files) {
            std::string c1 = "\"" + okv + "\" \"" + f + "\"";
            FILE* fp1 = popen(c1.c_str(), "r");
            std::vector<int64_t> t;
            if (fp1) {
                char buf[4096];
                while (fgets(buf, sizeof(buf), fp1)) {
                    char* end; long v = strtol(buf, &end, 10);
                    if (end != buf && v >= 0 && v < VOCAB_SIZE) t.push_back(v);
                }
                pclose(fp1);
            }
            pages_tokens.push_back(t);
            remove(f.c_str());
        }
    }

    // 每页 token → 文本
    // pages_tokens[0]=空(首个PAGE_BREAK前), pages_tokens[k] 对应第 k 页(从1起)
    for (size_t pi = 0; pi < ppm_files.size(); pi++) {
        size_t idx = (pi + 1 < pages_tokens.size()) ? pi + 1 : pi;
        std::vector<int64_t> t;
        for (auto id : pages_tokens[idx]) { if (id == EOS_ID) break; t.push_back(id); }
        std::string text = vocab.decode(t);
        size_t b = text.find_first_not_of(" \t\n\r");
        text = (b == std::string::npos) ? "" : text.substr(b);
        size_t e = text.find_last_not_of(" \t\n\r");
        if (e != std::string::npos) text = text.substr(0, e + 1);
        std::cout << "<PAGE>" << std::endl;
        if (!text.empty()) std::cout << text << std::endl;
        remove(ppm_files[pi].c_str());
    }

    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return 0;
}
