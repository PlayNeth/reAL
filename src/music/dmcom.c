/* reDM: the DirectMusic COM objects a game creates (dmloader/dmime/dmusic classes), backed by the ported
 * performance engine.  Covers what DirectMusic games built on segments use: the loader's objects (the loader and
 * containers are dmldr.c), the performance (Init/InitAudio, standard audiopaths, PlaySegment(Ex), Stop(Ex), IsPlaying,
 * time conversion, global and track parameters, messages and notifications), segments, segment states and audiopaths
 * (Activate, SetVolume), and tool graphs with application tools (the graph object itself is dmgraph.c).  Ports are
 * dmport.c, scripts dmscript.c.
 * One translation unit: this file holds the object layouts, the master clock, IDirectMusic8, loader references and
 * class creation, and includes the rest in order (dmcomcoll.c collections, dmcomseg.c segments and parameters,
 * dmcomtrk.c tracks, dmcomstate.c segment states, dmcompath.c audiopaths, dmcomperf.c / dmcomperf8.c the performance,
 * dmcomtool.c tool graphs, dmcomsty.c styles and bands, dmcomcmp.c chordmaps and the composer, dmcompattern.c the
 * pattern track). */
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
#include "dmldr.h"
#include "dmscript.h"
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
/* a reference on an object met through the engine's weak pointer to it, unless its last one has already gone (its
 * Release waits for the lock to unhook it, then frees it): then it counts as not there */
