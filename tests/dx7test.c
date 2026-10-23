/* dx7test - what changes with the interface an application asks the performance for: dmime keeps a version (6 when
 * created, 7 after a QueryInterface for IDirectMusicPerformance2, 8 after one for IDirectMusicPerformance8, each
 * QueryInterface setting it again) and answers DirectX 6/7 applications as DirectX 6/7 did.  Prints every HRESULT and
 * out-parameter.  Run with tests/comcmp.sh dx7test.exe [6|7|8|qi|notify]. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dmusici.h>
#include <dmusicc.h>
#include <dsound.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

/* ------------------------------------------------------------------ a tool that notes what reaches it */
typedef struct {
    IDirectMusicTool IDirectMusicTool_iface;
    LONG ref;
    volatile LONG got;
    REFERENCE_TIME rt;
    MUSIC_TIME mt;
    DWORD flags;
} tool;
static tool *tl(IDirectMusicTool *i) { return (tool *)i; }
static HRESULT WINAPI t_qi(IDirectMusicTool *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicTool)) { *o = i; InterlockedIncrement(&tl(i)->ref); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI t_addref(IDirectMusicTool *i) { return InterlockedIncrement(&tl(i)->ref); }
static ULONG WINAPI t_release(IDirectMusicTool *i) { return InterlockedDecrement(&tl(i)->ref); }    /* (static) */
static HRESULT WINAPI t_init(IDirectMusicTool *i, IDirectMusicGraph *g) { return S_OK; }
static HRESULT WINAPI t_deliv(IDirectMusicTool *i, DWORD *d) { *d = DMUS_PMSGF_TOOL_IMMEDIATE; return S_OK; }
static HRESULT WINAPI t_nt(IDirectMusicTool *i, DWORD *n) { *n = 1; return S_OK; }
static HRESULT WINAPI t_types(IDirectMusicTool *i, DWORD **a, DWORD n) { if (n) (*a)[0] = DMUS_PMSGT_USER; return S_OK; }
static HRESULT WINAPI t_process(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m)
{
    tool *t = tl(i);
    if (m->dwType == DMUS_PMSGT_USER && !t->got) { t->rt = m->rtTime; t->mt = m->mtTime; t->flags = m->dwFlags; InterlockedExchange(&t->got, 1); }
    return DMUS_S_FREE;
}
static HRESULT WINAPI t_flush(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m, REFERENCE_TIME rt) { return S_OK; }
static IDirectMusicToolVtbl t_vtbl = { t_qi, t_addref, t_release, t_init, t_deliv, t_nt, t_types, t_process, t_flush };
static tool s_tool = { { &t_vtbl }, 1 };

static const GUID *iid_of(int v)
{
    return v == 8 ? &IID_IDirectMusicPerformance8 : v == 7 ? &IID_IDirectMusicPerformance2 : &IID_IDirectMusicPerformance;
}

/* the graph: before Init (DMUS_E_NOT_INIT is DirectX 8's answer), one set, and whether it is the one */
static void graph(IDirectMusicPerformance *pf, IDirectMusicGraph *mine, const char *when)
{
    IDirectMusicGraph *g = NULL;
    HRESULT hr = IDirectMusicPerformance_GetGraph(pf, &g);
    P("  %s: GetGraph %08lx %s\n", when, hr, !g ? "none" : g == mine ? "ours" : "another");
    if (g) IDirectMusicGraph_Release(g);
}
static void timing(IDirectMusicPerformance *pf, const char *when)
{
    DWORD a = 0xdead, b = 0xdead;
    HRESULT h1 = IDirectMusicPerformance_GetPrepareTime(pf, &a), h2 = IDirectMusicPerformance_GetBumperLength(pf, &b);
    P("  %s: GetPrepareTime %08lx %lu GetBumperLength %08lx %lu\n", when, h1, a, h2, b);
}
/* a user message with no time, as REFTIME, MUSICTIME or neither: DirectX 8 stamps "now", the others take it as it is */
static void send_untimed(IDirectMusicPerformance *pf, DWORD flags, const char *what)
{
    IDirectMusicGraph *pg = NULL;
    Sleep(50);                      /* (music time well past 0, as the performance just started) */
    IDirectMusicPerformance_QueryInterface(pf, &IID_IDirectMusicGraph, (void **)&pg);
    DMUS_PMSG *m = NULL;
    HRESULT hr = IDirectMusicPerformance_AllocPMsg(pf, sizeof(DMUS_PMSG), &m);
    if (FAILED(hr) || !m) { P("  %s: AllocPMsg %08lx\n", what, hr); if (pg) IDirectMusicGraph_Release(pg); return; }
    m->dwType = DMUS_PMSGT_USER;
    m->dwFlags = flags;
    m->rtTime = 0; m->mtTime = 0;
    HRESULT sh = pg ? IDirectMusicGraph_StampPMsg(pg, m) : E_FAIL;
    s_tool.got = 0;
    hr = IDirectMusicPerformance_SendPMsg(pf, m);
    if (FAILED(hr)) IDirectMusicPerformance_FreePMsg(pf, m);
    for (int k = 0; k < 100 && !s_tool.got; k++) Sleep(10);
    REFERENCE_TIME now = 0;
    IDirectMusicPerformance_GetTime(pf, &now, NULL);
    P("  %s: StampPMsg %08lx SendPMsg %08lx tool %s", what, sh, hr, s_tool.got ? "got it" : "nothing");
    if (s_tool.got) P(", time %s, mt %s, flags %lx", !s_tool.rt ? "0" : s_tool.rt <= now ? "now" : "later", !s_tool.mt ? "0" : "set",
                      s_tool.flags & (DMUS_PMSGF_REFTIME | DMUS_PMSGF_MUSICTIME));
    P("\n");
    if (pg) IDirectMusicGraph_Release(pg);
}

static void version(int v)
{
    IDirectMusicPerformance *pf = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, iid_of(v), (void **)&pf);
    P("  CoCreateInstance %08lx\n", hr);
    if (!pf) return;
    IDirectMusicGraph *mine = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&mine);
    if (mine) IDirectMusicGraph_InsertTool(mine, &s_tool.IDirectMusicTool_iface, NULL, 0, 0);
    graph(pf, mine, "before Init");
    P("  before Init: SetGraph %08lx\n", IDirectMusicPerformance_SetGraph(pf, mine));
    graph(pf, mine, "before Init, set");
    P("  before Init: SetPrepareTime %08lx SetBumperLength %08lx\n", IDirectMusicPerformance_SetPrepareTime(pf, 1500),
      IDirectMusicPerformance_SetBumperLength(pf, 80));
    timing(pf, "before Init");
    IDirectMusic *dm = NULL;
    hr = IDirectMusicPerformance_Init(pf, &dm, NULL, NULL);
    P("  Init %08lx dm %s\n", hr, dm ? "yes" : "none");
    graph(pf, mine, "after Init");
    timing(pf, "after Init");
    P("  SetGraph %08lx\n", IDirectMusicPerformance_SetGraph(pf, mine));
    graph(pf, mine, "set after Init");
    send_untimed(pf, DMUS_PMSGF_REFTIME, "untimed, REFTIME");
    send_untimed(pf, DMUS_PMSGF_MUSICTIME, "untimed, MUSICTIME");
    send_untimed(pf, 0, "untimed, no flag");
    P("  SetPrepareTime %08lx SetBumperLength %08lx\n", IDirectMusicPerformance_SetPrepareTime(pf, 1200),
      IDirectMusicPerformance_SetBumperLength(pf, 70));
    P("  CloseDown %08lx\n", IDirectMusicPerformance_CloseDown(pf));
    graph(pf, mine, "after CloseDown");
    timing(pf, "after CloseDown");
    if (dm) { IDirectMusic_Release(dm); dm = NULL; }
    hr = IDirectMusicPerformance_Init(pf, &dm, NULL, NULL);
    P("  Init again %08lx dm %s\n", hr, dm ? "yes" : "none");
    graph(pf, mine, "after Init again");
    timing(pf, "after Init again");
    P("  CloseDown %08lx\n", IDirectMusicPerformance_CloseDown(pf));
    if (dm) IDirectMusic_Release(dm);
    P("  Release %lu\n", IDirectMusicPerformance_Release(pf));
    if (mine) IDirectMusicGraph_Release(mine);
}

