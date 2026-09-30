#pragma once
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "font.h"

typedef void (*reader_progress_fn)(void *arg, const char *what, int percent);

typedef struct {
    uint32_t off;
    char *title;
} toc_entry_t;

typedef struct {
    uint16_t bg, fg, dim, accent;
} reader_colors_t;

typedef struct {
    char name[104];
    char title[104];
    FILE *txt;
    FILE *idx;
    uint32_t len;
    uint32_t npages;
    uint32_t page;
    /* text window */
    uint8_t *buf;
    uint32_t buf_off, buf_len;
    /* page index window */
    uint32_t pcache[64];
    uint32_t pcache_first;
    uint32_t pcache_n;
    /* layout */
    const font_t *font;
    const font_t *small;
    int x0, y0, w, h;
    int line_h, para_gap, indent;
    bool justify;
    bool status;
    /* chapters */
    toc_entry_t *toc;
    int ntoc;
} book_t;

/* opens (converting and paginating on first use) the book stored as
 * /books/<name>; returns 0 or -1 with a message in err */
int book_open(book_t *b, const char *name, reader_progress_fn cb, void *arg, char *err, size_t errsz);
void book_close(book_t *b);
/* re-runs pagination after a layout setting changed, keeping the position */
int book_relayout(book_t *b, reader_progress_fn cb, void *arg);

uint32_t book_page_offset(book_t *b, uint32_t page);
uint32_t book_page_for_offset(book_t *b, uint32_t off);
void book_goto_page(book_t *b, uint32_t page);
bool book_next(book_t *b);
bool book_prev(book_t *b);
int book_percent(book_t *b);
int book_chapter_index(book_t *b, uint32_t page); /* -1 if none */
void book_save_progress(book_t *b);

void book_draw(book_t *b, const reader_colors_t *c); /* draws the current page */
