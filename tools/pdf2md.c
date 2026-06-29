/**
 * pdf2md.c — Convert PDF ebook to per-chapter Markdown files.
 *
 * Uses Unlimited-OCR (via ocr_cuda binary) for full-page text recognition.
 * Also extracts embedded figures/images from the PDF and saves them as
 * separate PNG files.
 *
 * Usage:
 *   ./pdf2md <input.pdf> <output_dir> [options]
 *
 * Options:
 *   --dpi NN     Render DPI (default: 200)
 *   --help       Show usage
 *
 * Output:
 *   <output_dir>/chapter_*.md                 (one per outline entry)
 *   <output_dir>/page_NNNNNN.png              (full page image)
 *   <output_dir>/page_NNNNNN_img_MMM.png      (embedded figures/images)
 *
 * Compile:
 *   gcc -std=c11 -D_GLIBCXX_USE_CXX11_ABI=0 \
 *       -I/opt/mupdf/include \
 *       -o tools/pdf2md \
 *       tools/pdf2md.c tools/ocr_helper.c \
 *       -L/opt/mupdf/build/release -lmupdf -lmupdf-third \
 *       -lm -lpthread
 *
 * Dependencies: MuPDF (rendering + image extraction), ocr_cuda (OCR engine)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <mupdf/fitz.h>
#include "ocr_helper.h"

/* ─── Chapter info ─── */
typedef struct {
    char *title;
    int level;
    int page;     /* 0-based page number where chapter starts */
} Chapter;

/* ─── Per-page content (text + image refs) ─── */
typedef struct {
    char *text;       /* OCR text for this page */
    char *img_refs;   /* accumulated ![](ref) markers for embedded figures */
    int  npage;       /* 0-based page number */
} PageContent;

/* ─── Forward declarations ─── */
static Chapter *extract_outline(fz_context *ctx, fz_document *doc, int *count);
static void     free_chapters(Chapter *ch, int count);
static void     save_page_png(fz_context *ctx, fz_document *doc, int page_num,
                              const char *out_dir, int dpi);
static int      extract_embedded_images(fz_context *ctx, fz_document *doc,
                                        int page_num, const char *out_dir,
                                        int *img_counter,
                                        PageContent *pages);
static int      write_chapter_md(const char *out_dir, int idx,
                                 const Chapter *ch,
                                 int start_page, int end_page,
                                 PageContent *pages, int total_pages);

/* ===================================================================
 * PDF outline (TOC) extraction — single-pass dynamic array.
 *
 * Recursively walks the fz_outline tree to arbitrary depth and
 * accumulates entries using a growing array.
 * =================================================================== */
static void walk_outline(fz_outline *node, int level,
                         Chapter **ch_arr, int *count, int *capacity) {
    while (node) {
        if (node->title && node->title[0] != '\0') {
            if (*count >= *capacity) {
                *capacity = (*capacity == 0) ? 32 : (*capacity * 2);
                Chapter *tmp = realloc(*ch_arr, *capacity * sizeof(Chapter));
                if (!tmp) return;
                *ch_arr = tmp;
            }
            (*ch_arr)[*count].title = strdup(node->title);
            (*ch_arr)[*count].level = level;
            (*ch_arr)[*count].page  = node->page.page;
            (*count)++;
        }
        if (node->down)
            walk_outline(node->down, level + 1, ch_arr, count, capacity);
        node = node->next;
    }
}

static Chapter *extract_outline(fz_context *ctx, fz_document *doc, int *count) {
    *count = 0;

    fz_outline *root = NULL;
    fz_try(ctx) { root = fz_load_outline(ctx, doc); }
    fz_catch(ctx) { return NULL; }
    if (!root) return NULL;

    int capacity = 0;
    Chapter *ch = NULL;
    walk_outline(root, 1, &ch, count, &capacity);

    fz_drop_outline(ctx, root);

    if (*count == 0) {
        free(ch);
        return NULL;
    }
    return ch;
}

