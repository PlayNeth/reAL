/* dx7test - what changes with the interface an application asks the performance for: dmime keeps a version (6 when
 * created, 7 after a QueryInterface for IDirectMusicPerformance2, 8 after one for IDirectMusicPerformance8, each
 * QueryInterface setting it again) and answers DirectX 6/7 applications as DirectX 6/7 did; and the performance after
 * CloseDown; and Segment::InitPlay.  Prints every HRESULT and out-parameter.  Run with tests/comcmp.sh dx7test.exe
 * [6|7|8|qi|noport|stopall|closed|notify|initplay]. */
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

/* every method after CloseDown (dmime's CloseDown leaves the performance uninitialised: some refuse with
 * DMUS_E_NOT_INIT, the others answer as they do) */
static void closed(int audio)
{
    IDirectMusicPerformance8 *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf);
    if (!pf) { P("  no performance\n"); return; }
    HRESULT hr = audio ? IDirectMusicPerformance8_InitAudio(pf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 16, DMUS_AUDIOF_ALL, NULL)
                       : IDirectMusicPerformance8_Init(pf, NULL, NULL, NULL);
    P("  %s %08lx\n", audio ? "InitAudio" : "Init", hr);
    if (!audio) P("  AddPort(NULL) %08lx\n", IDirectMusicPerformance8_AddPort(pf, NULL));
    IDirectMusicSegment8 *seg = NULL;
    CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC, &IID_IDirectMusicSegment8, (void **)&seg);
    if (seg) IDirectMusicSegment8_SetLength(seg, DMUS_PPQ * 4);
    IDirectMusicAudioPath *defp = NULL;
    IDirectMusicPerformance8_GetDefaultAudioPath(pf, &defp);
    P("  CloseDown %08lx\n", IDirectMusicPerformance8_CloseDown(pf));
    IDirectMusicSegmentState *st = NULL;
    P("  PlaySegment %08lx\n", IDirectMusicPerformance8_PlaySegment(pf, (IDirectMusicSegment *)seg, 0, 0, &st));
    if (st) { IDirectMusicSegmentState_Release(st); st = NULL; }
    IDirectMusicSegmentState *st8 = NULL;
    P("  PlaySegmentEx %08lx\n", IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)seg, NULL, NULL, 0, 0, &st8, NULL, NULL));
    if (st8) IDirectMusicSegmentState_Release(st8);
    P("  Stop %08lx\n", IDirectMusicPerformance8_Stop(pf, NULL, NULL, 0, 0));
    P("  StopEx %08lx\n", IDirectMusicPerformance8_StopEx(pf, (IUnknown *)seg, 0, 0));
    P("  GetSegmentState %08lx\n", IDirectMusicPerformance8_GetSegmentState(pf, &st, 0));
    if (st) IDirectMusicSegmentState_Release(st);
    P("  IsPlaying %08lx\n", IDirectMusicPerformance8_IsPlaying(pf, (IDirectMusicSegment *)seg, NULL));
    REFERENCE_TIME rt = 0; MUSIC_TIME mt = 0;
    P("  GetTime %08lx\n", IDirectMusicPerformance8_GetTime(pf, &rt, &mt));
    P("  MusicToReferenceTime %08lx\n", IDirectMusicPerformance8_MusicToReferenceTime(pf, 0, &rt));
    P("  ReferenceToMusicTime %08lx\n", IDirectMusicPerformance8_ReferenceToMusicTime(pf, rt, &mt));
    P("  GetLatencyTime %08lx\n", IDirectMusicPerformance8_GetLatencyTime(pf, &rt));
    P("  GetQueueTime %08lx\n", IDirectMusicPerformance8_GetQueueTime(pf, &rt));
    P("  AdjustTime %08lx\n", IDirectMusicPerformance8_AdjustTime(pf, 0));
    REFERENCE_TIME rr = 0;
    P("  GetResolvedTime %08lx\n", IDirectMusicPerformance8_GetResolvedTime(pf, 0, &rr, 0));
    DMUS_PMSG *m = NULL;
    hr = IDirectMusicPerformance8_AllocPMsg(pf, sizeof(DMUS_PMSG), &m);
    P("  AllocPMsg %08lx\n", hr);
    if (m) {
        m->dwType = DMUS_PMSGT_USER; m->dwFlags = DMUS_PMSGF_REFTIME;
        hr = IDirectMusicPerformance8_SendPMsg(pf, m);
        P("  SendPMsg %08lx\n", hr);
        if (FAILED(hr)) P("  FreePMsg %08lx\n", IDirectMusicPerformance8_FreePMsg(pf, m));
    }
    IDirectMusicGraph *g = NULL;
    P("  GetGraph %08lx\n", IDirectMusicPerformance8_GetGraph(pf, &g));
    if (g) IDirectMusicGraph_Release(g);
    P("  SetGraph(NULL) %08lx\n", IDirectMusicPerformance8_SetGraph(pf, NULL));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    P("  SetNotificationHandle %08lx\n", IDirectMusicPerformance8_SetNotificationHandle(pf, ev, 0));
    DMUS_NOTIFICATION_PMSG *nm = NULL;
    P("  GetNotificationPMsg %08lx\n", IDirectMusicPerformance8_GetNotificationPMsg(pf, &nm));
    if (nm) IDirectMusicPerformance8_FreePMsg(pf, (DMUS_PMSG *)nm);
    P("  AddNotificationType %08lx\n", IDirectMusicPerformance8_AddNotificationType(pf, &GUID_NOTIFICATION_SEGMENT));
    P("  RemoveNotificationType %08lx\n", IDirectMusicPerformance8_RemoveNotificationType(pf, &GUID_NOTIFICATION_SEGMENT));
    P("  AddPort(NULL) %08lx\n", IDirectMusicPerformance8_AddPort(pf, NULL));
    P("  RemovePort(NULL) %08lx\n", IDirectMusicPerformance8_RemovePort(pf, NULL));
    IDirectMusicPort *port = NULL; DWORD gr = 0, ch = 0;
    P("  PChannelInfo %08lx\n", IDirectMusicPerformance8_PChannelInfo(pf, 0, &port, &gr, &ch));
    if (port) IDirectMusicPort_Release(port);
    P("  Invalidate %08lx\n", IDirectMusicPerformance8_Invalidate(pf, 0, 0));
    struct { MUSIC_TIME mtTime; double dblTempo; } tp;
    P("  GetParam(tempo) %08lx\n", IDirectMusicPerformance8_GetParam(pf, &GUID_TempoParam, 0xFFFFFFFF, 0, 0, NULL, &tp));
    P("  GetParamEx(tempo) %08lx\n", IDirectMusicPerformance8_GetParamEx(pf, &GUID_TempoParam, 0, 0xFFFFFFFF, 0, 0, NULL, &tp));
    P("  SetParam(mute) %08lx\n", IDirectMusicPerformance8_SetParam(pf, &GUID_MuteParam, 0xFFFFFFFF, 0, 0, NULL));
    long vol = 0;
    P("  SetGlobalParam(volume) %08lx\n", IDirectMusicPerformance8_SetGlobalParam(pf, &GUID_PerfMasterVolume, &vol, sizeof(vol)));
    P("  GetGlobalParam(volume) %08lx\n", IDirectMusicPerformance8_GetGlobalParam(pf, &GUID_PerfMasterVolume, &vol, sizeof(vol)));
    WORD w = 0; BYTE bb = 0;
    DMUS_CHORD_KEY ck; memset(&ck, 0, sizeof(ck));
    P("  MIDIToMusic %08lx\n", IDirectMusicPerformance8_MIDIToMusic(pf, 60, &ck, DMUS_PLAYMODE_FIXED, 0, &w));
    P("  MusicToMIDI %08lx\n", IDirectMusicPerformance8_MusicToMIDI(pf, 60, &ck, DMUS_PLAYMODE_FIXED, 0, &bb));
    DMUS_TIMESIGNATURE ts;              /* (TimeToRhythm divides by the time signature it has: none, on Microsoft's) */
    memset(&ts, 0, sizeof(ts)); ts.bBeatsPerMeasure = 4; ts.bBeat = 4; ts.wGridsPerBeat = 4;
    P("  RhythmToTime %08lx\n", IDirectMusicPerformance8_RhythmToTime(pf, 0, 0, 0, 0, &ts, &mt));
    IDirectMusicAudioPath *ap = NULL;
    P("  CreateStandardAudioPath %08lx\n", IDirectMusicPerformance8_CreateStandardAudioPath(pf, DMUS_APATH_DYNAMIC_STEREO, 16, TRUE, &ap));
    if (ap) IDirectMusicAudioPath_Release(ap);
    IDirectMusicAudioPath *dp = NULL;
    P("  GetDefaultAudioPath %08lx\n", IDirectMusicPerformance8_GetDefaultAudioPath(pf, &dp));
    if (dp) IDirectMusicAudioPath_Release(dp);
    P("  SetDefaultAudioPath(NULL) %08lx\n", IDirectMusicPerformance8_SetDefaultAudioPath(pf, NULL));
    P("  CloseDown again %08lx\n", IDirectMusicPerformance8_CloseDown(pf));
    if (defp) IDirectMusicAudioPath_Release(defp);
    if (seg) IDirectMusicSegment8_Release(seg);
    IDirectMusicPerformance8_Release(pf);
    CloseHandle(ev);
}

