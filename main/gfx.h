#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "font.h"

#define RGB565(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

/* Screen drawing. A frame is produced by gfx_render(), which calls the draw
 * function once per horizontal band; every primitive clips to the current
 * band, so draw functions must be free of side effects. */
typedef void (*gfx_draw_fn)(void *arg);

void gfx_render(gfx_draw_fn fn, void *arg);
int gfx_w(void);
int gfx_h(void);

uint16_t gfx_mix(uint16_t a, uint16_t b, int alpha255); /* alpha of b over a */

void gfx_fill(int x, int y, int w, int h, uint16_t c);
void gfx_fill_round(int x, int y, int w, int h, int r, uint16_t c);
void gfx_rect(int x, int y, int w, int h, uint16_t c);
void gfx_hline(int x, int y, int w, uint16_t c);

/* UTF-8 decode one code point; returns bytes consumed (>=1) */
int utf8_decode(const char *s, size_t len, uint32_t *cp);

/* Text: y is the TOP of the line box (baseline = y + font->ascent). */
int gfx_text_width(const font_t *f, const char *s, size_t len);
int gfx_char_width(const font_t *f, uint32_t cp);
int gfx_text(const font_t *f, int x, int y, const char *s, size_t len, uint16_t c);
int gfx_glyph(const font_t *f, int x, int y, uint32_t cp, uint16_t c);
/* draws string truncated with an ellipsis to fit max_w; returns drawn width */
int gfx_text_fit(const font_t *f, int x, int y, const char *s, int max_w, uint16_t c);
void gfx_text_center(const font_t *f, int y, const char *s, uint16_t c);
/* word-wrap helper: returns bytes of s that fit on one line of width w;
 * *next receives the offset where the next line starts */
size_t gfx_wrap_line(const font_t *f, const char *s, size_t len, int w, size_t *next);
/* draws wrapped text in a box, returns total height used */
int gfx_text_box(const font_t *f, int x, int y, int w, const char *s, uint16_t c, int line_h);
