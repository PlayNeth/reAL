/* fxtest - DirectSound effects (IDirectSoundBuffer8::SetFX) against Microsoft's: the API's results, the effects'
 * parameter interfaces and the processed audio.  Run through tests/comcmp.sh with the probe DMO (tests/fxprobe.c)
 * registered and FXPROBE_LOG=fxlog.txt FXPROBE_DATA=fxdata.raw in the environment:
 *   FXPROBE_LOG=fxlog.txt FXPROBE_DATA=fxdata.raw sh tests/comcmp.sh fxtest.exe [section]
 * Sections: api, params, data (default: all). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmreg.h>
#include <dsound.h>
#include <mediaobj.h>
#include <medparam.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

DEFINE_GUID(CLSID_FxProbe, 0x5e1d2c3b, 0x7a4f, 0x4c61, 0x9d, 0x2e, 0x0f, 0x8a, 0x1b, 0x6c, 0x4d, 0x55);
DEFINE_GUID(GUID_Unknown_FX, 0x12345678, 0x1111, 0x2222, 0x33, 0x33, 0x44, 0x44, 0x55, 0x55, 0x66, 0x66);
DEFINE_GUID(IID_IMediaObject_, 0xd8ad0f58, 0x5494, 0x4102, 0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4);
DEFINE_GUID(IID_IMediaObjectInPlace_, 0x651b9ad0, 0x0fc7, 0x4aa9, 0x95, 0x38, 0xd8, 0x99, 0x31, 0x01, 0x07, 0x41);
DEFINE_GUID(IID_IMediaParams_, 0x6d6cbb60, 0xa223, 0x44aa, 0x84, 0x2f, 0xa2, 0xf0, 0x67, 0x50, 0xbe, 0x6e);
DEFINE_GUID(IID_IMediaParamInfo_, 0x6d6cbb61, 0xa223, 0x44aa, 0x84, 0x2f, 0xa2, 0xf0, 0x67, 0x50, 0xbe, 0x6d);
DEFINE_GUID(IID_IPersistStream_, 0x00000109, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(GUID_All_Objects_, 0xaa114de5, 0xc262, 0x4169, 0xa1, 0xc8, 0x23, 0xd6, 0x98, 0xcc, 0x73, 0xb5);
DEFINE_GUID(KSDATAFORMAT_SUBTYPE_PCM_, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);

static IDirectSound8 *ds;

typedef struct { const char *name; const GUID *guid; const IID *iid; int nfields; const char *types; } fxinfo;
static const fxinfo fxs[] = {
    { "gargle", &GUID_DSFX_STANDARD_GARGLE, &IID_IDirectSoundFXGargle8, 2, "uu" },
    { "chorus", &GUID_DSFX_STANDARD_CHORUS, &IID_IDirectSoundFXChorus8, 7, "ffffifi" },
    { "flanger", &GUID_DSFX_STANDARD_FLANGER, &IID_IDirectSoundFXFlanger8, 7, "ffffifi" },
    { "echo", &GUID_DSFX_STANDARD_ECHO, &IID_IDirectSoundFXEcho8, 5, "ffffi" },
    { "distortion", &GUID_DSFX_STANDARD_DISTORTION, &IID_IDirectSoundFXDistortion8, 5, "fffff" },
    { "compressor", &GUID_DSFX_STANDARD_COMPRESSOR, &IID_IDirectSoundFXCompressor8, 6, "ffffff" },
    { "parameq", &GUID_DSFX_STANDARD_PARAMEQ, &IID_IDirectSoundFXParamEq8, 3, "fff" },
    { "waves", &GUID_DSFX_WAVES_REVERB, &IID_IDirectSoundFXWavesReverb8, 4, "ffff" },
    { "i3dl2", &GUID_DSFX_STANDARD_I3DL2REVERB, &IID_IDirectSoundFXI3DL2Reverb8, 12, "iifffififfff" },
};
#define NFX (sizeof(fxs) / sizeof(fxs[0]))

static void pparams(const fxinfo *fx, const void *p)
{
    const DWORD *w = p;
    for (int i = 0; i < fx->nfields; i++) {
        float f; memcpy(&f, &w[i], 4);
        if (fx->types[i] == 'f') printf(" %.9g", f); else printf(" %ld", (long)w[i]);
    }
}

static IDirectSoundBuffer8 *mkbuf(DWORD flags, WORD tag, WORD ch, DWORD rate, WORD bits, DWORD ms, HRESULT *phr)
{
    WAVEFORMATEXTENSIBLE x;
    memset(&x, 0, sizeof(x));
    WAVEFORMATEX *w = &x.Format;
    w->wFormatTag = tag; w->nChannels = ch; w->nSamplesPerSec = rate; w->wBitsPerSample = bits;
    w->nBlockAlign = ch * bits / 8; w->nAvgBytesPerSec = rate * w->nBlockAlign;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        w->cbSize = 22; x.Samples.wValidBitsPerSample = bits; x.dwChannelMask = ch == 4 ? 0x33 : ch == 6 ? 0x3f : 3;
        x.SubFormat = KSDATAFORMAT_SUBTYPE_PCM_;
    }
    DSBUFFERDESC d = { sizeof(d), flags, rate * w->nBlockAlign * ms / 1000, 0, w, { 0 } };
    d.dwBufferBytes -= d.dwBufferBytes % w->nBlockAlign;
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(ds, &d, &b, NULL);
    if (phr) *phr = hr;
    if (FAILED(hr)) return NULL;
    IDirectSoundBuffer8 *b8 = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundBuffer8, (void **)&b8);
    IDirectSoundBuffer_Release(b);
    return b8;
}

static void setfx(IDirectSoundBuffer8 *b, const char *what, int n, const GUID *const *g, DWORD flags)
{
    DSEFFECTDESC d[8];
    DWORD r[8];
    for (int i = 0; i < n; i++) { memset(&d[i], 0, sizeof(d[i])); d[i].dwSize = sizeof(d[i]); d[i].dwFlags = flags; d[i].guidDSFXClass = *g[i]; r[i] = 0xdead; }
    HRESULT hr = IDirectSoundBuffer8_SetFX(b, n, n ? d : NULL, r);
    printf("%s: SetFX %08lx", what, hr);
    for (int i = 0; i < n; i++) printf(" %lx", r[i]);
    printf("\n");
}

/* ------------------------------------------------------------------ the API */
static void test_api(void)
{
    HRESULT hr;
    static const struct { const char *n; WORD tag, ch; DWORD rate; WORD bits; DWORD ms, flags; } bufs[] = {
        { "16s", 1, 2, 44100, 16, 1000, 0 }, { "16s_100ms", 1, 2, 44100, 16, 100, 0 }, { "16s_149ms", 1, 2, 44100, 16, 149, 0 },
        { "16s_150ms", 1, 2, 44100, 16, 150, 0 }, { "8m", 1, 1, 22050, 8, 500, 0 }, { "f32s", 3, 2, 48000, 32, 500, 0 },
        { "24s", 1, 2, 44100, 24, 500, 0 }, { "ext16s", WAVE_FORMAT_EXTENSIBLE, 2, 44100, 16, 500, 0 },
        { "ext16q", WAVE_FORMAT_EXTENSIBLE, 4, 44100, 16, 500, 0 }, { "16m_3d", 1, 1, 44100, 16, 500, DSBCAPS_CTRL3D },
        { "16s_hw", 1, 2, 44100, 16, 500, DSBCAPS_LOCHARDWARE }, { "16s_defer", 1, 2, 44100, 16, 500, DSBCAPS_LOCDEFER },
        { "16s_static", 1, 2, 44100, 16, 500, DSBCAPS_STATIC }, { "16s_vol", 1, 2, 44100, 16, 500, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN },
    };
    const GUID *echo = &GUID_DSFX_STANDARD_ECHO;
    for (unsigned i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++) {
        IDirectSoundBuffer8 *b = mkbuf(DSBCAPS_CTRLFX | bufs[i].flags, bufs[i].tag, bufs[i].ch, bufs[i].rate, bufs[i].bits, bufs[i].ms, &hr);
        printf("create %s ctrlfx: %08lx\n", bufs[i].n, hr);
        if (!b) continue;
        DSBCAPS c = { sizeof(c) };
        IDirectSoundBuffer8_GetCaps(b, &c);
        printf("  caps flags %08lx\n", c.dwFlags);
        setfx(b, "  echo", 1, &echo, 0);
        for (unsigned k = 0; k < NFX; k++) { char t[64]; sprintf(t, "  %s", fxs[k].name); setfx(b, t, 1, &fxs[k].guid, 0); }
        IDirectSoundBuffer8_Release(b);
    }
    /* no CTRLFX */
    IDirectSoundBuffer8 *b = mkbuf(0, 1, 2, 44100, 16, 500, &hr);
    setfx(b, "noctrlfx echo", 1, &echo, 0);
    setfx(b, "noctrlfx none", 0, NULL, 0);
    printf("noctrlfx clear: %08lx\n", IDirectSoundBuffer8_SetFX(b, 0, NULL, NULL));
    void *u0 = NULL;
    printf("noctrlfx path: %08lx\n", IDirectSoundBuffer8_GetObjectInPath(b, &GUID_DSFX_STANDARD_ECHO, 0, &IID_IUnknown, (void **)&u0));
    IDirectSoundBuffer8_Release(b);
    /* IDirectSound (not 8) */
    IDirectSound *d7;
    if (SUCCEEDED(DirectSoundCreate(NULL, &d7, NULL))) {
        IDirectSound_SetCooperativeLevel(d7, GetDesktopWindow(), DSSCL_PRIORITY);
        WAVEFORMATEX w = { 1, 2, 44100, 44100 * 4, 4, 16, 0 };
        DSBUFFERDESC d = { sizeof(d), DSBCAPS_CTRLFX, 44100 * 4, 0, &w, { 0 } };
        IDirectSoundBuffer *b7 = NULL;
        printf("ds7 create ctrlfx: %08lx\n", IDirectSound_CreateSoundBuffer(d7, &d, &b7, NULL));
        if (b7) IDirectSoundBuffer_Release(b7);
        IDirectSound_Release(d7);
    }
    /* primary with CTRLFX */
    {
        DSBUFFERDESC d = { sizeof(d), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLFX, 0, 0, NULL, { 0 } };
        IDirectSoundBuffer *p = NULL;
        printf("primary ctrlfx: %08lx\n", IDirectSound8_CreateSoundBuffer(ds, &d, &p, NULL));
        if (p) IDirectSoundBuffer_Release(p);
    }

    b = mkbuf(DSBCAPS_CTRLFX, 1, 2, 44100, 16, 1000, &hr);
    DSEFFECTDESC e = { sizeof(e), 0, GUID_DSFX_STANDARD_ECHO, 0, 0 };
    DWORD r[4] = { 0xdead, 0xdead, 0xdead, 0xdead };
    printf("count1 null desc: %08lx\n", IDirectSoundBuffer8_SetFX(b, 1, NULL, r));
    printf("count0 desc: %08lx\n", IDirectSoundBuffer8_SetFX(b, 0, &e, r));
    printf("null results: %08lx\n", IDirectSoundBuffer8_SetFX(b, 1, &e, NULL));
    e.dwSize = 8; r[0] = 0xdead;
    printf("bad size: %08lx %lx\n", IDirectSoundBuffer8_SetFX(b, 1, &e, r), r[0]);
    e.dwSize = sizeof(e); e.dwReserved1 = 1; r[0] = 0xdead;
    printf("reserved1: %08lx %lx\n", IDirectSoundBuffer8_SetFX(b, 1, &e, r), r[0]);
    e.dwReserved1 = 0; e.dwReserved2 = 1; r[0] = 0xdead;
    printf("reserved2: %08lx %lx\n", IDirectSoundBuffer8_SetFX(b, 1, &e, r), r[0]);
    e.dwReserved2 = 0;
    for (DWORD fl = 1; fl <= 8; fl <<= 1) { e.dwFlags = fl; r[0] = 0xdead; printf("flags %lx: %08lx %lx\n", fl, IDirectSoundBuffer8_SetFX(b, 1, &e, r), r[0]); }
    e.dwFlags = DSFX_LOCHARDWARE | DSFX_LOCSOFTWARE; r[0] = 0xdead;
    printf("flags 3: %08lx %lx\n", IDirectSoundBuffer8_SetFX(b, 1, &e, r), r[0]);
    const GUID *mix[3] = { &GUID_DSFX_STANDARD_GARGLE, &GUID_Unknown_FX, &GUID_DSFX_STANDARD_ECHO };
    setfx(b, "with unknown", 3, mix, 0);
    const GUID *unk = &GUID_Unknown_FX;
    setfx(b, "unknown", 1, &unk, 0);
    const GUID *probe = &CLSID_FxProbe;
    setfx(b, "probe", 1, &probe, 0);
    const GUID *two[2] = { &GUID_DSFX_STANDARD_ECHO, &GUID_DSFX_STANDARD_ECHO };
    setfx(b, "echo echo", 2, two, 0);
    const GUID *eight[8] = { echo, echo, echo, echo, echo, echo, echo, echo };
    setfx(b, "8 echoes", 8, eight, 0);
    setfx(b, "clear", 0, NULL, 0);
    printf("clear null: %08lx\n", IDirectSoundBuffer8_SetFX(b, 0, NULL, NULL));
    IDirectSoundBuffer8_Play(b, 0, 0, DSBPLAY_LOOPING);
    setfx(b, "playing", 1, &echo, 0);
    setfx(b, "playing clear", 0, NULL, 0);
    IDirectSoundBuffer8_Stop(b);
    setfx(b, "stopped", 1, &echo, 0);
    {
        void *p1; DWORD n1;
        printf("lock: %08lx\n", IDirectSoundBuffer8_Lock(b, 0, 0, &p1, &n1, NULL, NULL, DSBLOCK_ENTIREBUFFER));
        setfx(b, "locked", 1, &echo, 0);
        IDirectSoundBuffer8_Unlock(b, p1, n1, NULL, 0);
    }
    DWORD st = 0xdead;
    IDirectSoundBuffer8_GetStatus(b, &st);
    printf("status stopped: %lx\n", st);
    IDirectSoundBuffer8_Play(b, 0, 0, DSBPLAY_LOOPING);
    IDirectSoundBuffer8_GetStatus(b, &st);
    printf("status playing: %lx\n", st);
    IDirectSoundBuffer8_Stop(b);
    {
        IDirectSoundBuffer *dup = (IDirectSoundBuffer *)1;
        printf("duplicate: %08lx\n", IDirectSound8_DuplicateSoundBuffer(ds, (IDirectSoundBuffer *)b, &dup));
        if (dup && dup != (IDirectSoundBuffer *)1) IDirectSoundBuffer_Release(dup);
    }
    DWORD ar[2] = { 0xdead, 0xdead };
    printf("acquire: %08lx %lx\n", IDirectSoundBuffer8_AcquireResources(b, 0, 1, ar), ar[0]);
    printf("acquire 2: %08lx %lx %lx\n", IDirectSoundBuffer8_AcquireResources(b, 0, 2, ar), ar[0], ar[1]);
    IDirectSoundBuffer8_Release(b);
    b = mkbuf(DSBCAPS_CTRLFX | DSBCAPS_LOCDEFER, 1, 2, 44100, 16, 1000, &hr);
    setfx(b, "defer echo", 1, &echo, 0);
    ar[0] = 0xdead;
    {
        DSBCAPS c = { sizeof(c) }; DWORD st = 0;
        IDirectSoundBuffer8_GetCaps(b, &c); IDirectSoundBuffer8_GetStatus(b, &st);
        printf("defer before: caps %08lx status %lx\n", c.dwFlags, st);
    }
    printf("defer acquire: %08lx %lx\n", IDirectSoundBuffer8_AcquireResources(b, DSBPLAY_LOCSOFTWARE, 1, ar), ar[0]);
    {
        DSBCAPS c = { sizeof(c) }; DWORD st = 0;
        IDirectSoundBuffer8_GetCaps(b, &c); IDirectSoundBuffer8_GetStatus(b, &st);
        printf("defer after: caps %08lx status %lx\n", c.dwFlags, st);
        printf("defer acquire again: %08lx %lx\n", IDirectSoundBuffer8_AcquireResources(b, 0, 1, ar), ar[0]);
    }
    IDirectSoundBuffer8_Release(b);
    b = mkbuf(DSBCAPS_CTRLFX | DSBCAPS_LOCDEFER, 1, 2, 44100, 16, 1000, &hr);
    setfx(b, "defer2 echo", 1, &echo, 0);
    IDirectSoundBuffer8_Play(b, 0, 0, DSBPLAY_LOOPING);
    {
        DSBCAPS c = { sizeof(c) }; DWORD st = 0;
        IDirectSoundBuffer8_GetCaps(b, &c); IDirectSoundBuffer8_GetStatus(b, &st);
        printf("defer2 playing: caps %08lx status %lx\n", c.dwFlags, st);
        IDirectSoundBuffer8_Stop(b);
        IDirectSoundBuffer8_GetStatus(b, &st);
        printf("defer2 stopped: status %lx\n", st);
        ar[0] = 0xdead;
        printf("defer2 acquire: %08lx %lx\n", IDirectSoundBuffer8_AcquireResources(b, 0, 1, ar), ar[0]);
        setfx(b, "defer2 again", 1, &echo, 0);
        IDirectSoundBuffer8_GetCaps(b, &c);
        printf("defer2 caps %08lx\n", c.dwFlags);
    }
    IDirectSoundBuffer8_Release(b);
    b = mkbuf(DSBCAPS_CTRLFX | DSBCAPS_LOCDEFER, 1, 2, 44100, 16, 1000, &hr);
    ar[0] = 0xdead;
    printf("defer3 acquire nofx: %08lx %lx\n", IDirectSoundBuffer8_AcquireResources(b, 0, 1, ar), ar[0]);
    printf("defer3 acquire 0: %08lx\n", IDirectSoundBuffer8_AcquireResources(b, 0, 0, NULL));
    IDirectSoundBuffer8_Release(b);

    /* GetObjectInPath */
    b = mkbuf(DSBCAPS_CTRLFX, 1, 2, 44100, 16, 1000, &hr);
    void *o = (void *)1;
    printf("path nofx: %08lx %p\n", IDirectSoundBuffer8_GetObjectInPath(b, &GUID_DSFX_STANDARD_ECHO, 0, &IID_IDirectSoundFXEcho8, &o), o);
    const GUID *chain[4] = { &GUID_DSFX_STANDARD_GARGLE, echo, echo, &CLSID_FxProbe };
    setfx(b, "chain", 4, chain, 0);
    static const struct { const char *n; const GUID *g; DWORD i; const IID *iid; } q[] = {
        { "echo 0", &GUID_DSFX_STANDARD_ECHO, 0, &IID_IDirectSoundFXEcho8 }, { "echo 1", &GUID_DSFX_STANDARD_ECHO, 1, &IID_IDirectSoundFXEcho8 },
        { "echo 2", &GUID_DSFX_STANDARD_ECHO, 2, &IID_IDirectSoundFXEcho8 }, { "echo as gargle", &GUID_DSFX_STANDARD_ECHO, 0, &IID_IDirectSoundFXGargle8 },
        { "echo unknown", &GUID_DSFX_STANDARD_ECHO, 0, &IID_IUnknown }, { "echo mediaobject", &GUID_DSFX_STANDARD_ECHO, 0, &IID_IMediaObject_ },
        { "all 0", &GUID_All_Objects_, 0, &IID_IUnknown }, { "all 3", &GUID_All_Objects_, 3, &IID_IUnknown },
        { "all 4", &GUID_All_Objects_, 4, &IID_IUnknown }, { "all 1 echo", &GUID_All_Objects_, 1, &IID_IDirectSoundFXEcho8 },
        { "probe 0", &CLSID_FxProbe, 0, &IID_IMediaObject_ }, { "chorus 0", &GUID_DSFX_STANDARD_CHORUS, 0, &IID_IUnknown },
        { "buffer", &IID_IDirectSoundBuffer8, 0, &IID_IDirectSoundBuffer8 },
    };
    for (unsigned i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
        IUnknown *u = (IUnknown *)1;
        hr = IDirectSoundBuffer8_GetObjectInPath(b, q[i].g, q[i].i, q[i].iid, (void **)&u);
        printf("path %s: %08lx %s\n", q[i].n, hr, u == (IUnknown *)1 ? "untouched" : u ? "object" : "NULL");
        if (SUCCEEDED(hr) && u && u != (IUnknown *)1) IUnknown_Release(u);
    }
    printf("path null out: %08lx\n", IDirectSoundBuffer8_GetObjectInPath(b, &GUID_DSFX_STANDARD_ECHO, 0, &IID_IDirectSoundFXEcho8, NULL));
    /* identity: is the same object returned twice, and does it survive SetFX? */
    IUnknown *u1 = NULL, *u2 = NULL;
    IDirectSoundBuffer8_GetObjectInPath(b, &GUID_DSFX_STANDARD_ECHO, 0, &IID_IUnknown, (void **)&u1);
    IDirectSoundBuffer8_GetObjectInPath(b, &GUID_DSFX_STANDARD_ECHO, 0, &IID_IUnknown, (void **)&u2);
    printf("same object: %d\n", u1 == u2);
    IDirectSoundFXEcho8 *ec = NULL;
    IUnknown_QueryInterface(u1, &IID_IDirectSoundFXEcho8, (void **)&ec);
    if (u2) IUnknown_Release(u2);
    setfx(b, "replace", 1, &echo, 0);
    DSFXEcho ep;
    if (ec) {
        printf("old echo get: %08lx\n", IDirectSoundFXEcho_GetAllParameters(ec, &ep));
        printf("old echo release: %lu\n", IDirectSoundFXEcho_Release(ec));
    }
    if (u1) printf("old unknown release: %lu\n", IUnknown_Release(u1));
    IDirectSoundBuffer8_Release(b);
}

