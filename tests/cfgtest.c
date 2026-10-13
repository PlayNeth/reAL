/* cfgtest - DirectMusic audiopath configurations (.aud, the 'DMAP' form): builds configurations in memory, loads
 * them through the loader, creates audiopaths from them and prints what GetObjectInPath finds (buffers, their caps,
 * format, volume and pan, their effects and those of mix-in buffers, the port).  Run with tests/comcmp.sh:
 *   sh tests/comcmp.sh cfgtest.exe [config]
 * With FXPROBE_LOG / FXPROBE_DATA set, the configurations that put the probe DMO (tests/fxprobe.c) in a buffer show
 * what reaches it. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dmusici.h>
#include <dmusicf.h>
#include <dsound.h>
#include <mediaobj.h>
#include <stdio.h>
#include <string.h>

typedef struct { LONG lSendLevel; } DSFXSend;

DEFINE_GUID(IID_IMediaObject_, 0xd8ad0f58, 0x5494, 0x4102, 0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4);
DEFINE_GUID(IID_IDirectSoundFXSend_, 0xb30f3564, 0x1698, 0x45ba, 0x9f, 0x75, 0xfc, 0x3c, 0x6c, 0x3b, 0x28, 0x10);
typedef struct { void **lpVtbl; } fxsend;   /* QI, AddRef, Release, SetAllParameters(DSFXSend *), GetAllParameters */
DEFINE_GUID(CLSID_FxProbe, 0x5e1d2c3b, 0x7a4f, 0x4c61, 0x9d, 0x2e, 0x0f, 0x8a, 0x1b, 0x6c, 0x4d, 0x55);
DEFINE_GUID(GUID_DSFX_SEND_, 0xef602176, 0xbcbb, 0x49e0, 0x8c, 0xca, 0xe0, 0x9a, 0x5a, 0x15, 0x2b, 0x33);
DEFINE_GUID(GUID_BufA, 0x11111111, 0x2222, 0x3333, 0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x01);
DEFINE_GUID(GUID_BufB, 0x11111111, 0x2222, 0x3333, 0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x02);
DEFINE_GUID(GUID_BufM, 0x11111111, 0x2222, 0x3333, 0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x03);
DEFINE_GUID(GUID_BufUnknown, 0x11111111, 0x2222, 0x3333, 0x44, 0x44, 0x55, 0x55, 0x66, 0x66, 0x77, 0x09);

/* ------------------------------------------------------------------ a RIFF writer */
typedef struct { BYTE *d; DWORD n, cap; DWORD stack[16]; int sp; } riff;
static void put(riff *r, const void *p, DWORD n)
{
    if (r->n + n > r->cap) { r->cap = (r->n + n) * 2 + 256; r->d = realloc(r->d, r->cap); }
    memcpy(r->d + r->n, p, n);
    r->n += n;
}
static void put4(riff *r, DWORD v) { put(r, &v, 4); }
static void open_(riff *r, const char *id, const char *type)
{
    put(r, id, 4);
    r->stack[r->sp++] = r->n;
    put4(r, 0);
    if (type) put(r, type, 4);
}
static void close_(riff *r)
{
    DWORD at = r->stack[--r->sp], len = r->n - at - 4;
    memcpy(r->d + at, &len, 4);
    if (len & 1) { BYTE z = 0; put(r, &z, 1); }
}
static void chunk(riff *r, const char *id, const void *p, DWORD n) { open_(r, id, NULL); put(r, p, n); close_(r); }

