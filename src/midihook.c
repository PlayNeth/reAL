/* winmm MIDI / MCI hooks.
 *
 * Games older than DirectMusic (Claw, ...) play music through winmm: the midiOut / midiStream API or the MCI
 * sequencer.  reAL is loaded into those games anyway (dsound.dll), so it hooks winmm's functions in place, like the
 * COM hooks for DirectMusic, instead of needing a winmm.dll proxy.
 *
 * miditrace=true: every call is logged with its arguments and result and passed on unchanged.  Calls winmm makes
 * internally while serving another one (the MCI sequencer driving midiOut, a stream thread) are indented. */
#include "real.h"
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

static __thread int t_depth;
#define IND (_d ? "    " : "")
#define ENTER() int _d = t_depth++
#define LEAVE() (t_depth--)

static void hexs(char *out, int n, const BYTE *p, DWORD len)
{
    int o = 0;
    out[0] = 0;
    for (DWORD i = 0; i < len && o + 4 < n; i++) o += snprintf(out + o, n - o, "%02x ", p[i]);
    if (len * 3 >= (DWORD)n - 4 && o + 4 < n) snprintf(out + o, n - o, "...");
}

/* ------------------------------------------------------------------ the reAL-Music MIDI device
 * midi=true: a General MIDI software synth device, "reAL-Music GM Synth", added after the system's MIDI outputs and
 * standing in for the MIDI mapper.  It is reAL-Music's synth (Microsoft's DLS synth) with every instrument of gm.dls
 * (or gmdls=) loaded, as the Microsoft GS Wavetable synth has them, playing through reAL like any game sound. */
#include "music/dmhost.h"

#define RM_MAGIC 0x4d4c4552     /* 'RELM' */
typedef struct rm_stream rm_stream;
typedef struct {
    DWORD magic;
    DWORD_PTR cb, inst;
    DWORD flags;
    DWORD volume;
    BYTE running;               /* running status */
    rm_stream *st;              /* a midiStream handle */
} rm_midiout;

static dm_host *s_host;
static CRITICAL_SECTION s_mcs;
static rm_midiout *s_outs[16];
static const char RM_NAME[] = "reAL-Music GM Synth";

static rm_midiout *ours(HMIDIOUT h)
{
    if (!h) return NULL;
    for (int i = 0; i < 16; i++) if (s_outs[i] && (HMIDIOUT)s_outs[i] == h) return s_outs[i];
    return NULL;
}

static int s_host_failed;     /* don't retry on every message; midiOutOpen tries again */
static dm_host *midi_host(void)
{
    if (s_host || s_host_failed) return s_host;
    s_host = dm_host_new(g_cfg.midi_rate, 64, 1);
    if (!s_host) { s_host_failed = 1; LOG_INFO("midi: cannot start the synth (no audio device?)"); return NULL; }
    dm_host_set_reverb(s_host, g_cfg.midi_reverb);
    const char *gm = dm_gm_dls_path();
    dm_collection *c = dm_collection_from_file(gm, 1);
    if (c) LOG_INFO("midi: %s: %d instruments loaded", gm, dm_host_download_all(s_host, c));
    else LOG_INFO("midi: no GM collection (%s): the synth has no instruments; set gmdls= to a DLS file", gm);
    return s_host;
}

static void midi_send(const BYTE *m, uint32_t len)
{
    dm_host *h = midi_host();
    if (h) dm_host_midi(h, m, len);
}

static void callback(rm_midiout *o, UINT msg, DWORD_PTR p1)
{
    static BOOL (WINAPI *dcb)(DWORD_PTR, DWORD, HDRVR, DWORD, DWORD_PTR, DWORD_PTR, DWORD_PTR);
    if ((o->flags & CALLBACK_TYPEMASK) == CALLBACK_NULL) return;
    if (!dcb) *(FARPROC *)&dcb = GetProcAddress(GetModuleHandleA("winmm.dll"), "DriverCallback");
    if (dcb) dcb(o->cb, HIWORD(o->flags & CALLBACK_TYPEMASK), (HDRVR)o, msg, o->inst, p1, 0);
}

static UINT real_devs(void);
static int is_rm_id(UINT_PTR id)
{
    if (!g_cfg.midi) return 0;
    if (id == MIDI_MAPPER || id == 0xffff) return 1;
    return id == real_devs();
}

static void fill_caps(MIDIOUTCAPSW *w, MIDIOUTCAPSA *a)
{
    WORD mid = 1, pid = 102;    /* MM_MICROSOFT, MM_MSFT_GENERIC_MIDISYNTH */
    DWORD support = MIDICAPS_VOLUME | MIDICAPS_LRVOLUME;
    if (w) {
        memset(w, 0, sizeof(*w));
        w->wMid = mid; w->wPid = pid; w->vDriverVersion = 0x0100;
        MultiByteToWideChar(CP_ACP, 0, RM_NAME, -1, w->szPname, MAXPNAMELEN);
        w->wTechnology = MOD_SWSYNTH; w->wVoices = 64; w->wNotes = 64; w->wChannelMask = 0xffff; w->dwSupport = support;
    }
    if (a) {
        memset(a, 0, sizeof(*a));
        a->wMid = mid; a->wPid = pid; a->vDriverVersion = 0x0100;
        lstrcpynA(a->szPname, RM_NAME, MAXPNAMELEN);
        a->wTechnology = MOD_SWSYNTH; a->wVoices = 64; a->wNotes = 64; a->wChannelMask = 0xffff; a->dwSupport = support;
    }
}

