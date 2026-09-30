#include "convert.h"
#include "html.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* source decoding (UTF-8, UTF-16, Windows-1252)                        */

enum { ENC_UTF8, ENC_CP1252, ENC_UTF16LE, ENC_UTF16BE };

static const uint16_t CP1252[32] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

typedef struct {
    FILE *f;
    uint8_t buf[4096];
    size_t pos, len;
    long consumed, total;
    int enc;
} src_t;

static int src_byte(src_t *s)
{
    if (s->pos == s->len) {
        s->len = fread(s->buf, 1, sizeof(s->buf), s->f);
        s->pos = 0;
        if (s->len == 0) return -1;
    }
    s->consumed++;
    return s->buf[s->pos++];
}

static int32_t src_cp(src_t *s)
{
    int b = src_byte(s);
    if (b < 0) return -1;
    switch (s->enc) {
    case ENC_CP1252:
        return (b >= 0x80 && b < 0xA0) ? CP1252[b - 0x80] : b;
    case ENC_UTF16LE:
    case ENC_UTF16BE: {
        int b2 = src_byte(s);
        if (b2 < 0) return -1;
        uint32_t u = s->enc == ENC_UTF16LE ? (uint32_t)(b | (b2 << 8)) : (uint32_t)((b << 8) | b2);
        if (u >= 0xD800 && u < 0xDC00) {
            int c1 = src_byte(s), c2 = src_byte(s);
            if (c1 < 0 || c2 < 0) return -1;
            uint32_t lo = s->enc == ENC_UTF16LE ? (uint32_t)(c1 | (c2 << 8)) : (uint32_t)((c1 << 8) | c2);
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        }
        return (int32_t)u;
    }
    default: {
        if (b < 0x80) return b;
        int n = (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : (b & 0xF8) == 0xF0 ? 3 : 0;
        if (!n) return 0xFFFD;
        uint32_t v = b & (0x3F >> n);
        for (int i = 0; i < n; i++) {
            int c = src_byte(s);
            if (c < 0) return -1;
            v = (v << 6) | (c & 0x3F);
        }
        return (int32_t)v;
    }
    }
}

/* detects BOM / validates UTF-8; leaves the file positioned after any BOM */
static int detect_encoding(FILE *f, long *bom)
{
    uint8_t b[3] = {0};
    size_t n = fread(b, 1, 3, f);
    *bom = 0;
    int enc = ENC_UTF8;
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) { *bom = 3; }
    else if (n >= 2 && b[0] == 0xFF && b[1] == 0xFE) { *bom = 2; enc = ENC_UTF16LE; }
    else if (n >= 2 && b[0] == 0xFE && b[1] == 0xFF) { *bom = 2; enc = ENC_UTF16BE; }
    if (enc == ENC_UTF8 && *bom == 0) {
        fseek(f, 0, SEEK_SET);
        uint8_t buf[1024];
        int need = 0;
        size_t r;
        long checked = 0;
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0 && checked < 4L * 1024 * 1024) {
            for (size_t i = 0; i < r; i++) {
                uint8_t c = buf[i];
                if (need) {
                    if ((c & 0xC0) != 0x80) { enc = ENC_CP1252; goto out; }
                    need--;
                } else if (c >= 0x80) {
                    if ((c & 0xE0) == 0xC0 && c >= 0xC2) need = 1;
                    else if ((c & 0xF0) == 0xE0) need = 2;
                    else if ((c & 0xF8) == 0xF0 && c <= 0xF4) need = 3;
                    else { enc = ENC_CP1252; goto out; }
                }
            }
            checked += (long)r;
        }
    }
out:
    fseek(f, *bom, SEEK_SET);
    return enc;
}

/* ------------------------------------------------------------------ */
/* plain text                                                           */

static bool word_is(const char *s, size_t n, const char *w)
{
    size_t l = strlen(w);
    if (n < l || strncasecmp(s, w, l) != 0) return false;
    return n == l || !isalpha((unsigned char)s[l]);
}

