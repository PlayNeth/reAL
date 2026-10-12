/* tooltest - DirectMusic tool graphs and application tools, printed for comparison with Microsoft's DirectMusic:
 * the graph object (InsertTool, GetTool, RemoveTool, StampPMsg, descriptor), the performance's, segment's, segment
 * state's and audiopath's graphs, and the messages an application tool receives while a segment plays.
 * tooltest <dir with segments> */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <dmplugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dsound.h>

static IDirectMusicPerformance8 *perf;
static IDirectMusicLoader8 *loader;
static IDirectMusicGraph *perfgraph;        /* the performance's own graph interface */
static IDirectMusicTool *perftool;          /* the performance's own (output) tool interface */

static IDirectMusicSegment8 *load(const char *name)
{
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
    IDirectMusicSegment8 *s = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&s);
    if (FAILED(hr)) { printf("load %s %08lx\n", name, hr); return NULL; }
    IDirectMusicSegment8_Download(s, (IUnknown *)perf);
    return s;
}

/* ------------------------------------------------------------------ a logging tool */
enum { ACT_STAMP, ACT_FREE, ACT_KEEP_SEND, ACT_STAMP_NOTES_ONLY };
typedef struct tool {
    IDirectMusicTool8 IDirectMusicTool8_iface;
    LONG ref;
    char name[16];
    BOOL is8, typesfail;
    DWORD delivery;
    DWORD ntypes, types[8];
    DWORD act;
    LONG inits, clones, flushes, processed;
    IDirectMusicGraph *initgraph;
    struct tool *parent;
} tool;
#define MAXTOOLS 32
static tool *tools[MAXTOOLS];
static int ntools;
static CRITICAL_SECTION logcs;

typedef struct { char tool[16]; MUSIC_TIME mt; DWORD type, pch, flags; char graph[16]; HRESULT shr; char tafter[16], gafter[16];
                 DWORD extra; } logent;
static logent logv[4096];
static int nlog;

static tool *tl(IDirectMusicTool8 *i) { return (tool *)i; }
static HRESULT WINAPI t_qi(IDirectMusicTool8 *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicTool) || (tl(i)->is8 && IsEqualIID(r, &IID_IDirectMusicTool8))) {
        *o = i; InterlockedIncrement(&tl(i)->ref); return S_OK;
    }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI t_addref(IDirectMusicTool8 *i) { return InterlockedIncrement(&tl(i)->ref); }
static ULONG WINAPI t_release(IDirectMusicTool8 *i) { return InterlockedDecrement(&tl(i)->ref); }    /* never freed */
static HRESULT WINAPI t_init(IDirectMusicTool8 *i, IDirectMusicGraph *g) { tl(i)->inits++; tl(i)->initgraph = g; return S_OK; }
static HRESULT WINAPI t_deliv(IDirectMusicTool8 *i, DWORD *d) { *d = tl(i)->delivery; return S_OK; }
static HRESULT WINAPI t_nt(IDirectMusicTool8 *i, DWORD *n) { if (!tl(i)->ntypes) return E_NOTIMPL; *n = tl(i)->ntypes; return S_OK; }
static HRESULT WINAPI t_types(IDirectMusicTool8 *i, DWORD **a, DWORD n)
{
    if (tl(i)->typesfail) return E_NOTIMPL;
    for (DWORD k = 0; k < n && k < tl(i)->ntypes; k++) (*a)[k] = tl(i)->types[k];
    return S_OK;
}
static const char *gname(IDirectMusicGraph *g);
static const char *tname(IDirectMusicTool *t);
static HRESULT WINAPI t_process(IDirectMusicTool8 *i, IDirectMusicPerformance *p, DMUS_PMSG *m)
{
    tool *t = tl(i);
    InterlockedIncrement(&t->processed);
    EnterCriticalSection(&logcs);
    logent *e = nlog < 4096 ? &logv[nlog++] : NULL;
    if (e) {
        strcpy(e->tool, t->name); e->mt = m->mtTime; e->type = m->dwType; e->pch = m->dwPChannel; e->flags = m->dwFlags;
        strcpy(e->graph, gname(m->pGraph)); e->extra = 0;
        if (m->dwType == DMUS_PMSGT_NOTE) e->extra = ((DMUS_NOTE_PMSG *)m)->bMidiValue | ((DMUS_NOTE_PMSG *)m)->bFlags << 8;
        else if (m->dwType == DMUS_PMSGT_MIDI) e->extra = ((DMUS_MIDI_PMSG *)m)->bStatus | ((DMUS_MIDI_PMSG *)m)->bByte1 << 8;
        else if (m->dwType == DMUS_PMSGT_PATCH) e->extra = ((DMUS_PATCH_PMSG *)m)->byInstrument;
    }
    LeaveCriticalSection(&logcs);
    HRESULT hr = DMUS_S_REQUEUE;
    if (t->act == ACT_FREE) hr = DMUS_S_FREE;
    else if (t->act == ACT_KEEP_SEND) {     /* send it on ourselves and keep nothing */
        HRESULT s = m->pGraph ? IDirectMusicGraph_StampPMsg(m->pGraph, m) : E_FAIL;
        if (e) { e->shr = s; strcpy(e->tafter, tname(m->pTool)); strcpy(e->gafter, gname(m->pGraph)); }
        HRESULT s2 = IDirectMusicPerformance_SendPMsg(p, m);
        if (e) e->extra ^= (DWORD)s2 << 16;
        return S_OK;
    } else if (!m->pGraph || FAILED(IDirectMusicGraph_StampPMsg(m->pGraph, m))) hr = DMUS_S_FREE;
    if (e) {
        e->shr = hr;
        strcpy(e->tafter, tname(m->pTool)); strcpy(e->gafter, gname(m->pGraph));
    }
    return hr;
}
static HRESULT WINAPI t_flush(IDirectMusicTool8 *i, IDirectMusicPerformance *p, DMUS_PMSG *m, REFERENCE_TIME rt) { InterlockedIncrement(&tl(i)->flushes); return S_OK; }
static tool *new_tool(const char *name, BOOL is8, DWORD delivery, DWORD ntypes, const DWORD *types);
static HRESULT WINAPI t_clone(IDirectMusicTool8 *i, IDirectMusicTool **o)
{
    tool *t = tl(i);
    t->clones++;
    char n[16];
    snprintf(n, sizeof(n), "%s'", t->name);
    tool *c = new_tool(n, TRUE, t->delivery, t->ntypes, t->types);
    c->act = t->act; c->typesfail = t->typesfail; c->parent = t;
    *o = (IDirectMusicTool *)&c->IDirectMusicTool8_iface;
    return S_OK;
}
static IDirectMusicTool8Vtbl t_vtbl = { t_qi, t_addref, t_release, t_init, t_deliv, t_nt, t_types, t_process, t_flush, t_clone };