#define TRACE(...) do { if (g_cfg.midi_trace) LOG_INFO(__VA_ARGS__); } while (0)
static void stream_flush(rm_stream *st);

/* ------------------------------------------------------------------ midiOut */
static UINT (WINAPI *o_getnumdevs)(void);
static MMRESULT (WINAPI *o_getcapsA)(UINT_PTR, LPMIDIOUTCAPSA, UINT);
static MMRESULT (WINAPI *o_getcapsW)(UINT_PTR, LPMIDIOUTCAPSW, UINT);
static MMRESULT (WINAPI *o_open)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT (WINAPI *o_close)(HMIDIOUT);
static MMRESULT (WINAPI *o_short)(HMIDIOUT, DWORD);
static MMRESULT (WINAPI *o_long)(HMIDIOUT, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *o_prep)(HMIDIOUT, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *o_unprep)(HMIDIOUT, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *o_reset)(HMIDIOUT);
static MMRESULT (WINAPI *o_setvol)(HMIDIOUT, DWORD);
static MMRESULT (WINAPI *o_getvol)(HMIDIOUT, LPDWORD);
static MMRESULT (WINAPI *o_message)(HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR);
static MMRESULT (WINAPI *o_cache)(HMIDIOUT, UINT, LPWORD, UINT);
static MMRESULT (WINAPI *o_getid)(HMIDIOUT, LPUINT);

static UINT real_devs(void) { return o_getnumdevs ? o_getnumdevs() : 0; }

