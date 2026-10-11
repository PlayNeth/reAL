/* dmgt - ground truth from Microsoft's DirectMusic, without DirectSound.
 *
 * Loads the DirectMusic DLLs from a directory (no registration needed: CoCreateInstance is redirected in their import
 * tables), downloads DLS instruments through Microsoft's own port code (dmusic.dll) into Microsoft's synth
 * (dmsynth.dll), and renders through the synth's legacy IDirectMusicSynth::Render with our own offline sink.
 *
 *   dmgt <dmdir> synth <collection.dls> <out.wav> <script>    script lines: "<ms> <status hex> <d1> <d2>"
 *                                                                (midi messages on channel group 1)
 *   options (env): DMGT_RATE (44100), DMGT_REVERB (0), DMGT_DLDUMP=file  (append the download buffers)
 *
 * Under Wine run with WINEDLLOVERRIDES="dmusic,dmsynth,dmloader,dmime,dmband,dmstyle,dmcompos,dmscript,dswave=n". */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dmusici.h>
#include <dmusics.h>
#include <dmdls.h>
#include <dsound.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

static char g_dir[MAX_PATH];
static const char *const g_dlls[] = { "dmusic.dll", "dmsynth.dll", "dmloader.dll", "dmime.dll", "dmband.dll", "dmstyle.dll",
                                      "dmcompos.dll", "dmscript.dll", "dswave.dll", "dsdmo.dll", NULL };
static HMODULE g_mods[16];
static FILE *g_dl;
static IDirectMusicSynth8 *g_synth;     /* the synth dmusic.dll created for its port */

static void die(const char *what, HRESULT hr) { fprintf(stderr, "%s failed: %08lx\n", what, (unsigned long)hr); exit(1); }

/* ------------------------------------------------------------------ download capture (vtable patch on the synth) */
typedef HRESULT (WINAPI *download_fn)(IDirectMusicSynth8 *, LPHANDLE, LPVOID, LPBOOL);
static download_fn g_orig_download;

static HRESULT WINAPI my_download(IDirectMusicSynth8 *s, LPHANDLE h, LPVOID data, LPBOOL free_)
{
    DMUS_DOWNLOADINFO *di = data;
    if (g_dl) {
        DWORD tag = 0x4c444d44; /* 'DMDL' */
        fwrite(&tag, 4, 1, g_dl);
        fwrite(&di->cbSize, 4, 1, g_dl);
        fwrite(data, 1, di->cbSize, g_dl);
    }
    HRESULT hr = g_orig_download(s, h, data, free_);
    fprintf(stderr, "  Download type %lu id %lu size %lu -> %08lx\n", di->dwDLType, di->dwDLId, di->cbSize, (unsigned long)hr);
    return hr;
}

/* DirectX 8 voices: PlayVoice / StopVoice recorded in the download dump ('DMPV' / 'DMSV' records) */
typedef HRESULT (WINAPI *playvoice_fn)(IDirectMusicSynth8 *, REFERENCE_TIME, DWORD, DWORD, DWORD, DWORD, LONG, LONG,
                                       SAMPLE_TIME, SAMPLE_TIME, SAMPLE_TIME);
