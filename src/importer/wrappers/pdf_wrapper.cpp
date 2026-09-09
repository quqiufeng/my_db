/**
 * PDF/EPUB C Wrapper - 封装 MuPDF 为 C 接口供 Python ctypes 调用
 * 支持章节提取
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

extern "C" {
#include "mupdf/fitz.h"

#define API __attribute__((visibility("default")))

// 章节信息结构（与 MOBI wrapper 保持相同布局）
struct PdfChapter {
    char* title;
    int level;
    size_t page;  // 使用 size_t 与 MOBI 的 offset 对齐
};

// Opaque handle
struct PdfHandle {
    fz_context* ctx;
    fz_document* doc;
    char* text_cache;
    size_t text_cache_len;
    PdfChapter* chapters;
    int chapter_count;
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
    handle->chapters = nullptr;
    handle->chapter_count = 0;

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
            // 页加载失败也要输出分页标记，保证 页码 → 字节偏移 映射对齐
            result += "\n--- Page Break ---\n\n";
            continue;
        }
        if (!page) {
            result += "\n--- Page Break ---\n\n";
            continue;
        }

        fz_stext_page* text_page = nullptr;
        fz_try(h->ctx) {
            text_page = fz_new_stext_page_from_page(h->ctx, page, nullptr);
        }
        fz_catch(h->ctx) {
            fz_drop_page(h->ctx, page);
            result += "\n--- Page Break ---\n\n";
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
                    // 文本块之间插入段落分隔符，让 split_paragraphs 能正确切分
                    result += "\n";
                }
            }
            fz_drop_stext_page(h->ctx, text_page);
        }
        // 每页固定输出分页标记（无论该页是否有文本），import_book 据此将
        // outline 页码映射为文本字节偏移
        result += "\n--- Page Break ---\n\n";
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

// 递归提取 outline 为章节列表
static void extract_outlines(fz_context* ctx, fz_outline* outline, PdfChapter** chapters, int* count, int level) {
    while (outline) {
        PdfChapter* new_chapters = (PdfChapter*)realloc(*chapters, sizeof(PdfChapter) * (*count + 1));
        if (!new_chapters) return;
        *chapters = new_chapters;
        
        (*chapters)[*count].title = outline->title ? strdup(outline->title) : strdup("");
        (*chapters)[*count].level = level;
        (*chapters)[*count].page = outline->page.page;  // fz_location.page (assign to size_t)
        (*count)++;
        
        // 递归处理子章节
        if (outline->down) {
            extract_outlines(ctx, outline->down, chapters, count, level + 1);
        }
        
        outline = outline->next;
    }
}

/**
 * 提取章节列表（从 PDF outline/书签）
 * @param handle pdf_open 返回的句柄
 * @param out_chapters 输出章节数组（调用者需用 pdf_free_chapters 释放）
 * @param out_count 输出章节数量
 * @return 0 成功，-1 失败
 */
API int pdf_get_chapters(void* handle, PdfChapter** out_chapters, int* out_count) {
    PdfHandle* h = static_cast<PdfHandle*>(handle);
    if (!h || !h->doc) return -1;

    // 如果已提取过，直接返回
    if (h->chapters) {
        *out_chapters = h->chapters;
        *out_count = h->chapter_count;
        return 0;
    }

    fz_outline* outline = nullptr;
    fz_try(h->ctx) {
        outline = fz_load_outline(h->ctx, h->doc);
    }
    fz_catch(h->ctx) {
        *out_count = 0;
        *out_chapters = nullptr;
        return 0;  // 没有 outline 不算失败
    }

    if (!outline) {
        *out_count = 0;
        *out_chapters = nullptr;
        return 0;
    }

    h->chapters = nullptr;
    h->chapter_count = 0;
    extract_outlines(h->ctx, outline, &h->chapters, &h->chapter_count, 0);
    
    fz_drop_outline(h->ctx, outline);

    *out_chapters = h->chapters;
    *out_count = h->chapter_count;
    return 0;
}

/**
 * 释放章节数组
 * @param chapters pdf_get_chapters 返回的数组
 * @param count 章节数量
 */
API void pdf_free_chapters(PdfChapter* chapters, int count) {
    if (!chapters) return;
    for (int i = 0; i < count; i++) {
        free(chapters[i].title);
    }
    free(chapters);
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

    if (h->chapters) {
        pdf_free_chapters(h->chapters, h->chapter_count);
    }
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
