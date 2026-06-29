/**
 * ocr_helper.c — Shared OCR integration implementation.
 *
 * Provides ocr_cuda binary location, execution, and output parsing.
 */
#define _GNU_SOURCE
#include "ocr_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ===================================================================
 * Locate ocr_cuda binary — tries /proc/self/exe, then exe_path, then guess.
 * =================================================================== */
int ocr_find_binary(const char *exe_path, char *out_bin, size_t bin_size) {
    if (!out_bin || bin_size == 0) return -1;

    /* ── Strategy 1: /proc/self/exe (most reliable on Linux) ── */
#if defined(__linux__)
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

    /* ── Strategy 2: use argv[0] as provided by caller ── */
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

    /* ── Strategy 3: guess ── */
    snprintf(out_bin, bin_size, "./ocr_cuda");
    return (int)strlen(out_bin);
}

/* ===================================================================
 * Run ocr_cuda on a PDF file.
 *
 * ocr_cuda stdout format:
 *   Line 1:  ---OCR_CHAPTERS:{"chapters":[...]}---
 *   Lines 2+: OCR text, pages separated by a line containing <PAGE>
 *
 * Parameters:
 *   out_chapters  — receives the extracted JSON string (free by caller)
 *   out_text      — receives remaining output (free by caller)
 *   out_text_len  — receives length of out_text
 * Returns 0 on success, -1 on failure.
 * =================================================================== */
int ocr_run(const char *pdf_path, const char *ocr_bin,
            char **out_chapters, char **out_text, size_t *out_text_len) {
    if (out_chapters) *out_chapters = NULL;
    if (out_text) *out_text = NULL;
    if (out_text_len) *out_text_len = 0;

    /* Capture stderr to a temp file for diagnostics */
    char err_path[1024];
    snprintf(err_path, sizeof(err_path), "/tmp/ocr_stderr_%d.log", getpid());
    char cmd[4096];
    int cmd_len = snprintf(cmd, sizeof(cmd),
                           "\"%s\" \"%s\" --dpi 200 2>\"%s\"",
                           ocr_bin, pdf_path, err_path);
    if (cmd_len >= (int)sizeof(cmd)) {
        fprintf(stderr, "[ocr_helper] Command too long\n");
        return -1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "[ocr_helper] Failed to run: %s\n", ocr_bin);
        unlink(err_path);
        return -1;
    }

    /* ── Read first line: chapter JSON marker ── */
    char *chapters_line = NULL;
    size_t chapters_cap = 0;
    ssize_t chapters_len = getline(&chapters_line, &chapters_cap, fp);
    if (chapters_len <= 0) {
        fprintf(stderr, "[ocr_helper] No output from ocr_cuda\n");
        /* Dump stderr from the child process */
        {
            char err_line[4096];
            FILE *efp = fopen(err_path, "r");
            if (efp) {
                fprintf(stderr, "[ocr_helper] Stderr from ocr_cuda:\n");
                while (fgets(err_line, sizeof(err_line), efp))
                    fprintf(stderr, "  %s", err_line);
                fclose(efp);
            }
            unlink(err_path);
        }
        free(chapters_line);
        pclose(fp);
        return -1;
    }

    /* Strip trailing \n\r */
    while (chapters_len > 0 && (chapters_line[chapters_len-1] == '\n' ||
                                chapters_line[chapters_len-1] == '\r'))
        chapters_line[--chapters_len] = '\0';

    /* Parse ---OCR_CHAPTERS:{"chapters":[...]}--- */
    if (out_chapters) {
        char *json_start = strstr(chapters_line, "---OCR_CHAPTERS:");
        if (json_start) {
            json_start += 16;
            char *json_end = strstr(json_start, "---");
            if (json_end) *json_end = '\0';
            *out_chapters = strdup(json_start);
        } else {
            *out_chapters = strdup(chapters_line);
        }
    }
    free(chapters_line);

    /* ── Read remaining lines ── */
    size_t cap = 1048576;
    size_t len = 0;
    char *text = malloc(cap);
    if (!text) { unlink(err_path); pclose(fp); return -1; }
    text[0] = '\0';

    char line[16384];
    while (fgets(line, sizeof(line), fp)) {
        size_t ll = strlen(line);
        if (len + ll + 1 > cap) {
            cap = (cap * 3) / 2;  /* grow by 50% */
            char *tmp = realloc(text, cap);
            if (!tmp) { free(text); pclose(fp); return -1; }
            text = tmp;
        }
        memcpy(text + len, line, ll + 1);
        len += ll;
    }

    int status = pclose(fp);
    unlink(err_path);  /* clean up stderr capture file */

    if (status != 0 && len == 0) {
        fprintf(stderr, "[ocr_helper] OCR exit %d, no output\n", status);
        free(text);
        return -1;
    }

    if (out_text) *out_text = text;
    if (out_text_len) *out_text_len = len;
    return 0;
}