static tool *new_tool(const char *name, BOOL is8, DWORD delivery, DWORD ntypes, const DWORD *types)
{
    tool *t = calloc(1, sizeof(*t));
    t->IDirectMusicTool8_iface.lpVtbl = &t_vtbl;
    t->ref = 1; strcpy(t->name, name); t->is8 = is8; t->delivery = delivery; t->ntypes = ntypes;
    for (DWORD k = 0; k < ntypes && k < 8; k++) t->types[k] = types[k];
    if (ntools < MAXTOOLS) tools[ntools++] = t;
    return t;
}
#define TI(t) ((IDirectMusicTool *)&(t)->IDirectMusicTool8_iface)

/* names for interface pointers met in messages */
#define MAXGRAPHS 16
static struct { IDirectMusicGraph *g; char name[16]; } graphs[MAXGRAPHS];
static int ngraphs;
static void name_graph(IDirectMusicGraph *g, const char *n) { if (ngraphs < MAXGRAPHS) { graphs[ngraphs].g = g; strcpy(graphs[ngraphs++].name, n); } }
static const char *gname(IDirectMusicGraph *g)
{
    if (!g) return "null";
    if (g == perfgraph) return "perf";
    for (int k = 0; k < ngraphs; k++) if (graphs[k].g == g) return graphs[k].name;
    IUnknown *u = NULL;
    if (SUCCEEDED(IDirectMusicGraph_QueryInterface(g, &IID_IDirectMusicSegmentState, (void **)&u))) { IUnknown_Release(u); return "state"; }
    if (SUCCEEDED(IDirectMusicGraph_QueryInterface(g, &IID_IDirectMusicAudioPath, (void **)&u))) { IUnknown_Release(u); return "apath"; }
    if (SUCCEEDED(IDirectMusicGraph_QueryInterface(g, &IID_IDirectMusicPerformance, (void **)&u))) { IUnknown_Release(u); return "perf?"; }
    return "other";
}
static const char *tname(IDirectMusicTool *t)
{
    if (!t) return "null";
    if (t == perftool) return "output";
    for (int k = 0; k < ntools; k++) if (TI(tools[k]) == t) return tools[k]->name;
    return "other";
}
static LONG refs(IUnknown *u) { IUnknown_AddRef(u); return (LONG)IUnknown_Release(u); }

static int cmplog(const void *a, const void *b)
{
    const logent *x = a, *y = b;
    int c = strcmp(x->tool, y->tool);
    if (c) return c;
    if (x->mt != y->mt) return x->mt < y->mt ? -1 : 1;
    if (x->type != y->type) return x->type < y->type ? -1 : 1;
    if (x->pch != y->pch) return x->pch < y->pch ? -1 : 1;
    if (x->extra != y->extra) return x->extra < y->extra ? -1 : 1;
    return memcmp(x, y, sizeof(*x));
}
/* print and clear the log: times relative to t0, only messages before tmax */
static void dumplog(MUSIC_TIME t0, MUSIC_TIME tmax)
{
    EnterCriticalSection(&logcs);
    qsort(logv, nlog, sizeof(logv[0]), cmplog);
    int shown = 0;
    for (int k = 0; k < nlog; k++) {
        logent *e = &logv[k];
        if (e->mt - t0 >= tmax || e->mt - t0 < -10000) continue;
        printf("    %-5s mt %5ld type %2lu pch %3ld flags %04lx graph %-6s -> %08lx tool %-6s graph %-6s x %lx\n", e->tool,
               e->mt - t0, e->type, (long)e->pch, e->flags, e->graph, e->shr, e->tafter, e->gafter, e->extra);
        shown++;
    }
    printf("    (%d messages)\n", shown);
    nlog = 0;
    LeaveCriticalSection(&logcs);
}

/* ------------------------------------------------------------------ the graph object */
static void stampseq(IDirectMusicGraph *g, const char *what, DWORD type, DWORD pch, IDirectMusicTool *pre, IDirectMusicGraph *preg)
{
    DMUS_NOTE_PMSG m;
    memset(&m, 0, sizeof(m));
    m.dwSize = sizeof(m); m.dwType = type; m.dwPChannel = pch; m.dwFlags = DMUS_PMSGF_MUSICTIME | DMUS_PMSGF_TOOL_ATTIME;
    m.pTool = pre; if (pre) IDirectMusicTool_AddRef(pre);
    m.pGraph = preg; if (preg) IDirectMusicGraph_AddRef(preg);
    printf("  stamp %s:", what);
    for (int k = 0; k < 8; k++) {
        HRESULT hr = IDirectMusicGraph_StampPMsg(g, (DMUS_PMSG *)&m);
        printf(" [%08lx %s %s %02lx]", hr, tname(m.pTool), gname(m.pGraph), m.dwFlags);
        if (hr != S_OK) break;
    }
    printf("\n");
    if (m.pTool) IDirectMusicTool_Release(m.pTool);
    if (m.pGraph) IDirectMusicGraph_Release(m.pGraph);
}

