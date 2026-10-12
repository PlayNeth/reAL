/* reAL-Music: the tool graph (dmime's CGraph, vtables 0x77301194 / 0x77301174 / 0x7730115c / 0x7730114c).
 *
 * A graph is an ordered list of tools.  StampPMsg moves a message on to the next tool after its pTool that takes the
 * message's type on its PChannel, and sets the message's delivery from that tool; after the last one it answers
 * DMUS_S_LAST_TOOL.  The performance, segment states and audiopaths chain their graphs (dmcom.c, dmperf.c).
 * Tools removed from a graph stay referenced until the graph goes, since messages may still point at them. */
#define COBJMACROS
#include "../real.h"
#include <dmusici.h>
#include <dmusicf.h>
#include <dmplugin.h>
#include <string.h>
#include "dmgraph.h"

/* dmime's private interfaces: the graph itself (77301484) and its Clone (77301474) */
static const GUID RM_IID_GraphSelf = { 0xb06c0c24, 0xd3c7, 0x11d3, { 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID RM_IID_GraphClone = { 0xb06c0c27, 0xd3c7, 0x11d3, { 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID RM_GUID_All_Objects = { 0xaa114de5, 0xc262, 0x4169, { 0xa1, 0xc8, 0x23, 0xd6, 0x98, 0xcc, 0x73, 0xb5 } };

typedef struct gnode {          /* 77302679: 0x30 bytes */
    struct gnode *next;         /* +0x00 */
    GUID clsid;                 /* +0x04 */
    BOOL is8;                   /* +0x14 IDirectMusicTool8 */
    IDirectMusicTool *tool;     /* +0x18 */
    DWORD delivery;             /* +0x1c */
    DWORD ntypes; DWORD *types; /* +0x20 +0x24 */
    DWORD npch; DWORD *pch;     /* +0x28 +0x2c */
} gnode;
typedef struct gremoved { IDirectMusicTool *tool; struct gremoved *next; } gremoved;

typedef struct {
    IUnknown IUnknown_iface;
} gclone;
typedef struct rm_graph {
    IDirectMusicGraph IDirectMusicGraph_iface;          /* +0x00 */
    IPersistStream IPersistStream_iface;                /* +0x04 */
    IDirectMusicObject IDirectMusicObject_iface;        /* +0x08 */
    IUnknown IGraphClone_iface;                         /* +0x0c */
    gnode *tools;                                       /* +0x10 */
    CRITICAL_SECTION cs;                                /* +0x18 */
    LONG ref;                                           /* +0x30 */
    DWORD valid;                                        /* +0x34 DMUS_OBJ_* */
    GUID guid;                                          /* +0x38 */
    FILETIME date;                                      /* +0x48 */
    DMUS_VERSION version;                               /* +0x50 */
    WCHAR name[DMUS_MAX_NAME], category[DMUS_MAX_CATEGORY], file[DMUS_MAX_FILENAME];
    gremoved *removed;                                  /* +0x360 */
} rm_graph;

static IDirectMusicGraphVtbl g_vtbl;
static IPersistStreamVtbl gp_vtbl;
static IDirectMusicObjectVtbl go_vtbl;
typedef struct { HRESULT (WINAPI *QueryInterface)(IUnknown *, REFIID, void **); ULONG (WINAPI *AddRef)(IUnknown *);
                 ULONG (WINAPI *Release)(IUnknown *); HRESULT (WINAPI *Clone)(IUnknown *, IDirectMusicGraph **); } gc_vtbl_t;
static gc_vtbl_t gc_vtbl;

static rm_graph *gr(IDirectMusicGraph *i) { return CONTAINING_RECORD(i, rm_graph, IDirectMusicGraph_iface); }
static rm_graph *grp(IPersistStream *i) { return CONTAINING_RECORD(i, rm_graph, IPersistStream_iface); }
static rm_graph *gro(IDirectMusicObject *i) { return CONTAINING_RECORD(i, rm_graph, IDirectMusicObject_iface); }
static rm_graph *grc(IUnknown *i) { return CONTAINING_RECORD(i, rm_graph, IGraphClone_iface); }

BOOL rm_graph_ours(IDirectMusicGraph *g) { return g && g->lpVtbl == &g_vtbl; }

/* 773026e1: a bounded copy; a string that does not fit is cut (0x8007007a) */
static HRESULT wcopy(WCHAR *d, DWORD n, const WCHAR *s)
{
    if (!n) return E_INVALIDARG;
    while (*s) { *d++ = *s++; if (!--n) { d[-1] = 0; return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER); } }
    *d = 0;
    return S_OK;
}

/* 773026aa */
static void node_free(gnode *n)
{
    free(n->pch);
    free(n->types);
    if (n->tool) IDirectMusicTool_Release(n->tool);
    free(n);
}

static int list_count(gnode *n) { int c = 0; for (; n; n = n->next) c++; return c; }

/* ------------------------------------------------------------------ construction (773036e2, 773037cd) */
static rm_graph *graph_new(void)
{
    rm_graph *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->IDirectMusicGraph_iface.lpVtbl = &g_vtbl;
    g->IPersistStream_iface.lpVtbl = &gp_vtbl;
    g->IDirectMusicObject_iface.lpVtbl = &go_vtbl;
    g->IGraphClone_iface.lpVtbl = (IUnknownVtbl *)&gc_vtbl;
    g->ref = 1;
    g->valid = DMUS_OBJ_CLASS;
    InitializeCriticalSection(&g->cs);
    return g;
}

/* 7730304d: drop every tool (S_FALSE: there were none) */
static HRESULT clear_tools(rm_graph *g)
{
    EnterCriticalSection(&g->cs);
    HRESULT hr = g->tools ? S_OK : S_FALSE;
    gnode *n;
    while ((n = g->tools)) { g->tools = n->next; node_free(n); }
    LeaveCriticalSection(&g->cs);
    return hr;
}

static void graph_free(rm_graph *g)
{
    for (gremoved *r = g->removed; r; r = r->next) IDirectMusicTool_Release(r->tool);    /* 77303695 */
    clear_tools(g);
    DeleteCriticalSection(&g->cs);
    while (g->removed) { gremoved *r = g->removed; g->removed = r->next; free(r); }
    free(g);
}

/* ------------------------------------------------------------------ IUnknown (7730272a, 77302820, 77303aef) */
static HRESULT WINAPI g_qi(IDirectMusicGraph *i, REFIID riid, void **out)
{
    if (!out || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    if (!riid || IsBadReadPtr(riid, sizeof(GUID))) return E_POINTER;
    rm_graph *g = gr(i);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicGraph) || IsEqualIID(riid, &RM_IID_GraphSelf))
        *out = &g->IDirectMusicGraph_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &g->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream)) *out = &g->IPersistStream_iface;
    else if (IsEqualIID(riid, &RM_IID_GraphClone)) *out = &g->IGraphClone_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&g->ref);
    return S_OK;
}
static ULONG WINAPI g_addref(IDirectMusicGraph *i) { return InterlockedIncrement(&gr(i)->ref); }
static ULONG WINAPI g_release(IDirectMusicGraph *i)
{
    rm_graph *g = gr(i);
    LONG r = InterlockedDecrement(&g->ref);
    if (!r) graph_free(g);
    return r;
}

