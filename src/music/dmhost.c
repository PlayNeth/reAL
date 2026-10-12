/* reAL-Music host: see dmhost.h.
 *
 * Audio: like DirectMusic's DirectSound sink, the synth is read in bus form (dry left/right and the reverb send)
 * into 16-bit stereo buffers: the dry one, and the shared reverb one whose Waves reverb we run ourselves before
 * writing it.  As in Windows 7's dsound.dll (sink 6efd878f/6efd7bf4): the standard reverb buffer is 16-bit stereo
 * on the one reverb-send bus, which is copied unchanged to both channels (6efdafe0) and then run through the buffer's
 * Waves reverb in place; the dry buffer interleaves the left and right buses (6efdb10c); no gain on either.
 * Both are reAL buffers, mixed (and resampled) by reAL's device like any game buffer.  The sink keeps a
 * fixed amount written ahead of the play cursor.
 * Time: the master clock follows the buffers' play position (so it never drifts from the audio), smoothed with
 * QueryPerformanceCounter between cursor reads; the latency clock is the time of the next sample to be written. */
#include "../real.h"
#include "dmhost.h"
#include "msynth.h"
#include "wavesrev.h"
#include "sf2dls.h"
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <string.h>

#define AHEAD_MS 80         /* written ahead of the play cursor */
#define BUF_MS 1000

/* the sink's buffers: [0] the shared stereo (dry) buffer, [1] the shared reverb buffer, then the audiopaths' own
 * (3D, mono, stereo).  Each takes one bus per channel (DirectX 8 sink: IDirectSoundSource::Read into the bus buffers) */
enum { HB_DRY, HB_REVERB, HB_3D, HB_MONO, HB_STEREO };
#define MAXHB 64
typedef struct { rl_buffer *b; int kind, nch, refs; uint32_t bus[2], func[2]; } hbuf;

struct dm_host {
    CRITICAL_SECTION lock;
    dm_perf *pf;
    msynth *syn;
    dls_port *dls;
    wavesrev *rev;
    uint32_t rate;
    rl_device *dev;
    rl_buffer *dry, *wet;
    hbuf hb[MAXHB];
    uint32_t nextbus;
    rl_buffer *primary;
    int buslog, peak[MAXHB];        /* REAL_MUSIC_BUSLOG: each sink buffer's peak, once a second */
    uint32_t bufframes;
    /* clock */
    LARGE_INTEGER qpf, qlast;
    rtime_t rt0, lastclock;
    int64_t played, written;
    DWORD lastcur;
    /* threads */
    HANDLE th[3], ev[2], quit;
    volatile LONG stop;
    int noreverb;
    FILE *dump;                 /* REAL_MUSIC_DUMP=file: raw 16-bit stereo of the dry + wet output (debugging) */
};

/* ------------------------------------------------------------------ clocks */
static rtime_t qpc_rt(dm_host *h)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (rtime_t)((double)c.QuadPart * 1e7 / (double)h->qpf.QuadPart);
}

static rtime_t host_clock(void *ctx)
{
    dm_host *h = ctx;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    int64_t since = (int64_t)((double)(c.QuadPart - h->qlast.QuadPart) * 1e7 / (double)h->qpf.QuadPart);
    if (since > 200000) since = 200000;         /* at most 20 ms past the last cursor reading */
    if (since < 0) since = 0;
    rtime_t t = h->rt0 + h->played * 10000000 / h->rate + since;
    if (t < h->lastclock) t = h->lastclock;
    h->lastclock = t;
    return t;
}

static uint32_t host_msec(void *ctx) { (void)ctx; return timeGetTime(); }
static void host_signal(void *ctx, int t) { dm_host *h = ctx; SetEvent(h->ev[t]); }

/* ------------------------------------------------------------------ the synth port */
static int port_latency(void *ctx, rtime_t *t)
{
    dm_host *h = ctx;
    *t = h->rt0 + h->written * 10000000 / h->rate;
    return 1;
}

static void port_play(void *ctx, const dm_event *ev, int n)
{
    dm_host *h = ctx;
    for (int i = 0; i < n; i++) {
        int64_t t = (ev[i].rt - h->rt0) * h->rate / 10000000;
        msynth_midi(h->syn, t, ev[i].data, ev[i].len, ev[i].group);
    }
}

static void port_priority(void *ctx, uint32_t g, uint32_t ch, uint32_t prio) { msynth_set_priority(((dm_host *)ctx)->syn, g, ch, prio); }

