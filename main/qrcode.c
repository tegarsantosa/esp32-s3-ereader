/*
 * Minimal QR code encoder (ISO/IEC 18004), written after the structure of
 * Project Nayuki's reference generator: byte mode, ECC level M, versions 1-10.
 */
#include "qrcode.h"
#include <string.h>

#define MAXV 10

/* error correction codewords per block and block count, level M, versions 1..10 */
static const uint8_t ECC_PER_BLOCK[MAXV + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
static const uint8_t NUM_BLOCKS[MAXV + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};

static uint8_t s_mod[QR_MAX_SIZE][QR_MAX_SIZE];
static uint8_t s_fun[QR_MAX_SIZE][QR_MAX_SIZE];
static int s_size;

bool qr_module(int x, int y)
{
    return x >= 0 && y >= 0 && x < s_size && y < s_size && s_mod[y][x];
}

static int raw_modules(int ver)
{
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int na = ver / 7 + 2;
        r -= (25 * na - 10) * na - 55;
        if (ver >= 7) r -= 36;
    }
    return r;
}

static int data_codewords(int ver)
{
    return raw_modules(ver) / 8 - ECC_PER_BLOCK[ver] * NUM_BLOCKS[ver];
}

static uint8_t gf_mul(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; i--) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

static void rs_divisor(int degree, uint8_t *res)
{
    memset(res, 0, degree);
    res[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; i++) {
        for (int j = 0; j < degree; j++) {
            res[j] = gf_mul(res[j], root);
            if (j + 1 < degree) res[j] ^= res[j + 1];
        }
        root = gf_mul(root, 0x02);
    }
}

static void rs_remainder(const uint8_t *data, int len, const uint8_t *div, int degree, uint8_t *out)
{
    memset(out, 0, degree);
    for (int i = 0; i < len; i++) {
        uint8_t factor = data[i] ^ out[0];
        memmove(out, out + 1, degree - 1);
        out[degree - 1] = 0;
        for (int j = 0; j < degree; j++) out[j] ^= gf_mul(div[j], factor);
    }
}

static void set_fun(int x, int y, bool dark)
{
    s_mod[y][x] = dark;
    s_fun[y][x] = 1;
}

static void finder(int cx, int cy)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= s_size || y >= s_size) continue;
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            int d = ax > ay ? ax : ay;
            set_fun(x, y, d != 2 && d != 4);
        }
}

static void alignment(int cx, int cy)
{
    for (int dy = -2; dy <= 2; dy++)
        for (int dx = -2; dx <= 2; dx++) {
            int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            set_fun(cx + dx, cy + dy, (ax > ay ? ax : ay) != 1);
        }
}

static void format_bits(int mask)
{
    int data = (0 << 3) | mask; /* level M = 00 */
    int rem = data;
    for (int i = 0; i < 10; i++) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
#define BIT(i) (((bits >> (i)) & 1) != 0)
    for (int i = 0; i <= 5; i++) set_fun(8, i, BIT(i));
    set_fun(8, 7, BIT(6));
    set_fun(8, 8, BIT(7));
    set_fun(7, 8, BIT(8));
    for (int i = 9; i < 15; i++) set_fun(14 - i, 8, BIT(i));
    for (int i = 0; i < 8; i++) set_fun(s_size - 1 - i, 8, BIT(i));
    for (int i = 8; i < 15; i++) set_fun(8, s_size - 15 + i, BIT(i));
    set_fun(8, s_size - 8, true);
#undef BIT
}

static void function_patterns(int ver)
{
    for (int i = 0; i < s_size; i++) {
        set_fun(6, i, i % 2 == 0);
        set_fun(i, 6, i % 2 == 0);
    }
    finder(3, 3);
    finder(s_size - 4, 3);
    finder(3, s_size - 4);
    if (ver >= 2) {
        int na = ver / 7 + 2;
        int step = (ver * 8 + na * 3 + 5) / (na * 4 - 4) * 2;
        int pos[7];
        pos[0] = 6;
        for (int i = na - 1, p = s_size - 7; i >= 1; i--, p -= step) pos[i] = p;
        for (int i = 0; i < na; i++)
            for (int j = 0; j < na; j++) {
                if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0)) continue;
                alignment(pos[i], pos[j]);
            }
    }
    format_bits(0); /* reserve; real bits drawn after masking */
    if (ver >= 7) {
        int rem = ver;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        long bits = ((long)ver << 12) | rem;
        for (int i = 0; i < 18; i++) {
            bool bit = (bits >> i) & 1;
            int a = s_size - 11 + i % 3, b = i / 3;
            set_fun(a, b, bit);
            set_fun(b, a, bit);
        }
    }
}

