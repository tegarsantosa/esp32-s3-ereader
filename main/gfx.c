#include "gfx.h"
#include "display.h"
#include <string.h>

#define SWAP16(c) (uint16_t)(((c) >> 8) | ((c) << 8))

static uint16_t *s_fb;     /* current band buffer */
static int s_by0, s_bh;    /* band y range */
static int s_w, s_h;

void gfx_render(gfx_draw_fn fn, void *arg)
{
    s_w = display_width();
    s_h = display_height();
    int rows = 0;
    int idx = 0;
    for (int y = 0; y < s_h; y += rows, idx ^= 1) {
        display_wait_buffer(idx);
        s_fb = display_band_buffer(idx, &rows);
        if (y + rows > s_h) rows = s_h - y;
        s_by0 = y;
        s_bh = rows;
        fn(arg);
        display_flush_band(idx, y, rows);
    }
    display_wait_all();
}

int gfx_w(void) { return display_width(); }
int gfx_h(void) { return display_height(); }

uint16_t gfx_mix(uint16_t a, uint16_t b, int al)
{
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + ((br - ar) * al) / 255;
    int g = ag + ((bg - ag) * al) / 255;
    int bl = ab + ((bb - ab) * al) / 255;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

void gfx_fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (x + w > s_w) w = s_w - x;
    int y0 = y < s_by0 ? s_by0 : y;
    int y1 = y + h > s_by0 + s_bh ? s_by0 + s_bh : y + h;
    if (w <= 0 || y1 <= y0) return;
    uint16_t sc = SWAP16(c);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *p = s_fb + (yy - s_by0) * s_w + x;
        for (int i = 0; i < w; i++) p[i] = sc;
    }
}

void gfx_hline(int x, int y, int w, uint16_t c) { gfx_fill(x, y, w, 1, c); }

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    gfx_fill(x, y, w, 1, c);
    gfx_fill(x, y + h - 1, w, 1, c);
    gfx_fill(x, y, 1, h, c);
    gfx_fill(x + w - 1, y, 1, h, c);
}

void gfx_fill_round(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int i = 0; i < r; i++) {
        /* inset for row i from the top/bottom of a circle of radius r */
        int dy = r - i;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r + r) dx++;
        int inset = r - dx;
        gfx_fill(x + inset, y + i, w - 2 * inset, 1, c);
        gfx_fill(x + inset, y + h - 1 - i, w - 2 * inset, 1, c);
    }
    gfx_fill(x, y + r, w, h - 2 * r, c);
}

