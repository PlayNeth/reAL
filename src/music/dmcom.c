/* reDM: the DirectMusic COM objects a game creates (dmloader/dmime/dmusic classes), backed by the ported
 * performance engine.  Covers what DirectMusic games built on segments use: the loader (files, memory, streams, the
 * default GM collection), the performance (Init/InitAudio, standard audiopaths, PlaySegment(Ex), Stop(Ex), IsPlaying,
 * time conversion, global and track parameters, messages and notifications), segments, segment states and audiopaths
 * (Activate, SetVolume), and tool graphs with application tools (the graph object itself is dmgraph.c).  Ports,
 * styles and scripts are not implemented. */
#include "../real.h"
#include <dmusici.h>
#include <string.h>
#include <stdio.h>
#include "dmhost.h"
#include "dmcom.h"
#include "dmgraph.h"
#include "dmapcfg.h"
#include "dmport.h"
#include "dmtheory.h"
#include "dmcomp.h"
#include <dmplugin.h>
#include <mmreg.h>
#include <msacm.h>

/* methods reDM does not implement: logged, so a game that relies on one shows up in reAL.log */
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
typedef struct rm_track rm_track;

typedef struct {
    IDirectMusicSegment8 IDirectMusicSegment8_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dm_segment *seg;
    DMUS_OBJECTDESC desc;
    rm_loader *loader;                      /* for references while loading from a stream (weak) */
    IDirectMusicGraph *graph;               /* +0x44 SetGraph (a segment state starts with a clone of it) */
} rm_segment;

typedef struct {
    IDirectMusicSegmentState8 IDirectMusicSegmentState8_iface;
    IDirectMusicGraph IDirectMusicGraph_iface;  /* +0x04 its StampPMsg: the state's graph, then the path's */
    LONG ref;
    dm_segstate *ss;
    rm_segment *seg;                        /* may be NULL (a state first met in a notification) */
    rm_perf *perf;
    BOOL lazy;                              /* made for a notification: holds an ordinary reference */
    IDirectMusicAudioPath *path;            /* the audiopath it plays on (the default one when none was given) */
    int xref;                               /* further engine references it drops when it goes */
} rm_state;

typedef struct {
    IDirectMusicAudioPath IDirectMusicAudioPath_iface;
    IDirectMusicGraph IDirectMusicGraph_iface;  /* +0x04 its StampPMsg: the path's graph, then the performance's */
    LONG ref;
    dm_audiopath *ap;
    rm_perf *perf;
    DWORD type, npch;
    int buf[2], nbuf;                       /* the host's sink buffers, in the path's buffer order */
    /* a path made from a configuration (77320f10): its buffers (+0x58 the top-level ones, +0x54 the ports') */
    BOOL cfg;
    struct rm_bref *top; DWORD ntop;
    struct rm_pcfg *ports; DWORD nports;
    struct rm_buf **held; DWORD nheld;      /* every buffer reference the path holds */
} rm_path;

/* the performance's buffers (dmime's buffer manager, perf +0x90): made from configurations, shared by GUID */
typedef struct rm_buf { struct rm_buf *next; GUID guid; DWORD flags; int host; LONG refs; } rm_buf;
typedef struct rm_bref { rm_buf *b; DWORD flags; } rm_bref;    /* a path's reference (0x24): the flags as they were */
typedef struct { DWORD base, count, nbuf; rm_buf **b; } rm_pmap;
typedef struct rm_pcfg { GUID port; DWORD base, count; rm_bref *bufs; DWORD nbufs; rm_pmap *maps; DWORD nmaps; } rm_pcfg;

