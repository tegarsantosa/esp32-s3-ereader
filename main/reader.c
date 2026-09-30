/*
 * Book reader: text layout (word wrap, justification, paragraphs),
 * pagination with an on-flash page index, chapters and progress.
 */
#include "reader.h"
#include "gfx.h"
#include "settings.h"
#include "library.h"
#include "convert.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define BUF_SIZE 16384
#define PAGE_MAX 8192   /* a page never spans more bytes than this */
#define PG_MAGIC 0x31584750u /* "PGX1" */

typedef struct {
    uint32_t magic, sig, len, npages;
} pg_header_t;

/* ---------------- text window ---------------- */

static void ensure(book_t *b, uint32_t off)
{
    uint32_t need_from = off > 0 ? off - 1 : 0;
    uint32_t need_to = off + PAGE_MAX;
    if (need_to > b->len) need_to = b->len;
    if (need_from >= b->buf_off && need_to <= b->buf_off + b->buf_len) return;
    fseek(b->txt, need_from, SEEK_SET);
    b->buf_off = need_from;
    b->buf_len = (uint32_t)fread(b->buf, 1, BUF_SIZE, b->txt);
}

static inline uint8_t byte_at(book_t *b, uint32_t off)
{
    return b->buf[off - b->buf_off];
}

/* ---------------- line layout ---------------- */

typedef struct {
    uint32_t start, end, next;
    int width;     /* including indent, excluding trailing spaces */
    int spaces;    /* stretchable spaces inside [start, end) */
    bool para_start, para_end;
} line_t;

static void layout_line(book_t *b, uint32_t pos, uint32_t limit, line_t *ln)
{
    const font_t *f = b->font;
    ln->para_start = pos == 0 || byte_at(b, pos - 1) == '\n';
    int x = (ln->para_start && b->indent) ? b->indent : 0;
    while (pos < limit && byte_at(b, pos) == ' ') pos++;
    ln->start = pos;
    uint32_t brk_end = 0, brk_next = 0;
    int brk_w = 0, brk_sp = 0, nsp = 0;
    uint32_t i = pos;
    const int sw = gfx_char_width(f, ' ');
    while (i < limit) {
        uint8_t c = byte_at(b, i);
        if (c == '\n') {
            ln->end = i;
            ln->next = i + 1;
            ln->width = x;
            ln->spaces = nsp;
            ln->para_end = true;
            return;
        }
        uint32_t cp;
        int n = utf8_decode((const char *)b->buf + (i - b->buf_off), limit - i, &cp);
        if (cp == ' ') {
            if (i > ln->start && byte_at(b, i - 1) != ' ') {
                brk_end = i;
                brk_w = x;
                brk_sp = nsp;
                nsp++;
            }
            brk_next = i + 1;
            x += sw;
            i += n;
            continue;
        }
        int cw = gfx_char_width(f, cp);
        if (x + cw > b->w) {
            if (brk_end > ln->start) {
                ln->end = brk_end;
                ln->next = brk_next > brk_end ? brk_next : brk_end;
                ln->width = brk_w;
                ln->spaces = brk_sp;
            } else if (i == ln->start) {
                ln->end = ln->next = i + n; /* a single glyph wider than the line */
                ln->width = x + cw;
                ln->spaces = 0;
            } else {
                ln->end = ln->next = i; /* word longer than the line: hard break */
                ln->width = x;
                ln->spaces = nsp;
            }
            ln->para_end = false;
            return;
        }
        x += cw;
        i += n;
        /* allow breaking after dashes and slashes inside words */
        if ((cp == '-' || cp == 0x2013 || cp == 0x2014 || cp == '/') && i < limit && byte_at(b, i) != ' ' &&
            byte_at(b, i) != '\n') {
            brk_end = brk_next = i;
            brk_w = x;
            brk_sp = nsp;
        }
    }
    ln->end = ln->next = i;
    ln->width = x;
    ln->spaces = nsp;
    ln->para_end = true;
}