/* each QueryInterface sets the version again: GetGraph before Init tells which */
static void qi(void)
{
    static const struct { const GUID *iid; const char *n; } seq[] = {
        { &IID_IDirectMusicPerformance8, "IDirectMusicPerformance8" }, { &IID_IDirectMusicPerformance2, "IDirectMusicPerformance2" },
        { &IID_IUnknown, "IUnknown" }, { &IID_IDirectMusicPerformance, "IDirectMusicPerformance" },
        { &IID_IDirectMusicPerformance8, "IDirectMusicPerformance8" }, { &IID_IDirectMusicGraph, "IDirectMusicGraph" },
        { &IID_IDirectMusicPerformance2, "IDirectMusicPerformance2" }, { &IID_IDirectMusicTool, "IDirectMusicTool" },
    };
    IDirectMusicPerformance *pf = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance, (void **)&pf);
    P("  CoCreateInstance(IDirectMusicPerformance) %08lx\n", hr);
    if (!pf) return;
    graph(pf, NULL, "as created");
    for (int k = 0; k < (int)(sizeof(seq) / sizeof(seq[0])); k++) {
        IUnknown *u = NULL;
        hr = IDirectMusicPerformance_QueryInterface(pf, seq[k].iid, (void **)&u);
        P("  QueryInterface(%s) %08lx\n", seq[k].n, hr);
        if (u) IUnknown_Release(u);
        graph(pf, NULL, "then");
    }
    P("  Release %lu\n", IDirectMusicPerformance_Release(pf));
}

