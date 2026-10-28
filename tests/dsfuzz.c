/* dsfuzz - random DirectSound calls, the same on any DLL, to compare reAL with Microsoft's line by line
 *
 *   dsfuzz.exe <first seed> <seeds> [ops per seed]
 *
 * Each seed makes an IDirectSound8 (and an IDirectSound), then does random calls on it and on the buffers, 3D
 * buffers, listeners and notifies it makes, with values drawn mostly from the edges (sizes a byte off a block, rates
 * and volumes one off their limits, infinite and NaN floats, flags one bit too many).  Every call prints its HRESULT
 * and what it gave back.  tests/fuzzcmp.py runs it under both DLLs and reports where each seed first differs.
 *
 * Only what doesn't depend on timing is printed: a playing buffer's position, Lock from the write cursor while
 * playing and the PLAYING bit of a buffer playing once are left out.  Pointers given are NULL or valid (a call with a
 * bad pointer may crash either DLL); a DirectSound object is released only after its buffers. */
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
#include <math.h>
#include <float.h>

#define P(...) printf(__VA_ARGS__)

static unsigned long long s_rng;
static unsigned rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (unsigned)(s_rng >> 16);
}
static unsigned rn(unsigned n) { return n ? rnd() % n : 0; }
#define PICK(a) ((a)[rn(sizeof(a) / sizeof((a)[0]))])
static int chance(int pct) { return (int)rn(100) < pct; }

/* ------------------------------------------------------------------ value pools */
static const DWORD k_sizes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 15, 16, 17, 100, 101, 4095, 4096, 4097, 22050, 22051, 44100,
                                 44101, 88200, 176400, 176401, 176402, 176403, 254745, 1000000, DSBSIZE_MAX + 1 };
static const DWORD k_rates[] = { 0, 1, 99, 100, 101, 8000, 11025, 22050, 32000, 44100, 48000, 96000, 100000, 192000,
                                 199999, 200000, 200001, 384000 };
static const WORD k_chans[] = { 0, 1, 1, 2, 2, 3, 4, 6, 8, 9 };
static const WORD k_bits[] = { 0, 4, 8, 8, 12, 16, 16, 20, 24, 32, 64 };
static const DWORD k_bflags[] = { DSBCAPS_PRIMARYBUFFER, DSBCAPS_STATIC, DSBCAPS_LOCHARDWARE, DSBCAPS_LOCSOFTWARE,
                                  DSBCAPS_CTRL3D, DSBCAPS_CTRLFREQUENCY, DSBCAPS_CTRLPAN, DSBCAPS_CTRLVOLUME,
                                  DSBCAPS_CTRLPOSITIONNOTIFY, DSBCAPS_CTRLFX, DSBCAPS_STICKYFOCUS, DSBCAPS_GLOBALFOCUS,
                                  DSBCAPS_GETCURRENTPOSITION2, DSBCAPS_MUTE3DATMAXDISTANCE, DSBCAPS_LOCDEFER,
                                  0x00000400, 0x00080000 /* DSBCAPS_TRUEPLAYPOSITION */, 0x00100000, 0x80000000 };
static const LONG k_vols[] = { -10001, -10000, -9999, -5000, -1, 0, 0, 1, 10000 };
static const LONG k_pans[] = { -10001, -10000, -9999, -1, 0, 1, 9999, 10000, 10001 };
static const DWORD k_freqs[] = { 0, 1, 99, 100, 101, 22050, 44100, 199999, 200000, 200001, 0xffffffff };
static const DWORD k_apply[] = { DS3D_IMMEDIATE, DS3D_IMMEDIATE, DS3D_DEFERRED, DS3D_DEFERRED, 2, 3, 0xffffffff };
static const DWORD k_modes[] = { DS3DMODE_NORMAL, DS3DMODE_HEADRELATIVE, DS3DMODE_DISABLE, 3, 0xffffffff };
static const DWORD k_angles[] = { 0, 1, 45, 90, 180, 359, 360, 361, 0xffffffff };
static const DWORD k_levels[] = { 0, DSSCL_NORMAL, DSSCL_PRIORITY, DSSCL_PRIORITY, DSSCL_EXCLUSIVE, DSSCL_WRITEPRIMARY, 5, 0xff };
static const DWORD k_speakers[] = { 0, DSSPEAKER_HEADPHONE, DSSPEAKER_MONO, DSSPEAKER_QUAD, DSSPEAKER_STEREO,
                                    DSSPEAKER_SURROUND, DSSPEAKER_5POINT1, DSSPEAKER_7POINT1, 9, 10, 0xff,
                                    DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE),
                                    DSSPEAKER_COMBINED(DSSPEAKER_STEREO, 181), DSSPEAKER_COMBINED(DSSPEAKER_STEREO, 1) };

static float fval(void)
{
    static const float k[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 10.0f, 100.0f, 1e-30f, 1e30f, FLT_MAX, -FLT_MAX,
                               DS3D_DEFAULTMINDISTANCE, DS3D_DEFAULTMAXDISTANCE, 0.0f, 1.0f };
    unsigned r = rn(100);
    if (r < 4) { union { DWORD u; float f; } x = { 0x7f800000 }; return x.f; }
    if (r < 6) { union { DWORD u; float f; } x = { 0xff800000 }; return x.f; }
    if (r < 9) { union { DWORD u; float f; } x = { 0x7fc00000 }; return x.f; }
    if (r < 70) return PICK(k);
    return (float)((int)rn(2001) - 1000) / 10.0f;
}

/* a float as printed: by its bits for inf / NaN (printf differs between C runtimes) */
static const char *fs(float f, char *b)
{
    union { float f; DWORD u; } x = { f };
    if ((x.u & 0x7f800000) == 0x7f800000) sprintf(b, "<%08lx>", (unsigned long)x.u);
    else sprintf(b, "%.4g", (double)f);
    return b;
}
static void pv(const char *name, const D3DVECTOR *v)
{
    char a[32], b[32], c[32];
    P(" %s %s,%s,%s", name, fs(v->x, a), fs(v->y, b), fs(v->z, c));
}

/* ------------------------------------------------------------------ objects */
#define NBUF 12
#define BL(i) (s_buf[i].primary ? 'p' : 'b')
typedef struct {
    IDirectSoundBuffer *b;          /* the buffer (one reference) */
    IDirectSoundBuffer8 *b8;        /* its IDirectSoundBuffer8, or NULL (one reference) */
    IDirectSound3DBuffer *d3;       /* or NULL (one reference) */
    IDirectSound3DListener *lst;    /* the primary's, or NULL (one reference) */
    IDirectSoundNotify *ntf;        /* or NULL (one reference) */
    int primary, ds;                /* which DirectSound made it */
    int playing;                    /* 0 stopped, 1 playing looped, 2 maybe playing (once, or unsure) */
    int pos_known;                  /* its position doesn't depend on timing (not played since made or set) */
    void *lp1, *lp2; DWORD lb1, lb2;/* an open Lock, or lp1 NULL */
    DWORD size, block;
} fbuf;
static fbuf s_buf[NBUF];
static IDirectSound8 *s_ds[2];      /* [0] IDirectSound8, [1] IDirectSound */
static HANDLE s_event[4];
static int s_op, s_trace;
static int s_deferred;              /* a deferred 3D change not committed: its values read back differ (reAL and ALchemy
                                     * answer the new ones, Windows the committed ones) */