static UINT WINAPI h_getnumdevs(void)
{
    ENTER(); UINT r = o_getnumdevs(); LEAVE();
    if (g_cfg.midi && !_d) r++;
    TRACE("midi: %smidiOutGetNumDevs() -> %u", IND, r);
    return r;
}
static MMRESULT WINAPI h_getcapsA(UINT_PTR id, LPMIDIOUTCAPSA c, UINT sz)
{
    ENTER();
    MMRESULT r;
    if (!_d && is_rm_id(id) && c && sz >= sizeof(*c)) { fill_caps(NULL, c); r = 0; }
    else r = o_getcapsA(id, c, sz);
    LEAVE();
    TRACE("midi: %smidiOutGetDevCapsA(%ld) -> %u %s", IND, (long)id, r, r ? "" : c->szPname);
    return r;
}
static MMRESULT WINAPI h_getcapsW(UINT_PTR id, LPMIDIOUTCAPSW c, UINT sz)
{
    ENTER();
    MMRESULT r;
    if (!_d && is_rm_id(id) && c && sz >= sizeof(*c)) { fill_caps(c, NULL); r = 0; }
    else r = o_getcapsW(id, c, sz);
    LEAVE();
    char name[64] = "";
    if (!r) WideCharToMultiByte(CP_ACP, 0, c->szPname, -1, name, sizeof(name), NULL, NULL);
    TRACE("midi: %smidiOutGetDevCapsW(%ld) -> %u %s", IND, (long)id, r, name);
    return r;
}
static MMRESULT WINAPI h_open(LPHMIDIOUT ph, UINT id, DWORD_PTR cb, DWORD_PTR inst, DWORD fl)
{
    ENTER();
    MMRESULT r;
    if (!_d && is_rm_id(id)) {
        r = MMSYSERR_NOMEM;
        rm_midiout *o = calloc(1, sizeof(*o));
        if (o && ph) {
            o->magic = RM_MAGIC; o->cb = cb; o->inst = inst; o->flags = fl; o->volume = 0xffffffff;
            EnterCriticalSection(&s_mcs);
            for (int i = 0; i < 16; i++) if (!s_outs[i]) { s_outs[i] = o; r = 0; break; }
            LeaveCriticalSection(&s_mcs);
            if (!r) { *ph = (HMIDIOUT)o; s_host_failed = 0; midi_host(); callback(o, MOM_OPEN, 0); }
            else free(o);
        } else { free(o); r = MMSYSERR_INVALPARAM; }
        LOG_INFO("midi: midiOutOpen(%d) -> reAL-Music GM Synth (%u)", (int)id, r);
    } else r = o_open(ph, id, cb, inst, fl);
    LEAVE();
    TRACE("midi: %smidiOutOpen(dev %d, callback %p, inst %p, flags %lx) -> %u handle %p", IND, (int)id, (void *)cb,
          (void *)inst, (unsigned long)fl, r, ph && !r ? (void *)*ph : NULL);
    return r;
}
static void all_off(void)
{
    for (int c = 0; c < 16; c++) {
        BYTE m1[3] = { (BYTE)(0xb0 | c), 0x40, 0 }, m2[3] = { (BYTE)(0xb0 | c), 0x7b, 0 };
        midi_send(m1, 3); midi_send(m2, 3);
    }
}
static MMRESULT WINAPI h_close(HMIDIOUT h)
{
    ENTER();
    MMRESULT r;
    rm_midiout *o = ours(h);
    if (o && o->st) { r = MMSYSERR_INVALHANDLE; }
    else if (o) {
        all_off();
        EnterCriticalSection(&s_mcs);
        for (int i = 0; i < 16; i++) if (s_outs[i] == o) s_outs[i] = NULL;
        LeaveCriticalSection(&s_mcs);
        callback(o, MOM_CLOSE, 0);
        free(o);
        r = 0;
    } else r = o_close(h);
    LEAVE();
    TRACE("midi: %smidiOutClose(%p) -> %u", IND, (void *)h, r);
    return r;
}
static MMRESULT WINAPI h_short(HMIDIOUT h, DWORD m)
{
    ENTER();
    MMRESULT r = 0;
    rm_midiout *o = ours(h);
    if (o) {
        BYTE b[3] = { (BYTE)m, (BYTE)(m >> 8), (BYTE)(m >> 16) };
        if (b[0] < 0x80) {      /* running status */
            if (!o->running) { LEAVE(); return MIDIERR_BADOPENMODE; }
            b[2] = b[1]; b[1] = b[0]; b[0] = o->running;
        } else if (b[0] < 0xf0) o->running = b[0];
        if (b[0] < 0xf0) {
            uint32_t len = (b[0] & 0xe0) == 0xc0 ? 2 : 3;
            midi_send(b, len);
        }
    } else r = o_short(h, m);
    LEAVE();
    TRACE("midi: %sshort %p %02x %02x %02x%s", IND, (void *)h, (unsigned)(m & 0xff), (unsigned)((m >> 8) & 0xff),
          (unsigned)((m >> 16) & 0xff), r ? " FAILED" : "");
    return r;
}
static MMRESULT WINAPI h_long(HMIDIOUT h, LPMIDIHDR hd, UINT sz)
{
    char x[200];
    if (hd) hexs(x, sizeof(x), (const BYTE *)hd->lpData, hd->dwBufferLength); else x[0] = 0;
    ENTER();
    MMRESULT r;
    rm_midiout *o = ours(h);
    if (o) {
        if (!hd || !hd->lpData) r = MMSYSERR_INVALPARAM;
        else if (!(hd->dwFlags & MHDR_PREPARED)) r = MIDIERR_UNPREPARED;
        else {
            const BYTE *d = (const BYTE *)hd->lpData;
            DWORD n = hd->dwBufferLength;
            /* a buffer may hold several messages: sysex blocks, or short messages */
            for (DWORD i = 0; i < n; ) {
                if (d[i] == 0xf0) {
                    DWORD e = i + 1;
                    while (e < n && d[e] != 0xf7) e++;
                    if (e < n) e++;
                    midi_send(d + i, e - i);
                    i = e;
                } else if (d[i] >= 0x80 && d[i] < 0xf0) {
                    uint32_t len = (d[i] & 0xe0) == 0xc0 ? 2 : 3;
                    if (i + len <= n) midi_send(d + i, len);
                    o->running = d[i];
                    i += len;
                } else i++;
            }
            hd->dwFlags &= ~MHDR_INQUEUE;
            hd->dwFlags |= MHDR_DONE;
            callback(o, MOM_DONE, (DWORD_PTR)hd);
            r = 0;
        }
    } else r = o_long(h, hd, sz);
    LEAVE();
    TRACE("midi: %smidiOutLongMsg(%p, %lu bytes) -> %u: %s", IND, (void *)h, hd ? (unsigned long)hd->dwBufferLength : 0UL, r, x);
    return r;
}
static MMRESULT WINAPI h_prep(HMIDIOUT h, LPMIDIHDR hd, UINT sz)
{
    ENTER();
    MMRESULT r;
    if (ours(h)) {
        if (!hd || sz < offsetof(MIDIHDR, dwOffset)) r = MMSYSERR_INVALPARAM;
        else { hd->dwFlags |= MHDR_PREPARED; hd->dwFlags &= ~MHDR_DONE; r = 0; }
    } else r = o_prep(h, hd, sz);
    LEAVE();
    TRACE("midi: %smidiOutPrepareHeader(%p, %p) -> %u", IND, (void *)h, (void *)hd, r);
    return r;
}
static MMRESULT WINAPI h_unprep(HMIDIOUT h, LPMIDIHDR hd, UINT sz)
{
    ENTER();
    MMRESULT r;
    if (ours(h)) {
        if (!hd) r = MMSYSERR_INVALPARAM;
        else if (hd->dwFlags & MHDR_INQUEUE) r = MIDIERR_STILLPLAYING;
        else { hd->dwFlags &= ~MHDR_PREPARED; r = 0; }
    } else r = o_unprep(h, hd, sz);
    LEAVE();
    TRACE("midi: %smidiOutUnprepareHeader(%p, %p) -> %u", IND, (void *)h, (void *)hd, r);
    return r;
}
static MMRESULT WINAPI h_reset(HMIDIOUT h)
{
    ENTER();
    MMRESULT r;
    rm_midiout *o = ours(h);
    if (o && o->st) { stream_flush(o->st); o->running = 0; r = 0; }
    else if (o) { all_off(); o->running = 0; r = 0; }
    else r = o_reset(h);
    LEAVE();
    TRACE("midi: %smidiOutReset(%p) -> %u", IND, (void *)h, r);
    return r;
}
static int32_t vol_mb(WORD v)
{
    if (!v) return -9600;
    double mb = 2000.0 * log10((double)v / 65535.0);
    return mb < -9600 ? -9600 : (int32_t)mb;
}
static MMRESULT WINAPI h_setvol(HMIDIOUT h, DWORD v)
{
    ENTER();
    MMRESULT r;
    rm_midiout *o = ours(h);
    if (o || (g_cfg.midi && is_rm_id((UINT_PTR)h))) {
        if (o) o->volume = v;
        WORD l = LOWORD(v), rr = HIWORD(v);
        dm_host *hh = midi_host();
        if (hh) dm_host_set_volume(hh, vol_mb((WORD)(((DWORD)l + rr) / 2)));
        r = 0;
    } else r = o_setvol(h, v);
    LEAVE();
    TRACE("midi: %smidiOutSetVolume(%p, %08lx) -> %u", IND, (void *)h, (unsigned long)v, r);
    return r;
}
static MMRESULT WINAPI h_getvol(HMIDIOUT h, LPDWORD v)
{
    ENTER();
    MMRESULT r;
    rm_midiout *o = ours(h);
    if (o) { if (v) *v = o->volume; r = v ? 0 : MMSYSERR_INVALPARAM; }
    else r = o_getvol(h, v);
    LEAVE();
    TRACE("midi: %smidiOutGetVolume(%p) -> %u %08lx", IND, (void *)h, r, v && !r ? (unsigned long)*v : 0UL);
    return r;
}
static MMRESULT WINAPI h_message(HMIDIOUT h, UINT m, DWORD_PTR a, DWORD_PTR b)
{
    ENTER();
    MMRESULT r = ours(h) ? MMSYSERR_NOTSUPPORTED : o_message(h, m, a, b);
    LEAVE();
    TRACE("midi: %smidiOutMessage(%p, %u, %p, %p) -> %u", IND, (void *)h, m, (void *)a, (void *)b, r);
    return r;
}
static MMRESULT WINAPI h_cache(HMIDIOUT h, UINT bank, LPWORD p, UINT fl)
{
    ENTER();
    MMRESULT r = ours(h) ? MMSYSERR_NOTSUPPORTED : o_cache(h, bank, p, fl);
    LEAVE();
    TRACE("midi: %smidiOutCachePatches(%p, bank %u, flags %x) -> %u", IND, (void *)h, bank, fl, r);
    return r;
}
static MMRESULT WINAPI h_getid(HMIDIOUT h, LPUINT id)
{
    ENTER();
    MMRESULT r;
    if (ours(h)) { if (id) *id = real_devs(); r = id ? 0 : MMSYSERR_INVALPARAM; }
    else r = o_getid(h, id);
    LEAVE();
    TRACE("midi: %smidiOutGetID(%p) -> %u", IND, (void *)h, r);
    return r;
}