static void *port_download(void *ctx, dm_collection *c, uint32_t patch, int sp, uint32_t np, const uint32_t *r, int nr, int *hr)
{
    void *d = dls_port_download(((dm_host *)ctx)->dls, c, patch, sp, np, r, nr, hr);
    if (!d) LOG_INFO("music: download of instrument %08x failed (%08x%s)", patch, *hr,
                     *hr == (int)0x88781129 ? ": it has no articulation, Microsoft's synth refuses it too" : "");
    return d;
}
static void port_unload(void *ctx, void *handle) { dls_port_unload(((dm_host *)ctx)->dls, handle); }
static void port_assign(void *ctx, uint32_t g, uint32_t c, const uint32_t *buses, uint32_t n) { msynth_assign_buses(((dm_host *)ctx)->syn, g, c, buses, n); }
static int sink_dl(void *ctx, const void *buf, uint32_t size, void **handle)
{
    int r = msynth_download(((dm_host *)ctx)->syn, buf, size, handle);
    if (r == 1) {
        const uint32_t *h = buf;    /* DMUS_DOWNLOADINFO + offset table; offset 0 is the DMUS_INSTRUMENT (patch first) */
        uint32_t patch = size >= 20 && h[4] + 4 <= size ? *(const uint32_t *)((const uint8_t *)buf + h[4]) : 0;
        LOG_INFO("music: instrument %08x has regions without articulation (Microsoft's synth refuses it): "
                 "playing them with the DLS default articulation", patch);
    }
    if (r < 0) LOG_INFO("music: synth refused a %s download (%d, %u bytes)", size >= 4 && *(const uint32_t *)buf == 2 ? "wave" : "instrument", r, size);
    return r;
}
static void sink_ul(void *ctx, void *handle) { msynth_unload(((dm_host *)ctx)->syn, handle); }


/* ------------------------------------------------------------------ the synth port's waves (dmusic.dll): a wave is
 * downloaded as dmusic does a one-shot wave (77192536 / 771934f7 / 77193799): one buffer per channel (type 6, the
 * whole channel) and one articulation per channel (type 4: the channel's bus, multichannel option when stereo); a
 * voice plays every channel (771945bd), with consecutive voice ids.  A wave already downloaded is shared (77192735).
 * Waves stream in dmusic when they ask to; here every wave is whole in memory, which plays the same. */
typedef struct hwdl {
    struct hwdl *next;
    int refs;
    dm_wave *w;
    uint32_t nch, rate;
    void *buf[8], *art[8];
    uint32_t artid[8];
} hwdl;
typedef struct { hwdl *dl; uint32_t id, chan, group; uint64_t start, ls, le; } hvoice;
static hwdl *g_wdls;
static uint32_t g_waveid = 0x40000000, g_voiceid;

static void hwdl_release(dm_host *h, hwdl *d)
{
    if (--d->refs > 0) return;
    for (hwdl **p = &g_wdls; *p; p = &(*p)->next) if (*p == d) { *p = d->next; break; }
    for (uint32_t c = 0; c < d->nch; c++) {
        if (d->art[c]) msynth_unload(h->syn, d->art[c]);
        if (d->buf[c]) msynth_unload(h->syn, d->buf[c]);
    }
    dm_wave_release(d->w);
    free(d);
}

static void *port_wave_download(void *ctx, dm_wave *w, rtime_t hint, int *hr)
{
    dm_host *h = ctx;
    (void)hint;
    if (!w->streaming)
        for (hwdl *d = g_wdls; d; d = d->next) if (d->w == w) { d->refs++; *hr = 0; return d; }
    if (w->channels == 0 || w->channels > 8) { *hr = (int)0x8878112a; return NULL; }
    hwdl *d = calloc(1, sizeof(*d));
    if (!d) { *hr = (int)0x8007000e; return NULL; }
    d->refs = 1; d->w = w; dm_wave_addref(w);
    d->nch = w->channels; d->rate = w->rate;
    uint32_t bps = w->bits / 8, bytes = w->frames * bps;
    for (uint32_t c = 0; c < d->nch; c++) {
        /* DMUS_DOWNLOADINFO + 2 offsets + DMUS_WAVEDATA (771927fe) */
        uint32_t size = 0x1c + ((bytes + 3) & ~3u);
        uint32_t *b = calloc(1, size + 4);
        uint32_t bid = g_waveid++;
        b[0] = 6; b[1] = bid; b[2] = 2; b[3] = size; b[4] = 0x18; b[5] = 0x1c; b[6] = bytes;
        uint8_t *o = (uint8_t *)(b + 7);
        for (uint32_t k = 0; k < w->frames; k++)
            memcpy(o + k * bps, w->data + (k * d->nch + c) * bps, bps);
        int r = msynth_download(h->syn, b, size, &d->buf[c]);
        free(b);
        if (r < 0) { *hr = (int)0x80004005; d->nch = c; hwdl_release(h, d); return NULL; }
        /* the articulation (77192ff8): DMUS_WAVEARTDL, WAVEFORMATEX, the buffer's id */
        uint32_t a[32] = { 0 };
        uint32_t aid = g_waveid++;
        a[0] = 4; a[1] = aid; a[2] = 3; a[3] = 0x50; a[4] = 0x1c; a[5] = 0x30; a[6] = 0x44;
        a[7] = 1; a[8] = c; a[9] = 1; a[10] = 0; a[11] = d->nch > 1 ? 2 : 0;
        uint8_t *wf = (uint8_t *)(a + 12);
        uint16_t v16;
        v16 = 1; memcpy(wf, &v16, 2);
        v16 = (uint16_t)d->nch; memcpy(wf + 2, &v16, 2);
        memcpy(wf + 4, &w->rate, 4);
        uint32_t avg = w->rate * bps * d->nch; memcpy(wf + 8, &avg, 4);
        v16 = (uint16_t)(bps * d->nch); memcpy(wf + 12, &v16, 2);
        v16 = w->bits; memcpy(wf + 14, &v16, 2);
        a[17] = bid;
        r = msynth_download(h->syn, a, 0x50, &d->art[c]);
        if (r < 0) { *hr = (int)0x80004005; d->nch = c + 1; hwdl_release(h, d); return NULL; }
        d->artid[c] = aid;
    }
    d->next = g_wdls; g_wdls = d;
    *hr = 0;
    return d;
}
static void port_wave_unload(void *ctx, void *dl) { hwdl_release(ctx, dl); }