/* ------------------------------------------------------------------ the parameter interfaces */
typedef struct { IUnknownVtbl u; HRESULT (WINAPI *Set)(void *, const void *); HRESULT (WINAPI *Get)(void *, void *); } fxvtbl;

static void test_params(void)
{
    HRESULT hr;
    for (unsigned k = 0; k < NFX; k++) {
        const fxinfo *fx = &fxs[k];
        int bits = !strcmp(fx->name, "waves") ? 16 : 16;
        IDirectSoundBuffer8 *b = mkbuf(DSBCAPS_CTRLFX, 1, 2, 22050, bits, 1000, &hr);
        setfx(b, fx->name, 1, &fx->guid, 0);
        void **o = NULL;
        hr = IDirectSoundBuffer8_GetObjectInPath(b, fx->guid, 0, fx->iid, (void **)&o);
        if (FAILED(hr) || !o) { printf("  path %08lx\n", hr); IDirectSoundBuffer8_Release(b); continue; }
        const fxvtbl *vt = *(const fxvtbl **)o;
        DWORD p[16], p2[16];
        memset(p, 0xcc, sizeof(p));
        hr = vt->Get(o, p);
        printf("  defaults %08lx", hr); pparams(fx, p); printf("\n");
        printf("  get null %08lx set null %08lx\n", vt->Get(o, NULL), vt->Set(o, NULL));
        /* each field to its extremes and beyond (just the first few) */
        for (int f = 0; f < fx->nfields; f++) {
            static const float tries[] = { -1e9f, -100, -60, -1, 0, 0.0005f, 0.5f, 1, 2, 4, 7.5f, 20, 100, 1000, 5000, 9000, 20000, 1e9f };
            for (unsigned t = 0; t < sizeof(tries) / sizeof(tries[0]); t++) {
                memcpy(p2, p, sizeof(p2));
                if (fx->types[f] == 'f') memcpy(&p2[f], &tries[t], 4); else p2[f] = (DWORD)(LONG)tries[t];
                hr = vt->Set(o, p2);
                DWORD p3[16];
                memset(p3, 0xcc, sizeof(p3));
                vt->Get(o, p3);
                printf("  field %d = %g: %08lx ->", f, tries[t], hr); pparams(fx, p3); printf("\n");
                vt->Set(o, p);
            }
        }
        /* NaN */
        memcpy(p2, p, sizeof(p2));
        p2[0] = 0x7fc00000;
        hr = vt->Set(o, p2);
        memset(p2, 0xcc, sizeof(p2));
        vt->Get(o, p2);
        printf("  nan: %08lx ->", hr); pparams(fx, p2); printf("\n");
        static const struct { const char *n; const IID *iid; } qi[] = {
            { "IMediaObject", &IID_IMediaObject_ }, { "IMediaObjectInPlace", &IID_IMediaObjectInPlace_ },
            { "IMediaParams", &IID_IMediaParams_ }, { "IMediaParamInfo", &IID_IMediaParamInfo_ },
            { "IPersistStream", &IID_IPersistStream_ }, { "IDirectSoundBuffer8", &IID_IDirectSoundBuffer8 },
        };
        for (unsigned i = 0; i < sizeof(qi) / sizeof(qi[0]); i++) {
            IUnknown *u = NULL;
            hr = ((IUnknown *)o)->lpVtbl->QueryInterface((IUnknown *)o, qi[i].iid, (void **)&u);
            printf("  qi %s: %08lx\n", qi[i].n, hr);
            if (u) IUnknown_Release(u);
        }
        for (unsigned j = 0; j < NFX; j++) if (j != k) {
            IUnknown *u = NULL;
            hr = ((IUnknown *)o)->lpVtbl->QueryInterface((IUnknown *)o, fxs[j].iid, (void **)&u);
            if (hr != E_NOINTERFACE) printf("  qi %s: %08lx\n", fxs[j].name, hr);
            if (u) IUnknown_Release(u);
        }
        if (!strcmp(fx->name, "i3dl2")) {
            IDirectSoundFXI3DL2Reverb8 *r = (IDirectSoundFXI3DL2Reverb8 *)o;
            DWORD v = 0xdead; LONG l = 0xdead;
            printf("  getpreset %08lx %lu\n", IDirectSoundFXI3DL2Reverb_GetPreset(r, &v), v);
            for (DWORD pr = 0; pr <= DSFX_I3DL2_ENVIRONMENT_PRESET_PLATE + 1; pr++) {
                hr = IDirectSoundFXI3DL2Reverb_SetPreset(r, pr);
                DSFXI3DL2Reverb rp; memset(&rp, 0, sizeof(rp));
                IDirectSoundFXI3DL2Reverb_GetAllParameters(r, &rp);
                IDirectSoundFXI3DL2Reverb_GetPreset(r, &v);
                printf("  preset %lu: %08lx get %lu ->", pr, hr, v); pparams(fx, &rp); printf("\n");
            }
            printf("  getquality %08lx", r->lpVtbl->GetQuality(r, &l)); printf(" %ld\n", l);
            for (LONG qv = -1; qv <= 4; qv++) {
                hr = r->lpVtbl->SetQuality(r, qv);
                r->lpVtbl->GetQuality(r, &l);
                printf("  quality %ld: %08lx -> %ld\n", qv, hr, l);
            }
        }
        if (!strcmp(fx->name, "parameq")) {
            DSFXParamEq e = { 9000, 12, 0 };      /* above a third of 22050 */
            printf("  center 9000 @22050: %08lx", vt->Set(o, &e));
            vt->Get(o, &e); printf(" -> %g\n", e.fCenter);
        }
        ((IUnknown *)o)->lpVtbl->Release((IUnknown *)o);
        IDirectSoundBuffer8_Release(b);
    }
}