/* ------------------------------------------------------------------ midiStream
 * Streams on the reAL-Music device: buffers of MIDIEVENTs (delta ticks, stream id, event, long data) queued with
 * midiStreamOut and played by a thread per stream, in stream time (it stops while paused).  Ticks follow the time
 * division (PPQN with the current tempo, or SMPTE frames) and MEVT_TEMPO events.  A stream opens paused; Stop and
 * midiOutReset return the queued buffers; MOM_DONE goes out when a buffer is finished, MOM_POSITIONCB for events
 * with MEVT_F_CALLBACK. */
struct rm_stream {
    rm_midiout *o;
    CRITICAL_SECTION cs;
    HANDLE thread, wake;
    volatile LONG quit;
    int running;
    DWORD timediv, tempo;           /* MIDIPROP values: ticks per quarter note (or SMPTE), microseconds per quarter */
    LPMIDIHDR q[64]; int nq;        /* queued buffers, oldest first */
    DWORD ofs;                      /* next event in q[0] */
    double due_us;                  /* stream time of the next event (its delta already added) */
    int have_due;
    double base_us;                 /* stream time accumulated before the current run */
    LARGE_INTEGER run_start, qpf;
    double last_us;                 /* stream time of the last event played */
    DWORD ticks;                    /* tick position of that event */
};

static double tick_us(rm_stream *st)
{
    if (st->timediv & 0x8000) {     /* SMPTE: -frames per second, ticks per frame */
        int fps = -(signed char)(st->timediv >> 8), tpf = st->timediv & 0xff;
        if (fps == 29) fps = 30;
        return fps > 0 && tpf > 0 ? 1e6 / ((double)fps * tpf) : 1000.0;
    }
    DWORD tpq = st->timediv & 0x7fff;
    return (double)st->tempo / (tpq ? tpq : 96);
}

static double stream_now(rm_stream *st)
{
    if (!st->running) return st->base_us;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return st->base_us + (double)(c.QuadPart - st->run_start.QuadPart) * 1e6 / (double)st->qpf.QuadPart;
}

typedef struct { UINT msg; DWORD_PTR p1; } pending_cb;

/* finish the head buffer (lock held): collect its MOM_DONE */
static void finish_head(rm_stream *st, pending_cb *cbs, int *ncb)
{
    LPMIDIHDR h = st->q[0];
    memmove(st->q, st->q + 1, (size_t)(--st->nq) * sizeof(*st->q));
    st->ofs = 0;
    st->have_due = 0;
    h->dwFlags &= ~MHDR_INQUEUE;
    h->dwFlags |= MHDR_DONE;
    if (*ncb < 64) { cbs[*ncb].msg = MOM_DONE; cbs[*ncb].p1 = (DWORD_PTR)h; (*ncb)++; }
}

