/* comtest - DirectMusic COM methods with deterministic results, printed for comparison with Microsoft's DirectMusic:
 * TimeToRhythm / RhythmToTime, ClonePMsg, AdjustTime, PChannelInfo, GetSegmentState, SegmentState::GetSeek.
 * comtest <dir with segments> */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <stdio.h>
#include <dsound.h>

static IDirectMusicPerformance8 *perf;
static IDirectMusicLoader8 *loader;

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

static void rhythm(void)
{
    static const struct { long t0; BYTE bpm, beat; WORD grids; } ts[] = {
        { 0, 4, 4, 4 }, { 0, 3, 4, 2 }, { 100, 6, 8, 3 }, { 0, 7, 8, 0 }, { -50, 5, 2, 7 }, { 0, 4, 0, 4 }, { 0, 2, 16, 5 },
        { 0, 12, 1, 100 },
    };
    static const long times[] = { 0, 1, 95, 96, 97, 191, 192, 383, 384, 767, 768, 769, 1500, 3071, 3072, 3073, 5000, 99999,
                                  -1, -96, -97, -768, -10000 };
    for (unsigned k = 0; k < sizeof(ts) / sizeof(ts[0]); k++) {
        DMUS_TIMESIGNATURE s = { ts[k].t0, ts[k].bpm, ts[k].beat, ts[k].grids };
        printf("timesig %ld %u/%u grids %u\n", s.mtTime, s.bBeatsPerMeasure, s.bBeat, s.wGridsPerBeat);
        for (unsigned j = 0; j < sizeof(times) / sizeof(times[0]); j++) {
            WORD m = 0xeeee; BYTE b = 0xee, g = 0xee; short o = 0x5555;
            HRESULT hr = IDirectMusicPerformance8_TimeToRhythm(perf, times[j], &s, &m, &b, &g, &o);
            MUSIC_TIME back = 0x12345678;
            HRESULT hr2 = IDirectMusicPerformance8_RhythmToTime(perf, m, b, g, o, &s, &back);
            printf("  %ld -> %08lx m %u b %u g %u o %d -> %08lx %ld\n", times[j], hr, m, b, g, o, hr2, back);
        }
        MUSIC_TIME t = 0;
        HRESULT hr = IDirectMusicPerformance8_RhythmToTime(perf, 3, 9, 300, -20, &s, &t);
        printf("  r2t(3,9,300,-20) %08lx %ld\n", hr, t);
    }
}

static void clone(void)
{
    DMUS_NOTE_PMSG *n = NULL, *c = NULL;
    HRESULT hr = IDirectMusicPerformance8_AllocPMsg(perf, sizeof(*n), (DMUS_PMSG **)&n);
    printf("alloc %08lx\n", hr);
    n->rtTime = 1234567; n->mtTime = 777; n->dwFlags = DMUS_PMSGF_MUSICTIME; n->dwPChannel = 5; n->dwVirtualTrackID = 9;
    n->dwType = DMUS_PMSGT_NOTE; n->dwGroupID = 3; n->mtDuration = 480; n->wMusicValue = 60; n->bVelocity = 100;
    n->bMidiValue = 61; n->bFlags = DMUS_NOTEF_NOTEON;
    n->punkUser = (IUnknown *)perf; IDirectMusicPerformance8_AddRef(perf);
    hr = IDirectMusicPerformance8_ClonePMsg(perf, (DMUS_PMSG *)n, (DMUS_PMSG **)&c);
    printf("clone %08lx same %d size %lu\n", hr, c && !memcmp(n, c, sizeof(*n)), c ? c->dwSize : 0);
    ULONG r1 = IDirectMusicPerformance8_AddRef(perf); IDirectMusicPerformance8_Release(perf);
    hr = IDirectMusicPerformance8_FreePMsg(perf, (DMUS_PMSG *)c);
    ULONG r2 = IDirectMusicPerformance8_AddRef(perf); IDirectMusicPerformance8_Release(perf);
    IDirectMusicPerformance8_FreePMsg(perf, (DMUS_PMSG *)n);
    ULONG r3 = IDirectMusicPerformance8_AddRef(perf); IDirectMusicPerformance8_Release(perf);
    printf("free %08lx refs %ld %ld\n", hr, (long)(r1 - r2), (long)(r2 - r3));
    printf("clone NULL %08lx\n", IDirectMusicPerformance8_ClonePMsg(perf, NULL, (DMUS_PMSG **)&c));
}

static void adjust(void)
{
    static const LONGLONG v[] = { 0, 1, -1, 10000000, 10000001, -10000000, -10000001, 5000000, -5000000, 0x100000000LL };
    REFERENCE_TIME a, b;
    for (unsigned k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
        IDirectMusicPerformance8_MusicToReferenceTime(perf, 100000, &a);
        HRESULT hr = IDirectMusicPerformance8_AdjustTime(perf, v[k]);
        IDirectMusicPerformance8_MusicToReferenceTime(perf, 100000, &b);
        printf("adjust %lld %08lx moved %lld\n", v[k], hr, b - a);
    }
}