/* "Chapter 12", "CHAPTER IV", "Bab 3", "Prologue", "Part One: ..." */
static bool looks_like_heading(const char *s, size_t n)
{
    if (n == 0 || n > 60) return false;
    static const char *const numbered[] = {"chapter", "bab", "part", "book", "bagian", "kapitel",
                                           "chapitre", "capitulo", "section", "volume", "act"};
    static const char *const single[] = {"prologue", "epilogue", "prolog", "epilog", "preface",
                                         "introduction", "foreword", "afterword", "pendahuluan",
                                         "kata pengantar", "penutup"};
    for (size_t i = 0; i < sizeof(single) / sizeof(single[0]); i++)
        if (word_is(s, n, single[i]) && n < 40) return true;
    for (size_t i = 0; i < sizeof(numbered) / sizeof(numbered[0]); i++) {
        if (!word_is(s, n, numbered[i])) continue;
        size_t k = strlen(numbered[i]);
        while (k < n && s[k] == ' ') k++;
        if (k >= n) return false;
        if (isdigit((unsigned char)s[k])) return true;
        size_t r = k;
        while (r < n && strchr("IVXLCDM", s[r])) r++;
        if (r > k && (r == n || !isalpha((unsigned char)s[r]))) return true;
        /* spelled-out number in capitals: CHAPTER ONE */
        bool upper = true;
        for (size_t j = 0; j < n; j++)
            if (islower((unsigned char)s[j])) { upper = false; break; }
        if (upper) return true;
        static const char *const nums[] = {"one", "two", "three", "four", "five", "six", "seven",
                                           "eight", "nine", "ten", "eleven", "twelve", "satu", "dua",
                                           "tiga", "empat", "lima", "first", "last"};
        for (size_t j = 0; j < sizeof(nums) / sizeof(nums[0]); j++)
            if (word_is(s + k, n - k, nums[j])) return true;
        return false;
    }
    return false;
}

typedef struct {
    textout_t *out;
    bool reflow, indent_mode;
    int page;
} txtctx_t;

static void txt_line(txtctx_t *c, char *line, size_t len, bool had_ff)
{
    if (had_ff) {
        c->page++;
        char t[24];
        snprintf(t, sizeof(t), "Page %d", c->page);
        tout_toc(c->out, tout_mark(c->out), t);
    }
    size_t a = 0, b = len;
    while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
    while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
    bool blank = a == b;
    bool indented = a > 0;
    if (blank) {
        if (c->reflow) tout_para(c->out);
        return;
    }
    if (looks_like_heading(line + a, b - a)) {
        uint32_t off = tout_mark(c->out);
        line[b] = 0;
        tout_toc(c->out, off, line + a);
        tout_text(c->out, line + a, b - a);
        tout_para(c->out);
        return;
    }
    if (c->reflow) {
        if (c->indent_mode && indented) tout_para(c->out);
        tout_text(c->out, line + a, b - a);
        tout_space(c->out);
    } else {
        tout_text(c->out, line + a, b - a);
        tout_para(c->out);
    }
}

