/* waveOut through reAL's mixer (winmm.dll, reAL's proxy, hands these calls to reAL_winmm_bind's functions).
 *
 * Each stream is a looping buffer of reAL's in the game's format, a second long, filled along the stream's timeline
 * (wavecore.c) a little ahead of the mixer by a thread of its own, every 5 ms.  The game's sound is mixed with
 * everything else reAL plays, at the output's rate, and Pajama Sam 2's way of resetting its mix ten times a second no
 * longer skips.
 *
 * Callbacks (WOM_OPEN, WOM_DONE, WOM_CLOSE) come from a thread of reAL's, as XP's driver sent them, never from inside
 * the call that causes them (Windows 10 and 11 send WOM_OPEN inside waveOutOpen, which deadlocks a game whose
 * callback suspends the thread opening the device).  waveOutReset and waveOutClose return once their callbacks have
 * run, as Wine's do.  A callback function is called with the registers it may trash saved (Windows 10 and 11 count on
 * EBX, ESI, EDI, which some old callbacks don't keep).
 *
 * A format reAL's buffers cannot play (compressed ones, which Windows' mapper converts) opens on the system's winmm,
 * and every call on that handle goes there. */
#include "real.h"
#include "wavecore.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef DRVM_MAPPER_PREFERRED_GET
#define DRVM_MAPPER_PREFERRED_GET 0x2015
#endif
#define TICK_MS 5

typedef struct rl_wave {
    struct rl_wave *next;
    DWORD_PTR cb, inst;
    DWORD flags;
    WAVEFORMATEXTENSIBLE fmt;
    rl_device *dev;
    rl_buffer *b;
    wc_stream wc;
    LPWAVEHDR *q;               /* the buffers queued, oldest first */
    int nq, capq;
    DWORD volume;
    int paused;
} rl_wave;

static CRITICAL_SECTION s_wl;   /* the streams, everything about them */
static rl_wave *s_waves;
static HANDLE s_tick_thread, s_tick_wake;
static DWORD s_vol_dev = 0xffffffff;    /* waveOutSetVolume on the device: new streams start at it */
static double s_qpf;

/* the system's winmm, for what is not ours */
static UINT (WINAPI *o_getnumdevs)(void);
static MMRESULT (WINAPI *o_getcapsA)(UINT_PTR, LPWAVEOUTCAPSA, UINT);
static MMRESULT (WINAPI *o_getcapsW)(UINT_PTR, LPWAVEOUTCAPSW, UINT);
static MMRESULT (WINAPI *o_open)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT (WINAPI *o_close)(HWAVEOUT);
static MMRESULT (WINAPI *o_prep)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *o_unprep)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *o_write)(HWAVEOUT, LPWAVEHDR, UINT);
static MMRESULT (WINAPI *o_pause)(HWAVEOUT);
static MMRESULT (WINAPI *o_restart)(HWAVEOUT);
static MMRESULT (WINAPI *o_reset)(HWAVEOUT);
static MMRESULT (WINAPI *o_breakloop)(HWAVEOUT);
static MMRESULT (WINAPI *o_getpos)(HWAVEOUT, LPMMTIME, UINT);
static MMRESULT (WINAPI *o_getpitch)(HWAVEOUT, LPDWORD);
static MMRESULT (WINAPI *o_setpitch)(HWAVEOUT, DWORD);
static MMRESULT (WINAPI *o_getrate)(HWAVEOUT, LPDWORD);
static MMRESULT (WINAPI *o_setrate)(HWAVEOUT, DWORD);
static MMRESULT (WINAPI *o_getvol)(HWAVEOUT, LPDWORD);
static MMRESULT (WINAPI *o_setvol)(HWAVEOUT, DWORD);
static MMRESULT (WINAPI *o_getid)(HWAVEOUT, LPUINT);
static MMRESULT (WINAPI *o_message)(HWAVEOUT, UINT, DWORD_PTR, DWORD_PTR);

static double now_s(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / s_qpf;
}

/* (s_wl held) ours, or NULL: a handle of the system's winmm */
static rl_wave *ours(HWAVEOUT h)
{
    for (rl_wave *w = s_waves; w; w = w->next) if ((HWAVEOUT)w == h) return w;
    return NULL;
}
/* a device id rather than a handle (waveOutGetVolume and others take either): ours is 0, or the mapper */
static int our_id(UINT_PTR id) { return id == 0 || id == WAVE_MAPPER || id == 0xffff; }