typedef HRESULT (WINAPI *stopvoice_fn)(IDirectMusicSynth8 *, REFERENCE_TIME, DWORD);
static playvoice_fn g_orig_playvoice;
static stopvoice_fn g_orig_stopvoice;
static HRESULT WINAPI my_playvoice(IDirectMusicSynth8 *s, REFERENCE_TIME rt, DWORD id, DWORD grp, DWORD ch, DWORD dlid,
                                   LONG pitch, LONG vol, SAMPLE_TIME st, SAMPLE_TIME ls, SAMPLE_TIME le)
{
    if (g_dl) {
        DWORD tag = 0x56504d44, sz = 56;    /* 'DMPV' */
        fwrite(&tag, 4, 1, g_dl); fwrite(&sz, 4, 1, g_dl);
        fwrite(&rt, 8, 1, g_dl); fwrite(&id, 4, 1, g_dl); fwrite(&grp, 4, 1, g_dl); fwrite(&ch, 4, 1, g_dl);
        fwrite(&dlid, 4, 1, g_dl); fwrite(&pitch, 4, 1, g_dl); fwrite(&vol, 4, 1, g_dl);
        fwrite(&st, 8, 1, g_dl); fwrite(&ls, 8, 1, g_dl); fwrite(&le, 8, 1, g_dl);
    }
    HRESULT hr = g_orig_playvoice(s, rt, id, grp, ch, dlid, pitch, vol, st, ls, le);
    fprintf(stderr, "  PlayVoice rt %lld id %lu grp %lu ch %lu dl %lu pitch %ld vol %ld start %llu loop %llu %llu -> %08lx\n",
            rt, id, grp, ch, dlid, pitch, vol, st, ls, le, (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI my_stopvoice(IDirectMusicSynth8 *s, REFERENCE_TIME rt, DWORD id)
{
    if (g_dl) {
        DWORD tag = 0x56534d44, sz = 12;    /* 'DMSV' */
        fwrite(&tag, 4, 1, g_dl); fwrite(&sz, 4, 1, g_dl); fwrite(&rt, 8, 1, g_dl); fwrite(&id, 4, 1, g_dl);
    }
    HRESULT hr = g_orig_stopvoice(s, rt, id);
    fprintf(stderr, "  StopVoice rt %lld id %lu -> %08lx\n", rt, id, (unsigned long)hr);
    return hr;
}

static void hook_synth(IDirectMusicSynth8 *s)
{
    void **vt = *(void ***)s;
    DWORD old;
    if (g_orig_download) return;
    VirtualProtect(&vt[20], 2 * sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    g_orig_playvoice = (playvoice_fn)vt[20]; vt[20] = (void *)my_playvoice;
    g_orig_stopvoice = (stopvoice_fn)vt[21]; vt[21] = (void *)my_stopvoice;
    VirtualProtect(&vt[20], 2 * sizeof(void *), old, &old);
    VirtualProtect(&vt[6], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    g_orig_download = (download_fn)vt[6];
    vt[6] = (void *)my_download;
    VirtualProtect(&vt[6], sizeof(void *), old, &old);
}


/* ------------------------------------------------------------------ virtual time for the DirectMusic threads
 * timeGetTime, Sleep, timed waits and SetEvent are redirected in the DirectMusic DLLs.  A thread in a timed wait is
 * "parked" until the virtual clock reaches its deadline or its event is set; the main thread advances the clock
 * only when no DirectMusic thread is running, so the threads' schedule depends on virtual time alone. */
static volatile LONGLONG g_now = 100000000;     /* 10 s, in 100 ns */
static CRITICAL_SECTION g_vcs;
static volatile LONG g_running;                 /* DirectMusic threads not parked */
static int g_vtime;
typedef struct { HANDLE h, wake; LONGLONG deadline; int state; DWORD tid; } parked;   /* state 0 free 1 parked 2 woken */
static parked g_park[16];

static void patch_import(HMODULE m, const char *dll, const char *name, void *fn);
static DWORD WINAPI v_timegettime(void) { return (DWORD)(g_now / 10000); }

static parked *park_slot(void)
{
    DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < 16; i++) if (g_park[i].tid == tid) return &g_park[i];
    for (int i = 0; i < 16; i++) if (!g_park[i].tid) {
        g_park[i].tid = tid;
        g_park[i].wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        InterlockedIncrement(&g_running);          /* a new thread counts as running */
        return &g_park[i];
    }
    return NULL;
}

static DWORD v_wait(HANDLE h, DWORD ms)
{
    EnterCriticalSection(&g_vcs);
    parked *p = park_slot();
    p->h = h;
    p->deadline = g_now + (LONGLONG)ms * 10000;
    p->state = 1;
    InterlockedDecrement(&g_running);
    LeaveCriticalSection(&g_vcs);
    HANDLE hs[2] = { p->wake, h };
    DWORD r = WaitForMultipleObjects(h ? 2 : 1, hs, FALSE, INFINITE);
    EnterCriticalSection(&g_vcs);
    if (p->state == 1) InterlockedIncrement(&g_running);    /* woken by its own event, unannounced */
    p->state = 0;
    p->h = NULL;
    LeaveCriticalSection(&g_vcs);
    return r == WAIT_OBJECT_0 ? WAIT_TIMEOUT : WAIT_OBJECT_0;
}

static void WINAPI v_sleep(DWORD ms)
{
    if (!g_vtime || ms == 0) { Sleep(ms); return; }
    v_wait(NULL, ms);
}

static DWORD WINAPI v_waitsingle(HANDLE h, DWORD ms)
{
    if (!g_vtime || ms == 0 || ms == INFINITE) return WaitForSingleObject(h, ms);
    return v_wait(h, ms);
}

static BOOL WINAPI v_setevent(HANDLE h)
{
    EnterCriticalSection(&g_vcs);
    for (int i = 0; i < 16; i++)
        if (g_park[i].tid && g_park[i].state == 1 && g_park[i].h == h) {
            g_park[i].state = 2;
            InterlockedIncrement(&g_running);
        }
    LeaveCriticalSection(&g_vcs);
    return SetEvent(h);
}

/* advance virtual time by one step once every DirectMusic thread is parked; returns after the woken ones park again */
static void v_step(LONGLONG step)
{
    while (g_running > 0) Sleep(0);
    EnterCriticalSection(&g_vcs);
    g_now += step;
    for (int i = 0; i < 16; i++)
        if (g_park[i].tid && g_park[i].state == 1 && g_park[i].deadline <= g_now) {
            g_park[i].state = 2;
            InterlockedIncrement(&g_running);
            SetEvent(g_park[i].wake);
        }
    LeaveCriticalSection(&g_vcs);
    while (g_running > 0) Sleep(0);
}

static void v_hook(HMODULE m)
{
    patch_import(m, "winmm.dll", "timeGetTime", v_timegettime);
    patch_import(m, "kernel32.dll", "Sleep", v_sleep);
    patch_import(m, "kernel32.dll", "WaitForSingleObject", v_waitsingle);
    patch_import(m, "kernel32.dll", "SetEvent", v_setevent);
}

/* ------------------------------------------------------------------ class objects straight from the DLLs */
static HRESULT get_class(REFCLSID clsid, REFIID iid, void **ppv)
{
    for (int i = 0; g_mods[i]; i++) {
        HRESULT (WINAPI *get)(REFCLSID, REFIID, void **) = (void *)GetProcAddress(g_mods[i], "DllGetClassObject");
        if (get && SUCCEEDED(get(clsid, iid, ppv))) return S_OK;
    }
    return REGDB_E_CLASSNOTREG;
}

static HRESULT WINAPI my_cocreate(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID *ppv)
{
    IClassFactory *cf;
    HRESULT hr = get_class(clsid, &IID_IClassFactory, (void **)&cf);
    if (FAILED(hr)) return CoCreateInstance(clsid, outer, ctx, iid, ppv);
    hr = IClassFactory_CreateInstance(cf, outer, iid, ppv);
    IClassFactory_Release(cf);
    if (SUCCEEDED(hr) && IsEqualGUID(clsid, &CLSID_DirectMusicSynth)) {
        IUnknown *u = *ppv;
        if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicSynth8, (void **)&g_synth))) {
            hook_synth(g_synth);
            IDirectMusicSynth8_Release(g_synth);    /* the port holds it */
        }
    }
    return hr;
}

static void patch_import(HMODULE m, const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return;
    void *target = (void *)GetProcAddress(GetModuleHandleA(dll), name);
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd->VirtualAddress); d->Name; d++) {
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++) {
            if (*slot != target) continue;
            DWORD old;
            VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
            *slot = fn;
            VirtualProtect(slot, sizeof(void *), old, &old);
        }
    }
}

static void patch_iat(HMODULE m)
{
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress) return;
    void *target = (void *)GetProcAddress(GetModuleHandleA("ole32.dll"), "CoCreateInstance");
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd->VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((char *)base + d->Name, "ole32.dll")) continue;
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++) {
            if (*slot != target) continue;
            DWORD old;
            VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
            *slot = (void *)my_cocreate;
            VirtualProtect(slot, sizeof(void *), old, &old);
        }
    }
}

/* replace an import by name (works for functions Wine only has stubs for) */
static void patch_import_name(HMODULE m, const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd->VirtualAddress); dd->VirtualAddress && d->Name; d++) {
        if (lstrcmpiA((char *)base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA *on = (void *)(base + d->OriginalFirstThunk);
        void **slot = (void **)(base + d->FirstThunk);
        for (; on->u1.AddressOfData; on++, slot++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (void *)(base + on->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, name)) continue;
            DWORD old;
            VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
            *slot = fn;
            VirtualProtect(slot, sizeof(void *), old, &old);
        }
    }
}

/* dsdmo.dll imports msvcrt's SSE2 libm entry points (double arguments in xmm0/xmm1, result in xmm0), which Wine's
 * msvcrt lacks: thunks to pow/sqrt */
__attribute__((used)) double __cdecl call_pow(double a, double b) { return pow(a, b); }
__attribute__((used)) double __cdecl call_sqrt(double a) { return sqrt(a); }
__attribute__((naked)) static void libm_pow_thunk(void)
{
    __asm__("subl $20, %esp\n movsd %xmm0, (%esp)\n movsd %xmm1, 8(%esp)\n call _call_pow\n fstpl (%esp)\n"
            " movsd (%esp), %xmm0\n addl $20, %esp\n ret");
}
__attribute__((naked)) static void libm_sqrt_thunk(void)
{
    __asm__("subl $12, %esp\n movsd %xmm0, (%esp)\n call _call_sqrt\n fstpl (%esp)\n"
            " movsd (%esp), %xmm0\n addl $12, %esp\n ret");
}

