/* test_sf2 <bank.sf2|.dls> <script.txt> <out.wav> [rate]: renders a MIDI script ("ms status d1 d2" per line) with
 * reDM's synth, every instrument of the bank downloaded (SF2 banks go through the DLS conversion) */
#include "../src/music/dmdls.h"
#include "../src/music/msynth.h"
#include "../src/music/sf2dls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static msynth *g_syn;
static int s_dl(void *c, const void *b, uint32_t n, void **h) { int r = msynth_download(g_syn, b, n, h); if (r < 0) fprintf(stderr, "synth refused download (%d)\n", r); return r; }
static void s_ul(void *c, void *h) { msynth_unload(g_syn, h); }
static void put16(FILE *f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }
int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    uint32_t rate = argc > 4 ? atoi(argv[4]) : 44100;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) return 1;
    fclose(f);
    dm_collection *c;
    if (!memcmp(b + 8, "sfbk", 4)) {
        size_t dn; int ni;
        void *d = sf2_to_dls(b, n, &dn, &ni);
        if (!d) { fprintf(stderr, "not a soundfont\n"); return 1; }
        fprintf(stderr, "%d presets -> %zu bytes of DLS\n", ni, dn);
        if (getenv("DLS_OUT")) { FILE *o = fopen(getenv("DLS_OUT"), "wb"); fwrite(d, 1, dn, o); fclose(o); }
        c = dls_load(d, dn, 1);
        free(d);
    } else c = dls_load(b, n, 1);
    if (!c) { fprintf(stderr, "cannot load\n"); return 1; }
    g_syn = msynth_new(rate, 64, 1, 1);
    msynth_set_gain(g_syn, 0);
    dls_sink sk = { s_dl, s_ul };
    dls_port *port = dls_port_new(&sk, NULL);
    int ok = 0;
    for (int i = 0; i < dls_count(c); i++) {
        uint32_t patch; int hr;
        dls_enum(c, i, &patch, NULL);
        if (dls_port_download(port, c, patch, 0, 0, NULL, 0, &hr)) ok++;
        else fprintf(stderr, "instrument %08x failed %08x\n", patch, hr);
    }
    fprintf(stderr, "%d / %d instruments downloaded\n", ok, dls_count(c));
    FILE *s = fopen(argv[2], "r");
    int ms, st, d1, d2, end = 0;
    while (fscanf(s, "%d %x %x %x", &ms, &st, &d1, &d2) == 4) {
        uint8_t m[3] = { (uint8_t)st, (uint8_t)d1, (uint8_t)d2 };
        msynth_midi(g_syn, (int64_t)ms * rate / 1000, m, (st & 0xe0) == 0xc0 ? 2 : 3, 1);
        if (ms > end) end = ms;
    }
    fclose(s);
    uint32_t frames = (uint32_t)((int64_t)(end + 1500) * rate / 1000);
    int16_t *pcm = calloc(frames * 2, 2);
    /* the DirectX 8 multi-bus path reAL's host reads, in sink-sized pieces (dry left / right; reverb not mixed) */
    msynth_seek(g_syn, 0);
    for (uint32_t at = 0; at < frames; at += 256) {
        uint32_t n = frames - at < 256 ? frames - at : 256;
        int16_t l[256], r[256], v[256], *bufs[3] = { l, r, v };
        static const uint32_t ids[3] = { 0, 1, 0x40 }, fn[3] = { 0, 1, 0x40 };
        static const int32_t pitch[3] = { 0, 0, 0 };
        memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); memset(v, 0, sizeof(v));
        msynth_read(g_syn, bufs, ids, fn, pitch, 3, n);
        for (uint32_t i = 0; i < n; i++) { pcm[2 * (at + i)] = l[i]; pcm[2 * (at + i) + 1] = r[i]; }
    }
    FILE *o = fopen(argv[3], "wb");
    fwrite("RIFF", 1, 4, o); put32(o, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, o); put32(o, 16); put16(o, 1); put16(o, 2);
    put32(o, rate); put32(o, rate * 4); put16(o, 4); put16(o, 16); fwrite("data", 1, 4, o); put32(o, frames * 4);
    fwrite(pcm, 4, frames, o);
    fclose(o);
    return 0;
}