static int g_nofeature;
static void (*g_inner)(riff *r);     /* buffers written inside the port configuration (as DirectMusic Producer does) */
static void portconfig(riff *r, DWORD base, DWORD count, int nbuf, const GUID *const *bufs, DWORD pflags)
{
    open_(r, "LIST", "pcfl");
    DMUS_IO_PORTCONFIG_HEADER h = { GUID_Synth_Default, base, count, pflags };
    chunk(r, "pcfh", &h, sizeof(h));
    DMUS_PORTPARAMS8 pp;
    memset(&pp, 0, sizeof(pp));
    pp.dwSize = sizeof(pp);
    pp.dwValidParams = DMUS_PORTPARAMS_CHANNELGROUPS | (g_nofeature ? 0 : DMUS_PORTPARAMS_FEATURES);
    pp.dwChannelGroups = (base + count + 15) / 16;
    pp.dwFeatures = DMUS_PORT_FEATURE_AUDIOPATH;    /* (as DirectMusic Producer writes it; "nofeature" leaves it out) */
    chunk(r, "pprh", &pp, sizeof(pp));
    if (g_inner) { g_inner(r); g_inner = NULL; }
    open_(r, "LIST", "pchl");
    DMUS_IO_PCHANNELTOBUFFER_HEADER ph = { base, count, (DWORD)nbuf, 0 };
    open_(r, "pchh", NULL);
    put(r, &ph, sizeof(ph));
    for (int i = 0; i < nbuf; i++) put(r, bufs[i], sizeof(GUID));
    close_(r);
    close_(r);
    close_(r);
}

typedef struct { const GUID *cls, *send; const void *data; DWORD n; } fxdesc;
/* a buffer: standard (defined) or described by a DSBC form */
static void buffer(riff *r, const GUID *id, DWORD dmflags, int dsbc, DWORD dsflags, WORD nch, LONG vol, LONG pan,
                   int nbus, const DWORD *bus, const DS3DBUFFER *ds3d, int nfx, const fxdesc *fx)
{
    open_(r, "LIST", "dbfl");
    DMUS_IO_BUFFER_ATTRIBUTES_HEADER h = { *id, dmflags };
    chunk(r, "ddah", &h, sizeof(h));
    if (dsbc) {
        open_(r, "RIFF", "DSBC");
        chunk(r, "guid", id, sizeof(GUID));
        DSOUND_IO_DSBUFFERDESC d = { dsflags, nch, vol, pan, 0 };
        chunk(r, "dsbd", &d, sizeof(d));
        if (nbus) chunk(r, "bsid", bus, nbus * sizeof(DWORD));
        if (ds3d) {
            DSOUND_IO_3D t;
            memset(&t, 0, sizeof(t));
            t.ds3d = *ds3d;
            chunk(r, "ds3d", &t, sizeof(t));
        }
        if (nfx) {
            open_(r, "LIST", "fxls");
            for (int i = 0; i < nfx; i++) {
                open_(r, "RIFF", "DSFX");
                DSOUND_IO_DXDMO_HEADER x;
                memset(&x, 0, sizeof(x));
                x.guidDSFXClass = *fx[i].cls;
                if (fx[i].send) x.guidSendBuffer = *fx[i].send;
                chunk(r, "fxhr", &x, sizeof(x));
                if (fx[i].n) chunk(r, "data", fx[i].data, fx[i].n);
                close_(r);
            }
            close_(r);
        }
        close_(r);
    }
    close_(r);
}

/* ------------------------------------------------------------------ the configurations */
static DSFXSend s_send = { -600 };
static fxdesc s_sendm[1] = { { &GUID_DSFX_SEND_, &GUID_BufM, &s_send, sizeof(s_send) } };
static void inner_a(riff *r)
{
    static const DWORD lr[2] = { DSBUSID_LEFT, DSBUSID_RIGHT };
    buffer(r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX, 2, 0, 0, 2, lr, NULL, 1, s_sendm);
}
static void inner_env(riff *r)
{
    static const DWORD dyn[1] = { DSBUSID_DYNAMIC_0 };
    static fxdesc fx[1] = { { &GUID_DSFX_SEND_, &GUID_Buffer_EnvReverb, &s_send, sizeof(s_send) } };
    buffer(r, &GUID_Buffer_3D_Dry, 0, 1, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX | DSBCAPS_CTRLFREQUENCY, 1, 0, 0,
           1, dyn, NULL, 1, fx);
}
static IDirectMusicLoader8 *loader;
static IDirectMusicPerformance8 *perf;

