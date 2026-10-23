/* reDM: the synth port object (IDirectMusicPort, IDirectMusicPortDownload, IKsControl) and the sink object,
 * as an application meets them through an audiopath's port and sink stages and Performance::PChannelInfo.  The port
 * is the performance's one synth port ("Microsoft Synthesizer", dmusic's CDirectMusicSynthPort); what it reports is
 * what tests/porttest.c finds on Windows.  The performance's own downloads go through it, not the port; an application
 * can download its own buffers through IDirectMusicPortDownload (Pro Evolution Soccer 6 does). */
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

struct rm_port {
    IDirectMusicPort IDirectMusicPort_iface;
    IDirectMusicPortDownload IDirectMusicPortDownload_iface;
    IKsControl IKsControl_iface;
    LONG ref;
    dm_host *host;                      /* NULL once the performance closed down (under the engine lock) */
    DWORD groups;                       /* SetNumChannelGroups beyond what the PChannels use */
    rm_dlrec *dls;                      /* the application's downloads (under the engine lock) */
};

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
static ULONG port_release(rm_port *p)
{
    LONG r = InterlockedDecrement(&p->ref);
    if (!r) { dlrecs_free(p->dls); free(p); }
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
static HRESULT WINAPI dp_PlayBuffer(IDirectMusicPort *i, LPDIRECTMUSICBUFFER b)
{
    rm_port *p = pt(i);
    if (!b) return E_POINTER;
    dm_host *h = port_hold(p);
    if (!h) return DMUS_E_NOT_INIT;
    port_drop();
    /* the buffer's events, played now (reDM hands out no buffers of its own; an application's works).  Read first:
     * the buffer is the application's object, not called with the engine lock held. */
    IDirectMusicBuffer_ResetReadPtr(b);
    REFERENCE_TIME t; DWORD g, len; BYTE *d;
    BYTE *ev = NULL;
    size_t n = 0, cap = 0;
    while (IDirectMusicBuffer_GetNextEvent(b, &t, &g, &len, &d) == S_OK) {
        if (!len || !d) continue;
        if (n + sizeof(DWORD) + len > cap) {
            size_t c = cap ? cap * 2 : 256;
            while (c < n + sizeof(DWORD) + len) c *= 2;
            BYTE *x = realloc(ev, c);
            if (!x) { free(ev); return E_OUTOFMEMORY; }
            ev = x; cap = c;
        }
        memcpy(ev + n, &len, sizeof(DWORD)); memcpy(ev + n + sizeof(DWORD), d, len);
        n += sizeof(DWORD) + len;
    }
    if ((h = port_hold(p))) {
        for (size_t k = 0; k < n; ) {
            DWORD l;
            memcpy(&l, ev + k, sizeof(DWORD));
            dm_host_midi(h, ev + k + sizeof(DWORD), l);
            k += sizeof(DWORD) + l;
        }
        port_drop();
    }
    free(ev);
    return S_OK;
}
static HRESULT WINAPI dp_SetReadNotificationHandle(IDirectMusicPort *i, HANDLE e) { return E_NOTIMPL; }
static HRESULT WINAPI dp_Read(IDirectMusicPort *i, LPDIRECTMUSICBUFFER b) { return b ? E_NOTIMPL : E_POINTER; }
static HRESULT WINAPI dp_DownloadInstrument(IDirectMusicPort *i, IDirectMusicInstrument *in, IDirectMusicDownloadedInstrument **d,
                                            DMUS_NOTERANGE *r, DWORD n)
{
    if (!in || !d) return E_POINTER;
    LOG_INFO("music: Port::DownloadInstrument not implemented (the performance's DownloadInstrument is)");
    return E_NOTIMPL;
}
static HRESULT WINAPI dp_UnloadInstrument(IDirectMusicPort *i, IDirectMusicDownloadedInstrument *d) { return d ? E_NOTIMPL : E_POINTER; }
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
static HRESULT WINAPI dp_Activate(IDirectMusicPort *i, WINBOOL on) { return S_OK; }
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
        WAVEFORMATEX f = { WAVE_FORMAT_PCM, 1, rate, rate * 2, 2, 16, 0 };
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
    return p;
}
void rm_port_detach(rm_port *p)
{
    if (!p) return;
    dm_engine_lock();
    p->host = NULL;
    rm_dlrec *r = p->dls;               /* (the synth they were in goes with the host) */
    p->dls = NULL;
    dm_engine_unlock();
    dlrecs_free(r);
}
void rm_port_release(rm_port *p) { if (p) port_release(p); }
HRESULT rm_port_query(rm_port *p, REFIID riid, void **out) { return port_qi(p, riid, out); }
HRESULT rm_port_sink(rm_port *p, REFIID riid, void **out) { return sink_new(p, 0, riid, out); }
BOOL rm_port_is(IDirectMusicPort *i, rm_port *p) { return i && i == &p->IDirectMusicPort_iface; }