static void graphobj(void)
{
    printf("graph object\n");
    IDirectMusicGraph *g = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&g);
    printf("  create %08lx\n", hr);
    if (!g) return;
    name_graph(g, "G");
    void *o;
    const struct { const IID *iid; const char *n; } q[] = {
        { &IID_IUnknown, "unknown" }, { &IID_IDirectMusicGraph, "graph" }, { &IID_IPersistStream, "persiststream" },
        { &IID_IPersist, "persist" }, { &IID_IDirectMusicObject, "object" }, { &IID_IDirectMusicTool, "tool" },
        { &IID_IDirectMusicPerformance, "perf" },
    };
    for (unsigned k = 0; k < sizeof(q) / sizeof(q[0]); k++) {
        o = (void *)1;
        hr = IDirectMusicGraph_QueryInterface(g, q[k].iid, &o);
        printf("  qi %s %08lx %s\n", q[k].n, hr, o ? "obj" : "null");
        if (SUCCEEDED(hr)) IUnknown_Release((IUnknown *)o);
    }
    static const DWORD tb[] = { DMUS_PMSGT_NOTE, DMUS_PMSGT_MIDI };
    static const DWORD te[] = { DMUS_PMSGT_CURVE, DMUS_PMSGT_NOTE };
    tool *A = new_tool("A", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    tool *B = new_tool("B", FALSE, DMUS_PMSGF_TOOL_QUEUE, 2, tb);
    tool *C = new_tool("C", TRUE, DMUS_PMSGF_TOOL_ATTIME, 0, NULL);
    tool *D = new_tool("D", FALSE, 0x40, 0, NULL);
    tool *E = new_tool("E", FALSE, DMUS_PMSGF_TOOL_QUEUE, 2, te); E->typesfail = TRUE;
    tool *F = new_tool("F", FALSE, DMUS_PMSGF_TOOL_ATTIME, 2, te);
    DWORD pc[] = { 2, 5 };
    printf("  insert A %08lx\n", IDirectMusicGraph_InsertTool(g, TI(A), NULL, 0, 0));
    printf("  insert A again %08lx\n", IDirectMusicGraph_InsertTool(g, TI(A), NULL, 0, 3));
    printf("  insert B -1 %08lx\n", IDirectMusicGraph_InsertTool(g, TI(B), NULL, 0, -1));
    printf("  insert C pch 5 %08lx\n", IDirectMusicGraph_InsertTool(g, TI(C), pc, 2, 5));
    printf("  insert D 1 %08lx\n", IDirectMusicGraph_InsertTool(g, TI(D), NULL, 0, 1));
    printf("  insert E -100 %08lx\n", IDirectMusicGraph_InsertTool(g, TI(E), NULL, 3, -100));
    printf("  insert F -2 %08lx\n", IDirectMusicGraph_InsertTool(g, TI(F), pc, 1, -2));
    printf("  insert NULL %08lx\n", IDirectMusicGraph_InsertTool(g, NULL, NULL, 0, 0));
    for (int k = 0; k < ntools; k++)
        printf("  tool %s inits %ld graph %s refs %ld\n", tools[k]->name, tools[k]->inits, gname(tools[k]->initgraph), refs((IUnknown *)TI(tools[k])));
    for (DWORD k = 0; k < 8; k++) {
        IDirectMusicTool *t = (IDirectMusicTool *)1;
        hr = IDirectMusicGraph_GetTool(g, k, &t);
        printf("  get %lu %08lx %s\n", k, hr, t == (void *)1 ? "untouched" : tname(t));
        if (SUCCEEDED(hr)) IDirectMusicTool_Release(t);
    }
    printf("  get NULL %08lx\n", IDirectMusicGraph_GetTool(g, 0, NULL));
    stampseq(g, "note 2", DMUS_PMSGT_NOTE, 2, NULL, NULL);
    stampseq(g, "note 1", DMUS_PMSGT_NOTE, 1, NULL, NULL);
    stampseq(g, "midi 5", DMUS_PMSGT_MIDI, 5, NULL, NULL);
    stampseq(g, "curve 5", DMUS_PMSGT_CURVE, 5, NULL, NULL);
    stampseq(g, "curve 2", DMUS_PMSGT_CURVE, 2, NULL, NULL);
    stampseq(g, "patch 7", DMUS_PMSGT_PATCH, 7, NULL, NULL);
    stampseq(g, "note bcast", DMUS_PMSGT_NOTE, 0xffffffff, NULL, NULL);
    stampseq(g, "note fffffffc", DMUS_PMSGT_NOTE, 0xfffffffc, NULL, NULL);
    stampseq(g, "note fffffffb", DMUS_PMSGT_NOTE, 0xfffffffb, NULL, NULL);
    stampseq(g, "from D", DMUS_PMSGT_NOTE, 2, TI(D), NULL);
    tool *X = new_tool("X", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    stampseq(g, "from X", DMUS_PMSGT_NOTE, 2, TI(X), NULL);
    stampseq(g, "preset graph", DMUS_PMSGT_NOTE, 2, NULL, perfgraph);
    printf("  stamp NULL %08lx\n", IDirectMusicGraph_StampPMsg(g, NULL));
    printf("  remove B %08lx refs %ld\n", IDirectMusicGraph_RemoveTool(g, TI(B)), refs((IUnknown *)TI(B)));
    printf("  remove B again %08lx refs %ld\n", IDirectMusicGraph_RemoveTool(g, TI(B)), refs((IUnknown *)TI(B)));
    printf("  remove X %08lx\n", IDirectMusicGraph_RemoveTool(g, TI(X)));
    printf("  insert B again %08lx refs %ld\n", IDirectMusicGraph_InsertTool(g, TI(B), NULL, 0, 0), refs((IUnknown *)TI(B)));
    printf("  remove B %08lx refs %ld inits %ld\n", IDirectMusicGraph_RemoveTool(g, TI(B)), refs((IUnknown *)TI(B)), B->inits);
    stampseq(g, "from B (removed)", DMUS_PMSGT_NOTE, 2, TI(B), NULL);
    printf("  remove NULL %08lx\n", IDirectMusicGraph_RemoveTool(g, NULL));
    IDirectMusicObject *ob = NULL;
    if (SUCCEEDED(IDirectMusicGraph_QueryInterface(g, &IID_IDirectMusicObject, (void **)&ob))) {
        DMUS_OBJECTDESC d;
        memset(&d, 0x55, sizeof(d)); d.dwSize = sizeof(d);
        hr = IDirectMusicObject_GetDescriptor(ob, &d);
        printf("  descriptor %08lx valid %08lx class %d version %lu.%lu\n", hr, d.dwValidData,
               IsEqualGUID(&d.guidClass, &CLSID_DirectMusicGraph), d.vVersion.dwVersionMS, d.vVersion.dwVersionLS);
        d.dwSize = 8;
        printf("  descriptor small %08lx\n", IDirectMusicObject_GetDescriptor(ob, &d));
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        d.dwValidData = DMUS_OBJ_NAME | DMUS_OBJ_CLASS | DMUS_OBJ_VERSION;
        lstrcpyW(d.wszName, L"toolgraph"); d.vVersion.dwVersionMS = 3; d.guidClass = CLSID_DirectMusicSegment;
        printf("  set descriptor %08lx\n", IDirectMusicObject_SetDescriptor(ob, &d));
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        hr = IDirectMusicObject_GetDescriptor(ob, &d);
        char nm[64]; WideCharToMultiByte(CP_ACP, 0, d.wszName, -1, nm, sizeof(nm), NULL, NULL);
        printf("  descriptor %08lx valid %08lx name '%s' class graph %d version %lu\n", hr, d.dwValidData, nm,
               IsEqualGUID(&d.guidClass, &CLSID_DirectMusicGraph), d.vVersion.dwVersionMS);
        IDirectMusicObject_Release(ob);
    }
    IPersistStream *ps = NULL;
    if (SUCCEEDED(IDirectMusicGraph_QueryInterface(g, &IID_IPersistStream, (void **)&ps))) {
        CLSID c;
        hr = IPersistStream_GetClassID(ps, &c);
        printf("  classid %08lx graph %d\n", hr, IsEqualGUID(&c, &CLSID_DirectMusicGraph));
        printf("  isdirty %08lx\n", IPersistStream_IsDirty(ps));
        IPersistStream_Release(ps);
    }
    LONG before[MAXTOOLS];
    for (int k = 0; k < ntools; k++) before[k] = refs((IUnknown *)TI(tools[k]));
    printf("  release %lu\n", IDirectMusicGraph_Release(g));
    for (int k = 0; k < ntools; k++) printf("  tool %s refs %ld -> %ld\n", tools[k]->name, before[k], refs((IUnknown *)TI(tools[k])));
    ngraphs = 0;
}

/* ------------------------------------------------------------------ the performance's graph */
static void perfgraphs(void)
{
    printf("performance graph\n");
    IDirectMusicGraph *g = (IDirectMusicGraph *)1;
    HRESULT hr = IDirectMusicPerformance8_GetGraph(perf, &g);
    printf("  get before %08lx %s\n", hr, g == (void *)1 ? "untouched" : g ? "obj" : "null");
    printf("  qi graph %s tool %s\n", perfgraph ? "yes" : "no", perftool ? "yes" : "no");
    if (perfgraph) {
        printf("  insert %08lx\n", IDirectMusicGraph_InsertTool(perfgraph, perftool, NULL, 0, 0));
        IDirectMusicTool *t = (IDirectMusicTool *)1;
        printf("  gettool %08lx\n", IDirectMusicGraph_GetTool(perfgraph, 0, &t));
        printf("  remove %08lx\n", IDirectMusicGraph_RemoveTool(perfgraph, perftool));
        IUnknown *u = NULL;
        hr = IDirectMusicGraph_QueryInterface(perfgraph, &IID_IDirectMusicPerformance8, (void **)&u);
        printf("  graph qi perf %08lx same %d\n", hr, u == (IUnknown *)perf);
        if (u) IUnknown_Release(u);
    }
    if (perftool) {
        DWORD d = 77;
        hr = IDirectMusicTool_GetMsgDeliveryType(perftool, &d);
        printf("  output delivery %08lx %lu\n", hr, d);
        d = 77;
        hr = IDirectMusicTool_GetMediaTypeArraySize(perftool, &d);
        printf("  output types %08lx %lu\n", hr, d);
        printf("  output init %08lx\n", IDirectMusicTool_Init(perftool, NULL));
    }
    printf("  set NULL %08lx\n", IDirectMusicPerformance8_SetGraph(perf, NULL));
    g = (IDirectMusicGraph *)1;
    printf("  get %08lx %s\n", IDirectMusicPerformance8_GetGraph(perf, &g), g == (void *)1 ? "untouched" : g ? "obj" : "null");
    if (perfgraph) stampseq(perfgraph, "perf, no graph", DMUS_PMSGT_NOTE, 3, NULL, NULL);

    IDirectMusicGraph *pg = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&pg);
    if (!pg) return;
    name_graph(pg, "PG");
    tool *T = new_tool("T", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    static const DWORD tq[] = { DMUS_PMSGT_NOTE };
    tool *Q = new_tool("Q", FALSE, DMUS_PMSGF_TOOL_QUEUE, 1, tq);
    IDirectMusicGraph_InsertTool(pg, TI(T), NULL, 0, 0);
    IDirectMusicGraph_InsertTool(pg, TI(Q), NULL, 0, 1);
    LONG r0 = refs((IUnknown *)pg);
    printf("  set %08lx refs +%ld\n", IDirectMusicPerformance8_SetGraph(perf, pg), refs((IUnknown *)pg) - r0);
    g = NULL;
    hr = IDirectMusicPerformance8_GetGraph(perf, &g);
    printf("  get %08lx %s\n", hr, gname(g));
    if (g) IDirectMusicGraph_Release(g);
    if (perfgraph) {
        stampseq(perfgraph, "perf note", DMUS_PMSGT_NOTE, 3, NULL, NULL);
        stampseq(perfgraph, "perf midi", DMUS_PMSGT_MIDI, 3, NULL, NULL);
        stampseq(perfgraph, "perf from output", DMUS_PMSGT_NOTE, 3, perftool, NULL);
        stampseq(perfgraph, "perf preset PG", DMUS_PMSGT_NOTE, 3, NULL, pg);
        printf("  perf stamp NULL %08lx\n", IDirectMusicGraph_StampPMsg(perfgraph, NULL));
    }

    IDirectMusicSegment8 *s = load("bells1.sgt");
    if (!s) return;
    IDirectMusicSegmentState *ss = NULL;
    hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)s, NULL, NULL, 0, 0, &ss, NULL, NULL);
    printf("  play %08lx\n", hr);
    Sleep(2500);
    MUSIC_TIME st = 0;
    IDirectMusicSegmentState_GetStartTime(ss, &st);
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(300);
    printf("  messages\n");
    dumplog(st, 3000);
    printf("  T processed>0 %d Q processed>0 %d\n", T->processed > 0, Q->processed > 0);
    printf("  set NULL %08lx refs %ld\n", IDirectMusicPerformance8_SetGraph(perf, NULL), refs((IUnknown *)pg));
    IDirectMusicSegmentState_Release(ss);
    IDirectMusicSegment8_Release(s);
    IDirectMusicGraph_Release(pg);
    ngraphs = 0;
    nlog = 0;
}