int utf8_decode(const char *str, size_t len, uint32_t *cp)
{
    const uint8_t *s = (const uint8_t *)str;
    if (len == 0) { *cp = 0; return 1; }
    uint8_t c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int n;
    uint32_t v;
    if ((c & 0xE0) == 0xC0) { n = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if ((size_t)n > len) { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return n;
}

static const font_glyph_t *glyph_or_fallback(const font_t *f, uint32_t cp)
{
    const font_glyph_t *g = font_glyph(f, cp);
    if (g) return g;
    switch (cp) {
    case 0x2032: case 0x02BC: case 0x2035: cp = '\''; break;
    case 0x2033: cp = '"'; break;
    case 0x2043: case 0x2027: cp = '-'; break;
    case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F: case 0x3000: cp = ' '; break;
    default: cp = 0xFFFD; break;
    }
    g = font_glyph(f, cp);
    return g ? g : font_glyph(f, '?');
}

int gfx_char_width(const font_t *f, uint32_t cp)
{
    if (cp == 0xAD || cp == 0x200B || cp == 0xFEFF || cp == '\r') return 0;
    const font_glyph_t *g = glyph_or_fallback(f, cp);
    return g ? g->adv : 0;
}

int gfx_text_width(const font_t *f, const char *s, size_t len)
{
    int w = 0;
    size_t i = 0;
    while (i < len && s[i]) {
        uint32_t cp;
        i += utf8_decode(s + i, len - i, &cp);
        w += gfx_char_width(f, cp);
    }
    return w;
}

int gfx_glyph(const font_t *f, int x, int y, uint32_t cp, uint16_t color)
{
    if (cp == 0xAD || cp == 0x200B || cp == 0xFEFF || cp == '\r') return 0;
    const font_glyph_t *g = glyph_or_fallback(f, cp);
    if (!g) return 0;
    int top = y + f->ascent - g->yo;
    int left = x + g->xo;
    if (top >= s_by0 + s_bh || top + g->h <= s_by0 || g->w == 0) return g->adv;
    const uint8_t *bits = f->bitmap + g->off;
    int r0 = s_by0 > top ? s_by0 - top : 0;
    int r1 = top + g->h > s_by0 + s_bh ? s_by0 + s_bh - top : g->h;
    for (int r = r0; r < r1; r++) {
        int py = top + r;
        uint16_t *row = s_fb + (py - s_by0) * s_w;
        int nib = r * g->w;
        for (int col = 0; col < g->w; col++, nib++) {
            int px = left + col;
            uint8_t b = bits[nib >> 1];
            int a = (nib & 1) ? (b & 0x0F) : (b >> 4);
            if (!a || px < 0 || px >= s_w) continue;
            if (a == 15) {
                row[px] = SWAP16(color);
            } else {
                uint16_t bg = SWAP16(row[px]);
                row[px] = SWAP16(gfx_mix(bg, color, a * 17));
            }
        }
    }
    return g->adv;
}

int gfx_text(const font_t *f, int x, int y, const char *s, size_t len, uint16_t c)
{
    int x0 = x;
    size_t i = 0;
    while (i < len && s[i]) {
        uint32_t cp;
        i += utf8_decode(s + i, len - i, &cp);
        x += gfx_glyph(f, x, y, cp, c);
    }
    return x - x0;
}

int gfx_text_fit(const font_t *f, int x, int y, const char *s, int max_w, uint16_t c)
{
    size_t len = strlen(s);
    int w = gfx_text_width(f, s, len);
    if (w <= max_w) return gfx_text(f, x, y, s, len, c);
    int ell = gfx_char_width(f, 0x2026);
    int acc = 0;
    size_t i = 0, fit = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        int cw = gfx_char_width(f, cp);
        if (acc + cw + ell > max_w) break;
        acc += cw;
        i += n;
        fit = i;
    }
    gfx_text(f, x, y, s, fit, c);
    gfx_glyph(f, x + acc, y, 0x2026, c);
    return acc + ell;
}

void gfx_text_center(const font_t *f, int y, const char *s, uint16_t c)
{
    int w = gfx_text_width(f, s, strlen(s));
    int x = (s_w - w) / 2;
    if (x < 2) x = 2;
    gfx_text_fit(f, x, y, s, s_w - 4, c);
}

size_t gfx_wrap_line(const font_t *f, const char *s, size_t len, int w, size_t *next)
{
    size_t i = 0, brk = 0, brk_next = 0;
    int x = 0;
    while (i < len) {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (cp == '\n') { *next = i + 1; return i; }
        int cw = gfx_char_width(f, cp);
        if (cp == ' ') { brk = i; brk_next = i + 1; }
        if (x + cw > w && cp != ' ') {
            if (brk) { *next = brk_next; return brk; }
            if (i == 0) i = n;
            *next = i;
            return i;
        }
        x += cw;
        i += n;
    }
    *next = len;
    return len;
}

int gfx_text_box(const font_t *f, int x, int y, int w, const char *s, uint16_t c, int line_h)
{
    size_t len = strlen(s);
    int y0 = y;
    while (len > 0) {
        size_t next;
        size_t n = gfx_wrap_line(f, s, len, w, &next);
        gfx_text(f, x, y, s, n, c);
        y += line_h;
        s += next;
        len -= next;
    }
    return y - y0;
}