static void show_buffer(IDirectSoundBuffer8 *b)
{
    WAVEFORMATEX wf; DWORD got = 0; DSBCAPS caps = { sizeof(caps) };
    memset(&wf, 0, sizeof(wf));
    IDirectSoundBuffer8_GetFormat(b, &wf, sizeof(wf), &got);
    IDirectSoundBuffer8_GetCaps(b, &caps);
    LONG vol = 0, pan = 0; DWORD freq = 0, st = 0;
    HRESULT hv = IDirectSoundBuffer8_GetVolume(b, &vol), hp = IDirectSoundBuffer8_GetPan(b, &pan);
    IDirectSoundBuffer8_GetFrequency(b, &freq);
    IDirectSoundBuffer8_GetStatus(b, &st);
    printf("      %u ch %lu Hz %u bit, caps %08lx bytes %lu, volume %08lx %ld, pan %08lx %ld, status %lx\n", wf.nChannels,
           wf.nSamplesPerSec, wf.wBitsPerSample, caps.dwFlags, caps.dwBufferBytes, hv, vol, hp, pan, st);
    IDirectSound3DBuffer8 *b3 = NULL;
    if (SUCCEEDED(IDirectSoundBuffer8_QueryInterface(b, &IID_IDirectSound3DBuffer8, (void **)&b3))) {
        DS3DBUFFER p = { sizeof(p) };
        IDirectSound3DBuffer_GetAllParameters(b3, &p);
        printf("      3D: pos %g %g %g min %g max %g mode %lu\n", p.vPosition.x, p.vPosition.y, p.vPosition.z, p.flMinDistance,
               p.flMaxDistance, p.dwMode);
        IDirectSound3DBuffer_Release(b3);
    }
}

static void show_dmo(IUnknown *u)
{
    IDirectSoundFXEcho8 *e = NULL; IDirectSoundFXWavesReverb8 *w = NULL; IDirectSoundFXI3DL2Reverb8 *r = NULL;
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectSoundFXEcho8, (void **)&e))) {
        DSFXEcho p; IDirectSoundFXEcho_GetAllParameters(e, &p);
        printf(" echo %g %g %g %g %ld", p.fWetDryMix, p.fFeedback, p.fLeftDelay, p.fRightDelay, p.lPanDelay);
        IDirectSoundFXEcho_Release(e);
    } else if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectSoundFXWavesReverb8, (void **)&w))) {
        DSFXWavesReverb p; IDirectSoundFXWavesReverb_GetAllParameters(w, &p);
        printf(" waves %g %g %g %g", p.fInGain, p.fReverbMix, p.fReverbTime, p.fHighFreqRTRatio);
        IDirectSoundFXWavesReverb_Release(w);
    } else if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectSoundFXI3DL2Reverb8, (void **)&r))) {
        DSFXI3DL2Reverb p; IDirectSoundFXI3DL2Reverb_GetAllParameters(r, &p);
        printf(" i3dl2 room %ld decay %g", p.lRoom, p.flDecayTime);
        IDirectSoundFXI3DL2Reverb_Release(r);
    } else {
        fxsend *s = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectSoundFXSend_, (void **)&s))) {
            DSFXSend p = { 12345 };
            HRESULT hr = ((HRESULT (WINAPI *)(void *, DSFXSend *))s->lpVtbl[4])(s, &p);
            printf(" send %08lx %ld", hr, p.lSendLevel);
            IMediaObject *mo = NULL;
            printf(" (IMediaObject %08lx)", IUnknown_QueryInterface(u, &IID_IMediaObject_, (void **)&mo));
            if (mo) IMediaObject_Release(mo);
            ((ULONG (WINAPI *)(void *))s->lpVtbl[2])(s);
        } else printf(" other");
    }
}