/* ===================================================================
 * Split OCR text into per-page array by <PAGE> markers.
 *
 * The text has page separator lines containing "<PAGE>".
 * Content before the first <PAGE> is treated as page 0.
 * Single-pass scan: no extra copies of the full text.
 *
 * Returns 0 on success, -1 on error.
 * Caller must free result with ocr_free_pages().
 * =================================================================== */
int ocr_split_pages(const char *text, size_t text_len,
                     char ***out_pages, int *out_npages) {
    *out_pages = NULL;
    *out_npages = 0;

    if (!text || text_len == 0) return -1;

    /* ── Count <PAGE> markers ── */
    int nmarkers = 0;
    for (const char *p = text; (p = strstr(p, "<PAGE>")) != NULL; nmarkers++, p += 6)
        ;

    /* If no markers, entire text is one page */
    if (nmarkers == 0) {
        *out_pages = malloc(sizeof(char *));
        if (!*out_pages) return -1;
        (*out_pages)[0] = malloc(text_len + 1);
        if (!(*out_pages)[0]) { free(*out_pages); *out_pages = NULL; return -1; }
        memcpy((*out_pages)[0], text, text_len);
        (*out_pages)[0][text_len] = '\0';
        *out_npages = 1;
        return 0;
    }

    /* n markers => n pages (each marker ends one page, content before first
     * marker is page 0, but in OCR output the first marker is at position 0) */
    int npages = nmarkers;
    /* Account for content before first marker */
    if (text_len > 0 && strstr(text, "<PAGE>") != text)
        npages++;

    char **pages = calloc(npages, sizeof(char *));
    if (!pages) return -1;

    /* ── Single-pass extraction ── */
    int idx = 0;
    const char *cur = text;
    const char *end = text + text_len;

    while (cur < end && idx < npages) {
        /* Find next <PAGE> marker */
        const char *page_start = cur;
        const char *marker = strstr(cur, "<PAGE>");

        const char *page_end;
        if (marker) {
            page_end = marker;          /* content ends before <PAGE> */
            cur = marker + 6;           /* skip past <PAGE> */
            if (*cur == '\n') cur++;    /* skip the newline after <PAGE> */
        } else {
            page_end = end;             /* last segment */
            cur = end;
        }

        size_t page_len = page_end - page_start;
        /* Trim trailing newline(s) */
        while (page_len > 0 && page_start[page_len - 1] == '\n')
            page_len--;

        /* Only create entry for non-empty segments.
         * Empty segments occur when text starts with <PAGE>
         * (first page has no content before the marker).
         * In that case npages already accounts for markers only
         * (not markers+1), so just skip without advancing idx. */
        if (page_len > 0) {
            pages[idx] = malloc(page_len + 1);
            if (!pages[idx]) { ocr_free_pages(pages, idx); return -1; }
            memcpy(pages[idx], page_start, page_len);
            pages[idx][page_len] = '\0';
            idx++;
        }
        /* else: empty segment, skip without advancing idx */
    }

    *out_pages = pages;
    *out_npages = idx;
    return 0;
}

/* ===================================================================
 * Free pages array
 * =================================================================== */
void ocr_free_pages(char **pages, int npages) {
    if (!pages) return;
    for (int i = 0; i < npages; i++) free(pages[i]);
    free(pages);
}