/* ------------------------------------------------------------------ a segment's graph */
static void seggraph(void)
{
    printf("segment graph\n");
    IDirectMusicSegment8 *s = load("bells1.sgt");
    if (!s) return;
    IDirectMusicGraph *g = (IDirectMusicGraph *)1;
    HRESULT hr = IDirectMusicSegment8_GetGraph(s, &g);
    printf("  get before %08lx %s\n", hr, g == (void *)1 ? "untouched" : g ? "obj" : "null");
    IDirectMusicGraph *sg = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&sg);
    name_graph(sg, "SG");
    tool *P = new_tool("P", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    tool *R = new_tool("R", TRUE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    DWORD pc[] = { 0, 1, 2, 3 };
    IDirectMusicGraph_InsertTool(sg, TI(P), NULL, 0, 0);
    IDirectMusicGraph_InsertTool(sg, TI(R), pc, 4, 1);
    printf("  set %08lx\n", IDirectMusicSegment8_SetGraph(s, sg));
    g = NULL;
    hr = IDirectMusicSegment8_GetGraph(s, &g);
    printf("  get %08lx %s\n", hr, gname(g));
    if (g) IDirectMusicGraph_Release(g);
    IDirectMusicSegment *cl = NULL;
    IDirectMusicSegment8_Clone(s, 0, 0, &cl);
    if (cl) {
        g = NULL;
        hr = IDirectMusicSegment_GetGraph(cl, &g);
        printf("  clone get %08lx %s\n", hr, gname(g));
        if (g) IDirectMusicGraph_Release(g);
        IDirectMusicSegment_Release(cl);
    }
    LONG pr = refs((IUnknown *)TI(P)), rr = refs((IUnknown *)TI(R));
    IDirectMusicSegmentState *ss = NULL;
    hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)s, NULL, NULL, 0, 0, &ss, NULL, NULL);
    printf("  play %08lx R clones %ld inits %ld\n", hr, R->clones, R->inits);
    IDirectMusicSegmentState8 *s8 = NULL;
    if (ss) IDirectMusicSegmentState_QueryInterface(ss, &IID_IDirectMusicSegmentState8, (void **)&s8);
    if (s8) {
        g = NULL;
        hr = IDirectMusicSegmentState8_GetObjectInPath(s8, 0, DMUS_PATH_SEGMENT_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, (void **)&g);
        printf("  state graph %08lx %s\n", hr, gname(g));
        if (g) {
            for (DWORD k = 0; k < 3; k++) {
                IDirectMusicTool *t = NULL;
                hr = IDirectMusicGraph_GetTool(g, k, &t);
                printf("    tool %lu %08lx %s\n", k, hr, tname(t));
                if (t) IDirectMusicTool_Release(t);
            }
            IDirectMusicGraph_Release(g);
        }
        IUnknown *u = NULL;
        hr = IDirectMusicSegmentState8_QueryInterface(s8, &IID_IDirectMusicGraph, (void **)&u);
        printf("  state qi graph %08lx\n", hr);
        if (u) {
            stampseq((IDirectMusicGraph *)u, "state graph note 1", DMUS_PMSGT_NOTE, 1, NULL, NULL);
            stampseq((IDirectMusicGraph *)u, "state graph note 9", DMUS_PMSGT_NOTE, 9, NULL, NULL);
            printf("  state graph insert %08lx\n", IDirectMusicGraph_InsertTool((IDirectMusicGraph *)u, TI(P), NULL, 0, 0));
            IUnknown_Release(u);
        }
    }
    Sleep(2500);
    MUSIC_TIME st = 0;
    IDirectMusicSegmentState_GetStartTime(ss, &st);
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(300);
    printf("  messages\n");
    dumplog(st, 3000);
    if (s8) IDirectMusicSegmentState8_Release(s8);
    IDirectMusicSegmentState_Release(ss);
    Sleep(100);
    printf("  set NULL %08lx\n", IDirectMusicSegment8_SetGraph(s, NULL));
    g = (IDirectMusicGraph *)1;
    printf("  get %08lx %s\n", IDirectMusicSegment8_GetGraph(s, &g), g == (void *)1 ? "untouched" : g ? "obj" : "null");
    printf("  after: P refs +%ld R refs +%ld\n", refs((IUnknown *)TI(P)) - pr, refs((IUnknown *)TI(R)) - rr);
    IDirectMusicSegment8_Release(s);
    IDirectMusicGraph_Release(sg);
    ngraphs = 0;
}