static void free_chapters(Chapter *ch, int count) {
    if (!ch) return;
    for (int i = 0; i < count; i++) free(ch[i].title);
    free(ch);
}

/* ===================================================================
 * Full page rendering (PNG)
 * =================================================================== */
static void save_page_png(fz_context *ctx, fz_document *doc, int page_num,
                           const char *out_dir, int dpi) {
    float zoom = (float)dpi / 72.0f;
    fz_matrix ctm = fz_scale(zoom, zoom);

    char path[1024];
    snprintf(path, sizeof(path), "%s/page_%06d.png", out_dir, page_num + 1);

    fz_pixmap *pix = NULL;
    fz_try(ctx) {
        pix = fz_new_pixmap_from_page_number(ctx, doc, page_num, ctm,
                                              fz_device_rgb(ctx), 0);
        if (pix) fz_save_pixmap_as_png(ctx, pix, path);
    }
    fz_always(ctx) { fz_drop_pixmap(ctx, pix); }
    fz_catch(ctx) { /* skip if render fails */ }
}

/* ===================================================================
 * Embedded image extraction
 *
 * Uses MuPDF stext to find IMAGE blocks (embedded raster images) and
 * saves them as page_NNNNNN_img_MMM.png. Returns number extracted.
 * =================================================================== */
static int extract_embedded_images(fz_context *ctx, fz_document *doc,
                                    int page_num, const char *out_dir,
                                    int *img_counter,
                                    PageContent *pages) {
    fz_page *page = NULL;
    fz_stext_page *stext = NULL;
    fz_device *dev = NULL;
    int found = 0;

    fz_try(ctx) {
        page = fz_load_page(ctx, doc, page_num);
        fz_rect rect = fz_bound_page(ctx, page);

        stext = fz_new_stext_page(ctx, rect);
        fz_stext_options opts = { 0 };
        dev = fz_new_stext_device(ctx, stext, &opts);
        fz_run_page(ctx, page, dev, fz_identity, NULL);
        fz_close_device(ctx, dev);

        fz_stext_block *block = stext->first_block;
        while (block) {
            if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                int img_idx = (*img_counter)++;
                found++;

                char img_path[1024];
                snprintf(img_path, sizeof(img_path),
                         "%s/page_%06d_img_%03d.png",
                         out_dir, page_num + 1, img_idx);

                fz_pixmap *img_pix = fz_get_pixmap_from_image(ctx,
                    block->u.i.image, NULL, NULL, NULL, NULL);
                if (img_pix) {
                    fz_save_pixmap_as_png(ctx, img_pix, img_path);
                    fz_drop_pixmap(ctx, img_pix);
                }

                /* Append image reference to page content */
                char marker[256];
                int marklen = snprintf(marker, sizeof(marker),
                            "\n\n![Figure](page_%06d_img_%03d.png)",
                            page_num + 1, img_idx);
                if (pages && page_num >= 0 && pages[page_num].img_refs) {
                    size_t old = strlen(pages[page_num].img_refs);
                    char *tmp = realloc(pages[page_num].img_refs,
                                        old + (size_t)marklen + 1);
                    if (tmp) {
                        memcpy(tmp + old, marker, (size_t)marklen + 1);
                        pages[page_num].img_refs = tmp;
                    }
                } else if (pages && page_num >= 0) {
                    pages[page_num].img_refs = strdup(marker);
                }
            }
            block = block->next;
        }
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_stext_page(ctx, stext);
        fz_drop_page(ctx, page);
    }
    fz_catch(ctx) { /* skip errors for this page */ }

    return found;
}

/* ===================================================================
 * Write chapter markdown file
 * =================================================================== */
