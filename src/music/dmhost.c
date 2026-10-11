/* reAL-Music host: see dmhost.h.
 *
 * Audio: like DirectMusic's DirectSound sink, the synth is read in bus form (dry left/right and the reverb send)
 * into 16-bit stereo buffers: the dry one, and the shared reverb one whose Waves reverb we run ourselves before
 * writing it.  Both are reAL buffers, mixed (and resampled) by reAL's device like any game buffer.  The sink keeps a
 * fixed amount written ahead of the play cursor.
 * Time: the master clock follows the buffers' play position (so it never drifts from the audio), smoothed with
 * QueryPerformanceCounter between cursor reads; the latency clock is the time of the next sample to be written. */
#include "../real.h"
#include "dmhost.h"
#include "msynth.h"
#include "wavesrev.h"
#include <stdio.h>
#include <float.h>
#include <string.h>

#define AHEAD_MS 80         /* written ahead of the play cursor */
#define BUF_MS 1000

struct dm_host {
    CRITICAL_SECTION lock;
    dm_perf *pf;
    msynth *syn;
    dls_port *dls;
    wavesrev *rev;
    uint32_t rate;
    rl_device *dev;
    rl_buffer *dry, *wet;
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

/* ------------------------------------------------------------------ the sink */
static rl_buffer *make_buffer(dm_host *h)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, h->rate, h->rate * 4, 4, 16, 0 };
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME | DSBCAPS_LOCSOFTWARE;
    d.dwBufferBytes = h->bufframes * 4;
    d.lpwfxFormat = &wf;
    rl_buffer *b = NULL;
    if (FAILED(rl_buffer_create(h->dev, NULL, &d, &b))) return NULL;
    return b;
}

static void write_buffer(rl_buffer *b, uint32_t frame, const int16_t *src, uint32_t n, uint32_t bufframes)
{
    void *p1, *p2;
    DWORD n1, n2;
    if (FAILED(IDirectSoundBuffer8_Lock(&b->IDirectSoundBuffer8_iface, frame * 4, n * 4, &p1, &n1, &p2, &n2, 0))) return;
    memcpy(p1, src, n1);
    if (p2) memcpy(p2, (const BYTE *)src + n1, n2);
    IDirectSoundBuffer8_Unlock(&b->IDirectSoundBuffer8_iface, p1, n1, p2, n2);
    (void)bufframes;
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
    static const uint32_t ids[3] = { 0, 1, 0x40 }, fn[3] = { 0, 1, 0x40 };
    static const int32_t pitch[3] = { 0, 0, 0 };
    while (h->written < target) {
        uint32_t n = (uint32_t)(target - h->written);
        if (n > 256) n = 256;
        int16_t l[256], r[256], v[512], dry[512], *bufs[3] = { l, r, v };
        memset(l, 0, sizeof(l)); memset(r, 0, sizeof(r)); memset(v, 0, sizeof(v));
        msynth_read(h->syn, bufs, ids, fn, pitch, 3, n);
        for (int i = (int)n - 1; i >= 0; i--) { v[2 * i] = v[2 * i + 1] = v[i]; dry[2 * i] = l[i]; dry[2 * i + 1] = r[i]; }
        if (h->noreverb) memset(v, 0, n * 4);
        else wavesrev_stereo16(h->rev, v, n);
        uint32_t at = (uint32_t)(h->written % h->bufframes);
        if (h->dump) {
            int16_t mix[512];
            for (uint32_t k = 0; k < 2 * n; k++) { int x = dry[k] + v[k]; mix[k] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
            fwrite(mix, 4, n, h->dump);
        }
        write_buffer(h->dry, at, dry, n, h->bufframes);
        write_buffer(h->wet, at, v, n, h->bufframes);
        h->written += n;
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
    }
    if (FAILED(rl_device_acquire(&h->dev))) { free(h); return NULL; }
    h->syn = msynth_new(h->rate, voices ? (int)voices : 64, groups ? (int)groups : 64, 1);
    msynth_set_gain(h->syn, 0);
    dls_sink sk = { sink_dl, sink_ul };
    h->dls = dls_port_new(&sk, h);
    h->rev = wavesrev_new(h->rate);
    h->dry = make_buffer(h);
    h->wet = make_buffer(h);
    if (!h->syn || !h->dls || !h->rev || !h->dry || !h->wet) { dm_host_free(h); return NULL; }
    dm_hostops ho = { host_clock, host_msec, host_signal };
    h->pf = dm_perf_new(&ho, h);
    dm_portops po = { port_latency, port_play, port_priority, port_download, port_unload };
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
        if (b && ReadFile(f, b, size, &got, NULL) && got == size) c = dls_load(b, size, gm);
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
        if (tmp[0] && (tmp[1] == ':' || (tmp[0] == '\\' && tmp[1] == '\\'))) lstrcpynA(cand[n++], tmp, MAX_PATH);
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
        LOG_INFO("music: gm.dls candidate %s: %s", cand[i], ok ? "found" : "not found");
        if (ok) { lstrcpynA(path, cand[i], MAX_PATH); return path; }
    }
    lstrcpynA(path, cand[0], MAX_PATH);
    LOG_INFO("music: no gm.dls found; set gmdls= in reAL.ini to a DLS file");
    return path;
}
