/* primtest - the primary buffer, and writing to it at DSSCL_WRITEPRIMARY, against Microsoft's DirectSound: every
 * HRESULT and out-parameter.  Run with tests/comcmp.sh primtest.exe [levels|api|secondary|more|object|refs]; not in the default
 * run: `audio` plays a tone through the primary buffer for recording the output, `overrelease` (crashes a DirectSound
 * that frees a buffer still in use), `slack` (the readable memory after a buffer's data: the heap's, not comparable). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

static HWND wnd(void) { return GetDesktopWindow(); }

static IDirectSound8 *make_ds(void)
{
    IDirectSound8 *ds = NULL;
    HRESULT hr = DirectSoundCreate8(NULL, &ds, NULL);
    if (FAILED(hr)) P("DirectSoundCreate8 %08lx\n", hr);
    return ds;
}
static IDirectSoundBuffer *make_primary(IDirectSound8 *ds, DWORD flags, HRESULT *phr)
{
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DSBCAPS_PRIMARYBUFFER | flags;
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(ds, &d, &b, NULL);
    if (phr) *phr = hr;
    return b;
}
static void caps(const char *what, IDirectSoundBuffer *b)
{
    DSBCAPS c;
    memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    HRESULT hr = IDirectSoundBuffer_GetCaps(b, &c);
    P("  %s GetCaps %08lx flags %lx bytes %lu\n", what, hr, c.dwFlags, c.dwBufferBytes);
}
static void format(const char *what, IDirectSoundBuffer *b)
{
    WAVEFORMATEX f;
    DWORD got = 0;
    memset(&f, 0, sizeof(f));
    HRESULT hr = IDirectSoundBuffer_GetFormat(b, &f, sizeof(f), &got);
    P("  %s GetFormat %08lx tag %u ch %u rate %lu bits %u align %u avg %lu got %lu\n", what, hr, f.wFormatTag, f.nChannels,
      f.nSamplesPerSec, f.wBitsPerSample, f.nBlockAlign, f.nAvgBytesPerSec, got);
}
static void status(const char *what, IDirectSoundBuffer *b)
{
    DWORD s = 0xdead;
    HRESULT hr = IDirectSoundBuffer_GetStatus(b, &s);
    P("  %s GetStatus %08lx %lx\n", what, hr, s);
}
static HRESULT lock(const char *what, IDirectSoundBuffer *b, DWORD off, DWORD n, DWORD flags, BOOL unlock)
{
    void *p1 = (void *)1, *p2 = (void *)1;
    DWORD b1 = 0xdead, b2 = 0xdead;
    HRESULT hr = IDirectSoundBuffer_Lock(b, off, n, &p1, &b1, &p2, &b2, flags);
    P("  %s Lock(%lu, %lu, %lx) %08lx p1 %s b1 %lu p2 %s b2 %lu\n", what, off, n, flags, hr,
      p1 == (void *)1 ? "untouched" : p1 ? "set" : "NULL", b1, p2 == (void *)1 ? "untouched" : p2 ? "set" : "NULL", b2);
    if (SUCCEEDED(hr) && unlock) P("    Unlock %08lx\n", IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2));
    return hr;
}
static WAVEFORMATEX pcm(DWORD rate, WORD ch, WORD bits)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, ch, rate, rate * ch * bits / 8, (WORD)(ch * bits / 8), bits, 0 };
    return f;
}

/* ------------------------------------------------------------------ the primary buffer at each level */
static void levels(void)
{
    static const struct { DWORD l; const char *n; } lv[] = {
        { 0, "none" }, { DSSCL_NORMAL, "NORMAL" }, { DSSCL_PRIORITY, "PRIORITY" }, { DSSCL_EXCLUSIVE, "EXCLUSIVE" },
        { DSSCL_WRITEPRIMARY, "WRITEPRIMARY" },
    };
    for (unsigned k = 0; k < sizeof(lv) / sizeof(lv[0]); k++) {
        IDirectSound8 *ds = make_ds();
        if (!ds) return;
        P("%s:\n", lv[k].n);
        if (lv[k].l) P("  SetCooperativeLevel %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), lv[k].l));
        HRESULT hr;
        IDirectSoundBuffer *b = make_primary(ds, 0, &hr);
        P("  CreateSoundBuffer(primary) %08lx %s\n", hr, b ? "buffer" : "none");
        if (b) {
            caps("primary", b);
            format("primary", b);
            status("primary", b);
            lock("primary", b, 0, 0, DSBLOCK_ENTIREBUFFER, TRUE);
            lock("primary", b, 0, 16, 0, TRUE);
            WAVEFORMATEX f = pcm(44100, 2, 16);
            P("  SetFormat(44100 2 16) %08lx\n", IDirectSoundBuffer_SetFormat(b, &f));
            format("primary", b);
            caps("primary", b);
            P("  Play(LOOPING) %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING));
            status("primary", b);
            P("  Stop %08lx\n", IDirectSoundBuffer_Stop(b));
            IDirectSoundBuffer_Release(b);
        }
        IDirectSound8_Release(ds);
    }
}

/* ------------------------------------------------------------------ the buffer's methods at WRITEPRIMARY */
static void api(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    P("  SetCooperativeLevel(WRITEPRIMARY) %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_WRITEPRIMARY));
    HRESULT hr;
    IDirectSoundBuffer *b = make_primary(ds, DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN, &hr);
    P("  CreateSoundBuffer(primary, volume, pan) %08lx\n", hr);
    if (!b) { IDirectSound8_Release(ds); return; }
    IDirectSoundBuffer *b2 = make_primary(ds, 0, &hr);
    P("  CreateSoundBuffer(primary again) %08lx %s\n", hr, b2 ? (b2 == b ? "the same" : "another") : "none");
    if (b2) IDirectSoundBuffer_Release(b2);
    caps("primary", b);
    static const struct { WORD ch, bits; DWORD rate; } fm[] = {
        { 2, 16, 22050 }, { 1, 8, 11025 }, { 2, 16, 48000 }, { 1, 16, 44100 }, { 2, 8, 22050 }, { 6, 16, 44100 },
        { 2, 32, 44100 }, { 2, 16, 200000 }, { 2, 16, 44100 },
    };
    for (unsigned k = 0; k < sizeof(fm) / sizeof(fm[0]); k++) {
        WAVEFORMATEX f = pcm(fm[k].rate, fm[k].ch, fm[k].bits);
        hr = IDirectSoundBuffer_SetFormat(b, &f);
        DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
        IDirectSoundBuffer_GetCaps(b, &c);
        WAVEFORMATEX g; memset(&g, 0, sizeof(g));
        IDirectSoundBuffer_GetFormat(b, &g, sizeof(g), NULL);
        P("  SetFormat(%lu %u %u) %08lx: now %lu %u %u, %lu bytes (%lu ms)\n", fm[k].rate, fm[k].ch, fm[k].bits, hr,
          g.nSamplesPerSec, g.nChannels, g.wBitsPerSample, c.dwBufferBytes,
          g.nAvgBytesPerSec ? c.dwBufferBytes * 1000 / g.nAvgBytesPerSec : 0);
    }
    P("  SetFormat(NULL) %08lx\n", IDirectSoundBuffer_SetFormat(b, NULL));
    DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    IDirectSoundBuffer_GetCaps(b, &c);
    DWORD size = c.dwBufferBytes;
    lock("entire", b, 0, 0, DSBLOCK_ENTIREBUFFER, TRUE);
    lock("start", b, 0, 100, 0, TRUE);
    lock("wrapping", b, size - 40, 100, 0, TRUE);
    lock("whole size", b, 0, size, 0, TRUE);
    lock("past the end", b, size, 4, 0, TRUE);
    lock("too long", b, 0, size + 4, 0, TRUE);
    lock("0 bytes", b, 0, 0, 0, TRUE);
    lock("odd offset", b, 3, 10, 0, TRUE);
    lock("from write cursor", b, 0, 64, DSBLOCK_FROMWRITECURSOR, TRUE);
    {   /* two at once */
        void *p1, *p2, *q1, *q2; DWORD b1, b2, c1, c2;
        HRESULT h1 = IDirectSoundBuffer_Lock(b, 0, 64, &p1, &b1, &p2, &b2, 0);
        HRESULT h2 = IDirectSoundBuffer_Lock(b, 128, 64, &q1, &c1, &q2, &c2, 0);
        P("  two locks: %08lx %08lx%s\n", h1, h2, SUCCEEDED(h1) && SUCCEEDED(h2) ? (q1 == (BYTE *)p1 + 128 ? ", the same memory" : ", other memory") : "");
        if (SUCCEEDED(h2)) P("    Unlock second %08lx\n", IDirectSoundBuffer_Unlock(b, q1, c1, q2, c2));
        if (SUCCEEDED(h1)) P("    Unlock first %08lx\n", IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2));
        h1 = IDirectSoundBuffer_Lock(b, 0, 64, &p1, &b1, &p2, &b2, 0);
        if (SUCCEEDED(h1)) {
            P("    Unlock(other pointer) %08lx\n", IDirectSoundBuffer_Unlock(b, (BYTE *)p1 + 8, b1, p2, b2));
            P("    Unlock(more bytes) %08lx\n", IDirectSoundBuffer_Unlock(b, p1, b1 + 64, p2, b2));
            P("    Unlock %08lx\n", IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2));
        }
        P("  Unlock(nothing locked) %08lx\n", IDirectSoundBuffer_Unlock(b, p1, b1, NULL, 0));
        void *n1; DWORD nb1;
        P("  Lock(NULL second pointers) %08lx\n", IDirectSoundBuffer_Lock(b, 0, 64, &n1, &nb1, NULL, NULL, 0));
        IDirectSoundBuffer_Unlock(b, n1, nb1, NULL, 0);
        P("  Lock(NULL first pointer) %08lx\n", IDirectSoundBuffer_Lock(b, 0, 64, NULL, &nb1, NULL, NULL, 0));
    }
    DWORD pl = 0xdead, wr = 0xdead;
    hr = IDirectSoundBuffer_GetCurrentPosition(b, &pl, &wr);
    P("  stopped: GetCurrentPosition %08lx play %lu write %lu\n", hr, pl, wr);
    P("  SetCurrentPosition(0) %08lx\n", IDirectSoundBuffer_SetCurrentPosition(b, 0));
    P("  SetFrequency(22050) %08lx\n", IDirectSoundBuffer_SetFrequency(b, 22050));
    LONG v = 1;
    P("  SetVolume(-600) %08lx GetVolume %08lx", IDirectSoundBuffer_SetVolume(b, -600), IDirectSoundBuffer_GetVolume(b, &v));
    P(" %ld\n", v);
    IDirectSoundBuffer_SetVolume(b, 0);
    P("  SetPan(100) %08lx GetPan %08lx", IDirectSoundBuffer_SetPan(b, 100), IDirectSoundBuffer_GetPan(b, &v));
    P(" %ld\n", v);
    IDirectSoundBuffer_SetPan(b, 0);
    P("  Play(0) %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, 0));
    status("Play(0)", b);
    IDirectSoundBuffer_Stop(b);
    P("  Play(LOOPING) %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING));
    status("playing", b);
    WAVEFORMATEX f = pcm(22050, 2, 16);
    P("  SetFormat while playing %08lx\n", IDirectSoundBuffer_SetFormat(b, &f));
    format("after", b);
    /* the cursors: moving, the write cursor ahead of the play cursor, both within the buffer */
    memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    IDirectSoundBuffer_GetCaps(b, &c);
    size = c.dwBufferBytes;
    WAVEFORMATEX g; memset(&g, 0, sizeof(g));
    IDirectSoundBuffer_GetFormat(b, &g, sizeof(g), NULL);
    DWORD moved = 0, lastp = 0, inside = 1, aligned = 1, maxlead = 0, n = 0;
    DWORD t0 = GetTickCount();
    unsigned long long total = 0;
    while (GetTickCount() - t0 < 1000) {
        pl = wr = 0;
        IDirectSoundBuffer_GetCurrentPosition(b, &pl, &wr);
        if (pl >= size || wr >= size) inside = 0;
        if (pl % g.nBlockAlign || wr % g.nBlockAlign) aligned = 0;
        DWORD lead = (wr + size - pl) % size;
        if (lead > maxlead) maxlead = lead;
        if (n && pl != lastp) { moved++; total += (pl + size - lastp) % size; }
        lastp = pl; n++;
        Sleep(5);
    }
    DWORD ms = (DWORD)(total * 1000 / g.nAvgBytesPerSec);
    P("  playing: the play cursor moves %s, %s, %s; in 1 s it covers %s; write lead up to %s\n", moved ? "yes" : "no",
      inside ? "inside the buffer" : "OUTSIDE", aligned ? "block aligned" : "NOT ALIGNED",
      ms > 850 && ms < 1100 ? "about 1 s" : "something else", maxlead * 1000 / g.nAvgBytesPerSec < 100 ? "< 100 ms" : ">= 100 ms");
    lock("playing", b, 0, 64, 0, TRUE);
    P("  Stop %08lx\n", IDirectSoundBuffer_Stop(b));
    status("stopped", b);
    IDirectSoundBuffer_GetCurrentPosition(b, &pl, &wr);
    Sleep(100);
    DWORD pl2 = 0, wr2 = 0;
    IDirectSoundBuffer_GetCurrentPosition(b, &pl2, &wr2);
    P("  stopped: the play cursor %s\n", pl2 == pl ? "stays" : "moves");
    P("  Restore %08lx\n", IDirectSoundBuffer_Restore(b));
    {
        IUnknown *u = NULL;
        P("  QueryInterface(IDirectSoundBuffer8) %08lx\n", IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundBuffer8, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        P("  QueryInterface(IDirectSound3DListener) %08lx\n", IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DListener, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
    }
    /* the level lowered while the buffer is out */
    P("  SetCooperativeLevel(PRIORITY) %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY));
    lock("at PRIORITY", b, 0, 64, 0, TRUE);
    status("at PRIORITY", b);
    P("  Play at PRIORITY %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING));
    IDirectSoundBuffer_Stop(b);
    P("  SetCooperativeLevel(WRITEPRIMARY) %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_WRITEPRIMARY));
    lock("WRITEPRIMARY again", b, 0, 64, 0, TRUE);
    caps("WRITEPRIMARY again", b);
    ULONG r = IDirectSoundBuffer_Release(b);
    P("  Release %lu\n", r);
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ secondary buffers alongside */
static void secondary(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    P("  SetCooperativeLevel(WRITEPRIMARY) %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_WRITEPRIMARY));
    WAVEFORMATEX f = pcm(22050, 2, 16);
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = 22050 * 4; d.lpwfxFormat = &f;
    IDirectSoundBuffer *s = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(ds, &d, &s, NULL);
    P("  CreateSoundBuffer(secondary, before the primary) %08lx\n", hr);
    if (s) {
        lock("secondary", s, 0, 64, 0, TRUE);
        P("  secondary Play %08lx\n", IDirectSoundBuffer_Play(s, 0, 0, DSBPLAY_LOOPING));
        status("secondary", s);
        IDirectSoundBuffer_Stop(s);
    }
    IDirectSoundBuffer *b = make_primary(ds, 0, &hr);
    P("  CreateSoundBuffer(primary) %08lx\n", hr);
    IDirectSoundBuffer *s2 = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &d, &s2, NULL);
    P("  CreateSoundBuffer(secondary, with the primary) %08lx\n", hr);
    if (s) {
        lock("first secondary", s, 0, 64, 0, TRUE);
        P("  first secondary Play %08lx\n", IDirectSoundBuffer_Play(s, 0, 0, DSBPLAY_LOOPING));
        status("first secondary", s);
        IDirectSoundBuffer_Stop(s);
    }
    if (s2) {
        P("  second secondary Play %08lx\n", IDirectSoundBuffer_Play(s2, 0, 0, DSBPLAY_LOOPING));
        status("second secondary", s2);
        IDirectSoundBuffer_Stop(s2);
        IDirectSoundBuffer_Release(s2);
    }
    if (b) {
        P("  primary Play %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING));
        if (s) {
            P("  secondary Play with the primary playing %08lx\n", IDirectSoundBuffer_Play(s, 0, 0, DSBPLAY_LOOPING));
            status("secondary", s);
            IDirectSoundBuffer_Stop(s);
        }
        IDirectSoundBuffer_Stop(b);
        IDirectSoundBuffer_Release(b);
    }
    if (s) IDirectSoundBuffer_Release(s);
    /* a second DirectSound object asking for write access too */
    IDirectSound8 *ds2 = make_ds();
    if (ds2) {
        P("  second object SetCooperativeLevel(WRITEPRIMARY) %08lx\n", IDirectSound8_SetCooperativeLevel(ds2, wnd(), DSSCL_WRITEPRIMARY));
        IDirectSoundBuffer *b2 = make_primary(ds2, 0, &hr);
        P("  second object CreateSoundBuffer(primary) %08lx\n", hr);
        if (b2) { lock("second object's primary", b2, 0, 64, 0, TRUE); IDirectSoundBuffer_Release(b2); }
        IDirectSound8_Release(ds2);
    }
    IDirectSound8_Release(ds);
}


/* ------------------------------------------------------------------ more of the primary's behaviour at each level */
static void cursor(const char *what, IDirectSoundBuffer *b)
{
    DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    IDirectSoundBuffer_GetCaps(b, &c);
    DWORD p0 = 0, w0 = 0, p1 = 0, w1 = 0;
    HRESULT hr = IDirectSoundBuffer_GetCurrentPosition(b, &p0, &w0);
    int moved = 0;
    for (int k = 0; k < 100 && !moved; k++) {
        Sleep(10);
        IDirectSoundBuffer_GetCurrentPosition(b, &p1, &w1);
        if (p1 != p0) moved = 1;
    }
    P("  %s GetCurrentPosition %08lx: %s\n", what, hr, moved ? "moves" : p1 || w1 ? "stays, not at 0" : "stays at 0");
}
static void more(void)
{
    static const struct { DWORD l; const char *n; } lv[] = {
        { DSSCL_NORMAL, "NORMAL" }, { DSSCL_PRIORITY, "PRIORITY" }, { DSSCL_WRITEPRIMARY, "WRITEPRIMARY" },
    };
    for (unsigned k = 0; k < sizeof(lv) / sizeof(lv[0]); k++) {
        IDirectSound8 *ds = make_ds();
        if (!ds) return;
        P("%s:\n", lv[k].n);
        IDirectSound8_SetCooperativeLevel(ds, wnd(), lv[k].l);
        HRESULT hr;
        IDirectSoundBuffer *b = make_primary(ds, DSBCAPS_CTRLVOLUME, &hr);
        if (!b) { IDirectSound8_Release(ds); continue; }
        ULONG r1 = IDirectSoundBuffer_AddRef(b), r2 = IDirectSoundBuffer_Release(b);
        P("  AddRef %lu Release %lu\n", r1, r2);
        IUnknown *u = NULL;
        P("  QueryInterface(IDirectSound3DListener) without CTRL3D %08lx\n", IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DListener, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        LONG v = 1;
        P("  SetVolume(-600) %08lx", IDirectSoundBuffer_SetVolume(b, -600));
        P(" GetVolume %08lx %ld\n", IDirectSoundBuffer_GetVolume(b, &v), v);
        IDirectSoundBuffer_SetVolume(b, 0);
        P("  Unlock(nothing locked) %08lx\n", IDirectSoundBuffer_Unlock(b, (void *)0x1000, 4, NULL, 0));
        void *p1 = NULL, *p2; DWORD b1 = 0, b2;
        if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER))) {
            BYTE *d = p1; DWORD nz = 0;
            for (DWORD i = 0; i < b1; i++) if (d[i] != d[0]) nz++;
            P("  the buffer at first: bytes %02x, %s\n", d[0], nz ? "not all the same" : "all the same");
            memset(d, 0x11, b1);
            IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
            WAVEFORMATEX f = pcm(44100, 2, 16);
            P("  SetFormat %08lx\n", IDirectSoundBuffer_SetFormat(b, &f));
            IDirectSoundBuffer_Lock(b, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER);
            d = p1;
            P("  after SetFormat: bytes %02x\n", d[0]);
            IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
        }
        P("  Play(0) %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, 0));
        status("Play(0)", b);
        cursor("Play(0)", b);
        IDirectSoundBuffer_Stop(b);
        P("  Play(LOOPING) %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING));
        cursor("Play(LOOPING)", b);
        DWORD p0 = 0, w0 = 0;
        IDirectSoundBuffer_Stop(b);
        IDirectSoundBuffer_GetCurrentPosition(b, &p0, &w0);
        Sleep(100);
        IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
        DWORD pa = 0, wa = 0;
        IDirectSoundBuffer_GetCurrentPosition(b, &pa, &wa);
        P("  Play after Stop: the play cursor %s\n", pa < p0 && pa < 2000 ? "back near 0" : pa >= p0 ? "from where it stopped" : "elsewhere");
        IDirectSoundBuffer_Stop(b);
        /* secondary buffers made before the level changes to WRITEPRIMARY, and back */
        WAVEFORMATEX f = pcm(22050, 2, 16);
        DSBUFFERDESC d;
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = 22050 * 4; d.lpwfxFormat = &f;
        IDirectSoundBuffer *s = NULL;
        IDirectSound8_CreateSoundBuffer(ds, &d, &s, NULL);
        if (s && lv[k].l != DSSCL_WRITEPRIMARY) {
            IDirectSoundBuffer_Play(s, 0, 0, DSBPLAY_LOOPING);
            P("  to WRITEPRIMARY %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_WRITEPRIMARY));
            status("playing secondary", s);
            lock("secondary", s, 0, 64, 0, TRUE);
            P("  Restore %08lx\n", IDirectSoundBuffer_Restore(s));
            P("  back %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), lv[k].l));
            status("secondary", s);
            lock("secondary", s, 0, 64, 0, TRUE);
            P("  Play %08lx\n", IDirectSoundBuffer_Play(s, 0, 0, DSBPLAY_LOOPING));
            P("  Restore %08lx\n", IDirectSoundBuffer_Restore(s));
            status("secondary", s);
            lock("secondary", s, 0, 64, 0, TRUE);
            P("  Play %08lx\n", IDirectSoundBuffer_Play(s, 0, 0, DSBPLAY_LOOPING));
            status("secondary", s);
            IDirectSoundBuffer_Stop(s);
        } else if (s) {
            P("  to PRIORITY %08lx\n", IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY));
            status("secondary", s);
            lock("secondary", s, 0, 64, 0, TRUE);
            P("  Restore %08lx\n", IDirectSoundBuffer_Restore(s));
            status("secondary", s);
            lock("secondary", s, 0, 64, 0, TRUE);
            P("  Play %08lx\n", IDirectSoundBuffer_Play(s, 0, 0, DSBPLAY_LOOPING));
            status("secondary", s);
            IDirectSoundBuffer_Stop(s);
        }
        if (s) IDirectSoundBuffer_Release(s);
        P("  last Release %lu\n", IDirectSoundBuffer_Release(b));
        IDirectSound8_Release(ds);
    }
    /* the listener with CTRL3D, and a secondary buffer's Unlock */
    IDirectSound8 *ds = make_ds();
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    HRESULT hr;
    IDirectSoundBuffer *b = make_primary(ds, DSBCAPS_CTRL3D, &hr);
    P("primary with CTRL3D %08lx\n", hr);
    if (b) {
        IUnknown *u = NULL;
        P("  QueryInterface(IDirectSound3DListener) %08lx\n", IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DListener, (void **)&u));
        if (u) IUnknown_Release(u);
        IDirectSoundBuffer_Release(b);
    }
    WAVEFORMATEX f = pcm(22050, 2, 16);
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = 4096; d.lpwfxFormat = &f;
    IDirectSoundBuffer *s = NULL;
    IDirectSound8_CreateSoundBuffer(ds, &d, &s, NULL);
    if (s) {
        void *p1, *p2; DWORD b1, b2;
        P("secondary Unlock(nothing locked) %08lx\n", IDirectSoundBuffer_Unlock(s, (void *)0x1000, 4, NULL, 0));
        IDirectSoundBuffer_Lock(s, 0, 64, &p1, &b1, &p2, &b2, 0);
        P("  Unlock(other pointer) %08lx\n", IDirectSoundBuffer_Unlock(s, (BYTE *)p1 + 8, b1, p2, b2));
        P("  Unlock(more bytes) %08lx\n", IDirectSoundBuffer_Unlock(s, p1, b1 + 64, p2, b2));
        P("  Unlock %08lx\n", IDirectSoundBuffer_Unlock(s, p1, b1, p2, b2));
        P("  Lock(odd offset) %08lx\n", IDirectSoundBuffer_Lock(s, 3, 10, &p1, &b1, &p2, &b2, 0));
        IDirectSoundBuffer_Unlock(s, p1, b1, p2, b2);
        IDirectSoundBuffer_Release(s);
    }
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ one primary object: its flags and references */
static void object(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    HRESULT hr;
    IDirectSoundBuffer *a = make_primary(ds, 0, &hr);
    P("  first (no flags) %08lx\n", hr);
    caps("first", a);
    IDirectSoundBuffer *b = make_primary(ds, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME, &hr);
    P("  second (CTRL3D, CTRLVOLUME) %08lx %s\n", hr, b ? (b == a ? "the same" : "another") : "none");
    if (b) {
        caps("second", b);
        caps("first now", a);
        IUnknown *u = NULL;
        P("  second QueryInterface(listener) %08lx\n", IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DListener, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        P("  first QueryInterface(listener) %08lx\n", IDirectSoundBuffer_QueryInterface(a, &IID_IDirectSound3DListener, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        LONG v;
        P("  first SetVolume %08lx GetVolume %08lx\n", IDirectSoundBuffer_SetVolume(a, -100), IDirectSoundBuffer_GetVolume(a, &v));
        P("  Release second %lu\n", IDirectSoundBuffer_Release(b));
    }
    P("  Release first %lu\n", IDirectSoundBuffer_Release(a));
    IDirectSoundBuffer *c = make_primary(ds, DSBCAPS_CTRL3D, &hr);
    P("  again after both released (CTRL3D) %08lx %s\n", hr, c ? (c == a ? "the same address" : "another address") : "none");
    if (c) {
        caps("again", c);
        IUnknown *u = NULL;
        P("  QueryInterface(listener) %08lx\n", IDirectSoundBuffer_QueryInterface(c, &IID_IDirectSound3DListener, (void **)&u));
        if (u) IUnknown_Release(u);
        IDirectSoundBuffer *d = make_primary(ds, 0, &hr);
        if (d) { caps("then without", d); P("  Release %lu\n", IDirectSoundBuffer_Release(d)); }
        P("  Release %lu\n", IDirectSoundBuffer_Release(c));
    }
    /* a reference kept past the DirectSound object */
    IDirectSoundBuffer *k = make_primary(ds, 0, &hr);
    P("  DirectSound Release %lu\n", IDirectSound8_Release(ds));
    if (k) {
        status("kept", k);
        caps("kept", k);
        void *p1, *p2; DWORD b1, b2;
        P("  kept Lock %08lx\n", IDirectSoundBuffer_Lock(k, 0, 64, &p1, &b1, &p2, &b2, 0));
        P("  kept Play %08lx\n", IDirectSoundBuffer_Play(k, 0, 0, DSBPLAY_LOOPING));
        P("  kept Release %lu\n", IDirectSoundBuffer_Release(k));
    }
    /* at WRITEPRIMARY: the primary kept past the object */
    ds = make_ds();
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_WRITEPRIMARY);
    k = make_primary(ds, 0, &hr);
    if (k) IDirectSoundBuffer_Play(k, 0, 0, DSBPLAY_LOOPING);
    P("  WRITEPRIMARY DirectSound Release %lu\n", IDirectSound8_Release(ds));
    if (k) {
        status("kept", k);
        void *p1, *p2; DWORD b1, b2;
        HRESULT h = IDirectSoundBuffer_Lock(k, 0, 64, &p1, &b1, &p2, &b2, 0);
        P("  kept Lock %08lx\n", h);
        if (SUCCEEDED(h)) IDirectSoundBuffer_Unlock(k, p1, b1, p2, b2);
        DWORD pl = 0, wr = 0;
        P("  kept GetCurrentPosition %08lx\n", IDirectSoundBuffer_GetCurrentPosition(k, &pl, &wr));
        P("  kept Release %lu\n", IDirectSoundBuffer_Release(k));
    }
}

/* ------------------------------------------------------------------ what plays: a tone written to the primary buffer
 * (left: 215 Hz, 40 periods in the buffer; right: silent), for recording the output (it prints nothing more) */
static void audio(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_WRITEPRIMARY);
    HRESULT hr;
    IDirectSoundBuffer *b = make_primary(ds, 0, &hr);
    if (!b) { IDirectSound8_Release(ds); return; }
    WAVEFORMATEX f = pcm(44100, 2, 16);
    P("  SetFormat %08lx\n", IDirectSoundBuffer_SetFormat(b, &f));
    void *p1, *p2; DWORD b1, b2;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER))) {
        short *s = p1;
        DWORD n = b1 / 4;
        for (DWORD i = 0; i < n; i++) {
            s[2 * i] = (short)(16000.0 * sin(2.0 * 3.14159265358979 * 40.0 * i / n));
            s[2 * i + 1] = 0;
        }
        IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
    }
    Sleep(500);
    P("  Play %08lx\n", IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING));
    Sleep(2000);
    P("  Stop %08lx\n", IDirectSoundBuffer_Stop(b));
    Sleep(500);
    IDirectSoundBuffer_Release(b);
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ each interface's references (a secondary buffer's
 * IDirectSoundBuffer, IDirectSound3DBuffer, IDirectSoundNotify, IKsPropertySet), and what over-releasing one does */
static void refs(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    WAVEFORMATEX f = pcm(22050, 1, 16);
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = 22050 * 2; d.lpwfxFormat = &f;
    d.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_CTRLVOLUME;
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(ds, &d, &b, NULL);
    P("  CreateSoundBuffer %08lx\n", hr);
    if (!b) { IDirectSound8_Release(ds); return; }
    IDirectSound3DBuffer *b3 = NULL;
    IDirectSoundNotify *n = NULL;
    IKsPropertySet *k = NULL;
    IDirectSoundBuffer8 *b8 = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3);
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&n);
    IDirectSoundBuffer_QueryInterface(b, &IID_IKsPropertySet, (void **)&k);
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundBuffer8, (void **)&b8);
    P("  interfaces: 3D %s notify %s property set %s buffer8 %s\n", b3 ? "yes" : "no", n ? "yes" : "no", k ? "yes" : "no",
      b8 ? (b8 == (IDirectSoundBuffer8 *)b ? "the same" : "another") : "no");
    if (!b3 || !n || !k) { IDirectSound8_Release(ds); return; }
    P("  AddRef: buffer %lu 3D %lu notify %lu property set %lu\n", IDirectSoundBuffer_AddRef(b), IDirectSound3DBuffer_AddRef(b3),
      IDirectSoundNotify_AddRef(n), IKsPropertySet_AddRef(k));
    P("  Release: buffer %lu 3D %lu notify %lu property set %lu\n", IDirectSoundBuffer_Release(b), IDirectSound3DBuffer_Release(b3),
      IDirectSoundNotify_Release(n), IKsPropertySet_Release(k));
    IUnknown *u = NULL;
    IDirectSound3DBuffer_QueryInterface(b3, &IID_IDirectSoundBuffer, (void **)&u);
    P("  3D QueryInterface(buffer): %s, AddRef %lu Release %lu\n", u == (IUnknown *)b ? "the same" : "another",
      IDirectSoundBuffer_AddRef(b), IDirectSoundBuffer_Release(b));
    if (u) P("  Release it %lu\n", IUnknown_Release(u));
    IDirectSound3DBuffer *b3b = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3b);
    P("  3D again: %s, Release %lu\n", b3b == b3 ? "the same" : "another", b3b ? IDirectSound3DBuffer_Release(b3b) : 99);
    if (b8) P("  Release buffer8 %lu\n", IDirectSoundBuffer8_Release(b8));
    P("  Release notify %lu, property set %lu\n", IDirectSoundNotify_Release(n), IKsPropertySet_Release(k));
    /* the buffer released as long as Release answers more than 0, the 3D interface still held */
    int loops = 0;
    ULONG r;
    do { r = IDirectSoundBuffer_Release(b); loops++; } while (r && loops < 10);
    P("  buffer released until 0: %d Release(s)\n", loops);
    D3DVALUE md = -1;
    hr = IDirectSound3DBuffer_GetMinDistance(b3, &md);
    P("  3D still works: GetMinDistance %08lx %.1f\n", hr, md);
    P("  Release 3D %lu\n", IDirectSound3DBuffer_Release(b3));
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ an interface released more often than it was
 * referenced: does the buffer go while another interface still holds it (each probe on a buffer of its own; one
 * that frees the buffer crashes the next call, which is printed first) */
static IDirectSoundBuffer *refbuf(IDirectSound8 *ds)
{
    static WAVEFORMATEX f;
    f = pcm(22050, 1, 16);
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = 22050 * 2; d.lpwfxFormat = &f;
    d.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
    IDirectSoundBuffer *b = NULL;
    IDirectSound8_CreateSoundBuffer(ds, &d, &b, NULL);
    return b;
}
static void overrelease(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    /* 1: the 3D interface released twice, the buffer interface still held */
    IDirectSoundBuffer *b = refbuf(ds);
    IDirectSound3DBuffer *b3 = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3);
    P("  3D Release %lu\n", IDirectSound3DBuffer_Release(b3));
    P("  3D Release again %lu\n", IDirectSound3DBuffer_Release(b3));
    LONG v = 1;
    P("  then the buffer: GetVolume %08lx\n", IDirectSoundBuffer_GetVolume(b, &v));
    P("  buffer AddRef %lu Release %lu\n", IDirectSoundBuffer_AddRef(b), IDirectSoundBuffer_Release(b));
    P("  buffer Release %lu\n", IDirectSoundBuffer_Release(b));
    /* 2: the buffer interface released twice, the 3D interface still held */
    b = refbuf(ds);
    b3 = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3);
    P("  buffer Release %lu\n", IDirectSoundBuffer_Release(b));
    P("  buffer Release again %lu\n", IDirectSoundBuffer_Release(b));
    D3DVALUE md = -1;
    P("  then the 3D interface: GetMinDistance %08lx\n", IDirectSound3DBuffer_GetMinDistance(b3, &md));
    P("  3D Release %lu\n", IDirectSound3DBuffer_Release(b3));
    /* 3: the 3D interface released twice while the buffer interface has two references */
    b = refbuf(ds);
    IDirectSoundBuffer_AddRef(b);
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3);
    P("  3D Release %lu, again %lu, again %lu\n", IDirectSound3DBuffer_Release(b3), IDirectSound3DBuffer_Release(b3),
      IDirectSound3DBuffer_Release(b3));
    P("  then the buffer: GetVolume %08lx\n", IDirectSoundBuffer_GetVolume(b, &v));
    P("  buffer Release %lu\n", IDirectSoundBuffer_Release(b));
    P("  then the buffer: GetVolume %08lx\n", IDirectSoundBuffer_GetVolume(b, &v));
    P("  buffer Release %lu\n", IDirectSoundBuffer_Release(b));
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ how much readable memory follows a buffer's data
 * (a game that reads past the end of what it locked: Fallout: New Vegas) */
static void slack(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    static const DWORD sizes[] = { 4096, 48004, 176400, 529280, 553184, 1000000 };
    for (int fx = 0; fx < 2; fx++)
        for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            WAVEFORMATEX f = pcm(48000, 2, 16);
            DSBUFFERDESC d;
            memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = sizes[k]; d.lpwfxFormat = &f;
            d.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | (fx ? DSBCAPS_CTRLFX : 0);
            IDirectSoundBuffer *b = NULL;
            HRESULT hr = IDirectSound8_CreateSoundBuffer(ds, &d, &b, NULL);
            if (!b) { P("  %s %lu: %08lx\n", fx ? "CTRLFX" : "plain", sizes[k], hr); continue; }
            void *p1, *p2; DWORD b1, b2;
            if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER))) {
                BYTE *end = (BYTE *)p1 + b1;
                MEMORY_BASIC_INFORMATION mi;
                VirtualQuery(end, &mi, sizeof(mi));
                DWORD after = (DWORD)((BYTE *)mi.BaseAddress + mi.RegionSize - end);
                P("  %s %lu: readable after the end %s\n", fx ? "CTRLFX" : "plain", sizes[k],
                  after >= 65536 ? ">= 64 KB" : after >= 23904 ? ">= 23904" : "less than 23904");
                IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
            }
            IDirectSoundBuffer_Release(b);
        }
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ the sizes a buffer reports (GetCaps, Lock of the
 * whole) for the sizes and flags Fallout asks for */