static void probe(IDirectMusicAudioPath *ap)
{
    static const struct { DWORD stage, pch; const char *n; } st[] = { { DMUS_PATH_BUFFER, DMUS_PCHANNEL_ALL, "buffer" },
        { DMUS_PATH_MIXIN_BUFFER, 0, "mixin" } };
    for (int s = 0; s < 2; s++)
        for (DWORD k = 0; k < 4; k++) {
            IDirectSoundBuffer8 *b = NULL;
            HRESULT hr = IDirectMusicAudioPath_GetObjectInPath(ap, st[s].pch, st[s].stage, k, &GUID_All_Objects, 0,
                                                               &IID_IDirectSoundBuffer8, (void **)&b);
            printf("  %s %lu: %08lx\n", st[s].n, k, hr);
            if (FAILED(hr)) break;
            show_buffer(b);
            IDirectSoundBuffer8_Release(b);
            for (DWORD j = 0; j < 4; j++) {
                IUnknown *u = NULL;
                hr = IDirectMusicAudioPath_GetObjectInPath(ap, st[s].pch, st[s].stage + 0x100, k, &GUID_All_Objects, j,
                                                           &IID_IUnknown, (void **)&u);
                printf("    dmo %lu: %08lx", j, hr);
                if (u) { show_dmo(u); IUnknown_Release(u); }
                printf("\n");
                if (FAILED(hr)) break;
            }
        }
    {   /* the stage rules: a mix-in for all PChannels, a buffer number at another stage, the primary buffer */
        IUnknown *u = NULL;
        printf("  mixin, PChannel ALL: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_MIXIN_BUFFER, 0,
               &GUID_All_Objects, 0, &IID_IUnknown, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        printf("  port with buffer 1: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_PORT, 1,
               &GUID_All_Objects, 0, &IID_IUnknown, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        printf("  primary: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_PRIMARY_BUFFER, 0,
               &GUID_All_Objects, 0, &IID_IUnknown, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        printf("  sink: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 0, 0x5000, 0, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        printf("  stage 0x6200: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 0, 0x6200, 0, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
    }
    for (DWORD pch = 0; pch < 18; pch += 17) {
        IDirectSoundBuffer8 *b = NULL;
        printf("  buffer for pchannel %lu: %08lx\n", pch, IDirectMusicAudioPath_GetObjectInPath(ap, pch, DMUS_PATH_BUFFER, 0,
               &GUID_All_Objects, 0, &IID_IDirectSoundBuffer8, (void **)&b));
        if (b) IDirectSoundBuffer8_Release(b);
    }
    IDirectMusicPort *port = NULL;
    printf("  port: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_PORT, 0, &GUID_All_Objects, 0,
           &IID_IDirectMusicPort, (void **)&port));
    if (port) IDirectMusicPort_Release(port);
    for (DWORD p = 0; p < 20; p += 5) {
        DWORD out = 0xdead;
        HRESULT hr = IDirectMusicAudioPath_ConvertPChannel(ap, p, &out);
        printf("  ConvertPChannel(%lu): %08lx %lu\n", p, hr, out);
    }
}

/* what the probe DMO saw (FXPROBE_LOG / FXPROBE_DATA): its calls but Process, and whether any sound reached it */
static void probe_report(void)
{
    if (getenv("FXPROBE_DATA") && !getenv("CFG_DATA")) DeleteFileA(getenv("FXPROBE_DATA"));
    char *lp = getenv("FXPROBE_LOG"), *dp = getenv("FXPROBE_DATA");
    if (!lp) return;
    FILE *f = fopen(lp, "r");
    if (f) {
        char l[512];
        /* (not Process or the releases: when they come is timing) */
        while (fgets(l, sizeof(l), f)) if (strncmp(l, "Process", 7) && strncmp(l, "released", 8)) printf("  probe: %s", l);
        fclose(f);
        DeleteFileA(lp);
    }
    if (dp && getenv("CFG_DATA") && (f = fopen(dp, "rb"))) {     /* (Microsoft's sink under Wine is not reliable here) */
        int nz = 0, c;
        long n = 0;
        while ((c = fgetc(f)) != EOF) { n++; if (c) nz = 1; }
        fclose(f);
        printf("  probe data: %s\n", n ? (nz ? "sound" : "silence") : "none");
    }
}
static void run(const char *name, riff *r, int play)
{
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_MEMORY;
    d.guidClass = CLSID_DirectMusicAudioPathConfig;
    d.pbMemData = r->d;
    d.llMemLength = r->n;
    IUnknown *cfg = NULL;
    HRESULT hr = IDirectMusicLoader8_GetObject(loader, &d, &IID_IUnknown, (void **)&cfg);
    printf("%s: load %08lx\n", name, hr);
    /* the loader keeps what it loads from memory, failures too, and matches the next memory load of the class to it */
    IDirectMusicLoader8_ClearCache(loader, &CLSID_DirectMusicAudioPathConfig);
    if (FAILED(hr)) { memset(r, 0, sizeof(*r)); return; }
    IDirectMusicObject *o = NULL;
    if (SUCCEEDED(IUnknown_QueryInterface(cfg, &IID_IDirectMusicObject, (void **)&o))) {
        DMUS_OBJECTDESC od = { sizeof(od) };
        IDirectMusicObject_GetDescriptor(o, &od);
        printf("  descriptor valid %lx\n", od.dwValidData);
        IDirectMusicObject_Release(o);
    }
    IDirectMusicAudioPath *ap = NULL;
    hr = IDirectMusicPerformance8_CreateAudioPath(perf, cfg, TRUE, &ap);
    printf("  CreateAudioPath %08lx\n", hr);
    if (ap) {
        probe(ap);
        /* a second path from the same configuration, and the default path: which buffers and effects are shared */
        IDirectMusicAudioPath *ap2 = NULL, *def = NULL;
        printf("  second path %08lx\n", IDirectMusicPerformance8_CreateAudioPath(perf, cfg, TRUE, &ap2));
        IDirectMusicPerformance8_GetDefaultAudioPath(perf, &def);
        for (DWORD k = 0; k < 3; k++) {
            IUnknown *a = NULL, *b = NULL, *d = NULL, *x = NULL, *y = NULL;
            IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, k, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&a);
            if (ap2) IDirectMusicAudioPath_GetObjectInPath(ap2, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, k, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&b);
            if (def) IDirectMusicAudioPath_GetObjectInPath(def, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, k, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&d);
            IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER_DMO, k, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&x);
            if (ap2) IDirectMusicAudioPath_GetObjectInPath(ap2, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER_DMO, k, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&y);
            if (a) printf("  buffer %lu: second path's %s, default path's %s; dmo 0 %s\n", k, b ? (a == b ? "same" : "other") : "none",
                          d ? (a == d ? "same" : "other") : "none", x ? (y ? (x == y ? "same" : "other") : "none in the second") : "none");
            if (a) IUnknown_Release(a);
            if (b) IUnknown_Release(b);
            if (d) IUnknown_Release(d);
            if (x) IUnknown_Release(x);
            if (y) IUnknown_Release(y);
        }
        IUnknown *m1 = NULL, *m2 = NULL;
        IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_MIXIN_BUFFER, 0, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&m1);
        if (ap2) IDirectMusicAudioPath_GetObjectInPath(ap2, 0, DMUS_PATH_MIXIN_BUFFER, 0, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&m2);
        if (m1) printf("  mixin 0: second path's %s\n", m2 ? (m1 == m2 ? "same" : "other") : "none");
        if (m1) IUnknown_Release(m1);
        if (m2) IUnknown_Release(m2);
        if (def) IDirectMusicAudioPath_Release(def);
        if (ap2) IDirectMusicAudioPath_Release(ap2);
        if (play) {
            IDirectMusicSegment8 *seg = NULL;
            WCHAR f[] = L"bells1.sgt";
            if (SUCCEEDED(IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, f, (void **)&seg))) {
                IDirectMusicSegment8_Download(seg, (IUnknown *)ap);
                hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)seg, NULL, NULL, 0, 0, NULL, NULL, (IUnknown *)ap);
                printf("  play %08lx\n", hr);
                Sleep(1500);
                IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
                IDirectMusicSegment8_Unload(seg, (IUnknown *)ap);
                IDirectMusicSegment8_Release(seg);
            }
        }
        IDirectMusicAudioPath_Activate(ap, FALSE);
        IDirectMusicAudioPath_Release(ap);
    }
    IUnknown_Release(cfg);
    probe_report();
    memset(r, 0, sizeof(*r));               /* (the memory stays: a new configuration at the address of an old one would
                                             * be the old one's for the loader, even a failed one) */
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    LoadLibraryA("dsound.dll");
    if (getenv("FXPROBE_LOG")) DeleteFileA(getenv("FXPROBE_LOG"));
    if (getenv("FXPROBE_DATA")) DeleteFileA(getenv("FXPROBE_DATA"));     /* reAL's hooks DirectMusic's classes from dsound.dll (nothing else here loads it) */
    CoInitialize(NULL);
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, dir, FALSE);
    HRESULT hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, 0, 64, DMUS_AUDIOF_ALL, NULL);
    printf("InitAudio %08lx\n", hr);
    riff r;
    memset(&r, 0, sizeof(r));