static ULONG refs(IUnknown *u) { IUnknown_AddRef(u); return IUnknown_Release(u); }
static void initplay(int init)
{
    IDirectMusicPerformance8 *pf = NULL;
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf);
    IDirectMusicSegment *seg = NULL;        /* (a segment asked for as IDirectMusicSegment2 or 8 has no InitPlay) */
    CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC, &IID_IDirectMusicSegment, (void **)&seg);
    if (!pf || !seg) { P("  no objects\n"); return; }
    if (init) {
        P("  Init %08lx\n", IDirectMusicPerformance8_Init(pf, NULL, NULL, NULL));
        P("  AddPort(NULL) %08lx\n", IDirectMusicPerformance8_AddPort(pf, NULL));
    }
    IDirectMusicSegment_SetLength(seg, DMUS_PPQ * 4);
    IDirectMusicSegment_SetRepeats(seg, 3);
    IDirectMusicSegmentState *st = (IDirectMusicSegmentState *)(ULONG_PTR)1;
    P("  InitPlay(NULL state) %08lx\n", IDirectMusicSegment_InitPlay(seg, NULL, (IDirectMusicPerformance *)pf, 0));
    P("  InitPlay(NULL perf) %08lx", IDirectMusicSegment_InitPlay(seg, &st, NULL, 0));
    P(" state %s\n", st == (IDirectMusicSegmentState *)(ULONG_PTR)1 ? "untouched" : st ? "set" : "NULL");
    st = NULL;
    ULONG pr = refs((IUnknown *)pf), sr = refs((IUnknown *)seg);
    HRESULT hr = IDirectMusicSegment_InitPlay(seg, &st, (IDirectMusicPerformance *)pf, 0x1234);
    P("  InitPlay %08lx state %s, performance refs %+ld, segment refs %+ld\n", hr, st ? "set" : "NULL",
      (long)refs((IUnknown *)pf) - (long)pr, (long)refs((IUnknown *)seg) - (long)sr);
    if (!st) goto out;
    P("  state refs %lu\n", refs((IUnknown *)st));
    IDirectMusicSegmentState8 *st8 = NULL;
    P("  QI(SegmentState8) %08lx\n", IDirectMusicSegmentState_QueryInterface(st, &IID_IDirectMusicSegmentState8, (void **)&st8));
    IDirectMusicGraph *g = NULL;
    hr = IDirectMusicSegmentState_QueryInterface(st, &IID_IDirectMusicGraph, (void **)&g);
    P("  QI(Graph) %08lx\n", hr);
    if (g) IDirectMusicGraph_Release(g);
    DWORD r = 99;
    hr = IDirectMusicSegmentState_GetRepeats(st, &r);
    P("  GetRepeats %08lx %lu\n", hr, r);
    IDirectMusicSegment *gs = NULL;
    hr = IDirectMusicSegmentState_GetSegment(st, &gs);
    P("  GetSegment %08lx %s\n", hr, gs == (IDirectMusicSegment *)seg ? "the segment" : gs ? "another" : "NULL");
    if (gs) IDirectMusicSegment_Release(gs);
    MUSIC_TIME t = 99;
    hr = IDirectMusicSegmentState_GetStartTime(st, &t);
    P("  GetStartTime %08lx %ld\n", hr, (long)t);
    t = 99; hr = IDirectMusicSegmentState_GetSeek(st, &t);
    P("  GetSeek %08lx %ld\n", hr, (long)t);
    t = 99; hr = IDirectMusicSegmentState_GetStartPoint(st, &t);
    P("  GetStartPoint %08lx %ld\n", hr, (long)t);
    if (st8) {
        P("  SetTrackConfig %08lx\n", IDirectMusicSegmentState8_SetTrackConfig(st8, &CLSID_DirectMusicTempoTrack, 1, 0, 0, 0));
        IUnknown *o = NULL;
        hr = IDirectMusicSegmentState8_GetObjectInPath(st8, DMUS_PCHANNEL_ALL, DMUS_PATH_SEGMENT, 0, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&o);
        P("  GetObjectInPath(segment) %08lx %s\n", hr, o ? "set" : "NULL");
        if (o) IUnknown_Release(o);
        o = NULL;
        hr = IDirectMusicSegmentState8_GetObjectInPath(st8, DMUS_PCHANNEL_ALL, DMUS_PATH_PERFORMANCE, 0, &GUID_All_Objects, 0, &IID_IUnknown, (void **)&o);
        P("  GetObjectInPath(performance) %08lx %s\n", hr, o == (IUnknown *)pf ? "the performance" : o ? "another" : "NULL");
        if (o) IUnknown_Release(o);
        IDirectMusicSegmentState8_Release(st8);
    }
    if (init) {
        IDirectMusicGraph *sg = NULL, *pg = NULL;
        IDirectMusicSegmentState_QueryInterface(st, &IID_IDirectMusicGraph, (void **)&sg);
        IDirectMusicPerformance8_QueryInterface(pf, &IID_IDirectMusicGraph, (void **)&pg);
        DMUS_PMSG *m = NULL;
        if (sg && SUCCEEDED(IDirectMusicPerformance8_AllocPMsg(pf, sizeof(DMUS_PMSG), &m))) {
            m->dwType = DMUS_PMSGT_USER; m->dwPChannel = 0;
            hr = IDirectMusicGraph_StampPMsg(sg, m);
            P("  StampPMsg %08lx flags %lx tool %s graph %s\n", hr, m->dwFlags, m->pTool ? "set" : "NULL",
              m->pGraph == sg ? "the state's" : m->pGraph == pg ? "the performance's" : m->pGraph ? "another" : "NULL");
            IDirectMusicPerformance8_FreePMsg(pf, m);
        }
        if (pg) IDirectMusicGraph_Release(pg);
        if (sg) IDirectMusicGraph_Release(sg);
        P("  IsPlaying(state) %08lx\n", IDirectMusicPerformance8_IsPlaying(pf, NULL, st));
        P("  Stop(state) %08lx\n", IDirectMusicPerformance8_Stop(pf, NULL, st, 0, 0));
        P("  StopEx(state) %08lx\n", IDirectMusicPerformance8_StopEx(pf, (IUnknown *)st, 0, 0));
        IDirectMusicSegmentState *ps = NULL;
        IDirectMusicSegment_SetRepeats(seg, 0);
        hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)seg, NULL, NULL, DMUS_SEGF_SECONDARY, 0, &ps, (IUnknown *)st, NULL);
        P("  PlaySegmentEx(from the state) %08lx\n", hr);
        if (ps) {
            P("  IsPlaying(the played one) %08lx\n", IDirectMusicPerformance8_IsPlaying(pf, NULL, ps));
            IDirectMusicPerformance8_Stop(pf, NULL, NULL, 0, 0);
            IDirectMusicSegmentState_Release(ps);
        }
    }
    P("  state Release %lu\n", IDirectMusicSegmentState_Release(st));
    P("  performance refs %lu\n", refs((IUnknown *)pf));      /* (the segment: still in the one played from the state) */