static void sizes(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    static const struct { DWORD rate; WORD ch; DWORD bytes, flags; } t[] = {
        { 32000, 1, 254744, DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_MUTE3DATMAXDISTANCE },
        { 32000, 1, 270680, DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_MUTE3DATMAXDISTANCE },
        { 48000, 2, 529280, DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX },
        { 48000, 2, 553184, DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME },
        { 32000, 1, 149030, DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_MUTE3DATMAXDISTANCE },
        { 44100, 1, 57294, DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_MUTE3DATMAXDISTANCE },
        /* sizes that aren't a whole number of blocks, as Fallout 3 asks (its sample's byte count) */
        { 32000, 1, 254745, DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFX | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_MUTE3DATMAXDISTANCE },
        { 32000, 1, 210739, DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_MUTE3DATMAXDISTANCE },
        { 48000, 2, 529283, DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME },
        { 48000, 2, 529281, DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_STATIC },
        { 22050, 1, 1001, 0 },
    };
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        WAVEFORMATEX f = pcm(t[k].rate, t[k].ch, 16);
        DSBUFFERDESC d;
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = t[k].bytes; d.lpwfxFormat = &f; d.dwFlags = t[k].flags;
        IDirectSoundBuffer *b = NULL;
        HRESULT hr = IDirectSound8_CreateSoundBuffer(ds, &d, &b, NULL);
        DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
        DWORD lk = 0;
        if (b) {
            IDirectSoundBuffer_GetCaps(b, &c);
            void *p1, *p2; DWORD b1 = 0, b2;
            if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER))) {
                lk = b1;
                IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
            }
        }
        P("  %lu Hz %u ch %lu bytes flags %lx: %08lx GetCaps %lu Lock %lu\n", t[k].rate, t[k].ch, t[k].bytes, t[k].flags, hr, c.dwBufferBytes, lk);
        if (b) {                            /* Lock of the size asked for, and its last byte */
            void *p1, *p2; DWORD b1 = 0, b2 = 0;
            hr = IDirectSoundBuffer_Lock(b, 0, t[k].bytes, &p1, &b1, &p2, &b2, 0);
            P("    Lock(0, %lu) %08lx %lu + %lu", t[k].bytes, hr, b1, b2);
            if (SUCCEEDED(hr)) {
                ((BYTE *)p1)[b1 - 1] = 0x55;
                P(", Unlock %08lx", IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2));
            }
            hr = IDirectSoundBuffer_Lock(b, t[k].bytes - 1, 1, &p1, &b1, &p2, &b2, 0);
            P("; Lock(%lu, 1) %08lx %lu + %lu", t[k].bytes - 1, hr, b1, b2);
            if (SUCCEEDED(hr)) IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
            DWORD play = 0, write = 0;
            IDirectSoundBuffer_SetCurrentPosition(b, t[k].bytes - 1);
            IDirectSoundBuffer_GetCurrentPosition(b, &play, &write);
            P("; SetCurrentPosition(%lu) -> %lu\n", t[k].bytes - 1, play);
            IDirectSoundBuffer_Release(b);
        }
    }
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ Lock of a secondary buffer beyond its size */
static void locksize(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    for (int fx = 0; fx < 2; fx++) {
        WAVEFORMATEX f = pcm(32000, 1, 16);
        DSBUFFERDESC d;
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = 254744; d.lpwfxFormat = &f;
        d.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLVOLUME | DSBCAPS_GETCURRENTPOSITION2 |
                    DSBCAPS_MUTE3DATMAXDISTANCE | (fx ? DSBCAPS_CTRLFX : 0);
        IDirectSoundBuffer *b = NULL;
        IDirectSound8_CreateSoundBuffer(ds, &d, &b, NULL);
        if (!b) continue;
        P("  %s:\n", fx ? "CTRLFX" : "plain");
        lock("size + 15936", b, 0, 270680, 0, TRUE);
        lock("size + 1", b, 0, 254745, 0, TRUE);
        lock("size + 2", b, 0, 254746, 0, TRUE);
        lock("odd size", b, 0, 254743, 0, TRUE);
        lock("odd offset, to the end", b, 1, 254743, 0, TRUE);
        lock("offset at the size", b, 254744, 16, 0, TRUE);
        lock("0 bytes", b, 0, 0, 0, TRUE);
        IDirectSoundBuffer_Release(b);
    }
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ the listener's orientation: which ones are refused */
static void orient(void)
{
    IDirectSound8 *ds = make_ds();
    if (!ds) return;
    IDirectSound8_SetCooperativeLevel(ds, wnd(), DSSCL_PRIORITY);
    HRESULT hr;
    IDirectSoundBuffer *b = make_primary(ds, DSBCAPS_CTRL3D, &hr);
    IDirectSound3DListener *l = NULL;
    if (b) IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DListener, (void **)&l);
    if (!l) { P("  no listener\n"); IDirectSound8_Release(ds); return; }
    union { DWORD u; float f; } inf = { 0x7f800000 }, nan = { 0x7fc00000 };
    static const struct { float v[6]; const char *n; } t[] = {
        { { 0, 0, 1, 0, 1, 0 }, "ordinary" }, { { 0, 0, 0, 0, 0, 0 }, "all zero" }, { { 0, 0, 0, 0, 1, 0 }, "front zero" },
        { { 0, 0, 1, 0, 0, 0 }, "top zero" }, { { 0, 0, 1, 0, 0, 1 }, "parallel" }, { { 0, 0, 1, 0, 0, -3 }, "opposite" },
        { { 0, 0, 5, 0, 7, 0 }, "not unit" }, { { 1e-30f, 0, 0, 0, 1e-30f, 0 }, "tiny" }, { { -0.0f, 0, 0, 0, 1, 0 }, "minus zero" },
    };
    for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        const float *v = t[k].v;
        hr = IDirectSound3DListener_SetOrientation(l, v[0], v[1], v[2], v[3], v[4], v[5], DS3D_IMMEDIATE);
        D3DVECTOR f, u;
        IDirectSound3DListener_GetOrientation(l, &f, &u);
        P("  SetOrientation(%s) %08lx -> front %.3f,%.3f,%.3f top %.3f,%.3f,%.3f\n", t[k].n, hr, f.x, f.y, f.z, u.x, u.y, u.z);
        IDirectSound3DListener_SetOrientation(l, 0, 0, 1, 0, 1, 0, DS3D_IMMEDIATE);
    }
    hr = IDirectSound3DListener_SetOrientation(l, inf.f, 0, 1, 0, 1, 0, DS3D_IMMEDIATE);
    P("  SetOrientation(infinite) %08lx\n", hr);
    hr = IDirectSound3DListener_SetOrientation(l, 0, 0, 1, 0, nan.f, 0, DS3D_IMMEDIATE);
    P("  SetOrientation(NaN) %08lx\n", hr);
    hr = IDirectSound3DListener_SetOrientation(l, 0, 0, 0, 0, 0, 0, DS3D_DEFERRED);
    P("  SetOrientation(all zero, deferred) %08lx, Commit %08lx\n", hr, IDirectSound3DListener_CommitDeferredSettings(l));
    D3DVECTOR f, u;
    IDirectSound3DListener_GetOrientation(l, &f, &u);
    P("    -> front %.3f,%.3f,%.3f top %.3f,%.3f,%.3f\n", f.x, f.y, f.z, u.x, u.y, u.z);
    IDirectSound3DListener_Release(l);
    IDirectSoundBuffer_Release(b);
    IDirectSound8_Release(ds);
}

