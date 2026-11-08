/* alprobe - DirectSound and EAX behaviour case by case, each on fresh objects, to compare reAL with ALchemy (and
 * Windows) line by line.  Where dsfuzz finds that two implementations part, this says exactly where and how.
 *
 *   alprobe.exe <section>      create ctrl lock play notify misc d3 eax eaxval
 *
 * One line per case: what was asked, the HRESULT, and what reads back.  Each section runs on its own objects (a run
 * script starts one process per section, so a crash costs one section).  A crash prints "CRASH <code> in <module>+<rva>
 * case <n>" and ends the process without a dialog. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <float.h>

#define P(...) printf(__VA_ARGS__)
static int s_case;                      /* the case running (for CRASH) */
static IDirectSound8 *s_ds8;            /* DirectSoundCreate8 */
static IDirectSound *s_ds7;             /* DirectSoundCreate */

/* ------------------------------------------------------------------ helpers */
static void fmt_make(WAVEFORMATEXTENSIBLE *e, WORD tag, WORD ch, WORD bits, DWORD rate)
{
    memset(e, 0, sizeof(*e));
    e->Format.wFormatTag = tag; e->Format.nChannels = ch; e->Format.wBitsPerSample = bits; e->Format.nSamplesPerSec = rate;
    e->Format.nBlockAlign = (WORD)(ch * bits / 8); e->Format.nAvgBytesPerSec = e->Format.nBlockAlign * rate;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        e->Format.cbSize = 22; e->Samples.wValidBitsPerSample = bits;
        e->dwChannelMask = ch == 1 ? 4 : ch == 2 ? 3 : ch == 3 ? 7 : ch == 4 ? 0x33 : ch == 6 ? 0x3f : ch == 8 ? 0x63f : 0;
        e->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
}

static HRESULT mk(IDirectSound *ds, DWORD flags, const WAVEFORMATEXTENSIBLE *e, DWORD bytes, DWORD dsize, IDirectSoundBuffer **out)
{
    DSBUFFERDESC d; memset(&d, 0, sizeof(d));
    d.dwSize = dsize; d.dwFlags = flags; d.dwBufferBytes = bytes; d.lpwfxFormat = e ? (WAVEFORMATEX *)&e->Format : NULL;
    *out = NULL;
    return IDirectSound_CreateSoundBuffer(ds, &d, out, NULL);
}

/* a plain buffer: 16-bit, `ch` channels, 22050 Hz, `bytes` long */
static IDirectSoundBuffer *mkb(IDirectSound *ds, DWORD flags, WORD ch, DWORD bytes)
{
    WAVEFORMATEXTENSIBLE e; fmt_make(&e, WAVE_FORMAT_PCM, ch, 16, 22050);
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = mk(ds, flags, &e, bytes, sizeof(DSBUFFERDESC), &b);
    if (FAILED(hr)) P("  (helper buffer flags %lx: %08lx)\n", flags, hr);
    return b;
}

static void caps_of(IDirectSoundBuffer *b)
{
    DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    HRESULT hr = IDirectSoundBuffer_GetCaps(b, &c);
    BYTE f[64]; memset(f, 0xcc, sizeof(f)); DWORD got = 0xdead;
    HRESULT h2 = IDirectSoundBuffer_GetFormat(b, (WAVEFORMATEX *)f, sizeof(f), &got);
    const WAVEFORMATEX *w = (const WAVEFORMATEX *)f;
    P(" caps %08lx flags %lx bytes %lu fmt %08lx got %lu tag %x ch %u rate %lu bits %u align %u avg %lu cb %u", hr, c.dwFlags,
      c.dwBufferBytes, h2, got, w->wFormatTag, w->nChannels, w->nSamplesPerSec, w->wBitsPerSample, w->nBlockAlign,
      w->nAvgBytesPerSec, w->cbSize);
}

static const char *fstr(float f, char *b)
{
    union { float f; DWORD u; } x = { f };
    if ((x.u & 0x7f800000) == 0x7f800000) sprintf(b, "<%08lx>", (unsigned long)x.u);
    else sprintf(b, "%.6g", (double)f);
    return b;
}

static void dump(const void *p, ULONG n)
{
    const DWORD *w = p;
    P(" [");
    for (ULONG k = 0; k < n / 4; k++) P(k ? " %08lx" : "%08lx", w[k]);
    P("]");
}

#define CASE(...) do { s_case++; P("%d ", s_case); P(__VA_ARGS__); } while (0)