struct rm_perf {
    IDirectMusicPerformance8 IDirectMusicPerformance8_iface;
    IDirectMusicTool IDirectMusicTool_iface;    /* +0x04 the output tool */
    IDirectMusicGraph IDirectMusicGraph_iface;  /* +0x08 its StampPMsg: the application's graph, then the output tool */
    LONG ref;
    dm_host *host;
    DSFXWavesReverb revparams;              /* the shared reverb's parameters as last set */
    rm_path *defpath;
    DWORD audioflags;                       /* InitAudio's (DMUS_AUDIOF_*) */
    rm_port *port;                          /* the synth port (dmport.c) */
    rm_buf *bufs;
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

typedef struct {
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dm_wave *w;
    DMUS_OBJECTDESC desc;
} rm_wave;

typedef struct { IDirectMusic8 IDirectMusic8_iface; LONG ref; } rm_dmusic;
typedef struct { IReferenceClock IReferenceClock_iface; LONG ref; } rm_clock;

typedef struct {                            /* the loader's object cache */
    WCHAR path[MAX_PATH];
    GUID guid;
    BOOL hasguid;
    IUnknown *obj;                          /* rm_segment, rm_collection or rm_wave */
} cache_entry;

static struct rm_loader *g_styleloader;     /* the loader a stream Load resolves references with (the newest) */
struct rm_loader {
    IDirectMusicLoader8 IDirectMusicLoader8_iface;
    LONG ref;
    WCHAR dir[MAX_PATH];
    BOOL cache;
    cache_entry *ce; int nce;
};

static const GUID RM_GUID_DefaultGMCollection = { 0xf17e8673, 0xc3b4, 0x11d1, { 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID RM_CLSID_DirectSoundWave = { 0x8a667154, 0xf9cb, 0x11d2, { 0xad, 0x8a, 0x00, 0x60, 0xb0, 0x57, 0x5a, 0xbc } };

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
    lstrcpyW(c->wszDescription, L"reDM Synthesizer");
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
    LOG_CALL("music: Collection::Release(%p) -> %ld", (void *)i, (long)r);
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
static rm_collection *col_of(IUnknown *u)
{
    IDirectMusicCollection *c = (IDirectMusicCollection *)u;
    return c && c->lpVtbl == &col_vtbl ? col(c) : NULL;
}

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
static dm_host *any_host(void);
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
    LOG_CALL("music: Segment::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        if (s->seg && s->seg->wrapper == s) s->seg->wrapper = NULL;
        dm_segment_release(s->seg);
        if (s->graph) IDirectMusicGraph_Release(s->graph);
        free(s);
    }
    return r;
}
static HRESULT WINAPI seg_getlength(IDirectMusicSegment8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; *t = sgm(i)->seg ? sgm(i)->seg->length : 0; return S_OK; }
static HRESULT WINAPI seg_setlength(IDirectMusicSegment8 *i, MUSIC_TIME t) { if (t <= 0) return E_INVALIDARG; if (sgm(i)->seg) sgm(i)->seg->length = t; return S_OK; }
static HRESULT WINAPI seg_getrepeats(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; *r = sgm(i)->seg ? sgm(i)->seg->repeats : 0; return S_OK; }
static HRESULT WINAPI seg_setrepeats(IDirectMusicSegment8 *i, DWORD r) { if (sgm(i)->seg) sgm(i)->seg->repeats = r; return S_OK; }
static HRESULT WINAPI seg_getres(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; *r = sgm(i)->seg ? sgm(i)->seg->resolution : 0; return S_OK; }
static HRESULT WINAPI seg_setres(IDirectMusicSegment8 *i, DWORD r) { if (sgm(i)->seg) sgm(i)->seg->resolution = r; return S_OK; }
static HRESULT WINAPI seg_gettrack(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, IDirectMusicTrack **t);
static HRESULT WINAPI seg_gettrackgroup(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD *g);
static HRESULT WINAPI seg_inserttrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD g);
static HRESULT WINAPI seg_removetrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t);
static HRESULT WINAPI seg_initplay(IDirectMusicSegment8 *i, IDirectMusicSegmentState **s, IDirectMusicPerformance *p, DWORD f) { return rm_notimpl(__func__); }
/* 77311edc GetGraph, 77311f48 SetGraph: a segment state starts with a clone of the graph (tops_state_graph) */
static HRESULT WINAPI seg_getgraph(IDirectMusicSegment8 *i, IDirectMusicGraph **g)
{
    if (IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    rm_segment *s = sgm(i);
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    IDirectMusicGraph *x = s->graph;
    if (x) { *g = x; IDirectMusicGraph_AddRef(x); }
    if (h) dm_host_unlock(h);
    return x ? S_OK : DMUS_E_NOT_FOUND;
}
static HRESULT WINAPI seg_setgraph(IDirectMusicSegment8 *i, IDirectMusicGraph *g)
{
    if (g && (IsBadReadPtr(g, 4) || IsBadReadPtr(*(void **)g, 4) || IsBadCodePtr(**(FARPROC **)g))) return E_POINTER;
    rm_segment *s = sgm(i);
    if (g) IDirectMusicGraph_AddRef(g);
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    IDirectMusicGraph *o = s->graph;
    s->graph = g;
    if (h) dm_host_unlock(h);
    if (o) IDirectMusicGraph_Release(o);
    return S_OK;
}
/* the segment's notification types are read by the performance thread: changed under the performance lock */
static rm_perf *g_lastperf;
static HRESULT seg_notif(IDirectMusicSegment8 *i, REFGUID g, int add)
{
    rm_segment *s = sgm(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!s->seg) return S_OK;
    dm_host *h = g_lastperf ? g_lastperf->host : NULL;
    if (h) dm_host_lock(h);
    int hr = add ? dm_segment_add_notification(s->seg, (const dm_guid *)g, 0) : dm_segment_remove_notification(s->seg, (const dm_guid *)g, 0);
    if (h) dm_host_unlock(h);
    return hr;
}
static HRESULT WINAPI seg_addnotif(IDirectMusicSegment8 *i, REFGUID g) { return seg_notif(i, g, 1); }
static HRESULT WINAPI seg_remnotif(IDirectMusicSegment8 *i, REFGUID g) { return seg_notif(i, g, 0); }
/* ------------------------------------------------------------------ Get/SetParam */
#define DMG(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
DMG(G_TempoParam, 0xd2ac28a5, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_TimeSignature, 0xd2ac28a4, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_EnableTempo, 0x45fc707e, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_DisableTempo, 0x45fc707d, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_EnableTimeSig, 0x45fc707c, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_DisableTimeSig, 0x45fc707b, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_Download, 0xd2ac28a7, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_Unload, 0xd2ac28a8, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_DownloadToAudioPath, 0x9f2c0341, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DMG(G_UnloadFromAudioPath, 0x9f2c0342, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DMG(G_EnableAutoDownload, 0xd2ac28a9, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_DisableAutoDownload, 0xd2ac28aa, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_ClearAllBands, 0xd2ac28ab, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_IDirectMusicBand, 0xd2ac28ac, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_BandParam, 0x02bb1938, 0xcb8b, 0x11d2, 0x8b, 0xb9, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6);
DMG(G_StandardMIDIFile, 0x06621075, 0xe92e, 0x11d1, 0xa8, 0xc5, 0x00, 0xc0, 0x4f, 0xa3, 0x72, 0x6e);
DMG(G_ConnectToDLSCollection, 0x1db1ae6b, 0xe92e, 0x11d1, 0xa8, 0xc5, 0x00, 0xc0, 0x4f, 0xa3, 0x72, 0x6e);
DMG(G_MuteParam, 0xd2ac28af, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_ChordParam, 0xd2ac289e, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_RhythmParam, 0xd2ac289f, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_CommandParam, 0xd2ac289d, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_CommandParam2, 0x28f97ef7, 0x9538, 0x11d2, 0x97, 0xa9, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DMG(G_CommandParamNext, 0x472afe7a, 0x281b, 0x11d3, 0x81, 0x7d, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DMG(G_IDirectMusicStyle, 0xd2ac28a1, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_SeedVariations, 0x65b76fa5, 0xff37, 0x11d2, 0x81, 0x4e, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DMG(G_StyleTimeSig, 0x76612507, 0x4f37, 0x4b35, 0x80, 0x92, 0x50, 0x48, 0x4e, 0xd4, 0xba, 0x92);
DMG(G_ValidStartTime, 0x7f6b1760, 0x1fdb, 0x11d3, 0x82, 0x26, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DMG(G_PlayMarker, 0xd8761a41, 0x801a, 0x11d3, 0x9b, 0xd1, 0xda, 0xf7, 0xe1, 0xc3, 0xd8, 0x34);
DMG(G_IDirectMusicChordMap_, 0xd2ac28ad, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_PerfMasterGrooveLevel, 0xd2ac28b2, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_PrivateTempo, 0xe8dbd832, 0xbcf0, 0x4c8c, 0xa0, 0x75, 0xa3, 0xf1, 0x5e, 0x67, 0xfd, 0x63);
static const struct { const GUID *g; int prm; } s_params[] = {
    { &G_TempoParam, PRM_TEMPO }, { &G_TimeSignature, PRM_TIMESIG }, { &G_EnableTempo, PRM_ENABLETEMPO },
    { &G_DisableTempo, PRM_DISABLETEMPO }, { &G_EnableTimeSig, PRM_ENABLETIMESIG }, { &G_DisableTimeSig, PRM_DISABLETIMESIG },
    { &G_Download, PRM_DOWNLOAD }, { &G_Unload, PRM_UNLOAD }, { &G_DownloadToAudioPath, PRM_DOWNLOAD_AP },
    { &G_UnloadFromAudioPath, PRM_UNLOAD_AP }, { &G_EnableAutoDownload, PRM_ENABLE_AUTODL },
    { &G_DisableAutoDownload, PRM_DISABLE_AUTODL }, { &G_ClearAllBands, PRM_CLEARBANDS }, { &G_IDirectMusicBand, PRM_IBAND },
    { &G_BandParam, PRM_BANDPARAM }, { &G_StandardMIDIFile, PRM_SMF }, { &G_ConnectToDLSCollection, PRM_CONNECTDLS },
    { &G_MuteParam, PRM_MUTE }, { &G_PrivateTempo, PRM_PRIVTEMPO }, { &G_ChordParam, PRM_CHORD }, { &G_RhythmParam, PRM_RHYTHM },
    { &G_CommandParam, PRM_COMMAND }, { &G_CommandParam2, PRM_COMMAND2 }, { &G_CommandParamNext, PRM_COMMANDNEXT },
    { &G_IDirectMusicStyle, PRM_STYLE }, { &G_SeedVariations, PRM_SEEDVAR }, { &G_StyleTimeSig, PRM_STYLETS }, { &G_ValidStartTime, PRM_VALIDSTART }, { &G_PlayMarker, PRM_PLAYMARKER }, { &G_IDirectMusicChordMap_, PRM_CHORDMAP }, { &GUID_NULL, PRM_ANY },
};
static int prm_of(REFGUID g)
{
    for (size_t k = 0; k < sizeof(s_params) / sizeof(s_params[0]); k++) if (IsEqualGUID(g, s_params[k].g)) return s_params[k].prm;
    return PRM_OTHER;
}
static int prm_takes_target(int prm)
{
    return prm == PRM_DOWNLOAD || prm == PRM_UNLOAD || prm == PRM_DOWNLOAD_AP || prm == PRM_UNLOAD_AP;
}
static rm_collection *col_of(IUnknown *u);
static struct dms_style *style_engine_of(void *p);     /* dmcomsty.c */
static dm_band *band_engine_of(void *p);
static struct dmc_chordmap *chordmap_engine_of(void *p);     /* dmcomcmp.c */
static void prm_out(int prm, int hr, void *p);
static rm_perf *perf_from_unknown(IUnknown *u);
static rm_path *unsafe_path(IUnknown *u);

/* the engine data a SetParam passes: a performance / audiopath for the download parameters (the host to lock comes
 * from it), a collection for ConnectToDLSCollection; *h = the host to lock (the last performance created when the
 * parameter names none) */
static HRESULT prm_data(int prm, void *p, dm_dltarget *dt, void **data, dm_host **h)
{
    *data = p;
    *h = g_lastperf ? g_lastperf->host : NULL;
    if (prm_takes_target(prm)) {
        if (!p) return E_POINTER;
        rm_perf *pf = perf_from_unknown(p);
        if (!pf || !pf->host) return E_NOINTERFACE;
        rm_path *a = unsafe_path(p);
        dt->pf = dm_host_perf(pf->host); dt->ap = a ? a->ap : NULL;
        *data = dt; *h = pf->host;
    } else if (prm == PRM_CONNECTDLS) {
        if (!p) return E_POINTER;
        rm_collection *c = col_of(p);
        if (!c || !c->c) return E_NOINTERFACE;
        *data = c->c;
    } else if (prm == PRM_STYLE) {
        if (!p) return E_POINTER;
        if (!(*data = style_engine_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_CHORDMAP) {
        if (!p) return E_POINTER;
        if (!(*data = chordmap_engine_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_IBAND) {
        if (!p) return E_POINTER;
        if (!(*data = band_engine_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_BANDPARAM) {
        if (!p || IsBadReadPtr(p, 8)) return E_POINTER;
        const DMUS_BAND_PARAM *bp = p;
        if (!(dt->bp.band = band_engine_of(bp->pBand))) return E_NOINTERFACE;
        dt->bp.physical = bp->mtTimePhysical;
        *data = &dt->bp;
    }
    return S_OK;
}

static HRESULT WINAPI seg_getparam(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *p)
{
    char b[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    int prm = prm_of(g);
    if ((prm == PRM_TEMPO || prm == PRM_TIMESIG) && !p) return E_POINTER;
    dm_host *h = g_lastperf ? g_lastperf->host : NULL;
    mtime_t next = nx ? *nx : 0;      /* untouched when no track answers */
    if (h) dm_host_lock(h);
    int hr = dm_segment_getparam(sgm(i)->seg, prm, gb, n, t, nx ? &next : NULL, p);
    if (h) dm_host_unlock(h);
    prm_out(prm, hr, p);
    if (nx) *nx = next;
    LOG_CALL("music: Segment::GetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, hr);
    return hr;
}
static HRESULT WINAPI seg_setparam(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, void *p)
{
    char b[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    int prm = prm_of(g);
    dm_dltarget dt; void *data; dm_host *h;
    HRESULT hr = prm_data(prm, p, &dt, &data, &h);
    if (FAILED(hr)) return hr;
    if (h) dm_host_lock(h);
    hr = dm_segment_setparam(sgm(i)->seg, prm, gb, n, t, data);
    if (h) dm_host_unlock(h);
    if (prm == PRM_OTHER) LOG_INFO("music: Segment::SetParam(%s) -> %08x (not supported)", guidstr(g, b), (unsigned)hr);
    else LOG_CALL("music: Segment::SetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, (unsigned)hr);
    return hr;
}
static HRESULT WINAPI seg_clone(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicSegment **o);
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
static HRESULT WINAPI seg_setpch(IDirectMusicSegment8 *i, DWORD n, DWORD *p)
{
    if (!n) return S_OK;
    if (!p) return E_INVALIDARG;
    if (IsBadReadPtr(p, n * sizeof(DWORD))) return E_POINTER;
    if (!sgm(i)->seg) return S_OK;
    dm_host *h = g_lastperf ? g_lastperf->host : NULL;
    if (h) dm_host_lock(h);
    HRESULT hr = dm_segment_add_pchannels(sgm(i)->seg, (const uint32_t *)p, n);
    if (h) dm_host_unlock(h);
    return hr;
}
static HRESULT WINAPI seg_settrackcfg(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, DWORD on, DWORD off)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    dm_host *h = g_lastperf ? g_lastperf->host : NULL;
    if (h) dm_host_lock(h);
    HRESULT hr = dm_segment_set_trackconfig(sgm(i)->seg, (const dm_guid *)g, gb, n, on, off);
    if (h) dm_host_unlock(h);
    return hr;
}
static HRESULT WINAPI seg_getapconfig(IDirectMusicSegment8 *i, IUnknown **c) { if (c) *c = NULL; return DMUS_E_NO_AUDIOPATH_CONFIG; }
static rm_segment *new_segment(dm_segment *g);
int dmc_segment_compose(dm_segment *s);
/* 773150d0: the composing tracks composed (into a clone of the segment when one is asked for) */
static HRESULT WINAPI seg_compose(IDirectMusicSegment8 *i, MUSIC_TIME t, IDirectMusicSegment *a, IDirectMusicSegment *b, IDirectMusicSegment **c)
{
    if ((a && (IsBadReadPtr(a, 4) || IsBadReadPtr(*(void **)a, 4))) || (b && (IsBadReadPtr(b, 4) || IsBadReadPtr(*(void **)b, 4))) ||
        (c && IsBadWritePtr(c, 4)))
        return E_POINTER;
    rm_segment *s = sgm(i), *fa = a ? unsafe_segment((IUnknown *)a) : NULL, *fb = b ? unsafe_segment((IUnknown *)b) : NULL;
    if ((a && !fa) || (b && !fb)) return rm_notimpl("Segment::Compose from or to a segment not made by reDM");
    if (!s->seg) return DMUS_E_NOT_INIT;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    int hr = DM_OK;
    dm_segment *g = s->seg;
    if (c) { g = dm_segment_clone(s->seg, 0, s->seg->length, &hr); if (!g) hr = E_OUTOFMEMORY; }
    if (g) hr = dmc_segment_compose_ex(g, t, fa ? fa->seg : NULL, fb ? fb->seg : NULL);
    if (h) dm_host_unlock(h);
    if (c && g) {
        rm_segment *o = new_segment(g);
        if (!o) { dm_segment_release(g); return E_OUTOFMEMORY; }
        HRESULT r = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)c);
        seg_release(&o->IDirectMusicSegment8_iface);
        if (FAILED(r)) return r;
    }
    LOG_CALL("music: Segment::Compose -> %08x", hr);
    return hr;
}
static rm_perf *perf_from_unknown(IUnknown *u);
static rm_path *unsafe_path(IUnknown *u);
/* 77312338 / 773123e7: SetParam(GUID_DownloadToAudioPath / GUID_UnloadFromAudioPath) on every track; a segment
 * without such tracks succeeds */
static HRESULT WINAPI seg_download(IDirectMusicSegment8 *i, IUnknown *target)
{
    if (!target) return E_POINTER;
    HRESULT hr = seg_setparam(i, &G_DownloadToAudioPath, 0xffffffff, DM_SEG_ALLTRACKS, 0, target);
    if (hr == DMUS_E_TRACK_NOT_FOUND) hr = S_OK;
    LOG_CALL("music: Segment::Download -> %08x", (unsigned)hr);
    return hr;
}
static HRESULT WINAPI seg_unload(IDirectMusicSegment8 *i, IUnknown *target)
{
    if (!target) return E_POINTER;
    HRESULT hr = seg_setparam(i, &G_UnloadFromAudioPath, 0xffffffff, DM_SEG_ALLTRACKS, 0, target);
    return hr == DMUS_E_TRACK_NOT_FOUND ? S_OK : hr;
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
static dm_wave *loader_getwave(void *ctx, const dm_guid *g, const char *file, const char *name);
static struct dms_style *loader_getstyle(void *ctx, const dm_guid *g, const char *file, const char *name);
static struct dmc_chordmap *loader_getcmap(void *ctx, const dm_guid *g, const char *file, const char *name);
static dm_segment *segment_from_data(const BYTE *b, DWORD n, rm_loader *l)
{
    if (dm_is_midi(b, n)) return dm_segment_from_midi(b, n, loader_getcoll, l);
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {      /* a wave as a segment */
        dm_wave *w = dm_wave_load(b, n);
        if (!w) return NULL;
        dm_segment *g = dm_segment_from_wave(w);
        dm_wave_release(w);
        return g;
    }
    dm_loadcb cb = { loader_getcoll, loader_getwave, loader_getstyle, l, loader_getcmap };
    return dm_segment_load_cb(b, n, &cb);
}
static rm_segment *sgp(IPersistStream *i) { return impl_from(i, rm_segment, IPersistStream_iface); }
static HRESULT WINAPI sgp_qi(IPersistStream *i, REFIID r, void **o) { return seg_qi(&sgp(i)->IDirectMusicSegment8_iface, r, o); }
static ULONG WINAPI sgp_addref(IPersistStream *i) { return seg_addref(&sgp(i)->IDirectMusicSegment8_iface); }
static ULONG WINAPI sgp_release(IPersistStream *i) { return seg_release(&sgp(i)->IDirectMusicSegment8_iface); }
static HRESULT WINAPI sgp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = CLSID_DirectMusicSegment; return S_OK; }
static HRESULT WINAPI sgp_isdirty(IPersistStream *i) { return S_FALSE; }
/* 77314148 / 77312ab9: a segment file's tool graph (a 'DMTG' form in the 'DMSG' form): a new graph loads it (its
 * tools are created by class, so they must be registered) and becomes the segment's; if it fails, so does the
 * segment's Load */
static IDirectMusicGraph *segment_graph_from_data(const BYTE *b, DWORD n, HRESULT *res)
{
    *res = S_OK;
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DMSG", 4)) return NULL;
    DWORD end = 8 + *(const DWORD *)(b + 4);
    if (end > n) end = n;
    for (DWORD off = 12; off + 12 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4);
        if (!memcmp(b + off, "RIFF", 4) && !memcmp(b + off + 8, "DMTG", 4) && off + 8 + sz <= end) {
            IDirectMusicGraph *g = NULL;
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, 8 + sz);
            IStream *st = NULL;
            if (!hg || FAILED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&g))) { if (hg) GlobalFree(hg); return NULL; }
            memcpy(GlobalLock(hg), b + off, 8 + sz);
            GlobalUnlock(hg);
            if (SUCCEEDED(CreateStreamOnHGlobal(hg, TRUE, &st))) {
                HRESULT hr = rm_graph_load(g, st);
                if (FAILED(hr)) LOG_INFO("music: a segment's tool graph: Load %08lx", hr);
                *res = hr;
                IStream_Release(st);
            } else GlobalFree(hg);
            return g;
        }
        off += 8 + sz + (sz & 1);
    }
    return NULL;
}
static HRESULT WINAPI sgp_load(IPersistStream *i, IStream *st)
{
    rm_segment *s = sgp(i);
    BYTE *b; DWORD n;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    dm_segment *g = segment_from_data(b, n, s->loader);
    HRESULT ghr;
    IDirectMusicGraph *gr = g ? segment_graph_from_data(b, n, &ghr) : NULL;
    free(b);
    if (!g) return DMUS_E_UNSUPPORTED_STREAM;
    if (gr && FAILED(ghr)) { IDirectMusicGraph_Release(gr); dm_segment_release(g); return ghr; }
    if (s->seg) { if (s->seg->wrapper == s) s->seg->wrapper = NULL; dm_segment_release(s->seg); }
    s->seg = g;
    g->wrapper = s;
    if (gr) { if (s->graph) IDirectMusicGraph_Release(s->graph); s->graph = gr; }
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
    if (g) g->wrapper = s;
    s->desc.dwSize = sizeof(s->desc);
    s->desc.dwValidData = DMUS_OBJ_CLASS | (g ? DMUS_OBJ_LOADED : 0);
    s->desc.guidClass = CLSID_DirectMusicSegment;
    if (g && g->name[0]) { MultiByteToWideChar(CP_ACP, 0, g->name, -1, s->desc.wszName, DMUS_MAX_NAME); s->desc.dwValidData |= DMUS_OBJ_NAME; }
    return s;
}


/* ------------------------------------------------------------------ waves (dswave CDirectSoundWave): IDirectMusicObject
 * and IPersistStream; segments and wave tracks take the dm_wave inside */
static rm_wave *wvo(IDirectMusicObject *i) { return impl_from(i, rm_wave, IDirectMusicObject_iface); }
static rm_wave *wvp(IPersistStream *i) { return impl_from(i, rm_wave, IPersistStream_iface); }
static IDirectMusicObjectVtbl wvo_vtbl;
static HRESULT WINAPI wvo_qi(IDirectMusicObject *i, REFIID riid, void **out)
{
    rm_wave *w = wvo(i);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &w->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &w->IPersistStream_iface;
    else return rm_noiface("Wave::QueryInterface", riid, out);
    InterlockedIncrement(&w->ref);
    return S_OK;
}
static ULONG WINAPI wvo_addref(IDirectMusicObject *i) { return InterlockedIncrement(&wvo(i)->ref); }
static ULONG WINAPI wvo_release(IDirectMusicObject *i)
{
    rm_wave *w = wvo(i);
    LONG r = InterlockedDecrement(&w->ref);
    if (!r) { if (w->w) { if (w->w->wrapper == w) w->w->wrapper = NULL; dm_wave_release(w->w); } free(w); }
    return r;
}
/* 6c782466 */
static HRESULT WINAPI wvo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    if (!d) return E_POINTER;
    DWORD sz = d->dwSize;
    memcpy(d, &wvo(i)->desc, sz < sizeof(*d) ? sz : sizeof(*d));
    d->dwSize = sz;
    return S_OK;
}
static HRESULT WINAPI wvo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    rm_wave *w = wvo(i);
    if (!d) return E_POINTER;
    if (d->dwValidData & DMUS_OBJ_OBJECT) w->desc.guidObject = d->guidObject;
    if (d->dwValidData & DMUS_OBJ_NAME) memcpy(w->desc.wszName, d->wszName, sizeof(d->wszName));
    if (d->dwValidData & DMUS_OBJ_VERSION) w->desc.vVersion = d->vVersion;
    w->desc.dwValidData |= d->dwValidData & (DMUS_OBJ_OBJECT | DMUS_OBJ_NAME | DMUS_OBJ_VERSION);
    return (d->dwValidData & ~(DMUS_OBJ_OBJECT | DMUS_OBJ_NAME | DMUS_OBJ_VERSION | DMUS_OBJ_CLASS)) ? S_FALSE : S_OK;
}
static HRESULT WINAPI wvo_parse(IDirectMusicObject *i, LPSTREAM st, LPDMUS_OBJECTDESC d)
{
    BYTE *b; DWORD n;
    if (!st || !d) return E_POINTER;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    dm_wave *w = dm_wave_load(b, n);
    free(b);
    if (!w) return DMUS_E_UNSUPPORTED_STREAM;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwValidData = DMUS_OBJ_CLASS;
    d->guidClass = RM_CLSID_DirectSoundWave;
    if (w->hasguid) { memcpy(&d->guidObject, &w->guid, sizeof(GUID)); d->dwValidData |= DMUS_OBJ_OBJECT; }
    if (w->name[0]) { MultiByteToWideChar(CP_ACP, 0, w->name, -1, d->wszName, DMUS_MAX_NAME); d->dwValidData |= DMUS_OBJ_NAME; }
    if (w->hasvers) { d->vVersion.dwVersionMS = w->vers[0]; d->vVersion.dwVersionLS = w->vers[1]; d->dwValidData |= DMUS_OBJ_VERSION; }
    dm_wave_release(w);
    return S_OK;
}
static IDirectMusicObjectVtbl wvo_vtbl = { wvo_qi, wvo_addref, wvo_release, wvo_getdesc, wvo_setdesc, wvo_parse };
static HRESULT WINAPI wvp_qi(IPersistStream *i, REFIID r, void **o) { return wvo_qi(&wvp(i)->IDirectMusicObject_iface, r, o); }
static ULONG WINAPI wvp_addref(IPersistStream *i) { return wvo_addref(&wvp(i)->IDirectMusicObject_iface); }
static ULONG WINAPI wvp_release(IPersistStream *i) { return wvo_release(&wvp(i)->IDirectMusicObject_iface); }
static HRESULT WINAPI wvp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = RM_CLSID_DirectSoundWave; return S_OK; }
static HRESULT WINAPI wvp_isdirty(IPersistStream *i) { return S_FALSE; }
static void wave_desc(rm_wave *o)
{
    dm_wave *w = o->w;
    o->desc.dwValidData |= DMUS_OBJ_LOADED;
    if (w->hasguid) { memcpy(&o->desc.guidObject, &w->guid, sizeof(GUID)); o->desc.dwValidData |= DMUS_OBJ_OBJECT; }
    if (w->name[0]) { MultiByteToWideChar(CP_ACP, 0, w->name, -1, o->desc.wszName, DMUS_MAX_NAME); o->desc.dwValidData |= DMUS_OBJ_NAME; }
    if (w->hasvers) { o->desc.vVersion.dwVersionMS = w->vers[0]; o->desc.vVersion.dwVersionLS = w->vers[1]; o->desc.dwValidData |= DMUS_OBJ_VERSION; }
}
static HRESULT WINAPI wvp_load(IPersistStream *i, IStream *st)
{
    rm_wave *o = wvp(i);
    BYTE *b; DWORD n;
    if (!st) return E_INVALIDARG;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    dm_wave *w = dm_wave_load(b, n);
    free(b);
    if (!w) return E_INVALIDARG;
    if (o->w) { if (o->w->wrapper == o) o->w->wrapper = NULL; dm_wave_release(o->w); }
    o->w = w;
    w->wrapper = o;
    wave_desc(o);
    return S_OK;
}
static HRESULT WINAPI wvp_save(IPersistStream *i, IStream *s, BOOL c) { return rm_notimpl(__func__); }
static HRESULT WINAPI wvp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl wvp_vtbl = { wvp_qi, wvp_addref, wvp_release, wvp_getclass, wvp_isdirty, wvp_load, wvp_save, wvp_getsize };

static rm_wave *new_wave(dm_wave *w)
{
    rm_wave *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicObject_iface.lpVtbl = &wvo_vtbl;
    o->IPersistStream_iface.lpVtbl = &wvp_vtbl;
    o->ref = 1;
    o->desc.dwSize = sizeof(o->desc);
    o->desc.dwValidData = DMUS_OBJ_CLASS;
    o->desc.guidClass = RM_CLSID_DirectSoundWave;
    if (w) { o->w = w; w->wrapper = o; wave_desc(o); }
    return o;
}

/* compressed waves (dswave decodes through ACM: 6c783750): the whole file to PCM, in the format 'wavu' named or
 * acmFormatSuggest picks */
static int acm_decode(const uint8_t *wfx, uint32_t wfxlen, const uint8_t *in, uint32_t inlen, uint8_t **out,
                      uint32_t *outlen, uint16_t *ch, uint32_t *rate, uint16_t *bits)
{
    uint32_t need = wfxlen < 18 ? 18 : wfxlen;
    WAVEFORMATEX *src = calloc(1, need + 64);
    if (!src) return 0;
    memcpy(src, wfx, wfxlen);
    if (wfxlen < 18) src->cbSize = 0;
    WAVEFORMATEX dst;
    memset(&dst, 0, sizeof(dst));
    dst.wFormatTag = WAVE_FORMAT_PCM;
    if (*ch) {
        dst.nChannels = *ch; dst.nSamplesPerSec = *rate; dst.wBitsPerSample = *bits;
        dst.nBlockAlign = (WORD)(dst.nChannels * dst.wBitsPerSample / 8);
        dst.nAvgBytesPerSec = dst.nSamplesPerSec * dst.nBlockAlign;
    } else if (acmFormatSuggest(NULL, src, &dst, sizeof(dst), ACM_FORMATSUGGESTF_WFORMATTAG)) { free(src); return 0; }
    HACMSTREAM has;
    if (acmStreamOpen(&has, NULL, src, &dst, NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME)) { free(src); return 0; }
    DWORD osize = 0;
    if (acmStreamSize(has, inlen, &osize, ACM_STREAMSIZEF_SOURCE) || !osize) { acmStreamClose(has, 0); free(src); return 0; }
    uint8_t *ib = malloc(inlen ? inlen : 1), *ob = malloc(osize + dst.nBlockAlign);
    if (!ib || !ob) { free(ib); free(ob); acmStreamClose(has, 0); free(src); return 0; }
    memcpy(ib, in, inlen);
    ACMSTREAMHEADER hd;
    memset(&hd, 0, sizeof(hd));
    hd.cbStruct = sizeof(hd); hd.pbSrc = ib; hd.cbSrcLength = inlen; hd.pbDst = ob; hd.cbDstLength = osize;
    int ok = 0;
    if (!acmStreamPrepareHeader(has, &hd, 0)) {
        ok = !acmStreamConvert(has, &hd, ACM_STREAMCONVERTF_BLOCKALIGN | ACM_STREAMCONVERTF_START | ACM_STREAMCONVERTF_END);
        acmStreamUnprepareHeader(has, &hd, 0);
    }
    acmStreamClose(has, 0);
    free(ib); free(src);
    if (!ok) { free(ob); return 0; }
    *out = ob; *outlen = hd.cbDstLengthUsed;
    *ch = dst.nChannels; *rate = dst.nSamplesPerSec; *bits = dst.wBitsPerSample;
    return 1;
}


/* ------------------------------------------------------------------ tracks (GetTrack / InsertTrack / Clone) */
struct rm_track {
    IDirectMusicTrack8 IDirectMusicTrack8_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dm_track *t;
};
static IDirectMusicTrack8Vtbl trk_vtbl;
static IPersistStreamVtbl trp_vtbl;
static rm_track *trk(IDirectMusicTrack8 *i) { return impl_from(i, rm_track, IDirectMusicTrack8_iface); }
static dm_host *any_host(void) { return g_lastperf ? g_lastperf->host : NULL; }

/* the COM object of a track (one per track, made on demand) */
static IDirectMusicTrack8 *track_object(dm_track *t)
{
    rm_track *r = t->wrapper;
    if (r) { InterlockedIncrement(&r->ref); return &r->IDirectMusicTrack8_iface; }
    if (!(r = calloc(1, sizeof(*r)))) return NULL;
    r->IDirectMusicTrack8_iface.lpVtbl = &trk_vtbl;
    r->IPersistStream_iface.lpVtbl = &trp_vtbl;
    r->ref = 1; r->t = t;
    dm_track_addref(t);
    t->wrapper = r;
    return &r->IDirectMusicTrack8_iface;
}
static rm_track *unsafe_track(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicTrack8 *t = (IDirectMusicTrack8 *)u;
    if (t->lpVtbl == &trk_vtbl) return trk(t);
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicTrack8, (void **)&t))) {
        IDirectMusicTrack8_Release(t);
        if (t->lpVtbl == &trk_vtbl) return trk(t);
    }
    return NULL;
}
static HRESULT WINAPI trk_qi(IDirectMusicTrack8 *i, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    /* dmime's tracks: IDirectMusicTrack8 except the time signature and marker tracks' (7731cc26); IPersist only on dmband's */
    int kind = trk(i)->t->kind;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicTrack) ||
        (IsEqualIID(riid, &IID_IDirectMusicTrack8) && kind != TRK_TIMESIG && kind != TRK_MARKER)) *out = i;
    else if (IsEqualIID(riid, &IID_IPersistStream) || (IsEqualIID(riid, &IID_IPersist) && kind == TRK_BAND))
        *out = &trk(i)->IPersistStream_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&trk(i)->ref);
    return S_OK;
}
static ULONG WINAPI trk_addref(IDirectMusicTrack8 *i) { return InterlockedIncrement(&trk(i)->ref); }
static ULONG WINAPI trk_release(IDirectMusicTrack8 *i)
{
    rm_track *r = trk(i);
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) {
        dm_host *h = any_host();
        if (h) dm_host_lock(h);
        if (r->t->wrapper == r) r->t->wrapper = NULL;
        dm_track_release(r->t);
        if (h) dm_host_unlock(h);
        free(r);
    }
    return n;
}
static HRESULT WINAPI trk_init(IDirectMusicTrack8 *i, IDirectMusicSegment *s) { return S_OK; }
static HRESULT WINAPI trk_initplay(IDirectMusicTrack8 *i, IDirectMusicSegmentState *ss, IDirectMusicPerformance *p, void **d, DWORD id, DWORD f) { return rm_notimpl(__func__); }
static HRESULT WINAPI trk_endplay(IDirectMusicTrack8 *i, void *d) { return rm_notimpl(__func__); }
static HRESULT WINAPI trk_play(IDirectMusicTrack8 *i, void *d, MUSIC_TIME a, MUSIC_TIME b, MUSIC_TIME o, DWORD f, IDirectMusicPerformance *p, IDirectMusicSegmentState *ss, DWORD id) { return rm_notimpl(__func__); }
static HRESULT WINAPI trk_getparam(IDirectMusicTrack8 *i, REFGUID g, MUSIC_TIME t, MUSIC_TIME *nx, void *p)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int prm = prm_of(g);
    mtime_t next = nx ? *nx : 0;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    HRESULT hr = dm_track_getparam(trk(i)->t, prm, t, nx ? &next : NULL, p);
    prm_out(prm, hr, p);
    if (h) dm_host_unlock(h);
    if (nx) *nx = next;
    return hr;
}
static HRESULT WINAPI trk_setparam(IDirectMusicTrack8 *i, REFGUID g, MUSIC_TIME t, void *p)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int prm = prm_of(g);
    dm_dltarget dt; void *data; dm_host *h;
    HRESULT hr = prm_data(prm, p, &dt, &data, &h);
    if (FAILED(hr)) return hr;
    if (h) dm_host_lock(h);
    hr = dm_track_setparam(trk(i)->t, prm, t, data);
    if (h) dm_host_unlock(h);
    return hr;
}
static HRESULT WINAPI trk_isparam(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    return dm_track_isparam(trk(i)->t, prm_of(g));
}
static HRESULT WINAPI trk_addnotif(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    HRESULT hr = dm_track_add_notification(trk(i)->t, (const dm_guid *)g);
    if (h) dm_host_unlock(h);
    return hr;
}
static HRESULT WINAPI trk_remnotif(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    HRESULT hr = dm_track_remove_notification(trk(i)->t, (const dm_guid *)g);
    if (h) dm_host_unlock(h);
    return hr;
}
static HRESULT WINAPI trk_clone(IDirectMusicTrack8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicTrack **o)
{
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    int hr;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    dm_track *c = dm_track_clone(trk(i)->t, a, b, &hr);
    if (h) dm_host_unlock(h);
    if (!c) return hr;
    *o = (IDirectMusicTrack *)track_object(c);
    dm_track_release(c);
    return *o ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI trk_playex(IDirectMusicTrack8 *i, void *d, REFERENCE_TIME a, REFERENCE_TIME b, REFERENCE_TIME o, DWORD f, IDirectMusicPerformance *p, IDirectMusicSegmentState *ss, DWORD id) { return rm_notimpl(__func__); }
/* GetParamEx / SetParamEx (7731bded / 77324130): DMUS_TRACK_PARAMF_CLOCK times are reference time, used in 1/10000 */
static HRESULT WINAPI trk_getparamex(IDirectMusicTrack8 *i, REFGUID g, REFERENCE_TIME t, REFERENCE_TIME *nx, void *p, void *sd, DWORD f)
{
    MUSIC_TIME n = 0;
    HRESULT hr = trk_getparam(i, g, (MUSIC_TIME)((f & 1) ? t / 10000 : t), &n, p);
    if (nx) *nx = (f & 1) ? (REFERENCE_TIME)n * 10000 : n;
    return hr;
}
static HRESULT WINAPI trk_setparamex(IDirectMusicTrack8 *i, REFGUID g, REFERENCE_TIME t, void *p, void *sd, DWORD f)
{
    return trk_setparam(i, g, (MUSIC_TIME)((f & 1) ? t / 10000 : t), p);
}
static HRESULT WINAPI trk_compose(IDirectMusicTrack8 *i, IUnknown *c, DWORD id, IDirectMusicTrack **o) { return E_NOTIMPL; }
/* Join (tempo 7731caa4, chord 77402abe, command 77404040, style 7741305c, band 772027cc, chord map 7728c90c,
 * signpost 7728b802; E_NOTIMPL for the others) */
static rm_segment *unsafe_segment(IUnknown *u);
static HRESULT WINAPI trk_join(IDirectMusicTrack8 *i, IDirectMusicTrack *t, MUSIC_TIME j, IUnknown *c, DWORD id, IDirectMusicTrack **o)
{
    dm_track *me = trk(i)->t;
    switch (me->kind) {
    case TRK_TEMPO: case TRK_CHORD: case TRK_COMMAND: case TRK_STYLE: case TRK_BAND: case TRK_CHORDMAP: case TRK_SIGNPOST: break;
    default: return E_NOTIMPL;
    }
    if (!t || IsBadReadPtr(t, 4) || IsBadReadPtr(*(void **)t, 4) || (o && IsBadWritePtr(o, 4)) ||
        (c && (IsBadReadPtr(c, 4) || IsBadReadPtr(*(void **)c, 4))))
        return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    rm_segment *g = c ? unsafe_segment(c) : NULL;
    if (!r || (c && !g)) return rm_notimpl("Track::Join (a track or segment not made by reDM)");
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    dm_track *res = NULL;
    int hr = dm_track_join(me, r->t, j, g ? g->seg : NULL, id, o ? &res : NULL);
    if (h) dm_host_unlock(h);
    if (o) {
        *o = NULL;
        if (res) { *o = (IDirectMusicTrack *)track_object(res); dm_track_release(res); if (!*o) return E_OUTOFMEMORY; }
    }
    return hr;
}
static IDirectMusicTrack8Vtbl trk_vtbl = { trk_qi, trk_addref, trk_release, trk_init, trk_initplay, trk_endplay, trk_play,
    trk_getparam, trk_setparam, trk_isparam, trk_addnotif, trk_remnotif, trk_clone, trk_playex, trk_getparamex, trk_setparamex,
    trk_compose, trk_join };

static rm_track *trp(IPersistStream *i) { return impl_from(i, rm_track, IPersistStream_iface); }
static HRESULT WINAPI trp_qi(IPersistStream *i, REFIID riid, void **out) { return trk_qi(&trp(i)->IDirectMusicTrack8_iface, riid, out); }
static ULONG WINAPI trp_addref(IPersistStream *i) { return trk_addref(&trp(i)->IDirectMusicTrack8_iface); }
static ULONG WINAPI trp_release(IPersistStream *i) { return trk_release(&trp(i)->IDirectMusicTrack8_iface); }
static HRESULT WINAPI trp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; memcpy(c, &trp(i)->t->clsid, sizeof(*c)); return S_OK; }
static HRESULT WINAPI trp_isdirty(IPersistStream *i) { return S_FALSE; }
static HRESULT WINAPI trp_load(IPersistStream *i, IStream *s) { return rm_notimpl(__func__); }
/* the signpost track's (7728af59: its 'sgnp' chunk); the others are not written */
static HRESULT WINAPI trp_save(IPersistStream *i, IStream *s, BOOL c)
{
    dm_track *t = trp(i)->t;
    if (t->kind != TRK_SIGNPOST) return rm_notimpl(__func__);
    size_t n;
    uint8_t *b = dmc_sptrack_save(t->data, &n);
    if (!b) return E_OUTOFMEMORY;
    ULONG w = 0;
    HRESULT hr = IStream_Write(s, b, (ULONG)n, &w);
    free(b);
    return FAILED(hr) ? hr : w == n ? S_OK : STG_E_MEDIUMFULL;
}
static HRESULT WINAPI trp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl trp_vtbl = { trp_qi, trp_addref, trp_release, trp_getclass, trp_isdirty, trp_load, trp_save, trp_getsize };