static bool mask_bit(int m, int x, int y)
{
    switch (m) {
    case 0: return (x + y) % 2 == 0;
    case 1: return y % 2 == 0;
    case 2: return x % 3 == 0;
    case 3: return (x + y) % 3 == 0;
    case 4: return (x / 3 + y / 2) % 2 == 0;
    case 5: return x * y % 2 + x * y % 3 == 0;
    case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

static void apply_mask(int m)
{
    for (int y = 0; y < s_size; y++)
        for (int x = 0; x < s_size; x++)
            if (!s_fun[y][x] && mask_bit(m, x, y)) s_mod[y][x] ^= 1;
}

/* simplified penalty: long runs, 2x2 blocks and dark/light balance */
static long penalty(void)
{
    long p = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int a = 0; a < s_size; a++) {
            int run = 1;
            for (int b = 1; b < s_size; b++) {
                uint8_t cur = pass ? s_mod[b][a] : s_mod[a][b];
                uint8_t prev = pass ? s_mod[b - 1][a] : s_mod[a][b - 1];
                if (cur == prev) {
                    run++;
                    if (run == 5) p += 3;
                    else if (run > 5) p++;
                } else {
                    run = 1;
                }
            }
        }
    int dark = 0;
    for (int y = 0; y < s_size; y++)
        for (int x = 0; x < s_size; x++) {
            dark += s_mod[y][x];
            if (x + 1 < s_size && y + 1 < s_size) {
                uint8_t c = s_mod[y][x];
                if (c == s_mod[y][x + 1] && c == s_mod[y + 1][x] && c == s_mod[y + 1][x + 1]) p += 3;
            }
        }
    int total = s_size * s_size;
    int k = ((dark * 20 - total * 10) < 0 ? -(dark * 20 - total * 10) : (dark * 20 - total * 10)) / total;
    p += k * 10;
    return p;
}

int qr_encode(const char *text)
{
    int len = (int)strlen(text);
    int ver;
    for (ver = 1; ver <= MAXV; ver++) {
        int cap_bits = data_codewords(ver) * 8;
        int need = 4 + (ver < 10 ? 8 : 16) + len * 8;
        if (need <= cap_bits) break;
    }
    if (ver > MAXV) return 0;
    s_size = ver * 4 + 17;
    int ndata = data_codewords(ver);

    /* data bit stream */
    uint8_t data[400];
    memset(data, 0, sizeof(data));
    int bit = 0;
#define PUT(val, n)                                                             \
    for (int _i = (n) - 1; _i >= 0; _i--, bit++)                                \
        if (((val) >> _i) & 1) data[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7));
    PUT(0x4, 4);
    PUT(len, ver < 10 ? 8 : 16);
    for (int i = 0; i < len; i++) PUT((uint8_t)text[i], 8);
    int cap = ndata * 8;
    int term = cap - bit < 4 ? cap - bit : 4;
    bit += term;
    bit = (bit + 7) & ~7;
    for (uint8_t pad = 0xEC; bit < cap; pad ^= 0xEC ^ 0x11) PUT(pad, 8);
#undef PUT

    /* error correction + interleaving */
    int nblocks = NUM_BLOCKS[ver], ecc = ECC_PER_BLOCK[ver];
    int raw = raw_modules(ver) / 8;
    int nshort = nblocks - raw % nblocks;
    int short_len = raw / nblocks;
    uint8_t div[30], blocks[8][160];
    rs_divisor(ecc, div);
    for (int i = 0, k = 0; i < nblocks; i++) {
        int dl = short_len - ecc + (i < nshort ? 0 : 1);
        memcpy(blocks[i], data + k, dl);
        k += dl;
        uint8_t *e = blocks[i] + short_len - ecc + 1;
        rs_remainder(blocks[i], dl, div, ecc, e);
        if (i < nshort) blocks[i][dl] = 0; /* placeholder, skipped below */
    }
    uint8_t out[400];
    int n = 0;
    for (int i = 0; i <= short_len; i++)
        for (int j = 0; j < nblocks; j++)
            if (i != short_len - ecc || j >= nshort) out[n++] = blocks[j][i];

    /* modules */
    memset(s_mod, 0, sizeof(s_mod));
    memset(s_fun, 0, sizeof(s_fun));
    function_patterns(ver);
    int bi = 0;
    for (int right = s_size - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        for (int vert = 0; vert < s_size; vert++)
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                bool up = ((right + 1) & 2) == 0;
                int y = up ? s_size - 1 - vert : vert;
                if (!s_fun[y][x] && bi < n * 8) {
                    s_mod[y][x] = (out[bi >> 3] >> (7 - (bi & 7))) & 1;
                    bi++;
                }
            }
    }

    /* pick the mask with the lowest penalty */
    int best = 0;
    long best_p = -1;
    for (int m = 0; m < 8; m++) {
        apply_mask(m);
        format_bits(m);
        long p = penalty();
        if (best_p < 0 || p < best_p) { best_p = p; best = m; }
        apply_mask(m); /* undo (XOR) */
    }
    apply_mask(best);
    format_bits(best);
    return s_size;
}
