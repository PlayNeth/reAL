/* reAL-Music: the DirectMusic COM objects a game creates (dmloader/dmime/dmusic classes), backed by the ported
 * performance engine.  Covers what DirectMusic games built on segments use: the loader (files, memory, streams, the
 * default GM collection), the performance (Init/InitAudio, standard audiopaths, PlaySegment(Ex), Stop(Ex), IsPlaying,
 * time conversion, global parameters), segments, segment states and audiopaths (Activate, SetVolume).  Ports, tools,
 * styles, scripts and notifications are not implemented. */
#include "../real.h"
#include <dmusici.h>
#include <string.h>
#include <stdio.h>
#include "dmhost.h"
#include "dmcom.h"

/* methods reAL-Music does not implement: logged, so a game that relies on one shows up in reAL.log */
static HRESULT rm_notimpl(const char *fn)
{
    LOG_INFO("music: %s not implemented", fn);
    return E_NOTIMPL;
}
static HRESULT rm_noiface(const char *fn, REFIID riid, void **out)
{
    char b[64];
    LOG_INFO("music: %s: no interface %s", fn, rl_guid_str(riid, b));
    *out = NULL;
    return E_NOINTERFACE;
}

/* ------------------------------------------------------------------ objects */
typedef struct rm_loader rm_loader;
typedef struct rm_perf rm_perf;

typedef struct {
    IDirectMusicSegment8 IDirectMusicSegment8_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dm_segment *seg;
    DMUS_OBJECTDESC desc;
    rm_loader *loader;                      /* for references while loading from a stream (weak) */
} rm_segment;

typedef struct {
    IDirectMusicSegmentState8 IDirectMusicSegmentState8_iface;
    LONG ref;
    dm_segstate *ss;
    rm_segment *seg;
    rm_perf *perf;
} rm_state;

typedef struct {
    IDirectMusicAudioPath IDirectMusicAudioPath_iface;
    LONG ref;
    dm_audiopath *ap;
    rm_perf *perf;
} rm_path;

struct rm_perf {
    IDirectMusicPerformance8 IDirectMusicPerformance8_iface;
    LONG ref;
    dm_host *host;
    rm_path *defpath;
    IDirectMusic8 *dm;
    BOOL closed;
    HANDLE notify;
};

typedef struct {
    IDirectMusicCollection IDirectMusicCollection_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    LONG ref;
    dm_collection *c;
    DMUS_OBJECTDESC desc;
} rm_collection;

typedef struct { IDirectMusic8 IDirectMusic8_iface; LONG ref; } rm_dmusic;
typedef struct { IReferenceClock IReferenceClock_iface; LONG ref; } rm_clock;

typedef struct {                            /* the loader's object cache */
    WCHAR path[MAX_PATH];
    GUID guid;
    BOOL hasguid;
    IUnknown *obj;                          /* rm_segment or rm_collection */
} cache_entry;

struct rm_loader {
    IDirectMusicLoader8 IDirectMusicLoader8_iface;
    LONG ref;
    WCHAR dir[MAX_PATH];
    BOOL cache;
    cache_entry *ce; int nce;
};

static const GUID RM_GUID_DefaultGMCollection = { 0xf17e8673, 0xc3b4, 0x11d1, { 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };

static char *guidstr(const GUID *g, char b[64]) { return (char *)rl_guid_str(g, b); }

static void w2a(char *out, const WCHAR *in, int n)
{
    if (!WideCharToMultiByte(CP_ACP, 0, in, -1, out, n, NULL, NULL)) out[0] = 0;
}

static BOOL read_whole_file(const WCHAR *path, BYTE **data, DWORD *size)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD sz = GetFileSize(f, NULL), got = 0;
    BYTE *b = malloc(sz ? sz : 1);
    BOOL ok = b && ReadFile(f, b, sz, &got, NULL) && got == sz;
    CloseHandle(f);
    if (!ok) { free(b); return FALSE; }
    *data = b; *size = sz;
    return TRUE;
}

static BOOL read_stream(IStream *s, BYTE **data, DWORD *size)
{
    STATSTG st;
    LARGE_INTEGER zero = { { 0, 0 } };
    ULARGE_INTEGER pos;
    if (FAILED(IStream_Seek(s, zero, STREAM_SEEK_CUR, &pos))) return FALSE;
    if (FAILED(IStream_Stat(s, &st, STATFLAG_NONAME))) return FALSE;
    DWORD sz = (DWORD)(st.cbSize.QuadPart - pos.QuadPart), got = 0;
    BYTE *b = malloc(sz ? sz : 1);
    if (!b || FAILED(IStream_Read(s, b, sz, &got))) { free(b); return FALSE; }
    *data = b; *size = got;
    return TRUE;
}

/* ------------------------------------------------------------------ the master clock */
static rm_perf *s_clockperf;
static HRESULT WINAPI ck_qi(IReferenceClock *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IReferenceClock)) { *out = i; IReferenceClock_AddRef(i); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI ck_addref(IReferenceClock *i) { return InterlockedIncrement(&impl_from(i, rm_clock, IReferenceClock_iface)->ref); }
static ULONG WINAPI ck_release(IReferenceClock *i)
{
    rm_clock *c = impl_from(i, rm_clock, IReferenceClock_iface);
    LONG r = InterlockedDecrement(&c->ref);
    if (!r) free(c);
    return r;
}
static HRESULT WINAPI ck_gettime(IReferenceClock *i, REFERENCE_TIME *t)
{
    if (!t) return E_POINTER;
    rm_perf *p = s_clockperf;
    if (p && p->host) { dm_host_lock(p->host); *t = dm_host_clock(p->host); dm_host_unlock(p->host); }
    else { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); *t = (REFERENCE_TIME)((double)c.QuadPart * 1e7 / (double)f.QuadPart); }
    return S_OK;
}
static HRESULT WINAPI ck_advise(IReferenceClock *i, REFERENCE_TIME a, REFERENCE_TIME b, HEVENT e, DWORD_PTR *c) { return rm_notimpl(__func__); }
static HRESULT WINAPI ck_advper(IReferenceClock *i, REFERENCE_TIME a, REFERENCE_TIME b, HSEMAPHORE s, DWORD_PTR *c) { return rm_notimpl(__func__); }
static HRESULT WINAPI ck_unadvise(IReferenceClock *i, DWORD_PTR c) { return rm_notimpl(__func__); }
static IReferenceClockVtbl ck_vtbl = { ck_qi, ck_addref, ck_release, ck_gettime, ck_advise, ck_advper, ck_unadvise };

static IReferenceClock *new_clock(void)
{
    rm_clock *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->IReferenceClock_iface.lpVtbl = &ck_vtbl;
    c->ref = 1;
    return &c->IReferenceClock_iface;
}