/* ------------------------------------------------------------------ create */
static void sec_create(void)
{
    WAVEFORMATEXTENSIBLE e;
    IDirectSoundBuffer *b;
    HRESULT hr;
    /* 3D: channel counts by object, descriptor size and tag */
    static const struct { WORD tag, ch; } f3[] = { { 1, 1 }, { 1, 2 }, { 0xfffe, 1 }, { 0xfffe, 2 }, { 0xfffe, 3 },
                                                   { 0xfffe, 4 }, { 0xfffe, 6 }, { 0xfffe, 8 } };
    for (int o = 0; o < 2; o++)
        for (int sz = 0; sz < 2; sz++)
            for (unsigned k = 0; k < sizeof(f3) / sizeof(f3[0]); k++) {
                fmt_make(&e, f3[k].tag, f3[k].ch, 16, 22050);
                DWORD dsize = sz ? sizeof(DSBUFFERDESC1) : sizeof(DSBUFFERDESC);
                hr = mk(o ? s_ds7 : (IDirectSound *)s_ds8, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, &e, 22050 * 2 * f3[k].ch, dsize, &b);
                CASE("3d ds%d size %lu tag %x ch %u -> %08lx", o ? 7 : 8, dsize, f3[k].tag, f3[k].ch, hr);
                if (b) { caps_of(b); IDirectSoundBuffer_Release(b); }
                P("\n");
            }
    /* CTRLFX: object, size, formats, other flags; then SetFX and GetObjectInPath on it */
    static const struct { WORD tag, ch, bits; DWORD bytes, flags; const char *what; } fx[] = {
        { 1, 2, 16, 88200, 0, "16-bit stereo 0.5 s" }, { 1, 2, 16, 4096, 0, "16-bit stereo 4096 bytes" },
        { 1, 1, 8, 22050, 0, "8-bit mono 1 s" }, { 3, 2, 32, 176400, 0, "float stereo 1 s" },
        { 1, 2, 24, 132300, 0, "24-bit stereo 1 s" }, { 0xfffe, 6, 16, 264600, 0, "16-bit 5.1 1 s" },
        { 1, 2, 16, 88200, DSBCAPS_STATIC, "static" }, { 1, 2, 16, 88200, DSBCAPS_LOCHARDWARE, "LOCHARDWARE" },
        { 1, 2, 16, 88200, DSBCAPS_LOCDEFER, "LOCDEFER" }, { 1, 1, 16, 44100, DSBCAPS_CTRL3D, "3D mono" },
    };
    for (int o = 0; o < 2; o++)
        for (unsigned k = 0; k < sizeof(fx) / sizeof(fx[0]); k++) {
            fmt_make(&e, fx[k].tag, fx[k].ch, fx[k].bits, 22050);
            hr = mk(o ? s_ds7 : (IDirectSound *)s_ds8, DSBCAPS_CTRLFX | fx[k].flags, &e, fx[k].bytes, sizeof(DSBUFFERDESC), &b);
            CASE("fx ds%d %s -> %08lx", o ? 7 : 8, fx[k].what, hr);
            if (b) {
                caps_of(b);
                IDirectSoundBuffer8 *b8 = NULL;
                HRESULT h = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundBuffer8, (void **)&b8);
                P(" qi8 %08lx", h);
                if (b8) {
                    DSEFFECTDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.guidDSFXClass = GUID_DSFX_STANDARD_ECHO;
                    DWORD res = 0xdead;
                    h = IDirectSoundBuffer8_SetFX(b8, 1, &d, &res);
                    P(" SetFX(echo) %08lx res %lx", h, res);
                    IUnknown *u = NULL;
                    h = IDirectSoundBuffer8_GetObjectInPath(b8, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&u);
                    P(" path %08lx", h);
                    if (u) IUnknown_Release(u);
                    h = IDirectSoundBuffer8_SetFX(b8, 0, NULL, NULL);
                    P(" SetFX(none) %08lx", h);
                    IDirectSoundBuffer8_Release(b8);
                }
                IDirectSoundBuffer_Release(b);
            }
            P("\n");
        }
    /* plain PCM / float / extensible channel counts */
    static const WORD tags[] = { 1, 3, 0xfffe };
    for (unsigned t = 0; t < 3; t++)
        for (WORD ch = 1; ch <= 12; ch++) {
            fmt_make(&e, tags[t], ch, tags[t] == 3 ? 32 : 16, 22050);
            hr = mk((IDirectSound *)s_ds8, 0, &e, 22050 * 4 * ch, sizeof(DSBUFFERDESC), &b);
            CASE("channels tag %x ch %u -> %08lx", tags[t], ch, hr);
            if (b) { caps_of(b); IDirectSoundBuffer_Release(b); }
            P("\n");
        }
    /* rates */
    static const DWORD rates[] = { 1, 2, 99, 100, 200000, 200001, 384000, 1000000, 0x7fffffff };
    for (unsigned k = 0; k < sizeof(rates) / sizeof(rates[0]); k++) {
        fmt_make(&e, 1, 2, 16, rates[k]);
        hr = mk((IDirectSound *)s_ds8, DSBCAPS_CTRLFREQUENCY, &e, 65536, sizeof(DSBUFFERDESC), &b);
        CASE("rate %lu -> %08lx", rates[k], hr);
        if (b) { DWORD f = 0xdead; HRESULT h = IDirectSoundBuffer_GetFrequency(b, &f); P(" freq %08lx %lu", h, f); caps_of(b); IDirectSoundBuffer_Release(b); }
        P("\n");
    }
    /* sizes */
    static const DWORD sizes[] = { 0, 3, 4, 5, 6, 7, 8, 4095, 4097, DSBSIZE_MAX, DSBSIZE_MAX + 1 };
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        fmt_make(&e, 1, 2, 16, 22050);
        hr = mk((IDirectSound *)s_ds8, 0, &e, sizes[k], sizeof(DSBUFFERDESC), &b);
        CASE("size %lu -> %08lx", sizes[k], hr);
        if (b) { caps_of(b); IDirectSoundBuffer_Release(b); }
        P("\n");
    }
    /* LOCHARDWARE: made and released 40 times, then 40 kept */
    fmt_make(&e, 1, 2, 16, 22050);
    int first = -1;
    for (int k = 0; k < 40; k++) {
        hr = mk((IDirectSound *)s_ds8, DSBCAPS_LOCHARDWARE, &e, 8820, sizeof(DSBUFFERDESC), &b);
        if (FAILED(hr) && first < 0) first = k;
        if (b) IDirectSoundBuffer_Release(b);
    }
    CASE("hardware made and released 40 times: first refusal at %d (%08lx)\n", first, hr);
    IDirectSoundBuffer *keep[40] = { 0 };
    first = -1;
    for (int k = 0; k < 40; k++) {
        hr = mk((IDirectSound *)s_ds8, DSBCAPS_LOCHARDWARE, &e, 8820, sizeof(DSBUFFERDESC), &keep[k]);
        if (FAILED(hr) && first < 0) first = k;
    }
    CASE("hardware 40 kept: first refusal at %d (%08lx)", first, hr);
    hr = mk((IDirectSound *)s_ds8, 0, &e, 8820, sizeof(DSBUFFERDESC), &b);
    P(" then software %08lx", hr);
    if (b) IDirectSoundBuffer_Release(b);
    for (int k = 0; k < 40; k++) if (keep[k]) IDirectSoundBuffer_Release(keep[k]);
    hr = mk((IDirectSound *)s_ds8, DSBCAPS_LOCHARDWARE, &e, 8820, sizeof(DSBUFFERDESC), &b);
    P(" then hardware after releasing %08lx\n", hr);
    if (b) IDirectSoundBuffer_Release(b);
    /* a format the hardware check may look at, as software and hardware */
    static const struct { const char *what; int field; DWORD v; } odd[] = {
        { "exact", 0, 0 }, { "align 0", 1, 0 }, { "align 5", 1, 5 }, { "avg 0", 2, 0 }, { "avg +1", 2, 88201 }, { "cb 1", 3, 1 },
    };
    for (int hw = 0; hw < 2; hw++)
        for (unsigned k = 0; k < sizeof(odd) / sizeof(odd[0]); k++) {
            fmt_make(&e, 1, 2, 16, 22050);
            if (odd[k].field == 1) e.Format.nBlockAlign = (WORD)odd[k].v;
            if (odd[k].field == 2) e.Format.nAvgBytesPerSec = odd[k].v;
            if (odd[k].field == 3) e.Format.cbSize = (WORD)odd[k].v;
            hr = mk((IDirectSound *)s_ds8, hw ? DSBCAPS_LOCHARDWARE : DSBCAPS_LOCSOFTWARE, &e, 8820, sizeof(DSBUFFERDESC), &b);
            CASE("%s %s -> %08lx", hw ? "hardware" : "software", odd[k].what, hr);
            if (b) { caps_of(b); IDirectSoundBuffer_Release(b); }
            P("\n");
        }
}