static void load_dm(const char *dir)
{
    strcpy(g_dir, dir);
    InitializeCriticalSection(&g_vcs);
    g_vtime = getenv("DMGT_VTIME") != NULL;
    int n = 0;
    LoadLibraryA("ole32.dll");
    for (int i = 0; g_dlls[i]; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\%s", dir, g_dlls[i]);
        HMODULE m = LoadLibraryA(p);
        if (!m) { fprintf(stderr, "cannot load %s (%lu)\n", p, GetLastError()); continue; }
        patch_iat(m);
        if (!strcmp(g_dlls[i], "dsdmo.dll")) {
            patch_import_name(m, "msvcrt.dll", "_libm_sse2_pow_precise", (void *)libm_pow_thunk);
            patch_import_name(m, "msvcrt.dll", "_libm_sse2_sqrt_precise", (void *)libm_sqrt_thunk);
        }
        if (getenv("DMGT_VTIME")) { LoadLibraryA("winmm.dll"); v_hook(m); }
        g_mods[n++] = m;
    }
}

/* ------------------------------------------------------------------ offline sink */
typedef struct { IDirectMusicSynthSink iface; LONG ref; IDirectMusicSynth *synth; DWORD rate; } sink;
static sink *impl(IDirectMusicSynthSink *i) { return (sink *)i; }

static HRESULT WINAPI sk_qi(IDirectMusicSynthSink *i, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirectMusicSynthSink)) { *ppv = i; impl(i)->ref++; return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI sk_addref(IDirectMusicSynthSink *i) { return ++impl(i)->ref; }
static ULONG WINAPI sk_release(IDirectMusicSynthSink *i) { return --impl(i)->ref; }
static HRESULT WINAPI sk_init(IDirectMusicSynthSink *i, IDirectMusicSynth *s) { impl(i)->synth = s; return S_OK; }
static HRESULT WINAPI sk_setclock(IDirectMusicSynthSink *i, IReferenceClock *c) { return S_OK; }
static HRESULT WINAPI sk_latclock(IDirectMusicSynthSink *i, IReferenceClock **c) { *c = NULL; return E_NOTIMPL; }
static HRESULT WINAPI sk_activate(IDirectMusicSynthSink *i, BOOL on) { return S_OK; }
static HRESULT WINAPI sk_s2r(IDirectMusicSynthSink *i, LONGLONG s, REFERENCE_TIME *r)
{
    *r = s * 10000000LL / impl(i)->rate;
    return S_OK;
}
static HRESULT WINAPI sk_r2s(IDirectMusicSynthSink *i, REFERENCE_TIME r, LONGLONG *s)
{
    *s = r * impl(i)->rate / 10000000LL;
    return S_OK;
}
static HRESULT WINAPI sk_setds(IDirectMusicSynthSink *i, LPDIRECTSOUND ds, LPDIRECTSOUNDBUFFER b) { return S_OK; }
static HRESULT WINAPI sk_bufsize(IDirectMusicSynthSink *i, LPDWORD n) { *n = impl(i)->rate / 2; return S_OK; }

static IDirectMusicSynthSinkVtbl sink_vtbl = { sk_qi, sk_addref, sk_release, sk_init, sk_setclock, sk_latclock, sk_activate,
                                               sk_s2r, sk_r2s, sk_setds, sk_bufsize };

/* ------------------------------------------------------------------ DirectX 8 sink (dsound's private IDirectSoundSynthSink, as far as dmsynth uses it) */
static const GUID IID_DSSynthSink = { 0x73a6a85a, 0x493e, 0x4c87, { 0xb4, 0xa5, 0xbe, 0x53, 0xeb, 0x92, 0x74, 0x4b } };
typedef struct { void **vtbl; DWORD rate; } dssink;
static HRESULT WINAPI ds_qi(dssink *t, REFIID riid, void **ppv)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_DSSynthSink)) { *ppv = t; return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI ds_addref(dssink *t) { return 2; }
static ULONG WINAPI ds_release(dssink *t) { return 1; }
static HRESULT WINAPI ds_activate(dssink *t, BOOL on) { return S_OK; }
static HRESULT WINAPI ds_r2s(dssink *t, REFERENCE_TIME rt, LONGLONG *st)
{
    *st = rt * t->rate / 10000000LL;
    if (getenv("DMGT_R2S_LOG")) fprintf(stderr, "RefToSample(%lld) -> %lld\n", rt, *st);
    return S_OK;
}
static HRESULT WINAPI ds_fmt(dssink *t, WAVEFORMATEX *w, DWORD size, DWORD *written)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 1, t->rate, t->rate * 2, 2, 16, 0 };
    memcpy(w, &f, size < sizeof(f) ? size : sizeof(f));
    if (written) *written = sizeof(f);
    return S_OK;
}
#define DSSTUB(n) static HRESULT WINAPI ds_stub##n(void *t) { fprintf(stderr, "sink method %d called\n", n); return E_NOTIMPL; }
DSSTUB(3) DSSTUB(5) DSSTUB(8) DSSTUB(9) DSSTUB(10) DSSTUB(11) DSSTUB(12) DSSTUB(13) DSSTUB(14) DSSTUB(15)
static void *dssink_vtbl[] = { ds_qi, ds_addref, ds_release, ds_stub3, ds_activate, ds_stub5, ds_r2s, ds_fmt, ds_stub8,
                               ds_stub9, ds_stub10, ds_stub11, ds_stub12, ds_stub13, ds_stub14, ds_stub15 };

/* ------------------------------------------------------------------ wav */
static void write_wav_n(const char *path, const short *pcm, DWORD frames, DWORD rate, WORD ch)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("open output", E_FAIL);
    DWORD data = frames * 2 * ch, riff = 36 + data, fmtsz = 16, byterate = rate * 2 * ch;
    WORD tag = 1, align = 2 * ch, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtsz, 4, 1, f);
    fwrite(&tag, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byterate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(pcm, 2 * ch, frames, f);
    fclose(f);
}
static void write_wav(const char *path, const short *pcm, DWORD frames, DWORD rate) { write_wav_n(path, pcm, frames, rate, 2); }

/* ------------------------------------------------------------------ synth command */
typedef struct { DWORD ms; BYTE msg[3]; } ev;

