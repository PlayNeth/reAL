/* trktest - dmime's lyrics, segment trigger and parameter control tracks, printed for comparison with Microsoft's
 * DirectMusic: the tracks' parameters and Clone, and the messages a tool in the performance's graph sees while
 * segments with them play.  trktest [lyrics|trigger] (tools/sty/cases.py tracks writes the segments) */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <dmusici.h>
#include <dmplugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* time() fixed in dmstyle and dmime (their srand(time(NULL)) seeds), as comptest does; reDM takes REDM_TIME.
 * TRK_RANDLOG=<file> logs their rand() calls */
#define FIXTIME 1234567
static long __cdecl fixed_time(long *t) { if (t) *t = FIXTIME; return FIXTIME; }
static FILE *g_randlog;
static int (__cdecl *g_rand)(void);
static int __cdecl rand_log(void)
{
    int v = g_rand();
    if (g_randlog) { fprintf(g_randlog, "R %p %d %lu\n", __builtin_return_address(0), v, GetCurrentThreadId()); fflush(g_randlog); }
    return v;
}
static void patch_import_name(HMODULE m, const char *dll, const char *name, void *fn)
{
    BYTE *base = (BYTE *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dd->VirtualAddress); dd->VirtualAddress && d->Name; d++) {
        if (lstrcmpiA((char *)base + d->Name, dll)) continue;
        IMAGE_THUNK_DATA *on = (void *)(base + d->OriginalFirstThunk);
        void **slot = (void **)(base + d->FirstThunk);
        for (; on->u1.AddressOfData; on++, slot++) {
            if (IMAGE_SNAP_BY_ORDINAL(on->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (void *)(base + on->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, name)) continue;
            DWORD old;
            VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
            *slot = fn;
            VirtualProtect(slot, sizeof(void *), old, &old);
        }
    }
}

static IDirectMusicPerformance8 *perf;
static IDirectMusicLoader8 *loader;

static IDirectMusicSegment8 *load(const WCHAR *name)
{
    IDirectMusicSegment8 *s = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, (WCHAR *)name, (void **)&s);
    printf("load %ls %08lx\n", name, hr);
    return s;
}

/* ------------------------------------------------------------------ a tool that logs what it sees */
typedef struct { MUSIC_TIME mt; DWORD type, pch, flags, group, extra; WCHAR text[32]; } logent;
static logent logv[4096];
static int nlog;
static CRITICAL_SECTION logcs;
static MUSIC_TIME s0;
static IDirectMusicSegmentState *states[16];
static int nstates;

