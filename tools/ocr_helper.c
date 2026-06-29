/**
 * ocr_helper.c — Shared OCR integration implementation.
 */
#define _GNU_SOURCE
#include "ocr_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ===================================================================
 * Locate ocr_cuda binary
 * =================================================================== */
int ocr_find_binary(const char *exe_path, char *out_bin, size_t bin_size) {
    if (!out_bin || bin_size == 0) return -1;

#if defined(__linux__)
    /* Try /proc/self/exe first (most reliable) */
    ssize_t r = readlink("/proc/self/exe", out_bin, bin_size - 1);
    if (r > 0 && r < (ssize_t)bin_size) {
        out_bin[r] = '\0';
        char *slash = strrchr(out_bin, '/');
        if (slash) {
            size_t dlen = slash - out_bin + 1;
            if (dlen + 9 < bin_size) {
                snprintf(out_bin + dlen, bin_size - dlen, "ocr_cuda");
                return (int)(dlen + 8);
            }
        }
    }
#endif

    /* Fallback: use exe_path */
    if (exe_path && *exe_path) {
        size_t len = strlen(exe_path);
        if (len + 10 < bin_size) {
            memcpy(out_bin, exe_path, len + 1);
            char *slash = strrchr(out_bin, '/');
            if (slash) {
                size_t dlen = slash - out_bin + 1;
                snprintf(out_bin + dlen, bin_size - dlen, "ocr_cuda");
                return (int)(dlen + 8);
            }
        }
    }

    /* Last resort */
    snprintf(out_bin, bin_size, "./ocr_cuda");
    return (int)strlen(out_bin);
}

/* ===================================================================
 * Run ocr_cuda on PDF
 * =================================================================== */
int ocr_run(const char *pdf_path, const char *ocr_bin,
            char **out_chapters, char **out_text, size_t *out_text_len) {
    if (out_chapters) *out_chapters = NULL;
    if (out_text) *out_text = NULL;
    if (out_text_len) *out_text_len = 0;

    /* Build command */
    char cmd[4096];
    int cmd_len = snprintf(cmd, sizeof(cmd),
                           "\"%s\" \"%s\" --dpi 200 2>/dev/null",
                           ocr_bin, pdf_path);
    if (cmd_len >= (int)sizeof(cmd)) {
        fprintf(stderr, "[ocr_helper] Command too long\n");
        return -1;
    }

    fprintf(stderr, "[ocr_helper] Running: %s\n", cmd);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[ocr_helper] Failed to run: %s\n", ocr_bin);
        return -1;
    }

    /* ── Read first line: chapter JSON ── */
    char chapters_line[65536];
    if (!fgets(chapters_line, sizeof(chapters_line), fp)) {
        fprintf(stderr, "[ocr_helper] No output from ocr_cuda\n");
        pclose(fp);
        return -1;
    }

    /* Strip trailing newline/cr */
    size_t clen = strlen(chapters_line);
    while (clen > 0 && (chapters_line[clen-1] == '\n' ||
                        chapters_line[clen-1] == '\r'))
        chapters_line[--clen] = '\0';

    /* Extract JSON from ---OCR_CHAPTERS:{...}--- */
    if (out_chapters) {
        char *json_start = strstr(chapters_line, "---OCR_CHAPTERS:");
        if (json_start) {
            json_start += 16; /* skip past "---OCR_CHAPTERS:" */
            char *json_end = strstr(json_start, "---");
            if (json_end) *json_end = '\0';
            *out_chapters = strdup(json_start);
        } else {
            *out_chapters = strdup(chapters_line);
        }
    }

    /* ── Read remaining lines: OCR text ── */
    size_t cap = 1048576;
    size_t len = 0;
    char *text = malloc(cap);
    if (!text) { pclose(fp); return -1; }
    text[0] = '\0';

    char line[16384];
    while (fgets(line, sizeof(line), fp)) {
        size_t ll = strlen(line);
        if (len + ll + 1 > cap) {
            cap *= 2;
            char *tmp = realloc(text, cap);
            if (!tmp) { free(text); pclose(fp); return -1; }
            text = tmp;
        }
        memcpy(text + len, line, ll + 1);
        len += ll;
    }

    int status = pclose(fp);
    if (status != 0 && len == 0) {
        fprintf(stderr, "[ocr_helper] OCR binary exited with status %d, no output\n", status);
        free(text);
        return -1;
    }

    if (out_text) *out_text = text;
    if (out_text_len) *out_text_len = len;
    return 0;
}

/* ===================================================================
 * Split OCR text into per-page array by <PAGE> markers
 * =================================================================== */
int ocr_split_pages(const char *text, size_t text_len,
                     char ***out_pages, int *out_npages) {
    *out_pages = NULL;
    *out_npages = 0;

    if (!text || text_len == 0) return -1;

    /* Count <PAGE> markers */
    int count = 0;
    {
        const char *p = text;
        while ((p = strstr(p, "<PAGE>")) != NULL) { count++; p += 6; }
    }
    if (count == 0) count = 1;

    char **pages = calloc(count, sizeof(char *));
    if (!pages) return -1;

    int actual = 0;
    if (count == 1) {
        pages[0] = strdup(text);
        if (!pages[0]) { free(pages); return -1; }
        actual = 1;
    } else {
        /* Use strtok_r to split by lines */
        char *text_copy = strdup(text);
        if (!text_copy) { free(pages); return -1; }

        char *save = NULL;
        char *tok = strtok_r(text_copy, "\n", &save);
        int page_idx = -1;
        size_t page_cap = 4096;
        char *page_buf = malloc(page_cap);
        if (!page_buf) { free(text_copy); free(pages); return -1; }
        page_buf[0] = '\0';
        size_t page_len = 0;

        while (tok) {
            if (strstr(tok, "<PAGE>") != NULL) {
                /* Save previous page */
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
                    if (!tmp) {
                        free(page_buf); free(text_copy); free(pages);
                        return -1;
                    }
                    page_buf = tmp;
                }
                if (page_len > 0) page_buf[page_len++] = '\n';
                memcpy(page_buf + page_len, tok, ll);
                page_len += ll;
                page_buf[page_len] = '\0';
            }
            tok = strtok_r(NULL, "\n", &save);
        }
        /* Last page */
        if (page_idx >= 0 && page_idx < count) {
            pages[page_idx] = strdup(page_buf);
            if (page_idx + 1 > actual) actual = page_idx + 1;
        }
        free(page_buf);
        free(text_copy);
    }

    *out_pages = pages;
    *out_npages = actual;
    return 0;
}

/* ===================================================================
 * Free pages allocated by ocr_split_pages
 * =================================================================== */
void ocr_free_pages(char **pages, int npages) {
    if (!pages) return;
    for (int i = 0; i < npages; i++) free(pages[i]);
    free(pages);
}