/* ------------------------------------------------------------------ IDirectMusic8 (what Init/InitAudio hand out) */
static rm_dmusic *dmu(IDirectMusic8 *i) { return impl_from(i, rm_dmusic, IDirectMusic8_iface); }
static HRESULT WINAPI dmu_qi(IDirectMusic8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusic) || IsEqualIID(riid, &IID_IDirectMusic2) ||
        IsEqualIID(riid, &IID_IDirectMusic8)) { *out = i; IDirectMusic8_AddRef(i); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI dmu_addref(IDirectMusic8 *i) { return InterlockedIncrement(&dmu(i)->ref); }
static ULONG WINAPI dmu_release(IDirectMusic8 *i) { LONG r = InterlockedDecrement(&dmu(i)->ref); if (!r) free(dmu(i)); return r; }
static HRESULT WINAPI dmu_enumport(IDirectMusic8 *i, DWORD n, LPDMUS_PORTCAPS c)
{
    if (n || !c) return S_FALSE;
    DWORD sz = c->dwSize;
    memset(c, 0, sz);
    c->dwSize = sz;
    c->dwFlags = DMUS_PC_DLS | DMUS_PC_DLS2 | DMUS_PC_SOFTWARESYNTH | DMUS_PC_DIRECTSOUND | DMUS_PC_AUDIOPATH | DMUS_PC_WAVE;
    c->guidPort = CLSID_DirectMusicSynth;
    c->dwClass = DMUS_PC_OUTPUTCLASS;
    c->dwType = DMUS_PORT_USER_MODE_SYNTH;
    c->dwMemorySize = DMUS_PC_SYSTEMMEMORY;
    c->dwMaxChannelGroups = 1000; c->dwMaxVoices = 1000; c->dwMaxAudioChannels = 2;
    c->dwEffectFlags = DMUS_EFFECT_REVERB;
    lstrcpyW(c->wszDescription, L"reAL-Music Synthesizer");
    return S_OK;
}
static HRESULT WINAPI dmu_createbuffer(IDirectMusic8 *i, LPDMUS_BUFFERDESC d, LPDIRECTMUSICBUFFER *b, LPUNKNOWN o) { return rm_notimpl(__func__); }
static HRESULT WINAPI dmu_createport(IDirectMusic8 *i, REFCLSID c, LPDMUS_PORTPARAMS p, LPDIRECTMUSICPORT *port, LPUNKNOWN o)
{
    LOG_INFO("music: IDirectMusic::CreatePort is not supported");
    if (port) *port = NULL;
    return rm_notimpl(__func__);
}
static HRESULT WINAPI dmu_enumclock(IDirectMusic8 *i, DWORD n, LPDMUS_CLOCKINFO c) { return n ? S_FALSE : E_NOTIMPL; }
static HRESULT WINAPI dmu_getclock(IDirectMusic8 *i, LPGUID g, IReferenceClock **c)
{
    if (g) memset(g, 0, sizeof(*g));
    if (c && !(*c = new_clock())) return E_OUTOFMEMORY;
    return S_OK;
}
static HRESULT WINAPI dmu_setclock(IDirectMusic8 *i, REFGUID g) { return S_OK; }
static HRESULT WINAPI dmu_activate(IDirectMusic8 *i, BOOL on) { return S_OK; }
static HRESULT WINAPI dmu_defport(IDirectMusic8 *i, LPGUID g) { if (!g) return E_POINTER; *g = CLSID_DirectMusicSynth; return S_OK; }
static HRESULT WINAPI dmu_setds(IDirectMusic8 *i, LPDIRECTSOUND ds, HWND w) { return S_OK; }
static HRESULT WINAPI dmu_setext(IDirectMusic8 *i, IReferenceClock *c) { return S_OK; }
static IDirectMusic8Vtbl dmu_vtbl = { dmu_qi, dmu_addref, dmu_release, dmu_enumport, dmu_createbuffer, dmu_createport,
    dmu_enumclock, dmu_getclock, dmu_setclock, dmu_activate, dmu_defport, dmu_setds, dmu_setext };

static IDirectMusic8 *new_dmusic(void)
{
    rm_dmusic *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->IDirectMusic8_iface.lpVtbl = &dmu_vtbl;
    d->ref = 1;
    return &d->IDirectMusic8_iface;
}

/* ------------------------------------------------------------------ collections */
static rm_collection *col(IDirectMusicCollection *i) { return impl_from(i, rm_collection, IDirectMusicCollection_iface); }
static HRESULT WINAPI col_qi(IDirectMusicCollection *i, REFIID riid, void **out)
{
    rm_collection *c = col(i);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicCollection)) *out = &c->IDirectMusicCollection_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &c->IDirectMusicObject_iface;
    else { return rm_noiface(__func__, riid, out); }
    InterlockedIncrement(&c->ref);
    return S_OK;
}
static ULONG WINAPI col_addref(IDirectMusicCollection *i) { return InterlockedIncrement(&col(i)->ref); }
static ULONG WINAPI col_release(IDirectMusicCollection *i)
{
    LONG r = InterlockedDecrement(&col(i)->ref);
    if (!r) free(col(i));       /* the collection data stays in the process-wide cache */
    return r;
}
static HRESULT WINAPI col_getinst(IDirectMusicCollection *i, DWORD patch, IDirectMusicInstrument **out)
{
    if (out) *out = NULL;
    return dls_find(col(i)->c, patch) < 0 ? DMUS_E_NOT_FOUND : E_NOTIMPL;
}
static HRESULT WINAPI col_enum(IDirectMusicCollection *i, DWORD n, DWORD *patch, LPWSTR name, DWORD len)
{
    const char *nm;
    uint32_t p;
    if (dls_enum(col(i)->c, (int)n, &p, &nm) < 0) return S_FALSE;
    if (patch) *patch = p;
    if (name && len) MultiByteToWideChar(CP_ACP, 0, nm, -1, name, (int)len);
    return S_OK;
}
static IDirectMusicCollectionVtbl col_vtbl = { col_qi, col_addref, col_release, col_getinst, col_enum };

static rm_collection *colo(IDirectMusicObject *i) { return impl_from(i, rm_collection, IDirectMusicObject_iface); }
static HRESULT WINAPI colo_qi(IDirectMusicObject *i, REFIID r, void **o) { return col_qi(&colo(i)->IDirectMusicCollection_iface, r, o); }
static ULONG WINAPI colo_addref(IDirectMusicObject *i) { return col_addref(&colo(i)->IDirectMusicCollection_iface); }
static ULONG WINAPI colo_release(IDirectMusicObject *i) { return col_release(&colo(i)->IDirectMusicCollection_iface); }
static HRESULT WINAPI colo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { if (!d) return E_POINTER; DWORD sz = d->dwSize; memcpy(d, &colo(i)->desc, sz < sizeof(*d) ? sz : sizeof(*d)); d->dwSize = sz; return S_OK; }
static HRESULT WINAPI colo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return S_OK; }
static HRESULT WINAPI colo_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d) { return rm_notimpl(__func__); }
static IDirectMusicObjectVtbl colo_vtbl = { colo_qi, colo_addref, colo_release, colo_getdesc, colo_setdesc, colo_parse };

static rm_collection *new_collection(dm_collection *c)
{
    rm_collection *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicCollection_iface.lpVtbl = &col_vtbl;
    o->IDirectMusicObject_iface.lpVtbl = &colo_vtbl;
    o->ref = 1;
    o->c = c;
    o->desc.dwSize = sizeof(o->desc);
    o->desc.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_LOADED;
    o->desc.guidClass = CLSID_DirectMusicCollection;
    return o;
}

/* ------------------------------------------------------------------ segments */
static IDirectMusicSegment8Vtbl seg_vtbl;
static rm_segment *sgm(IDirectMusicSegment8 *i) { return impl_from(i, rm_segment, IDirectMusicSegment8_iface); }
static rm_segment *unsafe_segment(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicSegment8 *s = (IDirectMusicSegment8 *)u;
    if (s->lpVtbl == &seg_vtbl) return sgm(s);
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicSegment8, (void **)&s))) {
        IDirectMusicSegment8_Release(s);
        if (s->lpVtbl == &seg_vtbl) return sgm(s);
    }
    return NULL;
}

