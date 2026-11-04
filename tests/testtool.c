/* testtool - a DirectMusic tool in a DLL of its own (CLSID {8f6d1a10-7001-4000-8000-000000000001}), for a segment
 * file's tool graph ('DMTG') to load: tests/tooltest.c `filetool` registers it under HKEY_CLASSES_ROOT, loads
 * tools/sty's tgraph.sgt (cases.py toolgraph) and prints what the tools were asked and what they saw.
 *
 * Each tool loads its chunk through IPersistStream (the third DWORD read, after the chunk's header: how far it
 * transposes notes), takes every message type, transposes notes, and logs Init, Load, Clone and every message; the
 * log is read back through tt_log (the test loads the same DLL).  IDirectMusicTool8, so a graph's Clone clones it. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dmusici.h>
#include <dmusicc.h>
#include <string.h>

DEFINE_GUID(CLSID_TestTool, 0x8f6d1a10, 0x7001, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01);

typedef struct { LONG tool; DWORD what; DWORD a, b, c, d; } tt_entry;
enum { TT_MADE, TT_INIT, TT_LOAD, TT_CLONE, TT_MSG, TT_FLUSH };
static tt_entry s_log[2048];
static volatile LONG s_n;
static LONG s_serial;
static void log4(LONG tool, DWORD what, DWORD a, DWORD b, DWORD c, DWORD d)
{
    LONG k = InterlockedIncrement(&s_n) - 1;
    if (k >= 2048) return;
    s_log[k].tool = tool; s_log[k].what = what; s_log[k].a = a; s_log[k].b = b; s_log[k].c = c; s_log[k].d = d;
}
int __cdecl tt_log(const tt_entry **out) { *out = s_log; return s_n > 2048 ? 2048 : (int)s_n; }
void __cdecl tt_clear(void) { InterlockedExchange(&s_n, 0); }

typedef struct {
    IDirectMusicTool8 IDirectMusicTool8_iface;
    IPersistStream IPersistStream_iface;
    LONG ref, serial;
    LONG transpose;
} tool;
static tool *tl(IDirectMusicTool8 *i) { return CONTAINING_RECORD(i, tool, IDirectMusicTool8_iface); }
static tool *tps(IPersistStream *i) { return CONTAINING_RECORD(i, tool, IPersistStream_iface); }
static LONG s_objects;
static tool *tool_new(void);

static HRESULT WINAPI t_qi(IDirectMusicTool8 *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicTool) || IsEqualIID(r, &IID_IDirectMusicTool8)) *o = i;
    else if (IsEqualIID(r, &IID_IPersistStream) || IsEqualIID(r, &IID_IPersist)) *o = &tl(i)->IPersistStream_iface;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&tl(i)->ref);
    return S_OK;
}
static ULONG WINAPI t_addref(IDirectMusicTool8 *i) { return InterlockedIncrement(&tl(i)->ref); }
static ULONG WINAPI t_release(IDirectMusicTool8 *i)
{
    LONG r = InterlockedDecrement(&tl(i)->ref);
    if (!r) { HeapFree(GetProcessHeap(), 0, tl(i)); InterlockedDecrement(&s_objects); }
    return r;
}
static HRESULT WINAPI t_init(IDirectMusicTool8 *i, IDirectMusicGraph *g) { log4(tl(i)->serial, TT_INIT, g != NULL, 0, 0, 0); return S_OK; }
static HRESULT WINAPI t_deliv(IDirectMusicTool8 *i, DWORD *d) { *d = DMUS_PMSGF_TOOL_IMMEDIATE; return S_OK; }
static HRESULT WINAPI t_ntypes(IDirectMusicTool8 *i, DWORD *n) { *n = 0; return S_OK; }
static HRESULT WINAPI t_types(IDirectMusicTool8 *i, DWORD **a, DWORD n) { return E_NOTIMPL; }
static HRESULT WINAPI t_process(IDirectMusicTool8 *i, IDirectMusicPerformance *p, DMUS_PMSG *m)
{
    tool *t = tl(i);
    DWORD v = 0;
    if (m->dwType == DMUS_PMSGT_NOTE) {
        DMUS_NOTE_PMSG *n = (DMUS_NOTE_PMSG *)m;
        n->bMidiValue = (BYTE)(n->bMidiValue + t->transpose);
        v = n->bMidiValue;
    }
    log4(t->serial, TT_MSG, m->dwType, m->dwPChannel, (DWORD)m->mtTime, v);
    if (m->pGraph && SUCCEEDED(IDirectMusicGraph_StampPMsg(m->pGraph, m))) return DMUS_S_REQUEUE;
    return DMUS_S_FREE;
}
static HRESULT WINAPI t_flush(IDirectMusicTool8 *i, IDirectMusicPerformance *p, DMUS_PMSG *m, REFERENCE_TIME rt)
{
    log4(tl(i)->serial, TT_FLUSH, m->dwType, m->dwPChannel, 0, 0);
    return DMUS_S_FREE;
}
static HRESULT WINAPI t_clone(IDirectMusicTool8 *i, IDirectMusicTool **o)
{
    tool *c = tool_new();
    if (!c) return E_OUTOFMEMORY;
    c->transpose = tl(i)->transpose;
    log4(tl(i)->serial, TT_CLONE, c->serial, 0, 0, 0);
    *o = (IDirectMusicTool *)&c->IDirectMusicTool8_iface;
    return S_OK;
}
static IDirectMusicTool8Vtbl t_vtbl = { t_qi, t_addref, t_release, t_init, t_deliv, t_ntypes, t_types, t_process, t_flush, t_clone };

static HRESULT WINAPI ps_qi(IPersistStream *i, REFIID r, void **o) { return t_qi(&tps(i)->IDirectMusicTool8_iface, r, o); }
static ULONG WINAPI ps_addref(IPersistStream *i) { return t_addref(&tps(i)->IDirectMusicTool8_iface); }
static ULONG WINAPI ps_release(IPersistStream *i) { return t_release(&tps(i)->IDirectMusicTool8_iface); }
static HRESULT WINAPI ps_classid(IPersistStream *i, CLSID *c) { *c = CLSID_TestTool; return S_OK; }
static HRESULT WINAPI ps_dirty(IPersistStream *i) { return S_FALSE; }
/* the stream at the tool's chunk: its header, then a DWORD (the transposition) */
static HRESULT WINAPI ps_load(IPersistStream *i, IStream *s)
{
    tool *t = tps(i);
    DWORD h[3] = { 0, 0, 0 };
    ULONG got = 0;
    HRESULT hr = IStream_Read(s, h, sizeof(h), &got);
    t->transpose = (LONG)h[2];
    log4(t->serial, TT_LOAD, h[0], h[1], h[2], got);
    return SUCCEEDED(hr) && got == sizeof(h) ? S_OK : E_FAIL;
}
static HRESULT WINAPI ps_save(IPersistStream *i, IStream *s, BOOL c) { return E_NOTIMPL; }
static HRESULT WINAPI ps_size(IPersistStream *i, ULARGE_INTEGER *n) { return E_NOTIMPL; }
static IPersistStreamVtbl ps_vtbl = { ps_qi, ps_addref, ps_release, ps_classid, ps_dirty, ps_load, ps_save, ps_size };

