/* test_perf <segment.sgt> <out.log> <ms> [flags]: play a segment through reDM's performance with the threads
 * scheduled in virtual time the way tools/dmgt (DMGT_VTIME=1) schedules Microsoft's, and log every event handed to
 * the port as "<rt> <group> <bytes>" (same format as dmgt perf). */
#include "../src/music/dmperf.h"
#include "../src/music/dmdls.h"
#include "../src/music/dmsty.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int64_t g_now = 100000000;
static FILE *g_out;
/* TP_NOTIFY=<file>: request notifications (TP_NTYPES letters: s segment, p performance, m measure/beat; default all)
 * and log every delivered one: "<rt> <mt> <type> <option> <field1> <field2> <state> <vtrack>" */
static FILE *g_nf;
static dm_segstate *g_ss1, *g_ss2;
static void drain_notifications(dm_perf *pf)
{
    dm_pmsg *m;
    while (g_nf && (m = dm_perf_get_notification(pf))) {
        const char *ty = m->nf_guid.d1 == 0xd2ac2899 ? "SEGMENT" : m->nf_guid.d1 == 0xd2ac289a ? "MEASUREANDBEAT" :
                         m->nf_guid.d1 == 0x81f75bc5 ? "PERFORMANCE" : "OTHER";
        const char *us = !m->user ? "-" : m->user == g_ss1 ? "s1" : m->user == g_ss2 ? "s2" : "s?";
        fprintf(g_nf, "%lld %d %s %u %u %u %s %u\n", (long long)m->rt, m->mt, ty, m->nf_option, m->nf_field1, m->nf_field2, us,
                m->vpch);
        dm_perf_free_pmsg(pf, m);
    }
}
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

/* collections, as dmgt has them: the GM set (TP_GM, default the Wine prefix's gm.dls) and DLS files next to the
 * segment; downloads go to a sink that accepts everything (the download path decides patches the bands send) */
static char g_dir[1024];
static struct { char name[256]; dm_collection *c; } g_colls[64];
static dm_collection *load_coll(const char *path, int gm)
{
    for (int k = 0; k < 64; k++) if (g_colls[k].c && !strcmp(g_colls[k].name, path)) return g_colls[k].c;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(n);
    dm_collection *c = fread(b, 1, n, f) == (size_t)n ? dls_load(b, n, gm) : NULL;
    fclose(f); free(b);
    for (int k = 0; k < 64; k++) if (!g_colls[k].c) { snprintf(g_colls[k].name, 256, "%s", path); g_colls[k].c = c; break; }
    return c;
}
static dm_collection *getcoll(void *ctx, const dm_guid *g, const char *file)
{
    char path[1400];
    if (!file) {
        const char *gm = getenv("TP_GM");
        if (!gm) { snprintf(path, sizeof(path), "%s/.wine/drive_c/windows/system32/drivers/gm.dls", getenv("HOME") ? getenv("HOME") : "/root"); gm = path; }
        return load_coll(gm, 1);
    }
    snprintf(path, sizeof(path), "%s/%s", g_dir, file);
    for (char *p = path + strlen(g_dir) + 1; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    return load_coll(path, 0);
}
static int s_dl(void *c, const void *b, uint32_t n, void **h) { *h = (void *)1; return 0; }
static void s_ul(void *c, void *h) {}
static dls_port *g_dls;
static void *p_download(void *c, dm_collection *coll, uint32_t patch, int sp, uint32_t np, const uint32_t *r, int nr, int *hr)
{
    return dls_port_download(g_dls, coll, patch, sp, np, r, nr, hr);
}
static void p_unload(void *c, void *h) { dls_port_unload(g_dls, h); }

/* waves: logged as dmgt's DMGT_WAVE does ("W ..." lines, objects numbered in order of appearance; downloads before
 * the segment plays are numbered but not logged, as dmgt's log is not open yet then) */
static void *g_wobj[1024]; static int g_nwobj, g_wlog;
static int wid(void *o) { for (int i = 0; i < g_nwobj; i++) if (g_wobj[i] == o) return i; if (g_nwobj < 1024) g_wobj[g_nwobj] = o; return g_nwobj++; }
static int g_hn;
static void *w_download(void *c, dm_wave *w, rtime_t hint, int *hr)
{
    void *d = (void *)(intptr_t)(0x10000 + ++g_hn);
    int a = wid(w), b = wid(d);
    if (g_wlog) fprintf(g_out, "W download w%d -> d%d hint %lld -> %08x\n", a, b, (long long)hint, 0);
    *hr = 0;
    return d;
}
static void w_unload(void *c, void *d) { int a = wid(d); if (g_wlog) fprintf(g_out, "W unload d%d\n", a); }
static void *w_alloc(void *c, void *d, uint32_t ch, uint32_t grp, rtime_t rt, uint32_t ls, uint32_t le, int *hr)
{
    void *v = (void *)(intptr_t)(0x10000 + ++g_hn);
    int a = wid(d), b = wid(v);
    fprintf(g_out, "W alloc d%d ch %u grp %u rt %lld loop %u %u -> v%d %08x\n", a, ch, grp, (long long)rt, ls, le, b, 0);
    *hr = 0;
    return v;
}
static int w_play(void *c, void *v, rtime_t rt, int32_t pitch, int32_t vol)
{
    fprintf(g_out, "W play v%d rt %lld pitch %d vol %d -> %08x\n", wid(v), (long long)rt, pitch, vol, 0);
    return 0;
}
static int w_stop(void *c, void *v, rtime_t rt) { fprintf(g_out, "W stop v%d rt %lld -> %08x\n", wid(v), (long long)rt, 0); return 0; }
static void w_free(void *c, void *v) {}
static dm_wave *getwave(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    static struct { char name[256]; dm_wave *w; } cache[64];
    static int n;
    if (!file) return NULL;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_dir, file);
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, path)) { dm_wave_addref(cache[i].w); return cache[i].w; }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    dm_wave *w = dm_wave_load(b, sz);
    free(b);
    if (w && n < 64) { snprintf(cache[n].name, 256, "%s", path); cache[n++].w = w; dm_wave_addref(w); }
    return w;
}