static HRESULT WINAPI seg_qi(IDirectMusicSegment8 *i, REFIID riid, void **out)
{
    rm_segment *s = sgm(i);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicSegment) ||
        IsEqualIID(riid, &IID_IDirectMusicSegment2) || IsEqualIID(riid, &IID_IDirectMusicSegment8)) *out = &s->IDirectMusicSegment8_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &s->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &s->IPersistStream_iface;
    else { return rm_noiface(__func__, riid, out); }
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG WINAPI seg_addref(IDirectMusicSegment8 *i) { return InterlockedIncrement(&sgm(i)->ref); }
static ULONG WINAPI seg_release(IDirectMusicSegment8 *i)
{
    rm_segment *s = sgm(i);
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) { dm_segment_release(s->seg); free(s); }
    return r;
}
static HRESULT WINAPI seg_getlength(IDirectMusicSegment8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; *t = sgm(i)->seg ? sgm(i)->seg->length : 0; return S_OK; }
static HRESULT WINAPI seg_setlength(IDirectMusicSegment8 *i, MUSIC_TIME t) { if (t <= 0) return E_INVALIDARG; if (sgm(i)->seg) sgm(i)->seg->length = t; return S_OK; }
static HRESULT WINAPI seg_getrepeats(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; *r = sgm(i)->seg ? sgm(i)->seg->repeats : 0; return S_OK; }
static HRESULT WINAPI seg_setrepeats(IDirectMusicSegment8 *i, DWORD r) { if (sgm(i)->seg) sgm(i)->seg->repeats = r; return S_OK; }
static HRESULT WINAPI seg_getres(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; *r = sgm(i)->seg ? sgm(i)->seg->resolution : 0; return S_OK; }
static HRESULT WINAPI seg_setres(IDirectMusicSegment8 *i, DWORD r) { if (sgm(i)->seg) sgm(i)->seg->resolution = r; return S_OK; }
static HRESULT WINAPI seg_gettrack(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, IDirectMusicTrack **t) { if (t) *t = NULL; return DMUS_E_NOT_FOUND; }
static HRESULT WINAPI seg_gettrackgroup(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD *g) { return DMUS_E_NOT_FOUND; }
static HRESULT WINAPI seg_inserttrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD g) { return rm_notimpl(__func__); }
static HRESULT WINAPI seg_removetrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t) { return S_FALSE; }
static HRESULT WINAPI seg_initplay(IDirectMusicSegment8 *i, IDirectMusicSegmentState **s, IDirectMusicPerformance *p, DWORD f) { return rm_notimpl(__func__); }
static HRESULT WINAPI seg_getgraph(IDirectMusicSegment8 *i, IDirectMusicGraph **g) { if (g) *g = NULL; return DMUS_E_NOT_FOUND; }
static HRESULT WINAPI seg_setgraph(IDirectMusicSegment8 *i, IDirectMusicGraph *g) { return rm_notimpl(__func__); }
static HRESULT WINAPI seg_addnotif(IDirectMusicSegment8 *i, REFGUID g) { return S_OK; }
static HRESULT WINAPI seg_remnotif(IDirectMusicSegment8 *i, REFGUID g) { return S_OK; }
static HRESULT WINAPI seg_getparam(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *p) { return DMUS_E_GET_UNSUPPORTED; }
static HRESULT WINAPI seg_setparam(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, void *p)
{
    char b[64];
    LOG_CALL("music: Segment::SetParam(%s) ignored", guidstr(g, b));
    return S_OK;
}
static HRESULT WINAPI seg_clone(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicSegment **o) { if (o) *o = NULL; return rm_notimpl(__func__); }
static HRESULT WINAPI seg_setstart(IDirectMusicSegment8 *i, MUSIC_TIME t)
{
    dm_segment *s = sgm(i)->seg;
    if (!s) return E_FAIL;
    if (t < 0 || t >= s->length) return DMUS_E_OUT_OF_RANGE;
    s->playstart = t;
    return S_OK;
}
static HRESULT WINAPI seg_getstart(IDirectMusicSegment8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; *t = sgm(i)->seg ? sgm(i)->seg->playstart : 0; return S_OK; }
static HRESULT WINAPI seg_setloop(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b)
{
    dm_segment *s = sgm(i)->seg;
    if (!s) return E_FAIL;
    if ((a || b) && (a < 0 || b > s->length || a >= b)) return DMUS_E_OUT_OF_RANGE;
    s->loopstart = a; s->loopend = b;
    return S_OK;
}
static HRESULT WINAPI seg_getloop(IDirectMusicSegment8 *i, MUSIC_TIME *a, MUSIC_TIME *b)
{
    if (!a || !b) return E_POINTER;
    *a = sgm(i)->seg ? sgm(i)->seg->loopstart : 0; *b = sgm(i)->seg ? sgm(i)->seg->loopend : 0;
    return S_OK;
}
static HRESULT WINAPI seg_setpch(IDirectMusicSegment8 *i, DWORD n, DWORD *p) { return S_OK; }
static HRESULT WINAPI seg_settrackcfg(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, DWORD on, DWORD off) { return S_OK; }
static HRESULT WINAPI seg_getapconfig(IDirectMusicSegment8 *i, IUnknown **c) { if (c) *c = NULL; return DMUS_E_NO_AUDIOPATH_CONFIG; }
static HRESULT WINAPI seg_compose(IDirectMusicSegment8 *i, MUSIC_TIME t, IDirectMusicSegment *a, IDirectMusicSegment *b, IDirectMusicSegment **c) { return rm_notimpl(__func__); }
static rm_perf *perf_from_unknown(IUnknown *u);
static HRESULT WINAPI seg_download(IDirectMusicSegment8 *i, IUnknown *target)
{
    rm_perf *p = perf_from_unknown(target);
    if (!p || !p->host || !sgm(i)->seg) return E_INVALIDARG;
    dm_host_lock(p->host);
    int hr = dm_segment_download(dm_host_perf(p->host), sgm(i)->seg);
    dm_host_unlock(p->host);
    LOG_CALL("music: Segment::Download -> %08x", hr);
    return hr < 0 ? DMUS_S_PARTIALDOWNLOAD : S_OK;
}
static HRESULT WINAPI seg_unload(IDirectMusicSegment8 *i, IUnknown *target)
{
    rm_perf *p = perf_from_unknown(target);
    if (!p || !p->host || !sgm(i)->seg) return E_INVALIDARG;
    dm_host_lock(p->host);
    dm_segment_unload(dm_host_perf(p->host), sgm(i)->seg);
    dm_host_unlock(p->host);
    return S_OK;
}
static IDirectMusicSegment8Vtbl seg_vtbl = { seg_qi, seg_addref, seg_release, seg_getlength, seg_setlength, seg_getrepeats,
    seg_setrepeats, seg_getres, seg_setres, seg_gettrack, seg_gettrackgroup, seg_inserttrack, seg_removetrack, seg_initplay,
    seg_getgraph, seg_setgraph, seg_addnotif, seg_remnotif, seg_getparam, seg_setparam, seg_clone, seg_setstart, seg_getstart,
    seg_setloop, seg_getloop, seg_setpch, seg_settrackcfg, seg_getapconfig, seg_compose, seg_download, seg_unload };

static rm_segment *sgo(IDirectMusicObject *i) { return impl_from(i, rm_segment, IDirectMusicObject_iface); }
static HRESULT WINAPI sgo_qi(IDirectMusicObject *i, REFIID r, void **o) { return seg_qi(&sgo(i)->IDirectMusicSegment8_iface, r, o); }
static ULONG WINAPI sgo_addref(IDirectMusicObject *i) { return seg_addref(&sgo(i)->IDirectMusicSegment8_iface); }
static ULONG WINAPI sgo_release(IDirectMusicObject *i) { return seg_release(&sgo(i)->IDirectMusicSegment8_iface); }
static HRESULT WINAPI sgo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { if (!d) return E_POINTER; DWORD sz = d->dwSize; memcpy(d, &sgo(i)->desc, sz < sizeof(*d) ? sz : sizeof(*d)); d->dwSize = sz; return S_OK; }
static HRESULT WINAPI sgo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return S_OK; }
static HRESULT WINAPI sgo_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d) { return rm_notimpl(__func__); }
static IDirectMusicObjectVtbl sgo_vtbl = { sgo_qi, sgo_addref, sgo_release, sgo_getdesc, sgo_setdesc, sgo_parse };

static dm_collection *loader_getcoll(void *ctx, const dm_guid *g, const char *file);
static rm_segment *sgp(IPersistStream *i) { return impl_from(i, rm_segment, IPersistStream_iface); }
static HRESULT WINAPI sgp_qi(IPersistStream *i, REFIID r, void **o) { return seg_qi(&sgp(i)->IDirectMusicSegment8_iface, r, o); }
static ULONG WINAPI sgp_addref(IPersistStream *i) { return seg_addref(&sgp(i)->IDirectMusicSegment8_iface); }
static ULONG WINAPI sgp_release(IPersistStream *i) { return seg_release(&sgp(i)->IDirectMusicSegment8_iface); }
static HRESULT WINAPI sgp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = CLSID_DirectMusicSegment; return S_OK; }
static HRESULT WINAPI sgp_isdirty(IPersistStream *i) { return S_FALSE; }
static HRESULT WINAPI sgp_load(IPersistStream *i, IStream *st)
{
    rm_segment *s = sgp(i);
    BYTE *b; DWORD n;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    dm_segment *g = dm_segment_load(b, n, loader_getcoll, s->loader);
    free(b);
    if (!g) return DMUS_E_UNSUPPORTED_STREAM;
    if (s->seg) dm_segment_release(s->seg);
    s->seg = g;
    s->desc.dwValidData |= DMUS_OBJ_LOADED;
    return S_OK;
}
static HRESULT WINAPI sgp_save(IPersistStream *i, IStream *s, BOOL c) { return rm_notimpl(__func__); }
static HRESULT WINAPI sgp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl sgp_vtbl = { sgp_qi, sgp_addref, sgp_release, sgp_getclass, sgp_isdirty, sgp_load, sgp_save, sgp_getsize };

static rm_segment *new_segment(dm_segment *g)
{
    rm_segment *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->IDirectMusicSegment8_iface.lpVtbl = &seg_vtbl;
    s->IDirectMusicObject_iface.lpVtbl = &sgo_vtbl;
    s->IPersistStream_iface.lpVtbl = &sgp_vtbl;
    s->ref = 1;
    s->seg = g;
    s->desc.dwSize = sizeof(s->desc);
    s->desc.dwValidData = DMUS_OBJ_CLASS | (g ? DMUS_OBJ_LOADED : 0);
    s->desc.guidClass = CLSID_DirectMusicSegment;
    if (g && g->name[0]) { MultiByteToWideChar(CP_ACP, 0, g->name, -1, s->desc.wszName, DMUS_MAX_NAME); s->desc.dwValidData |= DMUS_OBJ_NAME; }
    return s;
}

