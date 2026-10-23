/* reDM: the DirectMusic COM objects a game creates (dmloader/dmime/dmusic classes), backed by the ported
 * performance engine.  Covers what DirectMusic games built on segments use: the loader's objects (the loader and
 * containers are dmldr.c), the performance (Init/InitAudio, standard audiopaths, PlaySegment(Ex), Stop(Ex), IsPlaying,
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
    IDispatch *autod;                       /* its automation (made on first QueryInterface) */
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
    rm_path *defpath;
    DWORD audioflags;                       /* InitAudio's (DMUS_AUDIOF_*) */
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
    lstrcpyW(c->wszDescription, L"Microsoft Synthesizer");   /* (as dmusic lists it, and GetCaps names it) */
    return S_OK;
}
static HRESULT WINAPI dmu_createbuffer(IDirectMusic8 *i, LPDMUS_BUFFERDESC d, LPDIRECTMUSICBUFFER *b, LPUNKNOWN o) { return rm_mbuf_create(d, b, o); }
/* a synth port of its own each time (dmport.c), as dmusic's; GUID_NULL is the default port, the synth */
static HRESULT WINAPI dmu_createport(IDirectMusic8 *i, REFCLSID c, LPDMUS_PORTPARAMS p, LPDIRECTMUSICPORT *port, LPUNKNOWN o)
{
    return rm_port_create(c, p, port, o);
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
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &c->IPersistStream_iface;
    else { return rm_noiface(__func__, riid, out); }
    InterlockedIncrement(&c->ref);
    return S_OK;
}
static ULONG WINAPI col_addref(IDirectMusicCollection *i) { return InterlockedIncrement(&col(i)->ref); }
static ULONG WINAPI col_release(IDirectMusicCollection *i)
{
    LONG r = InterlockedDecrement(&col(i)->ref);
    LOG_CALL("music: Collection::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) { dls_release(col(i)->c); free(col(i)); }   /* (a file's collection stays in the process-wide cache) */
    return r;
}
/* reAL: the engine collection with a reference (dls_release), read under the engine lock: a Load on another thread
 * replaces it and lets go of the old one */
static dm_collection *col_hold(rm_collection *o)
{
    dm_engine_lock();
    dm_collection *c = o->c;
    dls_addref(c);
    dm_engine_unlock();
    return c;
}
/* ------------------------------------------------------------------ instruments (Collection::GetInstrument) */
/* dmusic's CInstrument: the collection's instrument and a patch the application may change (SetPatch takes any
 * value).  The collection hands out one object per instrument while it lives; the object holds the collection. */
typedef struct rm_instrument {
    IDirectMusicInstrument IDirectMusicInstrument_iface;
    LONG ref;
    struct rm_instrument *next;         /* on its collection's list (s_inslock) */
    rm_collection *owner;               /* a reference */
    dm_collection *c;                   /* a reference: the collection as it was loaded then */
    DWORD orig;                         /* its patch in the collection */
    volatile LONG patch;                /* the one it downloads as */
    LONG serial;                        /* tells it from a later object at the same address */
} rm_instrument;
static SRWLOCK s_inslock = SRWLOCK_INIT;
static LONG s_insserial;
static IDirectMusicInstrumentVtbl ins_vtbl;
static rm_instrument *ins(IDirectMusicInstrument *i) { return impl_from(i, rm_instrument, IDirectMusicInstrument_iface); }
static HRESULT WINAPI ins_qi(IDirectMusicInstrument *i, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicInstrument)) *out = i;
    else return rm_noiface(__func__, riid, out);
    InterlockedIncrement(&ins(i)->ref);
    return S_OK;
}
static ULONG WINAPI ins_addref(IDirectMusicInstrument *i) { return InterlockedIncrement(&ins(i)->ref); }
static ULONG WINAPI ins_release(IDirectMusicInstrument *i)
{
    rm_instrument *o = ins(i);
    AcquireSRWLockExclusive(&s_inslock);        /* (GetInstrument takes a reference under it) */
    LONG r = InterlockedDecrement(&o->ref);
    if (!r) {
        rm_instrument **pp = &o->owner->ins;
        while (*pp && *pp != o) pp = &(*pp)->next;
        if (*pp) *pp = o->next;
    }
    ReleaseSRWLockExclusive(&s_inslock);
    if (!r) {
        dls_release(o->c);
        col_release(&o->owner->IDirectMusicCollection_iface);
        free(o);
    }
    return r;
}
static HRESULT WINAPI ins_getpatch(IDirectMusicInstrument *i, DWORD *patch)
{
    if (!patch) return E_POINTER;
    *patch = (DWORD)ins(i)->patch;
    return S_OK;
}
static HRESULT WINAPI ins_setpatch(IDirectMusicInstrument *i, DWORD patch)
{
    InterlockedExchange(&ins(i)->patch, (LONG)patch);
    return S_OK;
}
static IDirectMusicInstrumentVtbl ins_vtbl = { ins_qi, ins_addref, ins_release, ins_getpatch, ins_setpatch };
/* for the port's DownloadInstrument: one of ours, its collection (a reference: dls_release), the patch it has in it,
 * the patch to download it as, and its serial */
int rm_instrument_get(IDirectMusicInstrument *i, dm_collection **c, uint32_t *orig, uint32_t *patch, LONG *serial)
{
    if (!i || IsBadReadPtr(i, sizeof(void *)) || i->lpVtbl != &ins_vtbl) return 0;
    rm_instrument *o = ins(i);
    dls_addref(o->c);
    *c = o->c;
    *orig = o->orig;
    *patch = (uint32_t)o->patch;
    *serial = o->serial;
    return 1;
}

/* 0x88781114 (DMUS_E_INVALIDPATCH) for a patch the collection lacks, as dmusic answers; asking again for one while
 * its object lives hands out the same object */
static HRESULT WINAPI col_getinst(IDirectMusicCollection *i, DWORD patch, IDirectMusicInstrument **out)
{
    rm_collection *cl = col(i);
    if (!out || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    *out = NULL;
    dm_collection *c = col_hold(cl);
    if (!c || dls_find(c, patch) < 0) { dls_release(c); return DMUS_E_INVALIDPATCH; }
    rm_instrument *o = calloc(1, sizeof(*o));
    if (!o) { dls_release(c); return E_OUTOFMEMORY; }
    AcquireSRWLockExclusive(&s_inslock);
    for (rm_instrument *x = cl->ins; x; x = x->next)
        if (x->orig == patch && x->c == c) { InterlockedIncrement(&x->ref); *out = &x->IDirectMusicInstrument_iface; break; }
    if (!*out) {
        o->IDirectMusicInstrument_iface.lpVtbl = &ins_vtbl;
        o->ref = 1;
        o->owner = cl;
        InterlockedIncrement(&cl->ref);
        o->c = c; c = NULL;
        o->orig = patch;
        o->patch = (LONG)patch;
        o->serial = InterlockedIncrement(&s_insserial);
        o->next = cl->ins;
        cl->ins = o;
        *out = &o->IDirectMusicInstrument_iface;
        o = NULL;
    }
    ReleaseSRWLockExclusive(&s_inslock);
    free(o);
    dls_release(c);
    return S_OK;
}
static HRESULT WINAPI col_enum(IDirectMusicCollection *i, DWORD n, DWORD *patch, LPWSTR name, DWORD len)
{
    const char *nm;
    uint32_t p;
    dm_collection *c = col_hold(col(i));
    HRESULT hr = S_FALSE;
    if (c && dls_enum(c, (int)n, &p, &nm) >= 0) {
        if (patch) *patch = p;
        if (name && len) MultiByteToWideChar(CP_ACP, 0, nm, -1, name, (int)len);
        hr = S_OK;
    }
    dls_release(c);
    return hr;
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
static HRESULT WINAPI colo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_get(&colo(i)->desc, &CLSID_DirectMusicCollection, d); }
static HRESULT WINAPI colo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_set(&colo(i)->desc, d, TRUE); }
/* a DLS form's 'dlid', 'vers' and INFO list's INAM (dmusic's collection) */
static void dls_desc(const BYTE *b, DWORD n, DMUS_OBJECTDESC *d)
{
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DLS ", 4)) return;
    DWORD end = 8 + *(const DWORD *)(b + 4);
    if (end > n) end = n;
    for (DWORD off = 12; off + 8 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4), cn = end - (off + 8) < sz ? end - (off + 8) : sz;
        const BYTE *c = b + off + 8;
        if (!memcmp(b + off, "dlid", 4) && cn >= 16) { memcpy(&d->guidObject, c, 16); d->dwValidData |= DMUS_OBJ_OBJECT; }
        else if (!memcmp(b + off, "vers", 4) && cn >= 8) { memcpy(&d->vVersion, c, 8); d->dwValidData |= DMUS_OBJ_VERSION; }
        else if (!memcmp(b + off, "LIST", 4) && cn >= 4 && !memcmp(c, "INFO", 4)) {
            for (DWORD q = 4; q + 8 <= cn; ) {
                DWORD qs = *(const DWORD *)(c + q + 4), qn = cn - (q + 8) < qs ? cn - (q + 8) : qs;
                if (!memcmp(c + q, "INAM", 4)) {
                    char nm[DMUS_MAX_NAME];
                    DWORD k = qn < DMUS_MAX_NAME - 1 ? qn : DMUS_MAX_NAME - 1;
                    memcpy(nm, c + q + 8, k); nm[k] = 0;
                    MultiByteToWideChar(CP_ACP, 0, nm, -1, d->wszName, DMUS_MAX_NAME);
                    d->dwValidData |= DMUS_OBJ_NAME;
                }
                q += 8 + qn + (qn & 1);
            }
        }
        off += 8 + cn + (cn & 1);
    }
}
static HRESULT WINAPI colo_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d)
{
    if (!s || !d) return E_POINTER;
    BYTE h[12];
    ULONG got = 0;
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER at;
    if (FAILED(IStream_Seek(s, z, STREAM_SEEK_CUR, &at))) return E_FAIL;
    if (FAILED(IStream_Read(s, h, 12, &got)) || got != 12) return DMUS_E_INVALIDFILE;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "DLS ", 4)) return DMUS_E_NOTADLSCOL;
    /* the collection's header chunks only (an instrument set can be large) */
    DWORD n = *(DWORD *)(h + 4), cap = 12, len = 12;
    BYTE *b = malloc(4096);
    if (!b) return E_OUTOFMEMORY;
    cap = 4096;
    memcpy(b, h, 12);
    for (ULONGLONG used = 4; used + 8 <= n; ) {
        BYTE ch[8];
        if (FAILED(IStream_Read(s, ch, 8, &got)) || got != 8) break;
        DWORD cs = *(DWORD *)(ch + 4);
        used += 8 + (ULONGLONG)cs + (cs & 1);
        BOOL want = !memcmp(ch, "dlid", 4) || !memcmp(ch, "vers", 4) || !memcmp(ch, "LIST", 4);
        if (want && !memcmp(ch, "LIST", 4)) {
            BYTE t[4];
            if (FAILED(IStream_Read(s, t, 4, &got)) || got != 4) break;
            LARGE_INTEGER back; back.QuadPart = -4;
            IStream_Seek(s, back, STREAM_SEEK_CUR, NULL);
            want = !memcmp(t, "INFO", 4);
        }
        ULONGLONG pad = (ULONGLONG)cs + (cs & 1);
        if (want && pad > 0x100000) break;      /* (a header chunk of 1 MB is a broken size: no growing to it) */
        if (want) {
            if (len + 8 + pad > cap) { while (len + 8 + pad > cap) cap *= 2; BYTE *nb = realloc(b, cap); if (!nb) break; b = nb; }
            memcpy(b + len, ch, 8);
            if (FAILED(IStream_Read(s, b + len + 8, (ULONG)pad, &got)) || got < cs) break;
            len += 8 + pad;
        } else {
            LARGE_INTEGER sk; sk.QuadPart = (LONGLONG)pad;
            if (FAILED(IStream_Seek(s, sk, STREAM_SEEK_CUR, NULL))) break;
        }
    }
    *(DWORD *)(b + 4) = len - 8;
    d->dwValidData = DMUS_OBJ_CLASS;
    d->guidClass = CLSID_DirectMusicCollection;
    dls_desc(b, len, d);
    free(b);
    return S_OK;
}
static IDirectMusicObjectVtbl colo_vtbl = { colo_qi, colo_addref, colo_release, colo_getdesc, colo_setdesc, colo_parse };

static rm_collection *colp(IPersistStream *i) { return impl_from(i, rm_collection, IPersistStream_iface); }
static HRESULT WINAPI colp_qi(IPersistStream *i, REFIID r, void **o) { return col_qi(&colp(i)->IDirectMusicCollection_iface, r, o); }
static ULONG WINAPI colp_addref(IPersistStream *i) { return col_addref(&colp(i)->IDirectMusicCollection_iface); }
static ULONG WINAPI colp_release(IPersistStream *i) { return col_release(&colp(i)->IDirectMusicCollection_iface); }
static HRESULT WINAPI colp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = CLSID_DirectMusicCollection; return S_OK; }
static HRESULT WINAPI colp_isdirty(IPersistStream *i) { return S_FALSE; }
/* a collection from the loader's file is read through the process-wide collection cache (a SoundFont converted, the
 * default GM set marked as such); any other stream's DLS form is parsed as it is */
