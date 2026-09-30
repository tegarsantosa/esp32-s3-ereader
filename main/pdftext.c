/*
 * Basic PDF text extraction.
 *
 * Instead of building the full PDF object graph (too heavy for a
 * microcontroller) the file is scanned for streams:
 *   pass 1: ToUnicode CMaps are collected (for fonts with custom encodings)
 *   pass 2: page content streams are decoded and their text operators
 *           (Tj, TJ, ', ") are turned into reflowable paragraphs.
 * Works for most text PDFs produced by Word, LibreOffice, LaTeX, Google Docs
 * and browsers. Scanned PDFs (images only) and encrypted PDFs have no
 * extractable text; for those the web uploader can convert in the browser.
 */
#include "convert.h"
#include "inflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

#define MAX_STREAM (256 * 1024)
#define MAP2_MAX 6000
#define STR_MAX 1024
#define ARR_MAX 4096

typedef struct {
    uint16_t code, seq;
    uint16_t u[3];
} map2_t;

typedef struct {
    uint16_t m1[256][3];
    bool m1_has[256];
    bool have1;
    map2_t *m2;
    int n2;
} cmap_t;

typedef struct {
    float a, b, c, d, e, f;
} mat_t;

typedef struct {
    textout_t *out;
    cmap_t *cm;
    mat_t ctm, tm, tlm;
    mat_t stack[16];
    int sp;
    float fsize, leading, tc, tw, th;
    float nums[8];
    int nnum;
    uint8_t str[STR_MAX];
    int slen;
    bool have_str;
    uint8_t arr[ARR_MAX];
    int alen;
    bool in_arr, have_arr;
    bool has_last;
    float last_x, last_y, last_fs;
} pctx_t;

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool pass1, decided;
} sink_t;

typedef struct {
    FILE *f;
    long size;
    cmap_t *cm;
    pctx_t *pc;
    sink_t sink;
    textout_t *out;
    uint8_t *chunk;
    uint8_t *dict;
    int pages;
    conv_progress_fn cb;
    void *arg;
    int base, span, last_pct;
} pdf_t;

/* ---------------- helpers ---------------- */