/* ------------------------------------------------------------------ volume, pan, frequency */
static void sec_ctrl(void)
{
    static const LONG vols[] = { -10001, -10000, -9999, -1, 0, 1, 10000 };
    static const LONG pans[] = { -10001, -10000, -1, 0, 1, 10000, 10001 };
    static const DWORD freqs[] = { 0, 1, 99, 100, 101, 22050, 199999, 200000, 200001, 384000, 0xffffffff };
    for (int c = 0; c < 2; c++) {
        DWORD flags = c ? DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY : 0;
        for (int ch = 1; ch <= 2; ch++) {
            IDirectSoundBuffer *b = mkb((IDirectSound *)s_ds8, flags, (WORD)ch, 44100);
            if (!b) continue;
            for (unsigned k = 0; k < sizeof(vols) / sizeof(vols[0]); k++) {
                LONG g = 12345; HRESULT hr = IDirectSoundBuffer_SetVolume(b, vols[k]); HRESULT h2 = IDirectSoundBuffer_GetVolume(b, &g);
                CASE("flags %lx ch %d SetVolume(%ld) %08lx get %08lx %ld\n", flags, ch, vols[k], hr, h2, g);
            }
            for (unsigned k = 0; k < sizeof(pans) / sizeof(pans[0]); k++) {
                LONG g = 12345; HRESULT hr = IDirectSoundBuffer_SetPan(b, pans[k]); HRESULT h2 = IDirectSoundBuffer_GetPan(b, &g);
                CASE("flags %lx ch %d SetPan(%ld) %08lx get %08lx %ld\n", flags, ch, pans[k], hr, h2, g);
            }
            for (unsigned k = 0; k < sizeof(freqs) / sizeof(freqs[0]); k++) {
                DWORD g = 12345; HRESULT hr = IDirectSoundBuffer_SetFrequency(b, freqs[k]); HRESULT h2 = IDirectSoundBuffer_GetFrequency(b, &g);
                CASE("flags %lx ch %d SetFrequency(%lu) %08lx get %08lx %lu\n", flags, ch, freqs[k], hr, h2, g);
            }
            CASE("flags %lx ch %d GetVolume(NULL) %08lx GetPan(NULL) %08lx GetFrequency(NULL) %08lx\n", flags, ch,
                 IDirectSoundBuffer_GetVolume(b, NULL), IDirectSoundBuffer_GetPan(b, NULL), IDirectSoundBuffer_GetFrequency(b, NULL));
            IDirectSoundBuffer_Release(b);
        }
    }
    /* 3D buffers: pan */
    WAVEFORMATEXTENSIBLE e; fmt_make(&e, 1, 1, 16, 22050);
    IDirectSoundBuffer *b;
    if (SUCCEEDED(mk((IDirectSound *)s_ds8, DSBCAPS_CTRL3D | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME, &e, 44100, sizeof(DSBUFFERDESC), &b))) {
        LONG g = 12345; HRESULT hr = IDirectSoundBuffer_SetPan(b, 5000), h2 = IDirectSoundBuffer_GetPan(b, &g);
        CASE("3D with CTRLPAN SetPan(5000) %08lx get %08lx %ld\n", hr, h2, g);
        IDirectSoundBuffer_Release(b);
    } else CASE("3D with CTRLPAN refused\n");
    /* the primary buffer */
    IDirectSoundBuffer *p = NULL;
    DSBUFFERDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(s_ds8, &d, &p, NULL);
    CASE("primary -> %08lx\n", hr);
    if (p) {
        for (unsigned k = 0; k < sizeof(vols) / sizeof(vols[0]); k++) {
            LONG g = 12345; HRESULT a = IDirectSoundBuffer_SetVolume(p, vols[k]), c = IDirectSoundBuffer_GetVolume(p, &g);
            CASE("primary SetVolume(%ld) %08lx get %08lx %ld\n", vols[k], a, c, g);
        }
        for (unsigned k = 0; k < sizeof(pans) / sizeof(pans[0]); k++) {
            LONG g = 12345; HRESULT a = IDirectSoundBuffer_SetPan(p, pans[k]), c = IDirectSoundBuffer_GetPan(p, &g);
            CASE("primary SetPan(%ld) %08lx get %08lx %ld\n", pans[k], a, c, g);
        }
        DWORD g = 12345; HRESULT a = IDirectSoundBuffer_SetFrequency(p, 22050), c = IDirectSoundBuffer_GetFrequency(p, &g);
        CASE("primary SetFrequency(22050) %08lx get %08lx %lu\n", a, c, g);
        IDirectSoundBuffer_Release(p);
    }
}

/* ------------------------------------------------------------------ Lock / Unlock */
static void sec_lock(void)
{
    IDirectSoundBuffer *b = mkb((IDirectSound *)s_ds8, 0, 2, 4096);
    if (!b) return;
    static const DWORD offs[] = { 0, 3, 4, 2048, 4092, 4095, 4096, 4097, 0xffffffff };
    static const DWORD lens[] = { 0, 1, 3, 4, 2048, 4096, 4097, 8192 };
    static const DWORD flg[] = { 0, DSBLOCK_ENTIREBUFFER, DSBLOCK_FROMWRITECURSOR, 4 };
    for (unsigned f = 0; f < 4; f++)
        for (unsigned o = 0; o < sizeof(offs) / sizeof(offs[0]); o++)
            for (unsigned l = 0; l < sizeof(lens) / sizeof(lens[0]); l++) {
                if (f && (o || l != 1)) continue;          /* the flags once each */
                for (int n2 = 0; n2 < 2; n2++) {
                    void *p1 = (void *)1, *p2 = (void *)1; DWORD b1 = 0xdead, b2 = 0xdead;
                    HRESULT hr = IDirectSoundBuffer_Lock(b, offs[o], lens[l], &p1, &b1, n2 ? NULL : &p2, n2 ? NULL : &b2, flg[f]);
                    CASE("Lock(%lu, %lu, %s, %lx) %08lx p1 %s b1 %lx p2 %s b2 %lx", offs[o], lens[l], n2 ? "no p2" : "p2", flg[f], hr,
                         p1 == (void *)1 ? "untouched" : p1 ? "set" : "NULL", b1,
                         n2 ? "-" : p2 == (void *)1 ? "untouched" : p2 ? "set" : "NULL", n2 ? 0 : b2);
                    if (SUCCEEDED(hr)) {
                        HRESULT u = IDirectSoundBuffer_Unlock(b, p1, b1, n2 ? NULL : p2, n2 ? 0 : b2);
                        P(" unlock %08lx", u);
                    }
                    P("\n");
                }
            }
    /* Unlock variants on a Lock(0, 4096) */
    static const char *const how[] = { "as locked", "b1 - 1", "b1 + 1", "b1 0", "p1 NULL", "p1 + 4", "twice", "never locked" };
    for (int h = 0; h < 8; h++) {
        void *p1 = NULL, *p2 = NULL; DWORD b1 = 0, b2 = 0;
        HRESULT hr = h == 7 ? S_OK : IDirectSoundBuffer_Lock(b, 1024, 4096, &p1, &b1, &p2, &b2, 0);
        BYTE local[16];
        void *q = h == 4 ? NULL : h == 5 ? (BYTE *)p1 + 4 : h == 7 ? local : p1;
        DWORD n = h == 1 ? b1 - 1 : h == 2 ? b1 + 1 : h == 3 ? 0 : b1;
        HRESULT u = IDirectSoundBuffer_Unlock(b, q, n, h == 7 ? NULL : p2, h == 7 ? 0 : b2);
        HRESULT u2 = h == 6 ? IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2) : 0;
        CASE("Unlock %s: lock %08lx unlock %08lx%s", how[h], hr, u, h == 6 ? "" : "\n");
        if (h == 6) P(" again %08lx\n", u2);
        if (FAILED(u) && h != 7) IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
    }
    /* two Locks open */
    void *a1, *a2, *c1, *c2; DWORD x1, x2, y1, y2;
    HRESULT h1 = IDirectSoundBuffer_Lock(b, 0, 2048, &a1, &x1, &a2, &x2, 0);
    HRESULT h2 = IDirectSoundBuffer_Lock(b, 1024, 2048, &c1, &y1, &c2, &y2, 0);
    CASE("two Locks, overlapping: %08lx %08lx", h1, h2);
    if (SUCCEEDED(h2)) P(" unlock second %08lx", IDirectSoundBuffer_Unlock(b, c1, y1, c2, y2));
    if (SUCCEEDED(h1)) P(" unlock first %08lx", IDirectSoundBuffer_Unlock(b, a1, x1, a2, x2));
    P("\n");
    IDirectSoundBuffer_Release(b);
}