/* Init without AddPort: no port, so no latency and no PChannels, also once a segment plays */
static void latency(IDirectMusicPerformance *pf, const char *when)
{
    REFERENCE_TIME now = 0, lat = 0;
    IDirectMusicPerformance_GetTime(pf, &now, NULL);
    HRESULT hr = IDirectMusicPerformance_GetLatencyTime(pf, &lat);
    IDirectMusicPort *pp = NULL;
    DWORD g = 0xdead, c = 0xdead;
    HRESULT ph = IDirectMusicPerformance_PChannelInfo(pf, 0, &pp, &g, &c);
    P("  %s: GetLatencyTime %08lx %s, PChannelInfo(0) %08lx port %s group %lx channel %lx\n", when, hr,
      lat - now <= 50000 ? "now" : "ahead", ph, pp ? "yes" : "none", g, c);
    if (pp) IDirectMusicPort_Release(pp);
}
static void noport(int v)
{
    IDirectMusicPerformance *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, iid_of(v), (void **)&pf);
    if (!pf) { P("  no performance\n"); return; }
    P("  Init %08lx\n", IDirectMusicPerformance_Init(pf, NULL, NULL, NULL));
    latency(pf, "after Init");
    IDirectMusicSegment *seg = NULL;
    CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC, &IID_IDirectMusicSegment, (void **)&seg);
    if (seg) {
        IDirectMusicSegment_SetLength(seg, DMUS_PPQ * 4);
        IDirectMusicSegmentState *st = NULL;
        P("  PlaySegment %08lx\n", IDirectMusicPerformance_PlaySegment(pf, seg, 0, 0, &st));
        Sleep(300);
        latency(pf, "playing");
        P("  AddPort(NULL) %08lx\n", IDirectMusicPerformance_AddPort(pf, NULL));
        latency(pf, "after AddPort");
        P("  Stop %08lx\n", IDirectMusicPerformance_Stop(pf, NULL, NULL, 0, 0));
        if (st) IDirectMusicSegmentState_Release(st);
        IDirectMusicSegment_Release(seg);
    }
    IDirectMusicPerformance_CloseDown(pf);
    IDirectMusicPerformance_Release(pf);
}