/* 77313113 GetTrack: the index-th track of the class (GUID_NULL: any) in the group */
static HRESULT WINAPI seg_gettrack(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, IDirectMusicTrack **t)
{
    if (!t || IsBadWritePtr(t, sizeof(*t))) return E_POINTER;
    dm_segment *s = sgm(i)->seg;
    if (!s || !g) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    static const GUID null_guid;
    for (int k = 0; k < s->ntracks; k++) {
        dm_segtrack *e = &s->tracks[k];
        if (!(e->group & gb) || !(IsEqualGUID(g, &null_guid) || !memcmp(g, &e->t->clsid, sizeof(GUID)))) continue;
        if (n--) continue;
        *t = (IDirectMusicTrack *)track_object(e->t);
        hr = *t ? S_OK : E_OUTOFMEMORY;
        break;
    }
    if (h) dm_host_unlock(h);
    return hr;
}
/* 773119c5 GetTrackGroup */
static HRESULT WINAPI seg_gettrackgroup(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD *g)
{
    if (!t || !g || IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    dm_segment *s = sgm(i)->seg;
    if (!r || !s) return DMUS_E_NOT_FOUND;
    for (int k = 0; k < s->ntracks; k++) if (s->tracks[k].t == r->t) { *g = s->tracks[k].group; return S_OK; }
    return DMUS_E_NOT_FOUND;
}
/* 773131cb / 77311b2c InsertTrack: config DMUS_TRACKCONFIG_DEFAULT, position and priority 0, in track order; the track
 * takes the segment's notification types.  Only reDM's own tracks can go in. */
static HRESULT WINAPI seg_inserttrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD g)
{
    if (!t || IsBadReadPtr(t, sizeof(void *))) return E_POINTER;
    if (!g) return E_INVALIDARG;
    rm_track *r = unsafe_track((IUnknown *)t);
    if (!r) return rm_notimpl("Segment::InsertTrack (a track not made by reDM)");
    dm_segment *s = sgm(i)->seg;
    if (!s) return DMUS_E_NOT_INIT;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    dm_track_addref(r->t);
    dm_segment_insert_track(s, r->t, 0, g, 0x38, 0);
    if (h) dm_host_unlock(h);
    return S_OK;
}
/* 77311c54 RemoveTrack: S_FALSE when the track is not in the segment */
static HRESULT WINAPI seg_removetrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t)
{
    if (!t || IsBadReadPtr(t, sizeof(void *))) return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    dm_segment *s = sgm(i)->seg;
    if (!r || !s) return S_FALSE;
    HRESULT hr = S_FALSE;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    for (int k = 0; k < s->ntracks; k++)
        if (s->tracks[k].t == r->t) {
            dm_track *x = s->tracks[k].t;
            memmove(s->tracks + k, s->tracks + k + 1, (s->ntracks - k - 1) * sizeof(*s->tracks));
            s->ntracks--;
            dm_track_release(x);
            hr = S_OK;
            break;
        }
    if (h) dm_host_unlock(h);
    return hr;
}
/* 77313a27 Clone */
static HRESULT WINAPI seg_clone(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicSegment **o)
{
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    dm_segment *s = sgm(i)->seg;
    if (!s) { *o = NULL; return E_FAIL; }
    int hr;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    dm_segment *c = dm_segment_clone(s, a, b, &hr);
    if (h) dm_host_unlock(h);
    if (!c) { *o = NULL; return hr; }
    rm_segment *n = new_segment(c);
    if (!n) { dm_segment_release(c); *o = NULL; return E_OUTOFMEMORY; }
    if ((n->graph = sgm(i)->graph)) IDirectMusicGraph_AddRef(n->graph);     /* 77313acd: the same graph */
    *o = (IDirectMusicSegment *)&n->IDirectMusicSegment8_iface;
    return hr;
}
/* ------------------------------------------------------------------ segment states */
static IDirectMusicSegmentState8Vtbl st_vtbl;
static IDirectMusicGraphVtbl stg_vtbl;
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
    if (IsEqualIID(riid, &IID_IDirectMusicGraph)) { *out = &sst(i)->IDirectMusicGraph_iface; InterlockedIncrement(&sst(i)->ref); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI st_addref(IDirectMusicSegmentState8 *i) { return InterlockedIncrement(&sst(i)->ref); }
static ULONG WINAPI st_release(IDirectMusicSegmentState8 *i)
{
    rm_state *s = sst(i);
    LONG r = InterlockedDecrement(&s->ref);
    LOG_CALL("music: SegmentState::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        if (s->perf->host) {
            dm_host_lock(s->perf->host);
            void **w = dm_segstate_wrapper(s->ss);
            if (*w == s) *w = NULL;
            if (s->lazy) dm_segstate_unref(s->ss); else dm_segstate_release(s->ss);
            while (s->xref-- > 0) dm_segstate_unref(s->ss);
            dm_host_unlock(s->perf->host);
        }
        if (s->seg) IDirectMusicSegment8_Release(&s->seg->IDirectMusicSegment8_iface);
        if (s->path) IDirectMusicAudioPath_Release(s->path);
        IDirectMusicPerformance8_Release(&s->perf->IDirectMusicPerformance8_iface);
        free(s);
    }
    return r;
}
static HRESULT WINAPI st_getrepeats(IDirectMusicSegmentState8 *i, DWORD *r) { if (!r) return E_POINTER; *r = sst(i)->seg && sst(i)->seg->seg ? sst(i)->seg->seg->repeats : 0; return S_OK; }
static HRESULT WINAPI st_getsegment(IDirectMusicSegmentState8 *i, IDirectMusicSegment **s)
{
    if (!s) return E_POINTER;
    *s = NULL;
    if (!sst(i)->seg) return DMUS_E_NOT_FOUND;
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
static HRESULT WINAPI st_getseek(IDirectMusicSegmentState8 *i, MUSIC_TIME *t)
{
    rm_state *s = sst(i);
    if (!t || IsBadWritePtr(t, sizeof(*t))) return E_POINTER;
    if (s->perf->host) dm_host_lock(s->perf->host);
    *t = dm_segstate_seek(s->ss);
    if (s->perf->host) dm_host_unlock(s->perf->host);
    return S_OK;
}
static HRESULT WINAPI st_getstartpoint(IDirectMusicSegmentState8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; *t = sst(i)->seg && sst(i)->seg->seg ? sst(i)->seg->seg->playstart : 0; return S_OK; }
static HRESULT WINAPI st_settrackcfg(IDirectMusicSegmentState8 *i, REFGUID g, DWORD gb, DWORD n, DWORD on, DWORD off)
{
    rm_state *s = sst(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (s->perf->host) dm_host_lock(s->perf->host);
    HRESULT hr = dm_segstate_set_trackconfig(s->ss, (const dm_guid *)g, gb, n, on, off);
    if (s->perf->host) dm_host_unlock(s->perf->host);
    return hr;
}
/* 773161ad: the segment, its tracks (GUID_All_Objects: any class), the state's graph and its tools (the graph made
 * empty when it has none), the performance with its graph and tools; anything further down the path from the
 * state's audiopath */
static IDirectMusicGraph *perf_graph(rm_perf *p);
static IDirectMusicTrack8 *track_object(dm_track *t);
static HRESULT WINAPI st_getobj(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    rm_state *s = sst(i);
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    *o = NULL;
    if (buf && (stage < DMUS_PATH_BUFFER || stage > 0x7fff)) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    switch (stage) {
    case DMUS_PATH_SEGMENT:
        if (s->seg && !n && !pch) hr = IDirectMusicSegment8_QueryInterface(&s->seg->IDirectMusicSegment8_iface, iid, o);
        break;
    case DMUS_PATH_SEGMENT_TRACK: {
        if (pch || !g) break;
        dm_track *t = NULL;
        if (s->perf->host) dm_host_lock(s->perf->host);
        t = dm_segstate_track(s->ss, (const dm_guid *)g, n);
        IDirectMusicTrack8 *to = t ? track_object(t) : NULL;
        if (s->perf->host) dm_host_unlock(s->perf->host);
        if (to) { hr = IDirectMusicTrack8_QueryInterface(to, iid, o); IDirectMusicTrack8_Release(to); }
        break;
    }
    case DMUS_PATH_SEGMENT_GRAPH: case DMUS_PATH_SEGMENT_TOOL: {
        if (stage == DMUS_PATH_SEGMENT_GRAPH && (n || pch)) break;
        if (!s->perf->host) break;
        dm_host_lock(s->perf->host);
        IDirectMusicGraph *gr = dm_segstate_get_graph(s->ss);
        if (!gr && SUCCEEDED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&gr))) dm_segstate_set_graph(s->ss, gr);
        if (gr) IDirectMusicGraph_AddRef(gr);
        dm_host_unlock(s->perf->host);
        if (!gr) { hr = E_OUTOFMEMORY; break; }
        hr = stage == DMUS_PATH_SEGMENT_GRAPH ? IDirectMusicGraph_QueryInterface(gr, iid, o) : rm_graph_find_tool(gr, pch, g, n, iid, o);
        IDirectMusicGraph_Release(gr);
        break;
    }
    case DMUS_PATH_PERFORMANCE_GRAPH: case DMUS_PATH_PERFORMANCE_TOOL: {
        if (stage == DMUS_PATH_PERFORMANCE_GRAPH && (n || pch)) break;
        IDirectMusicGraph *gr = perf_graph(s->perf);
        if (!gr) break;
        hr = stage == DMUS_PATH_PERFORMANCE_GRAPH ? IDirectMusicGraph_QueryInterface(gr, iid, o) : rm_graph_find_tool(gr, pch, g, n, iid, o);
        IDirectMusicGraph_Release(gr);
        break;
    }
    case DMUS_PATH_PERFORMANCE:
        if (!n && !pch) hr = IDirectMusicPerformance8_QueryInterface(&s->perf->IDirectMusicPerformance8_iface, iid, o);
        break;
    default:
        if (s->path) hr = IDirectMusicAudioPath_GetObjectInPath(s->path, pch, stage, buf, g, n, iid, o);
        break;
    }
    return hr;
}
static IDirectMusicSegmentState8Vtbl st_vtbl = { st_qi, st_addref, st_release, st_getrepeats, st_getsegment, st_getstart,
    st_getseek, st_getstartpoint, st_settrackcfg, st_getobj };

/* the state's graph interface (vtable 0x77301f1c): StampPMsg 77315fdd; the tool methods are not there */
static HRESULT owner_stamp(rm_perf *p, DMUS_PMSG *m, int owner, void *obj);
static HRESULT WINAPI gx_insert(IDirectMusicGraph *i, IDirectMusicTool *t, DWORD *pch, DWORD n, LONG index);
static HRESULT WINAPI gx_gettool(IDirectMusicGraph *i, DWORD index, IDirectMusicTool **t);
static HRESULT WINAPI gx_remove(IDirectMusicGraph *i, IDirectMusicTool *t);
static rm_state *sgr(IDirectMusicGraph *i) { return impl_from(i, rm_state, IDirectMusicGraph_iface); }
static HRESULT WINAPI stg_qi(IDirectMusicGraph *i, REFIID r, void **o) { return st_qi(&sgr(i)->IDirectMusicSegmentState8_iface, r, o); }
static ULONG WINAPI stg_addref(IDirectMusicGraph *i) { return st_addref(&sgr(i)->IDirectMusicSegmentState8_iface); }
static ULONG WINAPI stg_release(IDirectMusicGraph *i) { return st_release(&sgr(i)->IDirectMusicSegmentState8_iface); }
static HRESULT WINAPI stg_stamp(IDirectMusicGraph *i, DMUS_PMSG *m)
{
    rm_state *s = sgr(i);
    if (!m || IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!s->perf->host) return DMUS_E_NOT_INIT;
    return owner_stamp(s->perf, m, DM_GRAPH_STATE, s->ss);
}
static IDirectMusicGraphVtbl stg_vtbl = { stg_qi, stg_addref, stg_release, stg_stamp, gx_insert, gx_gettool, gx_remove };