static void *port_voice_alloc(void *ctx, void *dl, uint32_t chan, uint32_t group, rtime_t start, uint32_t ls,
                              uint32_t le, int *hr)
{
    hwdl *d = dl;
    hvoice *v = calloc(1, sizeof(*v));
    if (!v) { *hr = (int)0x8007000e; return NULL; }
    v->dl = d; d->refs++;
    v->id = g_voiceid; g_voiceid += d->nch;       /* 77193fa9 */
    v->chan = chan; v->group = group;
    v->start = (uint64_t)((uint64_t)d->rate * (uint64_t)start / 10000000);   /* 77192f56 */
    v->ls = ls; v->le = le;
    *hr = 0;
    return v;
}
static int64_t rt_sample(dm_host *h, rtime_t rt) { return rt ? (rt - h->rt0) * h->rate / 10000000 : 0; }
static int port_voice_play(void *ctx, void *voice, rtime_t rt, int32_t pitch, int32_t vol)
{
    dm_host *h = ctx;
    hvoice *v = voice;
    for (uint32_t c = 0; c < v->dl->nch; c++)
        msynth_play_voice(h->syn, rt_sample(h, rt), v->id + c, v->group, v->chan, v->dl->artid[c], pitch, vol, v->start, v->ls, v->le);
    return 0;
}
static int port_voice_stop(void *ctx, void *voice, rtime_t rt)
{
    dm_host *h = ctx;
    hvoice *v = voice;
    for (uint32_t c = 0; c < v->dl->nch; c++) msynth_stop_voice(h->syn, rt_sample(h, rt), v->id + c);
    return 0;
}
static void port_voice_free(void *ctx, void *voice)
{
    hvoice *v = voice;
    hwdl_release(ctx, v->dl);
    free(v);
}

/* ------------------------------------------------------------------ the sink */
/* the standard buffers' formats as dmime asks for them (7732084b): 16-bit at the synth rate */
static rl_buffer *make_buffer_ex(dm_host *h, int nch, DWORD flags, const GUID *alg3d)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)nch, h->rate, h->rate * 2 * nch, (WORD)(2 * nch), 16, 0 };
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = flags | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_LOCSOFTWARE;
    d.dwBufferBytes = h->bufframes * 2 * nch;
    d.lpwfxFormat = &wf;
    if (alg3d) d.guid3DAlgorithm = *alg3d;
    rl_buffer *b = NULL;
    if (FAILED(rl_buffer_create(h->dev, NULL, &d, &b))) return NULL;
    return b;
}
static rl_buffer *make_buffer(dm_host *h) { return make_buffer_ex(h, 2, DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME, NULL); }

static void write_buffer(rl_buffer *b, uint32_t frame, const int16_t *src, uint32_t n, int nch)
{
    void *p1, *p2;
    DWORD n1, n2;
    uint32_t fb = 2 * nch;
    if (FAILED(IDirectSoundBuffer8_Lock(&b->IDirectSoundBuffer8_iface, frame * fb, n * fb, &p1, &n1, &p2, &n2, 0))) return;
    memcpy(p1, src, n1);
    if (p2) memcpy(p2, (const BYTE *)src + n1, n2);
    IDirectSoundBuffer8_Unlock(&b->IDirectSoundBuffer8_iface, p1, n1, p2, n2);
}

