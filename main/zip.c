#include "zip.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

int zip_open(zip_t *z, const char *path)
{
    memset(z, 0, sizeof(*z));
    z->f = fopen(path, "rb");
    if (!z->f) return -1;
    fseek(z->f, 0, SEEK_END);
    long size = ftell(z->f);
    if (size < 22) goto fail;
    long tail = size < 66000 ? size : 66000;
    uint8_t *buf = malloc(tail);
    if (!buf) goto fail;
    fseek(z->f, size - tail, SEEK_SET);
    if (fread(buf, 1, tail, z->f) != (size_t)tail) { free(buf); goto fail; }
    long eocd = -1;
    for (long i = tail - 22; i >= 0; i--) {
        if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6) { eocd = i; break; }
    }
    if (eocd < 0) { free(buf); goto fail; }
    z->count = rd16(buf + eocd + 10);
    z->cd_size = rd32(buf + eocd + 12);
    uint32_t cd_off = rd32(buf + eocd + 16);
    free(buf);
    if (z->cd_size == 0 || cd_off + z->cd_size > (uint32_t)size) goto fail;
    z->cd = malloc(z->cd_size);
    if (!z->cd) goto fail;
    fseek(z->f, cd_off, SEEK_SET);
    if (fread(z->cd, 1, z->cd_size, z->f) != z->cd_size) goto fail;
    return 0;
fail:
    zip_close(z);
    return -1;
}

void zip_close(zip_t *z)
{
    if (z->f) fclose(z->f);
    free(z->cd);
    memset(z, 0, sizeof(*z));
}

static int find_pass(zip_t *z, const char *name, zip_entry_t *e, int icase)
{
    size_t nlen = strlen(name);
    uint32_t p = 0;
    while (p + 46 <= z->cd_size) {
        const uint8_t *h = z->cd + p;
        if (rd32(h) != 0x02014b50) break;
        uint16_t fl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
        if (p + 46 + fl > z->cd_size) break;
        const char *fn = (const char *)h + 46;
        if (fl == nlen && (icase ? strncasecmp(fn, name, nlen) == 0 : memcmp(fn, name, nlen) == 0)) {
            e->method = rd16(h + 10);
            e->csize = rd32(h + 20);
            e->usize = rd32(h + 24);
            e->lho = rd32(h + 42);
            return 0;
        }
        p += 46 + fl + xl + cl;
    }
    return -1;
}

int zip_find(zip_t *z, const char *name, zip_entry_t *e)
{
    if (find_pass(z, name, e, 0) == 0) return 0;
    return find_pass(z, name, e, 1);
}

typedef struct {
    FILE *f;
    uint32_t remaining;
    uint8_t buf[1024];
    size_t pos, len;
} rd_t;

static int rd_byte(void *ctx)
{
    rd_t *r = ctx;
    if (r->pos == r->len) {
        if (r->remaining == 0) return -1;
        size_t want = r->remaining < sizeof(r->buf) ? r->remaining : sizeof(r->buf);
        r->len = fread(r->buf, 1, want, r->f);
        r->pos = 0;
        if (r->len == 0) return -1;
        r->remaining -= r->len;
    }
    return r->buf[r->pos++];
}

int zip_extract(zip_t *z, const zip_entry_t *e, inflate_out_fn out, void *ctx)
{
    uint8_t lh[30];
    fseek(z->f, e->lho, SEEK_SET);
    if (fread(lh, 1, 30, z->f) != 30 || rd32(lh) != 0x04034b50) return -1;
    fseek(z->f, e->lho + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    rd_t *r = malloc(sizeof(rd_t));
    if (!r) return -1;
    r->f = z->f;
    r->remaining = e->csize;
    r->pos = r->len = 0;
    int ret = 0;
    if (e->method == 0) {
        int c;
        uint8_t chunk[256];
        size_t n = 0;
        while ((c = rd_byte(r)) >= 0) {
            chunk[n++] = (uint8_t)c;
            if (n == sizeof(chunk)) {
                if (out(ctx, chunk, n)) { ret = 1; break; }
                n = 0;
            }
        }
        if (n && ret == 0) out(ctx, chunk, n);
    } else if (e->method == 8) {
        ret = inflate_stream(rd_byte, r, out, ctx);
        if (ret == 1) ret = 0;
    } else {
        ret = -2; /* unsupported compression */
    }
    free(r);
    return ret;
}

typedef struct {
    char *buf;
    size_t len, cap;
} membuf_t;

static int mem_out(void *ctx, const uint8_t *d, size_t n)
{
    membuf_t *m = ctx;
    if (m->len + n + 1 > m->cap) return 1;
    memcpy(m->buf + m->len, d, n);
    m->len += n;
    return 0;
}

char *zip_extract_alloc(zip_t *z, const zip_entry_t *e, size_t max, size_t *len)
{
    if (e->usize > max) return NULL;
    membuf_t m = {malloc(e->usize + 1), 0, e->usize + 1};
    if (!m.buf) return NULL;
    if (zip_extract(z, e, mem_out, &m) < 0 && m.len == 0) {
        free(m.buf);
        return NULL;
    }
    m.buf[m.len] = 0;
    if (len) *len = m.len;
    return m.buf;
}