/* ------------------------------------------------------------------ the shared reverb's DMO interface */
typedef struct { IDirectSoundFXWavesReverb8 iface; LONG ref; rm_perf *perf; } rm_reverb;
static rm_reverb *rvb(IDirectSoundFXWavesReverb8 *i) { return impl_from(i, rm_reverb, iface); }
static HRESULT WINAPI rv_qi(IDirectSoundFXWavesReverb8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundFXWavesReverb8)) { *out = i; InterlockedIncrement(&rvb(i)->ref); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI rv_addref(IDirectSoundFXWavesReverb8 *i) { return InterlockedIncrement(&rvb(i)->ref); }
static ULONG WINAPI rv_release(IDirectSoundFXWavesReverb8 *i)
{
    rm_reverb *r = rvb(i);
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) { IDirectMusicPerformance8_Release(&r->perf->IDirectMusicPerformance8_iface); free(r); }
    return n;
}
static HRESULT WINAPI rv_set(IDirectSoundFXWavesReverb8 *i, LPCDSFXWavesReverb p)
{
    rm_reverb *r = rvb(i);
    if (!p) return E_POINTER;
    if (!r->perf->host || dm_host_reverb_set(r->perf->host, p->fInGain, p->fReverbMix, p->fReverbTime, p->fHighFreqRTRatio) < 0)
        return E_INVALIDARG;
    r->perf->revparams = *p;
    return S_OK;
}
static HRESULT WINAPI rv_get(IDirectSoundFXWavesReverb8 *i, LPDSFXWavesReverb p)
{
    if (!p) return E_POINTER;
    *p = rvb(i)->perf->revparams;
    return S_OK;
}
static IDirectSoundFXWavesReverbVtbl rv_vtbl = { rv_qi, rv_addref, rv_release, rv_set, rv_get };
static HRESULT reverb_object(rm_perf *p, REFIID iid, void **out)
{
    rm_reverb *r = calloc(1, sizeof(*r));
    if (!r) return E_OUTOFMEMORY;
    r->iface.lpVtbl = &rv_vtbl; r->ref = 1; r->perf = p;
    IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    HRESULT hr = rv_qi(&r->iface, iid, out);
    rv_release(&r->iface);
    return hr;
}

/* ------------------------------------------------------------------ the buffer manager */
static void buf_release(rm_perf *p, rm_buf *b)
{
    if (--b->refs > 0) return;
    for (rm_buf **q = &p->bufs; *q; q = &(*q)->next) if (*q == b) { *q = b->next; break; }
    if (p->host) dm_host_path_close(p->host, &b->host, 1);
    free(b);
}
static void path_free_config(rm_path *a)
{
    for (DWORD i = 0; i < a->nheld; i++) buf_release(a->perf, a->held[i]);
    for (DWORD k = 0; k < a->nports; k++) {
        for (DWORD m = 0; m < a->ports[k].nmaps; m++) free(a->ports[k].maps[m].b);
        free(a->ports[k].maps); free(a->ports[k].bufs);
    }
    free(a->ports); free(a->top); free(a->held);
    a->ports = NULL; a->top = NULL; a->held = NULL; a->nports = a->ntop = a->nheld = 0;
}
static BOOL hold(rm_path *a, rm_buf *b)
{
    rm_buf **n = realloc(a->held, (a->nheld + 1) * sizeof(*n));
    if (!n) return FALSE;
    a->held = n;
    a->held[a->nheld++] = b;
    return TRUE;
}

/* the standard buffers DirectMusic's configurations can name (dmime 773016a4..), and the effects of the defined ones */
static const GUID RM_GUID_Buffer_Reverb = { 0x186cc541, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID RM_GUID_Buffer_Stereo = { 0x186cc545, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID RM_DS3DALG_NO_VIRTUALIZATION = { 0xc241333f, 0x1c1b, 0x11d2, { 0x94, 0xf5, 0x00, 0xc0, 0x4f, 0xc2, 0x8a, 0xca } };
static const GUID RM_GUID_DSFX_WAVES = { 0x87fc0268, 0x9a55, 0x4360, { 0x95, 0xaa, 0x00, 0x4a, 0x1d, 0x9d, 0xe2, 0x6c } };
static const GUID RM_GUID_DSFX_I3DL2 = { 0xef985e71, 0xd5c7, 0x42d4, { 0xba, 0x4d, 0x2d, 0x07, 0x3e, 0x2e, 0x96, 0xf4 } };

/* 7731e70c (any) / 773208c7 (a shared one): a buffer by GUID; the standard path's shared stereo and reverb buffers
 * count as the manager's while a path holds them */
static rm_buf *buf_find(rm_perf *p, const GUID *g, BOOL shared)
{
    for (rm_buf *b = p->bufs; b; b = b->next)
        if ((!shared || (b->flags & DMUS_BUFFERF_SHARED)) && IsEqualGUID(&b->guid, g)) { b->refs++; return b; }
    int std = IsEqualGUID(g, &RM_GUID_Buffer_Stereo) ? 0 : IsEqualGUID(g, &RM_GUID_Buffer_Reverb) ? 1 : -1;
    if (std < 0 || !p->host || !dm_host_buffer_inuse(p->host, std)) return NULL;
    rm_buf *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->guid = *g; b->flags = DMUS_BUFFERF_SHARED | DMUS_BUFFERF_DEFINED; b->host = std; b->refs = 1;
    dm_host_buffer_addref(p->host, std);
    b->next = p->bufs; p->bufs = b;
    return b;
}

/* 7732084b: a configuration's buffer for a path (shared ones found again); the reference's flags are the
 * configuration's as they were, a buffer that turns out to be a mix-in marking the configuration's */
static HRESULT buf_make(rm_perf *p, apc_buffer *c, rm_bref *ref)
{
    memset(ref, 0, sizeof(*ref));
    if (!(p->audioflags & DMUS_AUDIOF_BUFFERS)) return (HRESULT)0x8878122a;
    ref->flags = c->flags;
    if ((c->flags & DMUS_BUFFERF_SHARED) && (ref->b = buf_find(p, &c->guid, TRUE))) return S_OK;
    dm_hostbuf d;
    memset(&d, 0, sizeof(d));
    d.guid = c->guid;
    DWORD func[2];
    rl_fxsrc fx;
    IUnknown *fxobj = NULL;
    if (c->flags & DMUS_BUFFERF_DEFINED) {
        int hasfx = 0;
        switch (c->type) {
        case 1: d.nch = 2; func[0] = DSBUSID_REVERB_SEND; d.nfunc = 1; d.flags = 0x480c0; hasfx = 1; fx.cls = RM_GUID_DSFX_WAVES; break;
        case 2:
            d.nch = 2; d.nfunc = 0; d.flags = 0xa0c0; hasfx = 1; fx.cls = RM_GUID_DSFX_I3DL2;
            break;
        case 4: d.nch = 1; func[0] = DSBUSID_DYNAMIC_0; d.nfunc = 1; d.flags = 0x680b0; d.alg = &RM_DS3DALG_NO_VIRTUALIZATION; break;
        case 6: d.nch = 1; func[0] = DSBUSID_LEFT; d.nfunc = 1; d.flags = 0x480e0; break;
        case 7: d.nch = 2; func[0] = DSBUSID_LEFT; func[1] = DSBUSID_RIGHT; d.nfunc = 2; d.flags = 0x480e0; break;
        case 8: d.nch = 2; func[0] = DSBUSID_LEFT; func[1] = DSBUSID_RIGHT; d.nfunc = 2; d.flags = 0x480c0; break;
        default: return E_INVALIDARG;
        }
        if ((c->flags & DMUS_BUFFERF_MIXIN) || !d.nfunc) {
            d.nfunc = 0;
            c->flags |= DMUS_BUFFERF_MIXIN;
            d.flags = (d.flags & ~0x40000u) | 0x2000;
        }
        d.flags |= DSBCAPS_CTRLFX;
        d.caps = d.flags | (d.flags & 0x40000 ? 0 : DSBCAPS_LOCSOFTWARE);
        d.func = func;
        if (hasfx) {
            memset(&fx.send, 0, sizeof(fx.send)); fx.level = 0;
            HRESULT hr = rl_fx_create_config(&fx.cls, &fxobj);
            if (FAILED(hr)) return hr;
            fx.obj = fxobj;
            d.nfx = 1; d.fx = &fx;
        }
    } else {
        c->flags &= ~DMUS_BUFFERF_MIXIN;
        if (!c->dsbc || !c->hasdesc) return E_INVALIDARG;
        d.nch = c->nch; d.vol = c->vol; d.pan = c->pan;
        d.nfunc = c->nbus > 2 ? 2 : c->nbus;
        d.func = c->bus;
        d.flags = c->dsflags;
        if (c->has3d) { d.alg = &c->alg; d.ds3d = &c->ds3d; }
        d.nfx = c->nfx; d.fx = c->fx;
        d.caps = (c->dsflags & ~0x40000u) | (d.nfunc ? 0 : 0x2000) | DSBCAPS_LOCSOFTWARE;
        /* (77320b9f marks it a mix-in again only if its first bus id is DSBUSID_NULL, which dsound never gives: a
         * configuration's DSBC mix-in is one only for the first path made from it) */
    }
    rm_buf *b = calloc(1, sizeof(*b));
    if (!b) { if (fxobj) IUnknown_Release(fxobj); return E_OUTOFMEMORY; }
    HRESULT hr = dm_host_buffer_new(p->host, &d, &b->host);
    if (fxobj) IUnknown_Release(fxobj);
    if (FAILED(hr)) { free(b); return hr; }
    b->guid = c->guid; b->flags = ref->flags; b->refs = 1;
    b->next = p->bufs; p->bufs = b;
    ref->b = b;
    char g[64];
    LOG_CALL("music: audiopath buffer %s (type %d, flags %lx) -> host buffer %d", rl_guid_str(&c->guid, g), c->type,
             (unsigned long)ref->flags, b->host);
    return S_OK;
}

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
    if (IsEqualIID(riid, &IID_IDirectMusicGraph)) { *out = &apt(i)->IDirectMusicGraph_iface; InterlockedIncrement(&apt(i)->ref); return S_OK; }
    return rm_noiface(__func__, riid, out);
}
static ULONG WINAPI ap_addref(IDirectMusicAudioPath *i) { return InterlockedIncrement(&apt(i)->ref); }
static ULONG WINAPI ap_release(IDirectMusicAudioPath *i)
{
    rm_path *a = apt(i);
    LONG r = InterlockedDecrement(&a->ref);
    LOG_CALL("music: AudioPath::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        if (a->perf->host) {
            dm_host_lock(a->perf->host);
            void **w = dm_audiopath_wrapper(a->ap);
            if (*w == a) *w = NULL;
            dm_audiopath_release(a->ap);
            dm_host_unlock(a->perf->host);
            if (a->nbuf) dm_host_path_close(a->perf->host, a->buf, a->nbuf);
        }
        path_free_config(a);
        IDirectMusicPerformance8_Release(&a->perf->IDirectMusicPerformance8_iface);
        free(a);
    }
    return r;
}
/* the port stage: the synth port, by GUID_All_Objects or its own GUID (index 0: there is one) */
static HRESULT port_object(rm_perf *p, REFGUID g, DWORD n, REFIID iid, void **o)
{
    if (n || !p->port || (!IsEqualGUID(g, &GUID_All_Objects) && !IsEqualGUID(g, &RM_GUID_SynthPort))) return DMUS_E_NOT_FOUND;
    return rm_port_query(p->port, iid, o);
}
/* a buffer's object at the buffer stages: the buffer (index 0), or its effects (IDirectSoundBuffer8::GetObjectInPath) */
static HRESULT buf_object(rm_path *a, rm_buf *b, DWORD stage, REFGUID g, DWORD n, REFIID iid, void **o)
{
    if (stage == DMUS_PATH_BUFFER || stage == DMUS_PATH_MIXIN_BUFFER)
        return n ? DMUS_E_NOT_FOUND : dm_host_buffer_query(a->perf->host, b->host, iid, o);
    if (b->host == 1 && n == 0 && (IsEqualGUID(g, &GUID_All_Objects) || IsEqualGUID(g, &GUID_DSFX_WAVES_REVERB)))
        return reverb_object(a->perf, iid, o);      /* the standard path's shared reverb */
    return dm_host_buffer_dmo(a->perf->host, b->host, g, n, iid, o);
}
/* 7731fbc1, the stages a configured path has of its own */
static HRESULT cfg_getobj(rm_path *a, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFIID iid, void **o)
{
    HRESULT hr = DMUS_E_NOT_FOUND;
    if (!a->perf->host) return hr;
    dm_host_lock(a->perf->host);
    if (stage == DMUS_PATH_PORT) {
        /* the port configurations whose PChannels include pch: one port (the synth) */
        for (DWORD k = 0; k < a->nports; k++) {
            rm_pcfg *pc = &a->ports[k];
            if (pch != DMUS_PCHANNEL_ALL && (pch < pc->base || pch >= pc->base + pc->count)) continue;
            hr = port_object(a->perf, g, n, iid, o);
            break;
        }
    } else if (stage == 0x5000) {
        if (!n && !pch && a->perf->port) hr = rm_port_sink(a->perf->port, iid, o);
    } else if (stage == DMUS_PATH_BUFFER || stage == DMUS_PATH_BUFFER_DMO) {
        /* the port configurations whose PChannels include pch (DMUS_PCHANNEL_ALL: all), then their maps; dwBuffer
         * counts the maps' buffers */
        for (DWORD k = 0; k < a->nports && hr < 0; k++) {
            rm_pcfg *pc = &a->ports[k];
            if (pch != DMUS_PCHANNEL_ALL && (pch < pc->base || pch >= pc->base + pc->count)) continue;
            DWORD idx = buf;
            for (DWORD m = 0; m < pc->nmaps; m++) {
                rm_pmap *pm = &pc->maps[m];
                if (pch != DMUS_PCHANNEL_ALL && (pch < pm->base || pch >= pm->base + pm->count)) continue;
                if (idx >= pm->nbuf) { idx -= pm->nbuf; continue; }
                if (pm->b[idx]) hr = buf_object(a, pm->b[idx], stage, g, n, iid, o);
                break;
            }
        }
    } else {
        /* the top-level mix-in buffers (only on PChannel 0) */
        if (pch == 0)
            for (DWORD i = 0, idx = buf; i < a->ntop; i++) {
                if (!(a->top[i].flags & DMUS_BUFFERF_MIXIN)) continue;
                if (idx--) continue;
                hr = buf_object(a, a->top[i].b, stage, g, n, iid, o);
                break;
            }
    }
    dm_host_unlock(a->perf->host);
    return hr;
}
static HRESULT WINAPI ap_getobj(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    rm_path *a = apt(i);
    if (!o) return E_POINTER;
    *o = NULL;
    if (buf && (stage < DMUS_PATH_BUFFER || stage > 0x7fff)) return DMUS_E_NOT_FOUND;     /* 7731fbc1 */
    if (a->cfg && (stage == DMUS_PATH_PORT || stage == 0x5000 || stage == DMUS_PATH_BUFFER || stage == DMUS_PATH_BUFFER_DMO ||
                   stage == DMUS_PATH_MIXIN_BUFFER || stage == DMUS_PATH_MIXIN_BUFFER_DMO))
        return cfg_getobj(a, pch, stage, buf, g, n, iid, o);
    if (stage == DMUS_PATH_PERFORMANCE) return n || pch ? DMUS_E_NOT_FOUND : IDirectMusicPerformance8_QueryInterface(&a->perf->IDirectMusicPerformance8_iface, iid, o);
    if (stage == DMUS_PATH_AUDIOPATH) return n || pch ? DMUS_E_NOT_FOUND : ap_qi(i, iid, o);
    if (stage == DMUS_PATH_BUFFER) {
        /* the path's buffers for its PChannels (DMUS_PCHANNEL_ALL: any), dwBuffer counting them in order */
        if (n || !a->perf->host || (pch != DMUS_PCHANNEL_ALL && pch >= a->npch) || buf >= (DWORD)a->nbuf) return DMUS_E_NOT_FOUND;
        return dm_host_buffer_query(a->perf->host, a->buf[buf], iid, o);
    }
    if (stage == DMUS_PATH_PRIMARY_BUFFER) {
        if (n || pch || !a->perf->host) return DMUS_E_NOT_FOUND;
        return dm_host_primary_query(a->perf->host, iid, o);
    }
    if (stage == DMUS_PATH_BUFFER_DMO) {
        if (!a->perf->host || (pch != DMUS_PCHANNEL_ALL && pch >= a->npch) || buf >= (DWORD)a->nbuf) return DMUS_E_NOT_FOUND;
        if (a->buf[buf] == 1 && n == 0 && (IsEqualGUID(g, &GUID_All_Objects) || IsEqualGUID(g, &GUID_DSFX_WAVES_REVERB)))
            return reverb_object(a->perf, iid, o);
        return DMUS_E_NOT_FOUND;
    }
    if (stage == DMUS_PATH_AUDIOPATH_GRAPH || stage == DMUS_PATH_AUDIOPATH_TOOL) {
        /* its graph: made empty for the graph stage if it has none, not for the tool stage */
        if (stage == DMUS_PATH_AUDIOPATH_GRAPH && (n || pch)) return DMUS_E_NOT_FOUND;
        if (!a->perf->host) return DMUS_E_NOT_FOUND;
        dm_host_lock(a->perf->host);
        IDirectMusicGraph *gr = dm_audiopath_get_graph(a->ap);
        if (!gr && stage == DMUS_PATH_AUDIOPATH_GRAPH && SUCCEEDED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&gr)))
            dm_audiopath_set_graph(a->ap, gr);
        if (gr) IDirectMusicGraph_AddRef(gr);
        dm_host_unlock(a->perf->host);
        if (!gr) return stage == DMUS_PATH_AUDIOPATH_GRAPH ? E_OUTOFMEMORY : DMUS_E_NOT_FOUND;
        HRESULT hr = stage == DMUS_PATH_AUDIOPATH_GRAPH ? IDirectMusicGraph_QueryInterface(gr, iid, o) : rm_graph_find_tool(gr, pch, g, n, iid, o);
        IDirectMusicGraph_Release(gr);
        return hr;
    }
    if (stage == DMUS_PATH_PERFORMANCE_GRAPH || stage == DMUS_PATH_PERFORMANCE_TOOL) {
        /* the performance's graph (made empty if it has none); its tools by the path's PChannel turned into the
         * performance's */
        if (stage == DMUS_PATH_PERFORMANCE_GRAPH && (n || pch)) return DMUS_E_NOT_FOUND;
        IDirectMusicGraph *gr = perf_graph(a->perf);
        if (!gr) return DMUS_E_NOT_FOUND;
        HRESULT hr;
        if (stage == DMUS_PATH_PERFORMANCE_GRAPH) hr = IDirectMusicGraph_QueryInterface(gr, iid, o);
        else {
            uint32_t r;
            if (dm_audiopath_convert(a->ap, pch, &r) >= 0) pch = r;
            hr = rm_graph_find_tool(gr, pch, g, n, iid, o);
        }
        IDirectMusicGraph_Release(gr);
        return hr;
    }
    if (stage == DMUS_PATH_PORT) {          /* the synth port, for the path's PChannels */
        uint32_t r;
        if (pch != DMUS_PCHANNEL_ALL && dm_audiopath_convert(a->ap, pch, &r) < 0) return DMUS_E_NOT_FOUND;
        return port_object(a->perf, g, n, iid, o);
    }
    if (stage == 0x5000) return n || pch || !a->perf->port ? DMUS_E_NOT_FOUND : rm_port_sink(a->perf->port, iid, o);
    switch (stage) {        /* stages an audiopath knows that are not here (mix-in buffers of a standard path) */
    case DMUS_PATH_MIXIN_BUFFER: case DMUS_PATH_MIXIN_BUFFER_DMO: {
        char b1[64];
        LOG_INFO("music: AudioPath::GetObjectInPath(stage %lx, %s) not supported", (unsigned long)stage, guidstr(iid, b1));
        return DMUS_E_NOT_FOUND;
    }
    }
    return E_INVALIDARG;
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
/* 7731eedb: the broadcast values (0xfffffff0 and up) as they are; a PChannel the path does not have: DMUS_E_NOT_FOUND */
static HRESULT WINAPI ap_convert(IDirectMusicAudioPath *i, DWORD in, DWORD *out)
{
    if (!out || IsBadWritePtr(out, 4)) return E_POINTER;
    if (in >= 0xfffffff0) { *out = in; return S_OK; }
    uint32_t r;
    if (dm_audiopath_convert(apt(i)->ap, in, &r) < 0) return DMUS_E_NOT_FOUND;
    *out = r;
    return S_OK;
}
static IDirectMusicAudioPathVtbl ap_vtbl = { ap_qi, ap_addref, ap_release, ap_getobj, ap_activate, ap_setvolume, ap_convert };