/* ------------------------------------------------------------------ tools */
/* 77302861: insert a tool (its class from IPersist unless given), Init'ed with the graph */
static HRESULT insert_tool(rm_graph *g, IDirectMusicTool *t, const DWORD *pch, DWORD npch, LONG index, const GUID *clsid)
{
    HRESULT hr = S_OK;
    EnterCriticalSection(&g->cs);
    for (gnode *n = g->tools; n; n = n->next)
        if (n->tool == t) { LeaveCriticalSection(&g->cs); return DMUS_E_ALREADY_EXISTS; }
    gnode *n = calloc(1, sizeof(*n));
    if (!n) { LeaveCriticalSection(&g->cs); return E_OUTOFMEMORY; }
    n->tool = t;
    IDirectMusicTool_AddRef(t);
    IDirectMusicTool_Init(t, &g->IDirectMusicGraph_iface);
    IUnknown *u = NULL;
    if (SUCCEEDED(IDirectMusicTool_QueryInterface(t, &IID_IDirectMusicTool8, (void **)&u))) { n->is8 = TRUE; IUnknown_Release(u); }
    if (clsid) n->clsid = *clsid;
    else {
        IPersistStream *ps = NULL;
        if (SUCCEEDED(IDirectMusicTool_QueryInterface(t, &IID_IPersistStream, (void **)&ps))) {
            IPersistStream_GetClassID(ps, &n->clsid);
            IPersistStream_Release(ps);
        }
    }
    DWORD d = 0;
    IDirectMusicTool_GetMsgDeliveryType(t, &d);
    if (d != DMUS_PMSGF_TOOL_IMMEDIATE && d != DMUS_PMSGF_TOOL_QUEUE && d != DMUS_PMSGF_TOOL_ATTIME) d = DMUS_PMSGF_TOOL_IMMEDIATE;
    n->delivery = d;
    DWORD nt = 0;
    if (FAILED(IDirectMusicTool_GetMediaTypeArraySize(t, &nt))) nt = 0;
    n->ntypes = nt;
    if (nt) {
        DWORD *a = malloc(nt * sizeof(DWORD));
        if (!a) goto oom;
        if (IDirectMusicTool_GetMediaTypes(t, &a, nt) == E_NOTIMPL) { free(a); n->ntypes = 0; }
        else n->types = a;
    }
    if (pch) {
        if (!(n->pch = malloc(npch ? npch * sizeof(DWORD) : 1))) goto oom;
        memcpy(n->pch, pch, npch * sizeof(DWORD));
        n->npch = npch;
    }
    if (index < 0) index += list_count(g->tools);
    gnode *at = g->tools;               /* 77302598: a negative index finds nothing */
    for (LONG k = index; at && k; k--) at = at->next;
    if (at) {
        gnode **pp = &g->tools;
        while (*pp != at) pp = &(*pp)->next;
        n->next = at; *pp = n;
    } else {
        gnode **pp = &g->tools;
        while (*pp) pp = &(*pp)->next;
        *pp = n;
    }
    LeaveCriticalSection(&g->cs);
    return hr;
oom:
    node_free(n);
    LeaveCriticalSection(&g->cs);
    return E_OUTOFMEMORY;
}