static void sink_pass(dm_host *h)
{
    DWORD play = 0, wr = 0;
    if (FAILED(IDirectSoundBuffer8_GetCurrentPosition(&h->dry->IDirectSoundBuffer8_iface, &play, &wr))) return;
    play /= 4;
    uint32_t delta = (play + h->bufframes - h->lastcur) % h->bufframes;
    h->lastcur = play;
    h->played += delta;
    QueryPerformanceCounter(&h->qlast);
    int64_t target = h->played + (int64_t)h->rate * AHEAD_MS / 1000;
    if (h->written < h->played) {           /* fell behind: skip */
        h->written = h->played;
        msynth_seek(h->syn, h->written);
    }
    static uint32_t ids[2 * MAXHB], fn[2 * MAXHB];
    static int32_t pitch[2 * MAXHB];
    static int16_t tmp[2 * MAXHB][256], out[512];
    int16_t *bufs[2 * MAXHB];
    int nb = 0, first[MAXHB];
    for (int k = 0; k < MAXHB; k++) {
        hbuf *x = &h->hb[k];
        first[k] = nb;
        if (!x->b) continue;
        int32_t pc = x->kind >= HB_3D ? rl_buffer_sink_cents(x->b) : 0;
        for (int c = 0; c < x->nch; c++) { ids[nb] = x->bus[c]; fn[nb] = x->func[c]; pitch[nb] = pc; bufs[nb] = tmp[nb]; nb++; }
    }
    while (h->written < target) {
        uint32_t n = (uint32_t)(target - h->written);
        if (n > 256) n = 256;
        for (int k = 0; k < nb; k++) memset(tmp[k], 0, n * sizeof(int16_t));
        msynth_read(h->syn, bufs, ids, fn, pitch, nb, n);
        uint32_t at = (uint32_t)(h->written % h->bufframes);
        int16_t dry[512], v[512];
        for (int k = 0; k < MAXHB; k++) {
            hbuf *x = &h->hb[k];
            if (!x->b) continue;
            const int16_t *a = tmp[first[k]], *b2 = x->nch > 1 ? tmp[first[k] + 1] : NULL;
            switch (x->kind) {
            case HB_DRY: case HB_STEREO:            /* two buses interleaved */
                for (uint32_t i = 0; i < n; i++) { out[2 * i] = a[i]; out[2 * i + 1] = b2[i]; }
                if (x->kind == HB_DRY) memcpy(dry, out, n * 4);
                write_buffer(x->b, at, out, n, 2);
                break;
            case HB_REVERB:                         /* the mono send to both channels, then the Waves reverb */
                for (uint32_t i = 0; i < n; i++) v[2 * i] = v[2 * i + 1] = a[i];
                if (h->noreverb) memset(v, 0, n * 4);
                else wavesrev_stereo16(h->rev, v, n);
                write_buffer(x->b, at, v, n, 2);
                break;
            default:                                /* 3D and mono buffers: the bus as it is */
                write_buffer(x->b, at, a, n, 1);
                break;
            }
            if (h->buslog) for (uint32_t i = 0; i < n * x->nch; i++) {
                int v2 = (x->nch > 1 ? (i & 1 ? b2 : a)[i / 2] : a[i]);
                if (v2 < 0) v2 = -v2;
                if (v2 > h->peak[k]) h->peak[k] = v2;
            }
        }
        if (h->dump) {
            int16_t mix[512];
            for (uint32_t k = 0; k < 2 * n; k++) { int x = dry[k] + v[k]; mix[k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
            fwrite(mix, 4, n, h->dump);
        }
        h->written += n;
        if (h->buslog && h->written / h->rate != (h->written - n) / h->rate) {
            for (int k = 0; k < MAXHB; k++) if (h->hb[k].b) { LOG_INFO("music: bus buffer %d (kind %d) peak %d", k, h->hb[k].kind, h->peak[k]); h->peak[k] = 0; }
        }
    }
}

static DWORD WINAPI sink_thread(LPVOID p)
{
    dm_host *h = p;
    timeBeginPeriod(1);
    while (!h->stop) {
        EnterCriticalSection(&h->lock);
        sink_pass(h);
        LeaveCriticalSection(&h->lock);
        WaitForSingleObject(h->quit, 5);
    }
    timeEndPeriod(1);
    return 0;
}

static DWORD WINAPI transport_thread(LPVOID p)
{
    dm_host *h = p;
    _controlfp(_PC_53, _MCW_PC);    /* dmime's threads run the tempo arithmetic at 53-bit precision */
    HANDLE w[2] = { h->quit, h->ev[DM_THREAD_TRANSPORT] };
    while (!h->stop) {
        EnterCriticalSection(&h->lock);
        uint32_t ms = dm_perf_transport(h->pf);
        LeaveCriticalSection(&h->lock);
        WaitForMultipleObjects(2, w, FALSE, ms);
    }
    return 0;
}

static DWORD WINAPI realtime_thread(LPVOID p)
{
    dm_host *h = p;
    _controlfp(_PC_53, _MCW_PC);    /* dmime's threads run the tempo arithmetic at 53-bit precision */
    HANDLE w[2] = { h->quit, h->ev[DM_THREAD_REALTIME] };
    timeBeginPeriod(1);
    while (!h->stop) {
        EnterCriticalSection(&h->lock);
        uint32_t ms = dm_perf_realtime(h->pf);
        LeaveCriticalSection(&h->lock);
        WaitForMultipleObjects(2, w, FALSE, ms);
    }
    timeEndPeriod(1);
    return 0;
}

/* ------------------------------------------------------------------ construction */
dm_host *dm_host_new(uint32_t rate, uint32_t voices, uint32_t groups)
{
    dm_host *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    InitializeCriticalSection(&h->lock);
    h->rate = rate ? rate : 22050;
    h->bufframes = h->rate * BUF_MS / 1000;
    QueryPerformanceFrequency(&h->qpf);
    QueryPerformanceCounter(&h->qlast);
    h->rt0 = qpc_rt(h);
    h->lastclock = h->rt0;
    {
        char dp[MAX_PATH];
        if (GetEnvironmentVariableA("REAL_MUSIC_DUMP", dp, MAX_PATH)) h->dump = fopen(dp, "wb");
        h->buslog = GetEnvironmentVariableA("REAL_MUSIC_BUSLOG", dp, MAX_PATH) != 0;
    }
    if (FAILED(rl_device_acquire(&h->dev))) { free(h); return NULL; }
    h->syn = msynth_new(h->rate, voices ? (int)voices : 64, groups ? (int)groups : 64, 1);
    if (h->syn) msynth_set_fixes(h->syn, MS_FIX_WAVE_END | MS_FIX_WAVE_PASTEND);
    msynth_set_gain(h->syn, 0);
    dls_sink sk = { sink_dl, sink_ul };
    h->dls = dls_port_new(&sk, h);
    h->rev = wavesrev_new(h->rate);
    h->dry = make_buffer(h);
    h->wet = make_buffer(h);
    if (!h->syn || !h->dls || !h->rev || !h->dry || !h->wet) { dm_host_free(h); return NULL; }
    rl_buffer_set_sink(h->dry, 0x482c0);
    rl_buffer_set_sink(h->wet, 0x482c0);
    h->hb[0] = (hbuf){ h->dry, HB_DRY, 2, 1, { 0, 1 }, { 0, 1 } };
    h->hb[1] = (hbuf){ h->wet, HB_REVERB, 1, 1, { 0x40 }, { 0x40 } };
    h->nextbus = 0x1000;
    dm_hostops ho = { host_clock, host_msec, host_signal };
    h->pf = dm_perf_new(&ho, h);
    /* fixes of Microsoft's flaws: on, except for comparison runs (REAL_MUSIC_NOFIXES in the environment) */
    int fixes = !GetEnvironmentVariableA("REAL_MUSIC_NOFIXES", NULL, 0);
    if (h->pf && fixes) dm_perf_set_fixes(h->pf, DM_FIX_FIRST_TEMPO | DM_FIX_MEASURE_NUMBER | DM_FIX_WAVE_SEEK |
                                          DM_FIX_WAVE_SYNCVAR | DM_FIX_STATE_TRACKCONFIG);
    if (!fixes) msynth_set_fixes(h->syn, 0);
    dm_wave_set_fix_length(fixes);
    dm_set_clone_fix(fixes);
    dm_portops po = { port_latency, port_play, port_priority, port_download, port_unload, port_assign,
                      port_wave_download, port_wave_unload, port_voice_alloc, port_voice_play, port_voice_stop, port_voice_free };
    dm_perf_add_port(h->pf, &po, h);
    /* start both buffers in the same mixer period so they stay sample-aligned */
    EnterCriticalSection(&h->dev->lock);
    IDirectSoundBuffer8_Play(&h->dry->IDirectSoundBuffer8_iface, 0, 0, DSBPLAY_LOOPING);
    IDirectSoundBuffer8_Play(&h->wet->IDirectSoundBuffer8_iface, 0, 0, DSBPLAY_LOOPING);
    LeaveCriticalSection(&h->dev->lock);
    h->quit = CreateEventA(NULL, TRUE, FALSE, NULL);
    h->ev[0] = CreateEventA(NULL, FALSE, FALSE, NULL);
    h->ev[1] = CreateEventA(NULL, FALSE, FALSE, NULL);
    EnterCriticalSection(&h->lock);
    sink_pass(h);
    LeaveCriticalSection(&h->lock);
    h->th[0] = CreateThread(NULL, 0, transport_thread, h, 0, NULL);
    h->th[1] = CreateThread(NULL, 0, realtime_thread, h, 0, NULL);
    h->th[2] = CreateThread(NULL, 0, sink_thread, h, 0, NULL);
    SetThreadPriority(h->th[1], THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadPriority(h->th[2], THREAD_PRIORITY_HIGHEST);
    LOG_INFO("music: host %p, synth %u Hz, %u voices", (void *)h, h->rate, voices);
    return h;
}

void dm_host_free(dm_host *h)
{
    if (!h) return;
    InterlockedExchange(&h->stop, 1);
    if (h->quit) SetEvent(h->quit);
    for (int i = 0; i < 3; i++) if (h->th[i]) { WaitForSingleObject(h->th[i], 2000); CloseHandle(h->th[i]); }
    for (int i = 0; i < 2; i++) if (h->ev[i]) CloseHandle(h->ev[i]);
    if (h->quit) CloseHandle(h->quit);
    for (int k = 2; k < MAXHB; k++) if (h->hb[k].b) IDirectSoundBuffer8_Release(&h->hb[k].b->IDirectSoundBuffer8_iface);
    if (h->primary) IDirectSoundBuffer8_Release(&h->primary->IDirectSoundBuffer8_iface);
    if (h->dry) IDirectSoundBuffer8_Release(&h->dry->IDirectSoundBuffer8_iface);
    if (h->wet) IDirectSoundBuffer8_Release(&h->wet->IDirectSoundBuffer8_iface);
    dm_perf_free(h->pf);
    dls_port_free(h->dls);
    msynth_free(h->syn);
    wavesrev_free(h->rev);
    if (h->dev) rl_device_release(h->dev);
    if (h->dump) fclose(h->dump);
    DeleteCriticalSection(&h->lock);
    free(h);
}

void dm_host_lock(dm_host *h) { EnterCriticalSection(&h->lock); }
void dm_host_unlock(dm_host *h) { LeaveCriticalSection(&h->lock); }
dm_perf *dm_host_perf(dm_host *h) { return h->pf; }
uint32_t dm_host_rate(dm_host *h) { return h->rate; }
rtime_t dm_host_clock(dm_host *h) { return host_clock(h); }
void dm_host_set_volume(dm_host *h, int32_t mb)
{
    EnterCriticalSection(&h->lock);
    msynth_set_gain(h->syn, mb);
    LeaveCriticalSection(&h->lock);
}

/* a MIDI message played as soon as possible: stamped at the play position plus the sink's lead, so messages keep
 * their relative timing (no quantising to sink passes); never before what has been rendered already */
void dm_host_midi(dm_host *h, const uint8_t *msg, uint32_t len)
{
    EnterCriticalSection(&h->lock);
    int64_t t = (host_clock(h) - h->rt0) * h->rate / 10000000 + (int64_t)h->rate * AHEAD_MS / 1000;
    if (t < h->written) t = h->written;
    msynth_midi(h->syn, t, msg, len, 1);
    LeaveCriticalSection(&h->lock);
}

/* every instrument of a collection, as a GM synth has them (the Microsoft GS Wavetable synth loads all of gm.dls) */
int dm_host_download_all(dm_host *h, dm_collection *c)
{
    int n = 0;
    EnterCriticalSection(&h->lock);
    for (int i = 0; i < dls_count(c); i++) {
        uint32_t patch;
        int hr;
        if (dls_enum(c, i, &patch, NULL) < 0) continue;
        if (dls_port_download(h->dls, c, patch, 0, 0, NULL, 0, &hr)) n++;
    }
    LeaveCriticalSection(&h->lock);
    return n;
}

/* ------------------------------------------------------------------ audiopath buffers
 * A standard audiopath's buffers (dmime 773211f9 / 7732084b): shared stereo + reverb (DMUS_APATH_SHARED_STEREOPLUSREVERB),
 * a 3D mono buffer (DYNAMIC_3D, no virtualization asked), a mono one (DYNAMIC_MONO) or a stereo one (DYNAMIC_STEREO).
 * The path's buses are written to buses[] (up to 4); the buffers' indices to idx[] (up to 2). */
static const GUID DS3DALG_NO_VIRTUALIZATION_ = { 0xc241333f, 0x1c1b, 0x11d2, { 0x94, 0xf5, 0x00, 0xc0, 0x4f, 0xc2, 0x8a, 0xca } };
int dm_host_path_open(dm_host *h, uint32_t type, int idx[2], int *nidx, uint32_t buses[4], uint32_t *nbuses)
{
    *nidx = 0; *nbuses = 0;
    EnterCriticalSection(&h->lock);
    int r = -1;
    if (type == 1) {        /* DMUS_APATH_SHARED_STEREOPLUSREVERB */
        h->hb[0].refs++; h->hb[1].refs++;
        idx[0] = 0; idx[1] = 1; *nidx = 2;
        buses[0] = 0; buses[1] = 1; buses[2] = 0x40; *nbuses = 3;
        r = 0;
    } else if (type == 6 || type == 7 || type == 8) {
        int k;
        for (k = 2; k < MAXHB && h->hb[k].b; k++) ;
        if (k < MAXHB) {
            hbuf *x = &h->hb[k];
            rl_buffer *b;
            if (type == 6) b = make_buffer_ex(h, 1, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS |
                                              DSBCAPS_MUTE3DATMAXDISTANCE, &DS3DALG_NO_VIRTUALIZATION_);
            else if (type == 7) b = make_buffer_ex(h, 1, DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS, NULL);
            else b = make_buffer_ex(h, 2, DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GLOBALFOCUS, NULL);
            if (b) {
                rl_buffer_set_sink(b, type == 6 ? 0x682b0 : 0x482e0);      /* 7732084b's flags | CTRLFX */
                x->b = b; x->refs = 1;
                x->kind = type == 6 ? HB_3D : type == 7 ? HB_MONO : HB_STEREO;
                x->nch = type == 8 ? 2 : 1;
                for (int c = 0; c < x->nch; c++) {
                    x->bus[c] = h->nextbus++;
                    x->func[c] = type == 6 ? 0x200 : (uint32_t)c;     /* DSBUSID_DYNAMIC_0 / LEFT, RIGHT */
                    buses[c] = x->bus[c];
                }
                *nbuses = x->nch;
                idx[0] = k; *nidx = 1;
                /* in step with the shared buffers: same frame position, playing */
                DWORD play = 0, wr = 0;
                IDirectSoundBuffer8_GetCurrentPosition(&h->dry->IDirectSoundBuffer8_iface, &play, &wr);
                IDirectSoundBuffer8_SetCurrentPosition(&b->IDirectSoundBuffer8_iface, play / 4 * 2 * x->nch);
                IDirectSoundBuffer8_Play(&b->IDirectSoundBuffer8_iface, 0, 0, DSBPLAY_LOOPING);
                r = 0;
            }
        }
    }
    LeaveCriticalSection(&h->lock);
    return r;
}

void dm_host_path_close(dm_host *h, const int *idx, int nidx)
{
    EnterCriticalSection(&h->lock);
    for (int i = 0; i < nidx; i++) {
        hbuf *x = &h->hb[idx[i]];
        if (--x->refs > 0 || idx[i] < 2) continue;
        rl_buffer *b = x->b;
        memset(x, 0, sizeof(*x));
        IDirectSoundBuffer8_Stop(&b->IDirectSoundBuffer8_iface);
        IDirectSoundBuffer8_Release(&b->IDirectSoundBuffer8_iface);
    }
    LeaveCriticalSection(&h->lock);
}

/* the buffer object (AudioPath::GetObjectInPath, DMUS_PATH_BUFFER) */
HRESULT dm_host_buffer_query(dm_host *h, int idx, REFIID iid, void **out)
{
    *out = NULL;
    if (idx < 0 || idx >= MAXHB || !h->hb[idx].b) return DMUS_E_NOT_FOUND;
    return IDirectSoundBuffer8_QueryInterface(&h->hb[idx].b->IDirectSoundBuffer8_iface, iid, out);
}

/* DMUS_PATH_PRIMARY_BUFFER: the primary buffer (with the 3D listener) of reAL's device */
HRESULT dm_host_primary_query(dm_host *h, REFIID iid, void **out)
{
    *out = NULL;
    EnterCriticalSection(&h->lock);
    if (!h->primary) {
        DSBUFFERDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
        rl_buffer_create(h->dev, NULL, &d, &h->primary);
    }
    LeaveCriticalSection(&h->lock);
    if (!h->primary) return DMUS_E_NOT_FOUND;
    return IDirectSoundBuffer8_QueryInterface(&h->primary->IDirectSoundBuffer8_iface, iid, out);
}

/* the shared reverb (DMUS_PATH_BUFFER_DMO on the reverb buffer): IDirectSoundFXWavesReverb parameters */
int dm_host_reverb_set(dm_host *h, float ingain, float mix, float time, float hfratio)
{
    EnterCriticalSection(&h->lock);
    int r = wavesrev_set(h->rev, ingain, mix, time, hfratio);
    LeaveCriticalSection(&h->lock);
    return r;
}

void dm_host_set_reverb(dm_host *h, int on)
{
    EnterCriticalSection(&h->lock);
    h->noreverb = !on;
    LeaveCriticalSection(&h->lock);
}

/* ------------------------------------------------------------------ collections */
static CRITICAL_SECTION s_ccs;
static INIT_ONCE s_cinit = INIT_ONCE_STATIC_INIT;
static struct { char path[MAX_PATH]; dm_collection *c; } s_coll[128];

static BOOL CALLBACK cinit(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&s_ccs); return TRUE; }

static HANDLE open_file(const char *path);
dm_collection *dm_collection_from_file(const char *path, int gm)
{
    InitOnceExecuteOnce(&s_cinit, cinit, NULL, NULL);
    char full[MAX_PATH];
    if (!GetFullPathNameA(path, MAX_PATH, full, NULL)) lstrcpynA(full, path, MAX_PATH);
    CharLowerA(full);
    EnterCriticalSection(&s_ccs);
    for (int i = 0; i < 128; i++)
        if (s_coll[i].c && !strcmp(s_coll[i].path, full)) { LeaveCriticalSection(&s_ccs); return s_coll[i].c; }
    dm_collection *c = NULL;
    HANDLE f = open_file(full);
    DWORD err = f == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    int parsed = 0;
    if (f != INVALID_HANDLE_VALUE) {
        parsed = 1;
        DWORD size = GetFileSize(f, NULL), got = 0;
        void *b = malloc(size ? size : 1);
        if (b && ReadFile(f, b, size, &got, NULL) && got == size) {
            if (size >= 12 && !memcmp(b, "RIFF", 4) && !memcmp((char *)b + 8, "sfbk", 4)) {
                size_t dn = 0;
                int ni = 0;
                void *d = sf2_to_dls(b, size, &dn, &ni);
                free(b);                    /* the SoundFont's samples are in d now: no need for both in memory */
                b = NULL;
                if (d) {
                    LOG_INFO("music: %s is a SoundFont: %d presets converted to DLS (%lu KB)", full, ni, (unsigned long)(dn >> 10));
                    c = dls_load_owned(d, dn, gm);
                } else if (dn) LOG_INFO("music: %s: not enough memory to convert the SoundFont (%lu KB needed)", full, (unsigned long)(dn >> 10));
            } else { c = dls_load_owned(b, size, gm); b = NULL; }
        }
        free(b);
        CloseHandle(f);
    }
    if (c) {
        for (int i = 0; i < 128; i++) if (!s_coll[i].c) { lstrcpynA(s_coll[i].path, full, MAX_PATH); s_coll[i].c = c; break; }
        LOG_INFO("music: collection %s (%d instruments)", full, dls_count(c));
    } else if (parsed) LOG_INFO("music: %s is not a DLS collection reAL can read", full);
    else LOG_INFO("music: cannot open collection %s (error %lu)", full, (unsigned long)err);
    LeaveCriticalSection(&s_ccs);
    return c;
}

/* CreateFile, retried with WOW64 file system redirection off: a 32-bit game asking for system32\... is sent to
 * syswow64\... (on Windows and in 64-bit Wine prefixes), where gm.dls may be missing */
static HANDLE open_file(const char *path)
{
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) return f;
    BOOL (WINAPI *dis)(PVOID *) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "Wow64DisableWow64FsRedirection");
    BOOL (WINAPI *rev)(PVOID) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "Wow64RevertWow64FsRedirection");
    PVOID old;
    if (dis && rev && dis(&old)) {
        f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        rev(old);
    }
    return f;
}