static HRESULT WINAPI t_qi(IDirectMusicTool *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicTool)) { *o = i; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI t_addref(IDirectMusicTool *i) { return 2; }
static ULONG WINAPI t_release(IDirectMusicTool *i) { return 1; }
static HRESULT WINAPI t_init(IDirectMusicTool *i, IDirectMusicGraph *g) { return S_OK; }
static HRESULT WINAPI t_deliv(IDirectMusicTool *i, DWORD *d) { *d = DMUS_PMSGF_TOOL_IMMEDIATE; return S_OK; }
static HRESULT WINAPI t_nt(IDirectMusicTool *i, DWORD *n) { return E_NOTIMPL; }
static HRESULT WINAPI t_types(IDirectMusicTool *i, DWORD **a, DWORD n) { return E_NOTIMPL; }
static HRESULT WINAPI t_process(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m)
{
    EnterCriticalSection(&logcs);
    logent *e = nlog < 4096 ? &logv[nlog++] : NULL;
    if (e) {
        memset(e, 0, sizeof(*e));
        e->mt = m->mtTime; e->type = m->dwType; e->pch = m->dwPChannel; e->flags = m->dwFlags; e->group = m->dwGroupID;
        if (m->dwType == DMUS_PMSGT_NOTE) e->extra = ((DMUS_NOTE_PMSG *)m)->bMidiValue;
        else if (m->dwType == DMUS_PMSGT_LYRIC) lstrcpynW(e->text, ((DMUS_LYRIC_PMSG *)m)->wszString, 32);
        else if (m->dwType == DMUS_PMSGT_NOTIFICATION) {
            DMUS_NOTIFICATION_PMSG *n = (DMUS_NOTIFICATION_PMSG *)m;
            e->extra = n->guidNotificationType.Data1 ^ n->dwNotificationOption;
            if (IsEqualGUID(&n->guidNotificationType, &GUID_NOTIFICATION_SEGMENT) && n->dwNotificationOption == DMUS_NOTIFICATION_SEGSTART &&
                n->punkUser && nstates < 16) {
                IDirectMusicSegmentState *s = NULL;
                if (SUCCEEDED(IUnknown_QueryInterface(n->punkUser, &IID_IDirectMusicSegmentState, (void **)&s))) states[nstates++] = s;
            }
        }
    }
    LeaveCriticalSection(&logcs);
    if (!m->pGraph || FAILED(IDirectMusicGraph_StampPMsg(m->pGraph, m))) return DMUS_S_FREE;
    return DMUS_S_REQUEUE;
}
static HRESULT WINAPI t_flush(IDirectMusicTool *i, IDirectMusicPerformance *p, DMUS_PMSG *m, REFERENCE_TIME rt) { return S_OK; }
static IDirectMusicToolVtbl t_vtbl = { t_qi, t_addref, t_release, t_init, t_deliv, t_nt, t_types, t_process, t_flush };
static IDirectMusicTool logtool = { &t_vtbl };

static int cmplog(const void *a, const void *b)
{
    const logent *x = a, *y = b;
    if (x->mt != y->mt) return x->mt < y->mt ? -1 : 1;
    if (x->type != y->type) return x->type < y->type ? -1 : 1;
    if (x->pch != y->pch) return x->pch < y->pch ? -1 : 1;
    if (x->extra != y->extra) return x->extra < y->extra ? -1 : 1;
    return lstrcmpW(x->text, y->text);
}
static void dump_log(MUSIC_TIME len)
{
    EnterCriticalSection(&logcs);
    for (int k = 0; k < nlog; k++) logv[k].mt -= s0;       /* (messages can come before the start time is known) */
    qsort(logv, nlog, sizeof(*logv), cmplog);
    for (int k = 0; k < nlog; k++) {
        logent *e = &logv[k];
        if (e->type == DMUS_PMSGT_DIRTY || e->type == DMUS_PMSGT_TIMESIG || e->type == DMUS_PMSGT_TEMPO) continue;
        if (e->mt >= len - 768) continue;         /* (what comes near the stop depends on when it comes) */
        if (e->type == DMUS_PMSGT_NOTIFICATION && e->extra == (GUID_NOTIFICATION_SEGMENT.Data1 ^ DMUS_NOTIFICATION_SEGALMOSTEND)) continue;
        printf("  %6ld type %lu pch %lu flags %lx group %lx %lx '%ls'\n", e->mt, e->type, e->pch, e->flags, e->group, e->extra, e->text);
    }
    nlog = 0;
    LeaveCriticalSection(&logcs);
}

/* plays a segment a second ahead, until the music time is `len` past its start */
static void play(IDirectMusicSegment8 *sg, MUSIC_TIME len)
{
    REFERENCE_TIME now = 0;
    IDirectMusicPerformance8_GetTime(perf, &now, NULL);
    IDirectMusicSegmentState *ss = NULL;
    nlog = 0;
    HRESULT hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)sg, NULL, NULL, DMUS_SEGF_REFTIME, now + 10000000, &ss, NULL, NULL);
    printf("play %08lx\n", hr);
    if (!ss) return;
    IDirectMusicSegmentState_GetStartTime(ss, &s0);
    MUSIC_TIME mt = 0;
    DWORD deadline = GetTickCount() + 60000;
    do { Sleep(20); IDirectMusicPerformance8_GetTime(perf, NULL, &mt); } while (mt < s0 + len && (LONG)(deadline - GetTickCount()) > 0);
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(300);
    dump_log(len);
    MUSIC_TIME sv[16][2];
    for (int k = 0; k < nstates; k++) {
        sv[k][0] = sv[k][1] = 0;
        IDirectMusicSegmentState_GetStartTime(states[k], &sv[k][0]);
        IDirectMusicSegmentState_GetStartPoint(states[k], &sv[k][1]);
        sv[k][0] -= s0;
        IDirectMusicSegmentState_Release(states[k]);
    }
    for (int k = 0; k < nstates; k++)
        for (int j = k + 1; j < nstates; j++)
            if (sv[j][0] < sv[k][0] || (sv[j][0] == sv[k][0] && sv[j][1] < sv[k][1])) {
                MUSIC_TIME a0 = sv[k][0], a1 = sv[k][1];
                sv[k][0] = sv[j][0]; sv[k][1] = sv[j][1]; sv[j][0] = a0; sv[j][1] = a1;
            }
    for (int k = 0; k < nstates; k++) printf("  state start %ld point %ld\n", sv[k][0], sv[k][1]);
    nstates = 0;
    IDirectMusicSegmentState_Release(ss);
}