/* ------------------------------------------------------------------ every method on a lost buffer: the owner's, and
 * another DirectSound object's, while one object writes the primary buffer and after it leaves that level */
static void lost_calls(const char *what, IDirectSoundBuffer *b)
{
    void *p1, *p2; DWORD b1 = 0, b2 = 0, pl = 0xdead, wr = 0xdead, st = 0xdead, fr = 0;
    LONG v = 1;
    HRESULT hr = IDirectSoundBuffer_Lock(b, 0, 64, &p1, &b1, &p2, &b2, 0);
    P("    %s: Lock %08lx", what, hr);
    if (SUCCEEDED(hr)) P(" Unlock %08lx", IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2));
    P(" Play %08lx", IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING));
    hr = IDirectSoundBuffer_GetStatus(b, &st);
    P(" GetStatus %08lx %lx", hr, st);
    P(" Stop %08lx", IDirectSoundBuffer_Stop(b));
    P(" SetCurrentPosition %08lx", IDirectSoundBuffer_SetCurrentPosition(b, 8));
    hr = IDirectSoundBuffer_GetCurrentPosition(b, &pl, &wr);
    P(" GetCurrentPosition %08lx %lx %lx\n", hr, pl, wr);
    P("      SetVolume %08lx", IDirectSoundBuffer_SetVolume(b, -100));
    hr = IDirectSoundBuffer_GetVolume(b, &v);
    P(" GetVolume %08lx %ld", hr, v);
    P(" SetFrequency %08lx", IDirectSoundBuffer_SetFrequency(b, 22050));
    hr = IDirectSoundBuffer_GetFrequency(b, &fr);
    P(" GetFrequency %08lx %lu", hr, fr);
    P(" SetPan %08lx", IDirectSoundBuffer_SetPan(b, 0));
    DSBCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
    P(" GetCaps %08lx", IDirectSoundBuffer_GetCaps(b, &c));
    WAVEFORMATEX f; DWORD got = 0;
    P(" GetFormat %08lx", IDirectSoundBuffer_GetFormat(b, &f, sizeof(f), &got));
    IDirectSoundNotify *n = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSoundNotify, (void **)&n);
    if (n) {
        DSBPOSITIONNOTIFY pn = { 0, CreateEventA(NULL, FALSE, FALSE, NULL) };
        P(" SetNotificationPositions %08lx", IDirectSoundNotify_SetNotificationPositions(n, 1, &pn));
        IDirectSoundNotify_Release(n);
        CloseHandle(pn.hEventNotify);
    }
    IDirectSound3DBuffer *d3 = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&d3);
    if (d3) {
        P(" 3D SetPosition %08lx", IDirectSound3DBuffer_SetPosition(d3, 1, 2, 3, DS3D_IMMEDIATE));
        IDirectSound3DBuffer_Release(d3);
    }
    P("\n");
}

