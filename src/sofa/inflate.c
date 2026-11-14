/* reAL - zlib-wrapped deflate (RFC 1950/1951) for the HDF5 reader's deflate filter, in place of zlib.
 *
 * gunzip() as libmysofa's (src/hdf/gunzip.c) calls zlib: the stream into `out` until it ends or `out` is full
 * (inflate with Z_SYNC_FLUSH stops there too), *outlen set to what was written.  Every read and write is bounded by
 * the buffers given: a damaged stream ends with an error, never outside them. */
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *in;
    uint32_t inlen, inpos;
    uint32_t bitbuf, bitcnt;
    uint8_t *out;
    uint32_t outlen, outpos;
    int err;
} inf;

enum { MAXBITS = 15, MAXLCODES = 286, MAXDCODES = 30, FIXLCODES = 288 };

static uint32_t bits(inf *s, int need)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < (uint32_t)need) {
        if (s->inpos >= s->inlen) { s->err = 1; return 0; }
        v |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return v & ((1u << need) - 1);
}

#define FASTBITS 9
/* a canonical code: counts per length and the symbols in code order, and for the codes of FASTBITS bits or fewer,
 * the symbol and length straight from the next FASTBITS bits (symbol | length << 9; 0 for a longer code) */
typedef struct { int16_t count[MAXBITS + 1]; int16_t symbol[FIXLCODES]; uint16_t fast[1 << FASTBITS]; } huff;

/* 0 when over-subscribed */
static int build(huff *h, const int16_t *len, int n)
{
    int16_t offs[MAXBITS + 1];
    for (int l = 0; l <= MAXBITS; l++) h->count[l] = 0;
    for (int s = 0; s < n; s++) h->count[len[s]]++;
    if (h->count[0] == n) return 1;             /* (no codes: an error only if one is used) */
    int left = 1;
    for (int l = 1; l <= MAXBITS; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return 0;
    }
    offs[1] = 0;
    for (int l = 1; l < MAXBITS; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int s = 0; s < n; s++) if (len[s]) h->symbol[offs[len[s]]++] = (int16_t)s;
    /* each code as deflate sends it (its bits reversed), for every value of the bits after it */
    int next[MAXBITS + 1], code = 0;
    memset(h->fast, 0, sizeof(h->fast));
    for (int l = 1; l <= MAXBITS; l++) { code = (code + (l > 1 ? h->count[l - 1] : 0)) << 1; next[l] = code; }
    for (int s = 0; s < n; s++) {
        int l = len[s];
        if (!l) continue;
        int c = next[l]++;
        if (l > FASTBITS) continue;
        int r = 0;
        for (int k = 0; k < l; k++) r |= ((c >> k) & 1) << (l - 1 - k);
        for (int f = r; f < 1 << FASTBITS; f += 1 << l) h->fast[f] = (uint16_t)(s | l << 9);
    }
    return 1;
}

static int decode(inf *s, const huff *h)
{
    while (s->bitcnt < FASTBITS && s->inpos < s->inlen) {
        s->bitbuf |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    uint32_t f = h->fast[s->bitbuf & ((1u << FASTBITS) - 1)];
    if (f && f >> 9 <= s->bitcnt) {
        s->bitbuf >>= f >> 9;
        s->bitcnt -= f >> 9;
        return (int)(f & 511);
    }
    int code = 0, first = 0, index = 0;     /* (a longer code, or the stream's last bits: one bit at a time) */
    for (int l = 1; l <= MAXBITS; l++) {
        code |= (int)bits(s, 1);
        if (s->err) return -1;
        int count = h->count[l];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;                                 /* (a code no length has) */
    return -1;
}

static const int16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                   115, 131, 163, 195, 227, 258 };
static const int16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
                                  0 };
static const int16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                   1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const int16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                  12, 13, 13 };