/* play everything due (lock held); returns the wait in ms until the next event, INFINITE when idle */
static DWORD stream_pump(rm_stream *st, pending_cb *cbs, int *ncb)
{
    while (st->running && st->nq) {
        LPMIDIHDR h = st->q[0];
        const BYTE *d = (const BYTE *)h->lpData;
        DWORD n = h->dwBytesRecorded;
        if (st->ofs + 12 > n) { finish_head(st, cbs, ncb); continue; }
        const MIDIEVENT *e = (const MIDIEVENT *)(d + st->ofs);
        if (!st->have_due) { st->due_us += e->dwDeltaTime * tick_us(st); st->have_due = 1; }
        double now = stream_now(st);
        if (st->due_us > now) {
            double w = (st->due_us - now) / 1000.0;
            return w < 1 ? 1 : (DWORD)w;
        }
        st->ticks += e->dwDeltaTime;
        st->last_us = st->due_us;
        DWORD ev = e->dwEvent, len = 12;
        if (ev & MEVT_F_LONG) {
            DWORD l = ev & 0xffffff;
            len = 12 + ((l + 3) & ~3u);
            if (st->ofs + 12 + l > n) { finish_head(st, cbs, ncb); continue; }
            if (MEVT_EVENTTYPE(ev) == MEVT_LONGMSG) {
                const BYTE *x = d + st->ofs + 12;
                /* a long message may carry sysex or a run of short messages */
                for (DWORD i = 0; i < l; ) {
                    if (x[i] == 0xf0) { DWORD k = i + 1; while (k < l && x[k] != 0xf7) k++; if (k < l) k++; midi_send(x + i, k - i); i = k; }
                    else if (x[i] >= 0x80 && x[i] < 0xf0) { uint32_t m = (x[i] & 0xe0) == 0xc0 ? 2 : 3; if (i + m <= l) midi_send(x + i, m); i += m; }
                    else i++;
                }
            }
        } else {
            switch (MEVT_EVENTTYPE(ev & ~MEVT_F_CALLBACK)) {
            case MEVT_SHORTMSG: {
                BYTE m[3] = { (BYTE)ev, (BYTE)(ev >> 8), (BYTE)(ev >> 16) };
                if (m[0] < 0x80) { m[2] = m[1]; m[1] = m[0]; m[0] = st->o->running; }
                else if (m[0] < 0xf0) st->o->running = m[0];
                if (m[0] >= 0x80 && m[0] < 0xf0) midi_send(m, (m[0] & 0xe0) == 0xc0 ? 2 : 3);
                break;
            }
            case MEVT_TEMPO: if (ev & 0xffffff) st->tempo = ev & 0xffffff; break;
            default: break;     /* MEVT_NOP */
            }
        }
        if ((ev & MEVT_F_CALLBACK) && *ncb < 64) {
            h->dwOffset = st->ofs;
            cbs[*ncb].msg = MOM_POSITIONCB; cbs[*ncb].p1 = (DWORD_PTR)h; (*ncb)++;
        }
        st->ofs += len;
        st->have_due = 0;
        if (st->ofs >= n) finish_head(st, cbs, ncb);
    }
    return INFINITE;
}

static DWORD WINAPI stream_thread(LPVOID p)
{
    rm_stream *st = p;
    timeBeginPeriod(1);
    while (!st->quit) {
        pending_cb cbs[64];
        int ncb = 0;
        EnterCriticalSection(&st->cs);
        DWORD w = stream_pump(st, cbs, &ncb);
        LeaveCriticalSection(&st->cs);
        for (int i = 0; i < ncb; i++) callback(st->o, cbs[i].msg, cbs[i].p1);
        if (ncb && w > 0) w = 0;        /* callbacks may have queued more */
        if (w) WaitForSingleObject(st->wake, w);
    }
    timeEndPeriod(1);
    return 0;
}

/* return every queued buffer (Stop / Reset / Close) and silence; position back to 0 */
static void stream_flush(rm_stream *st)
{
    pending_cb cbs[64];
    int ncb = 0;
    EnterCriticalSection(&st->cs);
    st->running = 0;
    while (st->nq) finish_head(st, cbs, &ncb);
    st->base_us = 0; st->due_us = 0; st->have_due = 0; st->ticks = 0; st->ofs = 0; st->last_us = 0;
    LeaveCriticalSection(&st->cs);
    all_off();
    for (int i = 0; i < ncb; i++) callback(st->o, cbs[i].msg, cbs[i].p1);
}

static rm_stream *our_stream(HMIDISTRM h)
{
    rm_midiout *o = ours((HMIDIOUT)h);
    return o ? o->st : NULL;
}

static MMRESULT (WINAPI *s_open)(LPHMIDISTRM, LPUINT, DWORD, DWORD_PTR, DWORD_PTR, DWORD);
static MMRESULT (WINAPI *s_out)(HMIDISTRM, LPMIDIHDR, UINT);
static MMRESULT (WINAPI *s_restart)(HMIDISTRM);
static MMRESULT (WINAPI *s_pause)(HMIDISTRM);
static MMRESULT (WINAPI *s_stop)(HMIDISTRM);
static MMRESULT (WINAPI *s_close)(HMIDISTRM);
static MMRESULT (WINAPI *s_prop)(HMIDISTRM, LPBYTE, DWORD);
static MMRESULT (WINAPI *s_pos)(HMIDISTRM, LPMMTIME, UINT);