/* ------------------------------------------------------------------ callbacks */
/* a callback function, its registers kept whatever it does to them (ESP and EBP excepted) and the direction flag
 * cleared after it: (fn, five arguments) */
DWORD rl_call5(DWORD_PTR fn, DWORD_PTR a, DWORD_PTR b, DWORD_PTR c, DWORD_PTR d, DWORD_PTR e);
__asm__(".text\n.globl _rl_call5\n_rl_call5:\n"
        "    pushl %ebp\n    movl %esp, %ebp\n    pushl %ebx\n    pushl %esi\n    pushl %edi\n"
        "    pushl 28(%ebp)\n    pushl 24(%ebp)\n    pushl 20(%ebp)\n    pushl 16(%ebp)\n    pushl 12(%ebp)\n"
        "    call *8(%ebp)\n    cld\n"
        "    leal -12(%ebp), %esp\n    popl %edi\n    popl %esi\n    popl %ebx\n    popl %ebp\n    ret\n");

typedef struct { DWORD_PTR cb, inst; DWORD flags; HWAVEOUT h; UINT msg; DWORD_PTR p1; } cbmsg;
static CRITICAL_SECTION s_cbl;
static cbmsg *s_cbq;
static int s_cbh, s_cbn, s_cbcap;
static LONGLONG s_cb_posted, s_cb_done;
static HANDLE s_cb_wake, s_cb_ran;
static DWORD s_cb_tid;

static DWORD WINAPI cb_thread(LPVOID p)
{
    (void)p;
    for (;;) {
        WaitForSingleObject(s_cb_wake, INFINITE);
        for (;;) {
            EnterCriticalSection(&s_cbl);
            if (s_cbh == s_cbn) { s_cbh = s_cbn = 0; LeaveCriticalSection(&s_cbl); break; }
            cbmsg m = s_cbq[s_cbh++];
            LeaveCriticalSection(&s_cbl);
            switch (m.flags & CALLBACK_TYPEMASK) {
            case CALLBACK_FUNCTION: rl_call5(m.cb, (DWORD_PTR)m.h, m.msg, m.inst, m.p1, 0); break;
            case CALLBACK_WINDOW:   PostMessageA((HWND)m.cb, m.msg, (WPARAM)m.h, (LPARAM)m.p1); break;
            case CALLBACK_THREAD:   PostThreadMessageA((DWORD)m.cb, m.msg, (WPARAM)m.h, (LPARAM)m.p1); break;
            case CALLBACK_EVENT:    SetEvent((HANDLE)m.cb); break;
            }
            EnterCriticalSection(&s_cbl);
            s_cb_done++;
            LeaveCriticalSection(&s_cbl);
            SetEvent(s_cb_ran);
        }
    }
    return 0;
}
/* a callback queued (in order); its number, 0 for none */
static LONGLONG post(const rl_wave *w, UINT msg, DWORD_PTR p1)
{
    DWORD type = w->flags & CALLBACK_TYPEMASK;
    if (type != CALLBACK_FUNCTION && type != CALLBACK_WINDOW && type != CALLBACK_THREAD && type != CALLBACK_EVENT)
        return 0;
    if (!w->cb && type != CALLBACK_THREAD) return 0;
    LONGLONG seq = 0;
    EnterCriticalSection(&s_cbl);
    if (s_cbn == s_cbcap) {
        if (s_cbh) {
            memmove(s_cbq, s_cbq + s_cbh, (size_t)(s_cbn - s_cbh) * sizeof(*s_cbq));
            s_cbn -= s_cbh; s_cbh = 0;
        }
        if (s_cbn == s_cbcap) {
            int cap = s_cbcap ? s_cbcap * 2 : 64;
            cbmsg *q = realloc(s_cbq, (size_t)cap * sizeof(*q));
            if (q) { s_cbq = q; s_cbcap = cap; }
        }
    }
    if (s_cbn < s_cbcap) {
        s_cbq[s_cbn++] = (cbmsg){ w->cb, w->inst, w->flags, (HWAVEOUT)w, msg, p1 };
        seq = ++s_cb_posted;
    }
    LeaveCriticalSection(&s_cbl);
    if (seq) SetEvent(s_cb_wake);
    return seq;
}
/* until callback `seq` has run (not on the callback thread: a call from inside a callback goes on at once) */
static void wait_cb(LONGLONG seq)
{
    if (!seq || GetCurrentThreadId() == s_cb_tid) return;
    for (;;) {
        EnterCriticalSection(&s_cbl);
        int ok = s_cb_done >= seq;
        LeaveCriticalSection(&s_cbl);
        if (ok) return;
        WaitForSingleObject(s_cb_ran, 1);
    }
}