static bool is_ws(uint8_t c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }
static bool is_delim(uint8_t c) { return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' || c == '%'; }

static const uint8_t *mfind(const uint8_t *h, size_t hn, const char *needle)
{
    size_t nn = strlen(needle);
    if (nn > hn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == (uint8_t)needle[0] && memcmp(h + i, needle, nn) == 0) return h + i;
    return NULL;
}

static int hexv(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static mat_t mul(mat_t m, mat_t n)
{
    mat_t r = {
        m.a * n.a + m.b * n.c, m.a * n.b + m.b * n.d,
        m.c * n.a + m.d * n.c, m.c * n.b + m.d * n.d,
        m.e * n.a + m.f * n.c + n.e, m.e * n.b + m.f * n.d + n.f,
    };
    return r;
}

static const mat_t IDENT = {1, 0, 0, 1, 0, 0};

/* ---------------- CMaps ---------------- */

/* parses <hex> at p; returns pointer after '>' and fills bytes */
static const uint8_t *parse_hex(const uint8_t *p, const uint8_t *end, uint8_t *out, int max, int *n)
{
    *n = 0;
    if (p >= end || *p != '<') return NULL;
    p++;
    int hi = -1;
    while (p < end && *p != '>') {
        int v = hexv(*p++);
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else {
            if (*n < max) out[(*n)++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    if (hi >= 0 && *n < max) out[(*n)++] = (uint8_t)(hi << 4);
    return p < end ? p + 1 : end;
}

static const uint8_t *skip_ws(const uint8_t *p, const uint8_t *end)
{
    while (p < end && is_ws(*p)) p++;
    return p;
}

static void cmap_add(cmap_t *cm, const uint8_t *src, int sn, const uint8_t *dst, int dn, int add)
{
    uint16_t u[3] = {0, 0, 0};
    int nu = dn / 2;
    if (nu > 3) nu = 3;
    for (int i = 0; i < nu; i++) u[i] = (uint16_t)(dst[2 * i] << 8 | dst[2 * i + 1]);
    if (nu == 0) return;
    u[nu - 1] = (uint16_t)(u[nu - 1] + add);
    if (sn == 1) {
        uint8_t c = (uint8_t)(src[0] + add);
        if (!cm->m1_has[c]) {
            memcpy(cm->m1[c], u, sizeof(u));
            cm->m1_has[c] = true;
            cm->have1 = true;
        }
    } else if (sn == 2) {
        if (cm->n2 >= MAP2_MAX) return;
        if (!cm->m2) {
            cm->m2 = malloc(sizeof(map2_t) * MAP2_MAX);
            if (!cm->m2) return;
        }
        map2_t *m = &cm->m2[cm->n2];
        m->code = (uint16_t)((src[0] << 8 | src[1]) + add);
        m->seq = (uint16_t)cm->n2;
        memcpy(m->u, u, sizeof(u));
        cm->n2++;
    }
}

static void cmap_parse(cmap_t *cm, const uint8_t *buf, size_t len)
{
    const uint8_t *end = buf + len, *p = buf;
    uint8_t a[8], b[8], d[16];
    int an, bn, dn;
    while (p < end) {
        const uint8_t *chr = mfind(p, end - p, "beginbfchar");
        const uint8_t *rng = mfind(p, end - p, "beginbfrange");
        if (!chr && !rng) break;
        bool is_rng = rng && (!chr || rng < chr);
        p = (is_rng ? rng + 12 : chr + 11);
        for (;;) {
            p = skip_ws(p, end);
            if (p >= end || *p != '<') break;
            p = parse_hex(p, end, a, sizeof(a), &an);
            p = skip_ws(p, end);
            if (!is_rng) {
                if (p >= end || *p != '<') break;
                p = parse_hex(p, end, d, sizeof(d), &dn);
                cmap_add(cm, a, an, d, dn, 0);
                continue;
            }
            p = parse_hex(p, end, b, sizeof(b), &bn);
            if (!p) break;
            p = skip_ws(p, end);
            if (p >= end) break;
            int lo = an == 1 ? a[0] : (a[0] << 8 | a[1]);
            int hi = bn == 1 ? b[0] : (b[0] << 8 | b[1]);
            if (hi < lo || hi - lo > 4096) hi = lo;
            if (*p == '<') {
                p = parse_hex(p, end, d, sizeof(d), &dn);
                for (int k = 0; k <= hi - lo; k++) cmap_add(cm, a, an, d, dn, k);
            } else if (*p == '[') {
                p++;
                for (int k = 0; p < end; k++) {
                    p = skip_ws(p, end);
                    if (p >= end || *p == ']') { p++; break; }
                    if (*p != '<') { p++; continue; }
                    p = parse_hex(p, end, d, sizeof(d), &dn);
                    if (k <= hi - lo) {
                        uint8_t src[2];
                        int code = lo + k;
                        if (an == 1) src[0] = (uint8_t)code;
                        else { src[0] = (uint8_t)(code >> 8); src[1] = (uint8_t)code; }
                        cmap_add(cm, src, an, d, dn, 0);
                    }
                }
            } else {
                break;
            }
        }
    }
}

static int map2_cmp(const void *x, const void *y)
{
    const map2_t *a = x, *b = y;
    if (a->code != b->code) return a->code - b->code;
    return a->seq - b->seq;
}

static const map2_t *map2_find(const cmap_t *cm, uint16_t code)
{
    int lo = 0, hi = cm->n2 - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (cm->m2[mid].code == code) {
            while (mid > 0 && cm->m2[mid - 1].code == code) mid--; /* first definition wins */
            return &cm->m2[mid];
        }
        if (cm->m2[mid].code < code) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* ---------------- text output ---------------- */

static const uint16_t CP1252_HI[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0, 0x017D, 0, 0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178};

static int emit_u16(pctx_t *c, const uint16_t *u, int n, int *spaces)
{
    int g = 0;
    for (int i = 0; i < n && u[i]; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp == 0xFB00) { tout_text(c->out, "ff", 2); g += 2; continue; }
        if (cp == 0xFB01) { tout_text(c->out, "fi", 2); g += 2; continue; }
        if (cp == 0xFB02) { tout_text(c->out, "fl", 2); g += 2; continue; }
        if (cp == 0xFB03) { tout_text(c->out, "ffi", 3); g += 3; continue; }
        if (cp == 0xFB04) { tout_text(c->out, "ffl", 3); g += 3; continue; }
        if (cp == ' ' || cp == 0xA0) (*spaces)++;
        if (cp < 0x20) cp = ' ';
        tout_cp(c->out, cp);
        g++;
    }
    return g;
}

/* decodes a PDF string and writes it; returns the glyph count */
static int emit_string(pctx_t *c, const uint8_t *s, int n, int *spaces)
{
    cmap_t *cm = c->cm;
    int g = 0;
    *spaces = 0;
    if (cm->n2 && n >= 2 && (n % 2) == 0) {
        int found = 0;
        for (int i = 0; i < n; i += 2)
            if (map2_find(cm, (uint16_t)(s[i] << 8 | s[i + 1]))) found++;
        if (found * 5 >= (n / 2) * 4) {
            for (int i = 0; i < n; i += 2) {
                const map2_t *m = map2_find(cm, (uint16_t)(s[i] << 8 | s[i + 1]));
                if (m) g += emit_u16(c, m->u, 3, spaces);
            }
            return g;
        }
    }
    int ctrl = 0, zeros = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < 0x20) ctrl++;
        if (s[i] == 0) zeros++;
    }
    if (cm->have1 && ctrl) {
        bool all = true;
        for (int i = 0; i < n; i++)
            if (!cm->m1_has[s[i]]) { all = false; break; }
        if (all) {
            for (int i = 0; i < n; i++) g += emit_u16(c, cm->m1[s[i]], 3, spaces);
            return g;
        }
    }
    if (zeros * 3 > n) return n / 2; /* 2-byte glyph ids without a map: unreadable */
    for (int i = 0; i < n; i++) {
        uint8_t b = s[i];
        if (b < 0x20) {
            /* TeX OT1 ligature slots */
            static const char *const lig[5] = {"ff", "fi", "fl", "ffi", "ffl"};
            if (b >= 0x0B && b <= 0x0F) { tout_text(c->out, lig[b - 0x0B], strlen(lig[b - 0x0B])); g += 2; }
            continue;
        }
        uint32_t cp = b;
        if (b >= 0x80 && b < 0xA0) cp = CP1252_HI[b - 0x80];
        if (!cp) continue;
        if (cp == ' ') (*spaces)++;
        tout_cp(c->out, cp);
        g++;
    }
    return g;
}

/* true when the string would render only spaces */
static bool string_blank(pctx_t *c, const uint8_t *s, int n)
{
    bool raw_blank = true;
    for (int i = 0; i < n; i++)
        if (s[i] != ' ') { raw_blank = false; break; }
    if (raw_blank) return true;
    if (c->cm->n2 && (n % 2) == 0) {
        for (int i = 0; i < n; i += 2) {
            const map2_t *m = map2_find(c->cm, (uint16_t)(s[i] << 8 | s[i + 1]));
            if (!m || (m->u[0] != ' ' && m->u[0] != 0xA0)) return false;
        }
        return true;
    }
    return false;
}

static void show(pctx_t *c, const uint8_t *s, int n)
{
    if (string_blank(c, s, n)) {
        float tx = (n * (0.5f * c->fsize + c->tc + c->tw)) * c->th;
        c->tm.e += tx * c->tm.a;
        c->tm.f += tx * c->tm.b;
        return;
    }
    mat_t trm = mul(c->tm, c->ctm);
    float x = trm.e, y = trm.f;
    float scale = fabsf(trm.d) > 0.01f ? fabsf(trm.d) : fabsf(trm.b);
    if (scale < 0.01f) scale = 1;
    float fs = c->fsize * scale;
    if (fs < 1) fs = 1;
    if (c->has_last) {
        float ref = fs > c->last_fs ? fs : c->last_fs;
        float dy = c->last_y - y;
        if (fabsf(dy) < 0.5f * ref) {
            float gap = x - c->last_x;
            if (gap > 0.25f * ref || gap < -3 * ref) tout_space(c->out);
        } else if (dy > 0 && dy < 1.9f * ref && fs < c->last_fs * 1.25f && c->last_fs < fs * 1.25f) {
            if (!tout_unhyphen(c->out)) tout_space(c->out);
        } else {
            tout_para(c->out);
        }
    }
    int spaces;
    int g = emit_string(c, s, n, &spaces);
    float tx = (g * (0.5f * c->fsize + c->tc) + spaces * c->tw) * c->th;
    c->tm.e += tx * c->tm.a;
    c->tm.f += tx * c->tm.b;
    mat_t t2 = mul(c->tm, c->ctm);
    c->last_x = t2.e;
    c->last_y = y;
    c->last_fs = fs;
    c->has_last = true;
}

static void td(pctx_t *c, float tx, float ty)
{
    mat_t t = {1, 0, 0, 1, tx, ty};
    c->tlm = mul(t, c->tlm);
    c->tm = c->tlm;
}

/* ---------------- content stream tokenizer ---------------- */

static const uint8_t *lit_string(const uint8_t *p, const uint8_t *end, uint8_t *out, int *n)
{
    int depth = 1;
    *n = 0;
    while (p < end) {
        uint8_t ch = *p++;
        int v;
        if (ch == '\\') {
            if (p >= end) break;
            uint8_t e = *p++;
            switch (e) {
            case 'n': v = '\n'; break;
            case 'r': v = '\r'; break;
            case 't': v = '\t'; break;
            case 'b': v = '\b'; break;
            case 'f': v = '\f'; break;
            case '\r': if (p < end && *p == '\n') p++; continue;
            case '\n': continue;
            default:
                if (e >= '0' && e <= '7') {
                    v = e - '0';
                    for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; k++) v = v * 8 + (*p++ - '0');
                } else {
                    v = e;
                }
            }
        } else if (ch == '(') {
            depth++;
            v = ch;
        } else if (ch == ')') {
            if (--depth == 0) break;
            v = ch;
        } else {
            v = ch;
        }
        if (*n < STR_MAX) out[(*n)++] = (uint8_t)v;
    }
    return p;
}

static void arr_add(pctx_t *c, uint8_t type, const void *data, int n)
{
    if (c->alen + 3 + n > ARR_MAX) return;
    c->arr[c->alen++] = type;
    c->arr[c->alen++] = (uint8_t)(n & 0xFF);
    c->arr[c->alen++] = (uint8_t)(n >> 8);
    memcpy(c->arr + c->alen, data, n);
    c->alen += n;
}

static void push_num(pctx_t *c, float v)
{
    if (c->in_arr) { arr_add(c, 'n', &v, sizeof(v)); return; }
    if (c->nnum == 8) { memmove(c->nums, c->nums + 1, 7 * sizeof(float)); c->nnum = 7; }
    c->nums[c->nnum++] = v;
}

static void got_string(pctx_t *c, const uint8_t *s, int n)
{
    if (c->in_arr) { arr_add(c, 's', s, n); return; }
    memcpy(c->str, s, n);
    c->slen = n;
    c->have_str = true;
}

static float num(pctx_t *c, int from_end)
{
    int i = c->nnum - 1 - from_end;
    return i >= 0 ? c->nums[i] : 0;
}

static const uint8_t *op(pctx_t *c, const char *o, size_t n, const uint8_t *p, const uint8_t *end)
{
#define IS(s) (n == sizeof(s) - 1 && memcmp(o, s, n) == 0)
    if (IS("BT")) { c->tm = c->tlm = IDENT; }
    else if (IS("Tf")) { if (c->nnum >= 1) c->fsize = num(c, 0); }
    else if (IS("Td")) { if (c->nnum >= 2) td(c, num(c, 1), num(c, 0)); }
    else if (IS("TD")) { if (c->nnum >= 2) { c->leading = -num(c, 0); td(c, num(c, 1), num(c, 0)); } }
    else if (IS("T*")) { td(c, 0, -c->leading); }
    else if (IS("TL")) { c->leading = num(c, 0); }
    else if (IS("Tc")) { c->tc = num(c, 0); }
    else if (IS("Tw")) { c->tw = num(c, 0); }
    else if (IS("Tz")) { c->th = num(c, 0) / 100.0f; }
    else if (IS("Tm")) {
        if (c->nnum >= 6) {
            mat_t m = {num(c, 5), num(c, 4), num(c, 3), num(c, 2), num(c, 1), num(c, 0)};
            c->tm = c->tlm = m;
        }
    }
    else if (IS("Tj")) { if (c->have_str) show(c, c->str, c->slen); }
    else if (IS("'")) { td(c, 0, -c->leading); if (c->have_str) show(c, c->str, c->slen); }
    else if (IS("\"")) {
        if (c->nnum >= 2) { c->tw = num(c, 1); c->tc = num(c, 0); }
        td(c, 0, -c->leading);
        if (c->have_str) show(c, c->str, c->slen);
    }
    else if (IS("TJ")) {
        if (c->have_arr) {
            int i = 0;
            while (i + 3 <= c->alen) {
                uint8_t t = c->arr[i];
                int len = c->arr[i + 1] | (c->arr[i + 2] << 8);
                const uint8_t *d = c->arr + i + 3;
                if (t == 's') {
                    show(c, d, len);
                } else if (t == 'n') {
                    float v;
                    memcpy(&v, d, sizeof(v));
                    float tx = -v / 1000.0f * c->fsize * c->th;
                    c->tm.e += tx * c->tm.a;
                    c->tm.f += tx * c->tm.b;
                    if (v < -250) tout_space(c->out);
                    mat_t t2 = mul(c->tm, c->ctm);
                    c->last_x = t2.e;
                }
                i += 3 + len;
            }
        }
    }
    else if (IS("q")) { if (c->sp < 16) c->stack[c->sp++] = c->ctm; }
    else if (IS("Q")) { if (c->sp > 0) c->ctm = c->stack[--c->sp]; }
    else if (IS("cm")) {
        if (c->nnum >= 6) {
            mat_t m = {num(c, 5), num(c, 4), num(c, 3), num(c, 2), num(c, 1), num(c, 0)};
            c->ctm = mul(m, c->ctm);
        }
    }
    else if (IS("BI")) {
        /* inline image: skip to "ID", then the binary data up to "EI" */
        const uint8_t *q = p;
        while (q + 2 < end && !(is_ws(q[0]) && q[1] == 'I' && q[2] == 'D')) q++;
        q += 3;
        while (q + 2 < end && !(is_ws(q[0]) && q[1] == 'E' && q[2] == 'I' && (q + 3 == end || is_ws(q[3])))) q++;
        p = q + 3 < end ? q + 3 : end;
    }
#undef IS
    c->nnum = 0;
    c->have_str = false;
    c->have_arr = false;
    return p;
}

static void parse_content(pctx_t *c, const uint8_t *p, size_t len)
{
    const uint8_t *end = p + len;
    static uint8_t sbuf[STR_MAX];
    while (p < end) {
        uint8_t ch = *p;
        if (is_ws(ch)) { p++; continue; }
        if (ch == '%') { while (p < end && *p != '\n' && *p != '\r') p++; continue; }
        if (ch == '(') {
            int n;
            p = lit_string(p + 1, end, sbuf, &n);
            got_string(c, sbuf, n);
            continue;
        }
        if (ch == '<') {
            if (p + 1 < end && p[1] == '<') { p += 2; continue; }
            int n;
            const uint8_t *q = parse_hex(p, end, sbuf, STR_MAX, &n);
            p = q ? q : end;
            got_string(c, sbuf, n);
            continue;
        }
        if (ch == '>' || ch == '{' || ch == '}' || ch == ')') { p++; continue; }
        if (ch == '[') { c->in_arr = true; c->alen = 0; p++; continue; }
        if (ch == ']') { c->in_arr = false; c->have_arr = true; p++; continue; }
        if (ch == '/') {
            p++;
            while (p < end && !is_ws(*p) && !is_delim(*p)) p++;
            continue;
        }
        if (isdigit(ch) || ch == '-' || ch == '+' || ch == '.') {
            char nb[24];
            int k = 0;
            while (p < end && (isdigit(*p) || *p == '-' || *p == '+' || *p == '.') && k < 23) nb[k++] = (char)*p++;
            nb[k] = 0;
            push_num(c, strtof(nb, NULL));
            continue;
        }
        const uint8_t *s = p;
        while (p < end && !is_ws(*p) && !is_delim(*p)) p++;
        if (p == s) { p++; continue; }
        p = op(c, (const char *)s, (size_t)(p - s), p, end);
    }
}

/* ---------------- stream scanning ---------------- */

typedef struct {
    FILE *f;
    long remaining;
    uint8_t buf[1024];
    size_t pos, len;
    int pushback;
} frd_t;

static int frd_byte(void *ctx)
{
    frd_t *r = ctx;
    if (r->pushback >= 0) { int b = r->pushback; r->pushback = -1; return b; }
    if (r->pos == r->len) {
        if (r->remaining <= 0) return -1;
        size_t want = r->remaining < (long)sizeof(r->buf) ? (size_t)r->remaining : sizeof(r->buf);
        r->len = fread(r->buf, 1, want, r->f);
        r->pos = 0;
        if (r->len == 0) return -1;
        r->remaining -= (long)r->len;
    }
    return r->buf[r->pos++];
}

static int sink_out(void *ctx, const uint8_t *d, size_t n)
{
    sink_t *s = ctx;
    if (s->len + n > s->cap) {
        size_t nc = s->cap ? s->cap : 16384;
        while (nc < s->len + n) nc *= 2;
        if (nc > MAX_STREAM) nc = MAX_STREAM;
        if (nc > s->cap) {
            uint8_t *nb = realloc(s->buf, nc);
            if (nb) { s->buf = nb; s->cap = nc; }
        }
        if (s->len + n > s->cap) n = s->cap - s->len;
        if (n == 0) return 1;
    }
    memcpy(s->buf + s->len, d, n);
    s->len += n;
    if (s->pass1 && !s->decided && s->len >= 600) {
        s->decided = true;
        if (!mfind(s->buf, s->len, "begincmap") && !mfind(s->buf, s->len, "CIDInit") &&
            !mfind(s->buf, s->len, "beginbf"))
            return 1;
    }
    return s->len >= MAX_STREAM;
}

static bool dict_has(const uint8_t *d, size_t n, const char *key) { return mfind(d, n, key) != NULL; }

static long dict_length(const uint8_t *d, size_t n)
{
    const uint8_t *p = mfind(d, n, "/Length");
    if (!p) return -1;
    const uint8_t *end = d + n;
    p += 7;
    if (p < end && isalpha(*p)) return -1; /* /Length1 etc */
    p = skip_ws(p, end);
    long v = 0;
    const uint8_t *q = p;
    while (q < end && isdigit(*q)) v = v * 10 + (*q++ - '0');
    if (q == p) return -1;
    /* "12 0 R" is an indirect reference, value unknown */
    const uint8_t *r = skip_ws(q, end);
    if (r < end && isdigit(*r)) {
        while (r < end && isdigit(*r)) r++;
        r = skip_ws(r, end);
        if (r < end && *r == 'R') return -1;
    }
    return v;
}

static const char *const SKIP_KEYS[] = {
    "/Image", "/FontFile", "/Length1", "/Length2", "/Length3", "/XRef", "/ObjStm",
    "/Metadata", "/EmbeddedFile", "/Alternate", "/FunctionType", "/ShadingType",
    "/PatternType", "Type1C", "CIDFontType0C", "/OpenType", "/XML", "/Predictor",
    "/DCT", "/JPX", "/JBIG2", "/CCITT", "/LZW", "/ASCII85", "/A85", "/ASCIIHex", "/AHx",
    "/RunLength", "/Crypt", "/Sig",
};

static void progress(pdf_t *pd, long pos)
{
    if (!pd->cb) return;
    int pct = pd->base + (int)((double)pos * pd->span / (pd->size ? pd->size : 1));
    if (pct != pd->last_pct) {
        pd->last_pct = pct;
        pd->cb(pd->arg, pct);
    }
}

/* processes the stream whose keyword starts at P; returns where scanning resumes */
static long process_stream(pdf_t *pd, long P, bool pass1)
{
    long ws = P > 1024 ? P - 1024 : 0;
    size_t dn = (size_t)(P - ws);
    fseek(pd->f, ws, SEEK_SET);
    dn = fread(pd->dict, 1, dn, pd->f);
    const uint8_t *d = pd->dict;
    size_t start = 0;
    for (size_t i = dn >= 3 ? dn - 3 : 0; i > 0; i--) {
        if (memcmp(d + i, "obj", 3) == 0 && is_ws(d[i - 1]) && !(i >= 3 && memcmp(d + i - 3, "end", 3) == 0)) {
            start = i + 3;
            break;
        }
    }
    const uint8_t *dict = d + start;
    size_t dlen = dn - start;

    long data = P + 6;
    uint8_t eol[2] = {0, 0};
    fseek(pd->f, data, SEEK_SET);
    size_t got = fread(eol, 1, 2, pd->f);
    if (got >= 1 && eol[0] == '\r') data += (got == 2 && eol[1] == '\n') ? 2 : 1;
    else if (got >= 1 && eol[0] == '\n') data += 1;

    long length = dict_length(dict, dlen);
    long resume = data + (length > 0 ? length : 0);
    if (!dict_has(dict, dlen, "<<")) return resume;
    for (size_t i = 0; i < sizeof(SKIP_KEYS) / sizeof(SKIP_KEYS[0]); i++)
        if (dict_has(dict, dlen, SKIP_KEYS[i])) return resume;
    bool has_filter = dict_has(dict, dlen, "/Filter");
    bool flate = dict_has(dict, dlen, "/FlateDecode") || dict_has(dict, dlen, "/Fl ") ||
                 dict_has(dict, dlen, "/Fl/") || dict_has(dict, dlen, "/Fl]") || dict_has(dict, dlen, "/Fl>");
    if (has_filter && !flate) return resume;

    sink_t *s = &pd->sink;
    s->len = 0;
    s->pass1 = pass1;
    s->decided = false;
    fseek(pd->f, data, SEEK_SET);
    if (flate) {
        frd_t *r = malloc(sizeof(frd_t));
        if (!r) return resume;
        r->f = pd->f;
        r->remaining = length > 0 ? length : pd->size - data;
        r->pos = r->len = 0;
        r->pushback = -1;
        int b0 = frd_byte(r), b1 = frd_byte(r);
        if (b0 < 0 || b1 < 0) { free(r); return resume; }
        if ((b0 & 0x0F) != 8 || ((b0 << 8) | b1) % 31 != 0) {
            /* no zlib header: raw deflate */
            fseek(pd->f, data, SEEK_SET);
            r->remaining = length > 0 ? length : pd->size - data;
            r->pos = r->len = 0;
        }
        inflate_stream(frd_byte, r, sink_out, s);
        free(r);
    } else {
        long want = length > 0 ? length : MAX_STREAM;
        if (want > MAX_STREAM) want = MAX_STREAM;
        uint8_t tmp[512];
        while (want > 0) {
            size_t n = fread(tmp, 1, want > (long)sizeof(tmp) ? sizeof(tmp) : (size_t)want, pd->f);
            if (n == 0) break;
            want -= (long)n;
            if (sink_out(s, tmp, n)) break;
        }
        if (length <= 0) {
            const uint8_t *e = mfind(s->buf, s->len, "endstream");
            if (e) s->len = (size_t)(e - s->buf);
            resume = data + (long)s->len;
        }
    }
    if (s->len == 0) return resume;

    if (pass1) {
        if (mfind(s->buf, s->len, "beginbf")) cmap_parse(pd->cm, s->buf, s->len);
        return resume;
    }
    if (mfind(s->buf, s->len, "begincmap")) return resume;
    if (!mfind(s->buf, s->len, "BT")) return resume;

    pctx_t *c = pd->pc;
    memset(c, 0, sizeof(*c));
    c->out = pd->out;
    c->cm = pd->cm;
    c->ctm = c->tm = c->tlm = IDENT;
    c->fsize = 12;
    c->th = 1;
    uint32_t mark = tout_mark(pd->out);
    parse_content(c, s->buf, s->len);
    tout_para(pd->out);
    if (pd->out->off > mark) {
        pd->pages++;
        char t[24];
        snprintf(t, sizeof(t), "Page %d", pd->pages);
        tout_toc(pd->out, mark, t);
    }
    return resume;
}

static void scan(pdf_t *pd, bool pass1)
{
    const long CH = 4096, OV = 16;
    long pos = 0;
    while (pos < pd->size) {
        progress(pd, pos);
        fseek(pd->f, pos, SEEK_SET);
        long n = (long)fread(pd->chunk, 1, CH, pd->f);
        if (n < 7) break;
        bool at_eof = pos + n >= pd->size;
        long next = at_eof ? pd->size : pos + n - OV;
        long resume = -1;
        for (long i = 0; i + 6 <= n; i++) {
            if (pd->chunk[i] != 's' || memcmp(pd->chunk + i, "stream", 6) != 0) continue;
            long A = pos + i;
            if (pos > 0 && i < 3) continue;          /* seen by the previous chunk */
            if (!at_eof && A >= next + 3) break;      /* the next chunk will see it */
            if (i + 6 >= n) break;
            uint8_t after = pd->chunk[i + 6];
            if (after != '\r' && after != '\n') continue;
            if (i >= 3 && memcmp(pd->chunk + i - 3, "end", 3) == 0) continue;
            resume = process_stream(pd, A, pass1);
            break;
        }
        if (resume > pos) pos = resume;
        else pos = next > pos ? next : pos + 1;
    }
}

int conv_pdf(const char *path, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t esz)
{
    pdf_t pd;
    memset(&pd, 0, sizeof(pd));
    pd.f = fopen(path, "rb");
    if (!pd.f) { snprintf(err, esz, "Cannot open file"); return -1; }
    fseek(pd.f, 0, SEEK_END);
    pd.size = ftell(pd.f);
    pd.out = out;
    pd.cb = cb;
    pd.arg = arg;
    pd.last_pct = -1;
    pd.cm = calloc(1, sizeof(cmap_t));
    pd.pc = calloc(1, sizeof(pctx_t));
    pd.chunk = malloc(4096);
    pd.dict = malloc(1100);
    int ret = -1;
    if (!pd.cm || !pd.pc || !pd.chunk || !pd.dict) { snprintf(err, esz, "Out of memory"); goto done; }

    /* encrypted files cannot be read without the key */
    {
        long tail = pd.size > 4096 ? 4096 : pd.size;
        fseek(pd.f, pd.size - tail, SEEK_SET);
        size_t n = fread(pd.chunk, 1, tail, pd.f);
        if (mfind(pd.chunk, n, "/Encrypt")) {
            snprintf(err, esz, "PDF is encrypted. Convert it in the browser uploader.");
            goto done;
        }
        fseek(pd.f, 0, SEEK_SET);
        n = fread(pd.chunk, 1, 8, pd.f);
        if (n < 5 || memcmp(pd.chunk, "%PDF", 4) != 0) {
            snprintf(err, esz, "Not a PDF file");
            goto done;
        }
    }
    pd.base = 0;
    pd.span = 20;
    scan(&pd, true);
    if (pd.cm->n2) qsort(pd.cm->m2, pd.cm->n2, sizeof(map2_t), map2_cmp);
    pd.base = 20;
    pd.span = 80;
    scan(&pd, false);
    if (out->off == 0) snprintf(err, esz, "No text found (scanned PDF?). Try the browser converter.");
    else ret = 0;
done:
    if (pd.cm) free(pd.cm->m2);
    free(pd.cm);
    free(pd.pc);
    free(pd.chunk);
    free(pd.dict);
    free(pd.sink.buf);
    fclose(pd.f);
    return ret;
}