static void pchinfo(void)
{
    static const DWORD p[] = { 0, 1, 15, 16, 17, 63, 64, 65, 127, 128, 1000, 0xffffffff, 0x80000000 };
    for (unsigned k = 0; k < sizeof(p) / sizeof(p[0]); k++) {
        IDirectMusicPort *port = (IDirectMusicPort *)1; DWORD g = 99, c = 99;
        HRESULT hr = IDirectMusicPerformance8_PChannelInfo(perf, p[k], NULL, &g, &c);
        printf("pchinfo %lu %08lx group %lu chan %lu\n", p[k], hr, g, c);
        (void)port;
    }
}

static const char *same(IDirectMusicSegmentState *a, IDirectMusicSegmentState *b)
{
    if (!a) return "none";
    IUnknown *ua, *ub;
    IDirectMusicSegmentState_QueryInterface(a, &IID_IUnknown, (void **)&ua);
    IDirectMusicSegmentState_QueryInterface(b, &IID_IUnknown, (void **)&ub);
    const char *r = ua == ub ? "same" : "other";
    IUnknown_Release(ua); IUnknown_Release(ub);
    return r;
}

static void segstate(void)
{
    IDirectMusicSegment8 *a = load("bells1.sgt"), *b = load("fe_hard_04.sgt");
    if (!a || !b) return;
    MUSIC_TIME la, lb;
    IDirectMusicSegment8_GetLength(a, &la);
    IDirectMusicSegment8_GetLength(b, &lb);
    IDirectMusicSegmentState *sa = NULL, *sb = NULL, *got;
    MUSIC_TIME seek = -5;
    HRESULT hr = IDirectMusicPerformance8_GetSegmentState(perf, &got, 0);
    printf("before: %08lx\n", hr);
    hr = IDirectMusicPerformance8_PlaySegment(perf, (IDirectMusicSegment *)a, 0, 0, &sa);
    printf("play a %08lx\n", hr);
    hr = IDirectMusicPerformance8_PlaySegment(perf, (IDirectMusicSegment *)b, DMUS_SEGF_QUEUE, 0, &sb);
    printf("queue b %08lx\n", hr);
    MUSIC_TIME s0, s1;
    IDirectMusicSegmentState_GetStartTime(sa, &s0);
    IDirectMusicSegmentState_GetStartTime(sb, &s1);
    printf("lengths %ld %ld, b starts %ld after a\n", la, lb, s1 - s0);
    long off[] = { -1, 0, 1, la / 2, la - 1, la, la + 1, s1 - s0 - 1, s1 - s0, s1 - s0 + lb - 1, s1 - s0 + lb, s1 - s0 + 5 * lb + 7 };
    for (unsigned k = 0; k < sizeof(off) / sizeof(off[0]); k++) {
        got = NULL;
        hr = IDirectMusicPerformance8_GetSegmentState(perf, &got, s0 + off[k]);
        printf("  at +%ld: %08lx %s/%s\n", off[k], hr, got ? same(got, sa) : "-", got ? same(got, sb) : "-");
        if (got) IDirectMusicSegmentState_Release(got);
    }
    hr = IDirectMusicSegmentState_GetSeek(sa, &seek);
    printf("seek a %08lx %s\n", hr, seek >= 0 && seek <= la ? "in range" : "out of range");
    printf("seek NULL %08lx\n", IDirectMusicSegmentState_GetSeek(sa, NULL));
    printf("getsegstate NULL %08lx\n", IDirectMusicPerformance8_GetSegmentState(perf, NULL, 0));
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(300);
    got = NULL;
    hr = IDirectMusicPerformance8_GetSegmentState(perf, &got, s0 + 1);
    printf("after stop at +1: %08lx %s\n", hr, got ? same(got, sa) : "-");
    if (got) IDirectMusicSegmentState_Release(got);
    IDirectMusicSegmentState_Release(sa); IDirectMusicSegmentState_Release(sb);
    IDirectMusicSegment8_Release(a); IDirectMusicSegment8_Release(b);
}

int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;
    WCHAR wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argc > 1 ? argv[1] : ".", -1, wdir, MAX_PATH);
    CoInitialize(NULL);
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    DWORD g = 0, c = 0;
    printf("pchinfo before init %08lx\n", IDirectMusicPerformance8_PChannelInfo(perf, 0, NULL, &g, &c));
    printf("adjust before init %08lx\n", IDirectMusicPerformance8_AdjustTime(perf, 0));
    HRESULT hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    if (FAILED(hr)) { printf("InitAudio %08lx\n", hr); return 1; }
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, wdir, FALSE);
    rhythm();
    clone();
    pchinfo();
    segstate();
    adjust();
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    IDirectMusicLoader8_Release(loader);
    CoUninitialize();
    return 0;
}
