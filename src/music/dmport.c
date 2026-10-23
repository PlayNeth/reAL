/* reDM: the synth port object (IDirectMusicPort, IDirectMusicPortDownload, IKsControl) and the sink object,
 * as an application meets them through an audiopath's port and sink stages and Performance::PChannelInfo.  The port
 * is the performance's one synth port ("Microsoft Synthesizer", dmusic's CDirectMusicSynthPort); what it reports is
 * what tests/porttest.c finds on Windows.  The performance's own downloads go through it, not the port; an application
 * can download its own buffers through IDirectMusicPortDownload (Pro Evolution Soccer 6 does), and a collection's
 * instruments one at a time through DownloadInstrument (the DirectX 6/7 way). */
#include "../real.h"
#include "dmport.h"
#include <dmusicc.h>
#include <dmksctrl.h>
#include <string.h>
#include <math.h>

static const GUID s_IID_IKsControl = { 0x28f54685, 0x06fd, 0x11d2, { 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
/* the Microsoft Synthesizer's port GUID */
const GUID RM_GUID_SynthPort = { 0x58c2b4d0, 0x46e7, 0x11d1, { 0x89, 0xac, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29 } };

#define E_UNKNOWN_PROPERTY ((HRESULT)0x88781122)       /* DMUS_E_UNKNOWN_PROPERTY */
#define E_GET_UNSUPPORTED  ((HRESULT)0x88781124)       /* DMUS_E_GET_UNSUPPORTED */

/* a download buffer of the application's (IDirectMusicPortDownload::AllocateBuffer, dmusic's CDownloadBuffer) */
typedef struct rm_dlbuf {
    IDirectMusicDownload IDirectMusicDownload_iface;
    LONG ref;
    void *data;
    DWORD size;
} rm_dlbuf;
/* one the port has downloaded: its synth download, by the id its DMUS_DOWNLOADINFO names */
typedef struct rm_dlrec {
    struct rm_dlrec *next;
    rm_dlbuf *buf;                      /* a reference */
    void *handle;
    DWORD id;
} rm_dlrec;

/* IDirectMusic::CreateMusicBuffer's buffer (dmusic's CDirectMusicBuffer): DMUS_EVENTHEADERs (20 bytes: the size of the
 * data, the channel group, the offset from the start time, the flags) each followed by its data, each event 8-byte
 * aligned, kept in time order */
typedef struct rm_mbuf {
    IDirectMusicBuffer IDirectMusicBuffer_iface;
    LONG ref;
    SRWLOCK lock;
    BYTE *data;
    DWORD max, used, rd;
    REFERENCE_TIME start;
    GUID fmt;
} rm_mbuf;
#define EVH 20                          /* sizeof(DMUS_EVENTHEADER) */
/* the event at offset o, whole within the bytes used (0 when there is none: past the end, or cut off by SetUsedBytes);
 * its data size, offset from the start time and the next event's offset */
static int ev_at(const rm_mbuf *b, DWORD o, DWORD *cb, LONGLONG *delta, DWORD *next)
{
    if (o > b->used || b->used - o < EVH) return 0;
    DWORD c;
    memcpy(&c, b->data + o, 4);
    if (c > b->used - o - EVH) return 0;
    if (cb) *cb = c;
    if (delta) memcpy(delta, b->data + o + 8, 8);
    if (next) *next = o + ((EVH + c + 7) & ~7u);    /* (no overflow: c < used) */
    return 1;
}

struct rm_port {
    IDirectMusicPort IDirectMusicPort_iface;
    IDirectMusicPortDownload IDirectMusicPortDownload_iface;
    IKsControl IKsControl_iface;
    LONG ref;
    dm_host *host;                      /* NULL once the performance closed down (under the engine lock) */
    DWORD groups;                       /* SetNumChannelGroups beyond what the PChannels use; a CreatePort'ed one's all */
    DWORD sgroups;                      /* a CreatePort'ed one's synth: the groups it plays (dwChannelGroups, 2 unasked) */
    BOOL own;                           /* IDirectMusic::CreatePort's: a synth of its own (the host), freed with it */
    volatile LONG active;               /* Activate */
    WORD chans;                         /* GetFormat's channels */
    rm_dlrec *dls;                      /* the application's downloads (under the engine lock) */
    struct rm_dlins *ins;               /* its DownloadInstrument downloads, each a reference (engine lock) */
};

/* a DownloadInstrument download (dmusic's CDownloadedInstrument): an IUnknown the application unloads with
 * UnloadInstrument.  Downloading the same instrument object with the same patch and no key ranges again hands out
 * the same object and counts it; UnloadInstrument answers S_FALSE until the last. */
typedef struct rm_dlins {
    IDirectMusicDownloadedInstrument IDirectMusicDownloadedInstrument_iface;
    LONG ref;
    struct rm_dlins *next;
    void *handle;                       /* the synth's (dls_port_download); NULL once gone */
    LONG serial;                        /* the instrument object's */
    DWORD patch;
    int ranged;
    DWORD count;
} rm_dlins;

typedef struct {
    IUnknown IUnknown_iface;
    IReferenceClock IReferenceClock_iface;
    IKsControl IKsControl_iface;
    LONG ref;
    rm_port *port;
    int latency;                        /* a latency clock (GetLatencyClock) rather than the sink */
} rm_sink;

static rm_port *pt(IDirectMusicPort *i) { return impl_from(i, rm_port, IDirectMusicPort_iface); }
static rm_port *pdl(IDirectMusicPortDownload *i) { return impl_from(i, rm_port, IDirectMusicPortDownload_iface); }
static rm_port *pks(IKsControl *i) { return impl_from(i, rm_port, IKsControl_iface); }

static HRESULT port_qi(rm_port *p, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicPort)) *out = &p->IDirectMusicPort_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicPortDownload)) *out = &p->IDirectMusicPortDownload_iface;
    else if (IsEqualIID(riid, &s_IID_IKsControl)) *out = &p->IKsControl_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&p->ref);
    return S_OK;
}
static void dlbuf_release(rm_dlbuf *b);
/* the application's downloads let go of (their synth goes with the performance, or has gone) */
static void dlrecs_free(rm_dlrec *r)
{
    while (r) { rm_dlrec *n = r->next; dlbuf_release(r->buf); free(r); r = n; }
}
static ULONG dlins_release(rm_dlins *o);
static void dlins_free_list(rm_dlins *o)
{
    while (o) { rm_dlins *n = o->next; o->handle = NULL; dlins_release(o); o = n; }
}
static ULONG port_release(rm_port *p)
{
    LONG r = InterlockedDecrement(&p->ref);
    if (!r) {
        dm_host *h = p->own ? p->host : NULL;
        if (h) rm_port_detach(p);       /* (its downloads go with its synth) */
        dlrecs_free(p->dls); dlins_free_list(p->ins);
        free(p);
        dm_host_free(h);
    }
    return r;
}

/* the host, with the engine lock held (port_drop lets it go), or NULL (not held) once the performance is gone: the
 * performance frees its host only after rm_port_detach, which takes the lock, so a host seen under it stays */