/* ------------------------------------------------------------------ the stream */
static void put(void *ctx, uint32_t at, const uint8_t *src, uint32_t frames)
{
    rl_wave *w = ctx;
    DWORD blk = w->fmt.Format.nBlockAlign;
    void *p1, *p2;
    DWORD n1, n2;
    IDirectSoundBuffer8 *b = &w->b->IDirectSoundBuffer8_iface;
    if (FAILED(IDirectSoundBuffer8_Lock(b, at * blk, frames * blk, &p1, &n1, &p2, &n2, 0))) return;
    if (src) memcpy(p1, src, n1); else memset(p1, w->wc.silence, n1);
    if (p2) { if (src) memcpy(p2, src + n1, n2); else memset(p2, w->wc.silence, n2); }
    IDirectSoundBuffer8_Unlock(b, p1, n1, p2, n2);
}
/* (s_wl held) a buffer the stream has played: back to the game */
static void done(void *ctx, void *hdr)
{
    rl_wave *w = ctx;
    LPWAVEHDR h = hdr;
    for (int i = 0; i < w->nq; i++)
        if (w->q[i] == h) { memmove(w->q + i, w->q + i + 1, (size_t)(w->nq - i - 1) * sizeof(*w->q)); w->nq--; break; }
    h->dwFlags = (h->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
    post(w, WOM_DONE, (DWORD_PTR)h);
}
/* (s_wl held) where the mixer is */
static void mixer_pos(rl_wave *w)
{
    wc_mixer_at(&w->wc, rl_buffer_position_bytes(w->b) / w->fmt.Format.nBlockAlign);
}

static DWORD WINAPI tick_thread(LPVOID p)
{
    (void)p;
    timeBeginPeriod(1);
    for (;;) {
        EnterCriticalSection(&s_wl);
        int any = s_waves != NULL;
        double t = now_s();
        for (rl_wave *w = s_waves; w; w = w->next) {
            if (!w->paused) mixer_pos(w);
            wc_tick(&w->wc, t);
        }
        LeaveCriticalSection(&s_wl);
        WaitForSingleObject(s_tick_wake, any ? TICK_MS : INFINITE);
    }
    return 0;
}

static void set_volume(rl_wave *w, DWORD v)
{
    w->volume = v;
    double l = LOWORD(v) / 65535.0, r = HIWORD(v) / 65535.0, m = l > r ? l : r;
    LONG vol = m > 0 ? (LONG)(2000.0 * log10(m)) : DSBVOLUME_MIN, pan = 0;
    if (vol < DSBVOLUME_MIN) vol = DSBVOLUME_MIN;
    if (l > r) pan = r > 0 ? (LONG)(2000.0 * log10(r / l)) : DSBPAN_LEFT;
    else if (r > l) pan = l > 0 ? -(LONG)(2000.0 * log10(l / r)) : DSBPAN_RIGHT;
    if (pan < DSBPAN_LEFT) pan = DSBPAN_LEFT;
    if (pan > DSBPAN_RIGHT) pan = DSBPAN_RIGHT;
    IDirectSoundBuffer8_SetVolume(&w->b->IDirectSoundBuffer8_iface, vol);
    IDirectSoundBuffer8_SetPan(&w->b->IDirectSoundBuffer8_iface, pan);
}

static const GUID SUB_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID SUB_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
/* a format reAL's buffers play (DirectSound's rules: rl_buffer_create decides in the end) */
static int playable(const WAVEFORMATEX *f)
{
    WORD tag = f->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (f->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return 0;
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)f;
        if (IsEqualGUID(&x->SubFormat, &SUB_PCM)) tag = WAVE_FORMAT_PCM;
        else if (IsEqualGUID(&x->SubFormat, &SUB_FLOAT)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else return 0;
        if (f->nChannels < 1 || f->nChannels > 8) return 0;
    } else if (tag != WAVE_FORMAT_PCM && tag != WAVE_FORMAT_IEEE_FLOAT) return 0;
    else if (f->nChannels < 1 || f->nChannels > 2) return 0;
    if (f->nSamplesPerSec < DSBFREQUENCY_MIN || f->nSamplesPerSec > DSBFREQUENCY_MAX) return 0;
    if (f->nBlockAlign != f->nChannels * f->wBitsPerSample / 8) return 0;
    if (tag == WAVE_FORMAT_IEEE_FLOAT) return f->wBitsPerSample == 32;
    return f->wBitsPerSample == 8 || f->wBitsPerSample == 16 || f->wBitsPerSample == 24 || f->wBitsPerSample == 32;
}

static rl_wave *open_stream(const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD fl)
{
    rl_wave *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->cb = cb; w->inst = inst; w->flags = fl;
    memcpy(&w->fmt, f, f->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? sizeof(WAVEFORMATEXTENSIBLE) : sizeof(WAVEFORMATEX));
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE) w->fmt.Format.cbSize = 0;
    const uint32_t rate = f->nSamplesPerSec, blk = f->nBlockAlign;
    if (FAILED(rl_device_acquire(&w->dev))) { free(w); return NULL; }
    DSBUFFERDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_LOCSOFTWARE | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME |
                DSBCAPS_CTRLPAN;
    d.dwBufferBytes = rate * blk;                       /* a second */
    d.lpwfxFormat = &w->fmt.Format;
    if (FAILED(rl_buffer_create(w->dev, NULL, &d, &w->b))) { rl_device_release(w->dev); free(w); return NULL; }
    /* the mixer's period at the stream's rate, and the ideal device that much (and 10 ms) ahead of it */
    EnterCriticalSection(&w->dev->lock);
    uint32_t orate = w->dev->out_rate ? w->dev->out_rate : 48000;
    uint32_t opf = w->dev->period_frames ? w->dev->period_frames : orate / 100;
    LeaveCriticalSection(&w->dev->lock);
    uint32_t period = (uint32_t)((uint64_t)opf * rate / orate) + 1, guard = 32;
    uint32_t delta = period + rate / 100 + guard;
    w->wc.put = put; w->wc.done = done; w->wc.ctx = w;
    if (!wc_init(&w->wc, rate, blk, f->wBitsPerSample == 8 ? 0x80 : 0, rate, period, delta, guard, now_s())) {
        IDirectSoundBuffer8_Release(&w->b->IDirectSoundBuffer8_iface);
        rl_device_release(w->dev);
        free(w);
        return NULL;
    }
    put(w, 0, NULL, rate);
    set_volume(w, s_vol_dev);
    IDirectSoundBuffer8_Play(&w->b->IDirectSoundBuffer8_iface, 0, 0, DSBPLAY_LOOPING);
    LOG_INFO("waveOut: %u Hz, %u ch, %u-bit through reAL (%.1f ms behind the game)", rate,
             f->nChannels, f->wBitsPerSample, (delta + period / 2.0) * 1000.0 / rate);
    return w;
}
static void free_stream(rl_wave *w)
{
    IDirectSoundBuffer8_Stop(&w->b->IDirectSoundBuffer8_iface);
    IDirectSoundBuffer8_Release(&w->b->IDirectSoundBuffer8_iface);
    rl_device_release(w->dev);
    wc_free(&w->wc);
    free(w->q);
    free(w);
}