/* ------------------------------------------------------------------ Play */
static void sec_play(void)
{
    static const DWORD pflags[] = { 0, DSBPLAY_LOOPING, DSBPLAY_LOCHARDWARE, DSBPLAY_LOCSOFTWARE, DSBPLAY_TERMINATEBY_TIME,
                                    DSBPLAY_TERMINATEBY_DISTANCE, DSBPLAY_TERMINATEBY_PRIORITY, 0x80, 0xffffffff };
    static const DWORD prios[] = { 0, 1, 0xffffffff };
    for (int defer = 0; defer < 2; defer++)
        for (unsigned f = 0; f < sizeof(pflags) / sizeof(pflags[0]); f++)
            for (unsigned p = 0; p < 3; p++)
                for (DWORD res = 0; res < 2; res++) {
                    if (res && (p || f)) continue;
                    IDirectSoundBuffer *b = mkb((IDirectSound *)s_ds8, defer ? DSBCAPS_LOCDEFER : 0, 2, 88200);
                    if (!b) return;
                    HRESULT hr = IDirectSoundBuffer_Play(b, res, prios[p], pflags[f] | DSBPLAY_LOOPING);
                    DWORD st = 0xdead; HRESULT h2 = IDirectSoundBuffer_GetStatus(b, &st);
                    DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c); IDirectSoundBuffer_GetCaps(b, &c);
                    CASE("%s Play(%lu, %lu, %lx | LOOPING) %08lx status %08lx %lx caps %lx", defer ? "LOCDEFER" : "plain", res,
                         prios[p], pflags[f], hr, h2, st, c.dwFlags);
                    P(" stop %08lx\n", IDirectSoundBuffer_Stop(b));
                    IDirectSoundBuffer_Release(b);
                }
    /* Play while playing, Stop while stopped, Play with no PRIORITY level */
    IDirectSoundBuffer *b = mkb((IDirectSound *)s_ds8, 0, 2, 88200);
    if (b) {
        HRESULT a = IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING), c = IDirectSoundBuffer_Play(b, 0, 0, 0);
        DWORD st = 0; IDirectSoundBuffer_GetStatus(b, &st);
        CASE("Play looping then once: %08lx %08lx status %lx", a, c, st & ~DSBSTATUS_PLAYING);
        a = IDirectSoundBuffer_Stop(b); c = IDirectSoundBuffer_Stop(b);
        P(" Stop twice %08lx %08lx\n", a, c);
        IDirectSoundBuffer_Release(b);
    }
}

/* ------------------------------------------------------------------ notifications */
static void sec_notify(void)
{
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (int ctl = 0; ctl < 2; ctl++) {
        IDirectSoundBuffer *b = mkb((IDirectSound *)s_ds8, ctl ? DSBCAPS_CTRLPOSITIONNOTIFY : 0, 2, 8192);
        if (!b) continue;
        IDirectSoundNotify *n = NULL;
        HRESULT hr = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&n);
        CASE("CTRLPOSITIONNOTIFY %d: QueryInterface(notify) %08lx\n", ctl, hr);
        if (!n) { IDirectSoundBuffer_Release(b); continue; }
        DSBPOSITIONNOTIFY p[4];
        static const struct { const char *what; int n; DWORD off[3]; int nullptr, nullev; } c[] = {
            { "none, NULL", 0, { 0 }, 1, 0 }, { "none, array", 0, { 0 }, 0, 0 }, { "one, NULL array", 1, { 0 }, 1, 0 },
            { "at 0", 1, { 0 }, 0, 0 }, { "at size - 1", 1, { 8191 }, 0, 0 }, { "at size", 1, { 8192 }, 0, 0 },
            { "at size + 1", 1, { 8193 }, 0, 0 }, { "OFFSETSTOP", 1, { DSBPN_OFFSETSTOP }, 0, 0 },
            { "not sorted", 3, { 4000, 100, 6000 }, 0, 0 }, { "same twice", 2, { 100, 100 }, 0, 0 },
            { "no event", 1, { 100 }, 0, 1 }, { "odd offset", 1, { 101 }, 0, 0 },
        };
        for (unsigned k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
            for (int i = 0; i < c[k].n; i++) { p[i].dwOffset = c[k].off[i]; p[i].hEventNotify = c[k].nullev ? NULL : ev; }
            hr = IDirectSoundNotify_SetNotificationPositions(n, c[k].n, c[k].nullptr ? NULL : p);
            CASE("ctl %d notify %s: %08lx\n", ctl, c[k].what, hr);
        }
        DSBPOSITIONNOTIFY *many = calloc(100001, sizeof(*many));     /* (a real array: an implementation may read it all) */
        if (many) {
            for (int i = 0; i < 100001; i++) { many[i].dwOffset = 4 * (DWORD)(i % 2048); many[i].hEventNotify = ev; }
            hr = IDirectSoundNotify_SetNotificationPositions(n, 100001, many);
            CASE("ctl %d notify 100001 positions: %08lx\n", ctl, hr);
            free(many);
        }
        IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
        p[0].dwOffset = 100; p[0].hEventNotify = ev;
        hr = IDirectSoundNotify_SetNotificationPositions(n, 1, p);
        CASE("ctl %d notify while playing: %08lx\n", ctl, hr);
        IDirectSoundBuffer_Stop(b);
        IDirectSoundNotify_Release(n);
        IDirectSoundBuffer_Release(b);
    }
    CloseHandle(ev);
}