static void draw_line(book_t *b, const line_t *ln, int y, uint16_t color)
{
    const font_t *f = b->font;
    int x = b->x0 + ((ln->para_start && b->indent) ? b->indent : 0);
    int extra = 0, per = 0, rem = 0;
    if (b->justify && !ln->para_end && ln->spaces > 0) {
        extra = b->w - ln->width;
        int sw = gfx_char_width(f, ' ');
        if (extra > 0 && extra <= ln->spaces * sw * 2) {
            per = extra / ln->spaces;
            rem = extra % ln->spaces;
        }
    }
    const int sw = gfx_char_width(f, ' ');
    int k = 0;
    uint32_t i = ln->start;
    while (i < ln->end) {
        uint32_t cp;
        int n = utf8_decode((const char *)b->buf + (i - b->buf_off), ln->end - i, &cp);
        if (cp == ' ') {
            x += sw;
            if (i > ln->start && byte_at(b, i - 1) != ' ') {
                x += per + (k < rem ? 1 : 0);
                k++;
            }
        } else {
            x += gfx_glyph(f, x, y, cp, color);
        }
        i += n;
    }
}

/* lays out one page starting at 'start'; returns the start of the next page */
static uint32_t layout_page(book_t *b, uint32_t start, bool draw, uint16_t color)
{
    ensure(b, start);
    uint32_t limit = start + PAGE_MAX;
    if (limit > b->len) limit = b->len;
    if (limit > b->buf_off + b->buf_len) limit = b->buf_off + b->buf_len;
    uint32_t pos = start;
    int y = b->y0;
    const int bottom = b->y0 + b->h;
    bool first = true;
    while (pos < limit) {
        line_t ln;
        layout_line(b, pos, limit, &ln);
        bool empty = ln.start == ln.end;
        if (empty && ln.para_end && first) { /* no blank lines at the top of a page */
            pos = ln.next;
            continue;
        }
        if (y + b->line_h > bottom && !first) break;
        if (draw && !empty) draw_line(b, &ln, y, color);
        y += b->line_h;
        if (ln.para_end) y += b->para_gap;
        pos = ln.next;
        first = false;
    }
    /* skip paragraph breaks so the next page starts at text */
    while (pos < b->len) {
        if (pos >= b->buf_off + b->buf_len) break;
        if (byte_at(b, pos) != '\n') break;
        pos++;
    }
    return pos > start ? pos : start + 1;
}

/* ---------------- layout setup / pagination ---------------- */

static uint32_t setup_layout(book_t *b)
{
    int W = gfx_w(), H = gfx_h();
    int mn = W < H ? W : H;
    b->font = g_fonts[g_set.font_idx < FONT_COUNT ? g_set.font_idx : 0];
    b->small = g_fonts[mn <= 160 ? 0 : 1];
    int base = mn / 32;
    if (base < 3) base = 3;
    int margin = g_set.margin == 0 ? (base + 1) / 2 : g_set.margin == 2 ? base * 2 : base;
    int lh = b->font->line_h;
    b->line_h = g_set.line_sp == 0 ? lh * 88 / 100 : g_set.line_sp == 2 ? lh * 125 / 100 : lh;
    if (b->line_h < b->font->px) b->line_h = b->font->px;
    b->status = g_set.status_bar;
    int status_h = b->status ? b->small->line_h + 3 : 0;
    b->x0 = margin;
    b->w = W - 2 * margin;
    b->y0 = margin > 2 ? margin / 2 + 1 : margin;
    b->h = H - b->y0 - status_h - (b->status ? 1 : margin / 2 + 1);
    if (b->h < b->line_h) b->h = b->line_h;
    b->indent = g_set.para == 0 ? b->font->px * 5 / 4 : 0;
    b->para_gap = g_set.para == 1 ? b->line_h / 2 : 0;
    b->justify = g_set.justify;
    /* signature of everything that changes where pages break */
    uint32_t sig = 2166136261u;
    int vals[] = {W, H, b->font->px, b->line_h, b->w, b->h, b->indent, b->para_gap, 3};
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        sig ^= (uint32_t)vals[i];
        sig *= 16777619u;
    }
    return sig;
}