static int free_slot(void) { for (int i = 0; i < NBUF; i++) if (!s_buf[i].b) return i; return -1; }
static int any_buf(void)
{
    int n = 0, idx[NBUF];
    for (int i = 0; i < NBUF; i++) if (s_buf[i].b) idx[n++] = i;
    return n ? idx[rn(n)] : -1;
}

static void drop(int i)
{
    fbuf *f = &s_buf[i];
    if (f->lp1) { IDirectSoundBuffer_Unlock(f->b, f->lp1, f->lb1, f->lp2, f->lb2); }
    if (f->ntf) IDirectSoundNotify_Release(f->ntf);
    if (f->lst) IDirectSound3DListener_Release(f->lst);
    if (f->d3) IDirectSound3DBuffer_Release(f->d3);
    if (f->b8) IDirectSoundBuffer8_Release(f->b8);
    if (f->b) IDirectSoundBuffer_Release(f->b);
    memset(f, 0, sizeof(*f));
}

static void show_caps(IDirectSoundBuffer *b)
{
    DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    HRESULT hr = IDirectSoundBuffer_GetCaps(b, &c);
    P(" caps %08lx flags %lx bytes %lu", hr, c.dwFlags, c.dwBufferBytes);
}
static void show_format(IDirectSoundBuffer *b)
{
    BYTE buf[64]; memset(buf, 0xcc, sizeof(buf));
    DWORD got = 0xdead;
    HRESULT hr = IDirectSoundBuffer_GetFormat(b, (WAVEFORMATEX *)buf, sizeof(buf), &got);
    const WAVEFORMATEX *w = (const WAVEFORMATEX *)buf;
    P(" fmt %08lx got %lu tag %x ch %u rate %lu bits %u align %u avg %lu cb %u", hr, got, w->wFormatTag, w->nChannels,
      w->nSamplesPerSec, w->wBitsPerSample, w->nBlockAlign, w->nAvgBytesPerSec, w->cbSize);
    if (SUCCEEDED(hr) && w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && got >= sizeof(WAVEFORMATEXTENSIBLE)) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *)buf;
        P(" valid %u mask %lx sub %08lx", e->Samples.wValidBitsPerSample, e->dwChannelMask, e->SubFormat.Data1);
    }
}

/* a random format, mostly sensible */
static void make_format(WAVEFORMATEXTENSIBLE *e)
{
    memset(e, 0, sizeof(*e));
    WAVEFORMATEX *w = &e->Format;
    unsigned kind = rn(100);
    w->nSamplesPerSec = chance(60) ? PICK(((DWORD[]){ 22050, 44100, 48000, 11025, 32000 })) : PICK(k_rates);
    w->nChannels = chance(60) ? (WORD)(1 + rn(2)) : PICK(k_chans);
    w->wBitsPerSample = chance(60) ? (WORD)(chance(50) ? 16 : 8) : PICK(k_bits);
    if (kind < 70) w->wFormatTag = WAVE_FORMAT_PCM;
    else if (kind < 80) { w->wFormatTag = WAVE_FORMAT_IEEE_FLOAT; if (chance(70)) w->wBitsPerSample = 32; }
    else if (kind < 95) {
        w->wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        w->cbSize = chance(85) ? 22 : PICK(((WORD[]){ 0, 21, 23, 100 }));
        e->Samples.wValidBitsPerSample = chance(80) ? w->wBitsPerSample : PICK(((WORD[]){ 0, 8, 12, 20, 33 }));
        e->dwChannelMask = chance(50) ? (w->nChannels == 1 ? 4 : w->nChannels == 2 ? 3 : w->nChannels == 6 ? 0x3f : 0)
                                      : PICK(((DWORD[]){ 0, 1, 3, 7, 0x3f, 0x63f, 0x80000000 }));
        e->SubFormat = chance(70) ? KSDATAFORMAT_SUBTYPE_PCM : chance(70) ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : GUID_NULL;
    } else w->wFormatTag = PICK(((WORD[]){ 0, 2, 0x55, 0x1234 }));
    w->nBlockAlign = (WORD)(w->nChannels * w->wBitsPerSample / 8);
    w->nAvgBytesPerSec = w->nBlockAlign * w->nSamplesPerSec;
    if (chance(8)) w->nBlockAlign += (WORD)PICK(((int[]){ -1, 1, 2 }));
    if (chance(8)) w->nAvgBytesPerSec += PICK(((int[]){ -1, 1, 1000 }));
    if (w->wFormatTag != WAVE_FORMAT_EXTENSIBLE && chance(8)) w->cbSize = (WORD)PICK(((int[]){ 1, 22, 0xffff }));
}

static void print_format_in(const WAVEFORMATEX *w)
{
    P("tag %x ch %u rate %lu bits %u align %u avg %lu cb %u", w->wFormatTag, w->nChannels, w->nSamplesPerSec,
      w->wBitsPerSample, w->nBlockAlign, w->nAvgBytesPerSec, w->cbSize);
}

/* ------------------------------------------------------------------ operations */
static void op_create(void)
{
    int slot = free_slot();
    if (slot < 0) return;
    int which = chance(80) ? 0 : 1;
    if (!s_ds[which]) return;
    DSBUFFERDESC d; memset(&d, 0, sizeof(d));
    WAVEFORMATEXTENSIBLE e;
    make_format(&e);
    d.dwSize = chance(92) ? sizeof(d) : PICK(((DWORD[]){ 0, sizeof(DSBUFFERDESC1), sizeof(d) - 1, sizeof(d) + 4 }));
    int primary = chance(12);
    if (primary) d.dwFlags = DSBCAPS_PRIMARYBUFFER | (chance(50) ? DSBCAPS_CTRL3D : 0) | (chance(30) ? DSBCAPS_CTRLVOLUME : 0);
    else {
        d.dwFlags = chance(50) ? DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY : 0;
        if (chance(40)) d.dwFlags |= DSBCAPS_CTRL3D;
        else if (chance(40)) d.dwFlags |= DSBCAPS_CTRLPAN;
        if (chance(25)) d.dwFlags |= DSBCAPS_CTRLPOSITIONNOTIFY;
        if (chance(20)) d.dwFlags |= DSBCAPS_GETCURRENTPOSITION2;
        if (chance(10)) d.dwFlags |= DSBCAPS_CTRLFX;
        for (int k = rn(3); k > 0; k--) if (chance(30)) d.dwFlags |= PICK(k_bflags);
    }
    if (primary) d.dwBufferBytes = chance(80) ? 0 : PICK(k_sizes);
    else if (chance(60)) {
        DWORD ba = e.Format.nBlockAlign ? e.Format.nBlockAlign : 1;
        d.dwBufferBytes = (DWORD)(e.Format.nAvgBytesPerSec / (1 + rn(4))) / ba * ba + (chance(30) ? rn(4) : 0);
        if (d.dwBufferBytes > 2000000) d.dwBufferBytes = 2000000;
    } else d.dwBufferBytes = PICK(k_sizes);
    if (!primary || chance(10)) d.lpwfxFormat = chance(96) ? &e.Format : NULL;
    if (d.dwSize >= sizeof(d) && chance(15)) d.guid3DAlgorithm = PICK(((GUID[]){ DS3DALG_NO_VIRTUALIZATION, DS3DALG_HRTF_FULL,
                                                                             DS3DALG_HRTF_LIGHT, IID_IUnknown }));
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(s_ds[which], &d, &b, NULL);
    P("%d ds%d CreateSoundBuffer(size %lu flags %lx bytes %lu ", s_op, which, d.dwSize, d.dwFlags, d.dwBufferBytes);
    if (d.lpwfxFormat) print_format_in(d.lpwfxFormat); else P("no format");
    P(") %08lx %s", hr, b ? "set" : "NULL");
    if (!b) { P("\n"); return; }
    fbuf *f = &s_buf[slot];
    f->b = b; f->ds = which; f->primary = (d.dwFlags & DSBCAPS_PRIMARYBUFFER) != 0; f->pos_known = 1;
    DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    IDirectSoundBuffer_GetCaps(b, &c);
    f->size = c.dwBufferBytes;
    WAVEFORMATEX w; memset(&w, 0, sizeof(w));
    IDirectSoundBuffer_GetFormat(b, &w, sizeof(w), NULL);
    f->block = w.nBlockAlign ? w.nBlockAlign : 1;
    show_caps(b); show_format(b);
    if (which == 0) {                   /* (an older DirectSound's buffer answers it under reAL and ALchemy, not Windows) */
        hr = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundBuffer8, (void **)&f->b8);
        P(" qi8 %08lx", hr);
    }
    hr = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&f->d3);
    P(" qi3d %08lx", hr);
    hr = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DListener, (void **)&f->lst);
    P(" qilst %08lx", hr);
    hr = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&f->ntf);
    P(" qintf %08lx\n", hr);
}