static int cmd_synth(const char *dls, const char *out, const char *script)
{
    DWORD rate = getenv("DMGT_RATE") ? atoi(getenv("DMGT_RATE")) : 44100;
    IDirectMusicLoader8 *loader;
    IDirectMusic8 *dm;
    HRESULT hr;
    if (FAILED(hr = my_cocreate(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader))) die("loader", hr);
    if (FAILED(hr = my_cocreate(&CLSID_DirectMusic, NULL, CLSCTX_INPROC, &IID_IDirectMusic8, (void **)&dm))) die("DirectMusic", hr);

    IDirectSound8 *ds;
    if (FAILED(hr = DirectSoundCreate8(NULL, &ds, NULL))) die("DirectSoundCreate8", hr);
    IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    if (FAILED(hr = IDirectMusic8_SetDirectSound(dm, (IDirectSound *)ds, NULL))) die("SetDirectSound", hr);
    DMUS_PORTPARAMS8 pp = { sizeof(pp) };
    pp.dwValidParams = DMUS_PORTPARAMS_VOICES | DMUS_PORTPARAMS_CHANNELGROUPS | DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_EFFECTS;
    pp.dwVoices = getenv("DMGT_VOICES") ? atoi(getenv("DMGT_VOICES")) : 64;
    pp.dwChannelGroups = 1;
    pp.dwSampleRate = rate;
    pp.dwEffectFlags = getenv("DMGT_REVERB") && atoi(getenv("DMGT_REVERB")) ? DMUS_EFFECT_REVERB : DMUS_EFFECT_NONE;
    if (getenv("DMGT_BUSES")) {     /* a DirectX 8 port (audiopath feature): wave voices need it */
        pp.dwValidParams |= DMUS_PORTPARAMS_FEATURES;
        pp.dwFeatures = DMUS_PORT_FEATURE_AUDIOPATH | DMUS_PORT_FEATURE_STREAMING;
    }
    IDirectMusicPort *port;
    if (FAILED(hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &port, NULL))) die("CreatePort", hr);
    if (!g_synth) die("synth not seen", E_FAIL);
    fprintf(stderr, "port: %lu Hz, %lu voices, effects %lx\n", pp.dwSampleRate, pp.dwVoices, pp.dwEffectFlags);

    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dls, -1, w, MAX_PATH);
    IDirectMusicCollection8 *col;
    if (FAILED(hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicCollection, &IID_IDirectMusicCollection8, w, (void **)&col)))
        die("load collection", hr);
    DWORD patch;
    WCHAR name[64];
    for (DWORD i = 0; IDirectMusicCollection_EnumInstrument(col, i, &patch, name, 64) == S_OK; i++) {
        IDirectMusicInstrument *ins;
        IDirectMusicDownloadedInstrument *dl;
        if (FAILED(IDirectMusicCollection_GetInstrument(col, patch, &ins))) continue;
        hr = IDirectMusicPort_DownloadInstrument(port, ins, &dl, NULL, 0);
        fprintf(stderr, "instrument %06lx %ls -> %08lx\n", patch, name, (unsigned long)hr);
    }

    if (getenv("DMGT_DATADUMP")) {    /* dmsynth's .data after initialisation (runtime-built tables) */
        HMODULE m = GetModuleHandleA("dmsynth.dll");
        BYTE *b = (BYTE *)m;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
        FILE *df = fopen(getenv("DMGT_DATADUMP"), "wb");
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (!memcmp(sh[i].Name, ".data", 5)) {
                DWORD rva = sh[i].VirtualAddress, sz = sh[i].Misc.VirtualSize;
                fwrite(&rva, 4, 1, df); fwrite(&sz, 4, 1, df); fwrite(b + rva, 1, sz, df);
            }
        fclose(df);
    }
    sink s = { { &sink_vtbl }, 1, NULL, rate };
    dssink dsk = { dssink_vtbl, rate };
    void **src = (void **)((BYTE *)g_synth + 4);     /* the synth's IDirectSoundSource */
    void **svt = *(void ***)src;
    int dx8 = getenv("DMGT_BUSES") != NULL;
    DWORD busid[32], busfn[32], nbus = 0;
    LONG buspitch[32] = { 0 };
    if (dx8) {
        for (char *p = getenv("DMGT_BUSES"); *p && nbus < 32;) {
            busid[nbus] = strtoul(p, &p, 0);
            busfn[nbus] = *p == ':' ? strtoul(p + 1, &p, 0) : busid[nbus];
            nbus++;
            if (*p == ',') p++; else break;
        }
        /* the port gave the synth its own (legacy) sink; a DirectX 8 synth has only the DirectSound one */
        IDirectMusicSynth8_SetSynthSink(g_synth, NULL);
        hr = ((HRESULT (WINAPI *)(void *, void *))svt[3])(src, &dsk);
        if (FAILED(hr)) die("SetSink (DX8)", hr);
        if (FAILED(hr = IDirectMusicSynth8_Activate(g_synth, TRUE))) die("Activate", hr);
        if (getenv("DMGT_ASSIGN") && *getenv("DMGT_ASSIGN")) {     /* "channel:id,id,...;channel:..." for group 1 */
            for (char *p = getenv("DMGT_ASSIGN"); *p;) {
                DWORD ch = strtoul(p, &p, 0), ids[32], n = 0;
                if (*p == ':') p++;
                while (n < 32) { ids[n++] = strtoul(p, &p, 0); if (*p == ',') p++; else break; }
                hr = IDirectMusicSynth8_AssignChannelToBuses(g_synth, 1, ch, ids, n);
                fprintf(stderr, "AssignChannelToBuses(%lu, %lu ids) -> %08lx\n", ch, n, (unsigned long)hr);
                if (*p == ';') p++; else break;
            }
        }
        ((HRESULT (WINAPI *)(void *, ULONGLONG))svt[5])(src, 0);
    } else {
        if (FAILED(hr = IDirectMusicSynth8_SetSynthSink(g_synth, &s.iface))) die("SetSynthSink", hr);
        if (FAILED(hr = IDirectMusicSynth8_Activate(g_synth, TRUE))) die("Activate", hr);
    }

    /* script */
    FILE *f = fopen(script, "r");
    if (!f) die("open script", E_FAIL);
    ev evs[4096];
    int nev = 0;
    DWORD end_ms = 0;
    char line[256];
    static const GUID IID_PortP = { 0x7048bcd8, 0x43fd, 0x4ca5, { 0x93, 0x11, 0xf3, 0x24, 0x8f, 0x0a, 0x25, 0x22 } };
    static const GUID CLSID_DSWave = { 0x8a667154, 0xf9cb, 0x11d2, { 0xad, 0x8a, 0x00, 0x60, 0xb0, 0x57, 0x5a, 0xbc } };
    void **portp = NULL;
    while (fgets(line, sizeof(line), f) && nev < 4096) {
        unsigned ms, st, d1 = 0, d2 = 0;
        char wfile[200];
        unsigned ch, sofs, dur; int pitch, vol; unsigned long ls, le;
        /* "<ms> W <file.wav> <channel> <pitch> <volume> <start offset ms> <loop start> <loop end> <duration ms>": a wave
         * through the port (DownloadWave, AllocVoice, the voice's Play and Stop) */
        if (sscanf(line, "%u W %199s %u %d %d %u %lu %lu %u", &ms, wfile, &ch, &pitch, &vol, &sofs, &ls, &le, &dur) == 9) {
            if (!portp && FAILED(hr = IDirectMusicPort_QueryInterface(port, &IID_PortP, (void **)&portp))) die("private port", hr);
            WCHAR ww[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, wfile, -1, ww, MAX_PATH);
            IUnknown *wave = NULL, *dlw = NULL, *voice = NULL;
            if (FAILED(hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DSWave, &IID_IUnknown, ww, (void **)&wave))) die("load wave", hr);
            hr = ((HRESULT (WINAPI *)(void *, IUnknown *, IUnknown **, REFERENCE_TIME))(*(void ***)portp)[3])(portp, wave, &dlw, (REFERENCE_TIME)sofs * 10000);
            if (FAILED(hr)) die("DownloadWave", hr);
            hr = ((HRESULT (WINAPI *)(void *, IUnknown *, DWORD, DWORD, REFERENCE_TIME, LONGLONG, LONGLONG, IUnknown **))(*(void ***)portp)[5])(
                portp, dlw, ch, 1, (REFERENCE_TIME)sofs * 10000, (LONGLONG)ls, (LONGLONG)le, &voice);
            if (FAILED(hr)) die("AllocVoice", hr);
            hr = ((HRESULT (WINAPI *)(void *, REFERENCE_TIME, LONG, LONG))(*(void ***)voice)[3])(voice, (REFERENCE_TIME)ms * 10000, pitch, vol);
            fprintf(stderr, "wave %s: Play -> %08lx\n", wfile, (unsigned long)hr);
            if (dur) ((HRESULT (WINAPI *)(void *, REFERENCE_TIME))(*(void ***)voice)[4])(voice, (REFERENCE_TIME)(ms + dur) * 10000);
            if (ms + dur > end_ms) end_ms = ms + dur;
            continue;
        }
        if (line[0] == '#' || sscanf(line, "%u %x %u %u", &ms, &st, &d1, &d2) < 2) continue;
        if (st == 0) { end_ms = ms; continue; }     /* "<ms> 0": end of render */
        evs[nev].ms = ms; evs[nev].msg[0] = st; evs[nev].msg[1] = d1; evs[nev].msg[2] = d2; nev++;
        if (ms > end_ms) end_ms = ms;
    }
    fclose(f);
    /* one PlayBuffer per event: the synth schedules by its reference time, which our sink maps to samples exactly */
    for (int i = 0; i < nev; i++) {
        struct { DMUS_EVENTHEADER h; BYTE d[8]; } b = { { 4 /* cbEvent */, 1 /* channel group */, 0, DMUS_EVENT_STRUCTURED } };
        b.h.cbEvent = (evs[i].msg[0] & 0xf0) == 0xc0 || (evs[i].msg[0] & 0xf0) == 0xd0 ? 2 : 3;
        memcpy(b.d, evs[i].msg, 3);
        REFERENCE_TIME rt = (REFERENCE_TIME)evs[i].ms * 10000;
        if (FAILED(hr = IDirectMusicSynth8_PlayBuffer(g_synth, rt, (BYTE *)&b, DMUS_EVENT_SIZE(b.h.cbEvent)))) die("PlayBuffer", hr);
    }

    DWORD frames = (DWORD)((ULONGLONG)end_ms * rate / 1000), block = 256;
    if (dx8) {
        short *pcm = calloc(frames + block, 2 * nbus), *bufs[32];
        for (DWORD i = 0; i < nbus; i++) bufs[i] = calloc(block * 4, 2);
        for (DWORD pos = 0; pos < frames; pos += block) {
            ULONGLONG bytes = block * 2;
            for (DWORD i = 0; i < nbus; i++) { memset(bufs[i], 0, block * 2); for (DWORD k = block; k < block * 4; k++) bufs[i][k] = 0x5555; }
            /* Read(buffers, bus ids, functions, pitch offsets, (unused), count, &length in bytes) */
            hr = ((HRESULT (WINAPI *)(void *, short **, DWORD *, DWORD *, LONG *, DWORD, DWORD, ULONGLONG *))svt[6])(
                src, bufs, busid, busfn, buspitch, nbus, nbus, &bytes);
            if (FAILED(hr)) die("Read", hr);
            if (pos == 0) {
                for (DWORD k = block; k < block * 4; k++) if (bufs[0][k] != 0x5555) { fprintf(stderr, "Read wrote past %lu samples (to %lu at least)\n", block, k + 1); }
                fprintf(stderr, "first Read ok\n");
            }
            if (pos < 256 * 4) {
                ULONGLONG sp = *(ULONGLONG *)((BYTE *)src + 0xc);
                fprintf(stderr, "after Read at %lu: source position %llu\n", pos, sp);
            }
            for (DWORD k = 0; k < block; k++)
                for (DWORD i = 0; i < nbus; i++) pcm[(pos + k) * nbus + i] = bufs[i][k];
        }
        write_wav_n(out, pcm, frames, rate, (WORD)nbus);
        fprintf(stderr, "wrote %s: %lu frames, %lu buses\n", out, frames, nbus);
        return 0;
    }
    short *pcm = calloc(frames + block, 4);
    for (DWORD pos = 0; pos < frames; pos += block)
        IDirectMusicSynth8_Render(g_synth, pcm + pos * 2, block, pos);
    write_wav(out, pcm, frames, rate);
    fprintf(stderr, "wrote %s: %lu frames\n", out, frames);
    IDirectMusicSynth8_Activate(g_synth, FALSE);
    return 0;
}