/* ------------------------------------------------------------------ an audiopath's graph */
static void apgraph(void)
{
    printf("audiopath graph\n");
    IDirectMusicAudioPath *ap = NULL;
    IDirectMusicPerformance8_GetDefaultAudioPath(perf, &ap);
    if (!ap) { printf("  no default path\n"); return; }
    void *o = (void *)1;
    HRESULT hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_AUDIOPATH_TOOL, 0, &GUID_All_Objects, 0, &IID_IDirectMusicTool, &o);
    printf("  tool before %08lx %s\n", hr, o ? "obj" : "null");
    IDirectMusicGraph *g = NULL;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_AUDIOPATH_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, (void **)&g);
    printf("  graph %08lx %s\n", hr, gname(g));
    IDirectMusicGraph *g2 = NULL;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_AUDIOPATH_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, (void **)&g2);
    printf("  graph again %08lx same %d\n", hr, g == g2);
    if (g2) IDirectMusicGraph_Release(g2);
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 1, DMUS_PATH_AUDIOPATH_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, (void **)&g2);
    printf("  graph pch 1 %08lx\n", hr);
    if (!g) { IDirectMusicAudioPath_Release(ap); return; }
    name_graph(g, "APG");
    tool *V = new_tool("V", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    DWORD pc[] = { 1 };
    tool *W = new_tool("W", FALSE, DMUS_PMSGF_TOOL_ATTIME, 0, NULL);
    printf("  insert %08lx\n", IDirectMusicGraph_InsertTool(g, TI(V), NULL, 0, 0));
    printf("  insert W %08lx\n", IDirectMusicGraph_InsertTool(g, TI(W), pc, 1, 1));
    o = NULL;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_AUDIOPATH_TOOL, 0, &GUID_All_Objects, 0, &IID_IDirectMusicTool, &o);
    printf("  tool %08lx %s\n", hr, tname(o));
    if (o) IUnknown_Release((IUnknown *)o);
    o = NULL;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_AUDIOPATH_TOOL, 0, &GUID_All_Objects, 1, &IID_IDirectMusicTool, &o);
    printf("  tool 1 %08lx %s\n", hr, tname(o));
    if (o) IUnknown_Release((IUnknown *)o);
    o = NULL;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 1, DMUS_PATH_AUDIOPATH_TOOL, 0, &GUID_All_Objects, 1, &IID_IDirectMusicTool, &o);
    printf("  tool pch 1 idx 1 %08lx %s\n", hr, tname(o));
    if (o) IUnknown_Release((IUnknown *)o);
    o = NULL;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_AUDIOPATH_TOOL, 0, &GUID_NULL, 0, &IID_IDirectMusicTool, &o);
    printf("  tool null class %08lx %s\n", hr, tname(o));
    if (o) IUnknown_Release((IUnknown *)o);
    IUnknown *u = NULL;
    hr = IDirectMusicAudioPath_QueryInterface(ap, &IID_IDirectMusicGraph, (void **)&u);
    printf("  path qi graph %08lx\n", hr);
    if (u) {
        stampseq((IDirectMusicGraph *)u, "path note 1", DMUS_PMSGT_NOTE, 1, NULL, NULL);
        stampseq((IDirectMusicGraph *)u, "path note 200", DMUS_PMSGT_NOTE, 200, NULL, NULL);
        stampseq((IDirectMusicGraph *)u, "path notif 200", DMUS_PMSGT_NOTIFICATION, 200, NULL, NULL);
        IUnknown_Release(u);
    }
    IDirectMusicSegment8 *s = load("bells1.sgt");
    IDirectMusicSegmentState *ss = NULL;
    if (s) {
        hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)s, NULL, NULL, 0, 0, &ss, NULL, NULL);
        printf("  play %08lx\n", hr);
        Sleep(2500);
        MUSIC_TIME st = 0;
        IDirectMusicSegmentState_GetStartTime(ss, &st);
        IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
        Sleep(300);
        printf("  messages\n");
        dumplog(st, 3000);
        IDirectMusicSegmentState_Release(ss);
        IDirectMusicSegment8_Release(s);
    }
    IDirectMusicGraph_RemoveTool(g, TI(V));
    IDirectMusicGraph_RemoveTool(g, TI(W));
    IDirectMusicGraph_Release(g);
    IDirectMusicAudioPath_Release(ap);
    ngraphs = 0;
}