int conv_txt(const char *path, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t esz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, esz, "Cannot open file"); return -1; }
    src_t *s = calloc(1, sizeof(src_t));
    char *line = malloc(2048);
    if (!s || !line) { fclose(f); free(s); free(line); snprintf(err, esz, "Out of memory"); return -1; }
    fseek(f, 0, SEEK_END);
    s->total = ftell(f);
    fseek(f, 0, SEEK_SET);
    long bom;
    s->f = f;
    s->enc = detect_encoding(f, &bom);

    /* pass 1: line statistics to detect hard-wrapped text */
    long lines = 0, blank = 0, mid = 0, indented = 0, chars = 0, cur = 0;
    bool lead_ws = false, any = false;
    int32_t cp;
    while ((cp = src_cp(s)) >= 0) {
        if (cp == '\r') continue;
        if (cp == '\n') {
            lines++;
            if (!any) blank++;
            else {
                chars += cur;
                if (cur >= 30 && cur <= 100) mid++;
                if (lead_ws) indented++;
            }
            cur = 0;
            any = false;
            lead_ws = false;
            continue;
        }
        if (cur == 0 && (cp == ' ' || cp == '\t')) lead_ws = true;
        if (cp > ' ') any = true;
        cur++;
    }
    long nonblank = lines - blank;
    txtctx_t c = {.out = out};
    bool hardwrap = nonblank >= 20 && mid * 10 >= nonblank * 6 && chars / (nonblank ? nonblank : 1) < 100;
    c.reflow = hardwrap && (blank * 100 >= lines * 3 || indented * 100 >= nonblank * 5);
    c.indent_mode = c.reflow && blank * 100 < lines * 3;

    /* pass 2: convert */
    fseek(f, bom, SEEK_SET);
    s->pos = s->len = 0;
    s->consumed = 0;
    size_t n = 0;
    bool ff = false;
    int last_pct = -1;
    while (1) {
        cp = src_cp(s);
        if (cp == '\r') continue;
        if (cp < 0 || cp == '\n') {
            txt_line(&c, line, n, ff);
            n = 0;
            ff = false;
            if (cp < 0) break;
            int pct = (int)(s->consumed * 100 / (s->total ? s->total : 1));
            if (cb && pct != last_pct) { cb(arg, pct); last_pct = pct; }
            continue;
        }
        if (cp == '\f') { ff = true; continue; }
        if (cp == '\t') cp = ' ';
        if (cp == 0xFEFF) continue;
        if (n > 2040) { /* very long line: emit what we have */
            line[n] = 0;
            if (ff) { txt_line(&c, line, 0, true); ff = false; }
            tout_text(out, line, n);
            n = 0;
        }
        n += utf8_encode((uint32_t)cp, line + n);
    }
    free(line);
    free(s);
    fclose(f);
    if (out->off == 0) { snprintf(err, esz, "The file is empty"); return -1; }
    return 0;
}


/* ------------------------------------------------------------------ */
/* Markdown: headings become chapters, markup is removed                */

static void md_inline(textout_t *out, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!>|~<", s[i + 1])) {
            tout_text(out, s + i + 1, 1);
            i += 2;
            continue;
        }
        if (c == '!' && i + 1 < n && s[i + 1] == '[') { /* image: dropped */
            const char *rb = memchr(s + i, ']', n - i);
            if (rb && (size_t)(rb - s) + 1 < n && rb[1] == '(') {
                const char *rp = memchr(rb, ')', n - (size_t)(rb - s));
                i = rp ? (size_t)(rp - s) + 1 : n;
                continue;
            }
        }
        if (c == '[') { /* [text](url) or [text][ref] -> text */
            const char *rb = memchr(s + i, ']', n - i);
            if (rb && (size_t)(rb - s) + 1 < n && (rb[1] == '(' || rb[1] == '[')) {
                md_inline(out, s + i + 1, (size_t)(rb - s) - i - 1);
                const char *close = memchr(rb + 1, rb[1] == '(' ? ')' : ']', n - (size_t)(rb - s) - 1);
                i = close ? (size_t)(close - s) + 1 : n;
                continue;
            }
        }
        if (c == '<') { /* <https://...> keeps the address, other tags are removed */
            const char *gt = memchr(s + i, '>', n - i);
            if (gt) {
                size_t len = (size_t)(gt - s) - i - 1;
                if (len > 4 && (!strncmp(s + i + 1, "http", 4) || !strncmp(s + i + 1, "mailto:", 7)))
                    tout_text(out, s + i + 1, len);
                i = (size_t)(gt - s) + 1;
                continue;
            }
        }
        if (c == '*' || c == '`' || (c == '~' && i + 1 < n && s[i + 1] == '~')) {
            i += (c == '~') ? 2 : 1;
            continue;
        }
        if (c == '_') {
            bool left = i == 0 || !isalnum((unsigned char)s[i - 1]);
            bool right = i + 1 >= n || !isalnum((unsigned char)s[i + 1]);
            if (left || right) { i++; continue; }
        }
        tout_text(out, &c, 1);
        i++;
    }
}

static bool md_is_rule(const char *t, size_t n)
{
    char ch = 0;
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        if (t[i] == ' ') continue;
        if (t[i] != '-' && t[i] != '*' && t[i] != '_') return false;
        if (ch && t[i] != ch) return false;
        ch = t[i];
        count++;
    }
    return count >= 3;
}

typedef struct {
    textout_t *out;
    int line_no;
    bool code, front;
} mdctx_t;