static MMRESULT WINAPI hs_open(LPHMIDISTRM ph, LPUINT dev, DWORD n, DWORD_PTR cb, DWORD_PTR inst, DWORD fl)
{
    UINT d = dev ? *dev : 0;
    ENTER();
    MMRESULT r;
    if (!_d && dev && is_rm_id(d)) {
        r = MMSYSERR_NOMEM;
        rm_midiout *o = calloc(1, sizeof(*o));
        rm_stream *st = calloc(1, sizeof(*st));
        if (o && st && ph) {
            o->magic = RM_MAGIC; o->cb = cb; o->inst = inst; o->flags = fl; o->volume = 0xffffffff; o->st = st;
            st->o = o; st->timediv = 96; st->tempo = 500000;
            InitializeCriticalSection(&st->cs);
            QueryPerformanceFrequency(&st->qpf);
            st->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
            EnterCriticalSection(&s_mcs);
            for (int i = 0; i < 16; i++) if (!s_outs[i]) { s_outs[i] = o; r = 0; break; }
            LeaveCriticalSection(&s_mcs);
            if (!r) {
                st->thread = CreateThread(NULL, 0, stream_thread, st, 0, NULL);
                SetThreadPriority(st->thread, THREAD_PRIORITY_TIME_CRITICAL);
                *ph = (HMIDISTRM)o;
                *dev = real_devs();
                midi_host();
                callback(o, MOM_OPEN, 0);
            }
        }
        if (r) { if (st) { if (st->wake) CloseHandle(st->wake); free(st); } free(o); if (!ph) r = MMSYSERR_INVALPARAM; }
        LOG_INFO("midi: midiStreamOpen(%d) -> reAL-Music GM Synth (%u)", (int)d, r);
    } else r = s_open(ph, dev, n, cb, inst, fl);
    LEAVE();
    TRACE("midi: %smidiStreamOpen(dev %d, %lu, callback %p, flags %lx) -> %u handle %p", IND, (int)d, (unsigned long)n,
          (void *)cb, (unsigned long)fl, r, ph && !r ? (void *)*ph : NULL);
    return r;
}
static MMRESULT WINAPI hs_out(HMIDISTRM h, LPMIDIHDR hd, UINT sz)
{
    char x[200];
    if (hd && hd->lpData) hexs(x, sizeof(x), (const BYTE *)hd->lpData, hd->dwBytesRecorded < 48 ? hd->dwBytesRecorded : 48); else x[0] = 0;
    ENTER();
    MMRESULT r;
    rm_stream *st = our_stream(h);
    if (st) {
        if (!hd || !hd->lpData || sz < offsetof(MIDIHDR, dwOffset) || hd->dwBytesRecorded > hd->dwBufferLength) r = MMSYSERR_INVALPARAM;
        else if (!(hd->dwFlags & MHDR_PREPARED)) r = MIDIERR_UNPREPARED;
        else if (hd->dwFlags & MHDR_INQUEUE) r = MIDIERR_STILLPLAYING;
        else {
            EnterCriticalSection(&st->cs);
            if (st->nq < 64) {
                hd->dwFlags = (hd->dwFlags & ~MHDR_DONE) | MHDR_INQUEUE;
                st->q[st->nq++] = hd;
                r = 0;
            } else r = MMSYSERR_NOMEM;
            LeaveCriticalSection(&st->cs);
            SetEvent(st->wake);
        }
    } else r = s_out(h, hd, sz);
    LEAVE();
    TRACE("midi: %smidiStreamOut(%p, %lu bytes) -> %u: %s", IND, (void *)h, hd ? (unsigned long)hd->dwBytesRecorded : 0UL, r, x);
    return r;
}
static MMRESULT WINAPI hs_restart(HMIDISTRM h)
{
    ENTER();
    MMRESULT r = 0;
    rm_stream *st = our_stream(h);
    if (st) {
        EnterCriticalSection(&st->cs);
        if (!st->running) { QueryPerformanceCounter(&st->run_start); st->running = 1; }
        LeaveCriticalSection(&st->cs);
        SetEvent(st->wake);
    } else r = s_restart(h);
    LEAVE();
    TRACE("midi: %smidiStreamRestart(%p) -> %u", IND, (void *)h, r);
    return r;
}
static MMRESULT WINAPI hs_pause(HMIDISTRM h)
{
    ENTER();
    MMRESULT r = 0;
    rm_stream *st = our_stream(h);
    if (st) {
        EnterCriticalSection(&st->cs);
        if (st->running) { st->base_us = stream_now(st); st->running = 0; }
        LeaveCriticalSection(&st->cs);
        SetEvent(st->wake);
    } else r = s_pause(h);
    LEAVE();
    TRACE("midi: %smidiStreamPause(%p) -> %u", IND, (void *)h, r);
    return r;
}
static MMRESULT WINAPI hs_stop(HMIDISTRM h)
{
    ENTER();
    MMRESULT r = 0;
    rm_stream *st = our_stream(h);
    if (st) { stream_flush(st); SetEvent(st->wake); }
    else r = s_stop(h);
    LEAVE();
    TRACE("midi: %smidiStreamStop(%p) -> %u", IND, (void *)h, r);
    return r;
}
static void stream_close(rm_midiout *o)
{
    rm_stream *st = o->st;
    stream_flush(st);
    InterlockedExchange(&st->quit, 1);
    SetEvent(st->wake);
    if (GetCurrentThreadId() != GetThreadId(st->thread)) WaitForSingleObject(st->thread, 2000);
    CloseHandle(st->thread);
    CloseHandle(st->wake);
    DeleteCriticalSection(&st->cs);
    EnterCriticalSection(&s_mcs);
    for (int i = 0; i < 16; i++) if (s_outs[i] == o) s_outs[i] = NULL;
    LeaveCriticalSection(&s_mcs);
    callback(o, MOM_CLOSE, 0);
    free(st);
    free(o);
}
static MMRESULT WINAPI hs_close(HMIDISTRM h)
{
    ENTER();
    MMRESULT r = 0;
    rm_midiout *o = ours((HMIDIOUT)h);
    if (o && o->st) stream_close(o);
    else r = s_close(h);
    LEAVE();
    TRACE("midi: %smidiStreamClose(%p) -> %u", IND, (void *)h, r);
    return r;
}
static MMRESULT WINAPI hs_prop(HMIDISTRM h, LPBYTE p, DWORD fl)
{
    ENTER();
    MMRESULT r;
    rm_stream *st = our_stream(h);
    if (st) {
        DWORD what = fl & ~(MIDIPROP_GET | MIDIPROP_SET);
        if (!p || !(fl & (MIDIPROP_GET | MIDIPROP_SET)) || (fl & MIDIPROP_GET && fl & MIDIPROP_SET)) r = MMSYSERR_INVALPARAM;
        else if (what != MIDIPROP_TIMEDIV && what != MIDIPROP_TEMPO) r = MMSYSERR_INVALPARAM;
        else {
            DWORD *v = &((DWORD *)p)[1];
            EnterCriticalSection(&st->cs);
            DWORD *field = what == MIDIPROP_TEMPO ? &st->tempo : &st->timediv;
            if (fl & MIDIPROP_GET) *v = *field;
            else if (what == MIDIPROP_TIMEDIV && st->running) r = MMSYSERR_INVALPARAM;     /* only while stopped / paused */
            else *field = *v ? *v : *field;
            LeaveCriticalSection(&st->cs);
            r = 0;
        }
    } else r = s_prop(h, p, fl);
    LEAVE();
    unsigned long v = p ? (unsigned long)((DWORD *)p)[1] : 0;
    TRACE("midi: %smidiStreamProperty(%p, flags %lx) -> %u value %lu", IND, (void *)h, (unsigned long)fl, r, v);
    return r;
}
static MMRESULT WINAPI hs_pos(HMIDISTRM h, LPMMTIME t, UINT sz)
{
    ENTER();
    MMRESULT r;
    rm_stream *st = our_stream(h);
    if (st) {
        if (!t || sz < sizeof(MMTIME)) r = MMSYSERR_INVALPARAM;
        else {
            EnterCriticalSection(&st->cs);
            double now = stream_now(st);
            if (t->wType == TIME_TICKS) {
                /* the last event's tick plus the ticks elapsed since it */
                double tu = tick_us(st);
                t->u.ticks = st->ticks + (now > st->last_us ? (DWORD)((now - st->last_us) / tu) : 0);
            } else { t->wType = TIME_MS; t->u.ms = (DWORD)(now / 1000.0); }
            LeaveCriticalSection(&st->cs);
            r = 0;
        }
    } else r = s_pos(h, t, sz);
    LEAVE();
    if (g_cfg.log_level >= 3) TRACE("midi: %smidiStreamPosition(%p) -> %u", IND, (void *)h, r);
    return r;
}