static dm_host *port_hold(rm_port *p)
{
    dm_engine_lock();
    if (p->host) return p->host;
    dm_engine_unlock();
    return NULL;
}
static void port_drop(void) { dm_engine_unlock(); }

static DWORD port_groups(rm_port *p)
{
    if (p->own) return p->groups;
    dm_host *h = port_hold(p);
    if (!h) return p->groups;
    DWORD g = dm_perf_port_groups(dm_host_perf(h));
    port_drop();
    return g > p->groups ? g : p->groups;
}

/* ------------------------------------------------------------------ IDirectMusicPort */
static HRESULT WINAPI dp_QueryInterface(IDirectMusicPort *i, REFIID riid, void **out) { return port_qi(pt(i), riid, out); }
static ULONG WINAPI dp_AddRef(IDirectMusicPort *i) { return InterlockedIncrement(&pt(i)->ref); }
static ULONG WINAPI dp_Release(IDirectMusicPort *i) { return port_release(pt(i)); }
static rm_mbuf *mbuf_of(IDirectMusicBuffer *i);
static void mbuf_lock(rm_mbuf *b);
static void mbuf_unlock(rm_mbuf *b);
/* the buffer's events, each at its time (the buffer's start time plus its offset, in reference time; one already
 * past as soon as possible) on its channel group (0: all of them), as dmusic's synth port plays them.  Read before
 * the engine lock is taken: an application's own buffer object is not called with it held. */
static HRESULT WINAPI dp_PlayBuffer(IDirectMusicPort *i, LPDIRECTMUSICBUFFER b)
{
    rm_port *p = pt(i);
    if (!b) return E_POINTER;
    if (!p->active) return DMUS_E_SYNTHINACTIVE;
    typedef struct { REFERENCE_TIME t; DWORD g, len; } evh;
    BYTE *ev = NULL;
    size_t n = 0, cap = 0;
    rm_mbuf *mb = mbuf_of(b);
    if (mb) mbuf_lock(mb);
    else IDirectMusicBuffer_ResetReadPtr(b);
    DWORD rd = 0;
    for (;;) {
        evh e; BYTE *d = NULL;
        if (mb) {               /* (ours read without moving its read pointer) */
            DWORD nx; LONGLONG dt;
            if (!ev_at(mb, rd, &e.len, &dt, &nx)) break;
            memcpy(&e.g, mb->data + rd + 4, 4);
            e.t = mb->start + dt; d = mb->data + rd + EVH;
            rd = nx;
        } else if (IDirectMusicBuffer_GetNextEvent(b, &e.t, &e.g, &e.len, &d) != S_OK) break;
        if (!e.len || !d || e.len > 0x10000) continue;     /* (a message no synth takes: an application's own buffer) */
        if (n + sizeof(e) + e.len > cap) {
            size_t c = cap ? cap * 2 : 256;
            while (c < n + sizeof(e) + e.len) c *= 2;
            BYTE *x = realloc(ev, c);
            if (!x) { free(ev); if (mb) mbuf_unlock(mb); return E_OUTOFMEMORY; }
            ev = x; cap = c;
        }
        memcpy(ev + n, &e, sizeof(e)); memcpy(ev + n + sizeof(e), d, e.len);
        n += sizeof(e) + e.len;
    }
    if (mb) mbuf_unlock(mb);
    DWORD groups = port_groups(p);      /* (a group beyond the synth's plays nothing, as on dmusic's; 0 is all of them) */
    if (p->own && p->sgroups > groups) groups = p->sgroups;
    dm_host *h = port_hold(p);
    if (h) {
        for (size_t k = 0; k < n; ) {
            evh e;
            memcpy(&e, ev + k, sizeof(e));
            if (e.g <= groups) dm_host_midi_at(h, e.t, e.g, ev + k + sizeof(e), e.len);
            k += sizeof(e) + e.len;
        }
        port_drop();
    }
    free(ev);
    return h ? S_OK : DMUS_E_NOT_INIT;
}
static HRESULT WINAPI dp_SetReadNotificationHandle(IDirectMusicPort *i, HANDLE e) { return E_NOTIMPL; }
static HRESULT WINAPI dp_Read(IDirectMusicPort *i, LPDIRECTMUSICBUFFER b) { return b ? E_NOTIMPL : E_POINTER; }
/* ------------------------------------------------------------------ IDirectMusicDownloadedInstrument */
static IDirectMusicDownloadedInstrumentVtbl di_vtbl;
static rm_dlins *dli(IDirectMusicDownloadedInstrument *i) { return impl_from(i, rm_dlins, IDirectMusicDownloadedInstrument_iface); }
static HRESULT WINAPI di_QueryInterface(IDirectMusicDownloadedInstrument *i, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IDirectMusicDownloadedInstrument)) { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&dli(i)->ref);
    *out = i;
    return S_OK;
}
static ULONG dlins_release(rm_dlins *o)
{
    LONG r = InterlockedDecrement(&o->ref);
    if (!r) free(o);
    return r;
}
static ULONG WINAPI di_AddRef(IDirectMusicDownloadedInstrument *i) { return InterlockedIncrement(&dli(i)->ref); }
static ULONG WINAPI di_Release(IDirectMusicDownloadedInstrument *i) { return dlins_release(dli(i)); }
static IDirectMusicDownloadedInstrumentVtbl di_vtbl = { di_QueryInterface, di_AddRef, di_Release };
static rm_dlins *dlins_of(IDirectMusicDownloadedInstrument *i)
{
    return i && !IsBadReadPtr(i, sizeof(void *)) && i->lpVtbl == &di_vtbl ? dli(i) : NULL;
}

/* as dmusic: NULL arguments (ranges NULL with a count included) are E_POINTER; an instrument without an
 * articulation is refused with 0x88781129, as Microsoft's synth refuses it */