static void md_line(mdctx_t *c, char *line, size_t len)
{
    textout_t *out = c->out;
    c->line_no++;
    while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
    line[len] = 0;
    size_t a = 0;
    while (a < len && (line[a] == ' ' || line[a] == '\t')) a++;
    const char *t = line + a;
    size_t n = len - a;
    if (c->line_no == 1 && !strcmp(t, "---")) { c->front = true; return; } /* YAML front matter */
    if (c->front) {
        if (!strcmp(t, "---") || !strcmp(t, "...")) c->front = false;
        return;
    }
    if (n >= 3 && (!strncmp(t, "```", 3) || !strncmp(t, "~~~", 3))) {
        c->code = !c->code;
        tout_para(out);
        return;
    }
    if (c->code) {
        if (n) { tout_text(out, t, n); tout_para(out); }
        return;
    }
    if (n == 0) { tout_para(out); return; }
    if (md_is_rule(t, n)) {
        tout_blank(out);
        tout_text(out, "* * *", 5);
        tout_blank(out);
        return;
    }
    if (t[0] == '#') {
        size_t lvl = 0;
        while (lvl < n && t[lvl] == '#') lvl++;
        if (lvl <= 6 && (lvl == n || t[lvl] == ' ')) {
            const char *h = t + lvl;
            size_t hn = n - lvl;
            while (hn && *h == ' ') { h++; hn--; }
            while (hn && (h[hn - 1] == '#' || h[hn - 1] == ' ')) hn--;
            uint32_t off = tout_mark(out);
            if (lvl <= 3) {
                char title[96];
                size_t j = 0;
                for (size_t k = 0; k < hn && j < sizeof(title) - 1; k++)
                    if (!strchr("*_`[]", h[k])) title[j++] = h[k];
                title[j] = 0;
                tout_toc(out, off, title);
            }
            md_inline(out, h, hn);
            tout_para(out);
            return;
        }
    }
    while (n && t[0] == '>') { /* blockquote */
        t++; n--;
        if (n && t[0] == ' ') { t++; n--; }
    }
    if (n >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
        tout_para(out);
        tout_cp(out, 0x2022);
        tout_space(out);
        t += 2; n -= 2;
    } else if (n >= 3 && isdigit((unsigned char)t[0])) {
        size_t k = 0;
        while (k < n && isdigit((unsigned char)t[k])) k++;
        if (k + 1 < n && (t[k] == '.' || t[k] == ')') && t[k + 1] == ' ') tout_para(out);
    }
    if (n && t[0] == '|') { /* table row: cells separated by spaces */
        bool sep = true;
        for (size_t k = 0; k < n; k++)
            if (!strchr("|-: ", t[k])) { sep = false; break; }
        if (sep) return;
        tout_para(out);
        for (size_t k = 0; k < n; k++) {
            if (t[k] == '|') tout_space(out);
            else md_inline(out, t + k, 1);
        }
        tout_para(out);
        return;
    }
    bool hard_break = n && t[n - 1] == '\\';  /* "text\" = line break */
    md_inline(out, t, hard_break ? n - 1 : n);
    if (hard_break) tout_para(out);
    else tout_space(out); /* soft line break joins the paragraph */
}