/* ------------------------------------------------------------------ MCI */
static MCIERROR (WINAPI *m_cmdA)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
static MCIERROR (WINAPI *m_cmdW)(MCIDEVICEID, UINT, DWORD_PTR, DWORD_PTR);
static MCIERROR (WINAPI *m_strA)(LPCSTR, LPSTR, UINT, HWND);
static MCIERROR (WINAPI *m_strW)(LPCWSTR, LPWSTR, UINT, HWND);

static const char *mci_name(UINT m)
{
    switch (m) {
    case MCI_OPEN: return "OPEN"; case MCI_CLOSE: return "CLOSE"; case MCI_PLAY: return "PLAY"; case MCI_STOP: return "STOP";
    case MCI_PAUSE: return "PAUSE"; case MCI_RESUME: return "RESUME"; case MCI_SEEK: return "SEEK"; case MCI_STATUS: return "STATUS";
    case MCI_SET: return "SET"; case MCI_GETDEVCAPS: return "GETDEVCAPS"; case MCI_INFO: return "INFO"; case MCI_SYSINFO: return "SYSINFO";
    default: return "?";
    }
}
static void mci_log(const char *fn, MCIDEVICEID id, UINT m, DWORD_PTR fl, DWORD_PTR p, MCIERROR r, int wide, int _d)
{
    char extra[300] = "";
    if (m == MCI_OPEN && p) {
        char t[MAX_PATH] = "", e[MAX_PATH] = "";
        if (wide) {
            MCI_OPEN_PARMSW *o = (MCI_OPEN_PARMSW *)p;
            if ((fl & MCI_OPEN_TYPE) && !(fl & MCI_OPEN_TYPE_ID) && o->lpstrDeviceType) WideCharToMultiByte(CP_ACP, 0, o->lpstrDeviceType, -1, t, MAX_PATH, NULL, NULL);
            if ((fl & MCI_OPEN_ELEMENT) && o->lpstrElementName) WideCharToMultiByte(CP_ACP, 0, o->lpstrElementName, -1, e, MAX_PATH, NULL, NULL);
            snprintf(extra, sizeof(extra), " type '%s' element '%s' -> id %u", t, e, o->wDeviceID);
        } else {
            MCI_OPEN_PARMSA *o = (MCI_OPEN_PARMSA *)p;
            if ((fl & MCI_OPEN_TYPE) && !(fl & MCI_OPEN_TYPE_ID) && o->lpstrDeviceType) lstrcpynA(t, o->lpstrDeviceType, MAX_PATH);
            if ((fl & MCI_OPEN_TYPE_ID)) snprintf(t, sizeof(t), "#%lx", (unsigned long)(DWORD_PTR)o->lpstrDeviceType);
            if ((fl & MCI_OPEN_ELEMENT) && o->lpstrElementName) lstrcpynA(e, o->lpstrElementName, MAX_PATH);
            snprintf(extra, sizeof(extra), " type '%s' element '%s' -> id %u", t, e, o->wDeviceID);
        }
    } else if (m == MCI_PLAY && p && (fl & (MCI_FROM | MCI_TO))) {
        MCI_PLAY_PARMS *pp = (MCI_PLAY_PARMS *)p;
        snprintf(extra, sizeof(extra), " from %lu to %lu", (unsigned long)pp->dwFrom, (unsigned long)pp->dwTo);
    } else if (m == MCI_STATUS && p) {
        MCI_STATUS_PARMS *sp = (MCI_STATUS_PARMS *)p;
        snprintf(extra, sizeof(extra), " item %lu -> %lu", (unsigned long)sp->dwItem, (unsigned long)sp->dwReturn);
    } else if (m == MCI_SET && p) {
        MCI_SET_PARMS *sp = (MCI_SET_PARMS *)p;
        snprintf(extra, sizeof(extra), " timeformat %lu", (unsigned long)sp->dwTimeFormat);
    }
    if (m == MCI_STATUS && g_cfg.log_level < 3 && !r) return;      /* polled constantly */
    TRACE("midi: %s%s(dev %u, %s %#x, flags %lx)%s -> %lu", IND, fn, id, mci_name(m), m, (unsigned long)fl, extra, (unsigned long)r);
}
static MCIERROR WINAPI hm_cmdA(MCIDEVICEID id, UINT m, DWORD_PTR fl, DWORD_PTR p)
{
    ENTER(); MCIERROR r = m_cmdA(id, m, fl, p); LEAVE();
    mci_log("mciSendCommandA", id, m, fl, p, r, 0, _d);
    return r;
}
static MCIERROR WINAPI hm_cmdW(MCIDEVICEID id, UINT m, DWORD_PTR fl, DWORD_PTR p)
{
    ENTER(); MCIERROR r = m_cmdW(id, m, fl, p); LEAVE();
    mci_log("mciSendCommandW", id, m, fl, p, r, 1, _d);
    return r;
}
static MCIERROR WINAPI hm_strA(LPCSTR cmd, LPSTR ret, UINT n, HWND w)
{
    ENTER(); MCIERROR r = m_strA(cmd, ret, n, w); LEAVE();
    if (g_cfg.log_level >= 3 || r || !cmd || _strnicmp(cmd, "status", 6))
        TRACE("midi: %smciSendStringA(\"%s\") -> %lu \"%s\"", IND, cmd ? cmd : "", (unsigned long)r, ret && n && !r ? ret : "");
    return r;
}
static MCIERROR WINAPI hm_strW(LPCWSTR cmd, LPWSTR ret, UINT n, HWND w)
{
    ENTER(); MCIERROR r = m_strW(cmd, ret, n, w); LEAVE();
    char c[300] = "", o[200] = "";
    if (cmd) WideCharToMultiByte(CP_ACP, 0, cmd, -1, c, sizeof(c), NULL, NULL);
    if (ret && n && !r) WideCharToMultiByte(CP_ACP, 0, ret, -1, o, sizeof(o), NULL, NULL);
    if (g_cfg.log_level >= 3 || r || _strnicmp(c, "status", 6))
        TRACE("midi: %smciSendStringW(\"%s\") -> %lu \"%s\"", IND, c, (unsigned long)r, o);
    return r;
}