static void lost(void)
{
    IDirectSound8 *a = make_ds(), *bo = NULL;
    IDirectSound *old = NULL;
    if (!a) return;
    DirectSoundCreate8(NULL, &bo, NULL);
    DirectSoundCreate(NULL, &old, NULL);
    IDirectSound8_SetCooperativeLevel(a, wnd(), DSSCL_PRIORITY);
    if (bo) IDirectSound8_SetCooperativeLevel(bo, wnd(), DSSCL_PRIORITY);
    if (old) IDirectSound_SetCooperativeLevel(old, wnd(), DSSCL_NORMAL);
    WAVEFORMATEX f = pcm(22050, 1, 16);
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); d.dwBufferBytes = 22050; d.lpwfxFormat = &f;
    d.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPOSITIONNOTIFY;
    IDirectSoundBuffer *own = NULL, *other = NULL, *older = NULL, *other3d = NULL;
    IDirectSound8_CreateSoundBuffer(a, &d, &own, NULL);
    if (bo) IDirectSound8_CreateSoundBuffer(bo, &d, &other, NULL);
    if (old) IDirectSound_CreateSoundBuffer(old, &d, &older, NULL);
    DSBUFFERDESC d3 = d; d3.dwFlags = DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME;
    if (bo) IDirectSound8_CreateSoundBuffer(bo, &d3, &other3d, NULL);
    if (other) IDirectSoundBuffer_Play(other, 0, 0, DSBPLAY_LOOPING);
    P("  SetCooperativeLevel(WRITEPRIMARY) %08lx\n", IDirectSound8_SetCooperativeLevel(a, wnd(), DSSCL_WRITEPRIMARY));
    if (own) lost_calls("owner's", own);
    if (other) lost_calls("another object's", other);
    if (older) lost_calls("an IDirectSound's", older);
    if (other3d) lost_calls("another object's 3D", other3d);
    IDirectSoundBuffer *late = NULL;
    HRESULT hr = bo ? IDirectSound8_CreateSoundBuffer(bo, &d, &late, NULL) : E_FAIL;
    P("  another object's CreateSoundBuffer %08lx\n", hr);
    if (late) lost_calls("made at that level", late);
    if (bo) {
        DSCAPS c; memset(&c, 0, sizeof(c)); c.dwSize = sizeof(c);
        P("  another object: SetCooperativeLevel(PRIORITY) %08lx, GetCaps %08lx\n",
          IDirectSound8_SetCooperativeLevel(bo, wnd(), DSSCL_PRIORITY), IDirectSound8_GetCaps(bo, &c));
        IDirectSoundBuffer *prim = make_primary(bo, 0, &hr);
        P("  another object's primary %08lx", hr);
        if (prim) {
            void *p1, *p2; DWORD b1 = 0, b2 = 0;
            WAVEFORMATEX pf = pcm(44100, 2, 16);
            P(" Lock %08lx SetFormat %08lx Play %08lx", IDirectSoundBuffer_Lock(prim, 0, 64, &p1, &b1, &p2, &b2, 0),
              IDirectSoundBuffer_SetFormat(prim, &pf), IDirectSoundBuffer_Play(prim, 0, 0, DSBPLAY_LOOPING));
            IDirectSoundBuffer_Stop(prim);
            IDirectSoundBuffer_Release(prim);
        }
        P("\n");
    }
    P("  SetCooperativeLevel(PRIORITY) %08lx\n", IDirectSound8_SetCooperativeLevel(a, wnd(), DSSCL_PRIORITY));
    if (other) {
        DWORD st = 0;
        IDirectSoundBuffer_GetStatus(other, &st);
        P("    another object's, after: GetStatus %lx Play %08lx Restore %08lx", st, IDirectSoundBuffer_Play(other, 0, 0, 0),
          IDirectSoundBuffer_Restore(other));
        IDirectSoundBuffer_GetStatus(other, &st);
        P(" GetStatus %lx Play %08lx\n", st, IDirectSoundBuffer_Play(other, 0, 0, 0));
        IDirectSoundBuffer_Stop(other);
    }
    if (late) {
        P("    made at that level, after: Restore %08lx\n", IDirectSoundBuffer_Restore(late));
        IDirectSoundBuffer_Release(late);
    }
    /* the other object releases while the first writes the primary: nothing of it stays lost */
    if (own) IDirectSoundBuffer_Release(own);
    if (other) IDirectSoundBuffer_Release(other);
    if (older) IDirectSoundBuffer_Release(older);
    if (other3d) IDirectSoundBuffer_Release(other3d);
    if (old) IDirectSound_Release(old);
    if (bo) IDirectSound8_Release(bo);
    IDirectSound8_Release(a);
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    CoInitialize(NULL);
    if (!only || !strcmp(only, "levels")) { P("levels:\n"); levels(); }
    if (!only || !strcmp(only, "api")) { P("api:\n"); api(); }
    if (!only || !strcmp(only, "secondary")) { P("secondary:\n"); secondary(); }
    if (!only || !strcmp(only, "more")) { P("more:\n"); more(); }
    if (!only || !strcmp(only, "object")) { P("object:\n"); object(); }
    if (!only || !strcmp(only, "refs")) { P("refs:\n"); refs(); }
    if (only && !strcmp(only, "overrelease")) { P("overrelease:\n"); overrelease(); }
    if (only && !strcmp(only, "slack")) { P("slack:\n"); slack(); }
    if (!only || !strcmp(only, "sizes")) { P("sizes:\n"); sizes(); }
    if (!only || !strcmp(only, "locksize")) { P("locksize:\n"); locksize(); }
    if (!only || !strcmp(only, "orient")) { P("orient:\n"); orient(); }
    if (!only || !strcmp(only, "lost")) { P("lost:\n"); lost(); }
    if (only && !strcmp(only, "audio")) { P("audio:\n"); audio(); }
    CoUninitialize();
    return 0;
}