static BOOL ref_if_alive(LONG *r)
{
    LONG n = *(volatile LONG *)r;
    while (n > 0) {
        LONG o = InterlockedCompareExchange(r, n + 1, n);
        if (o == n) return TRUE;
        n = o;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ objects */
typedef struct rm_perf rm_perf;
typedef struct rm_track rm_track;

typedef struct {
    IDirectMusicSegment8 IDirectMusicSegment8_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dm_segment *seg;
    DMUS_OBJECTDESC desc;
    IDirectMusicGraph *graph;               /* +0x44 SetGraph (a segment state starts with a clone of it) */
    IDispatch *autod;                       /* its automation (made on first QueryInterface) */
    int ver;                                /* +0x3bc: 2 or 8 once asked for IDirectMusicSegment2 / 8 (77311442), as its
                                             * clones are; InitPlay is there only for a segment never asked for them */
} rm_segment;

typedef struct {
    IDirectMusicSegmentState8 IDirectMusicSegmentState8_iface;
    IDirectMusicGraph IDirectMusicGraph_iface;  /* +0x04 its StampPMsg: the state's graph, then the path's */
    LONG ref;
    dm_segstate *ss;
    rm_segment *seg;                        /* may be NULL (a state first met in a notification) */
    rm_perf *perf;                          /* (no reference on it for a state from Segment::InitPlay: ss NULL) */
    BOOL lazy;                              /* made for a notification: holds an ordinary reference */
    IDirectMusicAudioPath *path;            /* the audiopath it plays on (the default one when none was given) */
    int xref;                               /* further engine references it drops when it goes */
    IDispatch *autod;                       /* its automation (made on first QueryInterface) */
    dm_segstate *bare;                      /* Segment::InitPlay's: the engine state tracks play on (no segment, no
                                             * audiopath, no graph: the performance's), made on a track's first use */
} rm_state;

typedef struct {
    IDirectMusicAudioPath IDirectMusicAudioPath_iface;
    IDirectMusicGraph IDirectMusicGraph_iface;  /* +0x04 its StampPMsg: the path's graph, then the performance's */
    LONG ref;
    dm_audiopath *ap;
    rm_perf *perf;
    DWORD type, npch;
    /* a path made from a configuration (77320f10): its buffers (+0x58 the top-level ones, +0x54 the ports') */
    BOOL cfg;
    struct rm_bref *top; DWORD ntop;
    struct rm_pcfg *ports; DWORD nports;
    struct rm_buf **held; DWORD nheld;      /* every buffer reference the path holds */
    IDispatch *autod;                       /* its automation (made on first QueryInterface) */
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
    void *revobj;                           /* its DMO interface while one is out (one object, as the buffer's DMO) */
    rm_path *defpath;
    DWORD audioflags;                       /* InitAudio's (DMUS_AUDIOF_*) */
    int apstate;                            /* +0x108: 0 not set up (or closed down), 1 Init's, 2 InitAudio's (audiopaths) */
    rm_port *port;                          /* the synth port (dmport.c) */
    /* reAL: the port behind each of the engine's port indices: [0] the synth port, the others CreatePort'ed ones
     * (references); and whether each is AddPort'ed (under the engine lock) */
    rm_port *slot[8];
    BYTE added[8];
    rm_buf *bufs;
    IDirectMusic8 *dm;
    BOOL closed;
    /* dmime's version (+0x370): 6 when created, 7 after a QueryInterface for IDirectMusicPerformance2, 8 after one for
     * IDirectMusicPerformance8, each setting it again.  Below 8: the graph works before Init, Init keeps the prepare
     * time and bumper length, and the DirectX 8 behaviours of the engine (dxver) are off. */
    volatile LONG ver;
    IDirectMusicGraph *pregraph;            /* SetGraph's before Init (a reference; the engine's from Init on) */
    DWORD prepare_ms, bumper_ms;            /* Set{PrepareTime,BumperLength}'s before Init */
    HANDLE notify;
    IDispatch *autod;                       /* its automation (dmscript's, aggregated: made on first QueryInterface) */
    struct rm_global { struct rm_global *next; GUID g; DWORD size; BYTE *data; } *globals;   /* +0x104 */
    CRITICAL_SECTION gcs;
    struct rm_perf *pnext;                  /* reAL: the performances with a host (s_perfs) */
};

typedef struct {
    IDirectMusicCollection IDirectMusicCollection_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dm_collection *c;
    DMUS_OBJECTDESC desc;
    struct rm_instrument *ins;          /* its instrument objects (GetInstrument), not references (s_inslock) */
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

static const GUID RM_GUID_DefaultGMCollection = { 0xf17e8673, 0xc3b4, 0x11d1, { 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID RM_CLSID_DirectSoundWave = { 0x8a667154, 0xf9cb, 0x11d2, { 0xad, 0x8a, 0x00, 0x60, 0xb0, 0x57, 0x5a, 0xbc } };

static char *guidstr(const GUID *g, char b[64]) { return (char *)rl_guid_str(g, b); }

/* the form at the stream's position (the loader's file streams have no Stat) */
static BOOL read_stream(IStream *s, BYTE **data, DWORD *size) { return rm_read_form(s, data, size); }

/* ------------------------------------------------------------------ the master clock */
/* the performances with a host, oldest first (under the engine lock): the master clock is the oldest one's, and
 * after it is gone the next one's (never earlier than a time it gave) */
static rm_perf *s_perfs;
static REFERENCE_TIME s_cklast;
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
    dm_engine_lock();
    REFERENCE_TIME x;
    if (s_perfs) x = dm_host_clock(s_perfs->host);
    else { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); x = (REFERENCE_TIME)((double)c.QuadPart * 1e7 / (double)f.QuadPart); }
    if (x < s_cklast) x = s_cklast;
    s_cklast = x;
    dm_engine_unlock();
    *t = x;
    return S_OK;
}
/* dmusic's master clock does not do these either (tests/dx6test.c clock: E_NOTIMPL, E_POINTER for no cookie) */
static HRESULT WINAPI ck_advise(IReferenceClock *i, REFERENCE_TIME a, REFERENCE_TIME b, HEVENT e, DWORD_PTR *c) { return c ? E_NOTIMPL : E_POINTER; }
static HRESULT WINAPI ck_advper(IReferenceClock *i, REFERENCE_TIME a, REFERENCE_TIME b, HSEMAPHORE s, DWORD_PTR *c) { return c ? E_NOTIMPL : E_POINTER; }
static HRESULT WINAPI ck_unadvise(IReferenceClock *i, DWORD_PTR c) { return E_NOTIMPL; }
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
static ULONG WINAPI dmu_release(IDirectMusic8 *i)
{
    LONG r = InterlockedDecrement(&dmu(i)->ref);
    if (!r) { rm_ports_disown(dmu(i)); free(dmu(i)); }
    return r;
}
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
    lstrcpyW(c->wszDescription, L"Microsoft Synthesizer");   /* (as dmusic lists it, and GetCaps names it) */
    return S_OK;
}
static HRESULT WINAPI dmu_createbuffer(IDirectMusic8 *i, LPDMUS_BUFFERDESC d, LPDIRECTMUSICBUFFER *b, LPUNKNOWN o) { return rm_mbuf_create(d, b, o); }
/* a synth port of its own each time (dmport.c), as dmusic's; GUID_NULL is the default port, the synth */
static HRESULT WINAPI dmu_createport(IDirectMusic8 *i, REFCLSID c, LPDMUS_PORTPARAMS p, LPDIRECTMUSICPORT *port, LPUNKNOWN o)
{
    return rm_port_create(dmu(i), c, p, port, o);
}
static HRESULT WINAPI dmu_enumclock(IDirectMusic8 *i, DWORD n, LPDMUS_CLOCKINFO c) { return n ? S_FALSE : E_NOTIMPL; }
static HRESULT WINAPI dmu_getclock(IDirectMusic8 *i, LPGUID g, IReferenceClock **c)
{
    if (g) memset(g, 0, sizeof(*g));
    if (c && !(*c = new_clock())) return E_OUTOFMEMORY;
    return S_OK;
}
static HRESULT WINAPI dmu_setclock(IDirectMusic8 *i, REFGUID g) { return S_OK; }
static HRESULT WINAPI dmu_activate(IDirectMusic8 *i, BOOL on) { return rm_ports_activate(dmu(i), on); }
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

/* one of reDM's own objects, by its vtable: the interface as given, else the one it answers for iid (released at
 * once: the caller holds the object).  Used by the unsafe_* lookups below (one per class) and the performance's */
static void *rm_ours(IUnknown *u, REFIID iid, const void *vtbl)
{
    if (!u) return NULL;
    if (*(void *const *)u == vtbl) return u;
    void *q;
    if (SUCCEEDED(IUnknown_QueryInterface(u, iid, &q))) {
        IUnknown_Release((IUnknown *)q);
        if (*(void *const *)q == vtbl) return q;
    }
    return NULL;
}

#include "dmcomcoll.c"
#include "dmcomseg.c"
#include "dmcomtrk.c"
#include "dmcomstate.c"
#include "dmcompath.c"
#include "dmcomperf.c"
#include "dmcomtool.c"
#include "dmcomperf8.c"
/* ------------------------------------------------------------------ references through the loader (dmldr.c) */
/* GetObject of a reference by class, guid, file and name (a file name with a drive or a root taken as a full path) */
static HRESULT ldr_get(IDirectMusicLoader8 *l, REFCLSID cls, REFIID iid, const dm_guid *g, const char *file,
                       const char *name, void **out)
{
    *out = NULL;
    if (!l) return E_FAIL;
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS;
    d.guidClass = *cls;
    if (g) { memcpy(&d.guidObject, g, sizeof(GUID)); d.dwValidData |= DMUS_OBJ_OBJECT; }
    if (file) {
        MultiByteToWideChar(CP_ACP, 0, file, -1, d.wszFileName, DMUS_MAX_FILENAME);
        d.dwValidData |= DMUS_OBJ_FILENAME;
        if (file[0] && (file[1] == ':' || file[0] == '\\')) d.dwValidData |= DMUS_OBJ_FULLPATH;
    }
    if (name) { MultiByteToWideChar(CP_ACP, 0, name, -1, d.wszName, DMUS_MAX_NAME); d.dwValidData |= DMUS_OBJ_NAME; }
    return IDirectMusicLoader8_GetObject(l, &d, iid, out);
}

/* a band's DLS reference (or the default GM set) while loading */
static dm_collection *loader_getcoll(void *ctx, const dm_guid *g, const char *file)
{
    IDirectMusicLoader8 *l = ctx;
    BOOL gm = !g || !memcmp(g, &RM_GUID_DefaultGMCollection, sizeof(GUID));
    if (!l) {                           /* (a stream of the application's own: no loader to ask) */
        if (file) { dm_collection *c = dm_collection_from_file(file, 0); if (c) return c; }
        return gm ? dm_collection_from_file(dm_gm_dls_path(), 1) : NULL;
    }
    IDirectMusicCollection *o = NULL;
    HRESULT hr = ldr_get(l, &CLSID_DirectMusicCollection, &IID_IDirectMusicCollection,
                         !g && !file ? (const dm_guid *)&RM_GUID_DefaultGMCollection : g, file, NULL, (void **)&o);
    if (FAILED(hr) || !o) {
        LOG_INFO("music: collection %s not found (%08lx)", file ? file : "(by guid)", (unsigned long)hr);
        return NULL;
    }
    rm_collection *c = col_of((IUnknown *)o);
    dm_collection *r = c ? col_hold(c) : NULL;     /* the caller's reference: the object may go with this Release */
    IDirectMusicCollection_Release(o);
    return r;
}

#include "dmcomsty.c"
#include "dmcomcmp.c"

#include "dmcompattern.c"
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
    dm_unk_ref = unk_ref;
    dms_resort_hook = resort_hook;
    if (IsEqualCLSID(clsid, &RM_CLSID_DirectSoundWave)) {
        rm_wave *o = new_wave(NULL);
        if (!o) return E_OUTOFMEMORY;
        HRESULT hr = wvo_qi(&o->IDirectMusicObject_iface, riid, out);
        wvo_release(&o->IDirectMusicObject_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicLoader)) return rm_loader_create(riid, out);
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicContainer)) return rm_container_create(riid, out);
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicCollection)) {
        rm_collection *c = new_collection(NULL);
        if (!c) return E_OUTOFMEMORY;
        HRESULT hr = col_qi(&c->IDirectMusicCollection_iface, riid, out);
        col_release(&c->IDirectMusicCollection_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicPerformance)) {
        rm_perf *p = calloc(1, sizeof(*p));
        if (!p) return E_OUTOFMEMORY;
        p->IDirectMusicPerformance8_iface.lpVtbl = &pf_vtbl;
        p->IDirectMusicTool_iface.lpVtbl = &pft_vtbl;
        p->IDirectMusicGraph_iface.lpVtbl = &pfg_vtbl;
        p->ref = 1;
        p->ver = 6;                         /* (7730a9ab; the QueryInterface below may raise it) */
        p->prepare_ms = 1000; p->bumper_ms = 50;
        InitializeCriticalSection(&p->gcs);
        HRESULT hr = pf_qi(&p->IDirectMusicPerformance8_iface, riid, out);
        pf_release(&p->IDirectMusicPerformance8_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicSegment)) {
        dm_segment *g = dm_segment_new();
        rm_segment *s = g ? new_segment(g) : NULL;
        if (!s) { dm_segment_release(g); return E_OUTOFMEMORY; }
        s->desc.dwValidData = 0;            /* (not loaded) */
        HRESULT hr = seg_qi(&s->IDirectMusicSegment8_iface, riid, out);
        seg_release(&s->IDirectMusicSegment8_iface);
        return hr;
    }
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicGraph)) return rm_graph_create(riid, out);
    if (dm_track_class_kind((const dm_guid *)clsid) >= 0) {     /* a track class: the track with no items */
        dm_track *t = dm_track_create((const dm_guid *)clsid);
        if (!t) return E_OUTOFMEMORY;
        IDirectMusicTrack8 *o = track_object(t);
        dm_track_release(t);
        if (!o) return E_OUTOFMEMORY;
        HRESULT hr = trk_qi(o, riid, out);
        trk_release(o);
        return hr;
    }
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
    if (IsEqualCLSID(clsid, &CLSID_DirectMusicScript)) return rm_script_create(riid, out);
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
           IsEqualCLSID(clsid, &CLSID_DirectMusicContainer) || IsEqualCLSID(clsid, &CLSID_DirectMusicCollection) ||
           dm_track_class_kind((const dm_guid *)clsid) >= 0 ||
           IsEqualCLSID(clsid, &CLSID_DirectMusicSegment) || IsEqualCLSID(clsid, &CLSID_DirectMusic) ||
           IsEqualCLSID(clsid, &CLSID_DirectMusicGraph) || IsEqualCLSID(clsid, &CLSID_DirectMusicAudioPathConfig) ||
           IsEqualCLSID(clsid, &RM_CLSID_DirectSoundWave) || IsEqualCLSID(clsid, &RM_CLSID_DirectMusicStyle) ||
           IsEqualCLSID(clsid, &RM_CLSID_DirectMusicBand) || IsEqualCLSID(clsid, &CLSID_DirectMusicScript) ||
           IsEqualCLSID(clsid, &RM_CLSID_DirectMusicChordMap) || IsEqualCLSID(clsid, &RM_CLSID_DirectMusicComposer);
}
