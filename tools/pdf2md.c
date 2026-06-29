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
 *       tools/pdf2md.c \
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
    int npage;        /* 0-based page number */
} PageContent;

/* ─── Forward declarations ─── */
static Chapter *extract_outline(fz_context *ctx, fz_document *doc, int *count);
static void free_chapters(Chapter *ch, int count);
static void save_page_png(fz_context *ctx, fz_document *doc, int page_num,
                           const char *out_dir, int dpi);
static int extract_embedded_images(fz_context *ctx, fz_document *doc,
                                    int page_num, const char *out_dir,
                                    int *img_counter,
                                    PageContent *pages);
static char **run_ocr(const char *pdf_path, int *out_npages);
static int write_chapter_md(const char *out_dir, int idx, const Chapter *ch,
                             int start_page, int end_page,
                             PageContent *pages, int total_pages);

/* ===================================================================
 * PDF outline (TOC) extraction
 * =================================================================== */
static Chapter *extract_outline(fz_context *ctx, fz_document *doc, int *count) {
    *count = 0;

    fz_outline *root = NULL;
    fz_try(ctx) { root = fz_load_outline(ctx, doc); }
    fz_catch(ctx) { return NULL; }
    if (!root) return NULL;

    /* First pass: count entries with titles */
    int n = 0;
    {
        fz_outline *node = root;
        while (node) {
            if (node->title && strlen(node->title) > 0) n++;
            if (node->down) {
                fz_outline *child = node->down;
                while (child) {
                    if (child->title && strlen(child->title) > 0) n++;
                    child = child->next;
                }
            }
            node = node->next;
        }
    }
    if (n == 0) { fz_drop_outline(ctx, root); return NULL; }

    Chapter *ch = calloc(n, sizeof(Chapter));
    if (!ch) { fz_drop_outline(ctx, root); return NULL; }

    int idx = 0;
    fz_outline *node = root;
    while (node && idx < n) {
        if (node->title && strlen(node->title) > 0) {
            ch[idx].title = strdup(node->title);
            ch[idx].level = 1;
            ch[idx].page = node->page.page;
            idx++;
        }
        if (node->down) {
            fz_outline *child = node->down;
            while (child && idx < n) {
                if (child->title && strlen(child->title) > 0) {
                    ch[idx].title = strdup(child->title);
                    ch[idx].level = 2;
                    ch[idx].page = child->page.page;
                    idx++;
                }
                child = child->next;
            }
        }
        node = node->next;
    }

    fz_drop_outline(ctx, root);
    *count = idx;
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
                snprintf(img_path, sizeof(img_path), "%s/page_%06d_img_%03d.png",
                         out_dir, page_num + 1, img_idx);

                fz_pixmap *img_pix = fz_get_pixmap_from_image(ctx,
                    block->u.i.image, NULL, NULL, NULL, NULL);
                if (img_pix) {
                    fz_save_pixmap_as_png(ctx, img_pix, img_path);
                    fz_drop_pixmap(ctx, img_pix);
                }

                /* Append image reference to page content */
                char marker[256];
                snprintf(marker, sizeof(marker),
                         "\n\n![Figure](page_%06d_img_%03d.png)",
                         page_num + 1, img_idx);
                if (pages && pages[page_num].img_refs) {
                    size_t old = strlen(pages[page_num].img_refs);
                    size_t add = strlen(marker);
                    char *tmp = realloc(pages[page_num].img_refs,
                                        old + add + 1);
                    if (tmp) {
                        memcpy(tmp + old, marker, add + 1);
                        pages[page_num].img_refs = tmp;
                    }
                } else if (pages) {
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
 * Run OCR via ocr_cuda binary.
 *
 * Returns array of per-page text strings (caller must free).
 * Output format from ocr_cuda:
 *   Line 1: ---OCR_CHAPTERS:{...}---
 *   Rest:   OCR text with <PAGE> markers separating pages
 * =================================================================== */
static char **run_ocr(const char *pdf_path, int *out_npages) {
    *out_npages = 0;

    /* Locate ocr_cuda binary (same dir as this exe) */
    char ocr_bin[1024];
    {
        ssize_t r = readlink("/proc/self/exe", ocr_bin, sizeof(ocr_bin) - 1);
        if (r > 0 && r < (ssize_t)sizeof(ocr_bin)) {
            ocr_bin[r] = '\0';
            char *slash = strrchr(ocr_bin, '/');
            if (slash) {
                size_t dlen = slash - ocr_bin + 1;
                snprintf(ocr_bin + dlen, sizeof(ocr_bin) - dlen, "ocr_cuda");
            } else {
                strcpy(ocr_bin, "./ocr_cuda");
            }
        } else {
            strcpy(ocr_bin, "./tools/ocr_cuda");
        }
    }

    /* Build command */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" --dpi 200 2>/dev/null",
             ocr_bin, pdf_path);
    fprintf(stderr, "[pdf2md] Running: %s\n", cmd);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[pdf2md] Failed to run ocr_cuda\n");
        return NULL;
    }

    /* Skip first line (chapter JSON) */
    char discard[65536];
    if (!fgets(discard, sizeof(discard), fp)) {
        fprintf(stderr, "[pdf2md] No output from ocr_cuda\n");
        pclose(fp);
        return NULL;
    }

    /* Read remaining output */
    size_t cap = 1048576;
    size_t len = 0;
    char *all = malloc(cap);
    if (!all) { pclose(fp); return NULL; }
    all[0] = '\0';

    char line[16384];
    while (fgets(line, sizeof(line), fp)) {
        size_t ll = strlen(line);
        if (len + ll + 1 > cap) {
            cap *= 2;
            char *tmp = realloc(all, cap);
            if (!tmp) { free(all); pclose(fp); return NULL; }
            all = tmp;
        }
        memcpy(all + len, line, ll + 1);
        len += ll;
    }
    pclose(fp);

    if (len == 0) { free(all); return NULL; }

    /* Split by <PAGE> markers */
    int count = 0;
    {
        char *p = all;
        while ((p = strstr(p, "<PAGE>")) != NULL) { count++; p += 6; }
    }
    if (count == 0) count = 1;

    char **pages = calloc(count, sizeof(char *));
    if (!pages) { free(all); return NULL; }

    int actual = 0;
    if (count == 1) {
        pages[0] = strdup(all);
        actual = 1;
    } else {
        char *save = NULL;
        char *tok = strtok_r(all, "\n", &save);
        int page_idx = -1;
        size_t page_cap = 4096;
        char *page_buf = malloc(page_cap);
        if (!page_buf) { free(all); free(pages); return NULL; }
        page_buf[0] = '\0';
        size_t page_len = 0;

        while (tok) {
            if (strstr(tok, "<PAGE>")) {
                if (page_idx >= 0 && page_idx < count) {
                    pages[page_idx] = strdup(page_buf);
                    if (page_idx + 1 > actual) actual = page_idx + 1;
                }
                page_idx++;
                page_buf[0] = '\0';
                page_len = 0;
            } else {
                size_t ll = strlen(tok);
                size_t need = page_len + ll + 2;
                if (need > page_cap) {
                    page_cap *= 2;
                    char *tmp = realloc(page_buf, page_cap);
                    if (!tmp) { free(page_buf); free(all); free(pages); return NULL; }
                    page_buf = tmp;
                }
                if (page_len > 0) page_buf[page_len++] = '\n';
                memcpy(page_buf + page_len, tok, ll);
                page_len += ll;
                page_buf[page_len] = '\0';
            }
            tok = strtok_r(NULL, "\n", &save);
        }
        if (page_idx >= 0 && page_idx < count) {
            pages[page_idx] = strdup(page_buf);
            if (page_idx + 1 > actual) actual = page_idx + 1;
        }
        free(page_buf);
    }

    free(all);
    *out_npages = actual;
    return pages;
}