static void op_duplicate(void)
{
    int i = any_buf(), slot = free_slot();
    if (i < 0 || slot < 0) return;
    int which = chance(80) ? s_buf[i].ds : !s_buf[i].ds;
    if (!s_ds[which]) return;
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_DuplicateSoundBuffer(s_ds[which], s_buf[i].b, &b);
    P("%d ds%d Duplicate(b%d) %08lx %s", s_op, which, i, hr, b ? "set" : "NULL");
    if (b) {
        fbuf *f = &s_buf[slot];
        f->b = b; f->ds = which; f->size = s_buf[i].size; f->block = s_buf[i].block; f->pos_known = 1;
        show_caps(b); show_format(b);
        DWORD st = 0; IDirectSoundBuffer_GetStatus(b, &st);
        LONG v = 1; IDirectSoundBuffer_GetVolume(b, &v);
        DWORD fr = 1; IDirectSoundBuffer_GetFrequency(b, &fr);
        DWORD pl = 1, wr = 1; IDirectSoundBuffer_GetCurrentPosition(b, &pl, &wr);
        if (s_buf[i].pos_known && !s_buf[i].playing) P(" status %lx vol %ld freq %lu pos %lu", st, v, fr, pl);
        else P(" status %lx vol %ld freq %lu", st, v, fr);
        hr = IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&f->d3);
        P(" qi3d %08lx", hr);
        if (f->d3) {
            DS3DBUFFER p; memset(&p, 0, sizeof(p)); p.dwSize = sizeof(p);
            IDirectSound3DBuffer_GetAllParameters(f->d3, &p);
            pv("pos", &p.vPosition);
            char a[32], c[32]; P(" min %s max %s mode %lu", fs(p.flMinDistance, a), fs(p.flMaxDistance, c), p.dwMode);
        }
    }
    P("\n");
}

static void op_release(void)
{
    int i = any_buf();
    if (i < 0) return;
    fbuf *f = &s_buf[i];
    if (f->lp1) return;
    ULONG r[5] = { 0 };
    if (f->ntf) r[0] = IDirectSoundNotify_Release(f->ntf);
    if (f->lst) r[1] = IDirectSound3DListener_Release(f->lst);
    if (f->d3) r[2] = IDirectSound3DBuffer_Release(f->d3);
    if (f->b8) r[3] = IDirectSoundBuffer8_Release(f->b8);
    r[4] = IDirectSoundBuffer_Release(f->b);
    P("%d %c%d Release ntf %lu lst %lu 3d %lu b8 %lu b %lu\n", s_op, BL(i), i, r[0], r[1], r[2], r[3], r[4]);
    memset(f, 0, sizeof(*f));
}

static void op_refs(void)
{
    int i = any_buf();
    if (i < 0) return;
    fbuf *f = &s_buf[i];
    ULONG a = IDirectSoundBuffer_AddRef(f->b), b = IDirectSoundBuffer_Release(f->b);
    P("%d %c%d AddRef %lu Release %lu", s_op, BL(i), i, a, b);
    if (f->d3) { a = IDirectSound3DBuffer_AddRef(f->d3); b = IDirectSound3DBuffer_Release(f->d3); P(" 3d %lu %lu", a, b); }
    if (f->ntf) { a = IDirectSoundNotify_AddRef(f->ntf); b = IDirectSoundNotify_Release(f->ntf); P(" ntf %lu %lu", a, b); }
    P("\n");
}

static void op_qi(void)
{
    int i = any_buf();
    if (i < 0) return;
    static const GUID *const iids[] = { &IID_IUnknown, &IID_IDirectSoundBuffer, &IID_IDirectSoundBuffer8, &IID_IDirectSound3DBuffer,
                                        &IID_IDirectSound3DListener, &IID_IDirectSoundNotify, &IID_IKsPropertySet, &IID_IDirectSound,
                                        &IID_IDirectSound8, &IID_IDirectSoundFXGargle, &GUID_NULL };
    unsigned k = rn(sizeof(iids) / sizeof(iids[0]));
    IUnknown *o = (IUnknown *)1;
    HRESULT hr = IDirectSoundBuffer_QueryInterface(s_buf[i].b, iids[k], (void **)&o);
    P("%d %c%d QueryInterface(iid %u) %08lx %s\n", s_op, BL(i), i, k, hr, o == (IUnknown *)1 ? "untouched" : o ? "set" : "NULL");
    if (SUCCEEDED(hr) && o && o != (IUnknown *)1) IUnknown_Release(o);
}

static void op_caps(void)
{
    int i = any_buf();
    if (i < 0) return;
    DSBCAPS c; memset(&c, 0, sizeof(c));
    c.dwSize = chance(80) ? sizeof(c) : PICK(((DWORD[]){ 0, sizeof(c) - 1, sizeof(c) + 4 }));
    DWORD sz = c.dwSize;
    HRESULT hr = IDirectSoundBuffer_GetCaps(s_buf[i].b, chance(97) ? &c : NULL);
    P("%d %c%d GetCaps(size %lu) %08lx flags %lx bytes %lu\n", s_op, BL(i), i, sz, hr, c.dwFlags, c.dwBufferBytes);
}

static void op_getformat(void)
{
    int i = any_buf();
    if (i < 0) return;
    BYTE buf[64]; memset(buf, 0xcc, sizeof(buf));
    DWORD got = 0xdead;
    DWORD size = PICK(((DWORD[]){ 0, 1, 14, 16, 17, 18, 19, 20, 39, 40, 64 }));
    int nul = chance(20), gnul = chance(20);
    HRESULT hr = IDirectSoundBuffer_GetFormat(s_buf[i].b, nul ? NULL : (WAVEFORMATEX *)buf, size, gnul ? NULL : &got);
    const WAVEFORMATEX *w = (const WAVEFORMATEX *)buf;
    P("%d %c%d GetFormat(%s, %lu, %s) %08lx got %lx tag %x ch %u rate %lu cb %x byte40 %02x\n", s_op, BL(i), i, nul ? "NULL" : "buf", size,
      gnul ? "NULL" : "&got", hr, got, w->wFormatTag, w->nChannels, w->nSamplesPerSec, w->cbSize, buf[40]);
}