/* ------------------------------------------------------------------ segment states */
static IDirectMusicSegmentState8Vtbl st_vtbl;
static rm_state *sst(IDirectMusicSegmentState8 *i) { return impl_from(i, rm_state, IDirectMusicSegmentState8_iface); }
static rm_state *unsafe_state(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicSegmentState8 *s = (IDirectMusicSegmentState8 *)u;
    if (s->lpVtbl == &st_vtbl) return sst(s);
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicSegmentState8, (void **)&s))) {
        IDirectMusicSegmentState8_Release(s);
        if (s->lpVtbl == &st_vtbl) return sst(s);
    }
    return NULL;
}
static HRESULT WINAPI st_qi(IDirectMusicSegmentState8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicSegmentState) ||
        IsEqualIID(riid, &IID_IDirectMusicSegmentState8)) { *out = i; InterlockedIncrement(&sst(i)->ref); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI st_addref(IDirectMusicSegmentState8 *i) { return InterlockedIncrement(&sst(i)->ref); }
static ULONG WINAPI st_release(IDirectMusicSegmentState8 *i)
{
    rm_state *s = sst(i);
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        if (s->perf->host) { dm_host_lock(s->perf->host); dm_segstate_release(s->ss); dm_host_unlock(s->perf->host); }
        IDirectMusicSegment8_Release(&s->seg->IDirectMusicSegment8_iface);
        IDirectMusicPerformance8_Release(&s->perf->IDirectMusicPerformance8_iface);
        free(s);
    }
    return r;
}
static HRESULT WINAPI st_getrepeats(IDirectMusicSegmentState8 *i, DWORD *r) { if (!r) return E_POINTER; *r = sst(i)->seg->seg->repeats; return S_OK; }
static HRESULT WINAPI st_getsegment(IDirectMusicSegmentState8 *i, IDirectMusicSegment **s)
{
    if (!s) return E_POINTER;
    *s = (IDirectMusicSegment *)&sst(i)->seg->IDirectMusicSegment8_iface;
    IDirectMusicSegment_AddRef(*s);
    return S_OK;
}
static HRESULT WINAPI st_getstart(IDirectMusicSegmentState8 *i, MUSIC_TIME *t)
{
    if (!t) return E_POINTER;
    dm_segstate_start_time(sst(i)->ss, (mtime_t *)t);
    return S_OK;
}
static HRESULT WINAPI st_getseek(IDirectMusicSegmentState8 *i, MUSIC_TIME *t) { return rm_notimpl(__func__); }
static HRESULT WINAPI st_getstartpoint(IDirectMusicSegmentState8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; *t = sst(i)->seg->seg->playstart; return S_OK; }
static HRESULT WINAPI st_settrackcfg(IDirectMusicSegmentState8 *i, REFGUID g, DWORD gb, DWORD n, DWORD on, DWORD off) { return S_OK; }
static HRESULT WINAPI st_getobj(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    if (o) *o = NULL;
    return DMUS_E_NOT_FOUND;
}
static IDirectMusicSegmentState8Vtbl st_vtbl = { st_qi, st_addref, st_release, st_getrepeats, st_getsegment, st_getstart,
    st_getseek, st_getstartpoint, st_settrackcfg, st_getobj };

/* ------------------------------------------------------------------ audiopaths */
static IDirectMusicAudioPathVtbl ap_vtbl;
static rm_path *apt(IDirectMusicAudioPath *i) { return impl_from(i, rm_path, IDirectMusicAudioPath_iface); }
static rm_path *unsafe_path(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicAudioPath *a = (IDirectMusicAudioPath *)u;
    if (a->lpVtbl == &ap_vtbl) return apt(a);
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicAudioPath, (void **)&a))) {
        IDirectMusicAudioPath_Release(a);
        if (a->lpVtbl == &ap_vtbl) return apt(a);
    }
    return NULL;
}
static HRESULT WINAPI ap_qi(IDirectMusicAudioPath *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicAudioPath)) { *out = i; InterlockedIncrement(&apt(i)->ref); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI ap_addref(IDirectMusicAudioPath *i) { return InterlockedIncrement(&apt(i)->ref); }
static ULONG WINAPI ap_release(IDirectMusicAudioPath *i)
{
    rm_path *a = apt(i);
    LONG r = InterlockedDecrement(&a->ref);
    if (!r) {
        if (a->perf->host) { dm_host_lock(a->perf->host); dm_audiopath_release(a->ap); dm_host_unlock(a->perf->host); }
        IDirectMusicPerformance8_Release(&a->perf->IDirectMusicPerformance8_iface);
        free(a);
    }
    return r;
}
static HRESULT WINAPI ap_getobj(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    rm_path *a = apt(i);
    if (!o) return E_POINTER;
    *o = NULL;
    if (stage == DMUS_PATH_PERFORMANCE) return IDirectMusicPerformance8_QueryInterface(&a->perf->IDirectMusicPerformance8_iface, iid, o);
    if (stage == DMUS_PATH_AUDIOPATH) return ap_qi(i, iid, o);
    char b1[64];
    LOG_INFO("music: AudioPath::GetObjectInPath(stage %lx, %s) not supported", (unsigned long)stage, guidstr(iid, b1));
    return DMUS_E_NOT_FOUND;
}
static HRESULT WINAPI ap_activate(IDirectMusicAudioPath *i, BOOL on)
{
    rm_path *a = apt(i);
    if (!a->perf->host) return DMUS_E_NOT_INIT;
    dm_host_lock(a->perf->host);
    int hr = dm_audiopath_activate(a->ap, on);
    dm_host_unlock(a->perf->host);
    return hr;
}
static HRESULT WINAPI ap_setvolume(IDirectMusicAudioPath *i, LONG vol, DWORD dur)
{
    rm_path *a = apt(i);
    if (!a->perf->host) return DMUS_E_NOT_INIT;
    dm_host_lock(a->perf->host);
    int hr = dm_audiopath_set_volume(a->ap, vol, dur);
    dm_host_unlock(a->perf->host);
    LOG_TRACE("music: AudioPath::SetVolume(%ld, %lu) -> %08x", vol, (unsigned long)dur, hr);
    return hr;
}
static HRESULT WINAPI ap_convert(IDirectMusicAudioPath *i, DWORD in, DWORD *out)
{
    if (!out) return E_POINTER;
    uint32_t r;
    if (dm_audiopath_convert(apt(i)->ap, in, &r) < 0) return E_INVALIDARG;
    *out = r;
    return S_OK;
}
static IDirectMusicAudioPathVtbl ap_vtbl = { ap_qi, ap_addref, ap_release, ap_getobj, ap_activate, ap_setvolume, ap_convert };

static rm_path *new_path(rm_perf *p, dm_audiopath *ap)
{
    rm_path *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->IDirectMusicAudioPath_iface.lpVtbl = &ap_vtbl;
    a->ref = 1;
    a->ap = ap;
    a->perf = p;
    IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    return a;
}

/* ------------------------------------------------------------------ the performance */
static IDirectMusicPerformance8Vtbl pf_vtbl;
static rm_perf *prf(IDirectMusicPerformance8 *i) { return impl_from(i, rm_perf, IDirectMusicPerformance8_iface); }
static rm_perf *perf_from_unknown(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicPerformance8 *p = (IDirectMusicPerformance8 *)u;
    if (p->lpVtbl == &pf_vtbl) return prf(p);
    rm_path *a = unsafe_path(u);
    if (a) return a->perf;
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicPerformance8, (void **)&p))) {
        IDirectMusicPerformance8_Release(p);
        if (p->lpVtbl == &pf_vtbl) return prf(p);
    }
    return NULL;
}

#define PERF_LOCKED(p, expr) do { if (!(p)->host) return DMUS_E_NOT_INIT; dm_host_lock((p)->host); expr; dm_host_unlock((p)->host); } while (0)

static HRESULT WINAPI pf_qi(IDirectMusicPerformance8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicPerformance) ||
        IsEqualIID(riid, &IID_IDirectMusicPerformance2) || IsEqualIID(riid, &IID_IDirectMusicPerformance8)) {
        *out = i; InterlockedIncrement(&prf(i)->ref); return S_OK;
    }
    char b[64];
    LOG_INFO("music: Performance::QueryInterface(%s): no", guidstr(riid, b));
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI pf_addref(IDirectMusicPerformance8 *i) { return InterlockedIncrement(&prf(i)->ref); }
static void pf_closedown(rm_perf *p)
{
    if (p->closed) return;
    p->closed = TRUE;
    if (p->defpath) { rm_path *a = p->defpath; p->defpath = NULL; IDirectMusicAudioPath_Release(&a->IDirectMusicAudioPath_iface); }
    if (p->host) {
        dm_host_lock(p->host);
        dm_perf_stop(dm_host_perf(p->host), NULL, NULL, 0, 0);
        dm_host_unlock(p->host);
    }
}
static ULONG WINAPI pf_release(IDirectMusicPerformance8 *i)
{
    rm_perf *p = prf(i);
    LONG r = InterlockedDecrement(&p->ref);
    if (!r) {
        p->ref = 1;     /* guard against re-entry from children */
        pf_closedown(p);
        if (s_clockperf == p) s_clockperf = NULL;
        dm_host_free(p->host);
        if (p->dm) IDirectMusic8_Release(p->dm);
        free(p);
    }
    return r;
}