static HRESULT WINAPI dp_DownloadInstrument(IDirectMusicPort *i, IDirectMusicInstrument *in, IDirectMusicDownloadedInstrument **d,
                                            DMUS_NOTERANGE *r, DWORD n)
{
    rm_port *p = pt(i);
    if (n > 0x100000) return E_INVALIDARG;     /* (more ranges than keys: the sizes below cannot overflow) */
    if (!in || !d || IsBadWritePtr(d, sizeof(*d)) || (n && (!r || IsBadReadPtr(r, n * sizeof(*r))))) return E_POINTER;
    *d = NULL;
    dm_collection *c;
    uint32_t orig, patch;
    LONG serial;
    if (!rm_instrument_get(in, &c, &orig, &patch, &serial)) return E_INVALIDARG;
    uint32_t *rg = n ? malloc(n * 2 * sizeof(uint32_t)) : NULL;    /* (the application's, read before the lock) */
    rm_dlins *o = calloc(1, sizeof(*o));
    if (!o || (n && !rg)) { free(rg); free(o); dls_release(c); return E_OUTOFMEMORY; }
    for (DWORD k = 0; k < n; k++) { rg[2 * k] = r[k].dwLowNote; rg[2 * k + 1] = r[k].dwHighNote; }
    HRESULT hr = S_OK;
    dm_host *h = port_hold(p);
    if (!h) hr = DMUS_E_NOT_INIT;
    else {
        rm_dlins *x = NULL;
        if (!n) for (x = p->ins; x && (x->ranged || x->serial != serial || x->patch != patch); x = x->next) ;
        if (x) {
            x->count++;
            InterlockedIncrement(&x->ref);
            *d = &x->IDirectMusicDownloadedInstrument_iface;
        } else {
            int e = 0;
            o->handle = dm_host_ins_download(h, c, orig, patch, rg, (int)n, &e);
            if (!o->handle) hr = e < 0 ? (HRESULT)e : E_FAIL;
            else {
                o->IDirectMusicDownloadedInstrument_iface.lpVtbl = &di_vtbl;
                o->ref = 2;             /* the port's and the caller's */
                o->serial = serial; o->patch = patch; o->ranged = n != 0; o->count = 1;
                o->next = p->ins;
                p->ins = o;
                *d = &o->IDirectMusicDownloadedInstrument_iface;
                o = NULL;
            }
        }
        port_drop();
    }
    free(o); free(rg);
    dls_release(c);
    LOG_CALL("music: Port::DownloadInstrument(patch %06lx as %06lx, %lu ranges) -> %08lx %p", (unsigned long)orig,
             (unsigned long)patch, (unsigned long)n, (unsigned long)hr, (void *)*d);
    return hr;
}
/* S_FALSE while it was downloaded more often than unloaded, 0x88781120 (DMUS_E_NOT_DOWNLOADED_TO_PORT) once unloaded */
static HRESULT WINAPI dp_UnloadInstrument(IDirectMusicPort *i, IDirectMusicDownloadedInstrument *d)
{
    rm_port *p = pt(i);
    if (!d) return E_POINTER;
    rm_dlins *o = dlins_of(d);
    if (!o) return E_INVALIDARG;
    HRESULT hr = DMUS_E_NOT_DOWNLOADED_TO_PORT;
    rm_dlins *gone = NULL;
    dm_host *h = port_hold(p);
    if (h) {
        rm_dlins **pp = &p->ins;
        while (*pp && *pp != o) pp = &(*pp)->next;
        if (*pp) {
            if (--o->count) hr = S_FALSE;
            else {
                *pp = o->next;
                dm_host_ins_unload(h, o->handle);
                o->handle = NULL;
                gone = o;
                hr = S_OK;
            }
        }
        port_drop();
    }
    if (gone) dlins_release(gone);
    LOG_CALL("music: Port::UnloadInstrument(%p) -> %08lx", (void *)d, (unsigned long)hr);
    return hr;
}
static HRESULT sink_new(rm_port *p, int latency, REFIID riid, void **out);
static HRESULT WINAPI dp_GetLatencyClock(IDirectMusicPort *i, IReferenceClock **c)
{
    if (!c) return E_POINTER;
    return sink_new(pt(i), 1, &IID_IReferenceClock, (void **)c);
}
static HRESULT WINAPI dp_GetRunningStats(IDirectMusicPort *i, LPDMUS_SYNTHSTATS s)
{
    if (!s || IsBadWritePtr(s, sizeof(DWORD)) || s->dwSize < sizeof(DMUS_SYNTHSTATS)) return E_INVALIDARG;
    DWORD size = s->dwSize;
    memset(s, 0, sizeof(*s));
    s->dwSize = size;
    /* voices, total CPU, CPU per voice, lost notes, peak volume (dmsynth's 0x1f, 7748fcd4; no free memory) */
    s->dwValidStats = 0x1f;
    msynth_stats st;
    memset(&st, 0, sizeof(st));
    dm_host *h = port_hold(pt(i));
    if (h) { dm_host_stats(h, &st); port_drop(); }
    s->dwVoices = st.voices;
    s->dwTotalCPU = st.cpu * 10;
    s->dwCPUPerVoice = (st.voices ? st.cpu / st.voices : 0) * 10;
    s->dwLostNotes = st.lost;
    /* the peak in dB of full scale, truncated; -96 when silent */
    s->lPeakVolume = st.peak < 1 ? -96 : (LONG)(20.0 * log10((double)st.peak * (1.0 / 32768)));
    return S_OK;
}
static HRESULT WINAPI dp_Compact(IDirectMusicPort *i) { return E_NOTIMPL; }
static HRESULT WINAPI dp_GetCaps(IDirectMusicPort *i, LPDMUS_PORTCAPS c)
{
    if (!c || IsBadWritePtr(c, sizeof(*c)) || c->dwSize != sizeof(*c)) return E_INVALIDARG;
    memset(c, 0, sizeof(*c));
    c->dwSize = sizeof(*c);
    c->dwFlags = DMUS_PC_DLS | DMUS_PC_SOFTWARESYNTH | DMUS_PC_DIRECTSOUND | DMUS_PC_DLS2 | DMUS_PC_AUDIOPATH |
                 DMUS_PC_WAVE;           /* 0xe85 */
    c->guidPort = RM_GUID_SynthPort;
    c->dwClass = DMUS_PC_OUTPUTCLASS;
    c->dwType = DMUS_PORT_USER_MODE_SYNTH;
    c->dwMemorySize = DMUS_PC_SYSTEMMEMORY;
    c->dwMaxChannelGroups = 1000;
    c->dwMaxVoices = 1000;
    c->dwMaxAudioChannels = 2;
    c->dwEffectFlags = DMUS_EFFECT_REVERB;
    lstrcpyW(c->wszDescription, L"Microsoft Synthesizer");
    return S_OK;
}
static HRESULT WINAPI dp_DeviceIoControl(IDirectMusicPort *i, DWORD code, LPVOID in, DWORD nin, LPVOID out, DWORD nout,
                                         LPDWORD got, LPOVERLAPPED o) { return E_NOTIMPL; }
