/* src/sofa/inflate.c: the deflate filter of the HDF5 (SOFA) reader.
 *   cc -O1 -std=gnu11 -Wall -o build/test_inflate tests/test_inflate.c src/sofa/inflate.c && ./build/test_inflate
 * Streams made here bit by bit, so each reaches one place of the decoder: a code longer than the nine bits looked up
 * at once, the stream's last code with fewer bits left than that, a stored block after the bytes the decoder had read
 * ahead, an output that fills before the stream ends; and the streams a damaged file can hold, which must end with an
 * error and nothing written outside the output. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gunzip(int inlen, char *in, int *outlen, char *out);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                     printf("\n"); } } while (0)

/* ------------------------------------------------------------------ a deflate stream written bit by bit */
typedef struct { uint8_t b[1 << 17]; int n; uint32_t acc; int cnt; } bw;
static void bw_init(bw *w) { w->n = 0; w->acc = 0; w->cnt = 0; w->b[w->n++] = 0x78; w->b[w->n++] = 0x9c; }   /* zlib */
static void put(bw *w, uint32_t v, int bits)                    /* the low `bits` of v, low bit first */
{
    for (int k = 0; k < bits; k++) {
        w->acc |= ((v >> k) & 1u) << w->cnt;
        if (++w->cnt == 8) { w->b[w->n++] = (uint8_t)w->acc; w->acc = 0; w->cnt = 0; }
    }
}
static void align(bw *w) { if (w->cnt) { w->b[w->n++] = (uint8_t)w->acc; w->acc = 0; w->cnt = 0; } }
static void bytes(bw *w, const uint8_t *p, int n) { align(w); memcpy(w->b + w->n, p, (size_t)n); w->n += n; }

