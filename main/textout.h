#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Writer for the reader's internal text format:
 *   UTF-8 text, one paragraph per line ('\n'), an empty line = scene break.
 * Whitespace is collapsed automatically. A table of contents is written to a
 * second file as lines of "<byte offset>\t<title>\n". */
typedef struct {
    FILE *f;
    FILE *toc;
    char buf[2048];
    size_t n;
    uint32_t off;        /* bytes written so far (including buffer) */
    bool para_text;      /* current paragraph has visible text */
    bool pending_space;
    bool last_blank;     /* last paragraph written was an empty line */
    int toc_count;
    uint32_t last_toc_off;
} textout_t;

void tout_init(textout_t *t, FILE *f, FILE *toc);
void tout_text(textout_t *t, const char *s, size_t len); /* raw UTF-8, whitespace collapses */
void tout_cp(textout_t *t, uint32_t cp);                   /* one code point */
void tout_space(textout_t *t);
void tout_para(textout_t *t);                              /* end paragraph */
void tout_blank(textout_t *t);                             /* scene break (empty line) */
bool tout_unhyphen(textout_t *t);                          /* drop a trailing "-" (for re-joined lines) */
uint32_t tout_mark(textout_t *t);                          /* ends paragraph, returns offset */
void tout_toc(textout_t *t, uint32_t off, const char *title);
void tout_finish(textout_t *t);

int utf8_encode(uint32_t cp, char *out);