/* Stop of everything sets every PChannel back on its port: All Sound Off, Reset All Controllers (and a GM reset to a
 * DirectX 8 application's ports); a note held through Stop goes */
static DWORD voices(IDirectMusicPort *p)
{
    DMUS_SYNTHSTATS st;
    memset(&st, 0, sizeof(st));
    st.dwSize = sizeof(st);
    IDirectMusicPort_GetRunningStats(p, &st);
    return st.dwVoices;
}
static void send_midi(IDirectMusicPerformance *pf, BYTE status, BYTE b1, BYTE b2)
{
    DMUS_MIDI_PMSG *m = NULL;
    if (FAILED(IDirectMusicPerformance_AllocPMsg(pf, sizeof(*m), (DMUS_PMSG **)&m))) return;
    m->dwType = DMUS_PMSGT_MIDI; m->dwFlags = DMUS_PMSGF_REFTIME; m->rtTime = 0; m->dwPChannel = 0;
    m->bStatus = status; m->bByte1 = b1; m->bByte2 = b2;
    IDirectMusicGraph *pg = NULL;
    IDirectMusicPerformance_QueryInterface(pf, &IID_IDirectMusicGraph, (void **)&pg);
    if (pg) { IDirectMusicGraph_StampPMsg(pg, (DMUS_PMSG *)m); IDirectMusicGraph_Release(pg); }
    if (FAILED(IDirectMusicPerformance_SendPMsg(pf, (DMUS_PMSG *)m))) IDirectMusicPerformance_FreePMsg(pf, (DMUS_PMSG *)m);
}
static void stopall(int v)
{
    IDirectMusicPerformance *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, iid_of(v), (void **)&pf);
    if (!pf) { P("  no performance\n"); return; }
    P("  Init %08lx AddPort %08lx\n", IDirectMusicPerformance_Init(pf, NULL, NULL, NULL), IDirectMusicPerformance_AddPort(pf, NULL));
    IDirectMusicLoader8 *ld = NULL;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    IDirectMusicCollection *c = NULL;
    IDirectMusicInstrument *in = NULL;
    IDirectMusicDownloadedInstrument *di = NULL;
    IDirectMusicPort *port = NULL;
    if (ld) {
        WCHAR dir[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, dir);
        IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, dir, FALSE);
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicCollection, &IID_IDirectMusicCollection, L"fe_hard.dls", (void **)&c);
    }
    DWORD patch = 0, g = 0, ch = 0;
    if (c) {
        WCHAR nm[64];
        IDirectMusicCollection_EnumInstrument(c, 0, &patch, nm, 64);
        IDirectMusicCollection_GetInstrument(c, patch, &in);
    }
    if (in) {
        IDirectMusicInstrument_SetPatch(in, 0);
        P("  DownloadInstrument %08lx\n", IDirectMusicPerformance_DownloadInstrument(pf, in, 0, &di, NULL, 0, &port, &g, &ch));
    }
    if (port) {
        send_midi(pf, 0x90, 60, 100);
        Sleep(2500);                /* (the synth's statistics are a second's) */
        P("  note on: %s\n", voices(port) ? "sounding" : "silent");
        P("  Stop %08lx\n", IDirectMusicPerformance_Stop(pf, NULL, NULL, 0, 0));
        Sleep(2500);
        P("  after Stop: %s\n", voices(port) ? "sounding" : "silent");
        send_midi(pf, 0x80, 60, 0);
        Sleep(300);
    }
    if (di && port) IDirectMusicPort_UnloadInstrument(port, di);
    if (di) IDirectMusicDownloadedInstrument_Release(di);
    if (port) IDirectMusicPort_Release(port);
    if (in) IDirectMusicInstrument_Release(in);
    if (c) IDirectMusicCollection_Release(c);
    if (ld) IDirectMusicLoader8_Release(ld);
    IDirectMusicPerformance_CloseDown(pf);
    IDirectMusicPerformance_Release(pf);
}