static HRESULT WINAPI dp_SetNumChannelGroups(IDirectMusicPort *i, DWORD n)
{
    rm_port *p = pt(i);
    if (n > 1000) return E_INVALIDARG;
    p->groups = n;
    return S_OK;
}
static HRESULT WINAPI dp_GetNumChannelGroups(IDirectMusicPort *i, LPDWORD n)
{
    if (!n) return E_POINTER;
    *n = port_groups(pt(i));
    return S_OK;
}
static HRESULT WINAPI dp_Activate(IDirectMusicPort *i, WINBOOL on) { InterlockedExchange(&pt(i)->active, on != 0); return S_OK; }
static HRESULT priority(rm_port *p, DWORD g, DWORD c, DWORD *prio, int set)
{
    if (!g || g > port_groups(p) || c > 15) return E_INVALIDARG;
    dm_host *h = port_hold(p);
    if (!h) return DMUS_E_NOT_INIT;
    uint32_t v = set ? *prio : 0;
    int r = dm_host_priority(h, g, c, &v, set);
    port_drop();
    if (r < 0) return E_INVALIDARG;
    if (!set) *prio = v;
    return S_OK;
}
static HRESULT WINAPI dp_SetChannelPriority(IDirectMusicPort *i, DWORD g, DWORD c, DWORD prio) { return priority(pt(i), g, c, &prio, 1); }
static HRESULT WINAPI dp_GetChannelPriority(IDirectMusicPort *i, DWORD g, DWORD c, LPDWORD prio)
{
    if (!prio) return E_POINTER;
    return priority(pt(i), g, c, prio, 0);
}
static HRESULT WINAPI dp_SetDirectSound(IDirectMusicPort *i, LPDIRECTSOUND ds, LPDIRECTSOUNDBUFFER b) { return S_OK; }
/* the synth's format as the port gives it (mono 16-bit at its rate) and a second of stereo as the buffer size */
static HRESULT WINAPI dp_GetFormat(IDirectMusicPort *i, LPWAVEFORMATEX wf, LPDWORD size, LPDWORD bufsize)
{
    rm_port *p = pt(i);
    if (!size) return E_POINTER;
    dm_host *h = port_hold(p);
    DWORD rate = 22050;
    if (h) { rate = dm_host_rate(h); port_drop(); }
    if (wf) {
        if (*size < sizeof(WAVEFORMATEX)) return E_INVALIDARG;
        WAVEFORMATEX f = { WAVE_FORMAT_PCM, p->chans, rate, rate * 2 * p->chans, (WORD)(2 * p->chans), 16, 0 };
        *wf = f;
    }
    *size = sizeof(WAVEFORMATEX);
    if (bufsize) *bufsize = rate * 4;
    return S_OK;
}
static IDirectMusicPortVtbl dp_vtbl = {
    dp_QueryInterface, dp_AddRef, dp_Release, dp_PlayBuffer, dp_SetReadNotificationHandle, dp_Read, dp_DownloadInstrument,
    dp_UnloadInstrument, dp_GetLatencyClock, dp_GetRunningStats, dp_Compact, dp_GetCaps, dp_DeviceIoControl,
    dp_SetNumChannelGroups, dp_GetNumChannelGroups, dp_Activate, dp_SetChannelPriority, dp_GetChannelPriority,
    dp_SetDirectSound, dp_GetFormat,
};

/* ------------------------------------------------------------------ IDirectMusicPortDownload */
static HRESULT WINAPI dd_QueryInterface(IDirectMusicPortDownload *i, REFIID riid, void **out) { return port_qi(pdl(i), riid, out); }
static ULONG WINAPI dd_AddRef(IDirectMusicPortDownload *i) { return InterlockedIncrement(&pdl(i)->ref); }
static ULONG WINAPI dd_Release(IDirectMusicPortDownload *i) { return port_release(pdl(i)); }
/* the buffers: memory the application fills with a DMUS_DOWNLOADINFO, its offset table and the data (IDirectMusicDownload::
 * GetBuffer), then hands to Download */