/* 77302a58 */
static HRESULT WINAPI g_insert(IDirectMusicGraph *i, IDirectMusicTool *t, DWORD *pch, DWORD npch, LONG index)
{
    if (IsBadReadPtr(t, 4) || IsBadReadPtr(*(void **)t, 4) || IsBadCodePtr(**(FARPROC **)t)) return E_POINTER;
    if (pch && IsBadReadPtr(pch, npch * sizeof(DWORD))) return E_POINTER;
    return insert_tool(gr(i), t, pch, npch, index, NULL);
}

/* 77302c17: no type list takes every type */
static BOOL takes_type(const gnode *n, DWORD type)
{
    if (!n->ntypes) return TRUE;
    for (DWORD k = 0; k < n->ntypes; k++) if (n->types[k] == type) return TRUE;
    return FALSE;
}

/* 77302c55 */
static HRESULT WINAPI g_stamp(IDirectMusicGraph *i, DMUS_PMSG *m)
{
    if (IsBadWritePtr(m, sizeof(DMUS_PMSG))) return E_POINTER;
    rm_graph *g = gr(i);
    HRESULT hr = S_OK;
    EnterCriticalSection(&g->cs);
    gnode *n = g->tools;
    if (m->pTool) {                     /* the tool after the message's (none if it is not ours) */
        gnode *c = n;
        while (c && c->tool != m->pTool) c = c->next;
        n = c ? c->next : NULL;
    }
    for (; n; n = n->next) {
        if (!takes_type(n, m->dwType)) continue;
        if (!n->pch || m->dwPChannel >= 0xfffffffc) break;
        DWORD k;
        for (k = 0; k < n->npch; k++) if (n->pch[k] == m->dwPChannel) break;
        if (k < n->npch) break;
    }
    if (m->pTool) { IDirectMusicTool_Release(m->pTool); m->pTool = NULL; }
    if (!n) hr = DMUS_S_LAST_TOOL;
    else {
        if (!m->pGraph) { m->pGraph = i; IDirectMusicGraph_AddRef(i); }
        if (n->tool) { m->pTool = n->tool; IDirectMusicTool_AddRef(n->tool); }
        m->dwFlags = (m->dwFlags & ~(DMUS_PMSGF_TOOL_IMMEDIATE | DMUS_PMSGF_TOOL_QUEUE | DMUS_PMSGF_TOOL_ATTIME)) | n->delivery;
    }
    LeaveCriticalSection(&g->cs);
    return hr;
}