/* ------------------------------------------------------------------ GetObjectInPath graph and tool stages */
static void stages(void)
{
    printf("stages\n");
    IDirectMusicSegment8 *s = load("bells1.sgt");
    if (!s) return;
    IDirectMusicGraph *pg = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&pg);
    tool *Z = new_tool("Z", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    DWORD pc[] = { 4 };
    IDirectMusicGraph_InsertTool(pg, TI(Z), pc, 1, 0);
    name_graph(pg, "PG2");
    IDirectMusicSegmentState *ss = NULL;
    IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)s, NULL, NULL, 0, 0, &ss, NULL, NULL);
    IDirectMusicSegmentState8 *s8 = NULL;
    IDirectMusicSegmentState_QueryInterface(ss, &IID_IDirectMusicSegmentState8, (void **)&s8);
    struct { DWORD pch, stage, buf; const GUID *g; DWORD n; const GUID *iid; const char *what; } c[] = {
        { 0, DMUS_PATH_SEGMENT_TOOL, 0, &GUID_All_Objects, 0, &IID_IDirectMusicTool, "segment tool (no graph)" },
        { 0, DMUS_PATH_SEGMENT_GRAPH, 0, &GUID_NULL, 1, &IID_IDirectMusicGraph, "segment graph 1" },
        { 1, DMUS_PATH_SEGMENT_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, "segment graph pch 1" },
        { 0, DMUS_PATH_SEGMENT_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, "segment graph" },
        { 0, DMUS_PATH_SEGMENT_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicSegmentState, "segment graph as state" },
        { 0, DMUS_PATH_SEGMENT_TOOL, 0, &GUID_All_Objects, 0, &IID_IDirectMusicTool, "segment tool" },
        { 0, DMUS_PATH_PERFORMANCE_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, "perf graph (none)" },
        { 0, DMUS_PATH_PERFORMANCE_TOOL, 0, &GUID_All_Objects, 0, &IID_IDirectMusicTool, "perf tool (none)" },
        { 0, DMUS_PATH_AUDIOPATH_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, "path graph" },
        { 0, DMUS_PATH_AUDIOPATH_TOOL, 0, &GUID_All_Objects, 0, &IID_IDirectMusicTool, "path tool" },
        { 0, DMUS_PATH_AUDIOPATH_GRAPH, 1, &GUID_NULL, 0, &IID_IDirectMusicGraph, "path graph buf 1" },
    };
    for (int pass = 0; pass < 2; pass++) {
        if (pass) printf("  with a performance graph\n");
        for (unsigned k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
            void *o = (void *)1;
            HRESULT hr = IDirectMusicSegmentState8_GetObjectInPath(s8, c[k].pch, c[k].stage, c[k].buf, c[k].g, c[k].n, c[k].iid, &o);
            const char *what = !o ? "null" : o == (void *)1 ? "untouched" : c[k].iid == &IID_IDirectMusicGraph ? gname(o) :
                               c[k].iid == &IID_IDirectMusicTool ? tname(o) : "obj";
            printf("  %s %08lx %s\n", c[k].what, hr, what);
            if (SUCCEEDED(hr) && o && o != (void *)1) IUnknown_Release((IUnknown *)o);
        }
        if (!pass) {
            /* the performance's graph made on demand by the PERFORMANCE_GRAPH stage? */
            IDirectMusicGraph *g = (IDirectMusicGraph *)1;
            HRESULT hr = IDirectMusicPerformance8_GetGraph(perf, &g);
            printf("  perf get graph %08lx %s\n", hr, g == (void *)1 ? "untouched" : gname(g));
            if (SUCCEEDED(hr)) {
                IDirectMusicTool *t = (IDirectMusicTool *)1;
                printf("  its tool 0 %08lx\n", IDirectMusicGraph_GetTool(g, 0, &t));
                IDirectMusicGraph_Release(g);
            }
            IDirectMusicPerformance8_SetGraph(perf, pg);
            struct { DWORD pch, n; const GUID *g; const char *what; } t[] = {
                { 0, 0, &GUID_All_Objects, "perf tool pch 0" }, { 4, 0, &GUID_All_Objects, "perf tool pch 4" },
                { 0xfffffffb, 0, &GUID_All_Objects, "perf tool pch fffffffb" }, { 0xfffffffa, 0, &GUID_All_Objects, "perf tool pch fffffffa" },
                { 4, 1, &GUID_All_Objects, "perf tool pch 4 idx 1" }, { 4, 0, &GUID_NULL, "perf tool pch 4 null class" },
            };
            for (unsigned k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
                void *o = (void *)1;
                hr = IDirectMusicSegmentState8_GetObjectInPath(s8, t[k].pch, DMUS_PATH_PERFORMANCE_TOOL, 0, t[k].g, t[k].n, &IID_IDirectMusicTool, &o);
                printf("  %s %08lx %s\n", t[k].what, hr, !o ? "null" : o == (void *)1 ? "untouched" : tname(o));
                if (SUCCEEDED(hr) && o) IUnknown_Release((IUnknown *)o);
                IDirectMusicAudioPath *ap = NULL;
                IDirectMusicPerformance8_GetDefaultAudioPath(perf, &ap);
                o = (void *)1;
                hr = IDirectMusicAudioPath_GetObjectInPath(ap, t[k].pch, DMUS_PATH_PERFORMANCE_TOOL, 0, t[k].g, t[k].n, &IID_IDirectMusicTool, &o);
                printf("  path %s %08lx %s\n", t[k].what, hr, !o ? "null" : o == (void *)1 ? "untouched" : tname(o));
                if (SUCCEEDED(hr) && o) IUnknown_Release((IUnknown *)o);
                IDirectMusicAudioPath_Release(ap);
            }
            void *o = (void *)1;
            hr = IDirectMusicSegmentState8_GetObjectInPath(s8, 0, DMUS_PATH_PERFORMANCE_GRAPH, 0, &GUID_NULL, 0, &IID_IDirectMusicGraph, &o);
            printf("  perf graph %08lx %s\n", hr, !o ? "null" : o == (void *)1 ? "untouched" : gname(o));
            if (SUCCEEDED(hr) && o) IUnknown_Release((IUnknown *)o);
        }
    }
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(200);
    IDirectMusicPerformance8_SetGraph(perf, NULL);
    IDirectMusicSegmentState8_Release(s8);
    IDirectMusicSegmentState_Release(ss);
    IDirectMusicSegment8_Release(s);
    IDirectMusicGraph_Release(pg);
    ngraphs = 0;
    nlog = 0;
}