/* ------------------------------------------------------------------ waveOut */
static void fill_caps(WAVEOUTCAPSW *cw, WAVEOUTCAPSA *ca)
{
    WAVEOUTCAPSW w;
    memset(&w, 0, sizeof(w));
    w.wMid = 1; w.wPid = 101;               /* MM_MICROSOFT, MM_MSFT_GENERIC_WAVEOUT */
    w.vDriverVersion = 0x0100;
    MultiByteToWideChar(CP_ACP, 0, "reAL", -1, w.szPname, MAXPNAMELEN);
    w.dwFormats = 0xfffff;                  /* every WAVE_FORMAT_* rate, channels and bits */
    w.wChannels = 2;
    w.dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME | WAVECAPS_SAMPLEACCURATE;
    if (cw) *cw = w;
    if (ca) {
        ca->wMid = w.wMid; ca->wPid = w.wPid; ca->vDriverVersion = w.vDriverVersion;
        lstrcpynA(ca->szPname, "reAL", MAXPNAMELEN);
        ca->dwFormats = w.dwFormats; ca->wChannels = w.wChannels; ca->wReserved1 = 0; ca->dwSupport = w.dwSupport;
    }
}

static UINT WINAPI w_getnumdevs(void) { return 1; }
static MMRESULT getcaps(UINT_PTR id, void *c, UINT sz, int wide)
{
    if (!our_id(id)) {
        EnterCriticalSection(&s_wl);
        rl_wave *w = ours((HWAVEOUT)id);
        LeaveCriticalSection(&s_wl);
        if (!w) return wide ? o_getcapsW(id, c, sz) : o_getcapsA(id, c, sz);    /* (a handle of the system's) */
    }
    if (!c) return MMSYSERR_INVALPARAM;
    WAVEOUTCAPSW w; WAVEOUTCAPSA a;
    fill_caps(&w, &a);
    if (wide) memcpy(c, &w, sz < sizeof(w) ? sz : sizeof(w));
    else memcpy(c, &a, sz < sizeof(a) ? sz : sizeof(a));
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_getcapsA(UINT_PTR id, LPWAVEOUTCAPSA c, UINT sz) { return getcaps(id, c, sz, 0); }
static MMRESULT WINAPI w_getcapsW(UINT_PTR id, LPWAVEOUTCAPSW c, UINT sz) { return getcaps(id, c, sz, 1); }

static MMRESULT WINAPI w_open(LPHWAVEOUT ph, UINT id, LPCWAVEFORMATEX f, DWORD_PTR cb, DWORD_PTR inst, DWORD fl)
{
    if (!our_id(id)) return MMSYSERR_BADDEVICEID;
    if (!f) return MMSYSERR_INVALPARAM;
    DWORD type = fl & CALLBACK_TYPEMASK;
    if (type == CALLBACK_WINDOW && cb && !IsWindow((HWND)cb)) return MMSYSERR_INVALPARAM;
    if (!playable(f)) {
        /* (the system's mapper converts a compressed format; reAL's buffers can't play it) */
        MMRESULT r = o_open(ph, WAVE_MAPPER, f, cb, inst, fl);
        LOG_CALL("waveOutOpen(format %u, %lu Hz, %u channels, %u bits, flags %lx) -> the system's: %u", f->wFormatTag,
                 (unsigned long)f->nSamplesPerSec, f->nChannels, f->wBitsPerSample, (unsigned long)fl, r);
        return r;
    }
    if (fl & WAVE_FORMAT_QUERY) return MMSYSERR_NOERROR;
    if (!ph) return MMSYSERR_INVALPARAM;
    rl_wave *w = open_stream(f, cb, inst, fl);
    if (!w) {
        MMRESULT r = o_open(ph, WAVE_MAPPER, f, cb, inst, fl);
        LOG_INFO("waveOut: reAL couldn't open a stream (%lu Hz, %u channels, %u bits): the system's winmm has it (%u)",
                 (unsigned long)f->nSamplesPerSec, f->nChannels, f->wBitsPerSample, r);
        return r;
    }
    EnterCriticalSection(&s_wl);
    w->next = s_waves;
    s_waves = w;
    *ph = (HWAVEOUT)w;
    post(w, WOM_OPEN, 0);
    LeaveCriticalSection(&s_wl);
    SetEvent(s_tick_wake);
    LOG_CALL("waveOutOpen(dev %d, callback %p (%lx), inst %p) -> %p", (int)id, (void *)cb, (unsigned long)type,
             (void *)inst, (void *)w);
    return MMSYSERR_NOERROR;
}

static MMRESULT WINAPI w_close(HWAVEOUT h)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (!w) { LeaveCriticalSection(&s_wl); return o_close(h); }
    if (w->nq) { LeaveCriticalSection(&s_wl); return WAVERR_STILLPLAYING; }
    for (rl_wave **p = &s_waves; *p; p = &(*p)->next) if (*p == w) { *p = w->next; break; }
    LONGLONG seq = post(w, WOM_CLOSE, 0);
    LeaveCriticalSection(&s_wl);
    wait_cb(seq);
    free_stream(w);
    LOG_CALL("waveOutClose(%p)", (void *)h);
    return MMSYSERR_NOERROR;
}