/* ------------------------------------------------------------------ perf: the event stream of Microsoft's performance
 * dmgt <dmdir> perf <segment> <out.log> <ms> [flags]
 * Plays a segment through dmime's performance with the master clock under our control (time advances in 1 ms steps)
 * and logs every buffer the performance hands the port: "<rt - segment start rt> <group> <bytes...>".
 * Instrument downloads go to the real port and synth (DMGT_DLDUMP captures them). */
static const GUID GUID_AllZeros = { 0 };
static IReferenceClock *g_clock;
static FILE *g_ev;
static REFERENCE_TIME g_base;
static CRITICAL_SECTION g_evcs;

static void patch_vt(void *obj, int idx, void *fn, void **orig)
{
    void **vt = *(void ***)obj;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    if (orig) *orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
}

static HRESULT WINAPI my_gettime(IReferenceClock *c, REFERENCE_TIME *t) { *t = g_now; return S_OK; }
static HRESULT WINAPI my_latclock(IDirectMusicPort *p, IReferenceClock **c)
{
    IReferenceClock_AddRef(g_clock);
    *c = g_clock;
    return S_OK;
}
static HRESULT WINAPI my_playbuffer(IDirectMusicPort *p, IDirectMusicBuffer *b)
{
    REFERENCE_TIME rt;
    DWORD grp, cb;
    BYTE *data;
    EnterCriticalSection(&g_evcs);
    if (!g_ev) { LeaveCriticalSection(&g_evcs); return S_OK; }
    IDirectMusicBuffer_ResetReadPtr(b);
    while (IDirectMusicBuffer_GetNextEvent(b, &rt, &grp, &cb, &data) == S_OK) {
        fprintf(g_ev, "%lld %lu", rt, grp);
        for (DWORD i = 0; i < cb; i++) fprintf(g_ev, " %02x", data[i]);
        fprintf(g_ev, "\n");
    }
    LeaveCriticalSection(&g_evcs);
    return S_OK;
}
static HRESULT WINAPI my_activate(IDirectMusicPort *p, BOOL on) { return S_OK; }

/* wave playback through the port (IDirectMusicPort8 DownloadWave / AllocVoice, the private voice's Play / Stop),
 * logged as "W ..." lines; objects get small ids in order of appearance */