static int write_chapter_md(const char *out_dir, int idx,
                             const Chapter *ch,
                             int start_page, int end_page,
                             PageContent *pages, int total_pages) {
    /* Build a safe filename: chapter_XXX_sanitized_title.md */
    const char *title = ch ? ch->title : "FrontMatter";

    char safe_title[256];
    int si = 0;
    for (int i = 0; title[i] && si < 250; i++) {
        unsigned char c = (unsigned char)title[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.')
            safe_title[si++] = (char)c;
        else if (c == ' ' || c == '\t')
            safe_title[si++] = '_';
        else if (c >= 0x80)
            safe_title[si++] = (char)c;
    }
    safe_title[si] = '\0';

    char filename[512];
    snprintf(filename, sizeof(filename), "%s/chapter_%03d_%s.md",
             out_dir, idx, *safe_title ? safe_title : "");

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "[pdf2md] Cannot write: %s\n", filename);
        return -1;
    }

    /* Chapter heading */
    if (ch && ch->title && ch->title[0] != '\0') {
        if (ch->level <= 1)
            fprintf(fp, "# %s\n\n", ch->title);
        else
            fprintf(fp, "## %s\n\n", ch->title);
    }

    /* Page content (text + embedded figure refs) */
    int p_end = (end_page < total_pages) ? end_page : total_pages - 1;
    for (int p = start_page; p <= p_end; p++) {
        if (p < 0 || p >= total_pages) continue;
        if (pages[p].text && pages[p].text[0] != '\0')
            fprintf(fp, "%s\n\n", pages[p].text);
        if (pages[p].img_refs && pages[p].img_refs[0] != '\0')
            fprintf(fp, "%s\n\n", pages[p].img_refs);
    }

    fclose(fp);
    printf("  %s  (pages %d-%d)\n", filename, start_page + 1, end_page + 1);
    return 0;
}

/* ===================================================================
 * Main
 * =================================================================== */