/* ------------------------------------------------------------------ install */
#define HOOK(name, var, fn) do { void *f = (void *)GetProcAddress(mm, name); \
        if (f) { void *t = rl_hotpatch(f, (void *)(fn), name); if (t) *(void **)&(var) = t; } } while (0)

void rl_midi_hook(int can_load)
{
    static LONG done;
    if (!g_cfg.enabled || !(g_cfg.midi_trace || g_cfg.midi) || done) return;
    HMODULE mm = GetModuleHandleA("winmm.dll");
    if (!mm && can_load) mm = LoadLibraryA("winmm.dll");
    if (!mm) return;
    if (InterlockedExchange(&done, 1)) return;     /* hook once: a second patch would wrap our own hooks */
    HMODULE pin;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)rl_midi_hook, &pin);
    InitializeCriticalSection(&s_mcs);
    if (g_cfg.midi) LOG_INFO("midi: reAL-Music GM Synth replaces the MIDI mapper");
    if (g_cfg.midi_trace) LOG_INFO("midi: tracing winmm MIDI and MCI calls");
    HOOK("midiOutGetNumDevs", o_getnumdevs, h_getnumdevs);
    HOOK("midiOutGetDevCapsA", o_getcapsA, h_getcapsA);
    HOOK("midiOutGetDevCapsW", o_getcapsW, h_getcapsW);
    HOOK("midiOutOpen", o_open, h_open);
    HOOK("midiOutClose", o_close, h_close);
    HOOK("midiOutShortMsg", o_short, h_short);
    HOOK("midiOutLongMsg", o_long, h_long);
    HOOK("midiOutPrepareHeader", o_prep, h_prep);
    HOOK("midiOutUnprepareHeader", o_unprep, h_unprep);
    HOOK("midiOutReset", o_reset, h_reset);
    HOOK("midiOutSetVolume", o_setvol, h_setvol);
    HOOK("midiOutGetVolume", o_getvol, h_getvol);
    HOOK("midiOutMessage", o_message, h_message);
    HOOK("midiOutCachePatches", o_cache, h_cache);
    HOOK("midiOutGetID", o_getid, h_getid);
    HOOK("midiStreamOpen", s_open, hs_open);
    HOOK("midiStreamOut", s_out, hs_out);
    HOOK("midiStreamRestart", s_restart, hs_restart);
    HOOK("midiStreamPause", s_pause, hs_pause);
    HOOK("midiStreamStop", s_stop, hs_stop);
    HOOK("midiStreamClose", s_close, hs_close);
    HOOK("midiStreamProperty", s_prop, hs_prop);
    HOOK("midiStreamPosition", s_pos, hs_pos);
    HOOK("mciSendCommandA", m_cmdA, hm_cmdA);
    HOOK("mciSendCommandW", m_cmdW, hm_cmdW);
    HOOK("mciSendStringA", m_strA, hm_strA);
    HOOK("mciSendStringW", m_strW, hm_strW);
}