static HRESULT ensure_host(rm_perf *p, uint32_t rate, uint32_t voices)
{
    if (p->host) return S_OK;
    p->host = dm_host_new(rate ? rate : 22050, voices ? voices : 64, 64);
    if (!p->host) return E_OUTOFMEMORY;
    if (!s_clockperf) s_clockperf = p;
    return S_OK;
}

static void hand_out_dmusic(rm_perf *p, IDirectMusic **pdm)
{
    if (!p->dm) p->dm = new_dmusic();
    if (pdm && !*pdm && p->dm) { *pdm = (IDirectMusic *)p->dm; IDirectMusic8_AddRef(p->dm); }
}

static HRESULT WINAPI pf_init(IDirectMusicPerformance8 *i, IDirectMusic **pdm, LPDIRECTSOUND ds, HWND w)
{
    rm_perf *p = prf(i);
    LOG_CALL("music: Performance::Init");
    if (p->host) return DMUS_E_ALREADY_INITED;
    HRESULT hr = ensure_host(p, 22050, 64);
    if (FAILED(hr)) return hr;
    hand_out_dmusic(p, pdm);
    return S_OK;
}
static HRESULT pf_play(rm_perf *p, IUnknown *src, DWORD flags, __int64 start, IDirectMusicSegmentState **out, IUnknown *from, IUnknown *path);
static HRESULT WINAPI pf_playsegment(IDirectMusicPerformance8 *i, IDirectMusicSegment *s, DWORD f, __int64 t, IDirectMusicSegmentState **o)
{
    return pf_play(prf(i), (IUnknown *)s, f, t, o, NULL, NULL);
}
static HRESULT WINAPI pf_stop(IDirectMusicPerformance8 *i, IDirectMusicSegment *s, IDirectMusicSegmentState *st, MUSIC_TIME t, DWORD f)
{
    rm_perf *p = prf(i);
    rm_segment *g = unsafe_segment((IUnknown *)s);
    rm_state *ss = unsafe_state((IUnknown *)st);
    if ((s && !g) || (st && !ss)) return E_INVALIDARG;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_stop(dm_host_perf(p->host), g ? g->seg : NULL, ss ? ss->ss : NULL, t, f));
    LOG_CALL("music: Performance::Stop(%p, %p, %ld) -> %08x", (void *)s, (void *)st, (long)t, hr);
    return hr;
}
static HRESULT WINAPI pf_getsegstate(IDirectMusicPerformance8 *i, IDirectMusicSegmentState **o, MUSIC_TIME t) { if (o) *o = NULL; return DMUS_E_NOT_FOUND; }
static HRESULT WINAPI pf_setprepare(IDirectMusicPerformance8 *i, DWORD ms) { rm_perf *p = prf(i); PERF_LOCKED(p, dm_perf_set_prepare_time(dm_host_perf(p->host), ms)); return S_OK; }
static HRESULT WINAPI pf_getprepare(IDirectMusicPerformance8 *i, DWORD *ms) { rm_perf *p = prf(i); if (!ms) return E_POINTER; PERF_LOCKED(p, *ms = dm_perf_get_prepare_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_setbumper(IDirectMusicPerformance8 *i, DWORD ms) { rm_perf *p = prf(i); PERF_LOCKED(p, dm_perf_set_bumper_length(dm_host_perf(p->host), ms)); return S_OK; }
static HRESULT WINAPI pf_getbumper(IDirectMusicPerformance8 *i, DWORD *ms) { rm_perf *p = prf(i); if (!ms) return E_POINTER; PERF_LOCKED(p, *ms = dm_perf_get_bumper_length(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_sendpmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_m2r(IDirectMusicPerformance8 *i, MUSIC_TIME mt, REFERENCE_TIME *rt) { rm_perf *p = prf(i); if (!rt) return E_POINTER; PERF_LOCKED(p, dm_perf_m2r(dm_host_perf(p->host), mt, rt)); return S_OK; }
static HRESULT WINAPI pf_r2m(IDirectMusicPerformance8 *i, REFERENCE_TIME rt, MUSIC_TIME *mt) { rm_perf *p = prf(i); if (!mt) return E_POINTER; PERF_LOCKED(p, dm_perf_r2m(dm_host_perf(p->host), rt, (mtime_t *)mt)); return S_OK; }
static HRESULT WINAPI pf_isplaying(IDirectMusicPerformance8 *i, IDirectMusicSegment *s, IDirectMusicSegmentState *st)
{
    rm_perf *p = prf(i);
    rm_segment *g = unsafe_segment((IUnknown *)s);
    rm_state *ss = unsafe_state((IUnknown *)st);
    if (!g && !ss) return s || st ? S_FALSE : E_POINTER;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_is_playing(dm_host_perf(p->host), g ? g->seg : NULL, ss ? ss->ss : NULL));
    return hr;
}
static HRESULT WINAPI pf_gettime(IDirectMusicPerformance8 *i, REFERENCE_TIME *rt, MUSIC_TIME *mt)
{
    rm_perf *p = prf(i);
    PERF_LOCKED(p, dm_perf_get_time(dm_host_perf(p->host), rt, (mtime_t *)mt));
    return S_OK;
}
static HRESULT WINAPI pf_allocpmsg(IDirectMusicPerformance8 *i, ULONG cb, DMUS_PMSG **m) { if (m) *m = NULL; return rm_notimpl(__func__); }
static HRESULT WINAPI pf_freepmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_getgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph **g) { if (g) *g = NULL; return DMUS_E_NOT_FOUND; }
static HRESULT WINAPI pf_setgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph *g) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_setnotif(IDirectMusicPerformance8 *i, HANDLE h, REFERENCE_TIME t) { prf(i)->notify = h; return S_OK; }
static HRESULT WINAPI pf_getnotif(IDirectMusicPerformance8 *i, DMUS_NOTIFICATION_PMSG **m) { if (m) *m = NULL; return S_FALSE; }
static HRESULT WINAPI pf_addnotif(IDirectMusicPerformance8 *i, REFGUID g) { return S_OK; }
static HRESULT WINAPI pf_remnotif(IDirectMusicPerformance8 *i, REFGUID g) { return S_OK; }
static HRESULT WINAPI pf_addport(IDirectMusicPerformance8 *i, IDirectMusicPort *port)
{
    rm_perf *p = prf(i);
    PERF_LOCKED(p, if (!dm_perf_pchannels(dm_host_perf(p->host))) dm_perf_assign_block(dm_host_perf(p->host), 0, 0, 1));
    return S_OK;
}
static HRESULT WINAPI pf_remport(IDirectMusicPerformance8 *i, IDirectMusicPort *port) { return S_OK; }
static HRESULT WINAPI pf_assignblock(IDirectMusicPerformance8 *i, DWORD b, IDirectMusicPort *port, DWORD g)
{
    rm_perf *p = prf(i);
    int hr;
    PERF_LOCKED(p, hr = dm_perf_assign_block(dm_host_perf(p->host), b, 0, g));
    return hr;
}
static HRESULT WINAPI pf_assignpch(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort *port, DWORD g, DWORD c)
{
    rm_perf *p = prf(i);
    int hr;
    PERF_LOCKED(p, hr = dm_perf_assign_pchannel(dm_host_perf(p->host), pch, 0, g, c));
    return hr;
}
static HRESULT WINAPI pf_pchinfo(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort **port, DWORD *g, DWORD *c) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_downloadinst(IDirectMusicPerformance8 *i, IDirectMusicInstrument *in, DWORD pch, IDirectMusicDownloadedInstrument **d,
                                      DMUS_NOTERANGE *r, DWORD n, IDirectMusicPort **port, DWORD *g, DWORD *c) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_invalidate(IDirectMusicPerformance8 *i, MUSIC_TIME t, DWORD f) { rm_perf *p = prf(i); PERF_LOCKED(p, dm_perf_invalidate(dm_host_perf(p->host), t, f)); return S_OK; }
static HRESULT WINAPI pf_getparam(IDirectMusicPerformance8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp) { return DMUS_E_NOT_FOUND; }
static HRESULT WINAPI pf_setparam(IDirectMusicPerformance8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, void *pp) { return DMUS_E_SET_UNSUPPORTED; }
static HRESULT WINAPI pf_getglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz)
{
    rm_perf *p = prf(i);
    if (!v) return E_POINTER;
    if (IsEqualGUID(g, &GUID_PerfMasterVolume) && sz >= 4) { *(long *)v = 0; return S_OK; }
    if (IsEqualGUID(g, &GUID_PerfMasterTempo) && sz >= 4) { *(float *)v = 1.0f; return S_OK; }
    if (IsEqualGUID(g, &GUID_PerfAutoDownload) && sz >= 4) { int on = 0; PERF_LOCKED(p, on = dm_perf_get_auto_download(dm_host_perf(p->host))); *(BOOL *)v = on; return S_OK; }
    return DMUS_E_NOT_FOUND;
}
static HRESULT WINAPI pf_setglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz)
{
    rm_perf *p = prf(i);
    char b[64];
    if (!v) return E_POINTER;
    LOG_CALL("music: Performance::SetGlobalParam(%s)", guidstr(g, b));
    if (IsEqualGUID(g, &GUID_PerfMasterVolume) && sz >= 4) { if (p->host) dm_host_set_volume(p->host, *(long *)v); return S_OK; }
    if (IsEqualGUID(g, &GUID_PerfAutoDownload) && sz >= 4) { PERF_LOCKED(p, dm_perf_set_auto_download(dm_host_perf(p->host), *(BOOL *)v)); return S_OK; }
    return S_OK;
}
static HRESULT WINAPI pf_getlatency(IDirectMusicPerformance8 *i, REFERENCE_TIME *t) { rm_perf *p = prf(i); if (!t) return E_POINTER; PERF_LOCKED(p, *t = dm_perf_latency_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_getqueue(IDirectMusicPerformance8 *i, REFERENCE_TIME *t) { rm_perf *p = prf(i); if (!t) return E_POINTER; PERF_LOCKED(p, *t = dm_perf_queue_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_adjust(IDirectMusicPerformance8 *i, REFERENCE_TIME t) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_closedown_m(IDirectMusicPerformance8 *i) { LOG_CALL("music: Performance::CloseDown"); pf_closedown(prf(i)); return S_OK; }
static HRESULT WINAPI pf_resolved(IDirectMusicPerformance8 *i, REFERENCE_TIME t, REFERENCE_TIME *o, DWORD f) { if (!o) return E_POINTER; *o = t; return S_OK; }
static HRESULT WINAPI pf_midi2music(IDirectMusicPerformance8 *i, BYTE v, DMUS_CHORD_KEY *c, BYTE m, BYTE l, WORD *o) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_music2midi(IDirectMusicPerformance8 *i, WORD v, DMUS_CHORD_KEY *c, BYTE m, BYTE l, BYTE *o) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_time2rhythm(IDirectMusicPerformance8 *i, MUSIC_TIME t, DMUS_TIMESIGNATURE *ts, WORD *m, BYTE *b, BYTE *g, short *o) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_rhythm2time(IDirectMusicPerformance8 *i, WORD m, BYTE b, BYTE g, short o, DMUS_TIMESIGNATURE *ts, MUSIC_TIME *t) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_createstdpath(IDirectMusicPerformance8 *i, DWORD type, DWORD npch, BOOL act, IDirectMusicAudioPath **out);
static HRESULT WINAPI pf_initaudio(IDirectMusicPerformance8 *i, IDirectMusic **pdm, IDirectSound **pds, HWND w, DWORD type, DWORD npch,
                                   DWORD flags, DMUS_AUDIOPARAMS *params)
{
    rm_perf *p = prf(i);
    LOG_CALL("music: Performance::InitAudio(type %lu, %lu pchannels, flags %lx)", (unsigned long)type, (unsigned long)npch, (unsigned long)flags);
    if (p->host) return DMUS_E_ALREADY_INITED;
    uint32_t rate = 22050, voices = 64;
    if (params && params->dwSize >= sizeof(DMUS_AUDIOPARAMS)) {
        if ((params->dwValidData & DMUS_AUDIOPARAMS_SAMPLERATE) && params->dwSampleRate) rate = params->dwSampleRate;
        if ((params->dwValidData & DMUS_AUDIOPARAMS_VOICES) && params->dwVoices) voices = params->dwVoices;
    }
    HRESULT hr = ensure_host(p, rate, voices);
    if (FAILED(hr)) return hr;
    if (params && params->dwSize >= sizeof(DMUS_AUDIOPARAMS)) {
        params->dwValidData |= DMUS_AUDIOPARAMS_SAMPLERATE | DMUS_AUDIOPARAMS_VOICES;
        params->dwSampleRate = rate; params->dwVoices = voices;
    }
    hand_out_dmusic(p, pdm);
    if (pds && !*pds) {
        IDirectSound8 *ds;
        if (SUCCEEDED(rl_dsound_create(&IID_IDirectSound8, (void **)&ds, TRUE))) {
            if (SUCCEEDED(IDirectSound8_Initialize(ds, NULL))) { IDirectSound8_SetCooperativeLevel(ds, w ? w : GetDesktopWindow(), DSSCL_PRIORITY); *pds = (IDirectSound *)ds; }
            else IDirectSound8_Release(ds);
        }
    }
    if (type) {
        IDirectMusicAudioPath *a;
        if (SUCCEEDED(pf_createstdpath(i, type, npch, TRUE, &a))) {
            p->defpath = apt(a);
            PERF_LOCKED(p, dm_perf_set_default_audiopath(dm_host_perf(p->host), p->defpath->ap));
        }
    }
    return S_OK;
}

static HRESULT pf_play(rm_perf *p, IUnknown *src, DWORD flags, __int64 start, IDirectMusicSegmentState **out, IUnknown *from, IUnknown *path)
{
    if (out) *out = NULL;
    rm_segment *g = unsafe_segment(src);
    if (!g || !g->seg) return E_INVALIDARG;
    rm_path *a = path ? unsafe_path(path) : NULL;
    if (path && !a) return E_INVALIDARG;
    if (!p->host) return DMUS_E_NOT_INIT;
    dm_segstate *ss = NULL;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    int hr = dm_perf_play(pf, g->seg, flags, start, a ? a->ap : NULL, &ss);
    if (hr >= 0 && from) {
        rm_segment *fg = unsafe_segment(from);
        rm_state *fs = unsafe_state(from);
        mtime_t t;
        dm_segstate_start_time(ss, &t);
        if (fg || fs) dm_perf_stop(pf, fg ? fg->seg : NULL, fs ? fs->ss : NULL, t, 0);
    }
    dm_host_unlock(p->host);
    LOG_CALL("music: PlaySegment(%s, flags %lx, %lld) -> %08x", g->seg->name, (unsigned long)flags, (long long)start, hr);
    if (hr < 0) return hr;
    rm_state *s = calloc(1, sizeof(*s));
    if (!s) { dm_host_lock(p->host); dm_segstate_release(ss); dm_host_unlock(p->host); return E_OUTOFMEMORY; }
    s->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    s->ref = 1;
    s->ss = ss;
    s->seg = g; IDirectMusicSegment8_AddRef(&g->IDirectMusicSegment8_iface);
    s->perf = p; IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    if (out) *out = (IDirectMusicSegmentState *)&s->IDirectMusicSegmentState8_iface;
    else IDirectMusicSegmentState8_Release(&s->IDirectMusicSegmentState8_iface);
    return S_OK;
}

static HRESULT WINAPI pf_playex(IDirectMusicPerformance8 *i, IUnknown *src, WCHAR *name, IUnknown *trans, DWORD flags, __int64 start,
                                IDirectMusicSegmentState **out, IUnknown *from, IUnknown *path)
{
    return pf_play(prf(i), src, flags, start, out, from, path);
}
static HRESULT WINAPI pf_stopex(IDirectMusicPerformance8 *i, IUnknown *obj, __int64 t, DWORD f)
{
    rm_perf *p = prf(i);
    if (!p->host) return DMUS_E_NOT_INIT;
    const void *vt = obj ? *(const void **)obj : NULL;        /* our own objects: no QueryInterface probing */
    rm_segment *g = (vt == &st_vtbl || vt == &ap_vtbl) ? NULL : unsafe_segment(obj);
    rm_state *s = (g || vt == &ap_vtbl) ? NULL : unsafe_state(obj);
    rm_path *a = (g || s) ? NULL : unsafe_path(obj);
    if (obj && !g && !s && !a) return E_INVALIDARG;
    int hr;
    dm_host_lock(p->host);
    if (a) hr = dm_perf_stop_audiopath(dm_host_perf(p->host), a->ap, (mtime_t)t, f);
    else hr = dm_perf_stop(dm_host_perf(p->host), g ? g->seg : NULL, s ? s->ss : NULL, (mtime_t)t, f);
    dm_host_unlock(p->host);
    LOG_CALL("music: Performance::StopEx(%p) -> %08x", (void *)obj, hr);
    return hr;
}
static HRESULT WINAPI pf_clonepmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *s, DMUS_PMSG **d) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_createpath(IDirectMusicPerformance8 *i, IUnknown *cfg, BOOL act, IDirectMusicAudioPath **out)
{
    LOG_INFO("music: CreateAudioPath from a configuration: using a standard stereo path");
    return pf_createstdpath(i, DMUS_APATH_DYNAMIC_STEREO, 64, act, out);
}
static HRESULT WINAPI pf_createstdpath(IDirectMusicPerformance8 *i, DWORD type, DWORD npch, BOOL act, IDirectMusicAudioPath **out)
{
    rm_perf *p = prf(i);
    if (!out) return E_POINTER;
    *out = NULL;
    if (!p->host) return DMUS_E_NOT_INIT;
    if (!npch) npch = 16;
    dm_audiopath *ap;
    PERF_LOCKED(p, ap = dm_perf_create_audiopath(dm_host_perf(p->host), npch, act));
    if (!ap) return E_OUTOFMEMORY;
    rm_path *a = new_path(p, ap);
    if (!a) { PERF_LOCKED(p, dm_audiopath_release(ap)); return E_OUTOFMEMORY; }
    LOG_CALL("music: CreateStandardAudioPath(type %lu, %lu pchannels) -> %p", (unsigned long)type, (unsigned long)npch, (void *)a);
    *out = &a->IDirectMusicAudioPath_iface;
    return S_OK;
}
static HRESULT WINAPI pf_setdefpath(IDirectMusicPerformance8 *i, IDirectMusicAudioPath *path)
{
    rm_perf *p = prf(i);
    rm_path *a = path ? unsafe_path((IUnknown *)path) : NULL;
    if (path && !a) return E_INVALIDARG;
    if (a) IDirectMusicAudioPath_AddRef(path);
    if (p->defpath) IDirectMusicAudioPath_Release(&p->defpath->IDirectMusicAudioPath_iface);
    p->defpath = a;
    PERF_LOCKED(p, dm_perf_set_default_audiopath(dm_host_perf(p->host), a ? a->ap : NULL));
    return S_OK;
}
static HRESULT WINAPI pf_getdefpath(IDirectMusicPerformance8 *i, IDirectMusicAudioPath **out)
{
    rm_perf *p = prf(i);
    if (!out) return E_POINTER;
    if (!p->defpath) { *out = NULL; return DMUS_E_NOT_FOUND; }
    *out = &p->defpath->IDirectMusicAudioPath_iface;
    IDirectMusicAudioPath_AddRef(*out);
    return S_OK;
}
static HRESULT WINAPI pf_getparamex(IDirectMusicPerformance8 *i, REFGUID g, DWORD id, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp) { return DMUS_E_NOT_FOUND; }

static IDirectMusicPerformance8Vtbl pf_vtbl = { pf_qi, pf_addref, pf_release, pf_init, pf_playsegment, pf_stop, pf_getsegstate,
    pf_setprepare, pf_getprepare, pf_setbumper, pf_getbumper, pf_sendpmsg, pf_m2r, pf_r2m, pf_isplaying, pf_gettime,
    pf_allocpmsg, pf_freepmsg, pf_getgraph, pf_setgraph, pf_setnotif, pf_getnotif, pf_addnotif, pf_remnotif, pf_addport,
    pf_remport, pf_assignblock, pf_assignpch, pf_pchinfo, pf_downloadinst, pf_invalidate, pf_getparam, pf_setparam,
    pf_getglobal, pf_setglobal, pf_getlatency, pf_getqueue, pf_adjust, pf_closedown_m, pf_resolved, pf_midi2music,
    pf_music2midi, pf_time2rhythm, pf_rhythm2time, pf_initaudio, pf_playex, pf_stopex, pf_clonepmsg, pf_createpath,
    pf_createstdpath, pf_setdefpath, pf_getdefpath, pf_getparamex };

/* ------------------------------------------------------------------ the loader */
static rm_loader *ldr(IDirectMusicLoader8 *i) { return impl_from(i, rm_loader, IDirectMusicLoader8_iface); }
static HRESULT WINAPI ld_qi(IDirectMusicLoader8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicLoader) || IsEqualIID(riid, &IID_IDirectMusicLoader8)) {
        *out = i; InterlockedIncrement(&ldr(i)->ref); return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI ld_addref(IDirectMusicLoader8 *i) { return InterlockedIncrement(&ldr(i)->ref); }
static void ld_clear(rm_loader *l)
{
    for (int k = 0; k < l->nce; k++) if (l->ce[k].obj) IUnknown_Release(l->ce[k].obj);
    free(l->ce); l->ce = NULL; l->nce = 0;
}
static ULONG WINAPI ld_release(IDirectMusicLoader8 *i)
{
    rm_loader *l = ldr(i);
    LONG r = InterlockedDecrement(&l->ref);
    if (!r) { ld_clear(l); free(l); }
    return r;
}

static void full_path(rm_loader *l, const DMUS_OBJECTDESC *d, WCHAR *out)
{
    if ((d->dwValidData & DMUS_OBJ_FULLPATH) || !l->dir[0] || d->wszFileName[1] == L':' || d->wszFileName[0] == L'\\') {
        lstrcpynW(out, d->wszFileName, MAX_PATH);
        return;
    }
    lstrcpynW(out, l->dir, MAX_PATH);
    int n = lstrlenW(out);
    if (n && out[n - 1] != L'\\' && out[n - 1] != L'/' && n < MAX_PATH - 1) { out[n++] = L'\\'; out[n] = 0; }
    lstrcpynW(out + n, d->wszFileName, MAX_PATH - n);
}

static cache_entry *cache_find(rm_loader *l, const WCHAR *path, const GUID *g)
{
    for (int k = 0; k < l->nce; k++) {
        if (path && path[0] && !lstrcmpiW(l->ce[k].path, path)) return &l->ce[k];
        if (g && l->ce[k].hasguid && IsEqualGUID(&l->ce[k].guid, g)) return &l->ce[k];
    }
    return NULL;
}

static void cache_add(rm_loader *l, const WCHAR *path, const GUID *g, IUnknown *obj)
{
    if (!l->cache) return;
    l->ce = realloc(l->ce, (l->nce + 1) * sizeof(*l->ce));
    cache_entry *e = &l->ce[l->nce++];
    memset(e, 0, sizeof(*e));
    if (path) lstrcpynW(e->path, path, MAX_PATH);
    if (g) { e->guid = *g; e->hasguid = TRUE; }
    e->obj = obj;
    IUnknown_AddRef(obj);
}

/* a band's DLS reference (or the default GM set) while loading a segment */
static dm_collection *loader_getcoll(void *ctx, const dm_guid *g, const char *file)
{
    rm_loader *l = ctx;
    if ((!g && !file) || (g && !memcmp(g, &RM_GUID_DefaultGMCollection, sizeof(GUID)) && !file))
        return dm_collection_from_file(dm_gm_dls_path(), 1);
    if (file) {
        char path[MAX_PATH * 2];
        if (file[0] && file[1] == ':') lstrcpynA(path, file, sizeof(path));
        else {
            char dir[MAX_PATH] = "";
            if (l) w2a(dir, l->dir, MAX_PATH);
            snprintf(path, sizeof(path), "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '\\' ? "\\" : "", file);
        }
        dm_collection *c = dm_collection_from_file(path, 0);
        if (c) return c;
    }
    if (g && !memcmp(g, &RM_GUID_DefaultGMCollection, sizeof(GUID))) return dm_collection_from_file(dm_gm_dls_path(), 1);
    return NULL;
}

static HRESULT WINAPI ld_getobject(IDirectMusicLoader8 *i, LPDMUS_OBJECTDESC d, REFIID riid, LPVOID *out)
{
    rm_loader *l = ldr(i);
    char b1[64];
    if (!d || !out) return E_POINTER;
    *out = NULL;
    WCHAR path[MAX_PATH] = L"";
    if (d->dwValidData & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH)) full_path(l, d, path);
    const GUID *g = (d->dwValidData & DMUS_OBJ_OBJECT) ? &d->guidObject : NULL;
    BOOL isseg = (d->dwValidData & DMUS_OBJ_CLASS) && IsEqualCLSID(&d->guidClass, &CLSID_DirectMusicSegment);
    BOOL iscol = (d->dwValidData & DMUS_OBJ_CLASS) && IsEqualCLSID(&d->guidClass, &CLSID_DirectMusicCollection);
    char a[MAX_PATH];
    w2a(a, path, MAX_PATH);
    LOG_CALL("music: Loader::GetObject(%s, %s)", guidstr(&d->guidClass, b1), a);
    cache_entry *e = cache_find(l, path, g);
    if (e) return IUnknown_QueryInterface(e->obj, riid, out);
    if (iscol) {
        dm_collection *c = NULL;
        if (g && IsEqualGUID(g, &RM_GUID_DefaultGMCollection) && !path[0]) c = dm_collection_from_file(dm_gm_dls_path(), 1);
        else if (path[0]) c = dm_collection_from_file(a, 0);
        else if (d->dwValidData & DMUS_OBJ_MEMORY) {
            c = dls_load(d->pbMemData, (size_t)d->llMemLength, 0);
        }
        if (!c) return DMUS_E_LOADER_FAILEDOPEN;
        rm_collection *o = new_collection(c);
        if (!o) return E_OUTOFMEMORY;
        if (g) o->desc.guidObject = *g;
        cache_add(l, path, g, (IUnknown *)&o->IDirectMusicCollection_iface);
        HRESULT hr = col_qi(&o->IDirectMusicCollection_iface, riid, out);
        col_release(&o->IDirectMusicCollection_iface);
        return hr;
    }
    if (!isseg) {
        LOG_INFO("music: Loader::GetObject: class %s not supported", guidstr(&d->guidClass, b1));
        return DMUS_E_LOADER_NOCLASSID;
    }
    BYTE *data = NULL;
    DWORD size = 0;
    if (path[0]) { if (!read_whole_file(path, &data, &size)) return DMUS_E_LOADER_FAILEDOPEN; }
    else if (d->dwValidData & DMUS_OBJ_MEMORY) {
        size = (DWORD)d->llMemLength;
        data = malloc(size ? size : 1);
        if (data) memcpy(data, d->pbMemData, size);
    } else if ((d->dwValidData & DMUS_OBJ_STREAM) && d->pStream) {
        if (!read_stream(d->pStream, &data, &size)) return DMUS_E_LOADER_FAILEDOPEN;
    } else return DMUS_E_LOADER_NOFILENAME;
    if (!data) return E_OUTOFMEMORY;
    dm_segment *sgt = dm_segment_load(data, size, loader_getcoll, l);
    free(data);
    if (!sgt) return DMUS_E_UNSUPPORTED_STREAM;
    rm_segment *s = new_segment(sgt);
    if (!s) { dm_segment_release(sgt); return E_OUTOFMEMORY; }
    s->loader = l;
    if (path[0]) { lstrcpynW(s->desc.wszFileName, path, DMUS_MAX_FILENAME); s->desc.dwValidData |= DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH; }
    if (g) { s->desc.guidObject = *g; s->desc.dwValidData |= DMUS_OBJ_OBJECT; }
    cache_add(l, path, g, (IUnknown *)&s->IDirectMusicSegment8_iface);
    HRESULT hr = seg_qi(&s->IDirectMusicSegment8_iface, riid, out);
    seg_release(&s->IDirectMusicSegment8_iface);
    LOG_CALL("music: loaded segment %s (%d tracks) -> %08lx", sgt->name, sgt->ntracks, (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI ld_setobject(IDirectMusicLoader8 *i, LPDMUS_OBJECTDESC d) { return S_OK; }
static HRESULT WINAPI ld_setdir(IDirectMusicLoader8 *i, REFGUID cls, WCHAR *path, BOOL clear)
{
    rm_loader *l = ldr(i);
    if (!path) return E_POINTER;
    WCHAR full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, NULL)) lstrcpynW(full, path, MAX_PATH);
    if (clear) ld_clear(l);
    BOOL same = !lstrcmpiW(full, l->dir);
    lstrcpynW(l->dir, full, MAX_PATH);
    char a[MAX_PATH];
    w2a(a, full, MAX_PATH);
    LOG_CALL("music: Loader::SetSearchDirectory(%s)", a);
    return same ? S_FALSE : S_OK;
}
static HRESULT WINAPI ld_scandir(IDirectMusicLoader8 *i, REFGUID c, WCHAR *ext, WCHAR *name) { return S_FALSE; }
static HRESULT WINAPI ld_cacheobj(IDirectMusicLoader8 *i, IDirectMusicObject *o) { return S_OK; }
static HRESULT WINAPI ld_releaseobj(IDirectMusicLoader8 *i, IDirectMusicObject *o)
{
    rm_loader *l = ldr(i);
    for (int k = 0; k < l->nce; k++) {
        IUnknown *u = NULL;
        if (l->ce[k].obj && SUCCEEDED(IUnknown_QueryInterface(l->ce[k].obj, &IID_IDirectMusicObject, (void **)&u))) {
            IUnknown_Release(u);
            if (u == (IUnknown *)o) {
                IUnknown_Release(l->ce[k].obj);
                l->ce[k] = l->ce[--l->nce];
                return S_OK;
            }
        }
    }
    return S_FALSE;
}
static HRESULT WINAPI ld_clearcache(IDirectMusicLoader8 *i, REFGUID c) { ld_clear(ldr(i)); return S_OK; }
static HRESULT WINAPI ld_enablecache(IDirectMusicLoader8 *i, REFGUID c, BOOL on)
{
    rm_loader *l = ldr(i);
    BOOL was = l->cache;
    l->cache = on;
    if (!on) ld_clear(l);
    return was == on ? S_FALSE : S_OK;
}
static HRESULT WINAPI ld_enum(IDirectMusicLoader8 *i, REFGUID c, DWORD n, LPDMUS_OBJECTDESC d) { return S_FALSE; }
static void WINAPI ld_gc(IDirectMusicLoader8 *i) {}
static HRESULT WINAPI ld_releasebyunk(IDirectMusicLoader8 *i, IUnknown *o)
{
    IDirectMusicObject *obj;
    if (!o || FAILED(IUnknown_QueryInterface(o, &IID_IDirectMusicObject, (void **)&obj))) return E_POINTER;
    HRESULT hr = ld_releaseobj(i, obj);
    IDirectMusicObject_Release(obj);
    return hr;
}
static HRESULT WINAPI ld_loadfile(IDirectMusicLoader8 *i, REFGUID cls, REFIID iid, WCHAR *file, void **out)
{
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_FILENAME;
    d.guidClass = *cls;
    if (!file) return E_POINTER;
    lstrcpynW(d.wszFileName, file, DMUS_MAX_FILENAME);
    if (file[0] && (file[1] == L':' || file[0] == L'\\')) d.dwValidData |= DMUS_OBJ_FULLPATH;
    return ld_getobject(i, &d, iid, out);
}
static IDirectMusicLoader8Vtbl ld_vtbl = { ld_qi, ld_addref, ld_release, ld_getobject, ld_setobject, ld_setdir, ld_scandir,
    ld_cacheobj, ld_releaseobj, ld_clearcache, ld_enablecache, ld_enum, ld_gc, ld_releasebyunk, ld_loadfile };

/* ------------------------------------------------------------------ class creation */
HRESULT rm_create(REFCLSID clsid, REFIID riid, void **out)
{
    char b1[64], b2[64];
    LOG_CALL("music: create %s (%s)", guidstr(clsid, b1), guidstr(riid, b2));
    *out = NULL;
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicLoader)) {
        rm_loader *l = calloc(1, sizeof(*l));
        if (!l) return E_OUTOFMEMORY;
        l->IDirectMusicLoader8_iface.lpVtbl = &ld_vtbl;
        l->ref = 1;
        l->cache = TRUE;
        HRESULT hr = ld_qi(&l->IDirectMusicLoader8_iface, riid, out);
        ld_release(&l->IDirectMusicLoader8_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicPerformance)) {
        rm_perf *p = calloc(1, sizeof(*p));
        if (!p) return E_OUTOFMEMORY;
        p->IDirectMusicPerformance8_iface.lpVtbl = &pf_vtbl;
        p->ref = 1;
        HRESULT hr = pf_qi(&p->IDirectMusicPerformance8_iface, riid, out);
        pf_release(&p->IDirectMusicPerformance8_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicSegment)) {
        rm_segment *s = new_segment(NULL);
        if (!s) return E_OUTOFMEMORY;
        HRESULT hr = seg_qi(&s->IDirectMusicSegment8_iface, riid, out);
        seg_release(&s->IDirectMusicSegment8_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusic)) {
        IDirectMusic8 *d = new_dmusic();
        if (!d) return E_OUTOFMEMORY;
        HRESULT hr = dmu_qi(d, riid, out);
        IDirectMusic8_Release(d);
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

BOOL rm_handles(REFCLSID clsid)
{
    return IsEqualCLSID(clsid, &CLSID_DirectMusicLoader) || IsEqualCLSID(clsid, &CLSID_DirectMusicPerformance) ||
           IsEqualCLSID(clsid, &CLSID_DirectMusicSegment) || IsEqualCLSID(clsid, &CLSID_DirectMusic);
}