/* ------------------------------------------------------------------ the rest of the objects' methods */
static void sec_misc(void)
{
    HRESULT hr;
    CASE("ds8 Initialize again %08lx, ds7 %08lx\n", IDirectSound8_Initialize(s_ds8, NULL), IDirectSound_Initialize(s_ds7, NULL));
    IDirectSound8 *fresh = NULL;
    hr = CoCreateInstance(&CLSID_DirectSound8, NULL, CLSCTX_INPROC_SERVER, &IID_IDirectSound8, (void **)&fresh);
    CASE("CoCreateInstance(DirectSound8) %08lx", hr);
    if (fresh) {
        DSCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
        P(" GetCaps before Initialize %08lx", IDirectSound8_GetCaps(fresh, &c));
        P(" Initialize %08lx", IDirectSound8_Initialize(fresh, NULL));
        P(" again %08lx", IDirectSound8_Initialize(fresh, NULL));
        IDirectSound8_Release(fresh);
    }
    P("\n");
    /* cooperative levels */
    static const DWORD lv[] = { 0, DSSCL_NORMAL, DSSCL_PRIORITY, DSSCL_EXCLUSIVE, DSSCL_WRITEPRIMARY, 5, 0x100 };
    for (unsigned k = 0; k < sizeof(lv) / sizeof(lv[0]); k++)
        for (int w = 0; w < 2; w++) {
            hr = IDirectSound8_SetCooperativeLevel(s_ds8, w ? GetDesktopWindow() : NULL, lv[k]);
            CASE("SetCooperativeLevel(%s, %lu) %08lx\n", w ? "desktop" : "NULL", lv[k], hr);
        }
    IDirectSound8_SetCooperativeLevel(s_ds8, GetDesktopWindow(), DSSCL_NORMAL);
    CASE("Compact at NORMAL %08lx", IDirectSound8_Compact(s_ds8));
    IDirectSound8_SetCooperativeLevel(s_ds8, GetDesktopWindow(), DSSCL_PRIORITY);
    P(" at PRIORITY %08lx\n", IDirectSound8_Compact(s_ds8));
    /* speaker config */
    DWORD orig = 0; HRESULT hsc = IDirectSound8_GetSpeakerConfig(s_ds8, &orig);
    static const DWORD sc[] = { DSSPEAKER_HEADPHONE, DSSPEAKER_MONO, DSSPEAKER_QUAD, DSSPEAKER_STEREO, DSSPEAKER_SURROUND,
                                DSSPEAKER_5POINT1, DSSPEAKER_7POINT1, 9, 10, 0xff, DSSPEAKER_COMBINED(DSSPEAKER_STEREO, 5),
                                DSSPEAKER_COMBINED(DSSPEAKER_STEREO, 181), 0 };
    for (unsigned k = 0; k < sizeof(sc) / sizeof(sc[0]); k++) {
        DWORD g = 0xdead;
        hr = IDirectSound8_SetSpeakerConfig(s_ds8, sc[k]);
        HRESULT h2 = IDirectSound8_GetSpeakerConfig(s_ds8, &g);
        CASE("SetSpeakerConfig(%lx) %08lx get %08lx %s\n", sc[k], hr, h2, g == orig ? "unchanged" : g == sc[k] ? "as set" : "other");
    }
    if (SUCCEEDED(hsc)) IDirectSound8_SetSpeakerConfig(s_ds8, orig);   /* (back as it was: Windows 7's DirectSound keeps it in the registry) */
    /* caps sizes */
    static const DWORD cs[] = { 0, sizeof(DSCAPS) - 1, sizeof(DSCAPS), sizeof(DSCAPS) + 1 };
    for (unsigned k = 0; k < 4; k++) {
        DSCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = cs[k];
        CASE("ds GetCaps(size %lu) %08lx\n", cs[k], IDirectSound8_GetCaps(s_ds8, (DSCAPS *)&c));
    }
    IDirectSoundBuffer *b = mkb((IDirectSound *)s_ds8, 0, 2, 8192);
    if (b) {
        static const DWORD bs[] = { 0, sizeof(DSBCAPS) - 1, sizeof(DSBCAPS), sizeof(DSBCAPS) + 1 };
        for (unsigned k = 0; k < 4; k++) {
            DSBCAPS c[2]; memset(c, 0, sizeof(c)); c[0].dwSize = bs[k];
            CASE("buffer GetCaps(size %lu) %08lx\n", bs[k], IDirectSoundBuffer_GetCaps(b, c));
        }
        WAVEFORMATEXTENSIBLE e; fmt_make(&e, 1, 1, 8, 11025);
        CASE("secondary SetFormat %08lx", IDirectSoundBuffer_SetFormat(b, &e.Format));
        caps_of(b); P("\n");
        CASE("secondary Initialize %08lx\n", IDirectSoundBuffer_Initialize(b, (IDirectSound *)s_ds8, NULL));
        IDirectSoundBuffer8 *b8 = NULL;
        IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundBuffer8, (void **)&b8);
        if (b8) {
            DWORD r = 0xdead;
            CASE("AcquireResources on a plain buffer %08lx %lx\n", IDirectSoundBuffer8_AcquireResources(b8, 0, 0, &r), r);
            IDirectSoundBuffer8_Release(b8);
        }
        IDirectSoundBuffer *dup = NULL;
        CASE("Duplicate plain %08lx", IDirectSound8_DuplicateSoundBuffer(s_ds8, b, &dup));
        if (dup) { caps_of(dup); IDirectSoundBuffer_Release(dup); }
        P("\n");
        dup = NULL;
        CASE("Duplicate through ds7 %08lx\n", IDirectSound_DuplicateSoundBuffer(s_ds7, b, &dup));
        if (dup) IDirectSoundBuffer_Release(dup);
        IDirectSoundBuffer_Release(b);
    }
    /* the primary buffer */
    IDirectSoundBuffer *p = NULL;
    DSBUFFERDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwFlags = DSBCAPS_PRIMARYBUFFER;
    hr = IDirectSound8_CreateSoundBuffer(s_ds8, &d, &p, NULL);
    CASE("primary %08lx", hr);
    if (p) {
        caps_of(p);
        IDirectSoundBuffer *dup = NULL;
        P(" Duplicate %08lx", IDirectSound8_DuplicateSoundBuffer(s_ds8, p, &dup));
        if (dup) IDirectSoundBuffer_Release(dup);
        DWORD st = 0xdead;
        P(" GetStatus %08lx %lx", IDirectSoundBuffer_GetStatus(p, &st), st);
        void *p1, *p2; DWORD b1, b2;
        hr = IDirectSoundBuffer_Lock(p, 0, 64, &p1, &b1, &p2, &b2, 0);
        P(" Lock %08lx", hr);
        if (SUCCEEDED(hr)) IDirectSoundBuffer_Unlock(p, p1, b1, p2, b2);
        P(" Play %08lx", IDirectSoundBuffer_Play(p, 0, 0, 0));
        P(" Play looping %08lx", IDirectSoundBuffer_Play(p, 0, 0, DSBPLAY_LOOPING));
        P(" Stop %08lx", IDirectSoundBuffer_Stop(p));
        WAVEFORMATEXTENSIBLE e; fmt_make(&e, 1, 2, 16, 44100);
        P(" SetFormat %08lx", IDirectSoundBuffer_SetFormat(p, &e.Format));
        caps_of(p);
        IDirectSoundBuffer_Release(p);
    }
    P("\n");
    /* interfaces */
    static const struct { const GUID *iid; const char *name; } q[] = {
        { &IID_IUnknown, "IUnknown" }, { &IID_IDirectSoundBuffer, "Buffer" }, { &IID_IDirectSoundBuffer8, "Buffer8" },
        { &IID_IDirectSound3DBuffer, "3DBuffer" }, { &IID_IDirectSound3DListener, "Listener" }, { &IID_IDirectSoundNotify, "Notify" },
        { &IID_IKsPropertySet, "KsPropertySet" }, { &IID_IDirectSound, "DirectSound" }, { &IID_IDirectSound8, "DirectSound8" },
    };
    static const struct { DWORD flags; const char *what; } bt[] = {
        { 0, "plain" }, { DSBCAPS_CTRL3D, "3D" }, { DSBCAPS_PRIMARYBUFFER, "primary" }, { DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, "primary 3D" },
    };
    for (unsigned t = 0; t < 4; t++)
        for (int o = 0; o < 2; o++) {
            IDirectSoundBuffer *x = NULL;
            WAVEFORMATEXTENSIBLE e; fmt_make(&e, 1, 1, 16, 22050);
            hr = mk(o ? s_ds7 : (IDirectSound *)s_ds8, bt[t].flags, bt[t].flags & DSBCAPS_PRIMARYBUFFER ? NULL : &e,
                    bt[t].flags & DSBCAPS_PRIMARYBUFFER ? 0 : 8192, sizeof(DSBUFFERDESC), &x);
            CASE("ds%d %s %08lx:", o ? 7 : 8, bt[t].what, hr);
            if (x) {
                for (unsigned k = 0; k < sizeof(q) / sizeof(q[0]); k++) {
                    IUnknown *u = NULL;
                    HRESULT h = IDirectSoundBuffer_QueryInterface(x, q[k].iid, (void **)&u);
                    P(" %s %s", q[k].name, SUCCEEDED(h) ? "yes" : "no");
                    if (FAILED(h)) P("(%08lx)", h);
                    if (u) IUnknown_Release(u);
                }
                IDirectSoundBuffer_Release(x);
            }
            P("\n");
        }
}