out:;
    IUnknown *s2 = NULL;
    IDirectMusicSegment_QueryInterface(seg, &IID_IDirectMusicSegment2, (void **)&s2);
    st = NULL;
    hr = IDirectMusicSegment_InitPlay(seg, &st, (IDirectMusicPerformance *)pf, 0);
    P("  InitPlay after QI(Segment2) %08lx state %s\n", hr, st ? "set" : "NULL");
    if (st) IDirectMusicSegmentState_Release(st);
    IDirectMusicSegment *c = NULL;
    hr = IDirectMusicSegment_Clone(seg, 0, 0, &c);
    if (c) {
        st = NULL;
        hr = IDirectMusicSegment_InitPlay(c, &st, (IDirectMusicPerformance *)pf, 0);
        P("  InitPlay on its clone %08lx state %s\n", hr, st ? "set" : "NULL");
        if (st) IDirectMusicSegmentState_Release(st);
        IDirectMusicSegment_Release(c);
    }
    if (s2) IUnknown_Release(s2);
    if (init) IDirectMusicPerformance8_CloseDown(pf);
    IDirectMusicSegment_Release(seg);
    IDirectMusicPerformance8_Release(pf);
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
    if (!only || !strcmp(only, "closed")) {
        P("closed, after Init:\n"); closed(0);
        P("closed, after InitAudio:\n"); closed(1);
    }
    if (!only || !strcmp(only, "notify"))
        for (int v = 6; v <= 8; v++) { P("notify, as DirectX %d:\n", v); notify(v); }
    if (!only || !strcmp(only, "initplay")) {
        P("initplay, no Init:\n"); initplay(0);
        P("initplay, after Init:\n"); initplay(1);
    }
    CoUninitialize();
    return 0;
}