static HRESULT WINAPI colp_load(IPersistStream *i, IStream *st)
{
    rm_collection *o = colp(i);
    if (!st) return E_POINTER;
    const WCHAR *path = rm_stream_path(st);
    dm_collection *c = NULL;
    if (path) {
        char a[MAX_PATH], full[MAX_PATH], gm[MAX_PATH];
        if (!WideCharToMultiByte(CP_ACP, 0, path, -1, a, MAX_PATH, NULL, NULL)) return DMUS_E_LOADER_FAILEDOPEN;
        if (!GetFullPathNameA(a, MAX_PATH, full, NULL)) lstrcpynA(full, a, MAX_PATH);
        const char *g = dm_gm_dls_path();
        if (!g || !GetFullPathNameA(g, MAX_PATH, gm, NULL)) gm[0] = 0;
        c = dm_collection_from_file(full, gm[0] && !lstrcmpiA(full, gm));
        if (!c) return DMUS_E_NOTADLSCOL;
        /* the descriptor from the form's header */
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        if (SUCCEEDED(colo_parse(&o->IDirectMusicObject_iface, st, &d)))
            o->desc.dwValidData |= d.dwValidData & ~DMUS_OBJ_CLASS, o->desc.guidObject = d.guidObject,
            o->desc.vVersion = d.vVersion, memcpy(o->desc.wszName, d.wszName, sizeof(d.wszName));
    } else {
        BYTE *b; DWORD n;
        if (!read_stream(st, &b, &n)) return DMUS_E_INVALIDFILE;
        if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DLS ", 4)) { free(b); return DMUS_E_NOTADLSCOL; }
        dls_desc(b, n, &o->desc);
        c = dls_load_owned(b, n, 0);
        if (!c) return DMUS_E_NOTADLSCOL;
    }
    dm_engine_lock();                   /* (col_hold reads o->c under it) */
    dm_collection *old = o->c;           /* (bands and downloads made from it keep their own references) */
    o->c = c;
    dm_engine_unlock();
    dls_release(old);
    o->desc.dwValidData |= DMUS_OBJ_LOADED;
    return S_OK;
}
static HRESULT WINAPI colp_save(IPersistStream *i, IStream *s, BOOL c) { return E_NOTIMPL; }
static HRESULT WINAPI colp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return E_NOTIMPL; }
static IPersistStreamVtbl colp_vtbl = { colp_qi, colp_addref, colp_release, colp_getclass, colp_isdirty, colp_load, colp_save, colp_getsize };