static IDirectMusicDownloadVtbl db_vtbl;
static rm_dlbuf *dbuf(IDirectMusicDownload *i) { return CONTAINING_RECORD(i, rm_dlbuf, IDirectMusicDownload_iface); }
static rm_dlbuf *dbuf_of(IDirectMusicDownload *i)
{
    return i && !IsBadReadPtr(i, sizeof(void *)) && i->lpVtbl == &db_vtbl ? dbuf(i) : NULL;
}
static ULONG WINAPI db_Release(IDirectMusicDownload *i);
static void dlbuf_release(rm_dlbuf *b) { if (b) db_Release(&b->IDirectMusicDownload_iface); }
static HRESULT WINAPI db_QueryInterface(IDirectMusicDownload *i, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicDownload)) {
        *out = i;
        InterlockedIncrement(&dbuf(i)->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI db_AddRef(IDirectMusicDownload *i) { return InterlockedIncrement(&dbuf(i)->ref); }
static ULONG WINAPI db_Release(IDirectMusicDownload *i)
{
    rm_dlbuf *b = dbuf(i);
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) { free(b->data); free(b); }
    return r;
}
static HRESULT WINAPI db_GetBuffer(IDirectMusicDownload *i, void **data, DWORD *size)
{
    rm_dlbuf *b = dbuf(i);
    if (!data || !size) return E_POINTER;
    *data = b->data;
    *size = b->size;
    return S_OK;
}
static IDirectMusicDownloadVtbl db_vtbl = { db_QueryInterface, db_AddRef, db_Release, db_GetBuffer };

/* the port's side (dmusic's CDirectMusicPortDownload): download ids from the port's counter, so they never meet the
 * ones its own collections and waves take; a buffer goes to the synth as it is (the synth copies what it keeps) */
static rm_dlrec **dlrec_find(rm_port *p, rm_dlbuf *b, DWORD id)
{
    rm_dlrec **r = &p->dls;
    while (*r && (b ? (*r)->buf != b : (*r)->id != id)) r = &(*r)->next;
    return r;
}
static HRESULT WINAPI dd_GetBuffer(IDirectMusicPortDownload *i, DWORD id, IDirectMusicDownload **d)
{
    rm_port *p = pdl(i);
    if (!d || IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    *d = NULL;
    dm_host *h = port_hold(p);
    if (!h) return DMUS_E_NOT_INIT;
    rm_dlrec *r = *dlrec_find(p, NULL, id);
    if (r) { InterlockedIncrement(&r->buf->ref); *d = &r->buf->IDirectMusicDownload_iface; }
    port_drop();
    return r ? S_OK : DMUS_E_NOT_DOWNLOADED_TO_PORT;
}
static HRESULT WINAPI dd_AllocateBuffer(IDirectMusicPortDownload *i, DWORD size, IDirectMusicDownload **d)
{
    if (!d || IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    *d = NULL;
    if (!size) return E_INVALIDARG;
    rm_dlbuf *b = calloc(1, sizeof(*b));
    if (b) b->data = calloc(1, size);
    if (!b || !b->data) { free(b); return E_OUTOFMEMORY; }
    b->IDirectMusicDownload_iface.lpVtbl = &db_vtbl;
    b->ref = 1;
    b->size = size;
    *d = &b->IDirectMusicDownload_iface;
    return S_OK;
}
static HRESULT WINAPI dd_GetDLId(IDirectMusicPortDownload *i, DWORD *id, DWORD n)
{
    rm_port *p = pdl(i);
    if (!id || IsBadWritePtr(id, sizeof(*id))) return E_POINTER;
    if (!n) return E_INVALIDARG;
    dm_host *h = port_hold(p);
    if (!h) return DMUS_E_NOT_INIT;
    *id = dm_host_reserve_dlids(h, n);
    port_drop();
    return S_OK;
}
static HRESULT WINAPI dd_GetAppend(IDirectMusicPortDownload *i, DWORD *a)
{
    if (!a) return E_POINTER;
    *a = 2;                             /* the synth's sample padding */
    return S_OK;
}
/* as dmusic: a buffer too small for its header is E_POINTER, a type the synth has none of DMUS_E_UNKNOWNDOWNLOAD
 * (0x8878112d), and an instrument with regions but no articulation is refused (0x88781129) as Microsoft's synth
 * refuses it */
static HRESULT WINAPI dd_Download(IDirectMusicPortDownload *i, IDirectMusicDownload *d)
{
    rm_port *p = pdl(i);
    rm_dlbuf *b = dbuf_of(d);
    if (!b || b->size < sizeof(DMUS_DOWNLOADINFO)) return E_POINTER;
    const DWORD *info = b->data;        /* DMUS_DOWNLOADINFO: type, id, offset table entries, size */
    DWORD type = info[0];
    if (type < DMUS_DOWNLOADINFO_INSTRUMENT || type > DMUS_DOWNLOADINFO_ONESHOTWAVE) return (HRESULT)0x8878112d;
    rm_dlrec *r = calloc(1, sizeof(*r));
    if (!r) return E_OUTOFMEMORY;
    dm_host *h = port_hold(p);
    if (!h) { free(r); return DMUS_E_NOT_INIT; }
    HRESULT hr = S_OK;
    int got;
    if (*dlrec_find(p, b, 0)) hr = DMUS_E_ALREADY_DOWNLOADED;
    else if ((got = dm_host_raw_download(h, b->data, b->size, &r->handle)) < 0)
        hr = type == DMUS_DOWNLOADINFO_INSTRUMENT || type == DMUS_DOWNLOADINFO_INSTRUMENT2 ? DMUS_E_BADINSTRUMENT : DMUS_E_BADWAVE;
    else if (got == 1) {                /* (regions without an articulation) */
        dm_host_raw_unload(h, r->handle);
        hr = (HRESULT)0x88781129;
    } else {
        r->buf = b;
        InterlockedIncrement(&b->ref);
        r->id = info[1];
        r->next = p->dls;
        p->dls = r;
        r = NULL;
    }
    port_drop();
    free(r);
    LOG_CALL("music: PortDownload::Download(type %lu, id %lu, %lu bytes) -> %08lx", (unsigned long)type,
             (unsigned long)info[1], (unsigned long)b->size, (unsigned long)hr);
    return hr;
}
/* (dmusic answers S_OK for a buffer it does not have downloaded, or has unloaded already) */
static HRESULT WINAPI dd_Unload(IDirectMusicPortDownload *i, IDirectMusicDownload *d)
{
    rm_port *p = pdl(i);
    rm_dlbuf *b = dbuf_of(d);
    if (!b) return E_POINTER;
    dm_host *h = port_hold(p);
    if (!h) return DMUS_E_NOT_INIT;
    rm_dlrec **pr = dlrec_find(p, b, 0), *r = *pr;
    if (r) { *pr = r->next; dm_host_raw_unload(h, r->handle); }
    port_drop();
    if (r) { dlbuf_release(r->buf); free(r); }
    return S_OK;
}
static IDirectMusicPortDownloadVtbl dd_vtbl = { dd_QueryInterface, dd_AddRef, dd_Release, dd_GetBuffer, dd_AllocateBuffer,
                                                dd_GetDLId, dd_GetAppend, dd_Download, dd_Unload };

/* ------------------------------------------------------------------ IKsControl: the synth's properties */
static HRESULT WINAPI dk_QueryInterface(IKsControl *i, REFIID riid, void **out) { return port_qi(pks(i), riid, out); }
static ULONG WINAPI dk_AddRef(IKsControl *i) { return InterlockedIncrement(&pks(i)->ref); }
static ULONG WINAPI dk_Release(IKsControl *i) { return port_release(pks(i)); }
static HRESULT WINAPI dk_KsProperty(IKsControl *i, PKSPROPERTY prop, ULONG plen, LPVOID data, ULONG dlen, ULONG *ret)
{
    rm_port *p = pks(i);
    if (!prop || plen < sizeof(KSPROPERTY)) return E_INVALIDARG;
    const GUID *s = &prop->Set;
    DWORD v;
    if (ret) *ret = 0;
    if (prop->Flags & KSPROPERTY_TYPE_SET) {
        if (IsEqualGUID(s, &GUID_DMUS_PROP_Volume)) {
            if (!data || dlen < 4) return E_INVALIDARG;
            dm_host *h = port_hold(p);
            if (h) { dm_host_set_volume(h, *(LONG *)data); port_drop(); }
            return S_OK;
        }
        if (IsEqualGUID(s, &GUID_DMUS_PROP_Effects)) {
            if (!data || dlen < 4) return E_INVALIDARG;
            dm_host *h = port_hold(p);
            if (h) { dm_host_set_reverb(h, (*(DWORD *)data & DMUS_EFFECT_REVERB) != 0); port_drop(); }
            return S_OK;
        }
        return E_UNKNOWN_PROPERTY;
    }
    if (!(prop->Flags & KSPROPERTY_TYPE_GET)) return E_UNKNOWN_PROPERTY;
    if (IsEqualGUID(s, &GUID_DMUS_PROP_GM_Hardware) || IsEqualGUID(s, &GUID_DMUS_PROP_GS_Hardware) ||
        IsEqualGUID(s, &GUID_DMUS_PROP_XG_Hardware) || IsEqualGUID(s, &GUID_DMUS_PROP_Effects)) v = 0;
    else if (IsEqualGUID(s, &GUID_DMUS_PROP_DLS1) || IsEqualGUID(s, &GUID_DMUS_PROP_DLS2)) v = 1;
    else if (IsEqualGUID(s, &GUID_DMUS_PROP_SampleMemorySize)) v = 0x7fffffff;
    else if (IsEqualGUID(s, &GUID_DMUS_PROP_SamplePlaybackRate)) {
        dm_host *h = port_hold(p);
        v = 22050;
        if (h) { v = dm_host_rate(h); port_drop(); }
    }
    else if (IsEqualGUID(s, &GUID_DMUS_PROP_Volume)) return E_GET_UNSUPPORTED;
    else return E_UNKNOWN_PROPERTY;
    if (!data || dlen < 4) return E_INVALIDARG;
    *(DWORD *)data = v;
    if (ret) *ret = 4;
    return S_OK;
}
static HRESULT WINAPI dk_KsMethod(IKsControl *i, PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG *ret) { return E_NOTIMPL; }
static HRESULT WINAPI dk_KsEvent(IKsControl *i, PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG *ret) { return E_NOTIMPL; }
static IKsControlVtbl dk_vtbl = { dk_QueryInterface, dk_AddRef, dk_Release, dk_KsProperty, dk_KsMethod, dk_KsEvent };

/* ------------------------------------------------------------------ the sink (and the latency clock) */
static rm_sink *su(IUnknown *i) { return impl_from(i, rm_sink, IUnknown_iface); }
static rm_sink *sc(IReferenceClock *i) { return impl_from(i, rm_sink, IReferenceClock_iface); }
static rm_sink *sk(IKsControl *i) { return impl_from(i, rm_sink, IKsControl_iface); }
static HRESULT sink_qi(rm_sink *s, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown)) *out = s->latency ? (void *)&s->IReferenceClock_iface : (void *)&s->IUnknown_iface;
    else if (IsEqualIID(riid, &IID_IReferenceClock)) *out = &s->IReferenceClock_iface;
    else if (!s->latency && IsEqualIID(riid, &s_IID_IKsControl)) *out = &s->IKsControl_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG sink_release(rm_sink *s)
{
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) { port_release(s->port); free(s); }
    return r;
}
static HRESULT WINAPI su_QueryInterface(IUnknown *i, REFIID riid, void **out) { return sink_qi(su(i), riid, out); }
static ULONG WINAPI su_AddRef(IUnknown *i) { return InterlockedIncrement(&su(i)->ref); }
static ULONG WINAPI su_Release(IUnknown *i) { return sink_release(su(i)); }
static IUnknownVtbl su_vtbl = { su_QueryInterface, su_AddRef, su_Release };

static HRESULT WINAPI sc_QueryInterface(IReferenceClock *i, REFIID riid, void **out) { return sink_qi(sc(i), riid, out); }
static ULONG WINAPI sc_AddRef(IReferenceClock *i) { return InterlockedIncrement(&sc(i)->ref); }
static ULONG WINAPI sc_Release(IReferenceClock *i) { return sink_release(sc(i)); }
static HRESULT WINAPI sc_GetTime(IReferenceClock *i, REFERENCE_TIME *t)
{
    rm_sink *s = sc(i);
    if (!t) return E_POINTER;
    dm_host *h = port_hold(s->port);
    if (!h) return E_FAIL;
    *t = s->latency ? dm_host_latency(h) : dm_host_clock(h);
    port_drop();
    return S_OK;
}
static HRESULT WINAPI sc_AdviseTime(IReferenceClock *i, REFERENCE_TIME b, REFERENCE_TIME o, HEVENT e, DWORD_PTR *c) { return E_NOTIMPL; }
static HRESULT WINAPI sc_AdvisePeriodic(IReferenceClock *i, REFERENCE_TIME s, REFERENCE_TIME p, HSEMAPHORE h, DWORD_PTR *c) { return E_NOTIMPL; }
static HRESULT WINAPI sc_Unadvise(IReferenceClock *i, DWORD_PTR c) { return E_NOTIMPL; }
static IReferenceClockVtbl sc_vtbl = { sc_QueryInterface, sc_AddRef, sc_Release, sc_GetTime, sc_AdviseTime, sc_AdvisePeriodic, sc_Unadvise };

static HRESULT WINAPI sk_QueryInterface(IKsControl *i, REFIID riid, void **out) { return sink_qi(sk(i), riid, out); }
static ULONG WINAPI sk_AddRef(IKsControl *i) { return InterlockedIncrement(&sk(i)->ref); }
static ULONG WINAPI sk_Release(IKsControl *i) { return sink_release(sk(i)); }
static HRESULT WINAPI sk_KsProperty(IKsControl *i, PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG *ret)
{
    if (ret) *ret = 0;
    return E_UNKNOWN_PROPERTY;
}
static IKsControlVtbl sk_vtbl = { sk_QueryInterface, sk_AddRef, sk_Release, sk_KsProperty, dk_KsMethod, dk_KsEvent };

static HRESULT sink_new(rm_port *p, int latency, REFIID riid, void **out)
{
    *out = NULL;
    rm_sink *s = calloc(1, sizeof(*s));
    if (!s) return E_OUTOFMEMORY;
    s->IUnknown_iface.lpVtbl = &su_vtbl;
    s->IReferenceClock_iface.lpVtbl = &sc_vtbl;
    s->IKsControl_iface.lpVtbl = &sk_vtbl;
    s->ref = 1; s->port = p; s->latency = latency;
    InterlockedIncrement(&p->ref);
    HRESULT hr = sink_qi(s, riid, out);
    sink_release(s);
    return hr;
}

/* ------------------------------------------------------------------ IDirectMusicBuffer */
static IDirectMusicBufferVtbl mb_vtbl;
static rm_mbuf *mb(IDirectMusicBuffer *i) { return CONTAINING_RECORD(i, rm_mbuf, IDirectMusicBuffer_iface); }
static rm_mbuf *mbuf_of(IDirectMusicBuffer *i) { return i && !IsBadReadPtr(i, sizeof(void *)) && i->lpVtbl == &mb_vtbl ? mb(i) : NULL; }
static void mbuf_lock(rm_mbuf *b) { AcquireSRWLockExclusive(&b->lock); }
static void mbuf_unlock(rm_mbuf *b) { ReleaseSRWLockExclusive(&b->lock); }
static HRESULT WINAPI mb_QueryInterface(IDirectMusicBuffer *i, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (!IsEqualIID(riid, &IID_IUnknown) && !IsEqualIID(riid, &IID_IDirectMusicBuffer)) { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&mb(i)->ref);
    *out = i;
    return S_OK;
}
static ULONG WINAPI mb_AddRef(IDirectMusicBuffer *i) { return InterlockedIncrement(&mb(i)->ref); }
static ULONG WINAPI mb_Release(IDirectMusicBuffer *i)
{
    rm_mbuf *b = mb(i);
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) { free(b->data); free(b); }
    return r;
}
static HRESULT WINAPI mb_Flush(IDirectMusicBuffer *i)
{
    rm_mbuf *b = mb(i);
    mbuf_lock(b);
    b->used = b->rd = 0;
    mbuf_unlock(b);
    return S_OK;
}
/* the last event's offset from the start time (0 when empty) */
static HRESULT WINAPI mb_TotalTime(IDirectMusicBuffer *i, LPREFERENCE_TIME t)
{
    rm_mbuf *b = mb(i);
    if (!t) return E_POINTER;
    LONGLONG last = 0, d;
    mbuf_lock(b);
    for (DWORD o = 0; ev_at(b, o, NULL, &d, &o); ) last = d;
    mbuf_unlock(b);
    *t = last;
    return S_OK;
}
/* an event put in time order (after those at the same time); one earlier than the start time moves the start time
 * to it.  The data is copied as given: the alignment padding keeps what the memory had. */