/* ------------------------------------------------------------------ 3D buffer and listener */
static void sec_d3(void)
{
    WAVEFORMATEXTENSIBLE e; fmt_make(&e, 1, 1, 16, 22050);
    IDirectSoundBuffer *b = NULL;
    if (FAILED(mk((IDirectSound *)s_ds8, DSBCAPS_CTRL3D, &e, 44100, sizeof(DSBUFFERDESC), &b))) return;
    IDirectSound3DBuffer *d = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&d);
    if (!d) { IDirectSoundBuffer_Release(b); return; }
    char s1[32], s2[32];
    static const float dv[] = { -1.0f, 0.0f, 1e-30f, 0.5f, 1.0f, 1e9f, 1e30f, FLT_MAX };
    for (unsigned k = 0; k < sizeof(dv) / sizeof(dv[0]); k++) {
        float g = -7, h = -7;
        HRESULT a = IDirectSound3DBuffer_SetMinDistance(d, dv[k], DS3D_IMMEDIATE); IDirectSound3DBuffer_GetMinDistance(d, &g);
        HRESULT c = IDirectSound3DBuffer_SetMaxDistance(d, dv[k], DS3D_IMMEDIATE); IDirectSound3DBuffer_GetMaxDistance(d, &h);
        CASE("min/max %s: SetMinDistance %08lx get %s, SetMaxDistance %08lx get %s\n", fstr(dv[k], s1), a, fstr(g, s2), c, fstr(h, s1));
    }
    IDirectSound3DBuffer_SetMinDistance(d, 1.0f, DS3D_IMMEDIATE);
    IDirectSound3DBuffer_SetMaxDistance(d, 1e9f, DS3D_IMMEDIATE);
    { float g; HRESULT a = IDirectSound3DBuffer_SetMinDistance(d, 100.0f, 0); IDirectSound3DBuffer_SetMaxDistance(d, 10.0f, 0);
      IDirectSound3DBuffer_GetMaxDistance(d, &g);
      CASE("min 100 then max 10: %08lx max reads %s\n", a, fstr(g, s1)); }
    static const DWORD ang[][2] = { { 0, 0 }, { 360, 360 }, { 361, 360 }, { 90, 45 }, { 0, 361 }, { 0xffffffff, 0 } };
    for (unsigned k = 0; k < 6; k++) {
        DWORD i = 7, o = 7;
        HRESULT a = IDirectSound3DBuffer_SetConeAngles(d, ang[k][0], ang[k][1], DS3D_IMMEDIATE);
        IDirectSound3DBuffer_GetConeAngles(d, &i, &o);
        CASE("SetConeAngles(%lu, %lu) %08lx get %lu %lu\n", ang[k][0], ang[k][1], a, i, o);
    }
    static const LONG cv[] = { -10001, -10000, 0, 1 };
    for (unsigned k = 0; k < 4; k++) {
        LONG g = 7; HRESULT a = IDirectSound3DBuffer_SetConeOutsideVolume(d, cv[k], DS3D_IMMEDIATE);
        IDirectSound3DBuffer_GetConeOutsideVolume(d, &g);
        CASE("SetConeOutsideVolume(%ld) %08lx get %ld\n", cv[k], a, g);
    }
    static const DWORD md[] = { 0, 1, 2, 3, 0xffffffff };
    for (unsigned k = 0; k < 5; k++) {
        DWORD g = 7; HRESULT a = IDirectSound3DBuffer_SetMode(d, md[k], DS3D_IMMEDIATE); IDirectSound3DBuffer_GetMode(d, &g);
        CASE("SetMode(%lu) %08lx get %lu\n", md[k], a, g);
    }
    static const float ov[][3] = { { 0, 0, 0 }, { 0, 0, 2 }, { 1e-30f, 0, 0 }, { 3, 4, 0 } };
    for (unsigned k = 0; k < 4; k++) {
        D3DVECTOR g = { 7, 7, 7 };
        HRESULT a = IDirectSound3DBuffer_SetConeOrientation(d, ov[k][0], ov[k][1], ov[k][2], DS3D_IMMEDIATE);
        IDirectSound3DBuffer_GetConeOrientation(d, &g);
        CASE("SetConeOrientation(%g,%g,%g) %08lx get %g,%g,%g\n", ov[k][0], ov[k][1], ov[k][2], a, g.x, g.y, g.z);
    }
    static const DWORD ap[] = { 2, 3, 0xffffffff };
    for (unsigned k = 0; k < 3; k++) {
        D3DVECTOR g = { 7, 7, 7 };
        HRESULT a = IDirectSound3DBuffer_SetPosition(d, 1, 2, 3, ap[k]);
        IDirectSound3DBuffer_GetPosition(d, &g);
        CASE("SetPosition(1,2,3, apply %lx) %08lx get %g,%g,%g\n", ap[k], a, g.x, g.y, g.z);
    }
    static const DWORD sz[] = { 0, sizeof(DS3DBUFFER) - 1, sizeof(DS3DBUFFER) + 1 };
    for (unsigned k = 0; k < 3; k++) {
        DS3DBUFFER p; memset(&p, 0, sizeof(p)); p.dwSize = sz[k];
        HRESULT g = IDirectSound3DBuffer_GetAllParameters(d, &p);
        p.dwSize = sz[k]; p.flMinDistance = 1; p.flMaxDistance = 2; p.dwInsideConeAngle = p.dwOutsideConeAngle = 360; p.vConeOrientation.z = 1;
        HRESULT s = IDirectSound3DBuffer_SetAllParameters(d, &p, DS3D_IMMEDIATE);
        CASE("3D AllParameters size %lu: get %08lx set %08lx\n", sz[k], g, s);
    }
    /* the listener */
    IDirectSoundBuffer *p = NULL;
    DSBUFFERDESC pd; memset(&pd, 0, sizeof(pd)); pd.dwSize = sizeof(pd); pd.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D;
    if (SUCCEEDED(IDirectSound8_CreateSoundBuffer(s_ds8, &pd, &p, NULL))) {
        IDirectSound3DListener *l = NULL;
        IDirectSoundBuffer_QueryInterface(p, &IID_IDirectSound3DListener, (void **)&l);
        if (l) {
            static const float lv[] = { -1.0f, 0.0f, 1e-30f, 0.5f, 1.0f, 10.0f, 10.5f, 1e30f };
            for (unsigned k = 0; k < sizeof(lv) / sizeof(lv[0]); k++) {
                float a = -7, c = -7, f = -7;
                HRESULT h1 = IDirectSound3DListener_SetDistanceFactor(l, lv[k], DS3D_IMMEDIATE); IDirectSound3DListener_GetDistanceFactor(l, &a);
                HRESULT h2 = IDirectSound3DListener_SetDopplerFactor(l, lv[k], DS3D_IMMEDIATE); IDirectSound3DListener_GetDopplerFactor(l, &c);
                HRESULT h3 = IDirectSound3DListener_SetRolloffFactor(l, lv[k], DS3D_IMMEDIATE); IDirectSound3DListener_GetRolloffFactor(l, &f);
                CASE("listener %s: distance %08lx %s", fstr(lv[k], s1), h1, fstr(a, s2));
                P(" doppler %08lx %s", h2, fstr(c, s2));
                P(" rolloff %08lx %s\n", h3, fstr(f, s2));
                IDirectSound3DListener_SetDistanceFactor(l, 1, DS3D_IMMEDIATE);
                IDirectSound3DListener_SetDopplerFactor(l, 1, DS3D_IMMEDIATE);
                IDirectSound3DListener_SetRolloffFactor(l, 1, DS3D_IMMEDIATE);
            }
            static const float of[][6] = { { 0, 0, 1, 0, 1, 0 }, { 0, 0, 1, 0, 0, 1 }, { 0, 0, 0, 0, 1, 0 }, { 0, 0, 2, 0, 3, 0 }, { 1, 0, 1, 0, 1, 0 } };
            for (unsigned k = 0; k < 5; k++) {
                D3DVECTOR f = { 7, 7, 7 }, t = { 7, 7, 7 };
                HRESULT h = IDirectSound3DListener_SetOrientation(l, of[k][0], of[k][1], of[k][2], of[k][3], of[k][4], of[k][5], DS3D_IMMEDIATE);
                IDirectSound3DListener_GetOrientation(l, &f, &t);
                CASE("SetOrientation(%g,%g,%g / %g,%g,%g) %08lx get %g,%g,%g / %g,%g,%g\n", of[k][0], of[k][1], of[k][2], of[k][3],
                     of[k][4], of[k][5], h, f.x, f.y, f.z, t.x, t.y, t.z);
            }
            /* the listener's apply values (the buffer's: SetPosition above): what Get reads straight after, then after a
             * commit.  0 and 1 for reference; 2, 3 and 0xffffffff are what the API leaves undefined */
            static const DWORD lap[] = { 0, 1, 2, 3, 0xffffffff };
            for (unsigned k = 0; k < sizeof(lap) / sizeof(lap[0]); k++) {
                D3DVECTOR g = { 7, 7, 7 }, c = { 7, 7, 7 };
                float df = -7, dc = -7;
                IDirectSound3DListener_SetPosition(l, 0, 0, 0, DS3D_IMMEDIATE);
                IDirectSound3DListener_SetDistanceFactor(l, 1, DS3D_IMMEDIATE);
                HRESULT h = IDirectSound3DListener_SetPosition(l, 4, 5, 6, lap[k]);
                HRESULT h2 = IDirectSound3DListener_SetDistanceFactor(l, 2, lap[k]);
                IDirectSound3DListener_GetPosition(l, &g);
                IDirectSound3DListener_GetDistanceFactor(l, &df);
                HRESULT hc = IDirectSound3DListener_CommitDeferredSettings(l);
                IDirectSound3DListener_GetPosition(l, &c);
                IDirectSound3DListener_GetDistanceFactor(l, &dc);
                CASE("listener SetPosition(4,5,6, apply %lx) %08lx get %g,%g,%g; SetDistanceFactor(2) %08lx get %s;"
                     " commit %08lx get %g,%g,%g distance %s\n", lap[k], h, g.x, g.y, g.z, h2, fstr(df, s1), hc, c.x, c.y, c.z,
                     fstr(dc, s2));
            }
            IDirectSound3DListener_SetPosition(l, 0, 0, 0, DS3D_IMMEDIATE);
            IDirectSound3DListener_SetDistanceFactor(l, 1, DS3D_IMMEDIATE);
            IDirectSound3DListener_Release(l);
        }
        IDirectSoundBuffer_Release(p);
    }
    IDirectSound3DBuffer_Release(d);
    IDirectSoundBuffer_Release(b);
}