static tool *tool_new(void)
{
    tool *t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*t));
    if (!t) return NULL;
    t->IDirectMusicTool8_iface.lpVtbl = &t_vtbl;
    t->IPersistStream_iface.lpVtbl = &ps_vtbl;
    t->ref = 1;
    t->serial = InterlockedIncrement(&s_serial);
    InterlockedIncrement(&s_objects);
    log4(t->serial, TT_MADE, 0, 0, 0, 0);
    return t;
}

/* the class factory */
static HRESULT WINAPI cf_qi(IClassFactory *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IClassFactory)) { *o = i; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI cf_addref(IClassFactory *i) { return 2; }
static ULONG WINAPI cf_release(IClassFactory *i) { return 1; }
static HRESULT WINAPI cf_create(IClassFactory *i, IUnknown *outer, REFIID r, void **o)
{
    *o = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    tool *t = tool_new();
    if (!t) return E_OUTOFMEMORY;
    HRESULT hr = t_qi(&t->IDirectMusicTool8_iface, r, o);
    t_release(&t->IDirectMusicTool8_iface);
    return hr;
}
static HRESULT WINAPI cf_lock(IClassFactory *i, BOOL l) { return S_OK; }
static IClassFactoryVtbl cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static IClassFactory s_cf = { &cf_vtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, void **o)
{
    if (!IsEqualCLSID(c, &CLSID_TestTool)) { *o = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
    return cf_qi(&s_cf, r, o);
}
HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }    /* (the test reads the log) */
