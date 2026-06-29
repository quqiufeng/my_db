/**
 * ocr_helper.h — Shared OCR integration for import_book and pdf2md.
 *
 * Provides common functions for locating and running the ocr_cuda binary,
 * parsing its output into per-page text and chapter JSON.
 *
 * ocr_cuda output format:
 *   Line 1:  ---OCR_CHAPTERS:{"chapters":[...]}---
 *   Lines 2+: OCR text with <PAGE> markers separating pages
 */
#ifndef OCR_HELPER_H
#define OCR_HELPER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Locate the ocr_cuda binary relative to the current executable.
 *
 * If exe_path is the path to the current executable (e.g. argv[0]),
 * this replaces the last component with "ocr_cuda".
 * Falls back to "./ocr_cuda" if resolution fails.
 *
 * @param exe_path  Path to the current executable, or NULL.
 * @param out_bin   Output buffer for the ocr_cuda path.
 * @param bin_size  Size of out_bin.
 * @return Number of chars written (excluding NUL), or -1 on error.
 */
int ocr_find_binary(const char *exe_path, char *out_bin, size_t bin_size);

/**
 * Run ocr_cuda on a PDF file.
 *
 * Reads stdout:
 *   - First line (---OCR_CHAPTERS:{...}---) is returned in *out_chapters.
 *   - Remaining lines (OCR text with <PAGE> markers) are returned in *out_text.
 *
 * @param pdf_path      Path to the input PDF file.
 * @param ocr_bin       Path to the ocr_cuda binary.
 * @param out_chapters  Receives first-line chapters JSON (must free).
 *                      Set to NULL if not needed.
 * @param out_text      Receives remaining OCR text (must free).
 * @param out_text_len  Receives length of out_text (excluding NUL).
 * @return 0 on success, -1 on failure.
 */
int ocr_run(const char *pdf_path, const char *ocr_bin,
            char **out_chapters, char **out_text, size_t *out_text_len);

/**
 * Split OCR text (with <PAGE> markers) into per-page text array.
 *
 * The <PAGE> marker lines themselves are removed.
 * If no <PAGE> markers are found, the entire text is returned as one page.
 *
 * @param text      OCR text containing <PAGE> markers.
 * @param text_len  Length of text.
 * @param out_pages Receives allocated array of per-page strings (must
 *                  free with ocr_free_pages).
 * @param out_npages Receives number of pages.
 * @return 0 on success, -1 on error.
 */
int ocr_split_pages(const char *text, size_t text_len,
                    char ***out_pages, int *out_npages);

/**
 * Free pages array returned by ocr_split_pages.
 */
void ocr_free_pages(char **pages, int npages);

#ifdef __cplusplus
}
#endif

#endif /* OCR_HELPER_H */