/* ------------------------------------------------------------------ the processed audio, through the probe */
static unsigned fnv(const BYTE *d, DWORD n) { unsigned h = 2166136261u; while (n--) h = (h ^ *d++) * 16777619u; return h; }

static void fill(BYTE *p, DWORD n, int bits, int tag)
{
    unsigned s = 12345;
    if (tag == 3) {
        float *f = (float *)p;
        for (DWORD i = 0; i < n / 4; i++) { s = s * 1103515245 + 12345; f[i] = (float)(sin(i * 0.031) * 0.6 + ((int)(s >> 16 & 0x7fff) - 16384) / 65536.0); }
    } else if (bits == 16) {
        short *v = (short *)p;
        for (DWORD i = 0; i < n / 2; i++) { s = s * 1103515245 + 12345; v[i] = (short)(sin(i * 0.031) * 18000 + ((int)(s >> 16 & 0x7fff) - 16384) / 2); }
    } else {
        for (DWORD i = 0; i < n; i++) { s = s * 1103515245 + 12345; p[i] = (BYTE)(128 + sin(i * 0.031) * 70 + ((int)(s >> 24) - 128) / 8); }
    }
}

static void readfile(const char *n, BYTE **d, DWORD *len)
{
    *d = NULL; *len = 0;
    HANDLE h = CreateFileA(n, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    *len = GetFileSize(h, NULL);
    *d = malloc(*len + 1);
    DWORD got = 0;
    ReadFile(h, *d, *len, &got, NULL);
    (*d)[got] = 0;
    *len = got;
    CloseHandle(h);
}

static void printlog(void)
{
    BYTE *d; DWORD n;
    readfile("fxlog.txt", &d, &n);
    if (!d) { printf("  (no probe log)\n"); return; }
    char *s = (char *)d, *e;
    while (*s) {
        e = strchr(s, '\n');
        if (e) *e = 0;
        if (!strncmp(s, "Process ", 8)) { if (getenv("FXTEST_SIZES")) printf("  probe: %s\n", s); }
        else if (!strncmp(s, "released after", 14)) printf("  probe: released\n");
        else printf("  probe: %s\n", s);
        if (!e) break;
        s = e + 1;
    }
    free(d);
}

static void run_data(const char *name, const GUID *g, const void *params, WORD tag, WORD ch, DWORD rate, WORD bits, int jump)
{
    char t[96]; sprintf(t, "data %s %u/%u/%lu/%u", name, tag, ch, rate, bits);
    if (getenv("FXTEST_ONLY") && !strstr(t, getenv("FXTEST_ONLY"))) return;   /* one run (FXTEST_SIZES: its Process calls) */
    DeleteFileA("fxlog.txt"); DeleteFileA("fxdata.raw");
    HRESULT hr;
    DWORD ms = 2000;
    IDirectSoundBuffer8 *b = mkbuf(DSBCAPS_CTRLFX | DSBCAPS_GETCURRENTPOSITION2, tag, ch, rate, bits, ms, &hr);
    if (!b) { printf("data %s: create %08lx\n", name, hr); return; }
    void *p; DWORD n;
    IDirectSoundBuffer8_Lock(b, 0, 0, &p, &n, NULL, NULL, DSBLOCK_ENTIREBUFFER);
    fill(p, n, bits, tag);
    unsigned before = fnv(p, n);
    IDirectSoundBuffer8_Unlock(b, p, n, NULL, 0);
    const GUID *chain[2] = { g, &CLSID_FxProbe };
    setfx(b, t, g ? 2 : 1, g ? chain : chain + 1, 0);
    if (g && params) {
        void **o = NULL;
        IDirectSoundBuffer8_GetObjectInPath(b, g, 0, &IID_IUnknown, (void **)&o);
        for (unsigned k = 0; k < NFX; k++) if (IsEqualGUID(fxs[k].guid, g)) {
            void **i = NULL;
            IUnknown_QueryInterface((IUnknown *)o, fxs[k].iid, (void **)&i);
            printf("  set %08lx\n", (*(const fxvtbl **)i)->Set(i, params));
            IUnknown_Release((IUnknown *)i);
        }
        IUnknown_Release((IUnknown *)o);
    }
    IDirectSoundBuffer8_Play(b, 0, 0, 0);
    if (jump) {
        Sleep(300);
        IDirectSoundBuffer8_SetCurrentPosition(b, 0);
    }
    Sleep(ms + 600);
    IDirectSoundBuffer8_Stop(b);
    IDirectSoundBuffer8_Lock(b, 0, 0, &p, &n, NULL, NULL, DSBLOCK_ENTIREBUFFER);
    printf("  app data %s\n", fnv(p, n) == before ? "unchanged" : "CHANGED");
    IDirectSoundBuffer8_Unlock(b, p, n, NULL, 0);
    IDirectSoundBuffer8_Release(b);
    BYTE *d; DWORD len;
    readfile("fxdata.raw", &d, &len);
    DWORD cmp = n * 3 / 4;           /* the first 1.5 s: written ahead well before the end */
    if (jump) {
        /* the stream restarts at 0 after the jump: find the second copy of the processed start (Discontinuity
         * makes it the same as the first) and compare from there */
        printf("  processed %s, first 1.5 s %08x\n", len >= cmp ? "enough" : "SHORT", len >= cmp ? fnv(d, cmp) : 0);
    } else printf("  processed %s, first 1.5 s %08x\n", len >= cmp ? "enough" : "SHORT", len >= cmp ? fnv(d, cmp) : 0);
    free(d);
    printlog();
}

/* the segments of the probe's data that follow each Discontinuity (FXPROBE_VERBOSE=1 logs the sizes) */
static void segments(DWORD block, DWORD rate)
{
    BYTE *log, *d; DWORD nl, nd;
    readfile("fxlog.txt", &log, &nl);
    readfile("fxdata.raw", &d, &nd);
    if (!log || !d) { printf("  (no probe data)\n"); free(log); free(d); return; }
    DWORD off = 0, starts[16], ns = 0;
    for (char *s = (char *)log; s && *s; ) {
        if (!strncmp(s, "Discontinuity", 13) && ns < 16) starts[ns++] = off;
        else if (!strncmp(s, "Process ", 8)) off += strtoul(s + 8, NULL, 10);
        s = strchr(s, '\n');
        if (s) s++;
    }
    DWORD want = (32 + rate * 3 / 10) * block;     /* the pre-roll and 300 ms */
    for (DWORD i = 0; i < ns; i++) {
        DWORD end = i + 1 < ns ? starts[i + 1] : nd;
        printf("  segment %lu: %s %08x\n", i, end - starts[i] >= want ? "enough" : "SHORT",
               end - starts[i] >= want ? fnv(d + starts[i], want) : 0);
    }
    free(log); free(d);
}

static void run_seq(const char *name, const GUID *g, const void *params, WORD tag, WORD ch, DWORD rate, WORD bits)
{
    DeleteFileA("fxlog.txt"); DeleteFileA("fxdata.raw");
    HRESULT hr;
    IDirectSoundBuffer8 *b = mkbuf(DSBCAPS_CTRLFX | DSBCAPS_GETCURRENTPOSITION2, tag, ch, rate, bits, 2000, &hr);
    if (!b) { printf("seq %s: create %08lx\n", name, hr); return; }
    void *p; DWORD n;
    IDirectSoundBuffer8_Lock(b, 0, 0, &p, &n, NULL, NULL, DSBLOCK_ENTIREBUFFER);
    fill(p, n, bits, tag);
    IDirectSoundBuffer8_Unlock(b, p, n, NULL, 0);
    const GUID *chain[2] = { g, &CLSID_FxProbe };
    char t[96]; sprintf(t, "seq %s %u/%u/%lu/%u", name, tag, ch, rate, bits);
    setfx(b, t, 2, chain, 0);
    if (params) {
        IUnknown *o = NULL;
        for (unsigned k = 0; k < NFX; k++) if (IsEqualGUID(fxs[k].guid, g)) {
            void **i = NULL;
            IDirectSoundBuffer8_GetObjectInPath(b, g, 0, fxs[k].iid, (void **)&i);
            printf("  set %08lx\n", (*(const fxvtbl **)i)->Set(i, params));
            IUnknown_Release((IUnknown *)i);
        }
        (void)o;
    }
    DWORD block = ch * bits / 8;
    IDirectSoundBuffer8_SetCurrentPosition(b, rate * 19 / 10 * block);    /* across the end of a looping buffer */
    IDirectSoundBuffer8_Play(b, 0, 0, DSBPLAY_LOOPING);
    Sleep(500);
    IDirectSoundBuffer8_SetCurrentPosition(b, rate * 7 / 10 * block);     /* a jump while playing */
    Sleep(500);
    IDirectSoundBuffer8_Stop(b);
    IDirectSoundBuffer8_SetCurrentPosition(b, rate / 4 * block);
    IDirectSoundBuffer8_Play(b, 0, 0, 0);                               /* again, not looping */
    Sleep(500);
    IDirectSoundBuffer8_Stop(b);
    DWORD st = 0;
    IDirectSoundBuffer8_GetStatus(b, &st);
    printf("  status %lx\n", st);
    IDirectSoundBuffer8_Release(b);
    segments(block, rate);
    printlog();
}

static void test_seq(void)
{
    DSFXEcho echo = { 40, 60, 120, 333, 1 };
    DSFXChorus chorus = { 60, 40, -30, 3, 0, 9, 1 };
    DSFXWavesReverb wv = { -2, -4, 1500, 0.3f };
    run_seq("echo", &GUID_DSFX_STANDARD_ECHO, &echo, 1, 2, 44100, 16);
    run_seq("chorus", &GUID_DSFX_STANDARD_CHORUS, &chorus, 1, 1, 22050, 8);
    run_seq("waves", &GUID_DSFX_WAVES_REVERB, &wv, 3, 2, 48000, 32);
}

static void test_data(void)
{
    DSFXEcho echo = { 40, 60, 120, 333, 1 };
    DSFXChorus chorus = { 60, 40, -30, 3, 0, 9, 1 };
    DSFXFlanger flanger = { 50, 80, 40, 2, 1, 3, 3 };
    DSFXDistortion dist = { -6, 40, 3000, 1200, 6000 };
    DSFXCompressor comp = { 6, 5, 300, -30, 8, 2 };
    DSFXParamEq eq = { 2000, 6, -9 };
    DSFXWavesReverb wv = { -2, -4, 1500, 0.3f };
    DSFXGargle gg = { 37, 1 };
    DSFXI3DL2Reverb i3 = { -500, -300, 1, 2.5f, 0.7f, -800, 0.02f, 300, 0.03f, 80, 60, 3000 };
    run_data("none", NULL, NULL, 1, 2, 44100, 16, 0);
    run_data("echo", &GUID_DSFX_STANDARD_ECHO, &echo, 1, 2, 44100, 16, 0);
    run_data("echo", &GUID_DSFX_STANDARD_ECHO, &echo, 1, 1, 22050, 8, 0);
    run_data("echo", &GUID_DSFX_STANDARD_ECHO, &echo, 3, 2, 48000, 32, 0);
    run_data("chorus", &GUID_DSFX_STANDARD_CHORUS, &chorus, 1, 2, 44100, 16, 0);
    run_data("flanger", &GUID_DSFX_STANDARD_FLANGER, &flanger, 1, 1, 44100, 16, 0);
    run_data("distortion", &GUID_DSFX_STANDARD_DISTORTION, &dist, 1, 2, 22050, 16, 0);
    run_data("compressor", &GUID_DSFX_STANDARD_COMPRESSOR, &comp, 1, 2, 44100, 16, 0);
    run_data("parameq", &GUID_DSFX_STANDARD_PARAMEQ, &eq, 3, 1, 44100, 32, 0);
    run_data("waves", &GUID_DSFX_WAVES_REVERB, &wv, 1, 2, 44100, 16, 0);
    run_data("waves", &GUID_DSFX_WAVES_REVERB, &wv, 3, 2, 44100, 32, 0);
    run_data("gargle", &GUID_DSFX_STANDARD_GARGLE, &gg, 1, 1, 11025, 8, 0);
    run_data("gargle defaults", &GUID_DSFX_STANDARD_GARGLE, NULL, 1, 2, 44100, 16, 0);
    run_data("i3dl2 defaults", &GUID_DSFX_STANDARD_I3DL2REVERB, NULL, 1, 2, 44100, 16, 0);
    /* (not 8-bit at half rate: its output depends on where the Process calls fall, which is timing in DirectSound) */
    run_data("i3dl2", &GUID_DSFX_STANDARD_I3DL2REVERB, &i3, 1, 1, 22050, 16, 0);
    run_data("i3dl2", &GUID_DSFX_STANDARD_I3DL2REVERB, &i3, 3, 1, 48000, 32, 0);
}

int main(int argc, char **argv)
{
    const char *sec = argc > 1 ? argv[1] : "all";
    CoInitialize(NULL);
    if (FAILED(DirectSoundCreate8(NULL, &ds, NULL))) { printf("no DirectSound\n"); return 1; }
    IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    if (!strcmp(sec, "all") || !strcmp(sec, "api")) test_api();
    if (!strcmp(sec, "all") || !strcmp(sec, "params")) test_params();
    if (!strcmp(sec, "all") || !strcmp(sec, "data")) test_data();
    if (!strcmp(sec, "all") || !strcmp(sec, "seq")) test_seq();
    if (!strcmp(sec, "none")) run_data("none", NULL, NULL, 1, 2, 44100, 16, 0);
    IDirectSound8_Release(ds);
    CoUninitialize();
    return 0;
}