/* ------------------------------------------------------------------ a tool that frees, or sends on itself */
static void actions(void)
{
    printf("tool actions\n");
    IDirectMusicGraph *pg = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&pg);
    name_graph(pg, "PG3");
    static const DWORD tn[] = { DMUS_PMSGT_NOTE };
    static const DWORD tm[] = { DMUS_PMSGT_MIDI, DMUS_PMSGT_PATCH };
    tool *K = new_tool("K", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 1, tn); K->act = ACT_KEEP_SEND;
    tool *M = new_tool("M", FALSE, DMUS_PMSGF_TOOL_QUEUE, 2, tm); M->act = ACT_FREE;
    tool *N = new_tool("N", FALSE, DMUS_PMSGF_TOOL_ATTIME, 1, tn);
    IDirectMusicGraph_InsertTool(pg, TI(K), NULL, 0, 0);
    IDirectMusicGraph_InsertTool(pg, TI(M), NULL, 0, 1);
    IDirectMusicGraph_InsertTool(pg, TI(N), NULL, 0, 2);
    IDirectMusicPerformance8_SetGraph(perf, pg);
    IDirectMusicSegment8 *s = load("fe_hard_04.sgt");
    IDirectMusicSegmentState *ss = NULL;
    if (s) {
        HRESULT hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)s, NULL, NULL, 0, 0, &ss, NULL, NULL);
        printf("  play %08lx\n", hr);
        Sleep(2500);
        MUSIC_TIME st = 0;
        IDirectMusicSegmentState_GetStartTime(ss, &st);
        IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
        Sleep(300);
        printf("  messages\n");
        dumplog(st, 768);
        IDirectMusicSegmentState_Release(ss);
        IDirectMusicSegment8_Release(s);
    }
    IDirectMusicPerformance8_SetGraph(perf, NULL);
    IDirectMusicGraph_Release(pg);
    ngraphs = 0;
}

/* ------------------------------------------------------------------ Flush on invalidation */
static void flushes(void)
{
    printf("flush\n");
    IDirectMusicGraph *pg = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&pg);
    name_graph(pg, "PG4");
    tool *L = new_tool("L", FALSE, DMUS_PMSGF_TOOL_ATTIME, 0, NULL);
    tool *Q2 = new_tool("Q2", FALSE, DMUS_PMSGF_TOOL_QUEUE, 0, NULL);
    IDirectMusicGraph_InsertTool(pg, TI(L), NULL, 0, 0);
    IDirectMusicPerformance8_SetGraph(perf, pg);
    REFERENCE_TIME now = 0;
    IDirectMusicPerformance8_GetTime(perf, &now, NULL);
    static const struct { DWORD type; BYTE flags; LONGLONG ahead; const char *what; } c[] = {
        { DMUS_PMSGT_NOTE, DMUS_NOTEF_NOTEON, 30000000, "note" },
        { DMUS_PMSGT_NOTE, DMUS_NOTEF_NOTEON | DMUS_NOTEF_NOINVALIDATE, 30000000, "note noinvalidate" },
        { DMUS_PMSGT_MIDI, 0, 30000000, "midi" },
        { DMUS_PMSGT_USER, 0, 30000000, "user" },
    };
    for (unsigned k = 0; k < sizeof(c) / sizeof(c[0]); k++) {
        DMUS_NOTE_PMSG *n = NULL;
        IDirectMusicPerformance8_AllocPMsg(perf, sizeof(*n), (DMUS_PMSG **)&n);
        n->dwType = c[k].type; n->rtTime = now + c[k].ahead; n->dwFlags = DMUS_PMSGF_REFTIME; n->dwPChannel = 0;
        n->bFlags = c[k].flags; n->mtDuration = 100; n->bVelocity = 100; n->bMidiValue = 60;
        if (c[k].type == DMUS_PMSGT_MIDI) { DMUS_MIDI_PMSG *mm = (DMUS_MIDI_PMSG *)n; mm->bStatus = 0xb0; mm->bByte1 = 7; mm->bByte2 = 100; }
        HRESULT hr = IDirectMusicGraph_StampPMsg(perfgraph, (DMUS_PMSG *)n);
        printf("  %s stamp %08lx tool %s flags %02lx", c[k].what, hr, tname(n->pTool), n->dwFlags);
        hr = IDirectMusicPerformance8_SendPMsg(perf, (DMUS_PMSG *)n);
        printf(" send %08lx\n", hr);
    }
    DMUS_PMSG *q = NULL;
    IDirectMusicPerformance8_AllocPMsg(perf, sizeof(DMUS_PMSG), &q);
    q->dwType = DMUS_PMSGT_USER; q->rtTime = now + 30000000; q->dwFlags = DMUS_PMSGF_REFTIME | DMUS_PMSGF_TOOL_QUEUE;
    q->pTool = TI(Q2); IDirectMusicTool_AddRef(q->pTool);
    printf("  own tool send %08lx\n", IDirectMusicPerformance8_SendPMsg(perf, q));
    Sleep(300);
    printf("  before: L processed %ld flushes %ld, Q2 processed %ld flushes %ld\n", L->processed, L->flushes, Q2->processed, Q2->flushes);
    printf("  invalidate %08lx\n", IDirectMusicPerformance8_Invalidate(perf, 0, 0));
    Sleep(300);
    printf("  after: L processed %ld flushes %ld, Q2 processed %ld flushes %ld\n", L->processed, L->flushes, Q2->processed, Q2->flushes);
    IDirectMusicPerformance8_SetGraph(perf, NULL);
    IDirectMusicGraph_Release(pg);
    ngraphs = 0;
    nlog = 0;
}