static void op_setformat(void)
{
    int i = any_buf();
    if (i < 0) return;
    WAVEFORMATEXTENSIBLE e;
    make_format(&e);
    int nul = chance(5);
    HRESULT hr = IDirectSoundBuffer_SetFormat(s_buf[i].b, nul ? NULL : &e.Format);
    P("%d %c%d SetFormat(", s_op, BL(i), i);
    if (nul) P("NULL"); else print_format_in(&e.Format);
    P(") %08lx", hr);
    show_format(s_buf[i].b);
    P("\n");
}

static void op_volpanfreq(void)
{
    int i = any_buf();
    if (i < 0) return;
    IDirectSoundBuffer *b = s_buf[i].b;
    HRESULT hr;
    switch (rn(3)) {
    case 0: { LONG v = PICK(k_vols); hr = IDirectSoundBuffer_SetVolume(b, v); LONG g = 12345; HRESULT h2 = IDirectSoundBuffer_GetVolume(b, &g);
              P("%d %c%d SetVolume(%ld) %08lx get %08lx %ld\n", s_op, BL(i), i, v, hr, h2, g); break; }
    case 1: { LONG v = PICK(k_pans); hr = IDirectSoundBuffer_SetPan(b, v); LONG g = 12345; HRESULT h2 = IDirectSoundBuffer_GetPan(b, &g);
              P("%d %c%d SetPan(%ld) %08lx get %08lx %ld\n", s_op, BL(i), i, v, hr, h2, g); break; }
    default: { DWORD v = PICK(k_freqs); hr = IDirectSoundBuffer_SetFrequency(b, v); DWORD g = 12345; HRESULT h2 = IDirectSoundBuffer_GetFrequency(b, &g);
               P("%d %c%d SetFrequency(%lu) %08lx get %08lx %lu\n", s_op, BL(i), i, v, hr, h2, g); break; }
    }
}

static void op_play(void)
{
    int i = any_buf();
    if (i < 0) return;
    fbuf *f = &s_buf[i];
    DWORD res = chance(90) ? 0 : 1, prio = chance(85) ? 0 : PICK(((DWORD[]){ 1, 2, 0xffffffff }));
    DWORD flags = chance(70) ? DSBPLAY_LOOPING : 0;
    if (chance(20)) flags |= PICK(((DWORD[]){ DSBPLAY_LOCHARDWARE, DSBPLAY_LOCSOFTWARE, DSBPLAY_TERMINATEBY_TIME,
                                               DSBPLAY_TERMINATEBY_DISTANCE, DSBPLAY_TERMINATEBY_PRIORITY, 0x80 }));
    HRESULT hr = IDirectSoundBuffer_Play(f->b, res, prio, flags);
    if (SUCCEEDED(hr)) { f->playing = (flags & DSBPLAY_LOOPING) && f->playing != 2 ? 1 : 2; f->pos_known = 0; }
    DWORD st = 0xdead;
    HRESULT h2 = IDirectSoundBuffer_GetStatus(f->b, &st);
    if (f->playing == 2) st &= ~DSBSTATUS_PLAYING;
    P("%d %c%d Play(%lu, %lu, %lx) %08lx status %08lx %lx\n", s_op, BL(i), i, res, prio, flags, hr, h2, st);
}

static void op_stop(void)
{
    int i = any_buf();
    if (i < 0) return;
    HRESULT hr = IDirectSoundBuffer_Stop(s_buf[i].b);
    if (SUCCEEDED(hr)) s_buf[i].playing = 0;
    DWORD st = 0xdead;
    HRESULT h2 = IDirectSoundBuffer_GetStatus(s_buf[i].b, &st);
    P("%d %c%d Stop %08lx status %08lx %lx\n", s_op, BL(i), i, hr, h2, st);
}

static DWORD offset_for(const fbuf *f)
{
    DWORD s = f->size, b = f->block;
    DWORD k[] = { 0, 0, 1, b - 1, b, b + 1, s / 2, s / 2 + 1, s - b, s - 1, s, s + 1, s + b, 0xffffffff, 0x80000000 };
    return PICK(k);
}

static void op_position(void)
{
    int i = any_buf();
    if (i < 0) return;
    fbuf *f = &s_buf[i];
    if (chance(50)) {
        DWORD p = offset_for(f);
        HRESULT hr = IDirectSoundBuffer_SetCurrentPosition(f->b, p);
        if (SUCCEEDED(hr) && !f->playing) f->pos_known = 1;
        P("%d %c%d SetCurrentPosition(%lu) %08lx", s_op, BL(i), i, p, hr);
    } else P("%d %c%d", s_op, BL(i), i);
    DWORD pl = 0xdead, wr = 0xdead;
    int np = chance(15), nw = chance(15);
    HRESULT hr = IDirectSoundBuffer_GetCurrentPosition(f->b, np ? NULL : &pl, nw ? NULL : &wr);
    if (f->playing || !f->pos_known) P(" GetCurrentPosition(%s, %s) %08lx\n", np ? "NULL" : "&p", nw ? "NULL" : "&w", hr);
    else P(" GetCurrentPosition(%s, %s) %08lx play %lx write %lx\n", np ? "NULL" : "&p", nw ? "NULL" : "&w", hr, pl, wr);
}

static void op_lock(void)
{
    int i = any_buf();
    if (i < 0) return;
    fbuf *f = &s_buf[i];
    if (f->lp1 && chance(60)) {         /* the open Lock: unlocked, sometimes with other lengths */
        DWORD b1 = f->lb1, b2 = f->lb2;
        int how = rn(10);
        if (how == 0 && b1) b1--;
        else if (how == 1) b1++;
        else if (how == 2) b2 = b2 ? b2 - 1 : 1;
        void *p2 = f->lp2;
        if (how == 3) p2 = NULL;
        HRESULT hr = IDirectSoundBuffer_Unlock(f->b, f->lp1, b1, p2, b2);
        P("%d %c%d Unlock(how %d) %08lx\n", s_op, BL(i), i, how > 3 ? 9 : how, hr);
        if (SUCCEEDED(hr) || chance(50)) {
            if (FAILED(hr)) IDirectSoundBuffer_Unlock(f->b, f->lp1, f->lb1, f->lp2, f->lb2);
            f->lp1 = NULL;
        }
        return;
    }
    DWORD off = offset_for(f), n = chance(15) ? 0 : offset_for(f);
    DWORD flags = 0;
    if (chance(15)) flags |= DSBLOCK_ENTIREBUFFER;
    if (chance(10)) flags |= DSBLOCK_FROMWRITECURSOR;
    if (chance(3)) flags |= 4;
    void *p1 = (void *)1, *p2 = (void *)1;
    DWORD b1 = 0xdead, b2 = 0xdead;
    int n2 = chance(15);
    HRESULT hr = IDirectSoundBuffer_Lock(f->b, off, n, &p1, &b1, n2 ? NULL : &p2, n2 ? NULL : &b2, flags);
    P("%d %c%d Lock(%lu, %lu, %s, %lx) %08lx", s_op, BL(i), i, off, n, n2 ? "no p2" : "p2", flags, hr);
    if ((flags & DSBLOCK_FROMWRITECURSOR) && (f->playing || !f->pos_known)) P("\n");
    else P(" p1 %s b1 %lx p2 %s b2 %lx\n", p1 == (void *)1 ? "untouched" : p1 ? "set" : "NULL", b1,
           n2 ? "-" : p2 == (void *)1 ? "untouched" : p2 ? "set" : "NULL", n2 ? 0 : b2);
    if (SUCCEEDED(hr)) {
        if (b1 && p1 && p1 != (void *)1) memset(p1, 0, b1);
        if (!n2 && b2 && p2 && p2 != (void *)1) memset(p2, 0, b2);
        if (f->lp1 || chance(50)) IDirectSoundBuffer_Unlock(f->b, p1, b1, n2 ? NULL : p2, n2 ? 0 : b2);
        else { f->lp1 = p1; f->lb1 = b1; f->lp2 = n2 ? NULL : p2; f->lb2 = n2 ? 0 : b2; }
    }
}

