/* test_perf <segment.sgt> <out.log> <ms> [flags]: play a segment through reAL-Music's performance with the threads
 * scheduled in virtual time the way tools/dmgt (DMGT_VTIME=1) schedules Microsoft's, and log every event handed to
 * the port as "<rt> <group> <bytes>" (same format as dmgt perf). */
#include "../src/music/dmperf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t g_now = 100000000;
static FILE *g_out;
static int g_sig[2];

static rtime_t h_clock(void *c) { return g_now; }
static uint32_t h_msec(void *c) { return (uint32_t)(g_now / 10000); }
static void h_signal(void *c, int t) { g_sig[t] = 1; }
static int p_latency(void *c, rtime_t *t) { *t = g_now; return 1; }
static void p_play(void *c, const dm_event *ev, int n)
{
    for (int i = 0; i < n; i++) {
        fprintf(g_out, "%lld %u", (long long)ev[i].rt, ev[i].group);
        for (uint32_t k = 0; k < ev[i].len; k++) fprintf(g_out, " %02x", ev[i].data[k]);
        fprintf(g_out, "\n");
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: test_perf seg.sgt out.log ms [flags]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);
    dm_segment *seg = dm_segment_load(buf, sz, NULL, NULL);
    if (!seg) { fprintf(stderr, "load failed\n"); return 1; }
    g_out = fopen(argv[2], "w");
    int ms = atoi(argv[3]);
    uint32_t flags = argc > 4 ? strtoul(argv[4], NULL, 0) : 0;
    const char *order = getenv("TP_ORDER");      /* realtime thread first on a shared tick (TP_ORDER=tr: transport first) */
    int rtfirst = !(order && !strcmp(order, "tr"));

    dm_hostops ho = { h_clock, h_msec, h_signal };
    dm_perf *pf = dm_perf_new(&ho, NULL);
    /* Init starts the threads: each runs one pass at the start time */
    int64_t dl[2];
    dl[0] = g_now + (int64_t)dm_perf_transport(pf) * 10000;
    dl[1] = g_now + (int64_t)dm_perf_realtime(pf) * 10000;
    dm_portops po = { p_latency, p_play, NULL, NULL, NULL };
    dm_perf_set_app_precision(pf, 1);     /* dmgt's main thread runs the x87 with 64-bit precision */
    int port = dm_perf_add_port(pf, &po, NULL);
    dm_perf_assign_block(pf, 0, port, 1);
    g_sig[0] = g_sig[1] = 0;
    dm_segstate *ss;
    int hr = dm_perf_play(pf, seg, flags, 0, NULL, &ss);
    if (hr < 0) { fprintf(stderr, "play %08x\n", hr); return 1; }
    /* TP_STOP=<ms>: stop the segment (as dmgt DMGT_STOP); TP_NEXT=<ms>:<segment>: play a second segment */
    int stop_ms = getenv("TP_STOP") ? atoi(getenv("TP_STOP")) : -1, next_ms = -1;
    dm_segment *seg2 = NULL;
    if (getenv("TP_NEXT")) {
        char *p;
        next_ms = strtol(getenv("TP_NEXT"), &p, 0);
        FILE *g = fopen(p + 1, "rb");
        if (!g) { perror(p + 1); return 1; }
        fseek(g, 0, SEEK_END); long n2 = ftell(g); fseek(g, 0, SEEK_SET);
        void *b2 = malloc(n2);
        if (fread(b2, 1, n2, g) != (size_t)n2) return 1;
        fclose(g);
        seg2 = dm_segment_load(b2, n2, NULL, NULL);
    }
    for (int t = 0; t <= ms + 1500; t++) {
        for (int phase = 0; phase < 2; phase++) {
            if (phase == 0 && t) g_now += 10000;
            if (phase == 1) {
                if (t == stop_ms) {      /* as MC2: Stop(segment, state), StopEx(segment), StopEx(state) */
                    dm_perf_stop(pf, seg, ss, 0, 0); dm_perf_stop(pf, seg, NULL, 0, 0); dm_perf_stop(pf, NULL, ss, 0, 0);
                }
                if (t == next_ms && seg2) { dm_segstate *s2; dm_perf_play(pf, seg2, flags, 0, NULL, &s2); }
            }
            for (int pass = 0; pass < 8; pass++) {
                int ran = 0;
                for (int j = 0; j < 2; j++) {
                    int th = rtfirst ? 1 - j : j;
                    if (g_sig[th] || (phase == 0 && dl[th] <= g_now)) {
                        g_sig[th] = 0;
                        uint32_t w = th ? dm_perf_realtime(pf) : dm_perf_transport(pf);
                        dl[th] = g_now + (int64_t)w * 10000;
                        ran = 1;
                    }
                }
                if (!ran) break;
            }
        }
    }
    mtime_t mt;
    rtime_t rt;
    dm_segstate_start_time(ss, &mt);
    dm_perf_m2r(pf, mt, &rt);
    fprintf(g_out, "# start mt %d rt %lld\n", mt, (long long)rt);
    fclose(g_out);
    return 0;
}