static MMRESULT WINAPI w_prep(HWAVEOUT h, LPWAVEHDR hd, UINT sz)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    if (!w) return o_prep(h, hd, sz);
    if (!hd || sz < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
    if (hd->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hd->dwFlags = (hd->dwFlags | WHDR_PREPARED) & ~WHDR_DONE;
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_unprep(HWAVEOUT h, LPWAVEHDR hd, UINT sz)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    if (!w) return o_unprep(h, hd, sz);
    if (!hd || sz < sizeof(WAVEHDR)) return MMSYSERR_INVALPARAM;
    if (hd->dwFlags & WHDR_INQUEUE) return WAVERR_STILLPLAYING;
    hd->dwFlags = (hd->dwFlags & ~WHDR_PREPARED) | WHDR_DONE;
    return MMSYSERR_NOERROR;
}

static MMRESULT WINAPI w_write(HWAVEOUT h, LPWAVEHDR hd, UINT sz)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (!w) { LeaveCriticalSection(&s_wl); return o_write(h, hd, sz); }
    MMRESULT r = MMSYSERR_NOERROR;
    if (!hd || sz < sizeof(WAVEHDR) || (!hd->lpData && hd->dwBufferLength)) r = MMSYSERR_INVALPARAM;
    else if (!(hd->dwFlags & WHDR_PREPARED)) r = WAVERR_UNPREPARED;
    else if (hd->dwFlags & WHDR_INQUEUE) r = WAVERR_STILLPLAYING;
    else if (w->nq == w->capq) {
        int cap = w->capq ? w->capq * 2 : 32;
        LPWAVEHDR *q = realloc(w->q, (size_t)cap * sizeof(*q));
        if (q) { w->q = q; w->capq = cap; } else r = MMSYSERR_NOMEM;
    }
    if (!r) {
        hd->dwFlags = (hd->dwFlags & ~WHDR_DONE) | WHDR_INQUEUE;
        hd->lpNext = NULL;
        w->q[w->nq++] = hd;
        if (!w->paused) mixer_pos(w);
        if (!wc_write(&w->wc, now_s(), hd, (const uint8_t *)hd->lpData, hd->dwBufferLength / w->fmt.Format.nBlockAlign,
                      hd->dwFlags & (WHDR_BEGINLOOP | WHDR_ENDLOOP), hd->dwLoops)) {
            w->nq--;
            hd->dwFlags &= ~WHDR_INQUEUE;
            r = MMSYSERR_NOMEM;
        }
    }
    LeaveCriticalSection(&s_wl);
    LOG_TRACE("waveOutWrite(%p, %p, %lu bytes, flags %lx) -> %u", (void *)h, (void *)hd,
              hd ? (unsigned long)hd->dwBufferLength : 0UL, hd ? (unsigned long)hd->dwFlags : 0UL, r);
    return r;
}