/* canonical Huffman codes from their lengths, a code sent high bit first */
typedef struct { int len[320]; int code[320]; int n; } hc;
static void hc_make(hc *h, const int *len, int n)
{
    int count[16] = { 0 }, next[16], code = 0;
    h->n = n;
    for (int s = 0; s < n; s++) { h->len[s] = len[s]; count[len[s]]++; }
    count[0] = 0;
    for (int l = 1; l < 16; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
    for (int s = 0; s < n; s++) h->code[s] = len[s] ? next[len[s]]++ : 0;
}
static void sym(bw *w, const hc *h, int s)
{
    for (int k = h->len[s] - 1; k >= 0; k--) put(w, (uint32_t)(h->code[s] >> k) & 1u, 1);
}
/* a dynamic block's header: the literal/length and distance code lengths as they are (no run lengths), under a
 * code-length code of thirteen 4-bit and six 5-bit codes */
static void dyn_header(bw *w, int final, const int *ll, int nll, const int *dl, int ndl)
{
    static const int order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int cl[19];
    hc c;
    for (int k = 0; k < 19; k++) cl[k] = k < 13 ? 4 : 5;
    hc_make(&c, cl, 19);
    put(w, (uint32_t)final, 1); put(w, 2, 2);
    put(w, (uint32_t)(nll - 257), 5); put(w, (uint32_t)(ndl - 1), 5); put(w, 19 - 4, 4);
    for (int k = 0; k < 19; k++) put(w, (uint32_t)cl[order[k]], 3);
    for (int k = 0; k < nll; k++) sym(w, &c, ll[k]);
    for (int k = 0; k < ndl; k++) sym(w, &c, dl[k]);
}
static void fixed_codes(hc *lc, hc *dc)
{
    int len[288], k = 0;
    for (; k < 144; k++) len[k] = 8;
    for (; k < 256; k++) len[k] = 9;
    for (; k < 280; k++) len[k] = 7;
    for (; k < 288; k++) len[k] = 8;
    hc_make(lc, len, 288);
    for (k = 0; k < 30; k++) len[k] = 5;
    hc_make(dc, len, 30);
}
static void stored_block(bw *w, int final, const uint8_t *p, int n)
{
    put(w, (uint32_t)final, 1); put(w, 0, 2);
    align(w);
    const unsigned u = (unsigned)n;
    uint8_t h[4] = { (uint8_t)u, (uint8_t)(u >> 8), (uint8_t)~u, (uint8_t)(~u >> 8) };
    bytes(w, h, 4);
    bytes(w, p, n);
}

/* ------------------------------------------------------------------ running it */
#define GUARD 64
static uint8_t s_out[(1 << 17) + GUARD];
/* the stack below the caller left full of what would pass for a table of 9-bit codes, so that a table the decoder
 * uses without having filled it does not happen to be zeros */
static void __attribute__((noinline)) dirty_stack(void)
{
    volatile uint16_t a[1 << 15];
    for (int k = 0; k < 1 << 15; k++) a[k] = (uint16_t)(9 | 9 << 9);
    (void)a[0];
}
static int s_dirty;
/* gunzip into `room` bytes; what it answers, *got what it wrote; nothing written past the room */
static int run(const uint8_t *in, int inlen, int room, int *got)
{
    memset(s_out, 0xa5, (size_t)room + GUARD);
    int outlen = room;
    /* (a copy of the exact size: a read past the stream shows under a sanitizer) */
    uint8_t *tight = malloc((size_t)inlen ? (size_t)inlen : 1);
    memcpy(tight, in, (size_t)inlen);
    if (s_dirty) dirty_stack();
    int r = gunzip(inlen, (char *)tight, &outlen, (char *)s_out);
    free(tight);
    *got = outlen;
    for (int k = 0; k < GUARD; k++)
        if (s_out[room + k] != 0xa5) { CHECK(0, "wrote past the output (%d bytes of room)", room); break; }
    CHECK(outlen >= 0 && outlen <= room, "said it wrote %d of %d", outlen, room);
    return r;
}
static void expect(const char *what, const bw *w, const uint8_t *want, int n)
{
    int got, r = run(w->b, w->n, n, &got);
    CHECK(r == 0 && got == n && !memcmp(s_out, want, (size_t)n), "%s: answered %d, %d of %d bytes", what, r, got, n);
}
static void expect_error(const char *what, const bw *w)
{
    int got, r = run(w->b, w->n, 4096, &got);
    CHECK(r == -3, "%s: answered %d (%d bytes)", what, r, got);
}

int main(void)
{
    static bw w;
    static uint8_t want[1 << 17];
    hc fl, fd;
    fixed_codes(&fl, &fd);

    /* a stream of zlib's own (zlib.compress(b'reAL reads SOFA files. ' * 6 + bytes(range(64)), 9)) */
    {
        static const uint8_t z[] = {
            0x78, 0xda, 0x2b, 0x4a, 0x75, 0xf4, 0x51, 0x28, 0x4a, 0x4d, 0x4c, 0x29, 0x56, 0x08, 0xf6, 0x77, 0x73, 0x54,
            0x48, 0xcb, 0xcc, 0x49, 0x2d, 0xd6, 0x03, 0x8a, 0xd0, 0x5d, 0x98, 0x81, 0x91, 0x89, 0x99, 0x85, 0x95, 0x8d,
            0x9d, 0x83, 0x93, 0x8b, 0x9b, 0x87, 0x97, 0x8f, 0x5f, 0x40, 0x50, 0x48, 0x58, 0x44, 0x54, 0x4c, 0x5c, 0x42,
            0x52, 0x4a, 0x5a, 0x46, 0x56, 0x4e, 0x5e, 0x41, 0x51, 0x49, 0x59, 0x45, 0x55, 0x4d, 0x5d, 0x43, 0x53, 0x4b,
            0x5b, 0x47, 0x57, 0x4f, 0xdf, 0xc0, 0xd0, 0xc8, 0xd8, 0xc4, 0xd4, 0xcc, 0xdc, 0xc2, 0xd2, 0xca, 0xda, 0xc6,
            0xd6, 0xce, 0x1e, 0x00, 0xc7, 0xca, 0x34, 0x0f };
        int n = 0;
        for (int k = 0; k < 6; k++) { memcpy(want + n, "reAL reads SOFA files. ", 23); n += 23; }
        for (int k = 0; k < 64; k++) want[n++] = (uint8_t)k;
        int got, r = run(z, (int)sizeof(z), n, &got);
        CHECK(r == 0 && got == n && !memcmp(s_out, want, (size_t)n), "zlib's stream: answered %d, %d of %d bytes", r, got, n);
    }

    /* codes of 1 to 15 bits: the long ones are read a bit at a time, and the block's end, 15 bits, is the stream's
     * last code, with fewer bits left than the nine looked up at once.  Then the same with 0 to 7 literals more, so
     * the end falls at every place in a byte. */
    {
        int ll[257] = { 0 }, dl[2] = { 1, 1 };
        /* 'a'..'n': 1..14 bits; 'o' and the block's end: 15 */
        for (int k = 0; k < 14; k++) ll['a' + k] = k + 1;
        ll['o'] = 15; ll[256] = 15;
        hc lc, dc;
        hc_make(&lc, ll, 257);
        hc_make(&dc, dl, 2);
        for (int extra = 0; extra < 8; extra++) {
            bw_init(&w);
            dyn_header(&w, 1, ll, 257, dl, 2);
            int n = 0;
            for (int k = 0; k < 15; k++) { sym(&w, &lc, 'a' + k); want[n++] = (uint8_t)('a' + k); }
            for (int k = 0; k < extra; k++) { sym(&w, &lc, 'a'); want[n++] = 'a'; }
            sym(&w, &lc, 'o'); want[n++] = 'o';
            sym(&w, &lc, 256);
            align(&w);
            expect("codes of 1 to 15 bits", &w, want, n);
            /* (and cut short anywhere: an error, never a wrong answer) */
            for (int cut = 2; cut < w.n; cut++) {
                int got, r = run(w.b, cut, n, &got);
                CHECK(r == -3 || (r == 0 && got == n && !memcmp(s_out, want, (size_t)n)), "cut at %d of %d: answered %d",
                      cut, w.n, r);
            }
        }
    }

    /* a stored block after a block of codes: the decoder has read up to two bytes ahead by then (the stored block's
     * length among them) and must go back to them.  The first block ends at every place in a byte. */
    for (int lits = 0; lits < 24; lits++) {
        bw_init(&w);
        int n = 0;
        put(&w, 0, 1); put(&w, 1, 2);                       /* a fixed block, not the last */
        for (int k = 0; k < lits; k++) {
            int c = k % 3 == 2 ? 200 + k : 'A' + k;         /* (9-bit and 8-bit codes) */
            sym(&w, &fl, c); want[n++] = (uint8_t)c;
        }
        sym(&w, &fl, 256);
        uint8_t st[40];
        for (int k = 0; k < 40; k++) st[k] = (uint8_t)(k * 7 + lits);
        stored_block(&w, 0, st, 40);
        memcpy(want + n, st, 40); n += 40;
        put(&w, 1, 1); put(&w, 1, 2);                       /* and codes again after it */
        sym(&w, &fl, 'z'); want[n++] = 'z';
        sym(&w, &fl, 256);
        align(&w);
        expect("a stored block after codes", &w, want, n);
    }

    /* matches: the longest (258) at the nearest distance, and the farthest distance (32768), over stored blocks */
    {
        bw_init(&w);
        int n = 0;
        static uint8_t far[32768];
        for (int k = 0; k < 32768; k++) far[k] = (uint8_t)(k * 31 >> 3);
        stored_block(&w, 0, far, 32768);
        memcpy(want, far, 32768); n = 32768;
        put(&w, 1, 1); put(&w, 1, 2);
        sym(&w, &fl, 257 + 0); sym(&w, &fd, 29); put(&w, 8191, 13);         /* length 3 at distance 24577 + 8191 */
        for (int k = 0; k < 3; k++, n++) want[n] = want[n - 32768];
        sym(&w, &fl, 'q'); want[n++] = 'q';
        sym(&w, &fl, 285); sym(&w, &fd, 0);                                 /* length 258 at distance 1 */
        for (int k = 0; k < 258; k++, n++) want[n] = want[n - 1];
        sym(&w, &fl, 284); put(&w, 30, 5); sym(&w, &fd, 4); put(&w, 1, 1);  /* length 227 + 30 at distance 5 + 1 */
        for (int k = 0; k < 257; k++, n++) want[n] = want[n - 6];
        sym(&w, &fl, 256);
        align(&w);
        expect("matches", &w, want, n);
        /* the output full before the stream ends, at every size down from the whole: what fits, and no error (the
         * reader asks for a chunk's size and takes what comes) */
        for (int room = n - 1; room > n - 600; room--) {
            int got, r = run(w.b, w.n, room, &got);
            CHECK(r == 0 && got == room && !memcmp(s_out, want, (size_t)room), "room for %d of %d: answered %d, %d bytes",
                  room, n, r, got);
        }
        int got, r = run(w.b, w.n, 100, &got);              /* (full inside the stored block) */
        CHECK(r == 0 && got == 100 && !memcmp(s_out, want, 100), "room for 100: answered %d, %d bytes", r, got);
        r = run(w.b, w.n, 0, &got);
        CHECK(r == 0 && got == 0, "no room: answered %d, %d bytes", r, got);
    }

    /* run lengths in a dynamic block's code lengths (16: the last one again; 17 and 18: zeros), and the same header
     * gone wrong: a run past the count, the last length again with none before it */
    for (int variant = 0; variant < 3; variant++) {
        static const int order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        int cl[19] = { 0 };
        cl[18] = 2; cl[3] = 2; cl[1] = 3; cl[2] = 3; cl[16] = 3; cl[17] = 3;
        hc c;
        hc_make(&c, cl, 19);
        bw_init(&w);
        put(&w, 1, 1); put(&w, 2, 2);
        put(&w, 260 - 257, 5); put(&w, 5 - 1, 5); put(&w, 19 - 4, 4);
        for (int k = 0; k < 19; k++) put(&w, (uint32_t)cl[order[k]], 3);
        /* 'w' 3 bits, 'x' 2, 'y' 3, the block's end and lengths 3 to 5 (257 to 259) 3 bits each; two distances */
        if (variant == 2) { sym(&w, &c, 16); put(&w, 0, 2); }
        sym(&w, &c, 18); put(&w, 119 - 11, 7);              /* 0..118 */
        sym(&w, &c, 3); sym(&w, &c, 2); sym(&w, &c, 3);     /* w x y */
        sym(&w, &c, 18); put(&w, 134 - 11, 7);              /* 122..255 */
        sym(&w, &c, 3);                                     /* 256 */
        sym(&w, &c, 16); put(&w, 3 - 3, 2);                 /* 257..259: the same */
        sym(&w, &c, 1); sym(&w, &c, 1);                     /* distances 1 and 2: one bit each */
        sym(&w, &c, 17); put(&w, (uint32_t)(variant == 1 ? 4 - 3 : 3 - 3), 3);     /* and three (or four) with none */
        int ll[260] = { 0 }, dl[5] = { 1, 1, 0, 0, 0 };
        ll['w'] = 3; ll['x'] = 2; ll['y'] = 3; ll[256] = ll[257] = ll[258] = ll[259] = 3;
        hc lc, dc;
        hc_make(&lc, ll, 260);
        hc_make(&dc, dl, 5);
        int n = 0;
        for (const char *p = "xwyxxy"; *p; p++) { sym(&w, &lc, *p); want[n++] = (uint8_t)*p; }
        sym(&w, &lc, 258); sym(&w, &dc, 1);                 /* length 4 at distance 2 */
        for (int k = 0; k < 4; k++, n++) want[n] = want[n - 2];
        sym(&w, &lc, 256);
        for (int k = 0; k < 4; k++) put(&w, 0, 8);
        if (variant == 0) expect("code lengths by runs", &w, want, n);
        else expect_error(variant == 1 ? "a run of code lengths past their count" :
                                         "the last length again, with none before it", &w);
    }

    /* ---------------------------------------------------------------- what a damaged file can hold */
    {
        static const uint8_t ok[] = { 0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01 };      /* an empty stream */
        int got, r = run(ok, 8, 16, &got);
        CHECK(r == 0 && got == 0, "an empty stream: answered %d, %d bytes", r, got);
        uint8_t z[8];
        memcpy(z, ok, 8); z[0] = 0x79;                      /* not deflate */
        CHECK(run(z, 8, 16, &got) == -3, "compression method 9 accepted");
        memcpy(z, ok, 8); z[0] = 0x88;                      /* a window larger than 32K */
        CHECK(run(z, 8, 16, &got) == -3, "a 64K window accepted");
        memcpy(z, ok, 8); z[1] = 0x9d;                      /* the header's check */
        CHECK(run(z, 8, 16, &got) == -3, "a header failing its check accepted");
        memcpy(z, ok, 8); z[1] = 0xbb;                      /* a preset dictionary (0x78bb is a multiple of 31) */
        CHECK(run(z, 8, 16, &got) == -3, "a preset dictionary accepted");
        CHECK(run(ok, 1, 16, &got) == -3 && run(ok, 0, 16, &got) == -3, "a stream of one byte or none accepted");
        CHECK(run(ok, 2, 16, &got) == -3, "a header with nothing after it accepted");
    }
    bw_init(&w); put(&w, 1, 1); put(&w, 3, 2); align(&w);
    expect_error("block type 3", &w);
    {   /* a stored block: its length and the length's complement, and more bytes than the stream has */
        bw_init(&w); put(&w, 1, 1); put(&w, 0, 2); align(&w);
        uint8_t h[4] = { 5, 0, 0xfb, 0xff };                /* (~5 is 0xfffa) */
        bytes(&w, h, 4); bytes(&w, (const uint8_t *)"hello", 5);
        expect_error("a stored block's length not matching its complement", &w);
        bw_init(&w); put(&w, 1, 1); put(&w, 0, 2); align(&w);
        uint8_t h2[4] = { 9, 0, 0xf6, 0xff };
        bytes(&w, h2, 4); bytes(&w, (const uint8_t *)"hello", 5);
        expect_error("a stored block longer than the stream", &w);
        bw_init(&w); put(&w, 1, 1); put(&w, 0, 2); align(&w);
        bytes(&w, h2, 3);
        expect_error("a stored block's header cut short", &w);
    }
    /* fixed codes the format has no use for: lengths 286 and 287, distances 30 and 31 */
    bw_init(&w); put(&w, 1, 1); put(&w, 1, 2); sym(&w, &fl, 'a'); sym(&w, &fl, 286); sym(&w, &fd, 0); align(&w);
    expect_error("length code 286", &w);
    bw_init(&w); put(&w, 1, 1); put(&w, 1, 2); sym(&w, &fl, 'a'); sym(&w, &fl, 257); put(&w, 0x0f, 5); align(&w);
    expect_error("distance code 30", &w);                   /* (30 is 11110, sent high bit first: 01111) */
    /* a distance before the output's start */
    bw_init(&w); put(&w, 1, 1); put(&w, 1, 2); sym(&w, &fl, 'a'); sym(&w, &fl, 257); sym(&w, &fd, 1); align(&w);
    expect_error("a distance of 2 after one byte", &w);
    bw_init(&w); put(&w, 1, 1); put(&w, 1, 2); sym(&w, &fl, 257); sym(&w, &fd, 0); align(&w);
    expect_error("a match with nothing before it", &w);
    /* dynamic blocks: too many codes, no code lengths at all, no code for the block's end, a code not in the table */
    bw_init(&w); put(&w, 1, 1); put(&w, 2, 2); put(&w, 30, 5); put(&w, 0, 5); put(&w, 0, 4); align(&w);
    for (int k = 0; k < 64; k++) w.b[w.n++] = 0;
    expect_error("287 literal/length codes", &w);
    bw_init(&w); put(&w, 1, 1); put(&w, 2, 2); put(&w, 0, 5); put(&w, 30, 5); put(&w, 0, 4); align(&w);
    for (int k = 0; k < 64; k++) w.b[w.n++] = 0;
    expect_error("31 distance codes", &w);
    {
        /* the code-length code with no lengths: there is no code to read what follows by.  (What follows is what a
         * table of 9-bit codes left over in memory would read as 258 lengths of 9, the literals "ok" and the end) */
        bw_init(&w); put(&w, 1, 1); put(&w, 2, 2); put(&w, 0, 5); put(&w, 0, 5); put(&w, 0, 4);
        for (int k = 0; k < 4; k++) put(&w, 0, 3);
        for (int k = 0; k < 258; k++) put(&w, 9, 9);
        for (int k = 8; k >= 0; k--) put(&w, ('o' >> k) & 1, 1);
        for (int k = 8; k >= 0; k--) put(&w, ('k' >> k) & 1, 1);
        for (int k = 8; k >= 0; k--) put(&w, (256 >> k) & 1, 1);
        align(&w);
        s_dirty = 1;
        expect_error("a code-length code with no codes", &w);
        s_dirty = 0;
    }
    {
        int ll[258] = { 0 }, dl[1] = { 0 };
        ll['a'] = 1; ll['b'] = 1;                           /* (no code for 256) */
        hc lc;
        hc_make(&lc, ll, 257);
        bw_init(&w);
        dyn_header(&w, 1, ll, 257, dl, 1);
        sym(&w, &lc, 'a'); sym(&w, &lc, 'b');
        for (int k = 0; k < 4; k++) put(&w, 0, 8);
        expect_error("no code for the block's end", &w);
        ll['b'] = 2; ll[256] = 3;                           /* 1/2 + 1/4 + 1/8: the code 111 is no symbol's */
        hc_make(&lc, ll, 257);
        bw_init(&w);
        dyn_header(&w, 1, ll, 257, dl, 1);
        sym(&w, &lc, 'a'); put(&w, 7, 3);
        for (int k = 0; k < 8; k++) put(&w, 0xff, 8);
        expect_error("a code that is no symbol's", &w);
        ll['c'] = 1;                                        /* three codes of one bit and more */
        bw_init(&w);
        dyn_header(&w, 1, ll, 257, dl, 1);
        for (int k = 0; k < 8; k++) put(&w, 0, 8);
        expect_error("an over-subscribed code", &w);
        memset(ll, 0, sizeof(ll));                          /* a match where no distance has a code */
        ll['a'] = 1; ll[256] = 2; ll[257] = 2;
        hc_make(&lc, ll, 258);
        bw_init(&w);
        dyn_header(&w, 1, ll, 258, dl, 1);
        sym(&w, &lc, 'a'); sym(&w, &lc, 257);
        for (int k = 0; k < 8; k++) put(&w, 0, 8);
        expect_error("a match with no distance code", &w);
    }

    /* damaged copies of a good stream, a hundred thousand: an error or an answer, nothing outside the output */
    {
        bw_init(&w);
        int ll[257] = { 0 }, dl[4] = { 2, 2, 2, 2 };
        for (int k = 0; k < 14; k++) ll['a' + k] = k + 1;
        ll['o'] = 15; ll[256] = 15;
        hc lc, dc;
        hc_make(&lc, ll, 257);
        hc_make(&dc, dl, 4);
        put(&w, 0, 1); put(&w, 1, 2);
        for (int k = 0; k < 40; k++) sym(&w, &fl, 'A' + k % 26);
        sym(&w, &fl, 260); sym(&w, &fd, 3);
        sym(&w, &fl, 256);
        stored_block(&w, 0, (const uint8_t *)"stored bytes here", 17);
        dyn_header(&w, 1, ll, 257, dl, 4);
        for (int k = 0; k < 30; k++) sym(&w, &lc, 'a' + k % 15);
        sym(&w, &lc, 256);
        align(&w);
        int got, r = run(w.b, w.n, 4096, &got);
        CHECK(r == 0 && got == 40 + 6 + 17 + 30, "the stream to damage: answered %d, %d bytes", r, got);
        static uint8_t d[4096];
        uint32_t seed = 12345;
        for (int i = 0; i < 100000; i++) {
            memcpy(d, w.b, (size_t)w.n);
            int n = w.n;
            for (int k = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 4; k--; ) {
                seed = seed * 1103515245u + 12345u;
                int p = (int)((seed >> 8) % (uint32_t)n);
                seed = seed * 1103515245u + 12345u;
                switch ((seed >> 12) % 4) {
                case 0: d[p] ^= (uint8_t)(1u << ((seed >> 20) & 7)); break;
                case 1: d[p] = (uint8_t)(seed >> 20); break;
                case 2: d[p] = (seed >> 20) & 1 ? 0xff : 0; break;
                case 3: if (p > 2) n = p; break;
                }
            }
            r = run(d, n, (int)((seed >> 4) % 200), &got);
            if (r != 0 && r != -3) { CHECK(0, "a damaged stream: answered %d", r); break; }
        }
    }

    if (fails) { printf("inflate tests: %d FAILED\n", fails); return 1; }
    printf("inflate tests: all passed\n");
    return 0;
}
