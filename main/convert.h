#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "textout.h"

/* Converts .epub / .pdf / .txt / .html / .md books into the reader's plain
 * text cache format (see textout.h). Runs once per book, on first open. */

typedef void (*conv_progress_fn)(void *arg, int percent);

int convert_book(const char *src, const char *txt_path, const char *toc_path,
                 conv_progress_fn cb, void *arg, char *err, size_t errsz);

bool convert_supported(const char *filename);

/* individual converters (return 0 on success) */
int conv_epub(const char *src, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t errsz);
int conv_pdf(const char *src, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t errsz);
int conv_txt(const char *src, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t errsz);
int conv_md(const char *src, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t errsz);
int conv_html(const char *src, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t errsz);