/* the path's graph interface (vtable 0x7730219c): StampPMsg 7731ef40 */
static rm_path *agr(IDirectMusicGraph *i) { return impl_from(i, rm_path, IDirectMusicGraph_iface); }
static HRESULT WINAPI apg_qi(IDirectMusicGraph *i, REFIID r, void **o) { return ap_qi(&agr(i)->IDirectMusicAudioPath_iface, r, o); }
static ULONG WINAPI apg_addref(IDirectMusicGraph *i) { return ap_addref(&agr(i)->IDirectMusicAudioPath_iface); }
static ULONG WINAPI apg_release(IDirectMusicGraph *i) { return ap_release(&agr(i)->IDirectMusicAudioPath_iface); }
static HRESULT WINAPI apg_stamp(IDirectMusicGraph *i, DMUS_PMSG *m)
{
    rm_path *a = agr(i);
    if (IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!a->perf->host) return DMUS_E_NOT_INIT;
    return owner_stamp(a->perf, m, DM_GRAPH_AUDIOPATH, a->ap);
}
static IDirectMusicGraphVtbl apg_vtbl = { apg_qi, apg_addref, apg_release, apg_stamp, gx_insert, gx_gettool, gx_remove };

/* takes over a reference on ap; the path's object unless it has one */
static rm_path *new_path(rm_perf *p, dm_audiopath *ap)
{
    rm_path *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->IDirectMusicAudioPath_iface.lpVtbl = &ap_vtbl;
    a->IDirectMusicGraph_iface.lpVtbl = &apg_vtbl;
    a->ref = 1;
    a->ap = ap;
    a->perf = p;
    void **w = dm_audiopath_wrapper(ap);
    if (!*w) *w = a;
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
    /* 77303eb4: the output tool and the graph (+4, +8) */
    if (IsEqualIID(riid, &IID_IDirectMusicTool)) { *out = &prf(i)->IDirectMusicTool_iface; InterlockedIncrement(&prf(i)->ref); return S_OK; }
    if (IsEqualIID(riid, &IID_IDirectMusicGraph)) { *out = &prf(i)->IDirectMusicGraph_iface; InterlockedIncrement(&prf(i)->ref); return S_OK; }
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
        dm_perf *pf = dm_host_perf(p->host);
        dm_perf_stop(pf, NULL, NULL, 0, 0);
        dm_perf_drop_app_messages(pf);
        dm_perf_set_graph(pf, NULL);
        dm_host_unlock(p->host);
    }
}
static ULONG WINAPI pf_release(IDirectMusicPerformance8 *i)
{
    rm_perf *p = prf(i);
    LONG r = InterlockedDecrement(&p->ref);
    LOG_CALL("music: Performance::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        p->ref = 1;     /* guard against re-entry from children */
        pf_closedown(p);
        if (s_clockperf == p) s_clockperf = NULL;
        if (g_lastperf == p) g_lastperf = NULL;
        rm_port_detach(p->port);
        rm_port_release(p->port);
        dm_host_free(p->host);
        if (p->dm) IDirectMusic8_Release(p->dm);
        free(p);
    }
    return r;
}