#define WANT(n) (!only || !strcmp(only, n))
    const DWORD lr[2] = { DSBUSID_LEFT, DSBUSID_RIGHT }, rev[1] = { DSBUSID_REVERB_SEND }, dyn[1] = { DSBUSID_DYNAMIC_0 };
    if (WANT("std")) {     /* the standard stereo + reverb path, as a configuration */
        const GUID *b[2] = { &GUID_Buffer_Stereo, &GUID_Buffer_Reverb };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 2, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_Buffer_Stereo, DMUS_BUFFERF_SHARED | DMUS_BUFFERF_DEFINED, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        buffer(&r, &GUID_Buffer_Reverb, DMUS_BUFFERF_SHARED | DMUS_BUFFERF_DEFINED, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        close_(&r);
        run("std", &r, 0);
    }
    if (WANT("std2")) {    /* the same buffers not shared, and on PChannels 0-31 */
        const GUID *b[2] = { &GUID_Buffer_Stereo, &GUID_Buffer_Reverb };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 32, 2, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_Buffer_Stereo, DMUS_BUFFERF_DEFINED, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        buffer(&r, &GUID_Buffer_Reverb, DMUS_BUFFERF_DEFINED, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        close_(&r);
        run("std2", &r, 0);
    }
    if (WANT("mono3d")) {
        const GUID *b[1] = { &GUID_Buffer_3D_Dry };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_Buffer_3D_Dry, DMUS_BUFFERF_DEFINED, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        buffer(&r, &GUID_Buffer_Mono, DMUS_BUFFERF_DEFINED, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        close_(&r);
        run("mono3d", &r, 0);
    }
    if (WANT("envrev")) {
        const GUID *b[1] = { &GUID_Buffer_3D_Dry };
        static const GUID *envsend = &GUID_Buffer_EnvReverb;
        DSFXSend s = { -1000 };
        fxdesc fx[1] = { { &GUID_DSFX_SEND_, envsend, &s, sizeof(s) } };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_Buffer_3D_Dry, 0, 1, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX | DSBCAPS_CTRLFREQUENCY, 1, 0, 0,
               1, dyn, NULL, 1, fx);
        buffer(&r, &GUID_Buffer_EnvReverb, DMUS_BUFFERF_SHARED | DMUS_BUFFERF_DEFINED | DMUS_BUFFERF_MIXIN, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        close_(&r);
        run("envrev", &r, 0);
    }
    if (WANT("custom")) {  /* a stereo buffer of its own with an echo and the probe */
        const GUID *b[1] = { &GUID_BufA };
        DSFXEcho e = { 30, 40, 200, 300, 0 };
        fxdesc fx[2] = { { &GUID_DSFX_STANDARD_ECHO, NULL, &e, sizeof(e) }, { &CLSID_FxProbe, NULL, NULL, 0 } };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFX, 2, -300, 1000, 2, lr, NULL, 2, fx);
        close_(&r);
        run("custom", &r, 1);
    }
    if (WANT("plain")) {   /* a stereo buffer of its own, no effects */
        const GUID *b[1] = { &GUID_BufA };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN, 2, -300, 1000, 2, lr, NULL, 0, NULL);
        close_(&r);
        run("plain", &r, 0);
        DSFXEcho e = { 30, 40, 200, 300, 0 };
        fxdesc fx1[1] = { { &GUID_DSFX_STANDARD_ECHO, NULL, &e, sizeof(e) } };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFX, 2, -300, 1000, 2, lr, NULL, 1, fx1);
        close_(&r);
        run("plain echo", &r, 0);
        fxdesc fx2[1] = { { &GUID_DSFX_STANDARD_ECHO, NULL, NULL, 0 } };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFX, 2, -300, 1000, 2, lr, NULL, 1, fx2);
        close_(&r);
        run("plain echo nodata", &r, 0);
    }
    if (WANT("custom3d")) {
        const GUID *b[1] = { &GUID_BufA };
        DS3DBUFFER p;
        memset(&p, 0, sizeof(p));
        p.dwSize = sizeof(p);
        p.vPosition.x = 3; p.vPosition.z = 1; p.flMinDistance = 2; p.flMaxDistance = 100; p.dwMode = DS3DMODE_NORMAL;
        p.dwInsideConeAngle = p.dwOutsideConeAngle = 360; p.vConeOrientation.z = 1;
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY, 1, -100, 0, 1, dyn, &p, 0, NULL);
        close_(&r);
        run("custom3d", &r, 0);
    }
    if (WANT("mixin")) {   /* a stereo buffer sending to a mix-in buffer with a reverb and the probe */
        const GUID *b[2] = { &GUID_BufA, &GUID_BufB };
        DSFXSend s = { -600 };
        DSFXWavesReverb w = { 0, -3, 1200, 0.2f };
        fxdesc fa[1] = { { &GUID_DSFX_SEND_, &GUID_BufM, &s, sizeof(s) } };
        fxdesc fm[2] = { { &GUID_DSFX_WAVES_REVERB, NULL, &w, sizeof(w) }, { &CLSID_FxProbe, NULL, NULL, 0 } };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 2, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX, 2, 0, 0, 2, lr, NULL, 1, fa);
        buffer(&r, &GUID_BufB, 0, 1, DSBCAPS_CTRLVOLUME, 1, -600, 0, 1, rev, NULL, 0, NULL);
        buffer(&r, &GUID_BufM, DMUS_BUFFERF_MIXIN, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX, 2, -200, 0, 0, NULL, NULL, 2, fm);
        close_(&r);
        run("mixin", &r, 1);
    }
    if (WANT("order")) {   /* the mix-in first; then the sending buffer inside the port configuration */
        const GUID *b[1] = { &GUID_BufA };
        open_(&r, "RIFF", "DMAP");
        buffer(&r, &GUID_BufM, DMUS_BUFFERF_MIXIN, 1, DSBCAPS_CTRLVOLUME, 2, -200, 0, 0, NULL, NULL, 0, NULL);
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX, 2, 0, 0, 2, lr, NULL, 1, s_sendm);
        close_(&r);
        run("mixin first", &r, 0);
        open_(&r, "RIFF", "DMAP");
        g_inner = inner_a;
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufM, DMUS_BUFFERF_MIXIN, 1, DSBCAPS_CTRLVOLUME, 2, -200, 0, 0, NULL, NULL, 0, NULL);
        close_(&r);
        run("sender in the port", &r, 0);
        const GUID *e[1] = { &GUID_Buffer_3D_Dry };
        open_(&r, "RIFF", "DMAP");
        g_inner = inner_env;
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, e, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_Buffer_EnvReverb, DMUS_BUFFERF_SHARED | DMUS_BUFFERF_DEFINED | DMUS_BUFFERF_MIXIN, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        close_(&r);
        run("envrev in the port", &r, 0);
    }
    if (WANT("nofeature")) {   /* a port without the audiopath feature: its PChannels keep their buses */
        const GUID *b[1] = { &GUID_BufA };
        g_nofeature = 1;
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLVOLUME, 2, 0, 0, 2, lr, NULL, 0, NULL);
        close_(&r);
        g_nofeature = getenv("CFG_NOFEATURE") != NULL;
        run("nofeature", &r, 0);
    }
    if (WANT("mixplay")) {   /* a stereo buffer sending to a mix-in with a reverb and the probe, playing */
        const GUID *b[1] = { &GUID_BufA };
        DSFXWavesReverb w = { 0, -3, 1200, 0.2f };
        fxdesc fm[2] = { { &GUID_DSFX_WAVES_REVERB, NULL, &w, sizeof(w) }, { &CLSID_FxProbe, NULL, NULL, 0 } };
        open_(&r, "RIFF", "DMAP");
        buffer(&r, &GUID_BufM, DMUS_BUFFERF_MIXIN, 1, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX, 2, -200, 0, 0, NULL, NULL, 2, fm);
        g_inner = inner_a;
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        close_(&r);
        run("mixplay", &r, 1);
    }
    if (WANT("envplay")) {   /* the 3D path's send to the environmental reverb (I3DL2), the probe after it */
        const GUID *e[1] = { &GUID_Buffer_3D_Dry };
        open_(&r, "RIFF", "DMAP");
        buffer(&r, &GUID_Buffer_EnvReverb, DMUS_BUFFERF_SHARED | DMUS_BUFFERF_DEFINED | DMUS_BUFFERF_MIXIN, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL);
        g_inner = inner_env;
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, e, DMUS_PORTCONFIGF_DRUMSON10); close_(&r);
        close_(&r);
        run("envplay", &r, 1);
    }
    if (WANT("errors")) {
        const GUID *b[1] = { &GUID_BufUnknown };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, b, 0); close_(&r);
        close_(&r);
        run("undeclared buffer", &r, 0);
        const GUID *a[1] = { &GUID_BufA };
        fxdesc fx[1] = { { &GUID_BufUnknown, NULL, NULL, 0 } };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, a, 0); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLFX, 2, 0, 0, 2, lr, NULL, 1, fx);
        close_(&r);
        run("unknown effect", &r, 0);
        fxdesc fs[1] = { { &GUID_DSFX_SEND_, &GUID_BufUnknown, NULL, 0 } };
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 1, a, 0); close_(&r);
        buffer(&r, &GUID_BufA, 0, 1, DSBCAPS_CTRLFX, 2, 0, 0, 2, lr, NULL, 1, fs);
        close_(&r);
        run("send nowhere", &r, 0);
        open_(&r, "RIFF", "DMAP");
        close_(&r);
        run("empty", &r, 0);
        open_(&r, "RIFF", "DMAP");
        open_(&r, "LIST", "pcsl"); portconfig(&r, 0, 16, 0, NULL, 0); close_(&r);
        close_(&r);
        run("no buffers", &r, 0);
    }
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    IDirectMusicLoader8_Release(loader);
    CoUninitialize();
    return 0;
}