static int paginate(book_t *b, const char *pg_path, uint32_t sig, reader_progress_fn cb, void *arg)
{
    char tmp[168];
    snprintf(tmp, sizeof(tmp), "%s.tmp", pg_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    pg_header_t hd = {PG_MAGIC, sig, b->len, 0};
    fwrite(&hd, sizeof(hd), 1, f);
    uint32_t batch[128];
    int nb = 0;
    uint32_t pos = 0, n = 0;
    int last_pct = -1;
    /* skip leading paragraph breaks */
    ensure(b, 0);
    while (pos < b->len && pos < b->buf_len && b->buf[pos] == '\n') pos++;
    while (pos < b->len) {
        batch[nb++] = pos;
        n++;
        if (nb == 128) { fwrite(batch, 4, nb, f); nb = 0; }
        pos = layout_page(b, pos, false, 0);
        int pct = (int)((uint64_t)pos * 100 / b->len);
        if (cb && pct != last_pct) { cb(arg, "Paginating", pct); last_pct = pct; }
    }
    if (n == 0) { batch[nb++] = 0; n = 1; }
    if (nb) fwrite(batch, 4, nb, f);
    hd.npages = n;
    fseek(f, 0, SEEK_SET);
    fwrite(&hd, sizeof(hd), 1, f);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) { remove(tmp); return -1; }
    remove(pg_path);
    if (rename(tmp, pg_path)) return -1;
    return 0;
}

static int open_index(book_t *b, reader_progress_fn cb, void *arg)
{
    char pg[160];
    library_paths(b->name, NULL, NULL, NULL, pg, sizeof(pg));
    if (b->idx) { fclose(b->idx); b->idx = NULL; }
    uint32_t sig = setup_layout(b);
    for (int attempt = 0; attempt < 2; attempt++) {
        FILE *f = fopen(pg, "rb");
        if (f) {
            pg_header_t hd;
            if (fread(&hd, sizeof(hd), 1, f) == 1 && hd.magic == PG_MAGIC && hd.sig == sig &&
                hd.len == b->len && hd.npages > 0) {
                b->idx = f;
                b->npages = hd.npages;
                b->pcache_n = 0;
                return 0;
            }
            fclose(f);
        }
        if (attempt == 0 && paginate(b, pg, sig, cb, arg) != 0) return -1;
    }
    return -1;
}

uint32_t book_page_offset(book_t *b, uint32_t page)
{
    if (page >= b->npages) return b->len;
    if (page < b->pcache_first || page >= b->pcache_first + b->pcache_n) {
        uint32_t first = page >= 16 ? page - 16 : 0;
        fseek(b->idx, sizeof(pg_header_t) + first * 4, SEEK_SET);
        b->pcache_n = (uint32_t)fread(b->pcache, 4, 64, b->idx);
        b->pcache_first = first;
        if (page >= first + b->pcache_n) return b->len;
    }
    return b->pcache[page - b->pcache_first];
}

uint32_t book_page_for_offset(book_t *b, uint32_t off)
{
    uint32_t lo = 0, hi = b->npages - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (book_page_offset(b, mid) <= off) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

/* ---------------- table of contents ---------------- */

static void load_toc(book_t *b, const char *toc_path)
{
    FILE *f = fopen(toc_path, "r");
    if (!f) return;
    char line[160];
    int cap = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char *t = tab + 1;
        size_t l = strlen(t);
        while (l && (t[l - 1] == '\n' || t[l - 1] == '\r')) t[--l] = 0;
        if (b->ntoc == cap) {
            cap = cap ? cap * 2 : 32;
            toc_entry_t *nt = realloc(b->toc, cap * sizeof(toc_entry_t));
            if (!nt) break;
            b->toc = nt;
        }
        b->toc[b->ntoc].off = (uint32_t)strtoul(line, NULL, 10);
        b->toc[b->ntoc].title = strdup(t);
        b->ntoc++;
        if (b->ntoc >= 4000) break;
    }
    fclose(f);
}

static bool cache_valid(const char *txt, const char *toc, uint32_t src_size)
{
    struct stat st;
    if (stat(txt, &st) != 0) return false;
    FILE *f = fopen(toc, "r");
    if (!f) return false;
    char line[32] = "";
    bool ok = fgets(line, sizeof(line), f) && line[0] == '#' && strtoul(line + 1, NULL, 10) == src_size;
    fclose(f);
    return ok;
}

typedef struct {
    reader_progress_fn cb;
    void *arg;
} conv_arg_t;

static void conv_cb(void *a, int pct)
{
    conv_arg_t *c = a;
    if (c->cb) c->cb(c->arg, "Preparing book", pct);
}