static void *g_wobj[256]; static int g_nwobj;
static int wid(void *o) { for (int i = 0; i < g_nwobj; i++) if (g_wobj[i] == o) return i; if (g_nwobj < 256) g_wobj[g_nwobj] = o; return g_nwobj++; }
typedef HRESULT (WINAPI *dlwave_fn)(void *, IUnknown *, IUnknown **, REFERENCE_TIME);
typedef HRESULT (WINAPI *unwave_fn)(void *, IUnknown *);
typedef HRESULT (WINAPI *alloc_fn)(void *, IUnknown *, DWORD, DWORD, REFERENCE_TIME, LONGLONG, LONGLONG, IUnknown **);
typedef HRESULT (WINAPI *vplay_fn)(void *, REFERENCE_TIME, LONG, LONG);
typedef HRESULT (WINAPI *vstop_fn)(void *, REFERENCE_TIME);
static dlwave_fn o_dlwave; static unwave_fn o_unwave; static alloc_fn o_alloc; static vplay_fn o_vplay; static vstop_fn o_vstop;
static void wlog(const char *f, ...)
{
    va_list a; va_start(a, f);
    EnterCriticalSection(&g_evcs);
    if (g_ev) { fprintf(g_ev, "W "); vfprintf(g_ev, f, a); fprintf(g_ev, "\n"); }
    LeaveCriticalSection(&g_evcs);
    va_end(a);
}
static HRESULT WINAPI my_vplay(void *v, REFERENCE_TIME rt, LONG pitch, LONG vol)
{
    HRESULT hr = o_vplay(v, rt, pitch, vol);
    wlog("play v%d rt %lld pitch %ld vol %ld -> %08lx", wid(v), rt, pitch, vol, hr);
    return hr;
}
static HRESULT WINAPI my_vstop(void *v, REFERENCE_TIME rt)
{
    HRESULT hr = o_vstop(v, rt);
    wlog("stop v%d rt %lld -> %08lx", wid(v), rt, hr);
    return hr;
}
static HRESULT WINAPI my_dlwave(void *p, IUnknown *w, IUnknown **dl, REFERENCE_TIME hint)
{
    HRESULT hr = o_dlwave(p, w, dl, hint);
    wlog("download w%d -> d%d hint %lld -> %08lx", wid(w), dl && *dl ? wid(*dl) : -1, hint, hr);
    return hr;
}
static HRESULT WINAPI my_unwave(void *p, IUnknown *dl)
{
    wlog("unload d%d", wid(dl));
    return o_unwave(p, dl);
}
static HRESULT WINAPI my_alloc(void *p, IUnknown *dl, DWORD ch, DWORD grp, REFERENCE_TIME rt, LONGLONG ls, LONGLONG le, IUnknown **v)
{
    HRESULT hr = o_alloc(p, dl, ch, grp, rt, ls, le, v);
    if (SUCCEEDED(hr) && v && *v && !o_vplay) {
        patch_vt(*v, 3, my_vplay, (void **)&o_vplay);
        patch_vt(*v, 4, my_vstop, (void **)&o_vstop);
    }
    wlog("alloc d%d ch %lu grp %lu rt %lld loop %lld %lld -> v%d %08lx", wid(dl), ch, grp, rt, ls, le, v && *v ? wid(*v) : -1, hr);
    return hr;
}
static void hook_wave_port(void *port)
{
    if (!getenv("DMGT_WAVE")) return;
    /* dmime uses the port's private interface (IID 7048bcd8-...): DownloadWave, UnloadWave, AllocVoice at 3..5 */
    static const GUID IID_PortP = { 0x7048bcd8, 0x43fd, 0x4ca5, { 0x93, 0x11, 0xf3, 0x24, 0x8f, 0x0a, 0x25, 0x22 } };
    IUnknown *pp = NULL;
    if (FAILED(IUnknown_QueryInterface((IUnknown *)port, &IID_PortP, (void **)&pp)) || !pp) { printf("no private port\n"); return; }
    patch_vt(pp, 3, my_dlwave, (void **)&o_dlwave);
    patch_vt(pp, 4, my_unwave, (void **)&o_unwave);
    patch_vt(pp, 5, my_alloc, (void **)&o_alloc);
    IUnknown_Release(pp);
}

/* DMGT_TMAP: print dmime's tempo map (perf+0x12c, history +0x134: internal pmsgs, hdr+0x20 rt, +0x28 mt, +0x50 tempo) */
static void dump_tmap(void *perf, const char *when)
{
    if (!getenv("DMGT_TMAP")) return;
    fprintf(stderr, "%s prepared %ld target %ld nextscan %ld\n", when, *(LONG *)((BYTE *)perf + 0x328), *(LONG *)((BYTE *)perf + 0x32c), *(LONG *)((BYTE *)perf + 0x330));
    for (int k = 0; k < 10; k++)
        for (BYTE *s = *(BYTE **)((BYTE *)perf + 0x3c + 8 * k); s; s = *(BYTE **)s)
            fprintf(stderr, "%s list%d ss %p mtstart %ld mtlast %ld length %ld\n", when, k, s - 8, *(LONG *)(s - 8 + 0x58), *(LONG *)(s - 8 + 0x64), *(LONG *)(s - 8 + 0x48));
    for (int q = 0; q < 2; q++) {
        BYTE *h = *(BYTE **)((BYTE *)perf + (q ? 0x134 : 0x12c));
        for (; h; h = *(BYTE **)h)
            fprintf(stderr, "%s %s mt %ld rt %lld tempo %f\n", when, q ? "hist" : "map", *(LONG *)(h + 0x28),
                    *(LONGLONG *)(h + 0x20), *(double *)(h + 0x50));
    }
}

/* DMGT_NOTIFY=<file>: request notifications (DMGT_NTYPES letters s/p/m, default all) and log every one delivered:
 * "<rt> <mt> <type> <option> <field1> <field2> <state> <vtrack>" (as test_perf TP_NOTIFY) */
static FILE *g_nf;
static IUnknown *g_nss1, *g_nss2;
static void drain_notifications(IDirectMusicPerformance8 *perf)
{
    DMUS_NOTIFICATION_PMSG *m;
    while (g_nf && IDirectMusicPerformance8_GetNotificationPMsg(perf, &m) == S_OK) {
        const char *ty = IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_SEGMENT) ? "SEGMENT" :
                         IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_MEASUREANDBEAT) ? "MEASUREANDBEAT" :
                         IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_PERFORMANCE) ? "PERFORMANCE" : "OTHER";
        const char *us = !m->punkUser ? "-" : m->punkUser == g_nss1 ? "s1" : m->punkUser == g_nss2 ? "s2" : "s?";
        fprintf(g_nf, "%lld %ld %s %lu %lu %lu %s %lu\n", m->rtTime, m->mtTime, ty, m->dwNotificationOption, m->dwField1,
                m->dwField2, us, m->dwVirtualTrackID);
        IDirectMusicPerformance8_FreePMsg(perf, (DMUS_PMSG *)m);
    }
}
static IUnknown *unk_of(void *o)
{
    IUnknown *u = NULL;
    if (o && SUCCEEDED(IUnknown_QueryInterface((IUnknown *)o, &IID_IUnknown, (void **)&u))) IUnknown_Release(u);
    return u;
}