/* 1: the block's end; 2: the output is full; 0: an error */
static int codes(inf *s, const huff *lc, const huff *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (s->outpos >= s->outlen) return 2;
            s->out[s->outpos++] = (uint8_t)sym;
        } else if (sym == 256) return 1;
        else {
            sym -= 257;
            if (sym >= 29) return 0;
            uint32_t len = (uint32_t)lbase[sym] + bits(s, lext[sym]);
            int d = decode(s, dc);
            if (d < 0 || d >= 30 || s->err) return 0;
            uint32_t dist = (uint32_t)dbase[d] + bits(s, dext[d]);
            if (s->err || dist > s->outpos) return 0;
            for (; len; len--) {
                if (s->outpos >= s->outlen) return 2;
                s->out[s->outpos] = s->out[s->outpos - dist];
                s->outpos++;
            }
        }
    }
}

static int fixed(inf *s)
{
    huff lc, dc;
    int16_t len[FIXLCODES];
    int k = 0;
    for (; k < 144; k++) len[k] = 8;
    for (; k < 256; k++) len[k] = 9;
    for (; k < 280; k++) len[k] = 7;
    for (; k < FIXLCODES; k++) len[k] = 8;
    build(&lc, len, FIXLCODES);
    for (k = 0; k < MAXDCODES; k++) len[k] = 5;
    build(&dc, len, MAXDCODES);
    return codes(s, &lc, &dc);
}

static int dynamic(inf *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int16_t len[MAXLCODES + MAXDCODES];
    huff lc, dc;
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (s->err || nlen > MAXLCODES || ndist > MAXDCODES) return 0;
    int k = 0;
    for (; k < ncode; k++) len[order[k]] = (int16_t)bits(s, 3);
    for (; k < 19; k++) len[order[k]] = 0;
    if (s->err || !build(&lc, len, 19)) return 0;
    for (k = 0; k < nlen + ndist; ) {
        int sym = decode(s, &lc);
        if (sym < 0) return 0;
        if (sym < 16) { len[k++] = (int16_t)sym; continue; }
        int16_t v = 0;
        int rep;
        if (sym == 16) {
            if (!k) return 0;
            v = len[k - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) rep = 3 + (int)bits(s, 3);
        else rep = 11 + (int)bits(s, 7);
        if (s->err || k + rep > nlen + ndist) return 0;
        while (rep--) len[k++] = v;
    }
    if (!len[256]) return 0;                    /* (no end-of-block code) */
    if (!build(&lc, len, nlen) || !build(&dc, len + nlen, ndist)) return 0;
    return codes(s, &lc, &dc);
}

static int stored(inf *s)
{
    s->inpos -= s->bitcnt / 8;                  /* (whole bytes decode() read ahead go back; to a byte boundary) */
    s->bitbuf = 0; s->bitcnt = 0;
    if (s->inlen - s->inpos < 4) return 0;
    uint32_t n = s->in[s->inpos] | (uint32_t)s->in[s->inpos + 1] << 8;
    uint32_t c = s->in[s->inpos + 2] | (uint32_t)s->in[s->inpos + 3] << 8;
    s->inpos += 4;
    if (n != (~c & 0xffff) || s->inlen - s->inpos < n) return 0;
    uint32_t room = s->outlen - s->outpos, k = n < room ? n : room;
    memcpy(s->out + s->outpos, s->in + s->inpos, k);
    s->outpos += k; s->inpos += k;
    return k < n ? 2 : 1;
}

/* 0 (MYSOFA_OK), or -3 (zlib's Z_DATA_ERROR) */
int gunzip(int inlen, char *in, int *outlen, char *out)
{
    inf s;
    memset(&s, 0, sizeof(s));
    if (inlen < 2 || *outlen < 0) return -3;
    const uint8_t *p = (const uint8_t *)in;
    if ((p[0] & 0x0f) != 8 || (p[0] >> 4) > 7 || ((p[0] << 8) | p[1]) % 31 || (p[1] & 0x20)) return -3;   /* (zlib) */
    s.in = p; s.inlen = (uint32_t)inlen; s.inpos = 2;
    s.out = (uint8_t *)out; s.outlen = (uint32_t)*outlen;
    int last, r = 1;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (s.err) { r = 0; break; }
        r = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : 0;
    } while (r == 1 && !last);
    *outlen = (int)s.outpos;
    return r ? 0 : -3;
}