static void track_params(IDirectMusicSegment8 *sg, REFGUID clsid, const char *what)
{
    IDirectMusicTrack *t = NULL;
    HRESULT hr = IDirectMusicSegment8_GetTrack(sg, clsid, 0xffffffff, 0, &t);
    printf("%s track %08lx\n", what, hr);
    if (!t) return;
    static const GUID *const gs[] = { &GUID_TempoParam, &GUID_TimeSignature, &GUID_DisableTempo, &GUID_Valid_Start_Time, &GUID_ChordParam };
    for (int k = 0; k < 5; k++) printf("  isparam %d %08lx\n", k, IDirectMusicTrack_IsParamSupported(t, gs[k]));
    BYTE buf[256];
    MUSIC_TIME nx = 0;
    printf("  getparam tempo %08lx\n", IDirectMusicTrack_GetParam(t, &GUID_TempoParam, 0, &nx, buf));
    printf("  setparam enable tempo %08lx\n", IDirectMusicTrack_SetParam(t, &GUID_EnableTempo, 0, NULL));
    IDirectMusicTrack8 *t8 = NULL;
    printf("  qi track8 %08lx\n", IDirectMusicTrack_QueryInterface(t, &IID_IDirectMusicTrack8, (void **)&t8));
    if (t8) IDirectMusicTrack8_Release(t8);
    IUnknown *u = NULL;
    printf("  qi persist %08lx\n", IDirectMusicTrack_QueryInterface(t, &IID_IPersistStream, (void **)&u));
    if (u) IUnknown_Release(u);
    IDirectMusicTrack *c = NULL;
    printf("  clone %08lx\n", IDirectMusicTrack_Clone(t, 1000, 4000, &c));
    if (c) IDirectMusicTrack_Release(c);
    printf("  clone bad %08lx\n", IDirectMusicTrack_Clone(t, 4000, 1000, &c));
    IDirectMusicTrack_Release(t);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    LoadLibraryA("dsound.dll");
    if (getenv("TRK_RANDLOG")) g_randlog = fopen(getenv("TRK_RANDLOG"), "w");
    g_rand = (void *)GetProcAddress(LoadLibraryA("msvcrt.dll"), "rand");
    static const char *const dlls[] = { "dmstyle.dll", "dmime.dll", "dmcompos.dll" };
    for (int k = 0; k < 3; k++) {
        HMODULE m = LoadLibraryA(dlls[k]);
        if (!m) continue;
        patch_import_name(m, "msvcrt.dll", "time", (void *)fixed_time);
        if (g_randlog) patch_import_name(m, "msvcrt.dll", "rand", (void *)rand_log);
    }
    InitializeCriticalSection(&logcs);
    CoInitialize(NULL);
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, dir, FALSE);
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    HRESULT hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 16, DMUS_AUDIOF_ALL, NULL);
    printf("init %08lx\n", hr);
    IDirectMusicGraph *g = NULL;
    CoCreateInstance(&CLSID_DirectMusicGraph, NULL, CLSCTX_INPROC, &IID_IDirectMusicGraph, (void **)&g);
    IDirectMusicGraph_InsertTool(g, &logtool, NULL, 0, 0);
    IDirectMusicPerformance8_SetGraph(perf, g);
    IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_SEGMENT);
    const char *mode = argc > 1 ? argv[1] : "lyrics";

    if (!strcmp(mode, "lyrics")) {
        IDirectMusicSegment8 *sg = load(L"lyr.sgt");
        if (sg) {
            track_params(sg, &CLSID_DirectMusicLyricsTrack, "lyrics");
            play(sg, 4 * 3072);
            IDirectMusicSegment *c = NULL;
            hr = IDirectMusicSegment8_Clone(sg, 1536, 9216, &c);
            printf("clone %08lx\n", hr);
            if (c) { play((IDirectMusicSegment8 *)c, 3 * 3072); IDirectMusicSegment_Release(c); }
            IDirectMusicSegment8_Release(sg);
        }
        IDirectMusicSegment8 *bad = load(L"lyrbad.sgt");
        if (bad) {
            IDirectMusicTrack *t = NULL;
            printf("  bad lyrics track %08lx\n", IDirectMusicSegment8_GetTrack(bad, &CLSID_DirectMusicLyricsTrack, 0xffffffff, 0, &t));
            if (t) IDirectMusicTrack_Release(t);
            IDirectMusicSegment8_Release(bad);
        }
    } else if (!strcmp(mode, "trigger")) {
        IDirectMusicSegment8 *sg = load(L"trig.sgt");
        if (sg) {
            track_params(sg, &CLSID_DirectMusicSegTriggerTrack, "trigger");
            BYTE buf[256];
            MUSIC_TIME nx = 0;
            printf("segment getparam tempo %08lx\n", IDirectMusicSegment8_GetParam(sg, &GUID_TempoParam, 0xffffffff, 0, 0, &nx, buf));
            printf("segment setparam disable tempo %08lx\n", IDirectMusicSegment8_SetParam(sg, &GUID_DisableTempo, 0xffffffff, DMUS_SEG_ALLTRACKS, 0, NULL));
            IDirectMusicSegment8 *a = load(L"trgA.sgt");
            if (a) {
                printf("  trgA disable tempo again %08lx\n", IDirectMusicSegment8_SetParam(a, &GUID_DisableTempo, 0xffffffff, 0, 0, NULL));
                printf("  trgA enable tempo %08lx\n", IDirectMusicSegment8_SetParam(a, &GUID_EnableTempo, 0xffffffff, 0, 0, NULL));
                IDirectMusicSegment8_Release(a);
            }
            play(sg, 4 * 3072);
            IDirectMusicSegment *c = NULL;
            hr = IDirectMusicSegment8_Clone(sg, 1000, 9216, &c);
            printf("clone %08lx\n", hr);
            if (c) { play((IDirectMusicSegment8 *)c, 3 * 3072); IDirectMusicSegment_Release(c); }
            IDirectMusicSegment8_Release(sg);
        }
        IDirectMusicSegment8 *bad = load(L"trigbad.sgt");
        if (bad) IDirectMusicSegment8_Release(bad);
    }
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    return 0;
}