/* ------------------------------------------------------------------ EAX */
static const struct { GUID g; const char *name; } k_sets[] = {
    { { 0x4a4e6fc1, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } }, "EAX1 listener" },
    { { 0x4a4e6fc0, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } }, "EAX1 buffer" },
    { { 0x0306a6a8, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } }, "EAX2 listener" },
    { { 0x0306a6a7, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } }, "EAX2 buffer" },
    { { 0xa8fa6882, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } }, "EAX3 listener" },
    { { 0xa8fa6881, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } }, "EAX3 buffer" },
    { { 0x1d4870ad, 0x0def, 0x43c0, { 0xa4, 0x0c, 0x52, 0x36, 0x32, 0x29, 0x63, 0x42 } }, "EAX4 context" },
    { { 0x1b86b823, 0x22df, 0x4eae, { 0x8b, 0x3c, 0x12, 0x78, 0xce, 0x54, 0x42, 0x27 } }, "EAX4 source" },
    { { 0xc4d79f1e, 0xf1ac, 0x436b, { 0xa8, 0x1d, 0xa7, 0x38, 0xe7, 0x04, 0x54, 0x69 } }, "EAX4 slot0" },
    { { 0x08c00e96, 0x74be, 0x4491, { 0x93, 0xaa, 0xe8, 0xad, 0x35, 0xa4, 0x91, 0x17 } }, "EAX4 slot1" },
    { { 0x1d433b88, 0xf0f6, 0x4637, { 0x91, 0x9f, 0x60, 0xe7, 0xe0, 0x6b, 0x5e, 0xdd } }, "EAX4 slot2" },
    { { 0xefff08ea, 0xc7d8, 0x44ab, { 0x93, 0xad, 0x6d, 0xbd, 0x5f, 0x91, 0x00, 0x64 } }, "EAX4 slot3" },
    { { 0x57e13437, 0xb932, 0x4ab2, { 0xb8, 0xbd, 0x52, 0x66, 0xc1, 0xa8, 0x87, 0xee } }, "EAX5 context" },
    { { 0x5edf82f0, 0x24a7, 0x4f38, { 0x8e, 0x64, 0x2f, 0x09, 0xca, 0x05, 0xde, 0xe1 } }, "EAX5 source" },
    { { 0x62a69bae, 0xdf9d, 0x11d1, { 0x99, 0xa6, 0x00, 0xc0, 0x4f, 0xc9, 0x9d, 0x46 } }, "voice manager" },
};
#define NSETS (sizeof(k_sets) / sizeof(k_sets[0]))

static IDirectSoundBuffer *eax_buffer(DWORD flags, IKsPropertySet **ks)
{
    WAVEFORMATEXTENSIBLE e; fmt_make(&e, 1, 1, 16, 22050);
    IDirectSoundBuffer *b = NULL;
    *ks = NULL;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        DSBUFFERDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwFlags = flags;
        IDirectSound8_CreateSoundBuffer(s_ds8, &d, &b, NULL);
    } else mk((IDirectSound *)s_ds8, flags, &e, 44100, sizeof(DSBUFFERDESC), &b);
    if (b) IDirectSoundBuffer_QueryInterface(b, &IID_IKsPropertySet, (void **)ks);
    return b;
}

static void get_line(IKsPropertySet *ks, unsigned s, ULONG id, ULONG len)
{
    DWORD w[64]; memset(w, 0xcd, sizeof(w));
    ULONG got = 0xdead;
    HRESULT hr = IKsPropertySet_Get(ks, &k_sets[s].g, id, NULL, 0, w, len, &got);
    P(" get %08lx got %lx", hr, got);
    if (SUCCEEDED(hr)) dump(w, got <= 256 ? got : 256);
}

