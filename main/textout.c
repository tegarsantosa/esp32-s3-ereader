#include "textout.h"
#include <string.h>

int utf8_encode(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void tout_init(textout_t *t, FILE *f, FILE *toc)
{
    memset(t, 0, sizeof(*t));
    t->f = f;
    t->toc = toc;
    t->last_blank = true;
    t->last_toc_off = 0xFFFFFFFF;
}

static void flush(textout_t *t)
{
    if (t->n) fwrite(t->buf, 1, t->n, t->f);
    t->n = 0;
}

static inline void emit(textout_t *t, char c)
{
    if (t->n == sizeof(t->buf)) flush(t);
    t->buf[t->n++] = c;
    t->off++;
}

void tout_space(textout_t *t)
{
    if (t->para_text) t->pending_space = true;
}

void tout_text(textout_t *t, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            tout_space(t);
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;
        if (t->pending_space) {
            emit(t, ' ');
            t->pending_space = false;
        }
        emit(t, (char)c);
        t->para_text = true;
    }
}

void tout_cp(textout_t *t, uint32_t cp)
{
    char b[4];
    int n = utf8_encode(cp, b);
    tout_text(t, b, n);
}

void tout_para(textout_t *t)
{
    if (t->para_text) {
        emit(t, '\n');
        t->last_blank = false;
    }
    t->para_text = false;
    t->pending_space = false;
}

void tout_blank(textout_t *t)
{
    tout_para(t);
    if (!t->last_blank) {
        emit(t, '\n');
        t->last_blank = true;
    }
}

bool tout_unhyphen(textout_t *t)
{
    if (t->pending_space || t->n < 2) return false;
    char p = t->buf[t->n - 2];
    if (t->buf[t->n - 1] == '-' && ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z'))) {
        t->n--;
        t->off--;
        return true;
    }
    return false;
}

uint32_t tout_mark(textout_t *t)
{
    tout_para(t);
    return t->off;
}

void tout_toc(textout_t *t, uint32_t off, const char *title)
{
    if (!t->toc || !title) return;
    char clean[96];
    size_t j = 0;
    bool sp = false;
    for (const char *p = title; *p && j < sizeof(clean) - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= ' ') { sp = j > 0; continue; }
        if (sp && j < sizeof(clean) - 2) clean[j++] = ' ';
        sp = false;
        clean[j++] = (char)c;
    }
    /* do not cut a UTF-8 sequence in half */
    if (j > 0 && ((unsigned char)clean[j - 1] & 0x80)) {
        size_t k = j - 1;
        while (k > 0 && ((unsigned char)clean[k] & 0xC0) == 0x80) k--;
        unsigned char lead = (unsigned char)clean[k];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (j - k < need) j = k;
    }
    clean[j] = 0;
    if (!j) return;
    if (off == t->last_toc_off) return; /* keep the first title for one position */
    t->last_toc_off = off;
    fprintf(t->toc, "%lu\t%s\n", (unsigned long)off, clean);
    t->toc_count++;
}

void tout_finish(textout_t *t)
{
    tout_para(t);
    flush(t);
}