/* ===================================================================
 * Write chapter markdown file
 * =================================================================== */
static int write_chapter_md(const char *out_dir, int idx, const Chapter *ch,
                             int start_page, int end_page,
                             PageContent *pages, int total_pages) {
    char filename[512];
    const char *title = ch ? ch->title : "FrontMatter";
    snprintf(filename, sizeof(filename), "%s/chapter_%03d_", out_dir, idx);

    char safe_title[256];
    int si = 0;
    for (int i = 0; title[i] && si < 250; i++) {
        char c = title[i];
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')
            safe_title[si++] = c;
        else if (c == ' ' || c == '\t')
            safe_title[si++] = '_';
        else if ((unsigned char)c >= 0x80)
            safe_title[si++] = c;
    }
    safe_title[si] = '\0';
    strncat(filename, safe_title, sizeof(filename) - strlen(filename) - 1);
    strncat(filename, ".md", sizeof(filename) - strlen(filename) - 1);

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "[pdf2md] Cannot write: %s\n", filename);
        return -1;
    }

    /* Chapter heading */
    if (ch && ch->title && strlen(ch->title) > 0) {
        if (ch->level <= 1)
            fprintf(fp, "# %s\n\n", ch->title);
        else
            fprintf(fp, "## %s\n\n", ch->title);
    }

    /* Page content (text + embedded figure refs) */
    for (int p = start_page; p <= end_page && p < total_pages; p++) {
        if (pages && pages[p].text && strlen(pages[p].text) > 0) {
            fprintf(fp, "%s\n\n", pages[p].text);
        }
        if (pages && pages[p].img_refs && strlen(pages[p].img_refs) > 0) {
            fprintf(fp, "%s\n\n", pages[p].img_refs);
        }
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
    const char *out_dir = argv[2];
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
    printf("[pdf2md] PDF: %d pages\n", npages);

    /* ─── Extract outline ─── */
    int nch = 0;
    Chapter *chapters = extract_outline(ctx, doc, &nch);
    printf("[pdf2md] Outline: %d entries\n", nch);

    /* ─── Run OCR for text ─── */
    printf("[pdf2md] Recognizing text with Unlimited-OCR...\n");
    int nocr = 0;
    char **ocr_texts = run_ocr(pdf_path, &nocr);
    if (!ocr_texts || nocr == 0) {
        fprintf(stderr, "[pdf2md] OCR failed, aborting.\n");
        free_chapters(chapters, nch);
        fz_drop_document(ctx, doc);
        fz_drop_context(ctx);
        return 1;
    }
    printf("[pdf2md] OCR text: %d pages\n", nocr);

    /* Transfer OCR text into pages array */
    PageContent *pages = calloc(npages, sizeof(PageContent));
    if (!pages) { fprintf(stderr, "[pdf2md] OOM\n"); return 1; }
    for (int p = 0; p < npages && p < nocr; p++) {
        pages[p].text = ocr_texts[p];
        pages[p].npage = p;
        ocr_texts[p] = NULL;
    }
    free(ocr_texts);

    /* ─── Render full pages ─── */
    printf("[pdf2md] Rendering page images (%d DPI)...\n", dpi);
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
        write_chapter_md(out_dir, 0, NULL, 0, npages - 1, pages, npages);
    } else {
        int ch_idx = 0;
        for (int i = 0; i < nch; i++) {
            int start = chapters[i].page;
            int end = (i + 1 < nch) ? chapters[i + 1].page - 1 : npages - 1;
            if (start < 0) start = 0;
            if (end < start) end = start;
            if (start >= npages) continue;

            if (i == 0 && start > 0) {
                write_chapter_md(out_dir, ch_idx++, NULL, 0, start - 1,
                                  pages, npages);
            }
            write_chapter_md(out_dir, ch_idx++, &chapters[i], start, end,
                              pages, npages);

            if (i + 1 < nch) {
                int next_start = chapters[i + 1].page;
                if (next_start > 0 && end + 1 < next_start) {
                    char gap_title[64];
                    snprintf(gap_title, sizeof(gap_title), "Pages %d-%d",
                             end + 2, next_start);
                    Chapter gap_ch = { gap_title, 1, end + 1 };
                    write_chapter_md(out_dir, ch_idx++, &gap_ch, end + 1,
                                      next_start - 1, pages, npages);
                }
            }
        }
    }

    /* ─── Cleanup ─── */
    if (pages) {
        for (int i = 0; i < npages; i++) {
            free(pages[i].text);
            free(pages[i].img_refs);
        }
        free(pages);
    }
    free_chapters(chapters, nch);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);

    printf("[pdf2md] Done. Output in: %s\n", out_dir);
    return 0;
}