/* every property's support and default, on a 3D buffer, a plain buffer and the primary */
static void sec_eax(void)
{
    static const struct { DWORD flags; const char *what; } bt[] = {
        { DSBCAPS_CTRL3D, "3D" }, { 0, "plain" }, { DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, "primary" } };
    for (unsigned t = 0; t < 3; t++) {
        IKsPropertySet *ks;
        IDirectSoundBuffer *b = eax_buffer(bt[t].flags, &ks);
        CASE("%s buffer %s, IKsPropertySet %s\n", bt[t].what, b ? "made" : "refused", ks ? "yes" : "no");
        if (!ks) { if (b) IDirectSoundBuffer_Release(b); continue; }
        for (unsigned s = 0; s < NSETS; s++)
            for (ULONG id = 0; id < 0x10008; id++) {
                if (id == 32) id = 0x10000;
                ULONG sup = 0xdead;
                HRESULT hr = IKsPropertySet_QuerySupport(ks, &k_sets[s].g, id, &sup);
                if (FAILED(hr) && id > 0 && id != 0x10000) continue;     /* (unsupported ids past the first: quiet) */
                CASE("%s %s id %lx: support %08lx %lx", bt[t].what, k_sets[s].name, id, hr, sup);
                get_line(ks, s, id, 256);
                P("\n");
            }
        IKsPropertySet_Release(ks);
        IDirectSoundBuffer_Release(b);
    }
    /* environments: EAX1 (id 1), EAX2 (id 11), EAX3 (id 2), EAX4 slot 0 (id 2), each read back whole */
    IKsPropertySet *ks;
    IDirectSoundBuffer *b = eax_buffer(DSBCAPS_CTRL3D, &ks);
    if (!ks) { if (b) IDirectSoundBuffer_Release(b); return; }
    static const struct { unsigned set; ULONG env, all, len; } ev[] = { { 0, 1, 0, 16 }, { 2, 11, 1, 256 }, { 4, 2, 1, 256 }, { 8, 2, 1, 256 } };
    for (unsigned k = 0; k < 4; k++)
        for (DWORD env = 0; env <= 27; env++) {
            HRESULT hr = IKsPropertySet_Set(ks, &k_sets[ev[k].set].g, ev[k].env, NULL, 0, &env, 4);
            CASE("%s environment %lu: set %08lx", k_sets[ev[k].set].name, env, hr);
            get_line(ks, ev[k].set, ev[k].all, ev[k].len);
            P("\n");
        }
    IKsPropertySet_Release(ks);
    IDirectSoundBuffer_Release(b);
}

/* each property of EAX 2 and 3 (listener and buffer) and EAX 4 set to values at and past its edges, read back */
static void sec_eaxval(void)
{
    IKsPropertySet *ks;
    IDirectSoundBuffer *b = eax_buffer(DSBCAPS_CTRL3D, &ks);
    if (!ks) { CASE("no IKsPropertySet\n"); if (b) IDirectSoundBuffer_Release(b); return; }
    static const LONG iv[] = { -10001, -10000, -9999, -100, -1, 0, 1, 99, 100, 1000, 1001, 10000, 0x7fffffff };
    static const float fv[] = { -1.0f, 0.0f, 0.001f, 0.1f, 0.5f, 0.99f, 1.0f, 1.01f, 2.0f, 5.0f, 10.0f, 20.0f, 100.0f,
                                1000.0f, 20000.0f, 20001.0f, 1e30f };
    static const struct { unsigned set; ULONG lo, hi; } rng[] = { { 2, 2, 15 }, { 3, 2, 14 }, { 4, 2, 25 }, { 5, 2, 21 }, { 6, 2, 6 },
                                                                 { 7, 2, 30 }, { 8, 0x10001, 0x10005 }, { 0, 1, 4 }, { 1, 1, 1 } };
    char s1[32];
    for (unsigned r = 0; r < sizeof(rng) / sizeof(rng[0]); r++)
        for (ULONG id = rng[r].lo; id <= rng[r].hi; id++) {
            unsigned s = rng[r].set;
            for (int f = 0; f < 2; f++) {
                unsigned n = f ? sizeof(fv) / sizeof(fv[0]) : sizeof(iv) / sizeof(iv[0]);
                for (unsigned k = 0; k < n; k++) {
                    DWORD v; if (f) memcpy(&v, &fv[k], 4); else v = (DWORD)iv[k];
                    HRESULT hr = IKsPropertySet_Set(ks, &k_sets[s].g, id, NULL, 0, &v, 4);
                    if (f) CASE("%s id %lx = %s: set %08lx", k_sets[s].name, id, fstr(fv[k], s1), hr);
                    else CASE("%s id %lx = %ld: set %08lx", k_sets[s].name, id, iv[k], hr);
                    get_line(ks, s, id, 4);
                    P("\n");
                }
            }
        }
    IKsPropertySet_Release(ks);
    IDirectSoundBuffer_Release(b);
}

/* ------------------------------------------------------------------ main */
static LONG WINAPI on_crash(EXCEPTION_POINTERS *e)
{
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    const void *addr = e->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m)
        GetModuleFileNameA(m, path, MAX_PATH);
    const char *base = strrchr(path, '\\');
    P("\nCRASH %08lx in %s+%lx case %d\n", e->ExceptionRecord->ExceptionCode, base ? base + 1 : path,
      (unsigned long)((const char *)addr - (const char *)m), s_case + 1);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_EXECUTE_HANDLER;
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    Sleep((DWORD)(UINT_PTR)arg * 1000);
    P("\nTIMEOUT case %d\n", s_case + 1);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char **argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(on_crash);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *to = getenv("ALPROBE_TIMEOUT");
    CreateThread(NULL, 0, watchdog, (LPVOID)(UINT_PTR)(to && atoi(to) > 0 ? atoi(to) : 120), 0, NULL);
    const char *sec = argc > 1 ? argv[1] : "create";
    CoInitialize(NULL);
    P("SECTION %s\n", sec);
    HRESULT h8 = DirectSoundCreate8(NULL, &s_ds8, NULL), h7 = DirectSoundCreate(NULL, &s_ds7, NULL);
    P("DirectSoundCreate8 %08lx DirectSoundCreate %08lx\n", h8, h7);
    if (!s_ds8 || !s_ds7) return 1;
    IDirectSound8_SetCooperativeLevel(s_ds8, GetDesktopWindow(), DSSCL_PRIORITY);
    IDirectSound_SetCooperativeLevel(s_ds7, GetDesktopWindow(), DSSCL_PRIORITY);
    if (!strcmp(sec, "create")) sec_create();
    else if (!strcmp(sec, "ctrl")) sec_ctrl();
    else if (!strcmp(sec, "lock")) sec_lock();
    else if (!strcmp(sec, "play")) sec_play();
    else if (!strcmp(sec, "notify")) sec_notify();
    else if (!strcmp(sec, "misc")) sec_misc();
    else if (!strcmp(sec, "d3")) sec_d3();
    else if (!strcmp(sec, "eax")) sec_eax();
    else if (!strcmp(sec, "eaxval")) sec_eaxval();
    else P("unknown section\n");
    IDirectSound_Release(s_ds7);
    IDirectSound8_Release(s_ds8);
    P("END\n");
    CoUninitialize();
    return 0;
}