static MMRESULT WINAPI w_pause(HWAVEOUT h)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (!w) { LeaveCriticalSection(&s_wl); return o_pause(h); }
    if (!w->paused) {
        mixer_pos(w);
        IDirectSoundBuffer8_Stop(&w->b->IDirectSoundBuffer8_iface);
        wc_pause(&w->wc, now_s());
        w->paused = 1;
    }
    LeaveCriticalSection(&s_wl);
    LOG_CALL("waveOutPause(%p)", (void *)h);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_restart(HWAVEOUT h)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (!w) { LeaveCriticalSection(&s_wl); return o_restart(h); }
    if (w->paused) {
        wc_restart(&w->wc, now_s());
        IDirectSoundBuffer8_Play(&w->b->IDirectSoundBuffer8_iface, 0, 0, DSBPLAY_LOOPING);
        w->paused = 0;
    }
    LeaveCriticalSection(&s_wl);
    LOG_CALL("waveOutRestart(%p)", (void *)h);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_reset(HWAVEOUT h)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (!w) { LeaveCriticalSection(&s_wl); return o_reset(h); }
    if (!w->paused) mixer_pos(w);
    wc_reset(&w->wc, now_s());
    LONGLONG seq = 0;
    for (int i = 0; i < w->nq; i++) {
        LPWAVEHDR hd = w->q[i];
        hd->dwFlags = (hd->dwFlags & ~WHDR_INQUEUE) | WHDR_DONE;
        LONGLONG s = post(w, WOM_DONE, (DWORD_PTR)hd);
        if (s) seq = s;
    }
    w->nq = 0;
    LeaveCriticalSection(&s_wl);
    wait_cb(seq);
    LOG_TRACE("waveOutReset(%p)", (void *)h);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_breakloop(HWAVEOUT h)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (!w) { LeaveCriticalSection(&s_wl); return o_breakloop(h); }
    if (!w->paused) mixer_pos(w);
    wc_breakloop(&w->wc, now_s());
    LeaveCriticalSection(&s_wl);
    return MMSYSERR_NOERROR;
}

