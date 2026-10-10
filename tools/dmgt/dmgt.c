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
#include <stdlib.h>
#include <string.h>

static char g_dir[MAX_PATH];
static const char *const g_dlls[] = { "dmusic.dll", "dmsynth.dll", "dmloader.dll", "dmime.dll", "dmband.dll", "dmstyle.dll",
                                      "dmcompos.dll", "dmscript.dll", "dswave.dll", NULL };
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

static void hook_synth(IDirectMusicSynth8 *s)
{
    void **vt = *(void ***)s;
    DWORD old;
    if (g_orig_download) return;
    VirtualProtect(&vt[6], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    g_orig_download = (download_fn)vt[6];
    vt[6] = (void *)my_download;
    VirtualProtect(&vt[6], sizeof(void *), old, &old);
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

static void load_dm(const char *dir)
{
    strcpy(g_dir, dir);
    int n = 0;
    LoadLibraryA("ole32.dll");
    for (int i = 0; g_dlls[i]; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\%s", dir, g_dlls[i]);
        HMODULE m = LoadLibraryA(p);
        if (!m) { fprintf(stderr, "cannot load %s (%lu)\n", p, GetLastError()); continue; }
        patch_iat(m);
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
    while (fgets(line, sizeof(line), f) && nev < 4096) {
        unsigned ms, st, d1 = 0, d2 = 0;
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

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: dmgt <dmdir> synth <dls> <out.wav> <script>\n"); return 2; }
    setvbuf(stderr, NULL, _IONBF, 0);
    CoInitialize(NULL);
    if (getenv("DMGT_DLDUMP")) g_dl = fopen(getenv("DMGT_DLDUMP"), "wb");
    load_dm(argv[1]);
    if (!strcmp(argv[2], "synth") && argc >= 6) return cmd_synth(argv[3], argv[4], argv[5]);
    fprintf(stderr, "unknown command\n");
    return 2;
}