static const dm_toolops s_toolops;
static HRESULT ensure_host(rm_perf *p, uint32_t rate, uint32_t voices)
{
    if (p->host) return S_OK;
    p->host = dm_host_new(rate ? rate : 22050, voices ? voices : 64, 64);
    if (!p->host) return E_OUTOFMEMORY;
    p->port = rm_port_new(p->host);
    p->revparams.fInGain = 0; p->revparams.fReverbMix = 0; p->revparams.fReverbTime = 1000; p->revparams.fHighFreqRTRatio = 0.001f;
    if (!s_clockperf) s_clockperf = p;
    g_lastperf = p;
    dm_host_lock(p->host);
    dm_perf_set_toolops(dm_host_perf(p->host), &s_toolops, p);
    dm_host_unlock(p->host);
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
static IUnknown *state_object(rm_perf *p, dm_segstate *ss);
/* 77304d68: the primary segment state at music time t */
static HRESULT WINAPI pf_getsegstate(IDirectMusicPerformance8 *i, IDirectMusicSegmentState **o, MUSIC_TIME t)
{
    rm_perf *p = prf(i);
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_FOUND;
    IUnknown *u = NULL;
    dm_host_lock(p->host);
    dm_segstate *ss = dm_perf_get_segstate(dm_host_perf(p->host), t);
    if (ss) u = state_object(p, ss);
    dm_host_unlock(p->host);
    if (!u) return DMUS_E_NOT_FOUND;
    *o = (IDirectMusicSegmentState *)u;
    return S_OK;
}
static HRESULT WINAPI pf_setprepare(IDirectMusicPerformance8 *i, DWORD ms) { rm_perf *p = prf(i); PERF_LOCKED(p, dm_perf_set_prepare_time(dm_host_perf(p->host), ms)); return S_OK; }
static HRESULT WINAPI pf_getprepare(IDirectMusicPerformance8 *i, DWORD *ms) { rm_perf *p = prf(i); if (!ms) return E_POINTER; PERF_LOCKED(p, *ms = dm_perf_get_prepare_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_setbumper(IDirectMusicPerformance8 *i, DWORD ms) { rm_perf *p = prf(i); PERF_LOCKED(p, dm_perf_set_bumper_length(dm_host_perf(p->host), ms)); return S_OK; }
static HRESULT WINAPI pf_getbumper(IDirectMusicPerformance8 *i, DWORD *ms) { rm_perf *p = prf(i); if (!ms) return E_POINTER; PERF_LOCKED(p, *ms = dm_perf_get_bumper_length(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_sendpmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m);
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
/* ------------------------------------------------------------------ messages the application holds
 * Every DMUS_PMSG the application gets from us is a block with a private header in front.  A message the engine
 * handed out (to a tool, or as a notification) is the engine message's application view: the header names the
 * engine message, which names the view (dm_pmsg.ext).  The view is current while the application has the message
 * (pm_to_com when it is handed out), the engine's fields while the engine has it (pm_from_com when it comes back:
 * SendPMsg, StampPMsg, a tool's DMUS_S_REQUEUE).  An AllocPMsg / ClonePMsg block gets its engine message when it
 * is first sent or stamped.  FreePMsg releases the interfaces in it (pTool, pGraph, punkUser, as dmime's FreePMsg). */
#define RM_PMSG_MAGIC 0x6d50526dUL
typedef struct { DWORD magic, size; dm_pmsg *e; DWORD pad; } rm_pmsghdr;
static DMUS_PMSG *pmsg_new(ULONG cb)
{
    rm_pmsghdr *h = calloc(1, sizeof(*h) + cb);
    if (!h) return NULL;
    h->magic = RM_PMSG_MAGIC; h->size = cb;
    DMUS_PMSG *m = (DMUS_PMSG *)(h + 1);
    m->dwSize = cb;
    return m;
}
static rm_pmsghdr *pmsg_hdr(DMUS_PMSG *m)
{
    if (!m) return NULL;
    rm_pmsghdr *h = (rm_pmsghdr *)m - 1;
    return IsBadReadPtr(h, sizeof(*h)) || h->magic != RM_PMSG_MAGIC ? NULL : h;
}
static HRESULT WINAPI pf_allocpmsg(IDirectMusicPerformance8 *i, ULONG cb, DMUS_PMSG **m)
{
    if (!m) return E_POINTER;
    *m = NULL;
    if (cb < sizeof(DMUS_PMSG)) return E_INVALIDARG;
    return (*m = pmsg_new(cb)) ? S_OK : E_OUTOFMEMORY;
}
/* the performance's own tool and graph interfaces are not counted (a message naming them would keep the
 * performance alive): pft_addref / pfg_addref */
static void pmsg_drop_ifaces(DMUS_PMSG *m)
{
    if (m->pTool) IDirectMusicTool_Release(m->pTool);
    if (m->pGraph) IDirectMusicGraph_Release(m->pGraph);
    if (m->punkUser) IUnknown_Release(m->punkUser);
    m->pTool = NULL; m->pGraph = NULL; m->punkUser = NULL;
}

/* a freed message's memory is kept a while (marked: SendPMsg / FreePMsg on it fail as on a queued one), so an
 * application that still touches it right after sending does not write into freed memory */
static DMUS_PMSG *s_retired[256];
static LONG s_retpos;
static void pmsg_retire(DMUS_PMSG *m)
{
    rm_pmsghdr *h = (rm_pmsghdr *)m - 1;
    pmsg_drop_ifaces(m);
    h->magic = 0; h->e = NULL;
    LONG k = InterlockedIncrement(&s_retpos) & 255;
    DMUS_PMSG *old = (DMUS_PMSG *)InterlockedExchangePointer((void **)&s_retired[k], m);
    if (old) free((rm_pmsghdr *)old - 1);
}

static ULONG type_size(DWORD t)
{
    switch (t) {
    case DMUS_PMSGT_MIDI: return sizeof(DMUS_MIDI_PMSG);
    case DMUS_PMSGT_NOTE: return sizeof(DMUS_NOTE_PMSG);
    case DMUS_PMSGT_SYSEX: return sizeof(DMUS_SYSEX_PMSG);
    case DMUS_PMSGT_NOTIFICATION: return sizeof(DMUS_NOTIFICATION_PMSG);
    case DMUS_PMSGT_TEMPO: return sizeof(DMUS_TEMPO_PMSG);
    case DMUS_PMSGT_CURVE: return sizeof(DMUS_CURVE_PMSG);
    case DMUS_PMSGT_TIMESIG: return sizeof(DMUS_TIMESIG_PMSG);
    case DMUS_PMSGT_PATCH: return sizeof(DMUS_PATCH_PMSG);
    case DMUS_PMSGT_TRANSPOSE: return sizeof(DMUS_TRANSPOSE_PMSG);
    case DMUS_PMSGT_CHANNEL_PRIORITY: return sizeof(DMUS_CHANNEL_PRIORITY_PMSG);
    case DMUS_PMSGT_WAVE: return sizeof(DMUS_WAVE_PMSG);
    default: return sizeof(DMUS_PMSG);
    }
}

static IUnknown *state_object(rm_perf *p, dm_segstate *ss);

/* the engine's fields into the application's view (its type's fields if it is large enough for them) */
static void pm_to_com(rm_perf *p, const dm_pmsg *e, DMUS_PMSG *m)
{
    rm_pmsghdr *hh = pmsg_hdr(m);
    ULONG cap = hh ? hh->size : m->dwSize;
    m->dwSize = e->size;
    m->rtTime = e->rt; m->mtTime = e->mt; m->dwFlags = e->flags; m->dwPChannel = e->pch; m->dwVirtualTrackID = e->vpch;
    m->dwType = e->type; m->dwVoiceID = e->voice; m->dwGroupID = e->group;
    if (e->tool == 1) {
        if (m->pTool != &p->IDirectMusicTool_iface) {
            if (m->pTool) IDirectMusicTool_Release(m->pTool);
            m->pTool = &p->IDirectMusicTool_iface;
        }
    } else if (e->tool == 0 && m->pTool) { IDirectMusicTool_Release(m->pTool); m->pTool = NULL; }
    if (!m->punkUser && e->user && e->userref) m->punkUser = state_object(p, e->user);
    if (cap < type_size(e->type) && e->type != DMUS_PMSGT_SYSEX) return;
    switch (e->type) {
    case DMUS_PMSGT_MIDI: { DMUS_MIDI_PMSG *x = (DMUS_MIDI_PMSG *)m; x->bStatus = e->m_status; x->bByte1 = e->m_b1; x->bByte2 = e->m_b2; break; }
    case DMUS_PMSGT_NOTE: {
        DMUS_NOTE_PMSG *x = (DMUS_NOTE_PMSG *)m;
        x->mtDuration = e->n_dur; x->wMusicValue = e->n_musicval; x->wMeasure = e->n_measure; x->nOffset = e->n_offset;
        x->bBeat = e->n_beat; x->bGrid = e->n_grid; x->bVelocity = e->n_vel; x->bFlags = e->n_flags; x->bTimeRange = e->n_timerange;
        x->bDurRange = e->n_durrange; x->bVelRange = e->n_velrange; x->bPlayModeFlags = e->n_playmode; x->bSubChordLevel = e->n_subchord;
        x->bMidiValue = e->n_midival; x->cTranspose = e->n_transpose;
        break;
    }
    case DMUS_PMSGT_SYSEX: {
        DMUS_SYSEX_PMSG *x = (DMUS_SYSEX_PMSG *)m;
        if (cap < FIELD_OFFSET(DMUS_SYSEX_PMSG, abData)) break;
        x->dwLen = e->sx_len;
        if (e->sx_data && FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) + e->sx_len <= cap) memcpy(x->abData, e->sx_data, e->sx_len);
        break;
    }
    case DMUS_PMSGT_NOTIFICATION: {
        DMUS_NOTIFICATION_PMSG *x = (DMUS_NOTIFICATION_PMSG *)m;
        memcpy(&x->guidNotificationType, &e->nf_guid, sizeof(GUID));
        x->dwNotificationOption = e->nf_option; x->dwField1 = e->nf_field1; x->dwField2 = e->nf_field2;
        break;
    }
    case DMUS_PMSGT_TEMPO: ((DMUS_TEMPO_PMSG *)m)->dblTempo = e->tempo; break;
    case DMUS_PMSGT_CURVE: {
        DMUS_CURVE_PMSG *x = (DMUS_CURVE_PMSG *)m;
        x->mtDuration = e->c_dur; x->mtOriginalStart = e->c_origstart; x->mtResetDuration = e->c_resetdur;
        x->nStartValue = e->c_start; x->nEndValue = e->c_end; x->nResetValue = e->c_reset; x->wMeasure = e->c_measure;
        x->nOffset = e->c_offset; x->bBeat = e->c_beat; x->bGrid = e->c_grid; x->bType = e->c_type; x->bCurveShape = e->c_shape;
        x->bCCData = e->c_cc; x->bFlags = e->c_flags; x->wParamType = e->c_param; x->wMergeIndex = e->c_merge;
        break;
    }
    case DMUS_PMSGT_TIMESIG: {
        DMUS_TIMESIG_PMSG *x = (DMUS_TIMESIG_PMSG *)m;
        x->bBeatsPerMeasure = e->ts_bpm; x->bBeat = e->ts_beat; x->wGridsPerBeat = e->ts_grids;
        break;
    }
    case DMUS_PMSGT_PATCH: { DMUS_PATCH_PMSG *x = (DMUS_PATCH_PMSG *)m; x->byInstrument = e->m_status; x->byMSB = e->m_b1; x->byLSB = e->m_b2; break; }
    case DMUS_PMSGT_TRANSPOSE: { DMUS_TRANSPOSE_PMSG *x = (DMUS_TRANSPOSE_PMSG *)m; x->nTranspose = e->t_transpose; x->wMergeIndex = e->t_merge; break; }
    case DMUS_PMSGT_CHANNEL_PRIORITY: ((DMUS_CHANNEL_PRIORITY_PMSG *)m)->dwChannelPriority = e->priority; break;
    case DMUS_PMSGT_WAVE: {
        DMUS_WAVE_PMSG *x = (DMUS_WAVE_PMSG *)m;
        x->rtStartOffset = e->w_startoff; x->rtDuration = e->w_dur; x->lOffset = e->w_offset; x->lVolume = e->w_vol;
        x->lPitch = e->w_pitch; x->bFlags = e->w_flags;
        break;
    }
    default: break;
    }
}

/* the application's message into the engine's (77304e7e's checks: a message too small for its type is refused) */
static HRESULT pm_from_com_type(const DMUS_PMSG *m, dm_pmsg *e);
static HRESULT pm_from_com(rm_perf *p, const DMUS_PMSG *m, dm_pmsg *e)
{
    e->size = m->dwSize;
    e->rt = m->rtTime; e->mt = m->mtTime; e->flags = m->dwFlags; e->pch = m->dwPChannel; e->vpch = m->dwVirtualTrackID;
    e->type = m->dwType; e->voice = m->dwVoiceID; e->group = m->dwGroupID;
    e->tool = !m->pTool ? 0 : m->pTool == &p->IDirectMusicTool_iface ? 1 : 2;
    return pm_from_com_type(m, e);
}
/* the same where the original does not look at the type's fields (StampPMsg, a tool's ProcessPMsg): one too small
 * keeps the engine's */
static void pm_from_com_loose(rm_perf *p, const DMUS_PMSG *m, dm_pmsg *e)
{
    pm_from_com(p, m, e);       /* the header always; a type's fields only when they are all there */
}
static HRESULT pm_from_com_type(const DMUS_PMSG *m, dm_pmsg *e)
{
    switch (m->dwType) {
    case DMUS_PMSGT_MIDI: if (m->dwSize >= sizeof(DMUS_MIDI_PMSG)) { const DMUS_MIDI_PMSG *x = (const DMUS_MIDI_PMSG *)m; e->m_status = x->bStatus; e->m_b1 = x->bByte1; e->m_b2 = x->bByte2; } else return E_INVALIDARG; break;
    case DMUS_PMSGT_NOTE:
        if (m->dwSize >= sizeof(DMUS_NOTE_PMSG)) {
            const DMUS_NOTE_PMSG *x = (const DMUS_NOTE_PMSG *)m;
            e->n_dur = x->mtDuration; e->n_musicval = x->wMusicValue; e->n_measure = x->wMeasure; e->n_offset = x->nOffset;
            e->n_beat = x->bBeat; e->n_grid = x->bGrid; e->n_vel = x->bVelocity; e->n_flags = x->bFlags; e->n_timerange = x->bTimeRange;
            e->n_durrange = x->bDurRange; e->n_velrange = x->bVelRange; e->n_playmode = x->bPlayModeFlags; e->n_subchord = x->bSubChordLevel;
            e->n_midival = x->bMidiValue; e->n_transpose = x->cTranspose;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_SYSEX: {
        const DMUS_SYSEX_PMSG *x = (const DMUS_SYSEX_PMSG *)m;
        if (m->dwSize < FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) || x->dwLen > m->dwSize - FIELD_OFFSET(DMUS_SYSEX_PMSG, abData)) return E_INVALIDARG;
        uint8_t *d = NULL;
        if (x->dwLen && !(d = malloc(x->dwLen))) return E_OUTOFMEMORY;
        if (d) memcpy(d, x->abData, x->dwLen);
        free(e->sx_data);
        e->sx_data = d; e->sx_len = x->dwLen;
        break;
    }
    case DMUS_PMSGT_NOTIFICATION:
        if (m->dwSize >= sizeof(DMUS_NOTIFICATION_PMSG)) {
            const DMUS_NOTIFICATION_PMSG *x = (const DMUS_NOTIFICATION_PMSG *)m;
            memcpy(&e->nf_guid, &x->guidNotificationType, sizeof(e->nf_guid));
            e->nf_option = x->dwNotificationOption; e->nf_field1 = x->dwField1; e->nf_field2 = x->dwField2;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_TEMPO: if (m->dwSize >= sizeof(DMUS_TEMPO_PMSG)) e->tempo = ((const DMUS_TEMPO_PMSG *)m)->dblTempo; else return E_INVALIDARG; break;
    case DMUS_PMSGT_CURVE:
        if (m->dwSize >= FIELD_OFFSET(DMUS_CURVE_PMSG, wParamType)) {
            const DMUS_CURVE_PMSG *x = (const DMUS_CURVE_PMSG *)m;
            e->c_dur = x->mtDuration; e->c_origstart = x->mtOriginalStart; e->c_resetdur = x->mtResetDuration;
            e->c_start = x->nStartValue; e->c_end = x->nEndValue; e->c_reset = x->nResetValue; e->c_measure = x->wMeasure;
            e->c_offset = x->nOffset; e->c_beat = x->bBeat; e->c_grid = x->bGrid; e->c_type = x->bType; e->c_shape = x->bCurveShape;
            e->c_cc = x->bCCData; e->c_flags = x->bFlags;
            if (m->dwSize >= sizeof(DMUS_CURVE_PMSG)) { e->c_param = x->wParamType; e->c_merge = x->wMergeIndex; }
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_TIMESIG:
        if (m->dwSize >= sizeof(DMUS_TIMESIG_PMSG)) {
            const DMUS_TIMESIG_PMSG *x = (const DMUS_TIMESIG_PMSG *)m;
            e->ts_bpm = x->bBeatsPerMeasure; e->ts_beat = x->bBeat; e->ts_grids = x->wGridsPerBeat;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_PATCH: if (m->dwSize >= sizeof(DMUS_PATCH_PMSG)) { const DMUS_PATCH_PMSG *x = (const DMUS_PATCH_PMSG *)m; e->m_status = x->byInstrument; e->m_b1 = x->byMSB; e->m_b2 = x->byLSB; } else return E_INVALIDARG; break;
    case DMUS_PMSGT_TRANSPOSE:
        if (m->dwSize >= FIELD_OFFSET(DMUS_TRANSPOSE_PMSG, wMergeIndex)) {
            const DMUS_TRANSPOSE_PMSG *x = (const DMUS_TRANSPOSE_PMSG *)m;
            e->t_transpose = x->nTranspose;
            if (m->dwSize >= sizeof(*x)) e->t_merge = x->wMergeIndex;
        } else return E_INVALIDARG;
        break;
    case DMUS_PMSGT_CHANNEL_PRIORITY: if (m->dwSize >= sizeof(DMUS_CHANNEL_PRIORITY_PMSG)) e->priority = ((const DMUS_CHANNEL_PRIORITY_PMSG *)m)->dwChannelPriority; else return E_INVALIDARG; break;
    case DMUS_PMSGT_WAVE: {
        /* the wave track's own: its engine message has the port's voice (an application's has none to play) */
        if (!e->w_voice || m->dwSize < sizeof(DMUS_WAVE_PMSG)) return E_INVALIDARG;
        const DMUS_WAVE_PMSG *x = (const DMUS_WAVE_PMSG *)m;
        e->w_startoff = x->rtStartOffset; e->w_dur = x->rtDuration; e->w_offset = x->lOffset; e->w_vol = x->lVolume;
        e->w_pitch = x->lPitch; e->w_flags = x->bFlags;
        break;
    }
    default: break;                                         /* stop, dirty, lyrics, user: no data the engine reads */
    }
    return S_OK;
}

/* the application's view of an engine message (made the first time), current */
static DMUS_PMSG *pm_view(rm_perf *p, dm_pmsg *e)
{
    DMUS_PMSG *m = e->ext;
    if (!m) {
        ULONG n = type_size(e->type);
        if (e->type == DMUS_PMSGT_SYSEX && FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) + e->sx_len > n) n = FIELD_OFFSET(DMUS_SYSEX_PMSG, abData) + e->sx_len;
        if (e->size > n) n = e->size;
        if (!(m = pmsg_new(n))) return NULL;
        ((rm_pmsghdr *)m - 1)->e = e;
        e->ext = m;
    }
    pm_to_com(p, e, m);
    return m;
}

/* the engine message behind an application's message: its own, or (keep) a new one that stays with it; a message
 * that is not ours gets a new one that does not (*temp, the caller frees it with pm_untemp) */
static dm_pmsg *pm_engine(rm_perf *p, dm_perf *pf, DMUS_PMSG *m, BOOL strict, BOOL *temp, HRESULT *hr)
{
    rm_pmsghdr *h = pmsg_hdr(m);
    dm_pmsg *e = h ? h->e : NULL;
    *temp = FALSE;
    *hr = S_OK;
    if (e) {
        if (strict) *hr = pm_from_com(p, m, e); else pm_from_com_loose(p, m, e);
        return e;
    }
    if (!(e = dm_perf_alloc_pmsg(pf, m->dwSize))) { *hr = E_OUTOFMEMORY; return NULL; }
    if (strict) {
        if (FAILED(*hr = pm_from_com(p, m, e))) { dm_perf_free_pmsg(pf, e); return NULL; }
    } else pm_from_com_loose(p, m, e);
    e->ext = m;
    if (h) h->e = e; else *temp = TRUE;
    return e;
}
static void pm_untemp(dm_perf *pf, dm_pmsg *e) { e->ext = NULL; dm_perf_free_pmsg(pf, e); }
static void pm_unlink(dm_perf *pf, DMUS_PMSG *m, dm_pmsg *e)       /* an engine message the application's goes without */
{
    rm_pmsghdr *h = pmsg_hdr(m);
    if (h) h->e = NULL;
    e->ext = NULL;
    dm_perf_free_pmsg(pf, e);
}

static HRESULT WINAPI pf_freepmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m)
{
    rm_perf *p = prf(i);
    rm_pmsghdr *h = pmsg_hdr(m);
    if (!h) return m ? DMUS_E_CANNOT_FREE : E_POINTER;
    if (h->e && p->host) {
        dm_host_lock(p->host);
        HRESULT hr = h->e ? dm_perf_free_pmsg(dm_host_perf(p->host), h->e) : S_OK;    /* the view goes with it */
        dm_host_unlock(p->host);
        return hr;
    }
    pmsg_drop_ifaces(m);
    h->magic = 0;
    free(h);
    return S_OK;
}

/* 77304e7e SendPMsg: the message goes into the queues (the performance owns it from here) */
static HRESULT WINAPI pf_sendpmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *m)
{
    rm_perf *p = prf(i);
    if (!m || IsBadReadPtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!p->host) return DMUS_E_NO_MASTER_CLOCK;
    rm_pmsghdr *h = pmsg_hdr(m);
    if (!h) return DMUS_E_ALREADY_SENT;
    if (m->dwType == 0xfffffff0) return pf_freepmsg(i, m);
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL fresh = !h->e, temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, TRUE, &temp, &hr);
    if (e) {
        hr = dm_perf_send_pmsg_keep(pf, e);
        if (FAILED(hr) && fresh && h->magic == RM_PMSG_MAGIC && h->e == e) pm_unlink(pf, m, e);   /* still the application's */
    }
    dm_host_unlock(p->host);
    return hr;
}

/* ------------------------------------------------------------------ tool graphs (the engine's dm_toolops) */
static rm_path *new_path(rm_perf *p, dm_audiopath *ap);
static rm_state *state_of(rm_perf *p, dm_segstate *ss);

/* the graph interface of a graph's owner (one reference) */
static IDirectMusicGraph *owner_graph(rm_perf *p, int owner, void *obj)
{
    if (owner == DM_GRAPH_PERF) return &p->IDirectMusicGraph_iface;
    if (owner == DM_GRAPH_STATE) {
        rm_state *s = state_of(p, obj);
        return s ? &s->IDirectMusicGraph_iface : NULL;
    }
    void **w = dm_audiopath_wrapper(obj);
    rm_path *a = *w;
    if (a) { InterlockedIncrement(&a->ref); return &a->IDirectMusicGraph_iface; }
    dm_audiopath_addref(obj);                   /* a path the application let go of: a new object for it */
    if (!(a = new_path(p, obj))) { dm_audiopath_release(obj); return NULL; }
    *w = a;
    return &a->IDirectMusicGraph_iface;
}

static int tops_stamp(void *ctx, void *graph, int owner, void *obj, dm_pmsg *e)
{
    rm_perf *p = ctx;
    DMUS_PMSG *m = pm_view(p, e);
    if (!m) return E_OUTOFMEMORY;
    HRESULT hr = IDirectMusicGraph_StampPMsg((IDirectMusicGraph *)graph, m);
    if (hr == S_OK) {
        IDirectMusicGraph *g = owner_graph(p, owner, obj);
        if (m->pGraph != g) {
            if (m->pGraph) IDirectMusicGraph_Release(m->pGraph);
            m->pGraph = g;
        } else if (g) IDirectMusicGraph_Release(g);
        e->flags = m->dwFlags;
    }
    return hr;
}
static void tops_unstamp(void *ctx, dm_pmsg *e)
{
    DMUS_PMSG *m = e->ext;
    if (m->pGraph) { IDirectMusicGraph_Release(m->pGraph); m->pGraph = NULL; }
    if (m->pTool) { IDirectMusicTool_Release(m->pTool); m->pTool = NULL; }
}
/* 77309396: ProcessPMsg, called with the engine let go (the tool may call back into the performance) */
static int tops_process(void *ctx, dm_pmsg *e)
{
    rm_perf *p = ctx;
    DMUS_PMSG *m = pm_view(p, e);
    IDirectMusicTool *t = m ? m->pTool : NULL;
    if (!t) return DMUS_S_FREE;
    IDirectMusicTool_AddRef(t);
    dm_host_unlock(p->host);
    HRESULT hr = IDirectMusicTool_ProcessPMsg(t, (IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, m);
    dm_host_lock(p->host);
    IDirectMusicTool_Release(t);
    if (hr == DMUS_S_REQUEUE && pmsg_hdr(m)) pm_from_com_loose(p, m, e);
    return hr;
}
static void tops_flush(void *ctx, dm_pmsg *e, rtime_t rt)
{
    rm_perf *p = ctx;
    DMUS_PMSG *m = pm_view(p, e);
    if (m && m->pTool) IDirectMusicTool_Flush(m->pTool, (IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, m, rt);
}
static void tops_clone_ext(void *ctx, dm_pmsg *d, const dm_pmsg *s)
{
    DMUS_PMSG *sm = s->ext;
    ULONG n = sm->dwSize;
    rm_pmsghdr *sh = pmsg_hdr(sm);
    if (sh && sh->size > n) n = sh->size;
    DMUS_PMSG *m = pmsg_new(n);
    if (!m) return;
    memcpy(m, sm, sh ? sh->size : sm->dwSize);
    if (m->pTool) IDirectMusicTool_AddRef(m->pTool);
    if (m->pGraph) IDirectMusicGraph_AddRef(m->pGraph);
    if (m->punkUser) IUnknown_AddRef(m->punkUser);
    ((rm_pmsghdr *)m - 1)->e = d;
    d->ext = m;
}
static void tops_free_ext(void *ctx, dm_pmsg *e) { pmsg_retire(e->ext); }
static void *tops_state_graph(void *ctx, dm_segstate *ss)
{
    dm_segment *g = dm_segstate_segment(ss);
    rm_segment *s = g ? g->wrapper : NULL;
    IDirectMusicGraph *sg = s ? s->graph : NULL, *c = NULL;
    if (sg) rm_graph_clone(sg, &c);             /* 77311d79: through the graph's private Clone */
    return c;
}
static void tops_graph_release(void *ctx, void *g) { IDirectMusicGraph_Release((IDirectMusicGraph *)g); }
static const dm_toolops s_toolops = { tops_stamp, tops_unstamp, tops_process, tops_flush, tops_clone_ext, tops_free_ext,
                                      tops_state_graph, tops_graph_release };

/* the StampPMsg of the performance's, a state's and an audiopath's graph interfaces on any message */
static HRESULT owner_stamp(rm_perf *p, DMUS_PMSG *m, int owner, void *obj)
{
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, FALSE, &temp, &hr);
    if (e) {
        hr = owner == DM_GRAPH_PERF ? dm_perf_stamp(pf, e) : owner == DM_GRAPH_STATE ? dm_segstate_stamp(obj, e) : dm_audiopath_stamp(obj, e);
        pm_to_com(p, e, m);
        if (temp) pm_untemp(pf, e);
    }
    dm_host_unlock(p->host);
    return hr;
}

static HRESULT WINAPI gx_insert(IDirectMusicGraph *i, IDirectMusicTool *t, DWORD *pch, DWORD n, LONG index) { return E_NOTIMPL; }
static HRESULT WINAPI gx_gettool(IDirectMusicGraph *i, DWORD index, IDirectMusicTool **t) { return E_NOTIMPL; }
static HRESULT WINAPI gx_remove(IDirectMusicGraph *i, IDirectMusicTool *t) { return E_NOTIMPL; }

/* the performance's graph interface (77307925): not counted (see self_iface) */
static rm_perf *pgr(IDirectMusicGraph *i) { return impl_from(i, rm_perf, IDirectMusicGraph_iface); }
static HRESULT WINAPI pf_qi(IDirectMusicPerformance8 *i, REFIID riid, void **out);
static HRESULT WINAPI pfg_qi(IDirectMusicGraph *i, REFIID r, void **o) { return pf_qi(&pgr(i)->IDirectMusicPerformance8_iface, r, o); }
static ULONG WINAPI pfg_addref(IDirectMusicGraph *i) { return 2; }
static ULONG WINAPI pfg_release(IDirectMusicGraph *i) { return 1; }
static HRESULT WINAPI pfg_stamp(IDirectMusicGraph *i, DMUS_PMSG *m)
{
    rm_perf *p = pgr(i);
    if (!m || IsBadWritePtr(m, sizeof(DWORD))) return E_POINTER;
    if (!p->host) {                             /* no engine yet: straight to the output tool */
        if (m->pGraph) { IDirectMusicGraph_Release(m->pGraph); m->pGraph = NULL; }
        if (m->pTool) IDirectMusicTool_Release(m->pTool);
        m->dwFlags = (m->dwFlags & ~(DMUS_PMSGF_TOOL_IMMEDIATE | DMUS_PMSGF_TOOL_ATTIME)) | DMUS_PMSGF_TOOL_QUEUE;
        m->pTool = &p->IDirectMusicTool_iface;
        return S_OK;
    }
    return owner_stamp(p, m, DM_GRAPH_PERF, NULL);
}
static IDirectMusicGraphVtbl pfg_vtbl = { pfg_qi, pfg_addref, pfg_release, pfg_stamp, gx_insert, gx_gettool, gx_remove };

/* the performance's output tool interface (0x77301290): not counted either */
static rm_perf *ptl(IDirectMusicTool *i) { return impl_from(i, rm_perf, IDirectMusicTool_iface); }
static HRESULT WINAPI pft_qi(IDirectMusicTool *i, REFIID r, void **o) { return pf_qi(&ptl(i)->IDirectMusicPerformance8_iface, r, o); }
static ULONG WINAPI pft_addref(IDirectMusicTool *i) { return 2; }
static ULONG WINAPI pft_release(IDirectMusicTool *i) { return 1; }
static HRESULT WINAPI pft_init(IDirectMusicTool *i, IDirectMusicGraph *g) { return E_NOTIMPL; }
static HRESULT WINAPI pft_deliv(IDirectMusicTool *i, DWORD *d)      /* 773078a3 */
{
    if (IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    *d = DMUS_PMSGF_TOOL_IMMEDIATE;
    return S_OK;
}
static HRESULT WINAPI pft_ntypes(IDirectMusicTool *i, DWORD *n)     /* 773078d2 */
{
    if (IsBadWritePtr(n, sizeof(*n))) return E_POINTER;
    *n = 0;
    return S_OK;
}
static HRESULT WINAPI pft_types(IDirectMusicTool *i, DWORD **t, DWORD n) { return E_NOTIMPL; }
static HRESULT WINAPI pft_process(IDirectMusicTool *i, IDirectMusicPerformance *perf, DMUS_PMSG *m)
{
    rm_perf *p = ptl(i);
    if (!m || IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, FALSE, &temp, &hr);
    if (e) {
        hr = dm_perf_output_process(pf, e);
        pm_to_com(p, e, m);
        if (temp) pm_untemp(pf, e);
    }
    dm_host_unlock(p->host);
    return hr;
}
static HRESULT WINAPI pft_flush(IDirectMusicTool *i, IDirectMusicPerformance *perf, DMUS_PMSG *m, REFERENCE_TIME rt)
{
    rm_perf *p = ptl(i);
    if (!m || IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    BOOL temp;
    HRESULT hr;
    dm_pmsg *e = pm_engine(p, pf, m, FALSE, &temp, &hr);
    if (e) {
        dm_perf_output_flush(pf, e, rt);
        if (temp) pm_untemp(pf, e);
    }
    dm_host_unlock(p->host);
    return hr;
}
static IDirectMusicToolVtbl pft_vtbl = { pft_qi, pft_addref, pft_release, pft_init, pft_deliv, pft_ntypes, pft_types, pft_process, pft_flush };

/* the COM object of a segment state (made on demand for a state the application never got, call with the lock) */
static IUnknown *state_object(rm_perf *p, dm_segstate *ss)
{
    void **w = dm_segstate_wrapper(ss);
    rm_state *s = *w;
    if (s) { InterlockedIncrement(&s->ref); return (IUnknown *)&s->IDirectMusicSegmentState8_iface; }
    if (!(s = calloc(1, sizeof(*s)))) return NULL;
    s->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    s->IDirectMusicGraph_iface.lpVtbl = &stg_vtbl;
    s->ref = 1; s->ss = ss; s->lazy = TRUE;
    dm_segstate_addref(ss);
    dm_segment *g = dm_segstate_segment(ss);
    if (g && g->wrapper) { s->seg = g->wrapper; IDirectMusicSegment8_AddRef(&s->seg->IDirectMusicSegment8_iface); }
    s->perf = p; IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    dm_audiopath *ap = dm_segstate_audiopath(ss);
    rm_path *a = ap ? *dm_audiopath_wrapper(ap) : NULL;
    if (a) { s->path = &a->IDirectMusicAudioPath_iface; IDirectMusicAudioPath_AddRef(s->path); }
    *w = s;
    return (IUnknown *)&s->IDirectMusicSegmentState8_iface;
}
static rm_state *state_of(rm_perf *p, dm_segstate *ss)
{
    IUnknown *u = state_object(p, ss);
    return u ? sst((IDirectMusicSegmentState8 *)u) : NULL;
}
/* 77305a93 GetGraph, 77305b61 SetGraph: refused before Init (DX8) */
static HRESULT WINAPI pf_getgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph **g)
{
    rm_perf *p = prf(i);
    if (IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    dm_host_lock(p->host);
    IDirectMusicGraph *x = dm_perf_get_graph(dm_host_perf(p->host));
    if (x) { *g = x; IDirectMusicGraph_AddRef(x); }
    dm_host_unlock(p->host);
    return x ? S_OK : DMUS_E_NOT_FOUND;
}
static HRESULT WINAPI pf_setgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph *g)
{
    rm_perf *p = prf(i);
    if (g && (IsBadReadPtr(g, 4) || IsBadReadPtr(*(void **)g, 4) || IsBadCodePtr(**(FARPROC **)g))) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    if (g) IDirectMusicGraph_AddRef(g);
    dm_host_lock(p->host);
    dm_perf_set_graph(dm_host_perf(p->host), g);
    dm_host_unlock(p->host);
    return S_OK;
}
/* 77305b0d: the performance's graph, made empty if it has none (GetObjectInPath's performance stages) */
static IDirectMusicGraph *perf_graph(rm_perf *p)
{
    if (!p->host) return NULL;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    IDirectMusicGraph *g = dm_perf_get_graph(pf);
    if (!g && SUCCEEDED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&g))) dm_perf_set_graph(pf, g);
    if (g) IDirectMusicGraph_AddRef(g);
    dm_host_unlock(p->host);
    return g;
}
/* ------------------------------------------------------------------ notifications */
static void notify_event(void *ctx) { rm_perf *p = ctx; if (p->notify) SetEvent(p->notify); }
static HRESULT WINAPI pf_setnotif(IDirectMusicPerformance8 *i, HANDLE h, REFERENCE_TIME t)
{
    rm_perf *p = prf(i);
    LOG_CALL("music: Performance::SetNotificationHandle(%p, %lld)", h, (long long)t);
    p->notify = h;
    PERF_LOCKED(p, dm_perf_set_notification_handle(dm_host_perf(p->host), notify_event, p, t));
    return S_OK;
}
static HRESULT WINAPI pf_getnotif(IDirectMusicPerformance8 *i, DMUS_NOTIFICATION_PMSG **m)
{
    rm_perf *p = prf(i);
    if (!m || IsBadWritePtr(m, sizeof(*m))) return E_POINTER;
    *m = NULL;
    if (!p->host) return S_FALSE;
    HRESULT hr = S_FALSE;
    dm_host_lock(p->host);
    dm_perf *pf = dm_host_perf(p->host);
    dm_pmsg *c = dm_perf_get_notification(pf);
    if (c) {
        DMUS_NOTIFICATION_PMSG *n = (DMUS_NOTIFICATION_PMSG *)pmsg_new(sizeof(*n));
        if (n) {
            n->rtTime = c->rt; n->mtTime = c->mt; n->dwFlags = c->flags; n->dwPChannel = c->pch;
            n->dwVirtualTrackID = c->vpch; n->dwType = DMUS_PMSGT_NOTIFICATION; n->dwVoiceID = c->voice;
            n->dwGroupID = c->group;
            n->punkUser = c->user ? state_object(p, c->user) : NULL;
            memcpy(&n->guidNotificationType, &c->nf_guid, sizeof(GUID));
            n->dwNotificationOption = c->nf_option; n->dwField1 = c->nf_field1; n->dwField2 = c->nf_field2;
            *m = n;
            hr = S_OK;
        } else hr = E_OUTOFMEMORY;
        dm_perf_free_pmsg(pf, c);
    }
    dm_host_unlock(p->host);
    return hr;
}
static HRESULT WINAPI pf_addnotif(IDirectMusicPerformance8 *i, REFGUID g)
{
    rm_perf *p = prf(i);
    char gb[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_add_notification_type(dm_host_perf(p->host), (const dm_guid *)g));
    LOG_CALL("music: Performance::AddNotificationType(%s) -> %d", rl_guid_str(g, gb), hr);
    return hr;
}
static HRESULT WINAPI pf_remnotif(IDirectMusicPerformance8 *i, REFGUID g)
{
    rm_perf *p = prf(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_remove_notification_type(dm_host_perf(p->host), (const dm_guid *)g));
    return hr;
}
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
    if (!port) return E_POINTER;
    PERF_LOCKED(p, hr = dm_perf_assign_block(dm_host_perf(p->host), b, 0, g));
    return hr;
}
static HRESULT WINAPI pf_assignpch(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort *port, DWORD g, DWORD c)
{
    rm_perf *p = prf(i);
    int hr;
    if (!port) return E_POINTER;
    PERF_LOCKED(p, hr = dm_perf_assign_pchannel(dm_host_perf(p->host), pch, 0, g, c));
    return hr;
}
/* 7730672d */
static HRESULT WINAPI pf_pchinfo(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort **port, DWORD *g, DWORD *c)
{
    rm_perf *p = prf(i);
    if ((port && IsBadWritePtr(port, sizeof(*port))) || (g && IsBadWritePtr(g, sizeof(*g))) || (c && IsBadWritePtr(c, sizeof(*c))))
        return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    int idx = 0, hr; uint32_t grp = 0, ch = 0;
    PERF_LOCKED(p, hr = dm_perf_pchannel_info(dm_host_perf(p->host), pch, &idx, &grp, &ch));
    if (hr < 0) return E_INVALIDARG;
    if (port) { *port = NULL; if (p->port) rm_port_query(p->port, &IID_IDirectMusicPort, (void **)port); }
    if (g) *g = grp;
    if (c) *c = ch;
    return S_OK;
}
static HRESULT WINAPI pf_downloadinst(IDirectMusicPerformance8 *i, IDirectMusicInstrument *in, DWORD pch, IDirectMusicDownloadedInstrument **d,
                                      DMUS_NOTERANGE *r, DWORD n, IDirectMusicPort **port, DWORD *g, DWORD *c) { return rm_notimpl(__func__); }
static HRESULT WINAPI pf_invalidate(IDirectMusicPerformance8 *i, MUSIC_TIME t, DWORD f) { rm_perf *p = prf(i); PERF_LOCKED(p, dm_perf_invalidate(dm_host_perf(p->host), t, f)); return S_OK; }
/* 77306acd / 773069c2: GetParam(Ex) through the playing segment states; 77306fbf: SetParam to the primary segment */
static HRESULT pf_getparam_any(IDirectMusicPerformance8 *i, REFGUID g, DWORD id, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp)
{
    rm_perf *p = prf(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!p->host) return DMUS_E_NO_MASTER_CLOCK;
    int prm = prm_of(g), hr;
    mtime_t next = 0;
    dm_host_lock(p->host);
    hr = dm_perf_getparam(dm_host_perf(p->host), prm, id, gb, n, t, nx ? &next : NULL, pp);
    prm_out(prm, hr, pp);
    dm_host_unlock(p->host);
    if (nx) *nx = next;
    return hr;
}
static HRESULT WINAPI pf_getparam(IDirectMusicPerformance8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp)
{
    return pf_getparam_any(i, g, 0, gb, n, t, nx, pp);
}
static HRESULT WINAPI pf_setparam(IDirectMusicPerformance8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, void *pp)
{
    rm_perf *p = prf(i);
    char b[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!p->host) return DMUS_E_NO_MASTER_CLOCK;
    int prm = prm_of(g);
    dm_dltarget dt; void *data; dm_host *h;
    HRESULT hr = prm_data(prm, pp, &dt, &data, &h);
    if (FAILED(hr)) return hr;
    dm_host_lock(p->host);
    hr = dm_perf_setparam(dm_host_perf(p->host), prm, gb, n, t, data);
    dm_host_unlock(p->host);
    LOG_CALL("music: Performance::SetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, (unsigned)hr);
    return hr;
}
static HRESULT WINAPI pf_getglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz)
{
    rm_perf *p = prf(i);
    if (!v) return E_POINTER;
    if (IsEqualGUID(g, &GUID_PerfMasterVolume) && sz >= 4) { *(long *)v = 0; return S_OK; }
    if (IsEqualGUID(g, &GUID_PerfMasterTempo) && sz >= 4) { *(float *)v = 1.0f; return S_OK; }
    if (IsEqualGUID(g, &GUID_PerfAutoDownload) && sz >= 4) { int on = 0; PERF_LOCKED(p, on = dm_perf_get_auto_download(dm_host_perf(p->host))); *(BOOL *)v = on; return S_OK; }
    if (IsEqualGUID(g, &G_PerfMasterGrooveLevel) && sz >= 1) {
        int8_t gl = 0; int ok = 0;
        PERF_LOCKED(p, ok = dm_perf_get_groove(dm_host_perf(p->host), &gl));
        if (!ok) return DMUS_E_NOT_FOUND;
        *(char *)v = gl;
        return S_OK;
    }
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
    if (IsEqualGUID(g, &G_PerfMasterGrooveLevel) && sz >= 1) { PERF_LOCKED(p, dm_perf_set_groove(dm_host_perf(p->host), *(char *)v)); return S_OK; }
    return S_OK;
}
static HRESULT WINAPI pf_getlatency(IDirectMusicPerformance8 *i, REFERENCE_TIME *t) { rm_perf *p = prf(i); if (!t) return E_POINTER; PERF_LOCKED(p, *t = dm_perf_latency_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_getqueue(IDirectMusicPerformance8 *i, REFERENCE_TIME *t) { rm_perf *p = prf(i); if (!t) return E_POINTER; PERF_LOCKED(p, *t = dm_perf_queue_time(dm_host_perf(p->host))); return S_OK; }
static HRESULT WINAPI pf_adjust(IDirectMusicPerformance8 *i, REFERENCE_TIME t)
{
    rm_perf *p = prf(i);
    if (t < -10000000 || t > 10000000) return E_INVALIDARG;
    if (!p->host) return S_OK;
    int hr;
    PERF_LOCKED(p, hr = dm_perf_adjust_time(dm_host_perf(p->host), t));
    return hr;
}
static HRESULT WINAPI pf_closedown_m(IDirectMusicPerformance8 *i) { LOG_CALL("music: Performance::CloseDown"); pf_closedown(prf(i)); return S_OK; }
/* 7730a4e3 */
static HRESULT WINAPI pf_resolved(IDirectMusicPerformance8 *i, REFERENCE_TIME t, REFERENCE_TIME *o, DWORD f)
{
    if (!o) return E_POINTER;
    rm_perf *p = prf(i);
    if (!p->host) { *o = t; return S_OK; }
    PERF_LOCKED(p, *o = dm_perf_resolved_time(dm_host_perf(p->host), t, f));
    return S_OK;
}
/* MIDIToMusic 773082a6 / MusicToMIDI 773085af (dmtheory.c) */
static HRESULT WINAPI pf_midi2music(IDirectMusicPerformance8 *i, BYTE v, DMUS_CHORD_KEY *c, BYTE m, BYTE l, WORD *o)
{
    if (IsBadReadPtr(c, sizeof(*c)) || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    return dmt_midi_to_music(v, (const dm_chord_key *)c, m, l, o);
}
static HRESULT WINAPI pf_music2midi(IDirectMusicPerformance8 *i, WORD v, DMUS_CHORD_KEY *c, BYTE m, BYTE l, BYTE *o)
{
    if (IsBadReadPtr(c, sizeof(*c)) || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    return dmt_music_to_midi(v, (const dm_chord_key *)c, m, l, o);
}
/* TimeToRhythm 7730897b / RhythmToTime 77308a97: measure, beat, grid and offset under a time signature (a beat of 0
 * counts as a quarter, 0 grids as 256).  Microsoft divides by zero on 0 beats per measure; that is refused here. */
static void ts_units(const DMUS_TIMESIGNATURE *ts, int *beat, int *grid)
{
    *beat = ts->bBeat ? 0xc00 / ts->bBeat : 0x300;
    *grid = *beat / (ts->wGridsPerBeat ? ts->wGridsPerBeat : 0x100);
}
static HRESULT WINAPI pf_time2rhythm(IDirectMusicPerformance8 *i, MUSIC_TIME t, DMUS_TIMESIGNATURE *ts, WORD *m, BYTE *b, BYTE *g, short *o)
{
    if (IsBadReadPtr(ts, 8) || IsBadWritePtr(m, 2) || IsBadWritePtr(b, 1) || IsBadWritePtr(g, 1) || IsBadWritePtr(o, 2)) return E_POINTER;
    int beat, grid;
    ts_units(ts, &beat, &grid);
    if (!ts->bBeatsPerMeasure || !grid) return E_INVALIDARG;
    int bar = ts->bBeatsPerMeasure * beat, rel = t - ts->mtTime, inbar = rel % bar, inbeat = inbar % beat;
    *m = (WORD)(rel / bar);
    *b = (BYTE)(inbar / beat);
    *g = (BYTE)(inbeat / grid);
    *o = (short)(inbeat % grid);
    if (grid >> 1 < *o) {               /* more than half a grid: the next grid, negative offset */
        *o = (short)(*o - grid);
        if (++*g == ts->wGridsPerBeat) {
            *g = 0;
            if (++*b == ts->bBeatsPerMeasure) { *b = 0; ++*m; }
        }
    }
    return S_OK;
}
static HRESULT WINAPI pf_rhythm2time(IDirectMusicPerformance8 *i, WORD m, BYTE b, BYTE g, short o, DMUS_TIMESIGNATURE *ts, MUSIC_TIME *t)
{
    if (IsBadReadPtr(ts, 8) || IsBadWritePtr(t, 4)) return E_POINTER;
    int beat, grid;
    ts_units(ts, &beat, &grid);
    *t = (MUSIC_TIME)((uint32_t)g * (uint32_t)grid + (uint32_t)m * ts->bBeatsPerMeasure * (uint32_t)beat + (uint32_t)b * beat + o + ts->mtTime);
    return S_OK;
}
static HRESULT WINAPI pf_createstdpath(IDirectMusicPerformance8 *i, DWORD type, DWORD npch, BOOL act, IDirectMusicAudioPath **out);
static HRESULT WINAPI pf_initaudio(IDirectMusicPerformance8 *i, IDirectMusic **pdm, IDirectSound **pds, HWND w, DWORD type, DWORD npch,
                                   DWORD flags, DMUS_AUDIOPARAMS *params)
{
    rm_perf *p = prf(i);
    LOG_CALL("music: Performance::InitAudio(dm %p/%p, ds %p/%p, type %lu, %lu pchannels, flags %lx)", (void *)pdm, pdm ? (void *)*pdm : NULL,
             (void *)pds, pds ? (void *)*pds : NULL, (unsigned long)type, (unsigned long)npch, (unsigned long)flags);
    if (p->host) return DMUS_E_ALREADY_INITED;
    p->audioflags = flags ? flags : DMUS_AUDIOF_ALL;
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
    dm_host_lock(p->host);
    rm_state *s = *dm_segstate_wrapper(ss);
    if (s) {        /* made on demand already (a tool or notification met the state first): it is the application's now */
        InterlockedIncrement(&s->ref);
        if (s->lazy) { s->lazy = FALSE; s->xref++; }    /* it holds the play's reference now, and still its own */
        else dm_segstate_release(ss);
        if (!s->path) {
            rm_path *pa = a ? a : p->defpath;
            if (pa) { s->path = &pa->IDirectMusicAudioPath_iface; IDirectMusicAudioPath_AddRef(s->path); }
        }
        dm_host_unlock(p->host);
        if (out) *out = (IDirectMusicSegmentState *)&s->IDirectMusicSegmentState8_iface;
        else IDirectMusicSegmentState8_Release(&s->IDirectMusicSegmentState8_iface);
        return S_OK;
    }
    dm_host_unlock(p->host);
    s = calloc(1, sizeof(*s));
    if (!s) { dm_host_lock(p->host); dm_segstate_release(ss); dm_host_unlock(p->host); return E_OUTOFMEMORY; }
    s->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    s->IDirectMusicGraph_iface.lpVtbl = &stg_vtbl;
    s->ref = 1;
    s->ss = ss;
    s->seg = g; IDirectMusicSegment8_AddRef(&g->IDirectMusicSegment8_iface);
    s->perf = p; IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    rm_path *pa = a ? a : p->defpath;
    if (pa) { s->path = &pa->IDirectMusicAudioPath_iface; IDirectMusicAudioPath_AddRef(s->path); }
    dm_host_lock(p->host);
    *dm_segstate_wrapper(ss) = s;
    dm_host_unlock(p->host);
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
/* 773056f8: AllocPMsg(dwSize), a copy, and a reference on each interface in it */
static HRESULT WINAPI pf_clonepmsg(IDirectMusicPerformance8 *i, DMUS_PMSG *s, DMUS_PMSG **d)
{
    if (!d || !s) return E_POINTER;
    HRESULT hr = IDirectMusicPerformance8_AllocPMsg(i, s->dwSize, d);
    if (FAILED(hr)) return hr;
    memcpy(*d, s, s->dwSize);
    if (s->punkUser) IUnknown_AddRef(s->punkUser);
    if (s->pTool) IDirectMusicTool_AddRef(s->pTool);
    if (s->pGraph) IDirectMusicGraph_AddRef(s->pGraph);
    return S_OK;
}
/* 7731f12c / 7731ea82: a buffer a PChannel map names: the port configuration's own, the path's top-level ones,
 * another port configuration's, then any of the performance's */
static rm_buf *path_find(rm_path *a, const rm_pcfg *pc, const GUID *g)
{
    for (DWORD i = 0; i < pc->nbufs; i++) if (IsEqualGUID(&pc->bufs[i].b->guid, g)) { pc->bufs[i].b->refs++; return pc->bufs[i].b; }
    for (DWORD i = 0; i < a->ntop; i++) if (IsEqualGUID(&a->top[i].b->guid, g)) { a->top[i].b->refs++; return a->top[i].b; }
    for (DWORD k = 0; k < a->nports; k++)
        for (DWORD i = 0; i < a->ports[k].nbufs; i++)
            if (IsEqualGUID(&a->ports[k].bufs[i].b->guid, g)) { a->ports[k].bufs[i].b->refs++; return a->ports[k].bufs[i].b; }
    return buf_find(a->perf, g, FALSE);
}

/* 77320f10: an audiopath from a configuration: the top-level buffers, then each port configuration's buffers and
 * PChannel maps (77320dd5), then the PChannels (7731f19b): on a port with the audiopath feature, each map's PChannels
 * on its buffers' buses; otherwise the port configuration's PChannels as they are */
static HRESULT cfg_path(rm_perf *p, apc *c, BOOL act, rm_path **out)
{
    *out = NULL;
    rm_path *a = calloc(1, sizeof(*a));
    if (!a) return E_OUTOFMEMORY;
    a->perf = p; a->cfg = TRUE;
    HRESULT hr = S_OK;
    a->top = calloc(c->nbufs + 1, sizeof(rm_bref));
    a->ports = calloc(c->nports + 1, sizeof(rm_pcfg));
    if (!a->top || !a->ports) hr = E_OUTOFMEMORY;
    for (DWORD i = 0; SUCCEEDED(hr) && i < c->nbufs; i++) {
        hr = buf_make(p, &c->bufs[i], &a->top[a->ntop]);
        if (SUCCEEDED(hr)) { hold(a, a->top[a->ntop].b); a->ntop++; }
    }
    for (DWORD k = 0; SUCCEEDED(hr) && k < c->nports; k++) {
        apc_port *cp = &c->ports[k];
        rm_pcfg *pc = &a->ports[a->nports++];
        pc->port = cp->h.guidPort; pc->base = cp->h.dwPChannelBase; pc->count = cp->h.dwPChannelCount;
        pc->bufs = calloc(cp->nbufs + 1, sizeof(rm_bref));
        pc->maps = calloc(cp->nmaps + 1, sizeof(rm_pmap));
        if (!pc->bufs || !pc->maps) { hr = E_OUTOFMEMORY; break; }
        for (DWORD i = 0; SUCCEEDED(hr) && i < cp->nbufs; i++) {
            hr = buf_make(p, &cp->bufs[i], &pc->bufs[pc->nbufs]);
            if (SUCCEEDED(hr)) { hold(a, pc->bufs[pc->nbufs].b); pc->nbufs++; }
        }
        for (DWORD m = 0; SUCCEEDED(hr) && m < cp->nmaps; m++) {
            rm_pmap *pm = &pc->maps[pc->nmaps++];
            pm->base = cp->maps[m].base; pm->count = cp->maps[m].count; pm->nbuf = cp->maps[m].nbuf;
            pm->b = calloc(pm->nbuf + 1, sizeof(rm_buf *));
            if (!pm->b) { hr = E_OUTOFMEMORY; break; }
            for (DWORD i = 0; i < pm->nbuf; i++) if ((pm->b[i] = path_find(a, pc, &cp->maps[m].bufs[i]))) hold(a, pm->b[i]);
        }
    }
    /* the PChannels */
    DWORD total = 0;
    for (DWORD k = 0; k < c->nports; k++) {
        total += c->ports[k].h.dwPChannelCount;
        for (DWORD m = 0; m < c->ports[k].nmaps; m++) total += c->ports[k].maps[m].count;
    }
    uint32_t *virt = malloc((total + 1) * sizeof(uint32_t)), *nbus = calloc(total + 1, sizeof(uint32_t));
    uint32_t *buses = calloc(total + 1, 32 * sizeof(uint32_t));
    int *drum = calloc(total + 1, sizeof(int));
    if (SUCCEEDED(hr) && (!virt || !nbus || !buses || !drum)) hr = E_OUTOFMEMORY;
    DWORD n = 0;
    for (DWORD k = 0; SUCCEEDED(hr) && k < c->nports; k++) {
        apc_port *cp = &c->ports[k];
        rm_pcfg *pc = &a->ports[k];
        BOOL feat = (cp->pp.dwValidParams & DMUS_PORTPARAMS_FEATURES) && (cp->pp.dwFeatures & DMUS_PORT_FEATURE_AUDIOPATH);
        BOOL drums = cp->h.dwFlags & DMUS_PORTCONFIGF_DRUMSON10;
        if (feat) {
            for (DWORD m = 0; m < pc->nmaps; m++) {
                rm_pmap *pm = &pc->maps[m];
                uint32_t bl[32];
                DWORD nb = 0;
                for (DWORD i = 0; i < pm->nbuf; i++) {
                    if (!pm->b[i] || (pm->b[i]->flags & 4)) continue;
                    nb += dm_host_buffer_buses(p->host, pm->b[i]->host, bl + nb, 32 - nb);
                }
                for (DWORD j = 0; j < pm->count; j++, n++) {
                    virt[n] = pm->base + j;
                    drum[n] = drums ? ((virt[n] & 15) == 9 ? 3 : 1) : 0;
                    nbus[n] = nb;
                    memcpy(buses + 32 * n, bl, nb * sizeof(uint32_t));
                }
            }
        } else {
            for (DWORD j = 0; j < cp->h.dwPChannelCount; j++, n++) {
                virt[n] = cp->h.dwPChannelBase + j;
                drum[n] = drums ? ((virt[n] & 15) == 9 ? 3 : 1) : 0;
            }
        }
    }
    if (SUCCEEDED(hr)) {
        dm_host_lock(p->host);
        a->ap = dm_perf_create_audiopath_ex(dm_host_perf(p->host), act, n, virt, drum, nbus, buses);
        if (a->ap && c->graph) {                    /* 7731f0f2: a clone of the configuration's graph */
            IDirectMusicGraph *g = NULL;
            if (SUCCEEDED(rm_graph_clone(c->graph, &g))) dm_audiopath_set_graph(a->ap, g);
        }
        dm_host_unlock(p->host);
        if (!a->ap) hr = E_OUTOFMEMORY;
    }
    free(virt); free(nbus); free(buses); free(drum);
    if (FAILED(hr)) {
        path_free_config(a);
        free(a);
        return hr;
    }
    a->IDirectMusicAudioPath_iface.lpVtbl = &ap_vtbl;
    a->IDirectMusicGraph_iface.lpVtbl = &apg_vtbl;
    a->ref = 1;
    a->npch = n;
    void **w = dm_audiopath_wrapper(a->ap);
    if (!*w) *w = a;
    IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    *out = a;
    return S_OK;
}

static HRESULT WINAPI pf_createpath(IDirectMusicPerformance8 *i, IUnknown *cfg, BOOL act, IDirectMusicAudioPath **out)
{
    rm_perf *p = prf(i);
    if (!out) return E_POINTER;
    *out = NULL;
    if (!p->host) return DMUS_E_NOT_INIT;
    IUnknown *holder = NULL;
    const apc *c = rm_apcfg_data(cfg, &holder);
    if (!c) {
        LOG_INFO("music: CreateAudioPath: not an audiopath configuration of ours");
        return E_INVALIDARG;
    }
    rm_path *a = NULL;
    HRESULT hr = cfg_path(p, (apc *)c, act, &a);
    IUnknown_Release(holder);
    LOG_CALL("music: CreateAudioPath(%p) -> %08lx (%p)", (void *)cfg, (unsigned long)hr, (void *)a);
    if (SUCCEEDED(hr)) *out = &a->IDirectMusicAudioPath_iface;
    return hr;
}
static HRESULT WINAPI pf_createstdpath(IDirectMusicPerformance8 *i, DWORD type, DWORD npch, BOOL act, IDirectMusicAudioPath **out)
{
    rm_perf *p = prf(i);
    if (!out) return E_POINTER;
    *out = NULL;
    if (!p->host) return DMUS_E_NOT_INIT;
    if (!npch) npch = 16;
    int idx[2], nidx;
    uint32_t buses[4], nbuses;
    if (dm_host_path_open(p->host, type, idx, &nidx, buses, &nbuses) < 0) {
        LOG_INFO("music: CreateStandardAudioPath(type %lu): not a standard type", (unsigned long)type);
        return E_INVALIDARG;
    }
    dm_audiopath *ap;
    PERF_LOCKED(p, ap = dm_perf_create_audiopath(dm_host_perf(p->host), npch, act, buses, nbuses));
    if (!ap) { dm_host_path_close(p->host, idx, nidx); return E_OUTOFMEMORY; }
    rm_path *a = new_path(p, ap);
    if (!a) { PERF_LOCKED(p, dm_audiopath_release(ap)); dm_host_path_close(p->host, idx, nidx); return E_OUTOFMEMORY; }
    a->type = type; a->npch = npch; a->nbuf = nidx;
    for (int k = 0; k < nidx; k++) a->buf[k] = idx[k];
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
static HRESULT WINAPI pf_getparamex(IDirectMusicPerformance8 *i, REFGUID g, DWORD id, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *pp)
{
    return pf_getparam_any(i, g, id, gb, n, t, nx, pp);
}

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
    LOG_CALL("music: Loader::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) { if (g_styleloader == l) g_styleloader = NULL; ld_clear(l); free(l); }
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

#include "dmcomsty.c"
#include "dmcomcmp.c"
static struct dms_style *style_engine_of(void *p) { rm_style *s = style_of(p); return s ? s->s : NULL; }
static dm_band *band_engine_of(void *p)
{
    if (!p || IsBadReadPtr(p, sizeof(void *))) return NULL;
    void *vt = *(void **)p;
    rm_band *b = vt == &bnd_vtbl ? bnd(p) : vt == &bno_vtbl ? bno(p) : vt == &bnp_vtbl ? bnp(p) : NULL;
    return b ? b->b : NULL;
}
static void prm_out(int prm, int hr, void *p)
{
    if (prm == PRM_STYLE && hr >= 0 && p) *(void **)p = style_iface(*(dms_style **)p);
    if (prm == PRM_CHORDMAP && hr >= 0 && p) *(void **)p = chordmap_iface(*(dmc_chordmap **)p);
    if (prm == PRM_BANDPARAM && hr >= 0 && p) {         /* the band copy as a band object (its own reference) */
        DMUS_BAND_PARAM *bp = p;
        dm_band *b = (dm_band *)bp->pBand;
        rm_band *o = b ? new_band(b) : NULL;
        if (b) dm_band_release(b);
        bp->pBand = o ? &o->IDirectMusicBand_iface : NULL;
    }
}

/* a wave track item's reference (773248af: the loader's GetObject of a DirectSoundWave by guid, file or name) */
static HRESULT WINAPI ld_getobject(IDirectMusicLoader8 *i, LPDMUS_OBJECTDESC d, REFIID riid, LPVOID *out);
static dm_wave *loader_getwave(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    rm_loader *l = ctx;
    if (!l) return NULL;
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS;
    d.guidClass = RM_CLSID_DirectSoundWave;
    if (g) { memcpy(&d.guidObject, g, sizeof(GUID)); d.dwValidData |= DMUS_OBJ_OBJECT; }
    if (file) {
        MultiByteToWideChar(CP_ACP, 0, file, -1, d.wszFileName, DMUS_MAX_FILENAME);
        d.dwValidData |= DMUS_OBJ_FILENAME;
        if (file[0] && (file[1] == ':' || file[0] == '\\')) d.dwValidData |= DMUS_OBJ_FULLPATH;
    }
    if (name) { MultiByteToWideChar(CP_ACP, 0, name, -1, d.wszName, DMUS_MAX_NAME); d.dwValidData |= DMUS_OBJ_NAME; }
    IDirectMusicObject *o = NULL;
    if (FAILED(ld_getobject(&l->IDirectMusicLoader8_iface, &d, &IID_IDirectMusicObject, (void **)&o)) || !o) {
        LOG_INFO("music: wave track: wave %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    rm_wave *w = o->lpVtbl == &wvo_vtbl ? wvo(o) : NULL;
    dm_wave *r = w ? w->w : NULL;
    dm_wave_addref(r);
    IDirectMusicObject_Release(o);
    return r;
}

/* a style track's reference (774123a0: the loader's GetObject of a style); the engine style with a reference */
static struct dms_style *loader_getstyle(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    rm_loader *l = ctx;
    if (!l) return NULL;
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS;
    d.guidClass = RM_CLSID_DirectMusicStyle;
    if (g) { memcpy(&d.guidObject, g, sizeof(GUID)); d.dwValidData |= DMUS_OBJ_OBJECT; }
    if (file) {
        MultiByteToWideChar(CP_ACP, 0, file, -1, d.wszFileName, DMUS_MAX_FILENAME);
        d.dwValidData |= DMUS_OBJ_FILENAME;
        if (file[0] && (file[1] == ':' || file[0] == '\\')) d.dwValidData |= DMUS_OBJ_FULLPATH;
    }
    if (name) { MultiByteToWideChar(CP_ACP, 0, name, -1, d.wszName, DMUS_MAX_NAME); d.dwValidData |= DMUS_OBJ_NAME; }
    IDirectMusicStyle8 *o = NULL;
    if (FAILED(ld_getobject(&l->IDirectMusicLoader8_iface, &d, &RM_IID_IDirectMusicStyle8, (void **)&o)) || !o) {
        LOG_INFO("music: style track: style %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    rm_style *s = style_of(o);
    dms_style *r = s ? s->s : NULL;
    dms_style_addref(r);
    IDirectMusicStyle8_Release(o);
    return r;
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
    if ((d->dwValidData & DMUS_OBJ_CLASS) && IsEqualCLSID(&d->guidClass, &RM_CLSID_DirectSoundWave)) {
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
        dm_wave *w = dm_wave_load(data, size);
        free(data);
        if (!w) return DMUS_E_UNSUPPORTED_STREAM;
        rm_wave *o = new_wave(w);
        if (!o) { dm_wave_release(w); return E_OUTOFMEMORY; }
        if (path[0]) { lstrcpynW(o->desc.wszFileName, path, DMUS_MAX_FILENAME); o->desc.dwValidData |= DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH; }
        if (g) { o->desc.guidObject = *g; o->desc.dwValidData |= DMUS_OBJ_OBJECT; }
        cache_add(l, path, g ? g : (w->hasguid ? (const GUID *)&w->guid : NULL), (IUnknown *)&o->IDirectMusicObject_iface);
        HRESULT hr = wvo_qi(&o->IDirectMusicObject_iface, riid, out);
        wvo_release(&o->IDirectMusicObject_iface);
        return hr;
    }
    if ((d->dwValidData & DMUS_OBJ_CLASS) && (IsEqualCLSID(&d->guidClass, &RM_CLSID_DirectMusicStyle) ||
                                              IsEqualCLSID(&d->guidClass, &RM_CLSID_DirectMusicBand))) {
        int isstyle = IsEqualCLSID(&d->guidClass, &RM_CLSID_DirectMusicStyle);
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
        IUnknown *o = NULL;
        HRESULT hr = S_OK;
        if (isstyle) {
            dms_style *x = style_from_data(data, size, l, &hr);
            rm_style *s = x ? new_style(x) : NULL;
            if (x && !s) dms_style_release(x);
            if (s) { o = (IUnknown *)&s->IDirectMusicStyle8_iface; style_load_cmaps(s, data, size, l); }
        } else {
            dm_band *x = size >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "DMBD", 4) ?
                         dm_band_load(data + 12, size - 12, loader_getcoll, l) : NULL;
            rm_band *b = x ? new_band(x) : NULL;
            dm_band_release(x);
            if (b) o = (IUnknown *)&b->IDirectMusicBand_iface;
        }
        free(data);
        LOG_CALL("music: loaded %s -> %p", isstyle ? "style" : "band", (void *)o);
        if (!o) return FAILED(hr) ? hr : DMUS_E_UNSUPPORTED_STREAM;
        if (path[0] && isstyle) { lstrcpynW(style_of(o)->desc.wszFileName, path, DMUS_MAX_FILENAME); style_of(o)->desc.dwValidData |= DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH; }
        const GUID *cg = g;
        if (!cg && isstyle && style_of(o)->s->hasguid) cg = (const GUID *)&style_of(o)->s->guid;
        cache_add(l, path, cg, o);
        hr = IUnknown_QueryInterface(o, riid, out);
        IUnknown_Release(o);
        return hr;
    }
    if ((d->dwValidData & DMUS_OBJ_CLASS) && IsEqualCLSID(&d->guidClass, &RM_CLSID_DirectMusicChordMap)) {
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
        IUnknown *o = NULL;
        HRESULT hr = chordmap_from_data(data, size, &o);
        free(data);
        LOG_CALL("music: loaded chord map -> %08lx", (unsigned long)hr);
        if (FAILED(hr)) return hr;
        dmc_chordmap *m = chordmap_engine_of(o);
        cache_add(l, path, g ? g : (const GUID *)&m->guid, o);
        hr = IUnknown_QueryInterface(o, riid, out);
        IUnknown_Release(o);
        return hr;
    }
    if ((d->dwValidData & DMUS_OBJ_CLASS) && IsEqualCLSID(&d->guidClass, &CLSID_DirectMusicAudioPathConfig)) {
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
        IUnknown *o = NULL;
        HRESULT hr = rm_apcfg_load(data, size, &o);
        free(data);
        LOG_CALL("music: loaded audiopath configuration -> %08lx", (unsigned long)hr);
        if (FAILED(hr)) return hr;
        cache_add(l, path, g, o);
        hr = IUnknown_QueryInterface(o, riid, out);
        IUnknown_Release(o);
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
    dm_segment *sgt = segment_from_data(data, size, l);
    HRESULT ghr = S_OK;
    IDirectMusicGraph *gr = sgt ? segment_graph_from_data(data, size, &ghr) : NULL;
    free(data);
    if (!sgt) return DMUS_E_UNSUPPORTED_STREAM;
    if (gr && FAILED(ghr)) { IDirectMusicGraph_Release(gr); dm_segment_release(sgt); return ghr; }
    rm_segment *s = new_segment(sgt);
    if (!s) { if (gr) IDirectMusicGraph_Release(gr); dm_segment_release(sgt); return E_OUTOFMEMORY; }
    s->graph = gr;
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
/* test hooks: REDM_TIME fixes time() for the rand() seeds (as tests do to Microsoft's DLLs); REDM_RANDLOG
 * logs the rand() calls (R: the application thread's, T: the transport thread's) */
static long g_fixtime;
static long fixed_time(void) { return g_fixtime; }
static FILE *g_randlog;
static void randlog(int app, int v, void *caller) { if (g_randlog) { fprintf(g_randlog, "%c %p %d\n", app ? 'R' : 'T', caller, v); fflush(g_randlog); } }
static void test_hooks(void)
{
    static int done;
    char b[MAX_PATH];
    if (done) return;
    done = 1;
    if (GetEnvironmentVariableA("REDM_TIME", b, sizeof(b))) { g_fixtime = strtol(b, NULL, 0); dm_set_time_source(fixed_time); }
    if (GetEnvironmentVariableA("REDM_RANDLOG", b, sizeof(b)) && (g_randlog = fopen(b, "w"))) dm_rand_hook = randlog;
}

HRESULT rm_create(REFCLSID clsid, REFIID riid, void **out)
{
    char b1[64], b2[64];
    LOG_CALL("music: create %s (%s)", guidstr(clsid, b1), guidstr(riid, b2));
    *out = NULL;
    test_hooks();
    dm_wave_set_decoder(acm_decode);
    if (IsEqualCLSID(clsid, &RM_CLSID_DirectSoundWave)) {
        rm_wave *o = new_wave(NULL);
        if (!o) return E_OUTOFMEMORY;
        HRESULT hr = wvo_qi(&o->IDirectMusicObject_iface, riid, out);
        wvo_release(&o->IDirectMusicObject_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicLoader)) {
        rm_loader *l = calloc(1, sizeof(*l));
        if (!l) return E_OUTOFMEMORY;
        l->IDirectMusicLoader8_iface.lpVtbl = &ld_vtbl;
        l->ref = 1;
        g_styleloader = l;
        l->cache = TRUE;
        HRESULT hr = ld_qi(&l->IDirectMusicLoader8_iface, riid, out);
        ld_release(&l->IDirectMusicLoader8_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicPerformance)) {
        rm_perf *p = calloc(1, sizeof(*p));
        if (!p) return E_OUTOFMEMORY;
        p->IDirectMusicPerformance8_iface.lpVtbl = &pf_vtbl;
        p->IDirectMusicTool_iface.lpVtbl = &pft_vtbl;
        p->IDirectMusicGraph_iface.lpVtbl = &pfg_vtbl;
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
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicGraph)) return rm_graph_create(riid, out);
    if (IsEqualCLSID(clsid, &RM_CLSID_DirectMusicStyle)) {
        rm_style *s = new_style(NULL);
        if (!s) return E_OUTOFMEMORY;
        HRESULT hr = sty_qi(&s->IDirectMusicStyle8_iface, riid, out);
        sty_release(&s->IDirectMusicStyle8_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &RM_CLSID_DirectMusicBand)) {
        rm_band *b = new_band(NULL);
        if (!b) return E_OUTOFMEMORY;
        HRESULT hr = bnd_qi(&b->IDirectMusicBand_iface, riid, out);
        bnd_release(&b->IDirectMusicBand_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &RM_CLSID_DirectMusicComposer)) return composer_create(riid, out);
    if (IsEqualCLSID(clsid, &RM_CLSID_DirectMusicChordMap)) {
        rm_chordmap *m = new_chordmap(NULL);
        if (!m) return E_OUTOFMEMORY;
        HRESULT hr = cmp_qi(&m->IDirectMusicChordMap_iface, riid, out);
        cmp_release(&m->IDirectMusicChordMap_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicAudioPathConfig)) return rm_apcfg_create(riid, out);
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
           IsEqualCLSID(clsid, &CLSID_DirectMusicSegment) || IsEqualCLSID(clsid, &CLSID_DirectMusic) ||
           IsEqualCLSID(clsid, &CLSID_DirectMusicGraph) || IsEqualCLSID(clsid, &CLSID_DirectMusicAudioPathConfig) ||
           IsEqualCLSID(clsid, &RM_CLSID_DirectSoundWave) || IsEqualCLSID(clsid, &RM_CLSID_DirectMusicStyle) ||
           IsEqualCLSID(clsid, &RM_CLSID_DirectMusicBand) ||
           IsEqualCLSID(clsid, &RM_CLSID_DirectMusicChordMap) || IsEqualCLSID(clsid, &RM_CLSID_DirectMusicComposer);
}