static MMRESULT WINAPI w_getpos(HWAVEOUT h, LPMMTIME t, UINT sz)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (!w) { LeaveCriticalSection(&s_wl); return o_getpos(h, t, sz); }
    if (!t || sz < sizeof(MMTIME)) { LeaveCriticalSection(&s_wl); return MMSYSERR_INVALPARAM; }
    uint64_t frames = (uint64_t)wc_position(&w->wc, now_s());
    const WAVEFORMATEX *f = &w->fmt.Format;
    LeaveCriticalSection(&s_wl);
    switch (t->wType) {
    case TIME_SAMPLES: t->u.sample = (DWORD)frames; break;
    case TIME_MS:      t->u.ms = (DWORD)(frames * 1000 / f->nSamplesPerSec); break;
    default:           t->wType = TIME_BYTES; /* (fall through) */
    case TIME_BYTES:   t->u.cb = (DWORD)(frames * f->nBlockAlign); break;
    }
    return MMSYSERR_NOERROR;
}

static MMRESULT WINAPI w_getpitch(HWAVEOUT h, LPDWORD v)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    return w ? MMSYSERR_NOTSUPPORTED : o_getpitch(h, v);
}
static MMRESULT WINAPI w_setpitch(HWAVEOUT h, DWORD v)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    return w ? MMSYSERR_NOTSUPPORTED : o_setpitch(h, v);
}
static MMRESULT WINAPI w_getrate(HWAVEOUT h, LPDWORD v)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    return w ? MMSYSERR_NOTSUPPORTED : o_getrate(h, v);
}
static MMRESULT WINAPI w_setrate(HWAVEOUT h, DWORD v)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    return w ? MMSYSERR_NOTSUPPORTED : o_setrate(h, v);
}

/* the volume of a stream, or (given a device id) of every stream and the ones to come */
static MMRESULT WINAPI w_getvol(HWAVEOUT h, LPDWORD v)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    int dev = !w && our_id((UINT_PTR)h);
    DWORD vol = w ? w->volume : s_vol_dev;
    LeaveCriticalSection(&s_wl);
    if (!w && !dev) return o_getvol(h, v);
    if (!v) return MMSYSERR_INVALPARAM;
    *v = vol;
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_setvol(HWAVEOUT h, DWORD v)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    if (w) set_volume(w, v);
    else if (our_id((UINT_PTR)h)) {
        s_vol_dev = v;
        for (rl_wave *x = s_waves; x; x = x->next) set_volume(x, v);
    } else { LeaveCriticalSection(&s_wl); return o_setvol(h, v); }
    LeaveCriticalSection(&s_wl);
    LOG_CALL("waveOutSetVolume(%p, %08lx)", (void *)h, (unsigned long)v);
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_getid(HWAVEOUT h, LPUINT id)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    if (!w) return o_getid(h, id);
    if (!id) return MMSYSERR_INVALPARAM;
    *id = 0;
    return MMSYSERR_NOERROR;
}
static MMRESULT WINAPI w_message(HWAVEOUT h, UINT m, DWORD_PTR a, DWORD_PTR b)
{
    EnterCriticalSection(&s_wl);
    rl_wave *w = ours(h);
    LeaveCriticalSection(&s_wl);
    if (!w && !our_id((UINT_PTR)h)) return o_message(h, m, a, b);
    if (m == DRVM_MAPPER_PREFERRED_GET) {   /* the preferred device: ours, the only one */
        if (a) *(DWORD *)a = 0;
        if (b) *(DWORD *)b = 0;
        return MMSYSERR_NOERROR;
    }
    return MMSYSERR_NOTSUPPORTED;
}