int conv_md(const char *path, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t esz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, esz, "Cannot open file"); return -1; }
    src_t *s = calloc(1, sizeof(src_t));
    char *line = malloc(2048);
    if (!s || !line) { fclose(f); free(s); free(line); snprintf(err, esz, "Out of memory"); return -1; }
    fseek(f, 0, SEEK_END);
    s->total = ftell(f);
    long bom;
    s->f = f;
    s->enc = detect_encoding(f, &bom);
    mdctx_t c = {.out = out};
    size_t n = 0;
    int last_pct = -1;
    for (;;) {
        int32_t cp = src_cp(s);
        if (cp == '\r' || cp == 0xFEFF) continue;
        if (cp < 0 || cp == '\n') {
            md_line(&c, line, n);
            n = 0;
            if (cp < 0) break;
            int pct = (int)(s->consumed * 100 / (s->total ? s->total : 1));
            if (cb && pct != last_pct) { cb(arg, pct); last_pct = pct; }
            continue;
        }
        if (cp == '\t') cp = ' ';
        if (n > 2040) { /* very long line: emit it as it comes */
            md_inline(out, line, n);
            n = 0;
        }
        n += utf8_encode((uint32_t)cp, line + n);
    }
    free(line);
    free(s);
    fclose(f);
    if (out->off == 0) { snprintf(err, esz, "The file is empty"); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */

int conv_html(const char *path, textout_t *out, conv_progress_fn cb, void *arg, char *err, size_t esz)
{
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, esz, "Cannot open file"); return -1; }
    html_t *h = malloc(sizeof(html_t));
    uint8_t *buf = malloc(2048);
    if (!h || !buf) { fclose(f); free(h); free(buf); snprintf(err, esz, "Out of memory"); return -1; }
    fseek(f, 0, SEEK_END);
    long total = ftell(f), done = 0;
    fseek(f, 0, SEEK_SET);
    html_init(h, out);
    size_t r;
    while ((r = fread(buf, 1, 2048, f)) > 0) {
        html_feed(h, buf, r);
        done += (long)r;
        if (cb) cb(arg, (int)(done * 100 / (total ? total : 1)));
    }
    html_finish(h);
    fclose(f);
    free(h);
    free(buf);
    if (out->off == 0) { snprintf(err, esz, "No text found"); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ */

static const char *ext_of(const char *name)
{
    const char *d = strrchr(name, '.');
    return d ? d + 1 : "";
}

bool convert_supported(const char *name)
{
    const char *e = ext_of(name);
    return !strcasecmp(e, "epub") || !strcasecmp(e, "pdf") || !strcasecmp(e, "txt") ||
           !strcasecmp(e, "html") || !strcasecmp(e, "htm") || !strcasecmp(e, "xhtml") ||
           !strcasecmp(e, "md") || !strcasecmp(e, "markdown") || !strcasecmp(e, "text");
}

int convert_book(const char *src, const char *txt_path, const char *toc_path,
                 conv_progress_fn cb, void *arg, char *err, size_t esz)
{
    char tmp_txt[160], tmp_toc[160];
    snprintf(tmp_txt, sizeof(tmp_txt), "%s.tmp", txt_path);
    snprintf(tmp_toc, sizeof(tmp_toc), "%s.tmp", toc_path);
    FILE *f = fopen(tmp_txt, "wb");
    FILE *t = fopen(tmp_toc, "wb");
    textout_t *out = malloc(sizeof(textout_t));
    if (!f || !t || !out) {
        if (f) fclose(f);
        if (t) fclose(t);
        free(out);
        snprintf(err, esz, "Storage is full or not writable");
        return -1;
    }
    /* the first TOC line records the source size so stale caches are detected */
    struct stat st;
    fprintf(t, "#%lu\n", stat(src, &st) == 0 ? (unsigned long)st.st_size : 0UL);
    tout_init(out, f, t);
    const char *e = ext_of(src);
    int ret;
    if (!strcasecmp(e, "epub")) ret = conv_epub(src, out, cb, arg, err, esz);
    else if (!strcasecmp(e, "pdf")) ret = conv_pdf(src, out, cb, arg, err, esz);
    else if (!strcasecmp(e, "md") || !strcasecmp(e, "markdown")) ret = conv_md(src, out, cb, arg, err, esz);
    else if (!strcasecmp(e, "html") || !strcasecmp(e, "htm") || !strcasecmp(e, "xhtml"))
        ret = conv_html(src, out, cb, arg, err, esz);
    else ret = conv_txt(src, out, cb, arg, err, esz);
    tout_finish(out);
    bool write_ok = !ferror(f) && !ferror(t);
    fclose(f);
    fclose(t);
    free(out);
    if (ret == 0 && !write_ok) {
        snprintf(err, esz, "Storage full while converting");
        ret = -1;
    }
    if (ret == 0) {
        remove(txt_path);
        remove(toc_path);
        if (rename(tmp_txt, txt_path) || rename(tmp_toc, toc_path)) {
            snprintf(err, esz, "Cannot save converted book");
            ret = -1;
        }
    }
    if (ret != 0) {
        remove(tmp_txt);
        remove(tmp_toc);
    }
    return ret;
}