static int cmd_perf(const char *seg, const char *out, DWORD ms, DWORD flags)
{
    IDirectMusicLoader8 *loader;
    IDirectMusic8 *dm;
    IDirectMusicPerformance8 *perf;
    HRESULT hr;
    InitializeCriticalSection(&g_evcs);
    if (FAILED(hr = my_cocreate(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader))) die("loader", hr);
    if (FAILED(hr = my_cocreate(&CLSID_DirectMusic, NULL, CLSCTX_INPROC, &IID_IDirectMusic8, (void **)&dm))) die("DirectMusic", hr);
    IDirectSound8 *ds;
    if (FAILED(hr = DirectSoundCreate8(NULL, &ds, NULL))) die("DirectSoundCreate8", hr);
    IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    if (FAILED(hr = IDirectMusic8_SetDirectSound(dm, (IDirectSound *)ds, NULL))) die("SetDirectSound", hr);
    if (FAILED(hr = IDirectMusic8_GetMasterClock(dm, NULL, &g_clock))) die("GetMasterClock", hr);
    patch_vt(g_clock, 3, my_gettime, NULL);
    DMUS_PORTPARAMS8 pp = { sizeof(pp) };
    pp.dwValidParams = DMUS_PORTPARAMS_VOICES | DMUS_PORTPARAMS_CHANNELGROUPS | DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_EFFECTS;
    pp.dwVoices = 64; pp.dwChannelGroups = 1; pp.dwSampleRate = 44100; pp.dwEffectFlags = 0;
    IDirectMusicPort *port;
    /* DMGT_APATH=1: set up like Midnight Club II: InitAudio(shared stereo+reverb, 128), four standard audiopaths of
     * 64 PChannels, the segment played on the first; DMGT_VOL="<ms>:<vol>,...": AudioPath::SetVolume calls */
    IDirectMusicAudioPath *apath = NULL;
    long t_init = (long)time(NULL);     /* dmime's transport thread seeds rand() with time(NULL) when it starts */
    if (getenv("DMGT_APATH")) {
        if (FAILED(hr = my_cocreate(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf))) die("performance", hr);
        IDirectMusic *dmi = (IDirectMusic *)dm;
        IDirectSound *dsi = (IDirectSound *)ds;
        if (FAILED(hr = IDirectMusicPerformance8_InitAudio(perf, &dmi, &dsi, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 128, DMUS_AUDIOF_ALL, NULL)))
            die("InitAudio", hr);
        IDirectMusicAudioPath *aps[4];
        for (int i = 0; i < 4; i++)
            if (FAILED(hr = IDirectMusicPerformance8_CreateStandardAudioPath(perf, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, TRUE, &aps[i]))) die("CreateStandardAudioPath", hr);
        apath = aps[0];
        if (FAILED(hr = IDirectMusicAudioPath_GetObjectInPath(apath, DMUS_PCHANNEL_ALL, DMUS_PATH_PORT, 0, &GUID_All_Objects, 0, &IID_IDirectMusicPort, (void **)&port)))
            die("GetObjectInPath(port)", hr);
        patch_vt(port, 3, my_playbuffer, NULL);
        patch_vt(port, 8, my_latclock, NULL);
        patch_vt(port, 15, my_activate, NULL);
        hook_wave_port(port);
    } else {
    if (FAILED(hr = IDirectMusic8_CreatePort(dm, &CLSID_DirectMusicSynth, (DMUS_PORTPARAMS *)&pp, &port, NULL))) die("CreatePort", hr);
    patch_vt(port, 3, my_playbuffer, NULL);
    patch_vt(port, 8, my_latclock, NULL);
    patch_vt(port, 15, my_activate, NULL);
    if (FAILED(hr = my_cocreate(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf))) die("performance", hr);
    IDirectMusic *dmi = (IDirectMusic *)dm;
    if (FAILED(hr = IDirectMusicPerformance8_Init(perf, &dmi, NULL, NULL))) die("Init", hr);
    if (FAILED(hr = IDirectMusicPerformance8_AddPort(perf, port))) die("AddPort", hr);
    if (FAILED(hr = IDirectMusicPerformance8_AssignPChannelBlock(perf, 0, port, 1))) die("AssignPChannelBlock", hr);
    }
    char dir[MAX_PATH];
    strcpy(dir, seg);
    char *sl = strrchr(dir, '\\');
    if (sl) *sl = 0;
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dir, -1, w, MAX_PATH);
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, w, FALSE);
    MultiByteToWideChar(CP_ACP, 0, seg, -1, w, MAX_PATH);
    IDirectMusicSegment8 *sg;
    if (FAILED(hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&sg)))
        die("load segment", hr);
    if (FAILED(hr = IDirectMusicSegment8_Download(sg, (IUnknown *)perf))) fprintf(stderr, "Download: %08lx\n", (unsigned long)hr);
    if (getenv("DMGT_VTBL")) {
        fprintf(stderr, "vtbl performance %p segment %p\n", *(void **)perf, *(void **)sg);
        IDirectMusicTrack *tr;
        for (DWORD i = 0; IDirectMusicSegment8_GetTrack(sg, &GUID_AllZeros, 0xffffffff, i, &tr) == S_OK; i++) {
            IPersistStream *ps;
            CLSID c = { 0 };
            if (SUCCEEDED(IDirectMusicTrack_QueryInterface(tr, &IID_IPersistStream, (void **)&ps))) {
                IPersistStream_GetClassID(ps, &c); IPersistStream_Release(ps);
            }
            fprintf(stderr, "vtbl track %lu %p class %08lx\n", i, *(void **)tr, c.Data1);
        }
    }
    g_ev = fopen(out, "w");
    if (g_ev) fprintf(g_ev, "# time %ld %ld\n", t_init, (long)time(NULL));
    if (getenv("DMGT_NOTIFY")) {
        const char *ty = getenv("DMGT_NTYPES") ? getenv("DMGT_NTYPES") : "spm";
        g_nf = fopen(getenv("DMGT_NOTIFY"), "w");
        if (strchr(ty, 's')) IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_SEGMENT);
        if (strchr(ty, 'p')) IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_PERFORMANCE);
        if (strchr(ty, 'm')) IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_MEASUREANDBEAT);
    }
    IDirectMusicSegmentState *st;
    if (FAILED(hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)sg, NULL, NULL, flags, 0, &st, NULL, (IUnknown *)apath)))
        die("PlaySegmentEx", hr);
    g_nss1 = unk_of(st);
    /* DMGT_STOP=<ms>: Stop(segment, state, 0, 0) then StopEx both, as MC2 does; DMGT_NEXT=<ms>:<segment>: then play
     * another segment with the same flags */
    DWORD stop_ms = getenv("DMGT_STOP") ? atoi(getenv("DMGT_STOP")) : 0, next_ms = 0;
    IDirectMusicSegment8 *sg2 = NULL;
    if (getenv("DMGT_NEXT")) {
        char *p;
        next_ms = strtoul(getenv("DMGT_NEXT"), &p, 0);
        MultiByteToWideChar(CP_ACP, 0, p + 1, -1, w, MAX_PATH);
        if (FAILED(hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&sg2)))
            die("load segment 2", hr);
        IDirectMusicSegment8_Download(sg2, (IUnknown *)perf);
    }
    const char *vols = getenv("DMGT_VOL");
    for (DWORD t = 0; t < ms + 1500; t++) {
        for (const char *v = vols; apath && v && *v;) {
            char *e;
            DWORD at = strtoul(v, &e, 0);
            long vol = *e == ':' ? strtol(e + 1, &e, 0) : 0;
            if (at == t) IDirectMusicAudioPath_SetVolume(apath, vol, 0);
            v = *e == ',' ? e + 1 : NULL;
        }
        if (stop_ms && t == stop_ms) {
            dump_tmap(perf, "prestop");
            IDirectMusicPerformance8_Stop(perf, (IDirectMusicSegment *)sg, st, 0, 0);
            IDirectMusicPerformance8_StopEx(perf, (IUnknown *)sg, 0, 0);
            IDirectMusicPerformance8_StopEx(perf, (IUnknown *)st, 0, 0);
            dump_tmap(perf, "stop");
        }
        if (sg2 && t == next_ms) {
            IDirectMusicSegmentState *st2;
            if (FAILED(hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)sg2, NULL, NULL, flags, 0, &st2, NULL, (IUnknown *)apath)))
                die("PlaySegmentEx 2", hr);
            g_nss2 = unk_of(st2);
            dump_tmap(perf, "next");
        }
        if (t && t % 1000 == 0) { char w[16]; sprintf(w, "t%lu", t); dump_tmap(perf, w); }
        if (g_vtime) { v_step(10000); drain_notifications(perf); continue; }
        g_now += 10000;
        Sleep(t < 50 ? 5 : (getenv("DMGT_SLEEP") ? atoi(getenv("DMGT_SLEEP")) : 1));
    }
    if (getenv("DMGT_VTBL")) fprintf(stderr, "vtbl segstate %p\n", *(void **)st);
    MUSIC_TIME mt;
    REFERENCE_TIME rt;
    IDirectMusicSegmentState_GetStartTime(st, &mt);
    IDirectMusicPerformance8_MusicToReferenceTime(perf, mt, &rt);
    EnterCriticalSection(&g_evcs);
    fprintf(g_ev, "# start mt %ld rt %lld\n", mt, rt);
    fclose(g_ev); g_ev = NULL;
    if (g_nf) fclose(g_nf);
    LeaveCriticalSection(&g_evcs);
    fprintf(stderr, "segment start mt %ld rt %lld\n", mt, rt);
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    ExitProcess(0);
}