/* ------------------------------------------------------------------ binding (winmm.dll, reAL's proxy) */
static const struct { const char *name; void *fn; void **orig; } s_fns[] = {
    { "waveOutGetNumDevs", (void *)w_getnumdevs, NULL },
    { "waveOutGetDevCapsA", (void *)w_getcapsA, (void **)&o_getcapsA },
    { "waveOutGetDevCapsW", (void *)w_getcapsW, (void **)&o_getcapsW },
    { "waveOutOpen", (void *)w_open, (void **)&o_open },
    { "waveOutClose", (void *)w_close, (void **)&o_close },
    { "waveOutPrepareHeader", (void *)w_prep, (void **)&o_prep },
    { "waveOutUnprepareHeader", (void *)w_unprep, (void **)&o_unprep },
    { "waveOutWrite", (void *)w_write, (void **)&o_write },
    { "waveOutPause", (void *)w_pause, (void **)&o_pause },
    { "waveOutRestart", (void *)w_restart, (void **)&o_restart },
    { "waveOutReset", (void *)w_reset, (void **)&o_reset },
    { "waveOutBreakLoop", (void *)w_breakloop, (void **)&o_breakloop },
    { "waveOutGetPosition", (void *)w_getpos, (void **)&o_getpos },
    { "waveOutGetPitch", (void *)w_getpitch, (void **)&o_getpitch },
    { "waveOutSetPitch", (void *)w_setpitch, (void **)&o_setpitch },
    { "waveOutGetPlaybackRate", (void *)w_getrate, (void **)&o_getrate },
    { "waveOutSetPlaybackRate", (void *)w_setrate, (void **)&o_setrate },
    { "waveOutGetVolume", (void *)w_getvol, (void **)&o_getvol },
    { "waveOutSetVolume", (void *)w_setvol, (void **)&o_setvol },
    { "waveOutGetID", (void *)w_getid, (void **)&o_getid },
    { "waveOutMessage", (void *)w_message, (void **)&o_message },
};

static int wave_init(HMODULE sys)
{
    for (size_t i = 0; i < sizeof(s_fns) / sizeof(s_fns[0]); i++) {
        if (!s_fns[i].orig) continue;
        void *f = (void *)GetProcAddress(sys, s_fns[i].name);
        if (!f) {
            LOG_INFO("waveOut: the system's winmm has no %s: waveOut stays the system's", s_fns[i].name);
            return 0;
        }
        *s_fns[i].orig = f;
    }
    *(FARPROC *)&o_getnumdevs = GetProcAddress(sys, "waveOutGetNumDevs");
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    s_qpf = (double)f.QuadPart;
    InitializeCriticalSection(&s_wl);
    InitializeCriticalSection(&s_cbl);
    s_tick_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_cb_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_cb_ran = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE cbt = CreateThread(NULL, 0, cb_thread, NULL, 0, &s_cb_tid);
    s_tick_thread = CreateThread(NULL, 0, tick_thread, NULL, 0, NULL);
    if (!s_tick_wake || !s_cb_wake || !s_cb_ran || !cbt || !s_tick_thread) {
        LOG_INFO("waveOut: cannot start reAL's threads: waveOut stays the system's");
        return 0;     /* (a thread that did start waits for good: nothing is ever queued to it) */
    }
    CloseHandle(cbt);
    SetThreadPriority(s_tick_thread, THREAD_PRIORITY_HIGHEST);
    return 1;
}

/* called by reAL's winmm.dll for each export it may hand to reAL, the first time one is called: reAL's function, or
 * NULL to leave the system's.  The MIDI and MCI ones stay the system's: reAL hooks those in the system's winmm
 * itself (rl_midi_hook), which catches the calls winmm makes internally too, as from the MCI sequencer. */
FARPROC WINAPI reAL_winmm_bind(HMODULE sys, const char *name)
{
    static LONG once;
    static int wave_ok;
    if (!InterlockedExchange(&once, 1)) {
        LOG_INFO("winmm: reAL's winmm.dll loaded reAL (waveout=%d, midi=%d)", g_cfg.waveout, g_cfg.midi);
        rl_midi_hook(1);
        if (g_cfg.enabled && g_cfg.waveout) wave_ok = wave_init(sys);
    }
    if (!wave_ok || !name) return NULL;
    for (size_t i = 0; i < sizeof(s_fns) / sizeof(s_fns[0]); i++)
        if (!strcmp(s_fns[i].name, name)) return (FARPROC)s_fns[i].fn;
    return NULL;
}
