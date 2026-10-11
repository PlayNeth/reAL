/* test_msynth <downloads.bin> <script> <reference.wav> [out.wav]
 * Replays a dmgt session (tools/dmgt: the download buffers Microsoft's dmusic.dll handed its synth, and the MIDI
 * script) through msynth and compares with what Microsoft's synth rendered. */
#include "../src/music/msynth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void *readall(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n + 16);
    if (fread(b, 1, *n, f) != (size_t)*n) exit(2);
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: test_msynth dl.bin script ref.wav [out.wav]\n"); return 2; }
    long n;
    uint8_t *dl = readall(argv[1], &n);
    long wn;
    uint8_t *w = readall(argv[3], &wn);
    uint32_t rate = *(uint32_t *)(w + 24);
    int16_t *ref = (int16_t *)(w + 44);
    uint32_t nch = *(uint16_t *)(w + 22);
    uint32_t frames = *(uint32_t *)(w + 40) / (2 * nch);
    int voices = getenv("MS_VOICES") ? atoi(getenv("MS_VOICES")) : 64;
    msynth *s = msynth_new(rate, voices, 1, 1);
    if (getenv("MS_NOMMX")) msynth_set_mmx(s, 0);
    msynth_set_gain(s, getenv("MS_GAIN") ? atoi(getenv("MS_GAIN")) : 0);
    int ndl = 0;
    for (long p = 0; p + 8 <= n;) {
        uint32_t tag = *(uint32_t *)(dl + p), sz = *(uint32_t *)(dl + p + 4);
        const uint8_t *q = dl + p + 8;
        if (tag == 0x56504d44) {        /* 'DMPV': PlayVoice(rt, id, group, channel, dlid, pitch, volume, start, loop) */
            int64_t rt; uint64_t st, ls, le;
            memcpy(&rt, q, 8); memcpy(&st, q + 32, 8); memcpy(&ls, q + 40, 8); memcpy(&le, q + 48, 8);
            const uint32_t *u = (const uint32_t *)(q + 8);
            msynth_play_voice(s, rt * rate / 10000000, u[0], u[1], u[2], u[3], (int32_t)u[4], (int32_t)u[5], st, ls, le);
        } else if (tag == 0x56534d44) { /* 'DMSV' */
            int64_t rt; memcpy(&rt, q, 8);
            msynth_stop_voice(s, rt * rate / 10000000, *(const uint32_t *)(q + 8));
        } else {
            void *h;
            int r = msynth_download(s, q, sz, &h);
            if (r < 0) fprintf(stderr, "download %d failed: %d\n", ndl, r);
            ndl++;
        }
        p += 8 + sz;
    }
    FILE *f = fopen(argv[2], "r");
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        unsigned ms, st, d1 = 0, d2 = 0;
        if (line[0] == '#' || sscanf(line, "%u %x %u %u", &ms, &st, &d1, &d2) < 2 || st == 0) continue;
        uint8_t m[3] = { (uint8_t)st, (uint8_t)d1, (uint8_t)d2 };
        int64_t rt = (int64_t)ms * 10000;
        msynth_midi(s, rt * rate / 10000000, m, ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0) ? 2 : 3, 1);
    }
    fclose(f);
    int16_t *out = calloc(frames + 256, 2 * nch);
    const char *bl = getenv("MS_BUSES");
    if (bl) {   /* DirectX 8 sink: one mono buffer per bus (dmgt DMGT_BUSES) */
        uint32_t ids[32], fn[32], nb = 0;
        int32_t pitch[32] = { 0 };
        for (char *p = (char *)bl; *p && nb < 32;) {
            ids[nb] = strtoul(p, &p, 0);
            fn[nb] = *p == ':' ? strtoul(p + 1, &p, 0) : ids[nb];
            nb++;
            if (*p == ',') p++; else break;
        }
        if (getenv("MS_ASSIGN") && *getenv("MS_ASSIGN"))
            for (char *p = getenv("MS_ASSIGN"); *p;) {
                uint32_t ch = strtoul(p, &p, 0), bi[32], n = 0;
                if (*p == ':') p++;
                while (n < 32) { bi[n++] = strtoul(p, &p, 0); if (*p == ',') p++; else break; }
                msynth_assign_buses(s, 1, ch, bi, n);
                if (*p == ';') p++; else break;
            }
        int16_t *b[32];
        for (uint32_t i = 0; i < nb; i++) b[i] = calloc(256, 2);
        msynth_seek(s, 0);
        for (uint32_t pos = 0; pos < frames; pos += 256) {
            for (uint32_t i = 0; i < nb; i++) memset(b[i], 0, 512);
            msynth_read(s, b, ids, fn, pitch, nb, 256);
            for (uint32_t k = 0; k < 256 && pos + k < frames; k++)
                for (uint32_t i = 0; i < nb; i++) out[(pos + k) * nch + i] = b[i][k];
        }
    } else
        for (uint32_t pos = 0; pos < frames; pos += 256) msynth_render(s, out + pos * 2, 256, pos);
    long bad = 0, first = -1, maxd = 0;
    double se = 0, sr = 0;
    for (uint32_t i = 0; i < frames * nch; i++) {
        long d = labs((long)out[i] - ref[i]);
        if (d) { bad++; if (first < 0) first = i; }
        if (d > maxd) maxd = d;
        se += (double)d * d; sr += (double)ref[i] * ref[i];
    }
    printf("%u frames, %d downloads: %ld samples differ (first at frame %ld), max diff %ld, ref rms %.1f, err rms %.3f\n",
           frames, ndl, bad, first >= 0 ? first / (long)nch : -1, maxd, sqrt(sr / (frames * (double)nch)), sqrt(se / (frames * (double)nch)));
    if (argc > 4) {
        FILE *o = fopen(argv[4], "wb");
        fwrite(w, 1, 44, o);
        fwrite(out, 2 * nch, frames, o);
        fclose(o);
    }
    msynth_free(s);
    return bad ? 1 : 0;
}