static HRESULT pack(rm_mbuf *b, REFERENCE_TIME rt, DWORD group, DWORD flags, DWORD cb, const BYTE *src)
{
    DWORD size = (EVH + cb + 7) & ~7u;
    mbuf_lock(b);
    if (size > b->max - b->used) { mbuf_unlock(b); return E_OUTOFMEMORY; }
    if (!b->used) b->start = rt;
    else if (rt < b->start) {
        LONGLONG by = b->start - rt, d;
        for (DWORD o = 0, nx; ev_at(b, o, NULL, &d, &nx); o = nx) { d += by; memcpy(b->data + o + 8, &d, 8); }
        b->start = rt;
    }
    LONGLONG delta = rt - b->start;
    DWORD at = 0, nx;
    LONGLONG d;
    while (ev_at(b, at, NULL, &d, &nx) && d <= delta) at = nx;
    if (at > b->used) at = b->used;
    memmove(b->data + at + size, b->data + at, b->used - at);
    memcpy(b->data + at, &cb, 4);
    memcpy(b->data + at + 4, &group, 4);
    memcpy(b->data + at + 8, &delta, 8);
    memcpy(b->data + at + 16, &flags, 4);
    memcpy(b->data + at + EVH, src, cb);
    b->used += size;
    mbuf_unlock(b);
    return S_OK;
}
/* a short message's length by its status byte, as dmusic takes it (0: not one it packs) */
static DWORD msg_len(BYTE st)
{
    static const BYTE sys[16] = { 0, 2, 3, 2, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1 };
    if (st < 0x80) return 0;
    if (st < 0xc0 || (st >= 0xe0 && st < 0xf0)) return 3;
    if (st < 0xe0) return 2;
    return sys[st & 15];
}
static HRESULT WINAPI mb_PackStructured(IDirectMusicBuffer *i, REFERENCE_TIME rt, DWORD group, DWORD msg)
{
    DWORD len = msg_len((BYTE)msg);
    if (!len) return DMUS_E_INVALID_EVENT;
    BYTE d[4] = { (BYTE)msg, (BYTE)(msg >> 8), (BYTE)(msg >> 16), (BYTE)(msg >> 24) };
    return pack(mb(i), rt, group, DMUS_EVENT_STRUCTURED, len, d);
}
static HRESULT WINAPI mb_PackUnstructured(IDirectMusicBuffer *i, REFERENCE_TIME rt, DWORD group, DWORD cb, LPBYTE data)
{
    if (!data || IsBadReadPtr(data, cb)) return E_POINTER;
    if (cb > 0x7fffffff - 32) return E_OUTOFMEMORY;
    return pack(mb(i), rt, group, 0, cb, data);
}
static HRESULT WINAPI mb_ResetReadPtr(IDirectMusicBuffer *i)
{
    rm_mbuf *b = mb(i);
    mbuf_lock(b);
    b->rd = 0;
    mbuf_unlock(b);
    return S_OK;
}
/* S_FALSE past the last; the data points into the buffer */
static HRESULT WINAPI mb_GetNextEvent(IDirectMusicBuffer *i, LPREFERENCE_TIME t, LPDWORD g, LPDWORD len, LPBYTE *data)
{
    rm_mbuf *b = mb(i);
    HRESULT hr = S_FALSE;
    mbuf_lock(b);
    DWORD cb, nx;
    LONGLONG d;
    if (ev_at(b, b->rd, &cb, &d, &nx)) {
        BYTE *h = b->data + b->rd;
        if (t) *t = b->start + d;
        if (g) memcpy(g, h + 4, 4);
        if (len) *len = cb;
        if (data) *data = h + EVH;
        b->rd = nx;
        hr = S_OK;
    }
    mbuf_unlock(b);
    return hr;
}
static HRESULT WINAPI mb_GetRawBufferPtr(IDirectMusicBuffer *i, LPBYTE *p)
{
    if (!p) return E_POINTER;
    *p = mb(i)->data;
    return S_OK;
}
/* DMUS_E_BUFFER_EMPTY, nothing written, when it has no events */
static HRESULT WINAPI mb_GetStartTime(IDirectMusicBuffer *i, LPREFERENCE_TIME t)
{
    rm_mbuf *b = mb(i);
    if (!t) return E_POINTER;
    HRESULT hr = DMUS_E_BUFFER_EMPTY;
    mbuf_lock(b);
    if (b->used) { *t = b->start; hr = S_OK; }
    mbuf_unlock(b);
    return hr;
}
static HRESULT WINAPI mb_GetUsedBytes(IDirectMusicBuffer *i, LPDWORD n)
{
    if (!n) return E_POINTER;
    *n = mb(i)->used;
    return S_OK;
}
static HRESULT WINAPI mb_GetMaxBytes(IDirectMusicBuffer *i, LPDWORD n)
{
    if (!n) return E_POINTER;
    *n = mb(i)->max;
    return S_OK;
}
static HRESULT WINAPI mb_GetBufferFormat(IDirectMusicBuffer *i, LPGUID g)
{
    if (!g) return E_POINTER;
    *g = mb(i)->fmt;
    return S_OK;
}
static HRESULT WINAPI mb_SetStartTime(IDirectMusicBuffer *i, REFERENCE_TIME t)
{
    rm_mbuf *b = mb(i);
    mbuf_lock(b);
    b->start = t;
    mbuf_unlock(b);
    return S_OK;
}
static HRESULT WINAPI mb_SetUsedBytes(IDirectMusicBuffer *i, DWORD n)
{
    rm_mbuf *b = mb(i);
    if (n > b->max) return DMUS_E_BUFFER_FULL;
    mbuf_lock(b);
    b->used = n;
    if (b->rd > n) b->rd = n;
    mbuf_unlock(b);
    return S_OK;
}
static IDirectMusicBufferVtbl mb_vtbl = { mb_QueryInterface, mb_AddRef, mb_Release, mb_Flush, mb_TotalTime,
    mb_PackStructured, mb_PackUnstructured, mb_ResetReadPtr, mb_GetNextEvent, mb_GetRawBufferPtr, mb_GetStartTime,
    mb_GetUsedBytes, mb_GetMaxBytes, mb_GetBufferFormat, mb_SetStartTime, mb_SetUsedBytes };