/* 77302b98 */
static HRESULT WINAPI g_gettool(IDirectMusicGraph *i, DWORD index, IDirectMusicTool **out)
{
    if (IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    rm_graph *g = gr(i);
    if (!g->tools) return DMUS_E_NOT_FOUND;
    HRESULT hr = S_OK;
    EnterCriticalSection(&g->cs);
    gnode *n = g->tools;
    for (; n && index; index--) n = n->next;
    if (!n || !n->tool) hr = DMUS_E_NOT_FOUND;
    else { *out = n->tool; IDirectMusicTool_AddRef(n->tool); }
    LeaveCriticalSection(&g->cs);
    return hr;
}

/* 77303408: the tool stays referenced (messages may still name it) until the graph goes */
static HRESULT WINAPI g_remove(IDirectMusicGraph *i, IDirectMusicTool *t)
{
    if (IsBadReadPtr(t, 4) || IsBadReadPtr(*(void **)t, 4) || IsBadCodePtr(**(FARPROC **)t)) return E_POINTER;
    rm_graph *g = gr(i);
    HRESULT hr = S_OK;
    EnterCriticalSection(&g->cs);
    gnode **pp = &g->tools;
    while (*pp && (*pp)->tool != t) pp = &(*pp)->next;
    if (!*pp) hr = DMUS_E_NOT_FOUND;
    else {
        gremoved *r = g->removed;
        while (r && r->tool != t) r = r->next;
        if (!r) {
            if (!(r = malloc(sizeof(*r)))) { LeaveCriticalSection(&g->cs); return E_OUTOFMEMORY; }
            r->tool = t; IDirectMusicTool_AddRef(t);
            r->next = g->removed; g->removed = r;
        }
        gnode *n = *pp;
        *pp = n->next;
        node_free(n);
    }
    LeaveCriticalSection(&g->cs);
    return hr;
}

static IDirectMusicGraphVtbl g_vtbl = { g_qi, g_addref, g_release, g_stamp, g_insert, g_gettool, g_remove };

/* 77302ace */
HRESULT rm_graph_find_tool(IDirectMusicGraph *gi, DWORD pch, REFGUID cls, DWORD index, REFIID iid, void **out)
{
    if (!out || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    if (!rm_graph_ours(gi)) return DMUS_E_NOT_FOUND;
    rm_graph *g = gr(gi);
    if (!g->tools) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    EnterCriticalSection(&g->cs);
    gnode *n;
    for (n = g->tools; n; n = n->next) {
        if (!IsEqualGUID(cls, &n->clsid) && !IsEqualGUID(cls, &RM_GUID_All_Objects)) continue;
        if (n->pch && pch < 0xfffffffb) {
            DWORD k;
            for (k = 0; k < n->npch; k++) if (n->pch[k] == pch) break;
            if (k == n->npch) continue;
        }
        if (!index) break;
        index--;
    }
    if (n && n->tool) hr = IDirectMusicTool_QueryInterface(n->tool, iid, out);
    LeaveCriticalSection(&g->cs);
    return hr;
}

/* ------------------------------------------------------------------ Clone (the private interface, 77303845) */
static HRESULT graph_clone(rm_graph *s, IDirectMusicGraph **out)
{
    if (IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    HRESULT hr = E_OUTOFMEMORY;
    EnterCriticalSection(&s->cs);
    rm_graph *g = graph_new();
    if (g) {
        g->valid = s->valid; g->guid = s->guid; g->date = s->date; g->version = s->version;
        wcopy(g->category, DMUS_MAX_CATEGORY, s->category);
        wcopy(g->file, DMUS_MAX_FILENAME, s->file);
        wcopy(g->name, DMUS_MAX_NAME, s->name);
        gnode **tail = &g->tools;
        for (gnode *o = s->tools; o; o = o->next) {
            gnode *n = calloc(1, sizeof(*n));
            if (!n) { g_release(&g->IDirectMusicGraph_iface); g = NULL; break; }
            *tail = n; tail = &n->next;
            n->ntypes = o->ntypes; n->npch = o->npch; n->delivery = o->delivery; n->is8 = o->is8; n->clsid = o->clsid;
            if (o->ntypes) { if ((n->types = malloc(o->ntypes * sizeof(DWORD)))) memcpy(n->types, o->types, o->ntypes * sizeof(DWORD)); else n->ntypes = 0; }
            if (o->npch) { if ((n->pch = malloc(o->npch * sizeof(DWORD)))) memcpy(n->pch, o->pch, o->npch * sizeof(DWORD)); else n->npch = 0; }
            if (o->tool) {
                if (n->is8) IDirectMusicTool8_Clone((IDirectMusicTool8 *)o->tool, &n->tool);
                else { n->tool = o->tool; IDirectMusicTool_AddRef(o->tool); }
            }
        }
    }
    *out = g ? &g->IDirectMusicGraph_iface : NULL;
    if (g) hr = S_OK;
    LeaveCriticalSection(&s->cs);
    return hr;
}
HRESULT rm_graph_clone(IDirectMusicGraph *g, IDirectMusicGraph **out)
{
    if (!rm_graph_ours(g)) { *out = NULL; return E_NOINTERFACE; }
    return graph_clone(gr(g), out);
}
static HRESULT WINAPI gc_qi(IUnknown *i, REFIID r, void **o) { return g_qi(&grc(i)->IDirectMusicGraph_iface, r, o); }
static ULONG WINAPI gc_addref(IUnknown *i) { return g_addref(&grc(i)->IDirectMusicGraph_iface); }
static ULONG WINAPI gc_release(IUnknown *i) { return g_release(&grc(i)->IDirectMusicGraph_iface); }
static HRESULT WINAPI gc_clone(IUnknown *i, IDirectMusicGraph **out) { return graph_clone(grc(i), out); }
static gc_vtbl_t gc_vtbl = { gc_qi, gc_addref, gc_release, gc_clone };

/* ------------------------------------------------------------------ RIFF reading on a stream */
typedef struct { DWORD id, size, type; ULONGLONG data, end; } rck;
static ULONGLONG s_tell(IStream *s)
{
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER p = { { 0, 0 } };
    IStream_Seek(s, z, STREAM_SEEK_CUR, &p);
    return p.QuadPart;
}
static void s_seek(IStream *s, ULONGLONG pos)
{
    LARGE_INTEGER z;
    z.QuadPart = (LONGLONG)pos;
    IStream_Seek(s, z, STREAM_SEEK_SET, NULL);
}
static BOOL s_read(IStream *s, void *b, ULONG n)
{
    ULONG got = 0;
    return SUCCEEDED(IStream_Read(s, b, n, &got)) && got == n;
}
/* the next chunk at the stream position, inside [.., limit) */
static BOOL rck_next(IStream *s, ULONGLONG limit, rck *c)
{
    ULONGLONG at = s_tell(s);
    DWORD h[2];
    if (at + 8 > limit || !s_read(s, h, 8)) return FALSE;
    c->id = h[0]; c->size = h[1]; c->type = 0;
    c->data = at + 8;
    c->end = c->data + c->size + (c->size & 1);
    if (c->id == FOURCC_RIFF || c->id == FOURCC_LIST) {
        if (c->size < 4 || !s_read(s, &c->type, 4)) return FALSE;
    }
    return TRUE;
}
static HRESULT rck_read(IStream *s, const rck *c, void *b, DWORD n)
{
    s_seek(s, c->data);
    if (n > c->size) { memset(b, 0, n); n = c->size; }
    return s_read(s, b, n) ? S_OK : DMUS_E_CANNOTREAD;
}

/* 77302d88: create a tool of the header's class, let it load from the stream, and insert it */
static HRESULT load_tool(rm_graph *g, const DMUS_IO_TOOL_HEADER *h, IStream *s, const DWORD *pch)
{
    IDirectMusicTool *t = NULL;
    HRESULT hr = CoCreateInstance(&h->guidClassID, NULL, CLSCTX_INPROC, &IID_IDirectMusicTool, (void **)&t);
    if (SUCCEEDED(hr)) {
        IPersistStream *ps = NULL;
        hr = IDirectMusicTool_QueryInterface(t, &IID_IPersistStream, (void **)&ps);
        if (SUCCEEDED(hr)) {
            hr = IPersistStream_Load(ps, s);
            if (SUCCEEDED(hr)) hr = insert_tool(g, t, pch, h->cPChannels, h->lIndex, &h->guidClassID);
            IPersistStream_Release(ps);
        }
        IDirectMusicTool_Release(t);
    }
    return hr;
}

/* 7730309b: a 'DMTL' form: the 'tolh' header first, then the tool's own chunk */
static HRESULT load_tool_form(rm_graph *g, IStream *s, const rck *form)
{
    rck c;
    s_seek(s, form->data + 4);
    if (!rck_next(s, form->end, &c)) return DMUS_E_DESCEND_CHUNK_FAIL;
    if (c.id != DMUS_FOURCC_TOOL_CHUNK) return DMUS_E_TOOL_HDR_NOT_FIRST_CK;
    DMUS_IO_TOOL_HEADER h;
    HRESULT hr = rck_read(s, &c, &h, sizeof(h));
    if (!h.ckid && !h.fccType) return DMUS_E_INVALID_TOOL_HDR;
    DWORD *pch = NULL;
    if (h.cPChannels) {
        if (!(pch = malloc(h.cPChannels * sizeof(DWORD)))) return E_OUTOFMEMORY;
        pch[0] = h.dwPChannels[0];
        if (h.cPChannels > 1 && !s_read(s, pch + 1, (h.cPChannels - 1) * sizeof(DWORD))) { free(pch); return DMUS_E_CANNOTREAD; }
    }
    s_seek(s, c.end);
    while (SUCCEEDED(hr) && rck_next(s, form->end, &c)) {
        if (((c.id == FOURCC_RIFF || c.id == FOURCC_LIST) && c.type == h.fccType) || c.id == h.ckid) {
            s_seek(s, c.data - 8);
            hr = load_tool(g, &h, s, pch);
        }
        s_seek(s, c.end);
    }
    free(pch);
    return hr;
}

/* 77303502: the 'DMTG' form's chunks, up to the first that fails (7731e107 stops on a failed result) */
static HRESULT load_graph(rm_graph *g, IStream *s, const rck *form)
{
    HRESULT hr = S_OK;
    rck c, d;
    s_seek(s, form->data + 4);
    while (SUCCEEDED(hr) && rck_next(s, form->end, &c)) {
        switch (c.id) {
        case DMUS_FOURCC_VERSION_CHUNK: hr = rck_read(s, &c, &g->version, sizeof(g->version)); g->valid |= DMUS_OBJ_VERSION; break;
        case DMUS_FOURCC_CATEGORY_CHUNK: hr = rck_read(s, &c, g->category, sizeof(g->category)); g->category[DMUS_MAX_CATEGORY - 1] = 0; g->valid |= DMUS_OBJ_CATEGORY; break;
        case DMUS_FOURCC_DATE_CHUNK: hr = rck_read(s, &c, &g->date, sizeof(g->date)); g->valid |= DMUS_OBJ_DATE; break;
        case DMUS_FOURCC_GUID_CHUNK: hr = rck_read(s, &c, &g->guid, sizeof(g->guid)); g->valid |= DMUS_OBJ_OBJECT; break;
        case FOURCC_LIST:
            if (c.type == DMUS_FOURCC_UNFO_LIST) {
                while (SUCCEEDED(hr) && rck_next(s, c.end, &d)) {
                    if (d.id == DMUS_FOURCC_UNAM_CHUNK) {
                        hr = rck_read(s, &d, g->name, sizeof(g->name));
                        g->name[DMUS_MAX_NAME - 1] = 0; g->valid |= DMUS_OBJ_NAME;
                    }
                    s_seek(s, d.end);
                }
            } else if (c.type == DMUS_FOURCC_TOOL_LIST) {
                while (SUCCEEDED(hr) && rck_next(s, c.end, &d)) {
                    if (d.id == FOURCC_RIFF && d.type == DMUS_FOURCC_TOOL_FORM) hr = load_tool_form(g, s, &d);
                    s_seek(s, d.end);
                }
            }
            break;
        }
        s_seek(s, c.end);
    }
    return hr;
}

HRESULT rm_graph_load(IDirectMusicGraph *gi, IStream *s)
{
    if (!rm_graph_ours(gi)) return E_INVALIDARG;
    rm_graph *g = gr(gi);
    rck c;
    if (!rck_next(s, ~0ULL, &c) || c.id != FOURCC_RIFF || c.type != DMUS_FOURCC_TOOLGRAPH_FORM) return DMUS_E_DESCEND_CHUNK_FAIL;
    clear_tools(g);
    HRESULT hr = load_graph(g, s, &c);
    s_seek(s, c.end);
    return hr;
}

/* ------------------------------------------------------------------ IPersistStream */
static HRESULT WINAPI gp_qi(IPersistStream *i, REFIID r, void **o) { return g_qi(&grp(i)->IDirectMusicGraph_iface, r, o); }
static ULONG WINAPI gp_addref(IPersistStream *i) { return g_addref(&grp(i)->IDirectMusicGraph_iface); }
static ULONG WINAPI gp_release(IPersistStream *i) { return g_release(&grp(i)->IDirectMusicGraph_iface); }
static HRESULT WINAPI gp_getclass(IPersistStream *i, CLSID *c)     /* 77302d54 */
{
    if (IsBadWritePtr(c, sizeof(*c))) return E_POINTER;
    *c = CLSID_DirectMusicGraph;
    return S_OK;
}
static HRESULT WINAPI gp_isdirty(IPersistStream *i) { return S_FALSE; }
static HRESULT WINAPI gp_load(IPersistStream *i, IStream *s)        /* 77303a59 */
{
    if (IsBadReadPtr(s, 4) || IsBadReadPtr(*(void **)s, 4) || IsBadCodePtr(**(FARPROC **)s)) return E_POINTER;
    return rm_graph_load(&grp(i)->IDirectMusicGraph_iface, s);
}
static HRESULT WINAPI gp_save(IPersistStream *i, IStream *s, BOOL clear) { return E_NOTIMPL; }
static HRESULT WINAPI gp_sizemax(IPersistStream *i, ULARGE_INTEGER *n) { return E_NOTIMPL; }
static IPersistStreamVtbl gp_vtbl = { gp_qi, gp_addref, gp_release, gp_getclass, gp_isdirty, gp_load, gp_save, gp_sizemax };

/* ------------------------------------------------------------------ IDirectMusicObject */
static HRESULT WINAPI go_qi(IDirectMusicObject *i, REFIID r, void **o) { return g_qi(&gro(i)->IDirectMusicGraph_iface, r, o); }
static ULONG WINAPI go_addref(IDirectMusicObject *i) { return g_addref(&gro(i)->IDirectMusicGraph_iface); }
static ULONG WINAPI go_release(IDirectMusicObject *i) { return g_release(&gro(i)->IDirectMusicGraph_iface); }
static BOOL desc_ok(DMUS_OBJECTDESC *d)
{
    return !IsBadReadPtr(d, 4) && d->dwSize >= sizeof(DMUS_OBJECTDESC) && !IsBadWritePtr(d, d->dwSize);
}
/* 77303212 */
static HRESULT WINAPI go_getdesc(IDirectMusicObject *i, DMUS_OBJECTDESC *d)
{
    if (!desc_ok(d)) return E_INVALIDARG;
    rm_graph *g = gro(i);
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->guidClass = CLSID_DirectMusicGraph;
    d->guidObject = g->guid; d->ftDate = g->date; d->vVersion = g->version;
    wcopy(d->wszName, DMUS_MAX_NAME, g->name);
    wcopy(d->wszCategory, DMUS_MAX_CATEGORY, g->category);
    wcopy(d->wszFileName, DMUS_MAX_FILENAME, g->file);
    d->dwValidData = g->valid | DMUS_OBJ_CLASS;
    return S_OK;
}
/* 773032e7: the fields it keeps; S_FALSE (and the valid flags cut down) if some were not taken */
static HRESULT WINAPI go_setdesc(IDirectMusicObject *i, DMUS_OBJECTDESC *d)
{
    if (!desc_ok(d)) return E_INVALIDARG;
    rm_graph *g = gro(i);
    DWORD v = d->dwValidData, took = 0;
    if (v & DMUS_OBJ_OBJECT) { g->guid = d->guidObject; took |= DMUS_OBJ_OBJECT; }
    if (v & DMUS_OBJ_NAME) { wcopy(g->name, DMUS_MAX_NAME, d->wszName); took |= DMUS_OBJ_NAME; }
    if (v & DMUS_OBJ_CATEGORY) { wcopy(g->category, DMUS_MAX_CATEGORY, d->wszCategory); took |= DMUS_OBJ_CATEGORY; }
    if (v & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH)) { wcopy(g->file, DMUS_MAX_FILENAME, d->wszFileName); took |= v & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH); }
    if (v & DMUS_OBJ_VERSION) { g->version = d->vVersion; took |= DMUS_OBJ_VERSION; }
    if (v & DMUS_OBJ_DATE) { g->date = d->ftDate; took |= DMUS_OBJ_DATE; }
    g->valid |= took;
    if (v & ~took) { d->dwValidData = took; return S_FALSE; }
    return S_OK;
}
/* 77302e43 */
static HRESULT WINAPI go_parsedesc(IDirectMusicObject *i, IStream *s, DMUS_OBJECTDESC *d)
{
    if (IsBadReadPtr(s, 4) || IsBadReadPtr(*(void **)s, 4) || IsBadCodePtr(**(FARPROC **)s)) return E_POINTER;
    if (IsBadReadPtr(d, 4) || d->dwSize < sizeof(DMUS_OBJECTDESC) || IsBadWritePtr(d, d->dwSize)) return E_INVALIDARG;
    rck c, e;
    if (!rck_next(s, ~0ULL, &c) || c.id != FOURCC_RIFF || c.type != DMUS_FOURCC_TOOLGRAPH_FORM) return DMUS_E_CHUNKNOTFOUND;
    d->guidClass = CLSID_DirectMusicGraph;
    DWORD v = DMUS_OBJ_CLASS;
    HRESULT hr = S_OK;
    while (rck_next(s, c.end, &e)) {
        switch (e.id) {
        case DMUS_FOURCC_VERSION_CHUNK: hr = rck_read(s, &e, &d->vVersion, sizeof(d->vVersion)); v |= DMUS_OBJ_VERSION; break;
        case DMUS_FOURCC_CATEGORY_CHUNK: hr = rck_read(s, &e, d->wszCategory, sizeof(d->wszCategory)); d->wszCategory[DMUS_MAX_CATEGORY - 1] = 0; v |= DMUS_OBJ_CATEGORY; break;
        case DMUS_FOURCC_DATE_CHUNK: hr = rck_read(s, &e, &d->ftDate, sizeof(d->ftDate)); v |= DMUS_OBJ_DATE; break;
        case DMUS_FOURCC_GUID_CHUNK: hr = rck_read(s, &e, &d->guidObject, sizeof(d->guidObject)); v |= DMUS_OBJ_OBJECT; break;
        case FOURCC_LIST:
            if (e.type == DMUS_FOURCC_UNFO_LIST) {
                rck f;
                while (rck_next(s, e.end, &f)) {
                    if (f.id == DMUS_FOURCC_UNAM_CHUNK) {
                        hr = rck_read(s, &f, d->wszName, sizeof(d->wszName));
                        d->wszName[DMUS_MAX_NAME - 1] = 0; v |= DMUS_OBJ_NAME;
                    }
                    s_seek(s, f.end);
                }
            }
            break;
        }
        s_seek(s, e.end);
    }
    if (SUCCEEDED(hr)) d->dwValidData = v;
    return hr;
}
static IDirectMusicObjectVtbl go_vtbl = { go_qi, go_addref, go_release, go_getdesc, go_setdesc, go_parsedesc };

HRESULT rm_graph_create(REFIID riid, void **out)
{
    rm_graph *g = graph_new();
    if (!g) { *out = NULL; return E_OUTOFMEMORY; }
    HRESULT hr = g_qi(&g->IDirectMusicGraph_iface, riid, out);
    g_release(&g->IDirectMusicGraph_iface);
    return hr;
}