static void op_status_misc(void)
{
    int i = any_buf();
    if (i < 0) return;
    fbuf *f = &s_buf[i];
    switch (rn(4)) {
    case 0: { DWORD st = 0xdead; HRESULT hr = IDirectSoundBuffer_GetStatus(f->b, chance(95) ? &st : NULL);
              if (f->playing == 2) st &= ~DSBSTATUS_PLAYING;
              P("%d %c%d GetStatus %08lx %lx\n", s_op, BL(i), i, hr, st); break; }
    case 1: P("%d %c%d Restore %08lx\n", s_op, BL(i), i, IDirectSoundBuffer_Restore(f->b)); break;
    case 2: { HRESULT hr = IDirectSoundBuffer_Initialize(f->b, (IDirectSound *)s_ds[f->ds], NULL);
              P("%d %c%d Initialize %08lx\n", s_op, BL(i), i, hr); break; }
    default:
        if (f->b8) {
            DWORD flags = PICK(((DWORD[]){ 0, DSBPLAY_LOCSOFTWARE, DSBPLAY_LOCHARDWARE, DSBPLAY_LOCSOFTWARE | DSBPLAY_LOCHARDWARE, 0x80 }));
            DWORD n = rn(3), r[2] = { 0xdead, 0xdead };
            HRESULT hr = IDirectSoundBuffer8_AcquireResources(f->b8, flags, n, n ? r : NULL);
            P("%d %c%d AcquireResources(%lx, %lu) %08lx %lx %lx\n", s_op, BL(i), i, flags, n, hr, n > 0 ? r[0] : 0, n > 1 ? r[1] : 0);
        }
    }
}

static void op_fx(void)
{
    int i = any_buf();
    if (i < 0 || !s_buf[i].b8) return;
    fbuf *f = &s_buf[i];
    static const GUID *const fx[] = { &GUID_DSFX_STANDARD_GARGLE, &GUID_DSFX_STANDARD_CHORUS, &GUID_DSFX_STANDARD_FLANGER,
                                      &GUID_DSFX_STANDARD_ECHO, &GUID_DSFX_STANDARD_DISTORTION, &GUID_DSFX_STANDARD_COMPRESSOR,
                                      &GUID_DSFX_STANDARD_PARAMEQ, &GUID_DSFX_STANDARD_I3DL2REVERB, &GUID_DSFX_WAVES_REVERB,
                                      &GUID_NULL, &IID_IUnknown };
    if (chance(70)) {
        DSEFFECTDESC d[3]; DWORD r[3] = { 0xdead, 0xdead, 0xdead };
        DWORD n = rn(4);
        if (n > 3) n = 3;
        memset(d, 0, sizeof(d));
        unsigned which[3] = { 0 };
        for (DWORD k = 0; k < n; k++) {
            d[k].dwSize = chance(95) ? sizeof(d[k]) : 0;
            d[k].dwFlags = chance(80) ? 0 : PICK(((DWORD[]){ DSFX_LOCSOFTWARE, DSFX_LOCHARDWARE, 4 }));
            which[k] = rn(sizeof(fx) / sizeof(fx[0]));
            d[k].guidDSFXClass = *fx[which[k]];
        }
        int nr = chance(20);
        HRESULT hr = IDirectSoundBuffer8_SetFX(f->b8, n, n ? d : (chance(50) ? d : NULL), nr ? NULL : r);
        P("%d %c%d SetFX(%lu:", s_op, BL(i), i, n);
        for (DWORD k = 0; k < n; k++) P(" %u/%lx/%lu", which[k], d[k].dwFlags, d[k].dwSize);
        P(") %08lx %lx %lx %lx\n", hr, n > 0 && !nr ? r[0] : 0, n > 1 && !nr ? r[1] : 0, n > 2 && !nr ? r[2] : 0);
    } else {
        unsigned k = rn(sizeof(fx) / sizeof(fx[0]));
        static const GUID *const iids[] = { &IID_IDirectSoundFXGargle, &IID_IDirectSoundFXEcho, &IID_IDirectSoundFXI3DL2Reverb,
                                            &IID_IUnknown, &IID_IDirectSoundFXWavesReverb, &GUID_NULL };
        unsigned j = rn(sizeof(iids) / sizeof(iids[0]));
        DWORD idx = PICK(((DWORD[]){ 0, 0, 1, 2, 0xffffffff }));
        int all = chance(30);
        IUnknown *o = (IUnknown *)1;
        HRESULT hr = IDirectSoundBuffer8_GetObjectInPath(f->b8, all ? &GUID_All_Objects : fx[k], idx, iids[j], (void **)&o);
        P("%d %c%d GetObjectInPath(%s, %lu, iid %u) %08lx %s\n", s_op, BL(i), i, all ? "all" : "fx", idx, j, hr,
          o == (IUnknown *)1 ? "untouched" : o ? "set" : "NULL");
        if (SUCCEEDED(hr) && o && o != (IUnknown *)1) IUnknown_Release(o);
    }
}

static void op_notify(void)
{
    int i = any_buf();
    if (i < 0 || !s_buf[i].ntf) return;
    fbuf *f = &s_buf[i];
    DSBPOSITIONNOTIFY n[5];
    DWORD cnt = PICK(((DWORD[]){ 0, 1, 1, 2, 3, 5 }));
    P("%d %c%d SetNotificationPositions(%lu:", s_op, BL(i), i, cnt);
    for (DWORD k = 0; k < cnt; k++) {
        n[k].dwOffset = chance(15) ? DSBPN_OFFSETSTOP : offset_for(f);
        n[k].hEventNotify = chance(90) ? s_event[rn(4)] : NULL;
        P(" %lx%s", n[k].dwOffset, n[k].hEventNotify ? "" : "/null");
    }
    int nul = chance(8);
    HRESULT hr = IDirectSoundNotify_SetNotificationPositions(f->ntf, cnt, nul ? NULL : n);
    P("%s) %08lx\n", nul ? " NULL" : "", hr);
}

