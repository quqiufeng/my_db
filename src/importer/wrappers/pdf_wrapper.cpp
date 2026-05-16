/**
 * PDF C Wrapper - 封装 MuPDF 为 C 接口供 Python ctypes 调用
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

extern "C" {
#include "mupdf/fitz.h"

#define API __attribute__((visibility("default")))

// Opaque handle
struct PdfHandle {
    fz_context* ctx;
    fz_document* doc;
    char* text_cache;
    size_t text_cache_len;
};

/**
 * 打开 PDF 文件
 * @param path 文件路径
 * @return handle（NULL 表示失败）
 */
API void* pdf_open(const char* path) {
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) {
        fprintf(stderr, "[PDF] 无法创建 MuPDF 上下文\n");
        return nullptr;
    }

    // 注册文档处理器（PDF/XPS/EPUB 等）
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
    }
    fz_catch(ctx) {}

    fz_document* doc = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, path);
    }
    fz_catch(ctx) {
        fprintf(stderr, "[PDF] 无法打开文件: %s\n", path);
        fz_drop_context(ctx);
        return nullptr;
    }

    PdfHandle* handle = new PdfHandle();
    handle->ctx = ctx;
    handle->doc = doc;
    handle->text_cache = nullptr;
    handle->text_cache_len = 0;

    return handle;
}

/**
 * 获取 PDF 页数
 * @param handle pdf_open 返回的句柄
 * @return 页数，-1 表示失败
 */
API int pdf_get_page_count(void* handle) {
    PdfHandle* h = static_cast<PdfHandle*>(handle);
    if (!h || !h->doc) return -1;

    int count = -1;
    fz_try(h->ctx) {
        count = fz_count_pages(h->ctx, h->doc);
    }
    fz_catch(h->ctx) {
        return -1;
    }
    return count;
}

/**
 * 提取所有页面的纯文本
 * @param handle pdf_open 返回的句柄
 * @param out_text 输出文本指针（需调用 pdf_free_text 释放）
 * @param out_len 输出文本长度
 * @return 0 成功，-1 失败
 */
API int pdf_extract_text(void* handle, char** out_text, size_t* out_len) {
    PdfHandle* h = static_cast<PdfHandle*>(handle);
    if (!h || !h->doc) return -1;

    // 如果已有缓存，直接返回
    if (h->text_cache) {
        *out_text = h->text_cache;
        *out_len = h->text_cache_len;
        return 0;
    }

    int page_count = pdf_get_page_count(h);
    if (page_count < 0) return -1;

    std::string result;

    for (int i = 0; i < page_count; i++) {
        fz_page* page = nullptr;
        fz_try(h->ctx) {
            page = fz_load_page(h->ctx, h->doc, i);
        }
        fz_catch(h->ctx) {
            continue;
        }
        if (!page) continue;

        fz_stext_page* text_page = nullptr;
        fz_try(h->ctx) {
            text_page = fz_new_stext_page_from_page(h->ctx, page, nullptr);
        }
        fz_catch(h->ctx) {
            fz_drop_page(h->ctx, page);
            continue;
        }

        if (text_page) {
            // 遍历文本块提取文本
            for (fz_stext_block* block = text_page->first_block; block; block = block->next) {
                if (block->type == FZ_STEXT_BLOCK_TEXT) {
                    for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                        for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                            // 将字符 UTF-32 转换为 UTF-8
                            char buf[8];
                            int len = fz_runetochar(buf, ch->c);
                            result.append(buf, len);
                        }
                        result += "\n";
                    }
                }
            }
            result += "\n--- Page Break ---\n\n";
            fz_drop_stext_page(h->ctx, text_page);
        }
        fz_drop_page(h->ctx, page);
    }

    // 分配并缓存结果
    h->text_cache_len = result.length();
    h->text_cache = (char*)malloc(h->text_cache_len + 1);
    if (!h->text_cache) return -1;

    memcpy(h->text_cache, result.c_str(), h->text_cache_len + 1);
    *out_text = h->text_cache;
    *out_len = h->text_cache_len;
    return 0;
}

/**
 * 释放 pdf_extract_text 返回的文本
 * @param text 文本指针
 */
API void pdf_free_text(char* text) {
    // 实际释放由 pdf_close 统一处理缓存
    (void)text;
}

/**
 * 关闭 PDF 文件并释放资源
 * @param handle pdf_open 返回的句柄
 */
API void pdf_close(void* handle) {
    PdfHandle* h = static_cast<PdfHandle*>(handle);
    if (!h) return;

    if (h->text_cache) {
        free(h->text_cache);
    }
    if (h->doc) {
        fz_drop_document(h->ctx, h->doc);
    }
    if (h->ctx) {
        fz_drop_context(h->ctx);
    }
    delete h;
}

} // extern "C"
