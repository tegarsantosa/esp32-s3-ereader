#include "font.h"

extern const font_t font_sans_10, font_sans_12, font_sans_14, font_sans_16,
    font_sans_18, font_sans_20, font_sans_24, font_sans_28;

const font_t *const g_fonts[FONT_COUNT] = {
    &font_sans_10, &font_sans_12, &font_sans_14, &font_sans_16,
    &font_sans_18, &font_sans_20, &font_sans_24, &font_sans_28,
};

const font_glyph_t *font_glyph(const font_t *f, uint32_t cp)
{
    /* printable ASCII is stored contiguously at the start of the table */
    if (cp >= 0x20 && cp < 0x7F) {
        const font_glyph_t *g = &f->glyphs[cp - 0x20];
        if (g->cp == cp) return g;
    }
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t c = f->glyphs[mid].cp;
        if (c == cp) return &f->glyphs[mid];
        if (c < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

int font_index_for_px(int px)
{
    int best = 0;
    for (int i = 0; i < FONT_COUNT; i++) {
        if (g_fonts[i]->px <= px) best = i;
    }
    return best;
}