/* style references: the file next to the segment ('DMST' form), loaded once */
static dms_style *getstyle(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    static struct { char name[256]; dms_style *s; } cache[16];
    static int n;
    if (!file) return NULL;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", g_dir, file);
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, path)) { dms_style_addref(cache[i].s); return cache[i].s; }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz);
    if (fread(b, 1, sz, f) != (size_t)sz || sz < 12 || memcmp(b + 8, "DMST", 4)) { fclose(f); free(b); return NULL; }
    fclose(f);
    int32_t hr;
    dms_style *s = dms_style_load(b + 12, (size_t)sz - 12, getcoll, NULL, &hr);
    free(b);
    if (s && n < 16) { snprintf(cache[n].name, 256, "%s", path); cache[n++].s = s; dms_style_addref(s); }
    return s;
}

static long g_fixtime;
static long fixed_time(void) { return g_fixtime; }
static FILE *g_randlog;
static void randhook(int app, int v, void *caller)
{
    (void)caller;
    fprintf(g_randlog, "R %s %lld %d\n", app ? "app" : "tr", (long long)g_now, v);
}

int main(int argc, char **argv)
{
    if (getenv("TP_APPSEED")) { g_fixtime = strtol(getenv("TP_APPSEED"), NULL, 0); dm_set_time_source(fixed_time); }
    if (getenv("TP_RANDLOG")) { g_randlog = fopen(getenv("TP_RANDLOG"), "w"); dm_rand_hook = randhook; }
    if (argc < 4) { fprintf(stderr, "usage: test_perf seg.sgt out.log ms [flags]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) return 1;
    fclose(f);
    snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    { char *sl = strrchr(g_dir, '/'); if (sl) *sl = 0; else strcpy(g_dir, "."); }
    dm_getcoll_fn gc = getenv("TP_NOCOLL") ? NULL : getcoll;
    dls_sink sk = { s_dl, s_ul };
    g_dls = dls_port_new(&sk, NULL);
    g_out = fopen(argv[2], "w");
    dm_segment *seg = NULL;
    if (dm_is_midi(buf, sz)) seg = dm_segment_from_midi(buf, sz, gc, NULL);
    else if (sz > 12 && !memcmp((char *)buf + 8, "WAVE", 4)) { dm_wave *w = dm_wave_load(buf, sz); if (w) { wid(w); seg = dm_segment_from_wave(w); dm_wave_release(w); } }
    else { dm_loadcb cb = { gc, getwave, getstyle, NULL }; seg = dm_segment_load_cb(buf, sz, &cb); }
    if (!seg) { fprintf(stderr, "load failed\n"); return 1; }
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
    dm_portops po = { p_latency, p_play, NULL, p_download, p_unload, NULL, w_download, w_unload, w_alloc, w_play, w_stop, w_free };
    dm_perf_set_app_precision(pf, getenv("TP_PREC") ? atoi(getenv("TP_PREC")) : 1);     /* dmgt main thread: 64-bit x87 */
    if (getenv("TP_FIX")) dm_perf_set_fixes(pf, strtoul(getenv("TP_FIX"), NULL, 0));
    if (getenv("TP_SEED")) dm_perf_set_seed(pf, strtoul(getenv("TP_SEED"), NULL, 0));
    int port = dm_perf_add_port(pf, &po, NULL);
    dm_perf_assign_block(pf, 0, port, 1);
    g_sig[0] = g_sig[1] = 0;
    if (getenv("TP_SMF")) dm_segment_setparam(seg, PRM_SMF, 0xffffffff, 0, 0, NULL);    /* as dmgt with DMGT_SMF */
    if (getenv("TP_CLONE") && *getenv("TP_CLONE")) {       /* as dmgt with DMGT_CLONE */
        long a = 0, b = 0; int hr;
        sscanf(getenv("TP_CLONE"), "%ld:%ld", &a, &b);
        if (getenv("TP_CLONEFIX")) dm_set_clone_fix(1);
        dm_segment *c = dm_segment_clone(seg, a, b, &hr);
        fprintf(stderr, "clone(%ld, %ld): %08x\n", a, b, hr);
        if (c) { dm_segment_release(seg); seg = c; }
    }
    dm_segment_download(pf, seg);       /* as dmgt: Segment::Download before playing */
    if (getenv("TP_NOTIFY")) {
        static const dm_guid gs = { 0xd2ac2899, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
        static const dm_guid gm = { 0xd2ac289a, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
        static const dm_guid gp = { 0x81f75bc5, 0x4e5d, 0x11d2, { 0xbc, 0xc7, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb } };
        const char *ty = getenv("TP_NTYPES") ? getenv("TP_NTYPES") : "spm";
        g_nf = fopen(getenv("TP_NOTIFY"), "w");
        if (strchr(ty, 's')) dm_perf_add_notification_type(pf, &gs);
        if (strchr(ty, 'p')) dm_perf_add_notification_type(pf, &gp);
        if (strchr(ty, 'm')) dm_perf_add_notification_type(pf, &gm);
    }
    dm_segstate *ss;
    g_wlog = 1;
    int hr = dm_perf_play(pf, seg, flags, getenv("TP_STARTMT") ? atoi(getenv("TP_STARTMT")) : 0, NULL, &ss);
    g_ss1 = ss;
    if (hr < 0) { fprintf(stderr, "play %08x\n", hr); return 1; }
    /* TP_STOP=<ms>: stop the segment (as dmgt DMGT_STOP); TP_NEXT=<ms>:<segment>: play a second segment */
    int stop_ms = getenv("TP_STOP") && *getenv("TP_STOP") ? atoi(getenv("TP_STOP")) : -1, next_ms = -1;
    dm_segment *seg2 = NULL;
    if (getenv("TP_NEXT") && *getenv("TP_NEXT")) {
        char *p;
        next_ms = strtol(getenv("TP_NEXT"), &p, 0);
        char *colon = strrchr(p + 1, ':');
        if (colon && colon > p + 3) {       /* <ms>:<style file>:<motif> (as dmgt) */
            *colon = 0;
            const char *base = strrchr(p + 1, '/');
            dms_style *st = getstyle(NULL, NULL, base ? base + 1 : p + 1, NULL);
            if (!st) { fprintf(stderr, "no style %s\n", p + 1); return 1; }
            for (int k = 0; k < st->nmotifs && !seg2; k++) {
                char nm[64]; int j;
                for (j = 0; j < 63 && st->motifs[k]->name[j]; j++) nm[j] = (char)st->motifs[k]->name[j];
                nm[j] = 0;
                if (!strcasecmp(nm, colon + 1)) seg2 = dms_motif_segment(st, st->motifs[k]);
            }
            if (!seg2) { fprintf(stderr, "no motif %s\n", colon + 1); return 1; }
        } else {
            FILE *g = fopen(p + 1, "rb");
            if (!g) { perror(p + 1); return 1; }
            fseek(g, 0, SEEK_END); long n2 = ftell(g); fseek(g, 0, SEEK_SET);
            void *b2 = malloc(n2);
            if (fread(b2, 1, n2, g) != (size_t)n2) return 1;
            fclose(g);
            dm_loadcb cb2 = { gc, getwave, getstyle, NULL };
            seg2 = dm_segment_load_cb(b2, n2, &cb2);
        }
        if (seg2) dm_segment_download(pf, seg2);
    }
    for (int t = 0; t <= ms + 1500; t++) {
        for (int phase = 0; phase < 2; phase++) {
            if (phase == 0 && t) g_now += 10000;
            if (phase == 1) {
                drain_notifications(pf);
                if (t == stop_ms) {      /* as MC2: Stop(segment, state), StopEx(segment), StopEx(state) */
                    dm_perf_stop(pf, seg, ss, 0, 0); dm_perf_stop(pf, seg, NULL, 0, 0); dm_perf_stop(pf, NULL, ss, 0, 0);
                }
                if (t == next_ms && seg2) {
                    dm_segstate *s2;
                    uint32_t f2 = getenv("TP_FLAGS2") && *getenv("TP_FLAGS2") ? strtoul(getenv("TP_FLAGS2"), NULL, 0) : flags;
                    dm_perf_play(pf, seg2, f2, 0, NULL, &s2); g_ss2 = s2;
                }
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
    if (g_nf) fclose(g_nf);
    { void dm_perf_debug_tmap(dm_perf *); if (getenv("TP_TMAP")) dm_perf_debug_tmap(pf); }
    if (getenv("TP_M2R")) { rtime_t r2; dm_perf_m2r(pf, atoi(getenv("TP_M2R")), &r2); fprintf(stderr, "m2r(%s) = %lld\n", getenv("TP_M2R"), (long long)r2); }
    fclose(g_out);
    return 0;
}