static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <input.pdf> <output_dir> [options]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --dpi NN     Render DPI (default: 200)\n");
    fprintf(stderr, "  --help       Show this message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Output:\n");
    fprintf(stderr, "  <output_dir>/chapter_*.md              (one per outline entry)\n");
    fprintf(stderr, "  <output_dir>/page_NNNNNN.png           (full page image)\n");
    fprintf(stderr, "  <output_dir>/page_NNNNNN_img_MMM.png   (embedded figures)\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 1; }

    const char *pdf_path = argv[1];
    const char *out_dir  = argv[2];
    int dpi = 200;

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--dpi") == 0 && i + 1 < argc)
            dpi = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0)
            { usage(argv[0]); return 0; }
    }

    /* Create output directory */
    struct stat st;
    if (stat(out_dir, &st) != 0) {
        if (mkdir(out_dir, 0755) != 0) {
            fprintf(stderr, "[pdf2md] Cannot create output dir: %s\n", out_dir);
            return 1;
        }
    }

    /* ─── Open PDF ─── */
    fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!ctx) { fprintf(stderr, "[pdf2md] Cannot create MuPDF context\n"); return 1; }
    fz_register_document_handlers(ctx);

    fz_document *doc = NULL;
    fz_try(ctx) { doc = fz_open_document(ctx, pdf_path); }
    fz_catch(ctx) {
        fprintf(stderr, "[pdf2md] Cannot open PDF: %s\n", pdf_path);
        fz_drop_context(ctx);
        return 1;
    }

    int npages = fz_count_pages(ctx, doc);
    printf("[pdf2md] PDF has %d pages\n", npages);

    /* ─── Extract outline ─── */
    int nch = 0;
    Chapter *chapters = extract_outline(ctx, doc, &nch);
    printf("[pdf2md] Outline entries: %d\n", nch);

    /* ─── Run OCR for text ─── */
    printf("[pdf2md] Running OCR (Unlimited-OCR)...\n");

    char ocr_bin[1024];
    ocr_find_binary(argv[0], ocr_bin, sizeof(ocr_bin));

    char *ocr_raw = NULL;
    size_t ocr_raw_len = 0;
    if (ocr_run(pdf_path, ocr_bin, NULL, &ocr_raw, &ocr_raw_len) != 0) {
        fprintf(stderr, "[pdf2md] OCR failed, aborting.\n");
        free_chapters(chapters, nch);
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
        return 1;
    }

    char **ocr_texts = NULL;
    int nocr = 0;
    ocr_split_pages(ocr_raw, ocr_raw_len, &ocr_texts, &nocr);
    free(ocr_raw);
    printf("[pdf2md] OCR returned %d pages\n", nocr);

    /* ─── Build pages array ───
     * Allocate based on max(npages, nocr) so we don't lose data
     * if OCR returns more pages than the PDF reports.
     */
    int total_pages = (nocr > npages) ? nocr : npages;
    PageContent *pages = calloc((size_t)total_pages, sizeof(PageContent));
    if (!pages) {
        fprintf(stderr, "[pdf2md] Out of memory\n");
        ocr_free_pages(ocr_texts, nocr);
        free_chapters(chapters, nch);
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
        return 1;
    }

    for (int p = 0; p < nocr; p++) {
        pages[p].text  = ocr_texts[p];
        pages[p].npage = p;
        ocr_texts[p]   = NULL;  /* ownership transferred */
    }
    ocr_free_pages(ocr_texts, nocr);

    /* ─── Render full pages (only PDF pages exist for rendering) ─── */
    printf("[pdf2md] Rendering %d page images (%d DPI)...\n", npages, dpi);
    for (int p = 0; p < npages; p++) {
        save_page_png(ctx, doc, p, out_dir, dpi);
        if ((p + 1) % 20 == 0 || p + 1 == npages)
            fprintf(stderr, "\r  [pdf2md] Render %d/%d", p + 1, npages);
    }
    fprintf(stderr, "\n");

    /* ─── Extract embedded images ─── */
    printf("[pdf2md] Extracting embedded figures...\n");
    int img_counter = 0;
    for (int p = 0; p < npages; p++) {
        extract_embedded_images(ctx, doc, p, out_dir, &img_counter, pages);
        if ((p + 1) % 100 == 0 || p + 1 == npages)
            fprintf(stderr, "\r  [pdf2md] Images %d/%d", p + 1, npages);
    }
    fprintf(stderr, "\n");
    printf("[pdf2md] Embedded figures saved: %d\n", img_counter);

    /* ─── Write chapter files ─── */
    printf("[pdf2md] Writing chapters...\n");

    if (nch == 0) {
        write_chapter_md(out_dir, 0, NULL, 0, npages - 1, pages, total_pages);
    } else {
        int ch_idx = 0;
        for (int i = 0; i < nch; i++) {
            int start = chapters[i].page;
            int end = (i + 1 < nch) ? chapters[i + 1].page - 1 : npages - 1;
            if (start < 0) start = 0;
            if (end < start) end = start;
            if (start >= npages) continue;

            /* Front matter before first chapter */
            if (i == 0 && start > 0) {
                write_chapter_md(out_dir, ch_idx++, NULL,
                                 0, start - 1, pages, total_pages);
            }

            write_chapter_md(out_dir, ch_idx++, &chapters[i],
                             start, end, pages, total_pages);

            /* Gap pages between chapters */
            if (i + 1 < nch) {
                int next_start = chapters[i + 1].page;
                if (next_start > 0 && end + 1 < next_start) {
                    char gap_title[64];
                    snprintf(gap_title, sizeof(gap_title),
                             "Pages %d-%d", end + 2, next_start);
                    Chapter gap_ch = { gap_title, 1, end + 1 };
                    write_chapter_md(out_dir, ch_idx++, &gap_ch,
                                     end + 1, next_start - 1,
                                     pages, total_pages);
                }
            }
        }
    }

    /* ─── Cleanup ─── */
    for (int i = 0; i < total_pages; i++) {
        free(pages[i].text);
        free(pages[i].img_refs);
    }
    free(pages);
    free_chapters(chapters, nch);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);

    printf("[pdf2md] Done. Output in: %s\n", out_dir);
    return 0;
}