static int file_exists(const char *path)
{
    HANDLE f = open_file(path);
    if (f == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(f);
    return 1;
}

static void unquote(char *d, const char *s, int n)
{
    while (*s == ' ' || *s == '"') s++;
    lstrcpynA(d, s, n);
    for (int l = lstrlenA(d); l > 0 && (d[l - 1] == '"' || d[l - 1] == ' '); l--) d[l - 1] = 0;
}

/* gm.dls: gmdls= (absolute, or relative to the game's folder), else DirectMusic's own lookup (HKLM\Software\
 * Microsoft\DirectMusic GMFilePath, system32\drivers\gm.dls), else the 64-bit system32 (Sysnative) and gm.dls next
 * to the game or to reAL.  Every candidate tried is logged the first time. */
const char *dm_gm_dls_path(void)
{
    static char path[MAX_PATH];
    if (path[0]) return path;
    char cand[8][MAX_PATH], tmp[MAX_PATH];
    int n = 0;
    if (g_cfg.gm_dls[0]) {
        unquote(tmp, g_cfg.gm_dls, MAX_PATH);
        /* a Linux path under Wine (/home/...): Wine's own conversion to its DOS name (normally Z:\home\...) */
        WCHAR *(CDECL *dosname)(LPCSTR) = tmp[0] == '/' ?
            (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_dos_file_name") : NULL;
        WCHAR *w = dosname ? dosname(tmp) : NULL;
        if (w) {
            WideCharToMultiByte(CP_ACP, 0, w, -1, cand[n++], MAX_PATH, NULL, NULL);
            HeapFree(GetProcessHeap(), 0, w);
        } else if (tmp[0] && (tmp[1] == ':' || tmp[0] == '/' || tmp[0] == '\\')) lstrcpynA(cand[n++], tmp, MAX_PATH);
        else {
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            char *sl = strrchr(exe, '\\');
            if (sl) sl[1] = 0;
            snprintf(cand[n++], MAX_PATH, "%s%s", exe, tmp);
        }
    }
    HKEY k;
    char raw[MAX_PATH] = "";
    DWORD sz = sizeof(raw), type = 0;
    if (!RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\Microsoft\\DirectMusic", 0, KEY_READ, &k)) {
        if (RegQueryValueExA(k, "GMFilePath", NULL, &type, (BYTE *)raw, &sz) || (type != REG_SZ && type != REG_EXPAND_SZ)) raw[0] = 0;
        RegCloseKey(k);
    }
    if (raw[0]) { ExpandEnvironmentStringsA(raw, tmp, MAX_PATH); unquote(cand[n++], tmp, MAX_PATH); }
    char sys[MAX_PATH], win[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    GetWindowsDirectoryA(win, MAX_PATH);
    snprintf(cand[n++], MAX_PATH, "%s\\drivers\\gm.dls", sys);
    snprintf(cand[n++], MAX_PATH, "%s\\Sysnative\\drivers\\gm.dls", win);
    snprintf(cand[n++], MAX_PATH, "%s\\SysWOW64\\drivers\\gm.dls", win);
    GetModuleFileNameA(NULL, tmp, MAX_PATH);
    { char *sl = strrchr(tmp, '\\'); if (sl) { lstrcpyA(sl + 1, "gm.dls"); lstrcpynA(cand[n++], tmp, MAX_PATH); } }
    GetModuleFileNameA(g_self, tmp, MAX_PATH);
    { char *sl = strrchr(tmp, '\\'); if (sl) { lstrcpyA(sl + 1, "gm.dls"); lstrcpynA(cand[n++], tmp, MAX_PATH); } }
    for (int i = 0; i < n; i++) {
        int ok = file_exists(cand[i]);
        LOG_INFO("music: GM collection candidate %s: %s", cand[i], ok ? "found" : "not found");
        if (ok) { lstrcpynA(path, cand[i], MAX_PATH); return path; }
    }
    lstrcpynA(path, cand[0], MAX_PATH);
    LOG_INFO("music: no gm.dls found; set gmdls= in reAL.ini to a DLS file");
    return path;
}