static rm_collection *new_collection(dm_collection *c)
{
    rm_collection *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicCollection_iface.lpVtbl = &col_vtbl;
    o->IDirectMusicObject_iface.lpVtbl = &colo_vtbl;
    o->IPersistStream_iface.lpVtbl = &colp_vtbl;
    o->ref = 1;
    o->c = c;
    o->desc.dwSize = sizeof(o->desc);
    o->desc.dwValidData = DMUS_OBJ_CLASS | (c ? DMUS_OBJ_LOADED : 0);
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
    else if (IsEqualIID(riid, &IID_IDispatch)) {
        if (!s->autod && !(s->autod = rm_auto_new((IUnknown *)&s->IDirectMusicSegment8_iface, RM_AUTO_SEGMENT))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = s->autod;
    }
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
        /* under the lock: the engine thread reads the wrapper (and its graph) and counts the segment under it */
        dm_engine_lock();
        if (s->seg && s->seg->wrapper == s) s->seg->wrapper = NULL;
        dm_segment_release(s->seg);
        dm_engine_unlock();
        if (s->graph) IDirectMusicGraph_Release(s->graph);
        rm_auto_free(s->autod);
        free(s);
    }
    return r;
}
/* reAL: the engine segment read with the engine lock held: a Load on another thread replaces (and may free) it */
#define SEG_GET(i, field, out) do { dm_engine_lock(); dm_segment *g_ = sgm(i)->seg; (out) = g_ ? g_->field : 0; dm_engine_unlock(); } while (0)
#define SEG_SET(i, field, v) do { dm_engine_lock(); dm_segment *g_ = sgm(i)->seg; if (g_) g_->field = (v); dm_engine_unlock(); } while (0)
static HRESULT WINAPI seg_getlength(IDirectMusicSegment8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; SEG_GET(i, length, *t); return S_OK; }
static HRESULT WINAPI seg_setlength(IDirectMusicSegment8 *i, MUSIC_TIME t) { if (t <= 0) return E_INVALIDARG; SEG_SET(i, length, t); return S_OK; }
static HRESULT WINAPI seg_getrepeats(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; SEG_GET(i, repeats, *r); return S_OK; }
static HRESULT WINAPI seg_setrepeats(IDirectMusicSegment8 *i, DWORD r) { SEG_SET(i, repeats, r); return S_OK; }
static HRESULT WINAPI seg_getres(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; SEG_GET(i, resolution, *r); return S_OK; }
static HRESULT WINAPI seg_setres(IDirectMusicSegment8 *i, DWORD r) { SEG_SET(i, resolution, r); return S_OK; }
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
    dm_engine_lock();
    IDirectMusicGraph *x = s->graph;
    if (x) { *g = x; IDirectMusicGraph_AddRef(x); }
    dm_engine_unlock();
    return x ? S_OK : DMUS_E_NOT_FOUND;
}
static HRESULT WINAPI seg_setgraph(IDirectMusicSegment8 *i, IDirectMusicGraph *g)
{
    if (g && (IsBadReadPtr(g, 4) || IsBadReadPtr(*(void **)g, 4) || IsBadCodePtr(**(FARPROC **)g))) return E_POINTER;
    rm_segment *s = sgm(i);
    if (g) IDirectMusicGraph_AddRef(g);
    dm_engine_lock();
    IDirectMusicGraph *o = s->graph;
    s->graph = g;
    dm_engine_unlock();
    if (o) IDirectMusicGraph_Release(o);
    return S_OK;
}
/* the segment's notification types are read by the performance thread: changed under the engine lock */
static HRESULT seg_notif(IDirectMusicSegment8 *i, REFGUID g, int add)
{
    rm_segment *s = sgm(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!s->seg) return S_OK;
    dm_engine_lock();
    int hr = add ? dm_segment_add_notification(s->seg, (const dm_guid *)g, 0) : dm_segment_remove_notification(s->seg, (const dm_guid *)g, 0);
    dm_engine_unlock();
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
DMG(G_Variations, 0x11f72cce, 0x26e6, 0x4ecd, 0xaf, 0x2e, 0xd6, 0x68, 0xe6, 0x67, 0x07, 0xd8);
DMG(G_PatternStyle, 0x689821f4, 0xb3bc, 0x44dd, 0x80, 0xd4, 0x0c, 0xf3, 0x2f, 0xe4, 0xd2, 0x1b);    /* dmstyle's private */
DMG(G_ScriptTrackNotify, 0x1cc7e0bf, 0x981c, 0x4b9f, 0xbe, 0x17, 0xd5, 0x72, 0xfc, 0x5f, 0xa9, 0x33);     /* dmscript's */
DMG(G_PatternPart, 0x09f25101, 0x505d, 0x4c7c, 0xbe, 0x15, 0x66, 0x2b, 0x33, 0xe7, 0x8a, 0x7c);     /* dmstyle's private */
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
    { &G_IDirectMusicStyle, PRM_STYLE }, { &G_SeedVariations, PRM_SEEDVAR }, { &G_StyleTimeSig, PRM_STYLETS }, { &G_ValidStartTime, PRM_VALIDSTART }, { &G_PlayMarker, PRM_PLAYMARKER }, { &G_IDirectMusicChordMap_, PRM_CHORDMAP },
    { &G_Variations, PRM_VARIATIONS }, { &G_PatternStyle, PRM_PATSTYLE }, { &G_PatternPart, PRM_PATPART }, { &G_ScriptTrackNotify, PRM_SCRIPTNOTIFY }, { &GUID_NULL, PRM_ANY },
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
static struct dms_style *style_hold_of(void *p);       /* dmcomsty.c: with a reference */
void dms_style_release(struct dms_style *s);            /* (dmsty.h) */
static dm_band *band_hold_of(void *p);
static struct dmc_chordmap *chordmap_engine_of(void *p);     /* dmcomcmp.c */
static void prm_out(int prm, int hr, void *p);
static rm_perf *perf_from_unknown(IUnknown *u);
static rm_path *unsafe_path(IUnknown *u);

/* the engine data a SetParam passes: a performance / audiopath for the download parameters, a collection for
 * ConnectToDLSCollection, a style, chord map or band (used under the engine lock); the collection, style and band
 * with a reference: prm_drop once the engine took its own (a Load on another thread may replace them) */
static HRESULT prm_data(int prm, void *p, dm_dltarget *dt, void **data)
{
    *data = p;
    if (prm_takes_target(prm)) {
        if (!p) return E_POINTER;
        rm_perf *pf = perf_from_unknown(p);
        if (!pf || !pf->host) return E_NOINTERFACE;
        rm_path *a = unsafe_path(p);
        dt->pf = dm_host_perf(pf->host); dt->ap = a ? a->ap : NULL;
        *data = dt;
    } else if (prm == PRM_CONNECTDLS) {
        if (!p) return E_POINTER;
        rm_collection *c = col_of(p);
        if (!c || !(*data = col_hold(c))) return E_NOINTERFACE;
    } else if (prm == PRM_STYLE) {
        if (!p) return E_POINTER;
        if (!(*data = style_hold_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_CHORDMAP) {
        if (!p) return E_POINTER;
        if (!(*data = chordmap_engine_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_IBAND) {
        if (!p) return E_POINTER;
        if (!(*data = band_hold_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_BANDPARAM) {
        if (!p || IsBadReadPtr(p, 8)) return E_POINTER;
        const DMUS_BAND_PARAM *bp = p;
        if (!(dt->bp.band = band_hold_of(bp->pBand))) return E_NOINTERFACE;
        dt->bp.physical = bp->mtTimePhysical;
        *data = &dt->bp;
    }
    return S_OK;
}
static void prm_drop(int prm, dm_dltarget *dt, void *data)
{
    if (prm == PRM_CONNECTDLS) dls_release(data);
    else if (prm == PRM_STYLE) dms_style_release(data);
    else if (prm == PRM_IBAND) dm_band_release(data);
    else if (prm == PRM_BANDPARAM) dm_band_release(dt->bp.band);
}

static HRESULT WINAPI seg_getparam(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *p)
{
    char b[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    int prm = prm_of(g);
    if ((prm == PRM_TEMPO || prm == PRM_TIMESIG) && !p) return E_POINTER;
    mtime_t next = nx ? *nx : 0;      /* untouched when no track answers */
    dm_engine_lock();
    int hr = dm_segment_getparam(sgm(i)->seg, prm, gb, n, t, nx ? &next : NULL, p);
    dm_engine_unlock();
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
    dm_dltarget dt; void *data;
    HRESULT hr = prm_data(prm, p, &dt, &data);
    if (FAILED(hr)) return hr;
    dm_engine_lock();
    hr = dm_segment_setparam(sgm(i)->seg, prm, gb, n, t, data);
    dm_engine_unlock();
    prm_drop(prm, &dt, data);
    if (prm == PRM_OTHER) LOG_INFO("music: Segment::SetParam(%s) -> %08x (not supported)", guidstr(g, b), (unsigned)hr);
    else LOG_CALL("music: Segment::SetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, (unsigned)hr);
    return hr;
}
static HRESULT WINAPI seg_clone(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicSegment **o);
static HRESULT WINAPI seg_setstart(IDirectMusicSegment8 *i, MUSIC_TIME t)
{
    HRESULT hr = S_OK;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    if (!s) hr = E_FAIL;
    else if (t < 0 || t >= s->length) hr = DMUS_E_OUT_OF_RANGE;
    else s->playstart = t;
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_getstart(IDirectMusicSegment8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; SEG_GET(i, playstart, *t); return S_OK; }
static HRESULT WINAPI seg_setloop(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b)
{
    HRESULT hr = S_OK;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    if (!s) hr = E_FAIL;
    else if ((a || b) && (a < 0 || b > s->length || a >= b)) hr = DMUS_E_OUT_OF_RANGE;
    else { s->loopstart = a; s->loopend = b; }
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_getloop(IDirectMusicSegment8 *i, MUSIC_TIME *a, MUSIC_TIME *b)
{
    if (!a || !b) return E_POINTER;
    SEG_GET(i, loopstart, *a); SEG_GET(i, loopend, *b);
    return S_OK;
}
static HRESULT WINAPI seg_setpch(IDirectMusicSegment8 *i, DWORD n, DWORD *p)
{
    if (!n) return S_OK;
    if (!p) return E_INVALIDARG;
    if (IsBadReadPtr(p, n * sizeof(DWORD))) return E_POINTER;
    if (!sgm(i)->seg) return S_OK;
    dm_engine_lock();
    HRESULT hr = dm_segment_add_pchannels(sgm(i)->seg, (const uint32_t *)p, n);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_settrackcfg(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, DWORD on, DWORD off)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    dm_engine_lock();
    HRESULT hr = dm_segment_set_trackconfig(sgm(i)->seg, (const dm_guid *)g, gb, n, on, off);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_getapconfig(IDirectMusicSegment8 *i, IUnknown **c) { if (c) *c = NULL; return DMUS_E_NO_AUDIOPATH_CONFIG; }
static rm_segment *new_segment(dm_segment *g);
int dmc_segment_compose(dm_segment *s);
/* 773150d0: the composing tracks composed (into a clone of the segment when one is asked for) */
/* Segment.Recompose (dmcompos 77287974 with flags 0, on the application's thread and rand()) */
HRESULT rm_segment_recompose(IUnknown *u)
{
    rm_segment *s = unsafe_segment(u);
    if (!s || !s->seg) return E_INVALIDARG;
    dm_engine_lock();
    int r = dmc_recompose(s->seg, dm_app_rand_state(), 1);
    dm_engine_unlock();
    return r;
}
static HRESULT WINAPI seg_compose(IDirectMusicSegment8 *i, MUSIC_TIME t, IDirectMusicSegment *a, IDirectMusicSegment *b, IDirectMusicSegment **c)
{
    if ((a && (IsBadReadPtr(a, 4) || IsBadReadPtr(*(void **)a, 4))) || (b && (IsBadReadPtr(b, 4) || IsBadReadPtr(*(void **)b, 4))) ||
        (c && IsBadWritePtr(c, 4)))
        return E_POINTER;
    rm_segment *s = sgm(i), *fa = a ? unsafe_segment((IUnknown *)a) : NULL, *fb = b ? unsafe_segment((IUnknown *)b) : NULL;
    if ((a && !fa) || (b && !fb)) return rm_notimpl("Segment::Compose from or to a segment not made by reDM");
    if (!s->seg) return DMUS_E_NOT_INIT;
    dm_engine_lock();
    int hr = DM_OK;
    dm_segment *g = s->seg;
    if (c) { g = dm_segment_clone(s->seg, 0, s->seg->length, &hr); if (!g) hr = E_OUTOFMEMORY; }
    if (g) hr = dmc_segment_compose_ex(g, t, fa ? fa->seg : NULL, fb ? fb->seg : NULL);
    dm_engine_unlock();
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
static HRESULT WINAPI sgo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_get(&sgo(i)->desc, &CLSID_DirectMusicSegment, d); }
static HRESULT WINAPI sgo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_set(&sgo(i)->desc, d, TRUE); }
/* 773136a4: a segment form's descriptor ('DMSG' or 'AASE' chunks), a wave's (what the wave class says of it), a MIDI
 * file's (the class only); the flags start from none */
static void wave_desc_of(const BYTE *b, DWORD n, DMUS_OBJECTDESC *d);
static HRESULT WINAPI sgo_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d)
{
    if (!s || !d) return E_POINTER;
    BYTE *b; DWORD n;
    if (!read_stream(s, &b, &n)) return DMUS_E_CHUNKNOTFOUND;
    HRESULT hr = S_OK;
    d->dwValidData = 0;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && (!memcmp(b + 8, "DMSG", 4) || !memcmp(b + 8, "AASE", 4)))
        rm_desc_from_form(b + 12, n - 12, d);
    else if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) wave_desc_of(b, n, d);
    else if (!dm_is_midi(b, n)) hr = DMUS_E_CHUNKNOTFOUND;
    free(b);
    if (FAILED(hr)) return hr;
    d->dwValidData |= DMUS_OBJ_CLASS;
    d->guidClass = CLSID_DirectMusicSegment;
    return S_OK;
}
static IDirectMusicObjectVtbl sgo_vtbl = { sgo_qi, sgo_addref, sgo_release, sgo_getdesc, sgo_setdesc, sgo_parse };

static dm_collection *loader_getcoll(void *ctx, const dm_guid *g, const char *file);
static dm_wave *loader_getwave(void *ctx, const dm_guid *g, const char *file, const char *name);
static struct dms_style *loader_getstyle(void *ctx, const dm_guid *g, const char *file, const char *name);
static struct dmc_chordmap *loader_getcmap(void *ctx, const dm_guid *g, const char *file, const char *name);
static dm_segment *loader_getseg(void *ctx, const dm_guid *g, const char *file, const char *name);
static void *loader_getscript(void *ctx, const dm_guid *g, const char *file, const char *name, int32_t *hr);
static dm_segment *segment_from_data(const BYTE *b, DWORD n, IDirectMusicLoader8 *l)
{
    if (dm_is_midi(b, n)) return dm_segment_from_midi(b, n, loader_getcoll, l);
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {      /* a wave as a segment */
        dm_wave *w = dm_wave_load(b, n);
        if (!w) return NULL;
        dm_segment *g = dm_segment_from_wave(w);
        dm_wave_release(w);
        return g;
    }
    dm_loadcb cb = { loader_getcoll, loader_getwave, loader_getstyle, l, loader_getcmap, loader_getseg, loader_getscript };
    /* (without the performance's lock: the loader is called; the style tracks sort a shared style again under it,
     * through dms_resort_hook) */
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
    DWORD fsz = *(const DWORD *)(b + 4), end = fsz > n - 8 ? n : 8 + fsz;
    for (DWORD off = 12; off + 12 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4);
        if (sz > end - off - 8) break;          /* (a chunk past the form's end: the last; no wrapping round) */
        if (!memcmp(b + off, "RIFF", 4) && !memcmp(b + off + 8, "DMTG", 4)) {
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
/* 77313c61: a segment form's embedded containers ('DMCN' forms), loaded through the loader from the segment's own
 * stream so that the objects in them can be found while the tracks load; let go of when the segment is loaded */
static int segment_containers(IStream *st, ULONGLONG at, const BYTE *b, DWORD n, IDirectMusicLoader8 *l,
                              IDirectMusicObject ***out)
{
    *out = NULL;
    if (!l || n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DMSG", 4)) return 0;
    DWORD fsz = *(const DWORD *)(b + 4), end = fsz > n - 8 ? n : 8 + fsz;
    int k = 0;
    for (DWORD off = 12; off + 12 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4);
        if (!memcmp(b + off, "RIFF", 4) && !memcmp(b + off + 8, "DMCN", 4)) {
            LARGE_INTEGER p; p.QuadPart = (LONGLONG)(at + off);
            DMUS_OBJECTDESC d;
            memset(&d, 0, sizeof(d));
            d.dwSize = sizeof(d);
            d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_STREAM;
            d.guidClass = CLSID_DirectMusicContainer;
            d.pStream = st;
            IDirectMusicObject *o = NULL;
            if (SUCCEEDED(IStream_Seek(st, p, STREAM_SEEK_SET, NULL)) &&
                SUCCEEDED(IDirectMusicLoader8_GetObject(l, &d, &IID_IDirectMusicObject, (void **)&o)) && o) {
                IDirectMusicObject **a = realloc(*out, (k + 1) * sizeof(*a));
                if (a) { *out = a; a[k++] = o; } else IDirectMusicObject_Release(o);
            }
        }
        if (sz > end - off - 8) break;          /* (past the form's end: the last; no wrapping round) */
        off += 8 + sz + (sz & 1);
    }
    return k;
}
static HRESULT WINAPI sgp_load(IPersistStream *i, IStream *st)
{
    rm_segment *s = sgp(i);
    BYTE *b; DWORD n;
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER at;
    if (!st) return E_POINTER;
    if (FAILED(IStream_Seek(st, z, STREAM_SEEK_CUR, &at)) || !read_stream(st, &b, &n)) return E_FAIL;
    IDirectMusicLoader8 *l = rm_stream_loader(st);
    IDirectMusicObject **cont;
    int ncont = segment_containers(st, at.QuadPart, b, n, l, &cont);
    dm_segment *g = segment_from_data(b, n, l);
    HRESULT ghr;
    IDirectMusicGraph *gr = g ? segment_graph_from_data(b, n, &ghr) : NULL;
    if (g && n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "DMSG", 4)) rm_desc_from_form(b + 12, n - 12, &s->desc);
    else if (g && n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) wave_desc_of(b, n, &s->desc);
    free(b);
    for (int k = 0; k < ncont; k++) {
        IDirectMusicLoader8_ReleaseObject(l, cont[k]);
        IDirectMusicObject_Release(cont[k]);
    }
    free(cont);
    if (!g) return DMUS_E_UNSUPPORTED_STREAM;
    if (gr && FAILED(ghr)) { IDirectMusicGraph_Release(gr); dm_segment_release(g); return ghr; }
    dm_engine_lock();                       /* (the old segment may be playing: its count and wrapper under the lock) */
    if (s->seg) { if (s->seg->wrapper == s) s->seg->wrapper = NULL; dm_segment_release(s->seg); }
    s->seg = g;
    g->wrapper = s;
    IDirectMusicGraph *og = NULL;
    if (gr) { og = s->graph; s->graph = gr; }
    dm_engine_unlock();
    if (og) IDirectMusicGraph_Release(og);
    s->desc.dwValidData |= DMUS_OBJ_LOADED;
    return g->partial ? DMUS_S_PARTIALLOAD : S_OK;
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
    s->desc.dwValidData = g ? DMUS_OBJ_LOADED : 0;
    s->desc.guidClass = CLSID_DirectMusicSegment;
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
/* what dswave says of a wave file: its guid, name and version */
static void wave_desc_of(const BYTE *b, DWORD n, DMUS_OBJECTDESC *d)
{
    dm_wave *w = dm_wave_load(b, n);
    if (!w) return;
    if (w->hasguid) { memcpy(&d->guidObject, &w->guid, sizeof(GUID)); d->dwValidData |= DMUS_OBJ_OBJECT; }
    if (w->name[0]) { MultiByteToWideChar(CP_ACP, 0, w->name, -1, d->wszName, DMUS_MAX_NAME); d->dwValidData |= DMUS_OBJ_NAME; }
    if (w->hasvers) { d->vVersion.dwVersionMS = w->vers[0]; d->vVersion.dwVersionLS = w->vers[1]; d->dwValidData |= DMUS_OBJ_VERSION; }
    dm_wave_release(w);
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
    dm_engine_lock();                   /* (loader_getwave reads o->w under it) */
    dm_wave *old = o->w;
    if (old && old->wrapper == o) old->wrapper = NULL;
    o->w = w;
    w->wrapper = o;
    wave_desc(o);
    dm_engine_unlock();
    dm_wave_release(old);
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
    IDirectMusicPatternTrack IDirectMusicPatternTrack_iface;   /* the pattern track's */
    LONG ref;
    dm_track *t;
};
static IDirectMusicTrack8Vtbl trk_vtbl;
static IPersistStreamVtbl trp_vtbl;
static IDirectMusicPatternTrackVtbl ptk_vtbl;
static rm_track *trk(IDirectMusicTrack8 *i) { return impl_from(i, rm_track, IDirectMusicTrack8_iface); }

/* the COM object of a track (one per track, made on demand) */
static IDirectMusicTrack8 *track_object(dm_track *t)
{
    rm_track *r = t->wrapper;
    if (r && ref_if_alive(&r->ref)) return &r->IDirectMusicTrack8_iface;
    if (!(r = calloc(1, sizeof(*r)))) return NULL;
    r->IDirectMusicTrack8_iface.lpVtbl = &trk_vtbl;
    r->IPersistStream_iface.lpVtbl = &trp_vtbl;
    r->IDirectMusicPatternTrack_iface.lpVtbl = &ptk_vtbl;
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
    else if (IsEqualIID(riid, &IID_IDirectMusicPatternTrack) && kind == TRK_PATTERN)     /* 77413466 */
        *out = &trk(i)->IDirectMusicPatternTrack_iface;
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
        dm_engine_lock();
        if (r->t->wrapper == r) r->t->wrapper = NULL;
        dm_track_release(r->t);
        dm_engine_unlock();
        free(r);
    }
    return n;
}
static HRESULT WINAPI trk_init(IDirectMusicTrack8 *i, IDirectMusicSegment *s) { return S_OK; }
/* a track played by the application (InitPlay, Play, EndPlay on a state and performance of its choosing): the engine
 * keeps the track's state as a segment state would; the chord map, signpost, mute and marker tracks have none
 * (their InitPlay gives NULL), a style or pattern track without a style fails with E_POINTER, a motif track without
 * a motif with E_FAIL */
static rm_state *unsafe_state(IUnknown *u);
static rm_perf *perf_from_unknown(IUnknown *u);
static BYTE s_nostate;                      /* (what a failed motif InitPlay leaves: never played or ended) */
static HRESULT WINAPI trk_initplay(IDirectMusicTrack8 *i, IDirectMusicSegmentState *ss, IDirectMusicPerformance *p, void **d, DWORD id, DWORD f)
{
    dm_track *t = trk(i)->t;
    if (!d || IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    *d = NULL;
    dm_engine_lock();
    int ok = dm_track_can_play(t);
    dm_engine_unlock();
    if (ok == DM_E_FAIL) *d = &s_nostate;
    if (ok < 0) return ok;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    rm_perf *pf = p ? perf_from_unknown((IUnknown *)p) : NULL;
    if (!st || !st->ss || !pf || !pf->host) return rm_notimpl("Track::InitPlay (a state or performance not made by reDM)");
    dm_host_lock(pf->host);
    if ((ok = dm_track_can_play(t)) < 0) {          /* (again: a Load on another thread may have replaced the data) */
        dm_host_unlock(pf->host);
        if (ok == DM_E_FAIL) *d = &s_nostate;
        return ok;
    }
    int ihr;
    void *x = dm_track_direct_init(dm_host_perf(pf->host), st->ss, t, f, id, &ihr);
    if (x && (t->kind == TRK_CHORDMAP || t->kind == TRK_SIGNPOST || t->kind == TRK_MUTE || t->kind == TRK_MARKER)) {
        dm_track_direct_end(x);
        x = NULL;
    } else if (!x) { dm_host_unlock(pf->host); return E_OUTOFMEMORY; }
    dm_host_unlock(pf->host);
    *d = x;
    /* (dmime's wave track hands out its state even so; nothing ever ends it, and it keeps the track) */
    if (ihr < 0) trk_addref(i);
    return ihr;
}
static HRESULT WINAPI trk_endplay(IDirectMusicTrack8 *i, void *d)
{
    if (!d || d == &s_nostate) return S_OK;
    dm_engine_lock();
    dm_track_direct_end(d);             /* (on the performance it started on) */
    dm_engine_unlock();
    return S_OK;
}
static HRESULT trk_play_any(IDirectMusicTrack8 *i, void *d, int64_t a, int64_t b, int64_t o, DWORD f, IDirectMusicPerformance *p,
                            IDirectMusicSegmentState *ss, int clock)
{
    if (d == &s_nostate) return E_POINTER;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    rm_perf *pf = p ? perf_from_unknown((IUnknown *)p) : NULL;
    if (!st || !st->ss || !pf || !pf->host) return rm_notimpl("Track::Play (a state or performance not made by reDM)");
    dm_host_lock(pf->host);
    int hr = dm_track_direct_play(dm_host_perf(pf->host), st->ss, d, trk(i)->t, a, b, o, f, clock);
    dm_host_unlock(pf->host);
    return hr;
}
static HRESULT WINAPI trk_play(IDirectMusicTrack8 *i, void *d, MUSIC_TIME a, MUSIC_TIME b, MUSIC_TIME o, DWORD f, IDirectMusicPerformance *p, IDirectMusicSegmentState *ss, DWORD id)
{
    return trk_play_any(i, d, a, b, o, f, p, ss, 0);
}
static HRESULT WINAPI trk_getparam(IDirectMusicTrack8 *i, REFGUID g, MUSIC_TIME t, MUSIC_TIME *nx, void *p)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int prm = prm_of(g);
    mtime_t next = nx ? *nx : 0;
    dm_engine_lock();
    HRESULT hr = dm_track_getparam(trk(i)->t, prm, t, nx ? &next : NULL, p);
    prm_out(prm, hr, p);
    dm_engine_unlock();
    if (nx) *nx = next;
    return hr;
}
static HRESULT WINAPI trk_setparam(IDirectMusicTrack8 *i, REFGUID g, MUSIC_TIME t, void *p)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int prm = prm_of(g);
    dm_dltarget dt; void *data;
    HRESULT hr = prm_data(prm, p, &dt, &data);
    if (FAILED(hr)) return hr;
    dm_engine_lock();
    hr = dm_track_setparam(trk(i)->t, prm, t, data);
    dm_engine_unlock();
    prm_drop(prm, &dt, data);
    return hr;
}
static HRESULT WINAPI trk_isparam(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_engine_lock();                   /* (a Load on another thread may replace the track's data) */
    HRESULT hr = dm_track_isparam(trk(i)->t, prm_of(g));
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI trk_addnotif(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_engine_lock();
    HRESULT hr = dm_track_add_notification(trk(i)->t, (const dm_guid *)g);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI trk_remnotif(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_engine_lock();
    HRESULT hr = dm_track_remove_notification(trk(i)->t, (const dm_guid *)g);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI trk_clone(IDirectMusicTrack8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicTrack **o)
{
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    int hr;
    dm_engine_lock();
    dm_track *c = dm_track_clone(trk(i)->t, a, b, &hr);
    dm_engine_unlock();
    if (!c) return hr;
    *o = (IDirectMusicTrack *)track_object(c);
    dm_track_release(c);
    return *o ? S_OK : E_OUTOFMEMORY;
}
/* PlayEx: clock time (DMUS_TRACKF_CLOCK) for the wave track; music time otherwise */
static HRESULT WINAPI trk_playex(IDirectMusicTrack8 *i, void *d, REFERENCE_TIME a, REFERENCE_TIME b, REFERENCE_TIME o, DWORD f, IDirectMusicPerformance *p, IDirectMusicSegmentState *ss, DWORD id)
{
    int clock = (f & DMUS_TRACKF_CLOCK) != 0;
    if (clock && trk(i)->t->kind != TRK_WAVE) { a /= 10000; b /= 10000; o /= 10000; }
    return trk_play_any(i, d, a, b, o, f & ~DMUS_TRACKF_CLOCK, p, ss, clock && trk(i)->t->kind == TRK_WAVE);
}
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
    dm_engine_lock();
    dm_track *res = NULL;
    int hr = dm_track_join(me, r->t, j, g ? g->seg : NULL, id, o ? &res : NULL);
    dm_engine_unlock();
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
/* the track's chunk (its header and size, then that much) */
static BOOL read_chunk(IStream *s, BYTE **data, DWORD *size)
{
    BYTE h[8];
    ULONG got = 0;
    *data = NULL; *size = 0;
    if (FAILED(IStream_Read(s, h, 8, &got)) || got != 8) return FALSE;
    DWORD n = *(DWORD *)(h + 4);
    if (n > 0x7ffffff0u) return FALSE;      /* (a broken size: 8 + n + 1 would wrap) */
    BYTE *b = malloc(8 + (size_t)n + 1);
    if (!b) return FALSE;
    memcpy(b, h, 8);
    if (n && (FAILED(IStream_Read(s, b + 8, n, &got)) || got != n)) { free(b); return FALSE; }
    *data = b; *size = 8 + n;
    return TRUE;
}
/* the end of a wave track chunk's first item, and the next one's header: where dmime's Load stops when it has no loader
 * to give the item's reference to */
static DWORD wavetrack_noloader_stop(const BYTE *b, DWORD n)
{
    for (DWORD off = 12; off + 12 <= n; ) {
        DWORD sz = *(const DWORD *)(b + off + 4);
        if (sz > n - off - 8) sz = n - off - 8;     /* (sizes past the data cut to it: no reading beyond, no wrapping) */
        if (!memcmp(b + off, "LIST", 4) && !memcmp(b + off + 8, "wavp", 4)) {
            for (DWORD q = off + 12; q + 12 <= off + 8 + sz; ) {
                DWORD qs = *(const DWORD *)(b + q + 4);
                if (!memcmp(b + q, "LIST", 4) && !memcmp(b + q + 8, "wavi", 4)) {
                    DWORD r = q + 12;
                    if (r + 8 <= n) { DWORD rs = *(const DWORD *)(b + r + 4); ULONGLONG e = (ULONGLONG)r + 16 + rs + (rs & 1); r = e < n ? (DWORD)e : n; }
                    return r < n ? r : n;
                }
                if (qs > off + 8 + sz - q - 8) break;
                q += 8 + qs + (qs & 1);
            }
        }
        off += 8 + sz + (sz & 1);
    }
    return n;
}
/* Load of a track made on its own: its chunk; references through the stream's loader (the segment trigger track
 * asks for it before it reads, the style, chord map and band tracks fail after reading without one, the wave track
 * at its first item's reference) */
static HRESULT WINAPI trp_load(IPersistStream *i, IStream *s)
{
    dm_track *t = trp(i)->t;
    if (!s || IsBadReadPtr(s, 4)) return E_POINTER;
    IDirectMusicLoader8 *l = rm_stream_loader(s);
    if ((t->kind == TRK_SEGTRIG || t->kind == TRK_SCRIPT) && !l) return E_NOINTERFACE;
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER at;
    if (FAILED(IStream_Seek(s, z, STREAM_SEEK_CUR, &at))) at.QuadPart = 0;
    BYTE *b; DWORD n;
    if (!read_chunk(s, &b, &n)) return DMUS_E_CHUNKNOTFOUND;
    HRESULT hr = S_OK;
    if (!l && (t->kind == TRK_STYLE || t->kind == TRK_CHORDMAP)) hr = E_NOINTERFACE;
    else if (!l && t->kind == TRK_BAND) hr = (HRESULT)0x88781150;
    else if (!l && t->kind == TRK_WAVE) {
        LARGE_INTEGER p; p.QuadPart = (LONGLONG)(at.QuadPart + wavetrack_noloader_stop(b, n));
        IStream_Seek(s, p, STREAM_SEEK_SET, NULL);
        hr = E_NOINTERFACE;
    }
    if (SUCCEEDED(hr)) {
        dm_loadcb cb = { loader_getcoll, loader_getwave, loader_getstyle, l, loader_getcmap, loader_getseg, loader_getscript };
        /* parsed without the performance's lock (the loader is called), put in place with it (states playing the
         * track move to the new data) */
        void *d = NULL;
        hr = dm_track_parse_chunk(t, b, n, &cb, &d);
        if (d) {
            dm_engine_lock();
            dm_track_set_data(t, d);
            dm_engine_unlock();
        }
    }
    free(b);
    return hr;
}
/* the signpost track's (7728af59: its 'sgnp' chunk); the others are not written */
static HRESULT WINAPI trp_save(IPersistStream *i, IStream *s, BOOL c)
{
    dm_track *t = trp(i)->t;
    if (t->kind != TRK_SIGNPOST) return rm_notimpl(__func__);
    size_t n;
    dm_engine_lock();
    uint8_t *b = dmc_sptrack_save(t->data, &n);
    dm_engine_unlock();
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
    if (!g) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    static const GUID null_guid;
    for (int k = 0; s && k < s->ntracks; k++) {
        dm_segtrack *e = &s->tracks[k];
        if (!(e->group & gb) || !(IsEqualGUID(g, &null_guid) || !memcmp(g, &e->t->clsid, sizeof(GUID)))) continue;
        if (n--) continue;
        *t = (IDirectMusicTrack *)track_object(e->t);
        hr = *t ? S_OK : E_OUTOFMEMORY;
        break;
    }
    dm_engine_unlock();
    return hr;
}
/* 773119c5 GetTrackGroup */
static HRESULT WINAPI seg_gettrackgroup(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD *g)
{
    if (!t || !g || IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    if (!r) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    for (int k = 0; s && k < s->ntracks; k++) if (s->tracks[k].t == r->t) { *g = s->tracks[k].group; hr = S_OK; break; }
    dm_engine_unlock();
    return hr;
}
/* 773131cb / 77311b2c InsertTrack: config DMUS_TRACKCONFIG_DEFAULT, position and priority 0, in track order; the track
 * takes the segment's notification types.  Only reDM's own tracks can go in. */
static HRESULT WINAPI seg_inserttrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD g)
{
    if (!t || IsBadReadPtr(t, sizeof(void *))) return E_POINTER;
    if (!g) return E_INVALIDARG;
    rm_track *r = unsafe_track((IUnknown *)t);
    if (!r) return rm_notimpl("Segment::InsertTrack (a track not made by reDM)");
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    if (!s) { dm_engine_unlock(); return DMUS_E_NOT_INIT; }
    dm_track_addref(r->t);
    int ok = dm_segment_insert_track(s, r->t, 0, g, 0x38, 0);
    dm_engine_unlock();
    return ok ? S_OK : E_OUTOFMEMORY;
}
/* 77311c54 RemoveTrack: S_FALSE when the track is not in the segment */
static HRESULT WINAPI seg_removetrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t)
{
    if (!t || IsBadReadPtr(t, sizeof(void *))) return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    if (!r) return S_FALSE;
    HRESULT hr = S_FALSE;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    for (int k = 0; s && k < s->ntracks; k++)
        if (s->tracks[k].t == r->t) {
            dm_track *x = s->tracks[k].t;
            memmove(s->tracks + k, s->tracks + k + 1, (s->ntracks - k - 1) * sizeof(*s->tracks));
            s->ntracks--;
            dm_track_release(x);
            hr = S_OK;
            break;
        }
    dm_engine_unlock();
    return hr;
}
/* 77313a27 Clone */
static HRESULT WINAPI seg_clone(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicSegment **o)
{
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    int hr = E_FAIL;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    dm_segment *c = s ? dm_segment_clone(s, a, b, &hr) : NULL;
    IDirectMusicGraph *gr = c ? sgm(i)->graph : NULL;
    if (gr) IDirectMusicGraph_AddRef(gr);       /* (the application's graph object: AddRef only, no call into it) */
    dm_engine_unlock();
    if (!c) { *o = NULL; return hr; }
    rm_segment *n = new_segment(c);
    if (!n) { if (gr) IDirectMusicGraph_Release(gr); dm_engine_lock(); dm_segment_release(c); dm_engine_unlock(); *o = NULL; return E_OUTOFMEMORY; }
    n->graph = gr;                              /* 77313acd: the same graph */
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
    if (IsEqualIID(riid, &IID_IDispatch)) {
        rm_state *s = sst(i);
        if (!s->autod && !(s->autod = rm_auto_new((IUnknown *)&s->IDirectMusicSegmentState8_iface, RM_AUTO_SEGMENTSTATE))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = s->autod; InterlockedIncrement(&s->ref); return S_OK;
    }
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
        rm_auto_free(s->autod);
        free(s);
    }
    return r;
}
static HRESULT WINAPI st_getrepeats(IDirectMusicSegmentState8 *i, DWORD *r)
{
    if (!r) return E_POINTER;
    *r = 0;
    if (sst(i)->seg) SEG_GET(&sst(i)->seg->IDirectMusicSegment8_iface, repeats, *r);
    return S_OK;
}
static HRESULT WINAPI st_getsegment(IDirectMusicSegmentState8 *i, IDirectMusicSegment **s)
{
    if (!s) return E_POINTER;
    *s = NULL;
    rm_state *st = sst(i);
    if (st->perf->host) dm_host_lock(st->perf->host);
    int gone = !dm_segstate_segment(st->ss);        /* (released when the state left the performance) */
    if (st->perf->host) dm_host_unlock(st->perf->host);
    if (!st->seg || gone) return DMUS_E_NOT_FOUND;
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
/* 77315e8c: where in the segment the state started (the segment's start point, moved on by DMUS_SEGF_ALIGN) */
static HRESULT WINAPI st_getstartpoint(IDirectMusicSegmentState8 *i, MUSIC_TIME *t)
{
    rm_state *s = sst(i);
    if (!t || IsBadWritePtr(t, sizeof(*t))) return E_POINTER;
    if (s->perf->host) dm_host_lock(s->perf->host);
    *t = dm_segstate_start_point(s->ss);
    if (s->perf->host) dm_host_unlock(s->perf->host);
    return S_OK;
}
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
static HRESULT st_getobj_do(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o);
static HRESULT WINAPI st_getobj(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    HRESULT hr = st_getobj_do(i, pch, stage, buf, g, n, iid, o);
    char b1[64], b2[64];
    LOG_CALL("music: SegmentState::GetObjectInPath(pch %lx, stage %lx, buffer %lu, %s, %lu, %s) -> %08lx", (unsigned long)pch,
             (unsigned long)stage, (unsigned long)buf, g && !IsBadReadPtr(g, sizeof(GUID)) ? guidstr(g, b1) : "(null)",
             (unsigned long)n, iid && !IsBadReadPtr(iid, sizeof(GUID)) ? guidstr(iid, b2) : "(null)", (unsigned long)hr);
    return hr;
}
static HRESULT st_getobj_do(IDirectMusicSegmentState8 *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
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
    if (IsEqualIID(riid, &IID_IDispatch)) {
        rm_path *a = apt(i);
        if (!a->autod && !(a->autod = rm_auto_new((IUnknown *)&a->IDirectMusicAudioPath_iface, RM_AUTO_AUDIOPATH))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = a->autod; InterlockedIncrement(&a->ref); return S_OK;
    }
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
        rm_auto_free(a->autod);
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
static HRESULT ap_getobj_do(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o);
static HRESULT WINAPI ap_getobj(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
{
    HRESULT hr = ap_getobj_do(i, pch, stage, buf, g, n, iid, o);
    char b1[64], b2[64];
    LOG_CALL("music: AudioPath::GetObjectInPath(pch %lx, stage %lx, buffer %lu, %s, %lu, %s) -> %08lx", (unsigned long)pch,
             (unsigned long)stage, (unsigned long)buf, g && !IsBadReadPtr(g, sizeof(GUID)) ? guidstr(g, b1) : "(null)",
             (unsigned long)n, iid && !IsBadReadPtr(iid, sizeof(GUID)) ? guidstr(iid, b2) : "(null)", (unsigned long)hr);
    return hr;
}
static HRESULT ap_getobj_do(IDirectMusicAudioPath *i, DWORD pch, DWORD stage, DWORD buf, REFGUID g, DWORD n, REFGUID iid, void **o)
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

static void perf_set_ver(rm_perf *p, LONG v)
{
    InterlockedExchange(&p->ver, v);
    if (p->host) dm_perf_set_dxver(dm_host_perf(p->host), (uint32_t)v);
}
/* 77303ed2: IDirectMusicPerformance8 makes it a DirectX 8 application's, IDirectMusicPerformance2 a DirectX 7 one's */
static HRESULT WINAPI pf_qi(IDirectMusicPerformance8 *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicPerformance) ||
        IsEqualIID(riid, &IID_IDirectMusicPerformance2) || IsEqualIID(riid, &IID_IDirectMusicPerformance8)) {
        if (IsEqualIID(riid, &IID_IDirectMusicPerformance8)) perf_set_ver(prf(i), 8);
        else if (IsEqualIID(riid, &IID_IDirectMusicPerformance2)) perf_set_ver(prf(i), 7);
        *out = i; InterlockedIncrement(&prf(i)->ref); return S_OK;
    }
    /* 77303eb4: the output tool and the graph (+4, +8).  reAL does not count these two (pft_addref / pfg_addref), so
     * handing one out takes no reference either: their Release would never give it back. */
    if (IsEqualIID(riid, &IID_IDirectMusicTool)) { *out = &prf(i)->IDirectMusicTool_iface; return S_OK; }
    if (IsEqualIID(riid, &IID_IDirectMusicGraph)) { *out = &prf(i)->IDirectMusicGraph_iface; return S_OK; }
    if (IsEqualIID(riid, &IID_IDispatch)) {
        rm_perf *p = prf(i);
        if (!p->autod && !(p->autod = rm_auto_new((IUnknown *)&p->IDirectMusicPerformance8_iface, RM_AUTO_PERFORMANCE))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = p->autod; InterlockedIncrement(&p->ref); return S_OK;
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
        dm_perf *pf = dm_host_perf(p->host);
        dm_perf_stop(pf, NULL, NULL, 0, 0);
        dm_perf_drop_app_messages(pf);
        dm_perf_end_states(pf);
        dm_perf_drop_all_messages(pf);
        dm_perf_set_graph(pf, NULL);
        dm_host_unlock(p->host);
    }
}
static void pf_teardown(rm_perf *p)
{
    dm_engine_lock();
    for (rm_perf **q = &s_perfs; *q; q = &(*q)->pnext) if (*q == p) { *q = p->pnext; break; }
    dm_engine_unlock();
    rm_port_detach(p->port);
    rm_port_release(p->port);
    dm_host_free(p->host);
    for (int k = 1; k < 8; k++) rm_port_release(p->slot[k]);  /* (CreatePort'ed: their synths may go now) */
    if (p->dm) IDirectMusic8_Release(p->dm);
    if (p->pregraph) IDirectMusicGraph_Release(p->pregraph);
    rm_auto_free(p->autod);
    while (p->globals) { struct rm_global *e = p->globals; p->globals = e->next; free(e->data); free(e); }
    DeleteCriticalSection(&p->gcs);
    free(p);
}
static DWORD WINAPI pf_reaper(LPVOID p) { pf_teardown(p); return 0; }
static ULONG WINAPI pf_release(IDirectMusicPerformance8 *i)
{
    rm_perf *p = prf(i);
    LONG r = InterlockedDecrement(&p->ref);
    LOG_CALL("music: Performance::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        p->ref = 1;     /* guard against re-entry from children */
        pf_closedown(p);
        /* reAL: the last reference let go of on one of the performance's own threads (a segment state's release the
         * engine deferred until its lock was let go): that thread cannot wait for itself to end, and it is still
         * inside the engine (an application tool's turn), so a thread of its own does the rest */
        if (p->host && dm_host_own_thread(p->host)) {
            HANDLE t = CreateThread(NULL, 0, pf_reaper, p, 0, NULL);
            if (t) CloseHandle(t);
            else LOG_INFO("music: performance %p released on its own thread, no thread to free it; not freed", (void *)p);
            return 0;
        }
        pf_teardown(p);
    }
    return r;
}

static const dm_toolops s_toolops;
static HRESULT WINAPI pf_setglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz);
/* 77303d4d, on Init (the engine lock held): the version; a DirectX 8 application's prepare time and bumper length back
 * to 1000 and 50 ms, an older one's as it set them (before Init, or before CloseDown); the graph set before Init */
static void perf_init_settings(rm_perf *p, BOOL again)
{
    dm_perf *pf = dm_host_perf(p->host);
    dm_perf_set_dxver(pf, (uint32_t)p->ver);
    if (p->ver >= 8) { dm_perf_set_prepare_time(pf, 1000); dm_perf_set_bumper_length(pf, 50); }
    else if (!again) { dm_perf_set_prepare_time(pf, p->prepare_ms); dm_perf_set_bumper_length(pf, p->bumper_ms); }
    if (p->pregraph) { dm_perf_set_graph(pf, p->pregraph); p->pregraph = NULL; }
}
static HRESULT ensure_host(rm_perf *p, uint32_t rate, uint32_t voices)
{
    if (p->host) return S_OK;
    p->host = dm_host_new(rate ? rate : 22050, voices ? voices : 64, 64);
    if (!p->host) return E_OUTOFMEMORY;
    p->port = rm_port_new(p->host);
    p->slot[0] = p->port;
    p->added[0] = 1;                        /* (InitAudio's; Init's is not, until AddPort) */
    p->revparams.fInGain = 0; p->revparams.fReverbMix = 0; p->revparams.fReverbTime = 1000; p->revparams.fHighFreqRTRatio = 0.001f;
    dm_host_lock(p->host);
    rm_perf **q = &s_perfs;
    while (*q) q = &(*q)->pnext;
    *q = p;
    dm_perf_set_toolops(dm_host_perf(p->host), &s_toolops, p);
    perf_init_settings(p, FALSE);
    for (struct rm_global *e = p->globals; e; e = e->next)        /* (set before the performance was) */
        if (IsEqualGUID(&e->g, &G_PerfMasterGrooveLevel) && e->size >= 1) dm_perf_set_groove(dm_host_perf(p->host), *(char *)e->data);
    dm_host_unlock(p->host);
    BOOL off = FALSE;                       /* 77303d4d: automatic download off */
    pf_setglobal(&p->IDirectMusicPerformance8_iface, &GUID_PerfAutoDownload, &off, 4);
    return S_OK;
}

/* Init or InitAudio again after CloseDown, as dmime allows (its CloseDown leaves it uninitialised): reAL keeps the
 * engine (its synth and sink) and starts it over as Init would.  S_FALSE when there is none yet. */
static HRESULT perf_reopen(rm_perf *p)
{
    if (!p->host) return S_FALSE;
    dm_host_lock(p->host);
    p->closed = FALSE;
    perf_init_settings(p, TRUE);
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
    if (p->host && !p->closed) return DMUS_E_ALREADY_INITED;
    HRESULT hr = perf_reopen(p);
    if (hr == S_FALSE) hr = ensure_host(p, 22050, 64);
    if (FAILED(hr)) return hr;
    dm_host_lock(p->host);
    p->added[0] = 0;                        /* (the DirectX 6 way: AddPort, then AssignPChannelBlock) */
    dm_host_port_enable(p->host, 0, p->host, 0);   /* (no port: no latency, nothing played, until AddPort) */
    dm_host_unlock(p->host);
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
/* these work before Init too (dmime keeps them in the performance; a DirectX 8 application's Init sets them back) */
static HRESULT perf_timing(rm_perf *p, int bumper, const DWORD *set, DWORD *get)
{
    dm_engine_lock();                       /* (Init hands what was set before it to the engine under it) */
    if (!p->host) {
        DWORD *v = bumper ? &p->bumper_ms : &p->prepare_ms;
        if (set) *v = *set;
        else *get = *v;
    } else {
        dm_perf *pf = dm_host_perf(p->host);
        if (set) { if (bumper) dm_perf_set_bumper_length(pf, *set); else dm_perf_set_prepare_time(pf, *set); }
        else *get = bumper ? dm_perf_get_bumper_length(pf) : dm_perf_get_prepare_time(pf);
    }
    dm_engine_unlock();
    return S_OK;
}
static HRESULT WINAPI pf_setprepare(IDirectMusicPerformance8 *i, DWORD ms) { return perf_timing(prf(i), 0, &ms, NULL); }
static HRESULT WINAPI pf_getprepare(IDirectMusicPerformance8 *i, DWORD *ms) { if (!ms) return E_POINTER; return perf_timing(prf(i), 0, NULL, ms); }
static HRESULT WINAPI pf_setbumper(IDirectMusicPerformance8 *i, DWORD ms) { return perf_timing(prf(i), 1, &ms, NULL); }
static HRESULT WINAPI pf_getbumper(IDirectMusicPerformance8 *i, DWORD *ms) { if (!ms) return E_POINTER; return perf_timing(prf(i), 1, NULL, ms); }
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
    /* (on the engine's threads this runs under the performance's lock: the objects go once it is let go, as an
     * application tool or a script event's script may call the loader or wait for the scripts' thread) */
    dm_host_release_later(m->pTool);
    dm_host_release_later(m->pGraph);
    dm_host_release_later(m->punkUser);
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
    case DMUS_PMSGT_LYRIC: return sizeof(DMUS_LYRIC_PMSG);
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
    case DMUS_PMSGT_LYRIC: {           /* the text a lyrics track made (one the application made stays in its view) */
        DMUS_LYRIC_PMSG *x = (DMUS_LYRIC_PMSG *)m;
        if (e->sx_data && FIELD_OFFSET(DMUS_LYRIC_PMSG, wszString) + e->sx_len <= cap) memcpy(x->wszString, e->sx_data, e->sx_len);
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
        if (e->type == DMUS_PMSGT_LYRIC && FIELD_OFFSET(DMUS_LYRIC_PMSG, wszString) + e->sx_len > n) n = FIELD_OFFSET(DMUS_LYRIC_PMSG, wszString) + e->sx_len;
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
    if (a && ref_if_alive(&a->ref)) return &a->IDirectMusicGraph_iface;
    dm_audiopath_addref(obj);                   /* a path the application let go of (or is letting go of): a new object */
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
            dm_host_release_later(m->pGraph);
            m->pGraph = g;
        } else dm_host_release_later(g);
        e->flags = m->dwFlags;
    }
    return hr;
}
static void tops_unstamp(void *ctx, dm_pmsg *e)
{
    DMUS_PMSG *m = e->ext;
    dm_host_release_later(m->pGraph); m->pGraph = NULL;
    dm_host_release_later(m->pTool); m->pTool = NULL;
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
    dm_host_release_later(t);
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
static void tops_graph_release(void *ctx, void *g) { dm_host_release_later(g); }    /* (its tools are the application's) */
/* the parameter control track's objects: IMediaParams (by its vtable: GetParam, SetParam, AddEnvelope,
 * FlushEnvelope, SetTimeFormat) */
static const GUID RM_IID_IMediaParams = { 0x6d6cbb61, 0xa223, 0x44aa, { 0x84, 0x2f, 0xa2, 0xf0, 0x67, 0x50, 0xbe, 0x6e } };
typedef struct { LONGLONG start, end; float v0, v1; DWORD curve, flags; } rm_envseg;
typedef struct rm_mpvtbl {
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *GetParam)(void *, DWORD, float *);
    HRESULT (WINAPI *SetParam)(void *, DWORD, float);
    HRESULT (WINAPI *AddEnvelope)(void *, DWORD, DWORD, rm_envseg *);
    HRESULT (WINAPI *FlushEnvelope)(void *, DWORD, LONGLONG, LONGLONG);
    HRESULT (WINAPI *SetTimeFormat)(void *, GUID, DWORD);
} rm_mpvtbl;
#define MPV(mp) (*(const rm_mpvtbl **)(mp))
/* 77322b08: the state's GetObjectInPath for IMediaParams, then SetTimeFormat */
static void *tops_mp_get(void *ctx, dm_segstate *ss, uint32_t pch, uint32_t stage, uint32_t buffer, const dm_guid *obj,
                         uint32_t index, const dm_guid *fmt, uint32_t fmtdata)
{
    rm_perf *p = ctx;
    IUnknown *u = state_object(p, ss);
    if (!u) return NULL;
    void *mp = NULL;
    HRESULT hr = IDirectMusicSegmentState8_GetObjectInPath((IDirectMusicSegmentState8 *)u, pch, stage, buffer,
                                                           (REFGUID)obj, index, &RM_IID_IMediaParams, &mp);
    dm_host_release_later(u);
    if (FAILED(hr) || !mp) return NULL;
    GUID g;
    memcpy(&g, fmt, sizeof(g));
    if (FAILED(MPV(mp)->SetTimeFormat(mp, g, fmtdata))) { dm_host_release_later(mp); return NULL; }
    return mp;
}
static int tops_mp_add(void *ctx, void *mp, uint32_t param, int64_t start, int64_t end, float v0, float v1,
                       uint32_t curve, uint32_t flags)
{
    rm_envseg e = { start, end, v0, v1, curve, flags };
    return MPV(mp)->AddEnvelope(mp, param, 1, &e);
}
static int tops_mp_flush(void *ctx, void *mp, uint32_t param, int64_t start, int64_t end)
{
    return MPV(mp)->FlushEnvelope(mp, param, start, end);
}
/* 77316457: a state that leaves the performance lets its audiopath go (its object stays) */
static void tops_state_end(void *ctx, dm_segstate *ss)
{
    rm_state *s = *dm_segstate_wrapper(ss);
    if (s && s->path) { IDirectMusicAudioPath *a = s->path; s->path = NULL; dm_host_release_later(a); }
}
static void tops_mp_release(void *ctx, void *mp) { dm_host_release_later(mp); }    /* (an effect's IMediaParams) */
/* the script track's tool (dmscript's, part of the track: the track's references; 0040f8c2): ProcessPMsg calls an
 * event's routine, the other methods are not for a graph */
typedef struct { IDirectMusicTool IDirectMusicTool_iface; LONG ref; dm_track *t; } rm_scripttool;
static rm_scripttool *sct(IDirectMusicTool *i) { return CONTAINING_RECORD(i, rm_scripttool, IDirectMusicTool_iface); }
static HRESULT WINAPI sct_qi(IDirectMusicTool *i, REFIID r, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicTool)) { *o = i; InterlockedIncrement(&sct(i)->ref); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI sct_addref(IDirectMusicTool *i) { return InterlockedIncrement(&sct(i)->ref); }
static ULONG WINAPI sct_release(IDirectMusicTool *i)
{
    rm_scripttool *x = sct(i);
    LONG n = InterlockedDecrement(&x->ref);
    if (!n) {
        dm_engine_lock();
        dm_track_release(x->t);
        dm_engine_unlock();
        free(x);
    }
    return n;
}
static HRESULT WINAPI sct_init(IDirectMusicTool *i, IDirectMusicGraph *g) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_delivery(IDirectMusicTool *i, DWORD *d) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_typesize(IDirectMusicTool *i, DWORD *n) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_types(IDirectMusicTool *i, DWORD **t, DWORD n) { return E_UNEXPECTED; }
static HRESULT WINAPI sct_process(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m)
{
    if (!m || !m->punkUser) return E_POINTER;
    return rm_script_track_process(m->punkUser, m->dwVirtualTrackID, ((dm_scripttrack *)sct(i)->t->data)->errnotify);
}
static HRESULT WINAPI sct_flush(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m, REFERENCE_TIME rt) { return DMUS_S_FREE; }
static IDirectMusicToolVtbl sct_vtbl = { sct_qi, sct_addref, sct_release, sct_init, sct_delivery, sct_typesize, sct_types,
                                         sct_process, sct_flush };

/* 0040fb32: each event's script Init (an OLE engine starting runs with the performance let go: its code may call it) */
static void tops_script_init(void *ctx, dm_segstate *ss, dm_track *t, uint32_t vid)
{
    rm_perf *p = ctx;
    dm_scripttrack *T = t->data;
    IUnknown *st = state_object(p, ss);
    for (dm_scriptev *e = T->items; e; e = e->next) {
        if (!e->script) continue;
        IUnknown *sc = e->script;
        IUnknown_AddRef(sc);
        BOOL let = rm_script_init_blocks(sc);
        if (let) dm_host_unlock(p->host);
        rm_script_track_init(sc, (IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, st, vid, T->errnotify);
        if (let) dm_host_lock(p->host);
        dm_host_release_later(sc);
        if (T != t->data) break;        /* (the track loaded again meanwhile) */
    }
    dm_host_release_later(st);
}
/* 0040fbe9: an event's message to the track's tool */
static void tops_script_event(void *ctx, dm_segstate *ss, dm_track *t, const dm_scriptev *e, mtime_t off, uint32_t vid)
{
    rm_perf *p = ctx;
    if (!e->script) return;
    rm_scripttool *x = calloc(1, sizeof(*x));
    if (!x) return;
    x->IDirectMusicTool_iface.lpVtbl = &sct_vtbl;
    x->ref = 1; x->t = t;
    dm_track_addref(t);
    IUnknown *st = state_object(p, ss);
    rm_script_track_event((IDirectMusicPerformance *)&p->IDirectMusicPerformance8_iface, st, &x->IDirectMusicTool_iface,
                          e->script, e->routine, e->flags, (MUSIC_TIME)(e->physical + off), vid);
    dm_host_release_later(st);
    sct_release(&x->IDirectMusicTool_iface);
}
static const dm_toolops s_toolops = { tops_stamp, tops_unstamp, tops_process, tops_flush, tops_clone_ext, tops_free_ext,
                                      tops_state_graph, tops_graph_release, tops_mp_get, tops_mp_add, tops_mp_flush,
                                      tops_mp_release, tops_state_end, tops_script_init, tops_script_event };

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
    if (s && ref_if_alive(&s->ref)) return (IUnknown *)&s->IDirectMusicSegmentState8_iface;
    if (!(s = calloc(1, sizeof(*s)))) return NULL;      /* (one being let go of unhooks itself only if still *w) */
    s->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    s->IDirectMusicGraph_iface.lpVtbl = &stg_vtbl;
    s->ref = 1; s->ss = ss; s->lazy = TRUE;
    dm_segstate_addref(ss);
    dm_segment *g = dm_segstate_segment(ss);
    if (g && g->wrapper && ref_if_alive(&((rm_segment *)g->wrapper)->ref)) s->seg = g->wrapper;
    s->perf = p; IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    dm_audiopath *ap = dm_segstate_audiopath(ss);
    rm_path *a = ap ? *dm_audiopath_wrapper(ap) : NULL;
    if (a && ref_if_alive(&a->ref)) s->path = &a->IDirectMusicAudioPath_iface;
    *w = s;
    return (IUnknown *)&s->IDirectMusicSegmentState8_iface;
}
static rm_state *state_of(rm_perf *p, dm_segstate *ss)
{
    IUnknown *u = state_object(p, ss);
    return u ? sst((IDirectMusicSegmentState8 *)u) : NULL;
}
/* 77305a93 GetGraph, 77305b61 SetGraph: refused while not initialised (before Init, after CloseDown) only to a
 * DirectX 8 application; an older one's graph set before Init is the one Init starts with */
static HRESULT WINAPI pf_getgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph **g)
{
    rm_perf *p = prf(i);
    if (IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    dm_engine_lock();                       /* (Init hands what was set before it to the engine under it) */
    IDirectMusicGraph *x = NULL;
    HRESULT hr;
    if ((!p->host || p->closed) && p->ver >= 8) hr = DMUS_E_NOT_INIT;
    else {
        x = p->host ? dm_perf_get_graph(dm_host_perf(p->host)) : p->pregraph;
        if (x) { *g = x; IDirectMusicGraph_AddRef(x); }
        hr = x ? S_OK : DMUS_E_NOT_FOUND;
    }
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI pf_setgraph(IDirectMusicPerformance8 *i, IDirectMusicGraph *g)
{
    rm_perf *p = prf(i);
    if (g && (IsBadReadPtr(g, 4) || IsBadReadPtr(*(void **)g, 4) || IsBadCodePtr(**(FARPROC **)g))) return E_POINTER;
    IDirectMusicGraph *old = NULL;
    dm_engine_lock();
    if ((!p->host || p->closed) && p->ver >= 8) { dm_engine_unlock(); return DMUS_E_NOT_INIT; }
    if (g) IDirectMusicGraph_AddRef(g);
    if (p->host) dm_perf_set_graph(dm_host_perf(p->host), g);
    else { old = p->pregraph; p->pregraph = g; }
    dm_engine_unlock();
    if (old) IDirectMusicGraph_Release(old);
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
/* the engine's index of a port the performance has (AddPort'ed or not, -1 for none), with the engine lock held */
static int perf_slot(rm_perf *p, rm_port *rp)
{
    for (int k = 0; rp && k < 8; k++) if (p->slot[k] == rp) return k;
    return -1;
}
/* as dmime: a port AddPort'ed again is S_OK; NULL is the synth port, PChannels 0-15 on its channel group 1 when none
 * are assigned yet.  A CreatePort'ed port joins the engine's ports (its own synth); another performance's port is
 * refused. */
static HRESULT WINAPI pf_addport(IDirectMusicPerformance8 *i, IDirectMusicPort *port)
{
    rm_perf *p = prf(i);
    if (!p->host) return DMUS_E_NOT_INIT;
    rm_port *rp = port ? rm_port_of(port) : p->port;
    dm_host *own = rp ? rm_port_own_host(rp) : NULL;
    if (!rp || (rp != p->port && !own)) {
        LOG_INFO("music: Performance::AddPort(%p): not a port of this performance's or IDirectMusic::CreatePort's", (void *)port);
        return E_INVALIDARG;
    }
    HRESULT hr = S_OK;
    long vol = 0;
    int hasvol = 0;
    EnterCriticalSection(&p->gcs);
    for (struct rm_global *e = p->globals; e; e = e->next)
        if (IsEqualGUID(&e->g, &GUID_PerfMasterVolume) && e->size == 4) { memcpy(&vol, e->data, 4); hasvol = 1; }
    LeaveCriticalSection(&p->gcs);
    dm_host_lock(p->host);
    int k = perf_slot(p, rp);
    if (k < 0) {
        k = dm_host_port_add(p->host, own);
        if (k < 1 || k >= 8) { hr = E_OUTOFMEMORY; k = -1; }
        else { p->slot[k] = rp; IDirectMusicPort_AddRef(port); }
    } else if (!p->added[k]) dm_host_port_enable(p->host, k, k ? own : p->host, 1);
    if (k > 0 && hasvol) dm_host_set_volume(own, vol);     /* (the master volume already set) */
    if (k >= 0) p->added[k] = 1;
    if (!port && !dm_perf_pchannels(dm_host_perf(p->host))) dm_perf_assign_block(dm_host_perf(p->host), 0, 0, 1);
    dm_host_unlock(p->host);
    LOG_CALL("music: Performance::AddPort(%p) -> %08lx (port %d)", (void *)port, (unsigned long)hr, k);
    return hr;
}
/* E_INVALIDARG for a port not added; the PChannels on it stay assigned, to no port (PChannelInfo), and play nothing */
static HRESULT WINAPI pf_remport(IDirectMusicPerformance8 *i, IDirectMusicPort *port)
{
    rm_perf *p = prf(i);
    if (!port) return S_OK;
    if (!p->host) return DMUS_E_NOT_INIT;
    rm_port *rp = rm_port_of(port);
    HRESULT hr = E_INVALIDARG;
    dm_host_lock(p->host);
    int k = perf_slot(p, rp);
    if (k >= 0 && p->added[k]) {
        p->added[k] = 0;
        dm_host_port_enable(p->host, k, k ? rm_port_own_host(rp) : p->host, 0);
        hr = S_OK;
    }
    dm_host_unlock(p->host);
    return hr;
}
/* the PChannels to an AddPort'ed port (E_INVALIDARG for another); S_FALSE when the group is beyond the port's channel
 * groups (assigned all the same), as dmime answers */
static HRESULT perf_assign(rm_perf *p, IDirectMusicPort *port, DWORD first, DWORD n, DWORD g, DWORD c)
{
    if (!port) return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    rm_port *rp = rm_port_of(port);
    HRESULT hr;
    dm_host_lock(p->host);
    int k = perf_slot(p, rp);
    if (k < 0 || !p->added[k]) hr = E_INVALIDARG;
    else {
        DWORD have = rm_port_groups(rp);
        dm_perf *pf = dm_host_perf(p->host);
        hr = n == 16 ? dm_perf_assign_block(pf, first / 16, k, g) : dm_perf_assign_pchannel(pf, first, k, g, c);
        if (hr == S_OK && g > have) hr = S_FALSE;
    }
    dm_host_unlock(p->host);
    return hr;
}
static HRESULT WINAPI pf_assignblock(IDirectMusicPerformance8 *i, DWORD b, IDirectMusicPort *port, DWORD g)
{
    return perf_assign(prf(i), port, b * 16, 16, g, 0);
}
static HRESULT WINAPI pf_assignpch(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort *port, DWORD g, DWORD c)
{
    return perf_assign(prf(i), port, pch, 1, g, c);
}
/* 7730672d */
static HRESULT WINAPI pf_pchinfo(IDirectMusicPerformance8 *i, DWORD pch, IDirectMusicPort **port, DWORD *g, DWORD *c)
{
    rm_perf *p = prf(i);
    if ((port && IsBadWritePtr(port, sizeof(*port))) || (g && IsBadWritePtr(g, sizeof(*g))) || (c && IsBadWritePtr(c, sizeof(*c))))
        return E_POINTER;
    if (!p->host) return DMUS_E_NOT_INIT;
    int idx = 0, hr; uint32_t grp = 0, ch = 0;
    IDirectMusicPort *pp = NULL;            /* (none when its port was removed) */
    PERF_LOCKED(p, hr = dm_perf_pchannel_info(dm_host_perf(p->host), pch, &idx, &grp, &ch);
                if (hr >= 0 && idx >= 0 && idx < 8 && p->added[idx] && p->slot[idx]) rm_port_query(p->slot[idx], &IID_IDirectMusicPort, (void **)&pp));
    if (hr < 0) return E_INVALIDARG;
    if (port) *port = pp;
    else if (pp) IDirectMusicPort_Release(pp);
    if (g) *g = grp;
    if (c) *c = ch;
    return S_OK;
}
/* the port the PChannel plays through (E_INVALIDARG when it has none, nothing written), its DownloadInstrument, and
 * the PChannel's group and MIDI channel as PChannelInfo gives them */
static HRESULT WINAPI pf_downloadinst(IDirectMusicPerformance8 *i, IDirectMusicInstrument *in, DWORD pch, IDirectMusicDownloadedInstrument **d,
                                      DMUS_NOTERANGE *r, DWORD n, IDirectMusicPort **port, DWORD *g, DWORD *c)
{
    rm_perf *p = prf(i);
    if (!in || !d || !port || !g || !c || IsBadWritePtr(d, sizeof(*d)) || IsBadWritePtr(port, sizeof(*port)) ||
        IsBadWritePtr(g, sizeof(*g)) || IsBadWritePtr(c, sizeof(*c)))
        return E_POINTER;
    if (!p->host || !p->port) return DMUS_E_NOT_INIT;
    int idx = 0, hr; uint32_t grp = 0, ch = 0;
    IDirectMusicPort *pt = NULL;
    PERF_LOCKED(p, hr = dm_perf_pchannel_info(dm_host_perf(p->host), pch, &idx, &grp, &ch);
                if (hr >= 0 && idx >= 0 && idx < 8 && p->added[idx] && p->slot[idx]) rm_port_query(p->slot[idx], &IID_IDirectMusicPort, (void **)&pt));
    if (hr < 0 || !pt) return E_INVALIDARG;
    HRESULT h;
    h = IDirectMusicPort_DownloadInstrument(pt, in, d, r, n);
    if (FAILED(h)) { IDirectMusicPort_Release(pt); return h; }
    *port = pt;
    *g = grp;
    *c = ch;
    return h;
}
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
    dm_dltarget dt; void *data;
    HRESULT hr = prm_data(prm, pp, &dt, &data);
    if (FAILED(hr)) return hr;
    dm_host_lock(p->host);
    hr = dm_perf_setparam(dm_host_perf(p->host), prm, gb, n, t, data);
    dm_host_unlock(p->host);
    prm_drop(prm, &dt, data);
    LOG_CALL("music: Performance::SetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, (unsigned)hr);
    return hr;
}
/* 7730709c: the global parameters are what SetGlobalParam kept (E_INVALIDARG when not set, or of another size) */
static HRESULT WINAPI pf_getglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz)
{
    rm_perf *p = prf(i);
    if (!g || (sz && !v)) return E_POINTER;
    HRESULT hr = E_INVALIDARG;
    EnterCriticalSection(&p->gcs);
    for (struct rm_global *e = p->globals; e; e = e->next)
        if (IsEqualGUID(&e->g, g)) {
            if (e->size == sz) { memcpy(v, e->data, sz); hr = S_OK; }
            break;
        }
    LeaveCriticalSection(&p->gcs);
    return hr;
}
/* 7730c4c3: kept by GUID (the size fixed by the first); the master tempo and volume take effect, as do the
 * groove level and automatic download the engine reads */
static HRESULT WINAPI pf_setglobal(IDirectMusicPerformance8 *i, REFGUID g, void *v, DWORD sz)
{
    rm_perf *p = prf(i);
    char b[64];
    if (!g || (sz && !v)) return E_POINTER;
    LOG_CALL("music: Performance::SetGlobalParam(%s)", guidstr(g, b));
    if (IsEqualGUID(g, &GUID_PerfMasterTempo)) {
        if (sz != 4) return E_INVALIDARG;
        if (p->host) { dm_host_lock(p->host); dm_perf_set_master_tempo(dm_host_perf(p->host), *(float *)v); dm_host_unlock(p->host); }
    } else if (IsEqualGUID(g, &GUID_PerfMasterVolume)) {
        if (sz != 4) return E_INVALIDARG;
        if (p->host) {
            dm_host_set_volume(p->host, *(long *)v);
            dm_host_lock(p->host);              /* (each port's synth, as dmime sets the volume on each port) */
            for (int k = 1; k < 8; k++) if (p->slot[k] && p->added[k]) dm_host_set_volume(rm_port_own_host(p->slot[k]), *(long *)v);
            dm_host_unlock(p->host);
        }
    }
    struct rm_global *e;
    EnterCriticalSection(&p->gcs);
    for (e = p->globals; e; e = e->next) if (IsEqualGUID(&e->g, g)) break;
    if (e && e->size != sz) { LeaveCriticalSection(&p->gcs); return E_INVALIDARG; }
    if (!e) {
        if (!(e = calloc(1, sizeof(*e))) || (sz && !(e->data = malloc(sz)))) { free(e); LeaveCriticalSection(&p->gcs); return E_OUTOFMEMORY; }
        e->g = *g; e->size = sz;
        e->next = p->globals; p->globals = e;
    }
    if (sz) memcpy(e->data, v, sz);
    LeaveCriticalSection(&p->gcs);
    if (IsEqualGUID(g, &GUID_PerfAutoDownload) && sz >= 4 && p->host) { dm_host_lock(p->host); dm_perf_set_auto_download(dm_host_perf(p->host), *(BOOL *)v); dm_host_unlock(p->host); }
    if (IsEqualGUID(g, &G_PerfMasterGrooveLevel) && sz >= 1 && p->host) { dm_host_lock(p->host); dm_perf_set_groove(dm_host_perf(p->host), *(char *)v); dm_host_unlock(p->host); }
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
    if (p->host && !p->closed) return DMUS_E_ALREADY_INITED;
    p->audioflags = flags ? flags : DMUS_AUDIOF_ALL;
    uint32_t rate = 22050, voices = 64;
    if (params && params->dwSize >= sizeof(DMUS_AUDIOPARAMS)) {
        if ((params->dwValidData & DMUS_AUDIOPARAMS_SAMPLERATE) && params->dwSampleRate) rate = params->dwSampleRate;
        if ((params->dwValidData & DMUS_AUDIOPARAMS_VOICES) && params->dwVoices) voices = params->dwVoices;
    }
    HRESULT hr = perf_reopen(p);
    if (hr == S_OK) {                       /* (again after CloseDown: the synth port added, as InitAudio adds it) */
        dm_host_lock(p->host);
        if (!p->added[0]) { p->added[0] = 1; dm_host_port_enable(p->host, 0, p->host, 1); }
        dm_host_unlock(p->host);
    } else hr = ensure_host(p, rate, voices);
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
    char name[64];
    snprintf(name, sizeof(name), "%s", g->seg->name);     /* (a Load on another thread may replace the segment) */
    if (hr >= 0 && from) {
        rm_segment *fg = unsafe_segment(from);
        rm_state *fs = unsafe_state(from);
        mtime_t t;
        dm_segstate_start_time(ss, &t);
        if (fg || fs) dm_perf_stop(pf, fg ? fg->seg : NULL, fs ? fs->ss : NULL, t, 0);
    }
    dm_host_unlock(p->host);
    LOG_CALL("music: PlaySegment(%s, flags %lx, %lld) -> %08x", name, (unsigned long)flags, (long long)start, hr);
    if (hr < 0) return hr;
    rm_state *n = calloc(1, sizeof(*n));   /* (made before the lock, put in under it: a tool may make one meanwhile) */
    dm_host_lock(p->host);
    rm_state *s = *dm_segstate_wrapper(ss);
    if (s && ref_if_alive(&s->ref)) {       /* made on demand already (a tool or notification met the state first): the application's now */
        free(n);
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
    s = n;
    if (!s) { dm_segstate_release(ss); dm_host_unlock(p->host); return E_OUTOFMEMORY; }
    s->IDirectMusicSegmentState8_iface.lpVtbl = &st_vtbl;
    s->IDirectMusicGraph_iface.lpVtbl = &stg_vtbl;
    s->ref = 1;
    s->ss = ss;
    s->seg = g; IDirectMusicSegment8_AddRef(&g->IDirectMusicSegment8_iface);
    s->perf = p; IDirectMusicPerformance8_AddRef(&p->IDirectMusicPerformance8_iface);
    rm_path *pa = a ? a : p->defpath;
    if (pa) { s->path = &pa->IDirectMusicAudioPath_iface; IDirectMusicAudioPath_AddRef(s->path); }
    *dm_segstate_wrapper(ss) = s;
    dm_host_unlock(p->host);
    if (out) *out = (IDirectMusicSegmentState *)&s->IDirectMusicSegmentState8_iface;
    else IDirectMusicSegmentState8_Release(&s->IDirectMusicSegmentState8_iface);
    return S_OK;
}

/* 7730d51c: with DMUS_SEGF_AUTOTRANSITION and a template, the template composes a transition from the segment
 * playing then (the from state's, else the primary one, unless secondary) into this one: the transition plays as
 * asked, then the segment from the transition's end (its secondary and control flags only) */
static HRESULT WINAPI pf_playex(IDirectMusicPerformance8 *i, IUnknown *src, WCHAR *name, IUnknown *trans, DWORD flags, __int64 start,
                                IDirectMusicSegmentState **out, IUnknown *from, IUnknown *path)
{
    rm_perf *p = prf(i);
    rm_segment *tmpl = (flags & DMUS_SEGF_AUTOTRANSITION) && trans ? unsafe_segment(trans) : NULL;
    rm_segment *to = unsafe_segment(src);
    if (!tmpl || !to || !p->host) return pf_play(p, src, flags, start, out, from, path);
    REFERENCE_TIME rt, rr;
    MUSIC_TIME mt = 0;
    if (!start) pf_getqueue(i, &rt);
    else if (!(flags & DMUS_SEGF_REFTIME)) pf_m2r(i, (MUSIC_TIME)start, &rt);
    else rt = start;
    pf_resolved(i, rt, &rr, flags);
    pf_r2m(i, rr, &mt);
    rm_state *fs = from ? unsafe_state(from) : NULL;
    IDirectMusicSegment *fromseg = NULL;
    mtime_t st0 = 0;
    dm_host_lock(p->host);
    dm_segstate *st = fs ? fs->ss : !(flags & DMUS_SEGF_SECONDARY) ? dm_perf_primary_at(dm_host_perf(p->host), mt) : NULL;
    if (st) {
        dm_segment *g = dm_segstate_segment(st);
        dm_segstate_start_time(st, &st0);
        if (g && g->wrapper && ref_if_alive(&((rm_segment *)g->wrapper)->ref))
            fromseg = (IDirectMusicSegment *)&((rm_segment *)g->wrapper)->IDirectMusicSegment8_iface;
    }
    dm_host_unlock(p->host);
    IDirectMusicSegment *ts = NULL;
    IDirectMusicSegment8_Compose(&tmpl->IDirectMusicSegment8_iface, st ? mt - (MUSIC_TIME)st0 : 0, fromseg,
                                 (IDirectMusicSegment *)&to->IDirectMusicSegment8_iface, &ts);
    if (fromseg) IDirectMusicSegment_Release(fromseg);
    if (!ts) return pf_play(p, src, flags, start, out, from, path);
    IDirectMusicSegmentState *s1 = NULL;
    HRESULT hr = pf_play(p, (IUnknown *)ts, flags, start, &s1, from, path);
    IDirectMusicSegment_Release(ts);
    if (FAILED(hr)) return hr;
    rm_state *r1 = unsafe_state((IUnknown *)s1);
    mtime_t end = 0;
    dm_host_lock(p->host);
    if (r1 && r1->ss) end = dm_segstate_end(r1->ss);
    dm_host_unlock(p->host);
    IDirectMusicSegmentState_Release(s1);
    return pf_play(p, src, flags & (DMUS_SEGF_SECONDARY | DMUS_SEGF_CONTROL), end, out, NULL, path);
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
            if (pm->nbuf > 0xffffu) { pm->nbuf = 0; hr = E_INVALIDARG; break; }   /* (a broken count: nbuf + 1 would wrap) */
            pm->b = calloc(pm->nbuf + 1, sizeof(rm_buf *));
            if (!pm->b) { hr = E_OUTOFMEMORY; break; }
            for (DWORD i = 0; i < pm->nbuf; i++) if ((pm->b[i] = path_find(a, pc, &cp->maps[m].bufs[i]))) hold(a, pm->b[i]);
        }
    }
    /* the PChannels */
    ULONGLONG total64 = 0;
    for (DWORD k = 0; k < c->nports; k++) {
        total64 += c->ports[k].h.dwPChannelCount;
        for (DWORD m = 0; m < c->ports[k].nmaps; m++) total64 += c->ports[k].maps[m].count;
    }
    if (total64 > 0x100000 && SUCCEEDED(hr)) hr = E_INVALIDARG;    /* (broken counts: the arrays below would wrap) */
    DWORD total = total64 > 0x100000 ? 0 : (DWORD)total64;
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

/* ------------------------------------------------------------------ IDirectMusicPatternTrack (774019c8) */
static rm_track *ptk(IDirectMusicPatternTrack *i) { return impl_from(i, rm_track, IDirectMusicPatternTrack_iface); }
static HRESULT WINAPI ptk_qi(IDirectMusicPatternTrack *i, REFIID r, void **o) { return trk_qi(&ptk(i)->IDirectMusicTrack8_iface, r, o); }
static ULONG WINAPI ptk_addref(IDirectMusicPatternTrack *i) { return trk_addref(&ptk(i)->IDirectMusicTrack8_iface); }
static ULONG WINAPI ptk_release(IDirectMusicPatternTrack *i) { return trk_release(&ptk(i)->IDirectMusicTrack8_iface); }
static BOOL bad_iface(void *p) { return IsBadReadPtr(p, 4) || IsBadReadPtr(*(void **)p, 4) || IsBadCodePtr(**(FARPROC **)p); }
/* 77415094 */
static HRESULT WINAPI ptk_createsegment(IDirectMusicPatternTrack *i, IDirectMusicStyle *style, IDirectMusicSegment **out)
{
    if (bad_iface(style) || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    struct dms_style *st = style_hold_of(style);
    if (!st) return E_NOINTERFACE;      /* (dmstyle's private style interface) */
    dm_engine_lock();
    dm_segment *g = NULL;
    HRESULT hr = dms_patterntrack_create_segment(ptk(i)->t, st, &g);
    dm_engine_unlock();
    dms_style_release(st);
    if (FAILED(hr)) return hr;
    rm_segment *n = new_segment(g);
    if (!n) { dm_segment_release(g); return E_OUTOFMEMORY; }
    *out = (IDirectMusicSegment *)&n->IDirectMusicSegment8_iface;
    return S_OK;
}
/* 774143af */
static HRESULT WINAPI ptk_setvariation(IDirectMusicPatternTrack *i, IDirectMusicSegmentState *ss, DWORD flags, DWORD part)
{
    if (ss && bad_iface(ss)) return E_POINTER;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    dm_engine_lock();
    HRESULT hr = dms_patterntrack_set_variation(ptk(i)->t, st ? st->ss : NULL, flags, part);
    dm_engine_unlock();
    return hr;
}
/* 77413c30: the style's pattern (0) or motif (1) of that name (DMUS_E_NOT_FOUND without one) */
static HRESULT WINAPI ptk_setpatternbyname(IDirectMusicPatternTrack *i, IDirectMusicSegmentState *ss, WCHAR *name,
                                           IDirectMusicStyle *style, DWORD type, DWORD *length)
{
    if ((ss && bad_iface(ss)) || IsBadReadPtr(name, 4) || bad_iface(style) || IsBadWritePtr(length, 4)) return E_POINTER;
    struct dms_style *sty = style_hold_of(style);
    if (!sty) return E_NOINTERFACE;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    dm_engine_lock();
    const dms_pattern *p = NULL;
    dms_pattern **list = type == 0 ? sty->patterns : type == 1 ? sty->motifs : NULL;
    int n = type == 0 ? sty->npatterns : type == 1 ? sty->nmotifs : 0;
    for (int k = 0; k < n && !p; k++) if (!_wcsicmp((const WCHAR *)list[k]->name, name)) p = list[k];     /* 7740ac84 */
    HRESULT hr = (int32_t)0x88781161;
    if (p) {
        mtime_t len = 0;
        hr = dms_patterntrack_set_pattern(ptk(i)->t, st ? st->ss : NULL, p, &len);
        if (SUCCEEDED(hr) || hr != (int32_t)0x88781162) *length = (DWORD)len;
    }
    dm_engine_unlock();
    dms_style_release(sty);
    return hr;
}
static IDirectMusicPatternTrackVtbl ptk_vtbl = { ptk_qi, ptk_addref, ptk_release, ptk_createsegment, ptk_setvariation, ptk_setpatternbyname };
static dm_band *band_hold_of(void *p)        /* (ours only: no call into the object) */
{
    if (!p || IsBadReadPtr(p, sizeof(void *))) return NULL;
    void *vt = *(void **)p;
    rm_band *b = vt == &bnd_vtbl ? bnd(p) : vt == &bno_vtbl ? bno(p) : vt == &bnp_vtbl ? bnp(p) : NULL;
    return b ? band_hold(b) : NULL;
}
static void prm_out(int prm, int hr, void *p)
{
    if ((prm == PRM_STYLE || prm == PRM_PATSTYLE) && hr >= 0 && p) *(void **)p = style_iface(*(dms_style **)p);
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
static dm_wave *loader_getwave(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    IDirectMusicObject *o = NULL;
    if (FAILED(ldr_get(ctx, &RM_CLSID_DirectSoundWave, &IID_IDirectMusicObject, g, file, name, (void **)&o)) || !o) {
        LOG_INFO("music: wave track: wave %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    rm_wave *w = o->lpVtbl == &wvo_vtbl ? wvo(o) : NULL;
    dm_engine_lock();                   /* (a Load on another thread may replace it) */
    dm_wave *r = w ? w->w : NULL;
    dm_wave_addref(r);
    dm_engine_unlock();
    IDirectMusicObject_Release(o);
    return r;
}

/* a segment trigger track's reference (77323220: the loader's GetObject of a segment); the engine segment with a
 * reference */
static dm_segment *loader_getseg(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    IDirectMusicSegment8 *o = NULL;
    if (FAILED(ldr_get(ctx, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, g, file, name, (void **)&o)) || !o) {
        LOG_INFO("music: segment trigger track: segment %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    rm_segment *r = o->lpVtbl == &seg_vtbl ? sgm(o) : NULL;
    dm_engine_lock();                       /* (the engine thread counts it too; a Load may replace it) */
    dm_segment *x = r ? r->seg : NULL;
    if (x) x->refs++;
    dm_engine_unlock();
    IDirectMusicSegment8_Release(o);
    return x;
}

/* a script track's reference (0040f6fd: the loader's GetObject of a script; its failure is the track's) */
static void *loader_getscript(void *ctx, const dm_guid *g, const char *file, const char *name, int32_t *hr)
{
    IDirectMusicScript *o = NULL;
    *hr = ldr_get(ctx, &CLSID_DirectMusicScript, &IID_IDirectMusicScript, g, file, name, (void **)&o);
    if (FAILED(*hr) || !o) {
        LOG_INFO("music: script track: script %s not found", file ? file : name ? name : "(by guid)");
        if (o) IDirectMusicScript_Release(o);
        if (SUCCEEDED(*hr)) *hr = E_FAIL;
        return NULL;
    }
    return o;
}
static void resort_hook(int enter)
{
    if (enter) dm_engine_lock();
    else dm_engine_unlock();
}
/* (a script track's scripts: let go of once the performance's lock is, as a script's last release can call the loader) */
static void unk_ref(void *u, int add) { if (add) IUnknown_AddRef((IUnknown *)u); else dm_host_release_later(u); }

/* a style track's reference (774123a0: the loader's GetObject of a style); the engine style with a reference */
static struct dms_style *loader_getstyle(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    IDirectMusicStyle8 *o = NULL;
    if (FAILED(ldr_get(ctx, &RM_CLSID_DirectMusicStyle, &RM_IID_IDirectMusicStyle8, g, file, name, (void **)&o)) || !o) {
        LOG_INFO("music: style track: style %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    dms_style *r = style_hold_of(o);
    IDirectMusicStyle8_Release(o);
    return r;
}

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