/* the notifications of a segment played to its end */
static void notify(int v)
{
    IDirectMusicPerformance *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, iid_of(v), (void **)&pf);
    if (!pf) { P("  no performance\n"); return; }
    HRESULT hr = IDirectMusicPerformance_Init(pf, NULL, NULL, NULL);
    P("  Init %08lx AddPort %08lx\n", hr, IDirectMusicPerformance_AddPort(pf, NULL));
    IDirectMusicSegment *seg = NULL;
    CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC, &IID_IDirectMusicSegment, (void **)&seg);
    if (!seg) { P("  no segment\n"); IDirectMusicPerformance_Release(pf); return; }
    IDirectMusicSegment_SetLength(seg, DMUS_PPQ * 4);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    IDirectMusicPerformance_SetNotificationHandle(pf, ev, 0);
    P("  AddNotificationType segment %08lx performance %08lx\n",
      IDirectMusicPerformance_AddNotificationType(pf, &GUID_NOTIFICATION_SEGMENT),
      IDirectMusicPerformance_AddNotificationType(pf, &GUID_NOTIFICATION_PERFORMANCE));
    IDirectMusicSegmentState *st = NULL;
    hr = IDirectMusicPerformance_PlaySegment(pf, seg, 0, 0, &st);
    P("  PlaySegment %08lx\n", hr);
    DWORD t0 = GetTickCount();
    int stopped = 0, n = 0;
    MUSIC_TIME mt0 = 0;
    if (st) IDirectMusicSegmentState_GetStartTime(st, &mt0);
    char got[32][48];               /* (sorted before printing: the ones at the same time come in either order) */
    while (!stopped && GetTickCount() - t0 < 8000) {
        WaitForSingleObject(ev, 100);
        DMUS_NOTIFICATION_PMSG *m;
        while (IDirectMusicPerformance_GetNotificationPMsg(pf, &m) == S_OK) {
            int perfn = IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_PERFORMANCE);
            long d = (long)(m->mtTime - mt0);
            if (n < 32) snprintf(got[n], sizeof(got[n]), "%s %lu %s", perfn ? "performance" :
                                 IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_SEGMENT) ? "segment" : "other",
                                 m->dwNotificationOption, d < 0 ? "before the end" : d ? "after the start" : "at the start");
            if (perfn && m->dwNotificationOption == DMUS_NOTIFICATION_MUSICSTOPPED) stopped = 1;
            IDirectMusicPerformance_FreePMsg(pf, (DMUS_PMSG *)m);
            n++;
        }
    }
    qsort(got, n < 32 ? n : 32, sizeof(got[0]), (int (*)(const void *, const void *))strcmp);
    for (int k = 0; k < n && k < 32; k++) P("  %s\n", got[k]);
    P("  %d notifications%s\n", n, stopped ? "" : ", no MUSICSTOPPED");
    if (st) IDirectMusicSegmentState_Release(st);
    IDirectMusicPerformance_CloseDown(pf);
    IDirectMusicSegment_Release(seg);
    IDirectMusicPerformance_Release(pf);
    CloseHandle(ev);
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    LoadLibraryA("dsound.dll");
    CoInitialize(NULL);
    for (int v = 6; v <= 8; v++) {
        char n[2] = { (char)('0' + v), 0 };
        if (!only || !strcmp(only, n)) { P("as DirectX %d:\n", v); version(v); }
    }
    if (!only || !strcmp(only, "qi")) { P("qi:\n"); qi(); }
    if (!only || !strcmp(only, "noport"))
        for (int v = 6; v <= 8; v += 2) { P("noport, as DirectX %d:\n", v); noport(v); }
    if (!only || !strcmp(only, "stopall"))
        for (int v = 6; v <= 8; v += 2) { P("stopall, as DirectX %d:\n", v); stopall(v); }
    if (!only || !strcmp(only, "notify"))
        for (int v = 6; v <= 8; v++) { P("notify, as DirectX %d:\n", v); notify(v); }
    CoUninitialize();
    return 0;
}