static const GUID s_KSDATAFORMAT_SUBTYPE_MIDI = { 0x1d262760, 0xe957, 0x11cf, { 0xa5, 0xd6, 0x28, 0xdb, 0x04, 0xc1, 0x00, 0x00 } };
/* as dmusic: the size rounded up to 8 bytes; GUID_NULL as the format is MIDI */
HRESULT rm_mbuf_create(const DMUS_BUFFERDESC *d, IDirectMusicBuffer **out, IUnknown *outer)
{
    if (!d || IsBadReadPtr(d, sizeof(DWORD)) || d->dwSize < sizeof(*d) || IsBadReadPtr(d, sizeof(*d))) return E_INVALIDARG;
    if (!out || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (d->cbBuffer > 0x7fffffff) return E_OUTOFMEMORY;
    rm_mbuf *b = calloc(1, sizeof(*b));
    DWORD max = (d->cbBuffer + 7) & ~7u;
    if (b) b->data = calloc(1, max ? max : 8);
    if (!b || !b->data) { free(b); return E_OUTOFMEMORY; }
    b->IDirectMusicBuffer_iface.lpVtbl = &mb_vtbl;
    b->ref = 1;
    InitializeSRWLock(&b->lock);
    b->max = max;
    static const GUID null_guid;
    b->fmt = IsEqualGUID(&d->guidBufferFormat, &null_guid) ? s_KSDATAFORMAT_SUBTYPE_MIDI : d->guidBufferFormat;
    *out = &b->IDirectMusicBuffer_iface;
    return S_OK;
}

/* ------------------------------------------------------------------ IDirectMusic::CreatePort */
/* the port's parameters as dmusic and its synth settle them (tests/dx6test.c): each one given and out of range is
 * brought into it and makes the answer S_FALSE, except that dwFeatures given makes it S_OK whatever came before; the
 * port keeps the channel groups asked for, the synth at most 1000 */
static HRESULT port_params(DMUS_PORTPARAMS8 *pp, DWORD *groups)
{
    DWORD v = pp->dwValidParams;
    BOOL part = FALSE, dx8 = pp->dwSize >= sizeof(DMUS_PORTPARAMS8);
    if (v & DMUS_PORTPARAMS_VOICES) {
        if (pp->dwVoices < 1) { pp->dwVoices = 1; part = TRUE; }
        else if (pp->dwVoices > 1000) { pp->dwVoices = 1000; part = TRUE; }
    } else pp->dwVoices = 32;
    if (v & DMUS_PORTPARAMS_CHANNELGROUPS) {
        *groups = pp->dwChannelGroups;
        if (pp->dwChannelGroups < 1) { pp->dwChannelGroups = 1; part = TRUE; }
        else if (pp->dwChannelGroups > 1000) { pp->dwChannelGroups = 1000; part = TRUE; }
    } else { *groups = 1; pp->dwChannelGroups = 2; }
    if (v & DMUS_PORTPARAMS_AUDIOCHANNELS) {
        if (pp->dwAudioChannels < 1) { pp->dwAudioChannels = 1; part = TRUE; }
        else if (pp->dwAudioChannels > 2) { pp->dwAudioChannels = 2; part = TRUE; }
    } else pp->dwAudioChannels = 2;
    if (v & DMUS_PORTPARAMS_SAMPLERATE) {
        if (pp->dwSampleRate < 11025) { pp->dwSampleRate = 11025; part = TRUE; }
        else if (pp->dwSampleRate > 96000) { pp->dwSampleRate = 96000; part = TRUE; }
    } else pp->dwSampleRate = 22050;
    if (v & DMUS_PORTPARAMS_EFFECTS) {
        if (pp->dwEffectFlags & ~DMUS_EFFECT_REVERB) { pp->dwEffectFlags &= DMUS_EFFECT_REVERB; part = TRUE; }
    } else pp->dwEffectFlags = DMUS_EFFECT_REVERB;
    if (v & DMUS_PORTPARAMS_SHARE) {
        if (pp->fShare) { pp->fShare = FALSE; part = TRUE; }
    } else pp->fShare = FALSE;
    if (dx8) {
        if (v & DMUS_PORTPARAMS_FEATURES) part = FALSE;
        else pp->dwFeatures = 0;
    }
    pp->dwValidParams = DMUS_PORTPARAMS_VOICES | DMUS_PORTPARAMS_CHANNELGROUPS | DMUS_PORTPARAMS_AUDIOCHANNELS |
                        DMUS_PORTPARAMS_SAMPLERATE | DMUS_PORTPARAMS_EFFECTS | DMUS_PORTPARAMS_SHARE |
                        (dx8 ? DMUS_PORTPARAMS_FEATURES : 0);
    return part ? S_FALSE : S_OK;
}
/* a synth port of its own, as each of dmusic's CreatePort is: a host (its synth and sink) that lives as long as the
 * port; a performance plays through it once AddPort'ed.  Created inactive (PlayBuffer wants Activate). */
HRESULT rm_port_create(REFCLSID cls, DMUS_PORTPARAMS *params, IDirectMusicPort **out, IUnknown *outer)
{
    static const GUID null_guid;
    if (!params || IsBadReadPtr(params, sizeof(DWORD))) return E_INVALIDARG;
    if (!out || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    *out = NULL;
    if (params->dwSize < sizeof(DMUS_PORTPARAMS7) || IsBadWritePtr(params, sizeof(DMUS_PORTPARAMS7))) return E_INVALIDARG;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!cls || (!IsEqualGUID(cls, &null_guid) && !IsEqualGUID(cls, &RM_GUID_SynthPort))) return E_NOINTERFACE;
    DMUS_PORTPARAMS8 pp;
    memset(&pp, 0, sizeof(pp));
    memcpy(&pp, params, params->dwSize < sizeof(pp) ? params->dwSize : sizeof(pp));
    DWORD groups;
    HRESULT hr = port_params(&pp, &groups);
    memcpy(params, &pp, params->dwSize < sizeof(pp) ? params->dwSize : sizeof(pp));
    DWORD sgroups = pp.dwChannelGroups < 64 ? 64 : pp.dwChannelGroups;
    dm_host *h = dm_host_new_ex(pp.dwSampleRate, pp.dwVoices, sgroups, (int)pp.dwAudioChannels);
    if (!h) return E_OUTOFMEMORY;
    if (!(pp.dwEffectFlags & DMUS_EFFECT_REVERB)) dm_host_set_reverb(h, 0);
    rm_port *p = rm_port_new(h);
    if (!p) { dm_host_free(h); return E_OUTOFMEMORY; }
    p->own = TRUE;
    p->active = FALSE;
    p->groups = groups;
    p->sgroups = pp.dwChannelGroups;
    p->chans = (WORD)pp.dwAudioChannels;
    *out = &p->IDirectMusicPort_iface;
    LOG_INFO("music: IDirectMusic::CreatePort: %lu Hz, %lu voices, %lu channel groups, %s -> %08lx", (unsigned long)pp.dwSampleRate,
             (unsigned long)pp.dwVoices, (unsigned long)groups, pp.dwEffectFlags ? "reverb" : "no reverb", (unsigned long)hr);
    return hr;
}

/* ------------------------------------------------------------------ for dmcom.c */
rm_port *rm_port_new(dm_host *h)
{
    rm_port *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->IDirectMusicPort_iface.lpVtbl = &dp_vtbl;
    p->IDirectMusicPortDownload_iface.lpVtbl = &dd_vtbl;
    p->IKsControl_iface.lpVtbl = &dk_vtbl;
    p->ref = 1;
    p->host = h;
    p->active = TRUE;
    p->chans = 1;
    return p;
}
void rm_port_detach(rm_port *p)
{
    if (!p) return;
    dm_engine_lock();
    p->host = NULL;
    rm_dlrec *r = p->dls;               /* (the synth they were in goes with the host) */
    p->dls = NULL;
    rm_dlins *in = p->ins;
    p->ins = NULL;
    dm_engine_unlock();
    dlrecs_free(r);
    dlins_free_list(in);
}
void rm_port_release(rm_port *p) { if (p) port_release(p); }
HRESULT rm_port_query(rm_port *p, REFIID riid, void **out) { return port_qi(p, riid, out); }
HRESULT rm_port_sink(rm_port *p, REFIID riid, void **out) { return sink_new(p, 0, riid, out); }
BOOL rm_port_is(IDirectMusicPort *i, rm_port *p) { return i && i == &p->IDirectMusicPort_iface; }
rm_port *rm_port_of(IDirectMusicPort *i) { return i && !IsBadReadPtr(i, sizeof(void *)) && i->lpVtbl == &dp_vtbl ? pt(i) : NULL; }
dm_host *rm_port_own_host(rm_port *p) { return p->own ? p->host : NULL; }
DWORD rm_port_groups(rm_port *p) { return port_groups(p); }