static void op_3d(void)
{
    int i = any_buf();
    if (i < 0 || !s_buf[i].d3) return;
    IDirectSound3DBuffer *d = s_buf[i].d3;
    char a[32], b[32], c[32];
    HRESULT hr;
    DWORD ap = PICK(k_apply);
    switch (rn(9)) {
    case 0: { float x = fval(), y = fval(), z = fval(); hr = IDirectSound3DBuffer_SetPosition(d, x, y, z, ap);
              P("%d %c%d SetPosition(%s,%s,%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), fs(y, b), fs(z, c), ap, hr); break; }
    case 1: { float x = fval(), y = fval(), z = fval(); hr = IDirectSound3DBuffer_SetVelocity(d, x, y, z, ap);
              P("%d %c%d SetVelocity(%s,%s,%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), fs(y, b), fs(z, c), ap, hr); break; }
    case 2: { float x = fval(); hr = IDirectSound3DBuffer_SetMinDistance(d, x, ap);
              P("%d %c%d SetMinDistance(%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), ap, hr); break; }
    case 3: { float x = fval(); hr = IDirectSound3DBuffer_SetMaxDistance(d, x, ap);
              P("%d %c%d SetMaxDistance(%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), ap, hr); break; }
    case 4: { DWORD m = PICK(k_modes); hr = IDirectSound3DBuffer_SetMode(d, m, ap);
              P("%d %c%d SetMode(%lx, %lx) %08lx", s_op, BL(i), i, m, ap, hr); break; }
    case 5: { DWORD in = PICK(k_angles), out = PICK(k_angles); hr = IDirectSound3DBuffer_SetConeAngles(d, in, out, ap);
              P("%d %c%d SetConeAngles(%lu, %lu, %lx) %08lx", s_op, BL(i), i, in, out, ap, hr); break; }
    case 6: { float x = fval(), y = fval(), z = fval(); hr = IDirectSound3DBuffer_SetConeOrientation(d, x, y, z, ap);
              P("%d %c%d SetConeOrientation(%s,%s,%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), fs(y, b), fs(z, c), ap, hr); break; }
    case 7: { LONG v = PICK(k_vols); hr = IDirectSound3DBuffer_SetConeOutsideVolume(d, v, ap);
              P("%d %c%d SetConeOutsideVolume(%ld, %lx) %08lx", s_op, BL(i), i, v, ap, hr); break; }
    default: {
        DS3DBUFFER p; memset(&p, 0, sizeof(p));
        p.dwSize = chance(85) ? sizeof(p) : PICK(((DWORD[]){ 0, sizeof(p) - 1, sizeof(p) + 4 }));
        p.vPosition.x = fval(); p.vPosition.y = fval(); p.vPosition.z = fval();
        p.vVelocity.x = fval(); p.vVelocity.y = fval(); p.vVelocity.z = fval();
        p.dwInsideConeAngle = PICK(k_angles); p.dwOutsideConeAngle = PICK(k_angles);
        p.vConeOrientation.x = fval(); p.vConeOrientation.y = fval(); p.vConeOrientation.z = fval();
        p.lConeOutsideVolume = PICK(k_vols); p.flMinDistance = fval(); p.flMaxDistance = fval(); p.dwMode = PICK(k_modes);
        DS3DBUFFER in = p;                  /* (printed as given: Windows writes into the caller's structure) */
        hr = IDirectSound3DBuffer_SetAllParameters(d, chance(97) ? &p : NULL, ap);
        p = in;
        P("%d %c%d SetAllParameters(size %lu", s_op, BL(i), i, p.dwSize);
        pv("pos", &p.vPosition); pv("vel", &p.vVelocity); pv("cone", &p.vConeOrientation);
        P(" angles %lu %lu outvol %ld min %s max %s mode %lx, %lx) %08lx", p.dwInsideConeAngle, p.dwOutsideConeAngle,
          p.lConeOutsideVolume, fs(p.flMinDistance, a), fs(p.flMaxDistance, b), p.dwMode, ap, hr);
    }
    }
    if (SUCCEEDED(hr) && ap != DS3D_IMMEDIATE) s_deferred = 1;
    if (s_deferred) { P(" | deferred\n"); return; }
    DS3DBUFFER g; memset(&g, 0, sizeof(g));
    g.dwSize = chance(90) ? sizeof(g) : PICK(((DWORD[]){ 0, sizeof(g) - 1, sizeof(g) + 4 }));
    hr = IDirectSound3DBuffer_GetAllParameters(d, &g);
    P(" | get(size %lu) %08lx", g.dwSize, hr);
    pv("pos", &g.vPosition); pv("vel", &g.vVelocity); pv("cone", &g.vConeOrientation);
    P(" angles %lu %lu outvol %ld min %s max %s mode %lx\n", g.dwInsideConeAngle, g.dwOutsideConeAngle, g.lConeOutsideVolume,
      fs(g.flMinDistance, a), fs(g.flMaxDistance, b), g.dwMode);
}

static void op_listener(void)
{
    int i = any_buf();
    if (i < 0 || !s_buf[i].lst) return;
    IDirectSound3DListener *l = s_buf[i].lst;
    char a[32], b[32], c[32];
    HRESULT hr;
    int commit = 0;
    DWORD ap = PICK(k_apply);
    switch (rn(8)) {
    case 0: { float x = fval(), y = fval(), z = fval(); hr = IDirectSound3DListener_SetPosition(l, x, y, z, ap);
              P("%d %c%d L.SetPosition(%s,%s,%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), fs(y, b), fs(z, c), ap, hr); break; }
    case 1: { float x = fval(), y = fval(), z = fval(); hr = IDirectSound3DListener_SetVelocity(l, x, y, z, ap);
              P("%d %c%d L.SetVelocity(%s,%s,%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), fs(y, b), fs(z, c), ap, hr); break; }
    case 2: { float v[6]; for (int k = 0; k < 6; k++) v[k] = chance(30) ? fval() : (float)PICK(((int[]){ 0, 0, 1, -1 }));
              hr = IDirectSound3DListener_SetOrientation(l, v[0], v[1], v[2], v[3], v[4], v[5], ap);
              char d2[32], e2[32], f2[32];
              P("%d %c%d L.SetOrientation(%s,%s,%s / %s,%s,%s, %lx) %08lx", s_op, BL(i), i, fs(v[0], a), fs(v[1], b), fs(v[2], c),
                fs(v[3], d2), fs(v[4], e2), fs(v[5], f2), ap, hr); break; }
    case 3: { float x = fval(); hr = IDirectSound3DListener_SetDistanceFactor(l, x, ap);
              P("%d %c%d L.SetDistanceFactor(%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), ap, hr); break; }
    case 4: { float x = fval(); hr = IDirectSound3DListener_SetDopplerFactor(l, x, ap);
              P("%d %c%d L.SetDopplerFactor(%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), ap, hr); break; }
    case 5: { float x = fval(); hr = IDirectSound3DListener_SetRolloffFactor(l, x, ap);
              P("%d %c%d L.SetRolloffFactor(%s, %lx) %08lx", s_op, BL(i), i, fs(x, a), ap, hr); break; }
    case 6: { hr = IDirectSound3DListener_CommitDeferredSettings(l); commit = 1; P("%d %c%d L.Commit %08lx", s_op, BL(i), i, hr); break; }
    default: {
        DS3DLISTENER p; memset(&p, 0, sizeof(p));
        p.dwSize = chance(85) ? sizeof(p) : PICK(((DWORD[]){ 0, sizeof(p) - 1, sizeof(p) + 4 }));
        p.vPosition.x = fval(); p.vPosition.y = fval(); p.vPosition.z = fval();
        p.vVelocity.x = fval(); p.vVelocity.y = fval(); p.vVelocity.z = fval();
        p.vOrientFront.x = (float)rn(2); p.vOrientFront.z = chance(50) ? 1.0f : fval();
        p.vOrientTop.y = chance(80) ? 1.0f : fval();
        p.flDistanceFactor = chance(50) ? 1.0f : fval(); p.flRolloffFactor = chance(50) ? 1.0f : fval();
        p.flDopplerFactor = chance(50) ? 1.0f : fval();
        DS3DLISTENER in = p;
        hr = IDirectSound3DListener_SetAllParameters(l, chance(97) ? &p : NULL, ap);
        p = in;
        P("%d %c%d L.SetAllParameters(size %lu", s_op, BL(i), i, p.dwSize);
        pv("pos", &p.vPosition); pv("vel", &p.vVelocity); pv("front", &p.vOrientFront); pv("top", &p.vOrientTop);
        P(" dist %s roll %s dopp %s, %lx) %08lx", fs(p.flDistanceFactor, a), fs(p.flRolloffFactor, b), fs(p.flDopplerFactor, c), ap, hr);
    }
    }
    if (SUCCEEDED(hr) && ap != DS3D_IMMEDIATE && !commit) s_deferred = 1;
    if (commit && SUCCEEDED(hr)) s_deferred = 0;
    if (s_deferred) { P(" | deferred\n"); return; }
    DS3DLISTENER g; memset(&g, 0, sizeof(g));
    g.dwSize = chance(90) ? sizeof(g) : PICK(((DWORD[]){ 0, sizeof(g) - 1, sizeof(g) + 4 }));
    hr = IDirectSound3DListener_GetAllParameters(l, &g);
    P(" | get(size %lu) %08lx", g.dwSize, hr);
    pv("pos", &g.vPosition); pv("vel", &g.vVelocity); pv("front", &g.vOrientFront); pv("top", &g.vOrientTop);
    P(" dist %s roll %s dopp %s\n", fs(g.flDistanceFactor, a), fs(g.flRolloffFactor, b), fs(g.flDopplerFactor, c));
}

static void op_ds(void)
{
    int which = chance(80) ? 0 : 1;
    IDirectSound8 *ds = s_ds[which];
    if (!ds) return;
    HRESULT hr;
    switch (rn(6)) {
    case 0: { DWORD l = PICK(k_levels); HWND w = chance(95) ? GetDesktopWindow() : NULL;
              hr = IDirectSound8_SetCooperativeLevel(ds, w, l);
              P("%d ds%d SetCooperativeLevel(%s, %lx) %08lx\n", s_op, which, w ? "hwnd" : "NULL", l, hr); break; }
    case 1: P("%d ds%d Compact %08lx\n", s_op, which, IDirectSound8_Compact(ds)); break;
    case 2: { DWORD c = PICK(k_speakers); hr = IDirectSound8_SetSpeakerConfig(ds, c);
              DWORD g = 0xdead; HRESULT h2 = IDirectSound8_GetSpeakerConfig(ds, chance(95) ? &g : NULL);
              P("%d ds%d SetSpeakerConfig(%lx) %08lx get %08lx\n", s_op, which, c, hr, h2); break; }
    case 3: P("%d ds%d Initialize %08lx\n", s_op, which, IDirectSound8_Initialize(ds, NULL)); break;
    case 4: { DSCAPS c; memset(&c, 0, sizeof(c));
              c.dwSize = chance(85) ? sizeof(c) : PICK(((DWORD[]){ 0, sizeof(c) - 1, sizeof(c) + 4 }));
              hr = IDirectSound8_GetCaps(ds, chance(97) ? &c : NULL);
              P("%d ds%d GetCaps(size %lu) %08lx min %lu max %lu\n", s_op, which, c.dwSize, hr, c.dwMinSecondarySampleRate,
                c.dwMaxSecondarySampleRate); break; }
    default: if (which == 0) { DWORD c = 0xdead; hr = IDirectSound8_VerifyCertification(ds, chance(95) ? &c : NULL);
                               P("%d ds%d VerifyCertification %08lx\n", s_op, which, hr); }
    }
}

typedef void (*opfn)(void);
static const struct { opfn f; int w; } k_ops[] = {
    { op_create, 14 }, { op_duplicate, 4 }, { op_release, 5 }, { op_refs, 2 }, { op_qi, 3 }, { op_caps, 2 },
    { op_getformat, 3 }, { op_setformat, 4 }, { op_volpanfreq, 8 }, { op_play, 6 }, { op_stop, 4 }, { op_position, 6 },
    { op_lock, 10 }, { op_status_misc, 4 }, { op_fx, 4 }, { op_notify, 4 }, { op_3d, 10 }, { op_listener, 6 }, { op_ds, 5 },
};

static void run_seed(unsigned seed, int ops)
{
    s_rng = 0x9e3779b97f4a7c15ull ^ ((unsigned long long)seed * 0x100000001b3ull);
    for (int k = 0; k < 8; k++) rnd();
    P("SEED %u\n", seed);
    s_deferred = 0;
    HRESULT hr = DirectSoundCreate8(NULL, &s_ds[0], NULL);
    P("DirectSoundCreate8 %08lx\n", hr);
    hr = DirectSoundCreate(NULL, (IDirectSound **)&s_ds[1], NULL);
    P("DirectSoundCreate %08lx\n", hr);
    for (int k = 0; k < 2; k++)
        if (s_ds[k]) IDirectSound8_SetCooperativeLevel(s_ds[k], GetDesktopWindow(), chance(80) ? DSSCL_PRIORITY : DSSCL_NORMAL);
    int total = 0;
    for (unsigned k = 0; k < sizeof(k_ops) / sizeof(k_ops[0]); k++) total += k_ops[k].w;
    for (s_op = 0; s_op < ops; s_op++) {
        int r = rn(total);
        unsigned k = 0;
        while (r >= k_ops[k].w) r -= k_ops[k].w, k++;
        if (s_trace) { fprintf(stderr, "op %d: %u\n", s_op, k); fflush(stderr); }
        k_ops[k].f();
        fflush(stdout);
    }
    for (int i = 0; i < NBUF; i++) if (s_buf[i].b) drop(i);
    for (int k = 0; k < 2; k++) if (s_ds[k]) { P("ds%d Release %lu\n", k, IDirectSound8_Release(s_ds[k])); s_ds[k] = NULL; }
    fflush(stdout);
}

/* ------------------------------------------------------------------ `formats`: CreateSoundBuffer over a grid of formats */
static void try_format(int which, DWORD flags, const WAVEFORMATEXTENSIBLE *e, const char *what)
{
    DSBUFFERDESC d; memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d); d.dwFlags = flags; d.dwBufferBytes = 4096 * 3; d.lpwfxFormat = (WAVEFORMATEX *)e;
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(s_ds[which], &d, &b, NULL);
    P("ds%d %lx %s: ", which, flags, what);
    print_format_in(&e->Format);
    if (e->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        P(" valid %u mask %lx sub %08lx", e->Samples.wValidBitsPerSample, e->dwChannelMask, e->SubFormat.Data1);
    P(" -> %08lx", hr);
    if (b) { show_caps(b); show_format(b); IDirectSoundBuffer_Release(b); }
    P("\n");
}

static void base_format(WAVEFORMATEXTENSIBLE *e, WORD tag, WORD ch, WORD bits, DWORD rate)
{
    memset(e, 0, sizeof(*e));
    e->Format.wFormatTag = tag; e->Format.nChannels = ch; e->Format.wBitsPerSample = bits; e->Format.nSamplesPerSec = rate;
    e->Format.nBlockAlign = (WORD)(ch * bits / 8); e->Format.nAvgBytesPerSec = e->Format.nBlockAlign * rate;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        e->Format.cbSize = 22; e->Samples.wValidBitsPerSample = bits;
        e->dwChannelMask = ch == 1 ? 4 : ch == 2 ? 3 : ch == 4 ? 0x33 : ch == 6 ? 0x3f : ch == 8 ? 0x63f : 0;
        e->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    }
}

static void formats(void)
{
    DirectSoundCreate8(NULL, &s_ds[0], NULL);
    DirectSoundCreate(NULL, (IDirectSound **)&s_ds[1], NULL);
    for (int k = 0; k < 2; k++) IDirectSound8_SetCooperativeLevel(s_ds[k], GetDesktopWindow(), DSSCL_PRIORITY);
    static const WORD tags[] = { WAVE_FORMAT_PCM, WAVE_FORMAT_IEEE_FLOAT, WAVE_FORMAT_EXTENSIBLE, 0, 2, 0x55 };
    static const WORD chans[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16 };
    static const WORD bits[] = { 0, 1, 4, 8, 12, 16, 20, 24, 32, 48, 64 };
    static const DWORD rates[] = { 0, 99, 100, 101, 22050, 200000, 200001, 384000 };
    static const DWORD flags[] = { 0, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY, DSBCAPS_CTRL3D,
                                   DSBCAPS_CTRLFX, DSBCAPS_STATIC, DSBCAPS_LOCSOFTWARE, DSBCAPS_LOCHARDWARE };
    WAVEFORMATEXTENSIBLE e;
    for (int w = 0; w < 2; w++)
        for (unsigned f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
            for (unsigned t = 0; t < sizeof(tags) / sizeof(tags[0]); t++)
                for (unsigned c = 0; c < sizeof(chans) / sizeof(chans[0]); c++)
                    for (unsigned b = 0; b < sizeof(bits) / sizeof(bits[0]); b++) {
                        if (f > 1 && (c > 4 || b > 6)) continue;
                        WORD bb = tags[t] == WAVE_FORMAT_IEEE_FLOAT && b == 0 ? 32 : bits[b];
                        if (flags[f] & DSBCAPS_CTRLFX && (chans[c] < 1 || chans[c] > 2 || (bits[b] != 8 && bits[b] != 16 && bb != 32) ||
                                                          (tags[t] != WAVE_FORMAT_PCM && tags[t] != WAVE_FORMAT_IEEE_FLOAT)))
                            continue;           /* (Windows 7 divides by zero on some of these) */
                        base_format(&e, tags[t], chans[c], bb, 22050);
                        if (tags[t] == WAVE_FORMAT_EXTENSIBLE && bb == 32 && c & 1) e.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                        try_format(w, flags[f], &e, "grid");
                    }
            for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
                base_format(&e, WAVE_FORMAT_PCM, 2, 16, rates[r]); try_format(w, flags[f], &e, "rate");
                base_format(&e, WAVE_FORMAT_PCM, 1, 8, rates[r]); try_format(w, flags[f], &e, "rate");
            }
            /* one field off at a time, from 16-bit stereo and mono 44100 */
            for (int ch = 1; ch <= 2; ch++) for (int tg = 0; tg < 3; tg++) {
                WORD tag = tags[tg], bb = tag == WAVE_FORMAT_IEEE_FLOAT ? 32 : 16;
                if (flags[f] & DSBCAPS_CTRLFX && tag == WAVE_FORMAT_EXTENSIBLE) continue;
                static const int da[] = { -2, -1, 1, 2, 100 };
                for (unsigned k = 0; k < 5; k++) { base_format(&e, tag, (WORD)ch, bb, 44100); e.Format.nBlockAlign = (WORD)(e.Format.nBlockAlign + da[k]); try_format(w, flags[f], &e, "align"); }
                base_format(&e, tag, (WORD)ch, bb, 44100); e.Format.nBlockAlign = 0; try_format(w, flags[f], &e, "align");
                static const int dv[] = { -1, 1, 1000, -100000 };
                for (unsigned k = 0; k < 4; k++) { base_format(&e, tag, (WORD)ch, bb, 44100); e.Format.nAvgBytesPerSec += dv[k]; try_format(w, flags[f], &e, "avg"); }
                base_format(&e, tag, (WORD)ch, bb, 44100); e.Format.nAvgBytesPerSec = 0; try_format(w, flags[f], &e, "avg");
                static const WORD cbs[] = { 1, 2, 21, 22, 23, 100, 0xffff };
                for (unsigned k = 0; k < 7; k++) { base_format(&e, tag, (WORD)ch, bb, 44100); e.Format.cbSize = cbs[k]; try_format(w, flags[f], &e, "cb"); }
                if (tag == WAVE_FORMAT_EXTENSIBLE) {
                    static const WORD vb[] = { 0, 1, 8, 12, 15, 17, 24 };
                    for (unsigned k = 0; k < 7; k++) { base_format(&e, tag, (WORD)ch, bb, 44100); e.Samples.wValidBitsPerSample = vb[k]; try_format(w, flags[f], &e, "valid"); }
                    static const DWORD mk[] = { 0, 1, 2, 3, 4, 7, 0x3f, 0x80000000, 0xffffffff };
                    for (unsigned k = 0; k < 9; k++) { base_format(&e, tag, (WORD)ch, bb, 44100); e.dwChannelMask = mk[k]; try_format(w, flags[f], &e, "mask"); }
                    base_format(&e, tag, (WORD)ch, bb, 44100); e.SubFormat = GUID_NULL; try_format(w, flags[f], &e, "sub");
                    base_format(&e, tag, (WORD)ch, bb, 44100); e.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT; try_format(w, flags[f], &e, "sub");
                    base_format(&e, tag, (WORD)ch, 32, 44100); e.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT; e.Samples.wValidBitsPerSample = 24; try_format(w, flags[f], &e, "sub");
                }
            }
        }
    for (int k = 0; k < 2; k++) IDirectSound8_Release(s_ds[k]);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "formats")) { CoInitialize(NULL); formats(); P("END\n"); return 0; }
    unsigned first = argc > 1 ? (unsigned)atoi(argv[1]) : 1, n = argc > 2 ? (unsigned)atoi(argv[2]) : 20;
    int ops = argc > 3 ? atoi(argv[3]) : 200;
    CoInitialize(NULL);
    s_trace = getenv("DSFUZZ_TRACE") != NULL;
    for (int k = 0; k < 4; k++) s_event[k] = CreateEventA(NULL, FALSE, FALSE, NULL);
    for (unsigned s = first; s < first + n; s++) run_seed(s, ops);
    P("END\n");
    CoUninitialize();
    return 0;
}