/* ------------------------------------------------------------------ wavesrev: dsdmo.dll's Waves reverb on raw PCM
 * dmgt <dmdir> wavesrev <in.raw> <out.raw> <rate> <channels> [ingain mix time hfratio] [chunk]: 16-bit PCM in place
 * through IMediaObjectInPlace::Process, in chunks of <chunk> frames (default 441) */
#include <mediaobj.h>
static const GUID CLSID_WavesReverb = { 0x87fc0268, 0x9a55, 0x4360, { 0x95, 0xaa, 0x00, 0x4a, 0x1d, 0x9d, 0xe2, 0x6c } };
static const GUID MT_Audio = { 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID MST_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID FMT_WaveFormatEx = { 0x05589f81, 0xc356, 0x11ce, { 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } };
static const GUID IID_MOInPlace = { 0x651b9ad0, 0x0fc7, 0x4aa9, { 0x95, 0x38, 0xd8, 0x99, 0x31, 0x01, 0x07, 0x41 } };
static const GUID IID_FXWaves = { 0x46858c3a, 0x0dc6, 0x45e3, { 0xb7, 0x60, 0xd4, 0xee, 0xf1, 0x6c, 0xb3, 0x25 } };
typedef struct { float fInGain, fReverbMix, fReverbTime, fHighFreqRTRatio; } wr_params;
static int cmd_wavesrev(int argc, char **argv)
{
    IClassFactory *cf;
    HRESULT hr = get_class(&CLSID_WavesReverb, &IID_IClassFactory, (void **)&cf);
    if (FAILED(hr)) die("WavesReverb class", hr);
    IMediaObject *mo;
    if (FAILED(hr = IClassFactory_CreateInstance(cf, NULL, &IID_IMediaObject, (void **)&mo))) die("CreateInstance", hr);
    DWORD rate = strtoul(argv[5], NULL, 0), ch = strtoul(argv[6], NULL, 0);
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)ch, rate, rate * ch * 2, (WORD)(ch * 2), 16, 0 };
    DMO_MEDIA_TYPE mt = { 0 };
    mt.majortype = MT_Audio; mt.subtype = MST_PCM; mt.bFixedSizeSamples = TRUE; mt.lSampleSize = ch * 2;
    mt.formattype = FMT_WaveFormatEx; mt.cbFormat = sizeof(wf); mt.pbFormat = (BYTE *)&wf;
    if (FAILED(hr = IMediaObject_SetInputType(mo, 0, &mt, 0))) die("SetInputType", hr);
    if (FAILED(hr = IMediaObject_SetOutputType(mo, 0, &mt, 0))) die("SetOutputType", hr);
    if (argc > 10) {
        IUnknown *fx;
        if (FAILED(hr = IMediaObject_QueryInterface(mo, &IID_FXWaves, (void **)&fx))) die("QI WavesReverb", hr);
        wr_params p = { atof(argv[7]), atof(argv[8]), atof(argv[9]), atof(argv[10]) };
        hr = ((HRESULT (WINAPI *)(void *, wr_params *))(*(void ***)fx)[3])(fx, &p);
        if (FAILED(hr)) die("SetAllParameters", hr);
    }
    if (FAILED(hr = IMediaObject_AllocateStreamingResources(mo))) die("AllocateStreamingResources", hr);
    void *ip;
    if (FAILED(hr = IMediaObject_QueryInterface(mo, &IID_MOInPlace, &ip))) die("QI InPlace", hr);
    FILE *f = fopen(argv[3], "rb");
    if (!f) die("open input", E_FAIL);
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    BYTE *b = malloc(n);
    fread(b, 1, n, f); fclose(f);
    DWORD chunk = (argc > 11 ? strtoul(argv[11], NULL, 0) : 441) * ch * 2;
    for (long o = 0; o < n; o += chunk) {
        DWORD sz = (DWORD)(n - o < (long)chunk ? n - o : chunk);
        hr = ((HRESULT (WINAPI *)(void *, ULONG, BYTE *, REFERENCE_TIME, DWORD))(*(void ***)ip)[3])(ip, sz, b + o, 0, 0);
        if (FAILED(hr)) die("Process", hr);
    }
    f = fopen(argv[4], "wb");
    fwrite(b, 1, n, f); fclose(f);
    fprintf(stderr, "processed %ld bytes\n", n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: dmgt <dmdir> synth <dls> <out.wav> <script>\n"); return 2; }
    setvbuf(stderr, NULL, _IONBF, 0);
    CoInitialize(NULL);
    if (getenv("DMGT_DLDUMP")) g_dl = fopen(getenv("DMGT_DLDUMP"), "wb");
    load_dm(argv[1]);
    if (!strcmp(argv[2], "synth") && argc >= 6) return cmd_synth(argv[3], argv[4], argv[5]);
    if (!strcmp(argv[2], "wavesrev") && argc >= 7) return cmd_wavesrev(argc, argv);
    if (!strcmp(argv[2], "perf") && argc >= 6) return cmd_perf(argv[3], argv[4], atoi(argv[5]), argc > 6 ? strtoul(argv[6], NULL, 0) : 0);
    fprintf(stderr, "unknown command\n");
    return 2;
}
