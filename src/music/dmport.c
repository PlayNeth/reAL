/* reDM: the synth port object (IDirectMusicPort, IDirectMusicPortDownload, IKsControl) and the sink object,
 * as an application meets them through an audiopath's port and sink stages and Performance::PChannelInfo.  The port
 * is the performance's one synth port ("Microsoft Synthesizer", dmusic's CDirectMusicSynthPort); what it reports is
 * what tests/porttest.c finds on Windows.  Downloads go through the performance, not the port. */
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

struct rm_port {
    IDirectMusicPort IDirectMusicPort_iface;
    IDirectMusicPortDownload IDirectMusicPortDownload_iface;
    IKsControl IKsControl_iface;
    LONG ref;
    dm_host *host;                      /* NULL once the performance closed down */
    DWORD groups;                       /* SetNumChannelGroups beyond what the PChannels use */
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
static ULONG port_release(rm_port *p)
{
    LONG r = InterlockedDecrement(&p->ref);
    if (!r) free(p);
    return r;
}

static DWORD port_groups(rm_port *p)
{
    if (!p->host) return p->groups;
    dm_host_lock(p->host);
    DWORD g = dm_perf_port_groups(dm_host_perf(p->host));
    dm_host_unlock(p->host);
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
    if (!p->host) return DMUS_E_NOT_INIT;
    /* the buffer's events, played now (reDM hands out no buffers of its own; an application's works) */
    IDirectMusicBuffer_ResetReadPtr(b);
    REFERENCE_TIME t; DWORD g, len; BYTE *d;
    while (IDirectMusicBuffer_GetNextEvent(b, &t, &g, &len, &d) == S_OK) dm_host_midi(p->host, d, len);
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
    if (pt(i)->host) dm_host_stats(pt(i)->host, &st);
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
    if (!p->host) return DMUS_E_NOT_INIT;
    uint32_t v = set ? *prio : 0;
    if (dm_host_priority(p->host, g, c, &v, set) < 0) return E_INVALIDARG;
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
    DWORD rate = p->host ? dm_host_rate(p->host) : 22050;
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
static HRESULT WINAPI dd_GetBuffer(IDirectMusicPortDownload *i, DWORD id, IDirectMusicDownload **d) { if (d) *d = NULL; return E_NOTIMPL; }
static HRESULT WINAPI dd_AllocateBuffer(IDirectMusicPortDownload *i, DWORD size, IDirectMusicDownload **d) { if (d) *d = NULL; return E_NOTIMPL; }
static HRESULT WINAPI dd_GetDLId(IDirectMusicPortDownload *i, DWORD *id, DWORD n) { return E_NOTIMPL; }
static HRESULT WINAPI dd_GetAppend(IDirectMusicPortDownload *i, DWORD *a)
{
    if (!a) return E_POINTER;
    *a = 2;                             /* the synth's sample padding */
    return S_OK;
}
static HRESULT WINAPI dd_Download(IDirectMusicPortDownload *i, IDirectMusicDownload *d) { return E_NOTIMPL; }
static HRESULT WINAPI dd_Unload(IDirectMusicPortDownload *i, IDirectMusicDownload *d) { return E_NOTIMPL; }
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
            if (p->host) dm_host_set_volume(p->host, *(LONG *)data);
            return S_OK;
        }
        if (IsEqualGUID(s, &GUID_DMUS_PROP_Effects)) {
            if (!data || dlen < 4) return E_INVALIDARG;
            if (p->host) dm_host_set_reverb(p->host, (*(DWORD *)data & DMUS_EFFECT_REVERB) != 0);
            return S_OK;
        }
        return E_UNKNOWN_PROPERTY;
    }
    if (!(prop->Flags & KSPROPERTY_TYPE_GET)) return E_UNKNOWN_PROPERTY;
    if (IsEqualGUID(s, &GUID_DMUS_PROP_GM_Hardware) || IsEqualGUID(s, &GUID_DMUS_PROP_GS_Hardware) ||
        IsEqualGUID(s, &GUID_DMUS_PROP_XG_Hardware) || IsEqualGUID(s, &GUID_DMUS_PROP_Effects)) v = 0;
    else if (IsEqualGUID(s, &GUID_DMUS_PROP_DLS1) || IsEqualGUID(s, &GUID_DMUS_PROP_DLS2)) v = 1;
    else if (IsEqualGUID(s, &GUID_DMUS_PROP_SampleMemorySize)) v = 0x7fffffff;
    else if (IsEqualGUID(s, &GUID_DMUS_PROP_SamplePlaybackRate)) v = p->host ? dm_host_rate(p->host) : 22050;
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
    dm_host *h = s->port->host;
    if (!h) return E_FAIL;
    *t = s->latency ? dm_host_latency(h) : dm_host_clock(h);
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
void rm_port_detach(rm_port *p) { if (p) p->host = NULL; }
void rm_port_release(rm_port *p) { if (p) port_release(p); }
HRESULT rm_port_query(rm_port *p, REFIID riid, void **out) { return port_qi(p, riid, out); }
HRESULT rm_port_sink(rm_port *p, REFIID riid, void **out) { return sink_new(p, 0, riid, out); }
BOOL rm_port_is(IDirectMusicPort *i, rm_port *p) { return i && i == &p->IDirectMusicPort_iface; }
