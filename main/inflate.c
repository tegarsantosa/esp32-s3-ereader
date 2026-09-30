/*
 * Small streaming DEFLATE decoder, modelled on Mark Adler's "puff" reference
 * implementation, extended with byte-callback input and a sliding window so
 * large files can be decompressed with ~34 KB of RAM.
 */
#include "inflate.h"
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288
#define WSIZE 32768u
#define WMASK (WSIZE - 1)
#define FLUSH_EVERY 4096u

typedef struct {
    inflate_in_fn in;
    void *in_ctx;
    inflate_out_fn out;
    void *out_ctx;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *win;
    uint32_t wpos;     /* total bytes produced (window index = wpos & WMASK) */
    uint32_t flushed;  /* total bytes handed to out() */
    jmp_buf env;
} state_t;

typedef struct {
    short *count;
    short *symbol;
} huff_t;

static int next_byte(state_t *s)
{
    int c = s->in(s->in_ctx);
    if (c < 0) longjmp(s->env, 2); /* truncated input */
    return c;
}

static int bits(state_t *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        val |= (uint32_t)next_byte(s) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1));
}

static void flush(state_t *s)
{
    while (s->flushed < s->wpos) {
        uint32_t start = s->flushed & WMASK;
        uint32_t n = s->wpos - s->flushed;
        if (start + n > WSIZE) n = WSIZE - start;
        if (s->out(s->out_ctx, s->win + start, n)) longjmp(s->env, 3);
        s->flushed += n;
    }
}

static inline void put(state_t *s, uint8_t b)
{
    s->win[s->wpos & WMASK] = b;
    s->wpos++;
    if ((s->wpos & (FLUSH_EVERY - 1)) == 0) flush(s);
}

static int stored(state_t *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    unsigned len = next_byte(s);
    len |= (unsigned)next_byte(s) << 8;
    unsigned nlen = next_byte(s);
    nlen |= (unsigned)next_byte(s) << 8;
    if (len != (~nlen & 0xffff)) return -2;
    while (len--) put(s, (uint8_t)next_byte(s));
    return 0;
}

static int decode(state_t *s, const huff_t *h)
{
    int code = 0, first = 0, index = 0, len = 1;
    uint32_t bitbuf = s->bitbuf;
    int left = s->bitcnt;
    const short *next = h->count + 1;
    for (;;) {
        while (left--) {
            code |= bitbuf & 1;
            bitbuf >>= 1;
            int count = *next++;
            if (code - count < first) {
                s->bitbuf = bitbuf;
                s->bitcnt = (s->bitcnt - len) & 7;
                return h->symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
            len++;
        }
        left = (MAXBITS + 1) - len;
        if (left == 0) break;
        bitbuf = (uint32_t)next_byte(s);
        if (left > 8) left = 8;
    }
    return -10;
}

static int construct(huff_t *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static const short LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                               3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                                193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                6145, 8193, 12289, 16385, 24577};
static const short DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                               7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(state_t *s, const huff_t *lencode, const huff_t *distcode)
{
    int sym;
    do {
        sym = decode(s, lencode);
        if (sym < 0) return sym;
        if (sym < 256) {
            put(s, (uint8_t)sym);
        } else if (sym > 256) {
            sym -= 257;
            if (sym >= 29) return -10;
            int len = LBASE[sym] + bits(s, LEXT[sym]);
            int dsym = decode(s, distcode);
            if (dsym < 0) return dsym;
            if (dsym >= 30) return -10;
            uint32_t dist = (uint32_t)DBASE[dsym] + (uint32_t)bits(s, DEXT[dsym]);
            if (dist > s->wpos) return -11;
            while (len--) put(s, s->win[(s->wpos - dist) & WMASK]);
        }
    } while (sym != 256);
    return 0;
}

typedef struct {
    short lencnt[MAXBITS + 1], lensym[FIXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    short lengths[FIXLCODES + MAXDCODES];
} tables_t;

static int fixed(state_t *s, tables_t *t)
{
    huff_t lencode = {t->lencnt, t->lensym}, distcode = {t->distcnt, t->distsym};
    int sym;
    for (sym = 0; sym < 144; sym++) t->lengths[sym] = 8;
    for (; sym < 256; sym++) t->lengths[sym] = 9;
    for (; sym < 280; sym++) t->lengths[sym] = 7;
    for (; sym < FIXLCODES; sym++) t->lengths[sym] = 8;
    construct(&lencode, t->lengths, FIXLCODES);
    for (sym = 0; sym < MAXDCODES; sym++) t->lengths[sym] = 5;
    construct(&distcode, t->lengths, MAXDCODES);
    return codes(s, &lencode, &distcode);
}

static int dynamic(state_t *s, tables_t *t)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    huff_t lencode = {t->lencnt, t->lensym}, distcode = {t->distcnt, t->distsym};
    short *lengths = t->lengths;
    int nlen = bits(s, 5) + 257;
    int ndist = bits(s, 5) + 1;
    int ncode = bits(s, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0) return -4;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (sym < 0) return sym;
        if (sym < 16) {
            lengths[index++] = (short)sym;
        } else {
            short len = 0;
            if (sym == 16) {
                if (index == 0) return -5;
                len = lengths[index - 1];
                sym = 3 + bits(s, 2);
            } else if (sym == 17) {
                sym = 3 + bits(s, 3);
            } else {
                sym = 11 + bits(s, 7);
            }
            if (index + sym > nlen + ndist) return -6;
            while (sym--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) return -9;
    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -7;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -8;
    return codes(s, &lencode, &distcode);
}

int inflate_stream(inflate_in_fn in, void *in_ctx, inflate_out_fn out, void *out_ctx)
{
    state_t *s = calloc(1, sizeof(state_t));
    tables_t *t = malloc(sizeof(tables_t));
    uint8_t *win = malloc(WSIZE);
    if (!s || !t || !win) {
        free(s); free(t); free(win);
        return -20;
    }
    s->in = in;
    s->in_ctx = in_ctx;
    s->out = out;
    s->out_ctx = out_ctx;
    s->win = win;
    volatile int err = 0;
    int jmp = setjmp(s->env);
    if (jmp == 0) {
        int last;
        do {
            last = bits(s, 1);
            int type = bits(s, 2);
            err = type == 0 ? stored(s) : type == 1 ? fixed(s, t) : type == 2 ? dynamic(s, t) : -1;
            if (err != 0) break;
        } while (!last);
        if (err == 0) flush(s);
    } else if (jmp == 3) {
        err = 1;
    } else {
        /* input ended early: hand out what we have so partial files still work */
        while (s->flushed < s->wpos) {
            uint32_t start = s->flushed & WMASK;
            uint32_t n = s->wpos - s->flushed;
            if (start + n > WSIZE) n = WSIZE - start;
            if (s->out(s->out_ctx, s->win + start, n)) break;
            s->flushed += n;
        }
        err = -2;
    }
    free(win);
    free(t);
    free(s);
    return err;
}
