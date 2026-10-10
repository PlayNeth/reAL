/* render_seg <segment.sgt> <out.wav> <seconds> [flags]: play a segment through reAL-Music offline (performance,
 * DLS downloads, msynth) and write what DirectMusic's shared stereo+reverb audiopath plays: the dry stereo buses plus
 * the reverb send through the Waves reverb (default parameters).  DLS files are looked up next to the segment.
 * RS_DRY=1: dry only. */
#include "../src/music/dmperf.h"
#include "../src/music/dmdls.h"
#include "../src/music/msynth.h"
#include "../src/music/wavesrev.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define RATE 44100
static int64_t g_pos;                   /* samples rendered */
static const int64_t RT0 = 100000000;
static msynth *g_syn;
static dls_port *g_dls;
static char g_dir[512];
static int g_sig[2];

static rtime_t h_clock(void *c) { return RT0 + g_pos * 10000000 / RATE; }
static uint32_t h_msec(void *c) { return (uint32_t)(h_clock(c) / 10000); }
static void h_signal(void *c, int t) { g_sig[t] = 1; }
static int p_latency(void *c, rtime_t *t) { *t = h_clock(c) + 500000; return 1; }
static void p_play(void *c, const dm_event *ev, int n)
{
    for (int i = 0; i < n; i++) {
        int64_t t = (ev[i].rt - RT0) * RATE / 10000000;
        msynth_midi(g_syn, t, ev[i].data, ev[i].len, ev[i].group);
    }
}
static void p_prio(void *c, uint32_t g, uint32_t ch, uint32_t prio) { msynth_set_priority(g_syn, g, ch, prio); }
static void *p_download(void *c, dm_collection *coll, uint32_t patch, int sp, uint32_t np, const uint32_t *r, int nr, int *hr)
{
    return dls_port_download(g_dls, coll, patch, sp, np, r, nr, hr);
}
static void p_unload(void *c, void *h) { dls_port_unload(g_dls, h); }
static int s_dl(void *ctx, const void *buf, uint32_t size, void **h) { return msynth_download(g_syn, buf, size, h); }
static void s_ul(void *ctx, void *h) { msynth_unload(g_syn, h); }

static void *readfile(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(*n);
    if (fread(b, 1, *n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

static struct { char name[256]; dm_collection *c; } g_colls[64];
static dm_collection *getcoll(void *ctx, const dm_guid *g, const char *file)
{
    if (!file) return NULL;     /* default GM set: not available offline */
    char lower[256];
    size_t i;
    for (i = 0; file[i] && i < 255; i++) lower[i] = (char)tolower((unsigned char)file[i]);
    lower[i] = 0;
    for (int k = 0; k < 64; k++) if (g_colls[k].c && !strcmp(g_colls[k].name, lower)) return g_colls[k].c;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_dir, lower);
    long n;
    void *b = readfile(path, &n);
    if (!b) { fprintf(stderr, "missing %s\n", path); return NULL; }
    dm_collection *c = dls_load(b, n, 0);
    free(b);
    for (int k = 0; k < 64; k++) if (!g_colls[k].c) { strcpy(g_colls[k].name, lower); g_colls[k].c = c; break; }
    return c;
}

static void put16(FILE *f, int v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: render_seg seg.sgt out.wav seconds [flags]\n"); return 2; }
    strncpy(g_dir, argv[1], sizeof(g_dir) - 1);
    char *sl = strrchr(g_dir, '/');
    if (sl) *sl = 0; else strcpy(g_dir, ".");
    long n;
    void *b = readfile(argv[1], &n);
    if (!b) { perror(argv[1]); return 1; }
    dm_segment *seg = dm_segment_load(b, n, getcoll, NULL);
    if (!seg) { fprintf(stderr, "bad segment\n"); return 1; }
    double secs = atof(argv[3]);
    uint32_t flags = argc > 4 ? strtoul(argv[4], NULL, 0) : 0x280;

    g_syn = msynth_new(RATE, 64, 1, 1);
    msynth_set_gain(g_syn, 0);
    msynth_set_mmx(g_syn, 1);
    dls_sink sk = { s_dl, s_ul };
    g_dls = dls_port_new(&sk, NULL);
    dm_hostops ho = { h_clock, h_msec, h_signal };
    dm_perf *pf = dm_perf_new(&ho, NULL);
    int64_t dl[2];
    dl[0] = h_clock(NULL) + (int64_t)dm_perf_transport(pf) * 10000;
    dl[1] = h_clock(NULL) + (int64_t)dm_perf_realtime(pf) * 10000;
    dm_portops po = { p_latency, p_play, p_prio, p_download, p_unload };
    int port = dm_perf_add_port(pf, &po, NULL);
    dm_perf_assign_block(pf, 0, port, 1);
    if (dm_segment_download(pf, seg) < 0) fprintf(stderr, "download failed\n");
    dm_segstate *ss;
    if (dm_perf_play(pf, seg, flags, 0, NULL, &ss) < 0) { fprintf(stderr, "play failed\n"); return 1; }

    int64_t total = (int64_t)(secs * RATE);
    int16_t *pcm = calloc((size_t)total * 2 + 4096, sizeof(int16_t));
    wavesrev *rev = wavesrev_new(RATE);
    int dry = getenv("RS_DRY") != NULL;
    msynth_seek(g_syn, 0);
    for (int64_t ms = 0; g_pos < total; ms++) {
        int64_t to = (ms + 1) * RATE / 1000;
        if (to > total) to = total;
        if (to > g_pos) {
            uint32_t n = (uint32_t)(to - g_pos);
            int16_t l[64], r[64], v[64 * 2], *bufs[3] = { l, r, v };
            static const uint32_t ids[3] = { 0, 1, 0x40 }, fn[3] = { 0, 1, 0x40 };
            static const int32_t pitch[3] = { 0, 0, 0 };
            memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); memset(v, 0, sizeof(v));
            msynth_read(g_syn, bufs, ids, fn, pitch, 3, n);
            /* the reverb buffer is stereo and gets the mono send on both channels */
            for (int i = (int)n - 1; i >= 0; i--) v[2 * i] = v[2 * i + 1] = v[i];
            wavesrev_stereo16(rev, v, n);
            for (uint32_t i = 0; i < n; i++) {
                int a = l[i] + (dry ? 0 : v[2 * i]), b = r[i] + (dry ? 0 : v[2 * i + 1]);
                pcm[(g_pos + i) * 2] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
                pcm[(g_pos + i) * 2 + 1] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
            }
            g_pos = to;
        }
        rtime_t now = h_clock(NULL);
        for (int pass = 0; pass < 8; pass++) {
            int ran = 0;
            for (int th = 1; th >= 0; th--)
                if (g_sig[th] || dl[th] <= now) {
                    g_sig[th] = 0;
                    uint32_t w = th ? dm_perf_realtime(pf) : dm_perf_transport(pf);
                    dl[th] = now + (int64_t)w * 10000;
                    ran = 1;
                }
            if (!ran) break;
        }
    }
    FILE *f = fopen(argv[2], "wb");
    uint32_t bytes = (uint32_t)total * 4;
    fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
    put32(f, RATE); put32(f, RATE * 4); put16(f, 4); put16(f, 16); fwrite("data", 1, 4, f); put32(f, bytes);
    fwrite(pcm, 4, (size_t)total, f);
    fclose(f);
    return 0;
}