/* ------------------------------------------------------------------ loading a graph ('DMTG') */
static BYTE gbuf[1024];
static DWORD gpos;
static void put(const void *d, DWORD n) { memcpy(gbuf + gpos, d, n); gpos += n; }
static void put4(const char *f) { put(f, 4); }
static void putd(DWORD v) { put(&v, 4); }
static DWORD open_ck(const char *id, const char *type) { put4(id); DWORD at = gpos; putd(0); if (type) put4(type); return at; }
static void close_ck(DWORD at) { DWORD n = gpos - at - 4; memcpy(gbuf + at, &n, 4); if (n & 1) gbuf[gpos++] = 0; }
static DWORD build_graph(void)
{
    gpos = 0;
    DWORD r = open_ck("RIFF", "DMTG");
    static const GUID gg = { 0x12345678, 0x1111, 0x2222, { 1, 2, 3, 4, 5, 6, 7, 8 } };
    DWORD c = open_ck("guid", NULL); put(&gg, 16); close_ck(c);
    c = open_ck("vers", NULL); putd(0x00010002); putd(3); close_ck(c);
    DWORD l = open_ck("LIST", "UNFO");
    c = open_ck("UNAM", NULL); put(L"tool graph", 22); close_ck(c);
    close_ck(l);
    l = open_ck("LIST", "toll");
    static const GUID bogus = { 0xdeadbeef, 0x1, 0x2, { 3, 4, 5, 6, 7, 8, 9, 10 } };
    DWORD t = open_ck("RIFF", "DMTL");                  /* an unregistered tool with two PChannels */
    c = open_ck("tolh", NULL); put(&bogus, 16); putd(0); putd(2); put4("abcd"); putd(0); putd(1); putd(3); close_ck(c);
    c = open_ck("abcd", NULL); putd(0); close_ck(c);
    close_ck(t);
    t = open_ck("RIFF", "DMTL");                        /* no chunk id or form type */
    c = open_ck("tolh", NULL); put(&bogus, 16); putd(0); putd(0); putd(0); putd(0); putd(0); close_ck(c);
    close_ck(t);
    t = open_ck("RIFF", "DMTL");                        /* the header not first */
    c = open_ck("abcd", NULL); putd(0); close_ck(c);
    c = open_ck("tolh", NULL); put(&bogus, 16); putd(0); putd(0); put4("abcd"); putd(0); putd(0); close_ck(c);
    close_ck(t);
    close_ck(l);
    close_ck(r);
    return gpos;
}
static IStream *mem_stream(const BYTE *d, DWORD n)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    memcpy(GlobalLock(h), d, n); GlobalUnlock(h);
    IStream *s = NULL;
    CreateStreamOnHGlobal(h, TRUE, &s);
    return s;
}
static void showdesc(const char *what, IUnknown *o)
{
    IDirectMusicObject *ob = NULL;
    if (FAILED(IUnknown_QueryInterface(o, &IID_IDirectMusicObject, (void **)&ob))) { printf("  %s: no object\n", what); return; }
    DMUS_OBJECTDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    HRESULT hr = IDirectMusicObject_GetDescriptor(ob, &d);
    char nm[64]; WideCharToMultiByte(CP_ACP, 0, d.wszName, -1, nm, sizeof(nm), NULL, NULL);
    printf("  %s descriptor %08lx valid %08lx name '%s' version %lx.%lx guid %08lx\n", what, hr, d.dwValidData, nm,
           d.vVersion.dwVersionMS, d.vVersion.dwVersionLS, d.guidObject.Data1);
    IDirectMusicObject_Release(ob);
}
static void graphload(void)
{
    printf("graph load\n");
    DWORD n = build_graph();
    IDirectMusicGraph *g = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&g);
    IPersistStream *ps = NULL;
    IDirectMusicGraph_QueryInterface(g, &IID_IPersistStream, (void **)&ps);
    tool *A = new_tool("LA", FALSE, DMUS_PMSGF_TOOL_IMMEDIATE, 0, NULL);
    IDirectMusicGraph_InsertTool(g, TI(A), NULL, 0, 0);
    IStream *st = mem_stream(gbuf, n);
    printf("  load %08lx\n", IPersistStream_Load(ps, st));
    LARGE_INTEGER z = { { 0, 0 } }; ULARGE_INTEGER pos;
    IStream_Seek(st, z, STREAM_SEEK_CUR, &pos);
    printf("  stream at %lu of %lu\n", (unsigned long)pos.QuadPart, n);
    IDirectMusicTool *t = (IDirectMusicTool *)1;
    printf("  tool 0 %08lx %s, LA refs %ld\n", IDirectMusicGraph_GetTool(g, 0, &t), t == (void *)1 ? "untouched" : tname(t), refs((IUnknown *)TI(A)));
    showdesc("graph", (IUnknown *)g);
    IStream_Release(st);
    st = mem_stream(gbuf + 12, n - 12);
    printf("  load not a graph %08lx\n", IPersistStream_Load(ps, st));
    IStream_Release(st);
    IDirectMusicObject *ob = NULL;
    IDirectMusicGraph_QueryInterface(g, &IID_IDirectMusicObject, (void **)&ob);
    DMUS_OBJECTDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    st = mem_stream(gbuf, n);
    HRESULT hr = IDirectMusicObject_ParseDescriptor(ob, st, &d);
    char nm[64]; WideCharToMultiByte(CP_ACP, 0, d.wszName, -1, nm, sizeof(nm), NULL, NULL);
    printf("  parse %08lx valid %08lx name '%s' class graph %d\n", hr, d.dwValidData, nm, IsEqualGUID(&d.guidClass, &CLSID_DirectMusicGraph));
    IStream_Release(st);
    st = mem_stream(gbuf + 12, n - 12);
    printf("  parse not a graph %08lx\n", IDirectMusicObject_ParseDescriptor(ob, st, &d));
    IStream_Release(st);
    IDirectMusicObject_Release(ob);
    IPersistStream_Release(ps);
    IDirectMusicGraph_Release(g);

    /* bells1.sgt with the graph in it */
    HANDLE f = CreateFileA("bells1.sgt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD sz = GetFileSize(f, NULL), got;
    BYTE *b = malloc(sz + n);
    ReadFile(f, b, sz, &got, NULL);
    CloseHandle(f);
    memcpy(b + sz, gbuf, n);
    DWORD rs = sz + n - 8; memcpy(b + 4, &rs, 4);
    f = CreateFileA("tg_bells.sgt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(f, b, sz + n, &got, NULL);
    CloseHandle(f);
    free(b);
    IDirectMusicSegment8 *s = load("tg_bells.sgt");
    if (s) {
        g = (IDirectMusicGraph *)1;
        hr = IDirectMusicSegment8_GetGraph(s, &g);
        printf("  segment graph %08lx %s\n", hr, g == (void *)1 ? "untouched" : g ? "obj" : "null");
        if (SUCCEEDED(hr)) {
            showdesc("segment's graph", (IUnknown *)g);
            t = (IDirectMusicTool *)1;
            printf("  its tool 0 %08lx\n", IDirectMusicGraph_GetTool(g, 0, &t));
            IDirectMusicGraph_Release(g);
        }
        IDirectMusicSegment8_Release(s);
    }
    DeleteFileA("tg_bells.sgt");
}

int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;
    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&logcs);
    WCHAR wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argc > 1 ? argv[1] : ".", -1, wdir, MAX_PATH);
    CoInitialize(NULL);
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    IDirectMusicGraph *g = (IDirectMusicGraph *)1;
    printf("get graph before init %08lx\n", IDirectMusicPerformance8_GetGraph(perf, &g));
    printf("set graph before init %08lx\n", IDirectMusicPerformance8_SetGraph(perf, NULL));
    HRESULT hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    if (FAILED(hr)) { printf("InitAudio %08lx\n", hr); return 1; }
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, wdir, FALSE);
    IDirectMusicPerformance8_QueryInterface(perf, &IID_IDirectMusicGraph, (void **)&perfgraph);
    IDirectMusicPerformance8_QueryInterface(perf, &IID_IDirectMusicTool, (void **)&perftool);
    graphobj();
    perfgraphs();
    seggraph();
    apgraph();
    stages();
    actions();
    flushes();
    graphload();
    if (perfgraph) IDirectMusicGraph_Release(perfgraph);
    if (perftool) IDirectMusicTool_Release(perftool);
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    IDirectMusicLoader8_Release(loader);
    CoUninitialize();
    return 0;
}