int book_open(book_t *b, const char *name, reader_progress_fn cb, void *arg, char *err, size_t esz)
{
    memset(b, 0, sizeof(*b));
    snprintf(b->name, sizeof(b->name), "%s", name);
    library_display_name(name, b->title, sizeof(b->title));
    char src[160], txt[160], toc[160];
    library_paths(name, src, txt, toc, NULL, sizeof(src));
    struct stat st;
    if (stat(src, &st) != 0) {
        snprintf(err, esz, "Book not found");
        return -1;
    }
    if (!cache_valid(txt, toc, (uint32_t)st.st_size)) {
        if (cb) cb(arg, "Preparing book", 0);
        conv_arg_t ca = {cb, arg};
        if (convert_book(src, txt, toc, conv_cb, &ca, err, esz) != 0) return -1;
    }
    b->txt = fopen(txt, "rb");
    b->buf = malloc(BUF_SIZE);
    if (!b->txt || !b->buf) {
        snprintf(err, esz, "Cannot open book");
        book_close(b);
        return -1;
    }
    fseek(b->txt, 0, SEEK_END);
    b->len = (uint32_t)ftell(b->txt);
    b->buf_off = 0xFFFFFFFF;
    b->buf_len = 0;
    load_toc(b, toc);
    if (open_index(b, cb, arg) != 0) {
        snprintf(err, esz, "Storage full (pagination)");
        book_close(b);
        return -1;
    }
    uint32_t off = 0;
    progress_get(name, &off, NULL);
    b->page = book_page_for_offset(b, off);
    return 0;
}

int book_relayout(book_t *b, reader_progress_fn cb, void *arg)
{
    uint32_t off = book_page_offset(b, b->page);
    if (open_index(b, cb, arg) != 0) return -1;
    b->page = book_page_for_offset(b, off);
    return 0;
}

void book_close(book_t *b)
{
    if (b->txt) fclose(b->txt);
    if (b->idx) fclose(b->idx);
    for (int i = 0; i < b->ntoc; i++) free(b->toc[i].title);
    free(b->toc);
    free(b->buf);
    memset(b, 0, sizeof(*b));
}

void book_goto_page(book_t *b, uint32_t page)
{
    if (b->npages == 0) return;
    b->page = page >= b->npages ? b->npages - 1 : page;
}

bool book_next(book_t *b)
{
    if (b->page + 1 >= b->npages) return false;
    b->page++;
    return true;
}

bool book_prev(book_t *b)
{
    if (b->page == 0) return false;
    b->page--;
    return true;
}

int book_percent(book_t *b)
{
    if (b->npages <= 1) return 100;
    return (int)((uint64_t)b->page * 100 / (b->npages - 1));
}

int book_chapter_index(book_t *b, uint32_t page)
{
    /* the last chapter that starts before the end of this page */
    uint32_t end = book_page_offset(b, page + 1);
    int best = -1;
    for (int i = 0; i < b->ntoc && b->toc[i].off < end; i++) best = i;
    return best;
}

void book_save_progress(book_t *b)
{
    if (!b->txt) return;
    progress_set(b->name, book_page_offset(b, b->page), book_percent(b));
}

/* ---------------- drawing ---------------- */

void book_draw(book_t *b, const reader_colors_t *c)
{
    int W = gfx_w(), H = gfx_h();
    gfx_fill(0, 0, W, H, c->bg);
    uint32_t start = book_page_offset(b, b->page);
    layout_page(b, start, true, c->fg);

    if (!b->status) return;
    const font_t *s = b->small;
    int sh = s->line_h + 3;
    int y = H - sh;
    /* progress line */
    int pw = (int)((uint64_t)W * (b->page + 1) / (b->npages ? b->npages : 1));
    gfx_fill(0, y, W, 1, gfx_mix(c->bg, c->dim, 90));
    gfx_fill(0, y, pw, 1, c->accent);
    char right[32];
    snprintf(right, sizeof(right), "%lu/%lu", (unsigned long)(b->page + 1), (unsigned long)b->npages);
    int rw = gfx_text_width(s, right, strlen(right));
    int ty = y + 2;
    gfx_text(s, W - b->x0 - rw, ty, right, strlen(right), c->dim);
    int ch = book_chapter_index(b, b->page);
    const char *left = ch >= 0 ? b->toc[ch].title : b->title;
    int avail = W - 2 * b->x0 - rw - 6;
    if (avail > 20) gfx_text_fit(s, b->x0, ty, left, avail, c->dim);
}
