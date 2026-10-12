/* comtest - DirectMusic COM methods with deterministic results, printed for comparison with Microsoft's DirectMusic:
 * TimeToRhythm / RhythmToTime, ClonePMsg, AdjustTime, PChannelInfo, GetSegmentState, SegmentState::GetSeek,
 * segment and performance Get/SetParam.
 * comtest <dir with segments> */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <stdio.h>
#include <dsound.h>


typedef struct { MUSIC_TIME mtTime; double dblTempo; } TEMPO_PARAM_;
#define DMUS_TEMPO_PARAM TEMPO_PARAM_
#define G(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n##_ = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
G(GUID_TempoParam, 0xd2ac28a5, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
G(GUID_TimeSignature, 0xd2ac28a4, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
G(GUID_EnableTempo, 0x45fc707e, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
G(GUID_DisableTempo, 0x45fc707d, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
G(GUID_EnableTimeSig, 0x45fc707c, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
G(GUID_DisableTimeSig, 0x45fc707b, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
G(GUID_Download, 0xd2ac28a7, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
G(GUID_Unload, 0xd2ac28a8, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
G(GUID_DownloadToAudioPath, 0x9f2c0341, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
G(GUID_UnloadFromAudioPath, 0x9f2c0342, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
G(GUID_Enable_Auto_Download, 0xd2ac28a9, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
G(GUID_Disable_Auto_Download, 0xd2ac28aa, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
G(GUID_Clear_All_Bands, 0xd2ac28ab, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
G(GUID_StandardMIDIFile, 0x06621075, 0xe92e, 0x11d1, 0xa8, 0xc5, 0x00, 0xc0, 0x4f, 0xa3, 0x72, 0x6e);
G(GUID_NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
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

static void pr_tempo(const char *what, HRESULT hr, MUSIC_TIME next, const DMUS_TEMPO_PARAM *t)
{
    if (SUCCEEDED(hr)) printf("  %s %08lx next %ld t %ld tempo %.4f\n", what, hr, next, t->mtTime, t->dblTempo);
    else printf("  %s %08lx next %ld\n", what, hr, next);
}
static void pr_ts(const char *what, HRESULT hr, MUSIC_TIME next, const DMUS_TIMESIGNATURE *s)
{
    if (SUCCEEDED(hr)) printf("  %s %08lx next %ld t %ld %u/%u grids %u\n", what, hr, next, s->mtTime, s->bBeatsPerMeasure, s->bBeat, s->wGridsPerBeat);
    else printf("  %s %08lx next %ld\n", what, hr, next);
}
static const GUID guid_bogus = { 0x12345678, 0x1234, 0x1234, { 1, 2, 3, 4, 5, 6, 7, 8 } };

static void segparams(const char *name)
{
    IDirectMusicSegment8 *s = load(name);
    if (!s) return;
    printf("params %s\n", name);
    static const long times[] = { 0, 1, 767, 768, 3072, 10000, 100000, -1 };
    static const DWORD idx[] = { 0, 1, DMUS_SEG_ANYTRACK };
    static const DWORD grp[] = { 0xffffffff, 1, 2 };
    DMUS_TEMPO_PARAM tp; DMUS_TIMESIGNATURE ts; MUSIC_TIME next;
    char w[64];
    for (unsigned g = 0; g < 3; g++)
        for (unsigned i = 0; i < 3; i++)
            for (unsigned k = 0; k < sizeof(times) / sizeof(times[0]); k++) {
                next = -7; memset(&tp, 0, sizeof(tp));
                HRESULT hr = IDirectMusicSegment8_GetParam(s, &GUID_TempoParam_, grp[g], idx[i], times[k], &next, &tp);
                sprintf(w, "tempo g%lx i%lx t%ld", grp[g], idx[i], times[k]); pr_tempo(w, hr, next, &tp);
                next = -7; memset(&ts, 0, sizeof(ts));
                hr = IDirectMusicSegment8_GetParam(s, &GUID_TimeSignature_, grp[g], idx[i], times[k], &next, &ts);
                sprintf(w, "timesig g%lx i%lx t%ld", grp[g], idx[i], times[k]); pr_ts(w, hr, next, &ts);
            }
    HRESULT hr = IDirectMusicSegment8_GetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 0, NULL, &tp);
    printf("  tempo nonext %08lx\n", hr);
    printf("  tempo NULL data %08lx\n", IDirectMusicSegment8_GetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 0, &next, NULL));
    printf("  null guid %08lx\n", IDirectMusicSegment8_GetParam(s, &GUID_NULL_, 0xffffffff, 0, 0, &next, &tp));
    printf("  bogus guid %08lx\n", IDirectMusicSegment8_GetParam(s, &guid_bogus, 0xffffffff, 0, 0, &next, &tp));
    printf("  get disable %08lx\n", IDirectMusicSegment8_GetParam(s, &GUID_DisableTempo_, 0xffffffff, 0, 0, &next, &tp));

    printf("  set bogus %08lx\n", IDirectMusicSegment8_SetParam(s, &guid_bogus, 0xffffffff, 0, 0, NULL));
    printf("  set null %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_NULL_, 0xffffffff, 0, 0, NULL));
    printf("  set null all %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_NULL_, 0xffffffff, DMUS_SEG_ALLTRACKS, 0, NULL));
    printf("  enable tempo %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_EnableTempo_, 0xffffffff, 0, 0, NULL));
    printf("  enable tempo again %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_EnableTempo_, 0xffffffff, 0, 0, NULL));
    tp.mtTime = 0; tp.dblTempo = 77.5;
    printf("  set tempo 500 %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 500, &tp));
    tp.dblTempo = 66.25;
    printf("  set tempo 0 %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 0, &tp));
    printf("  set tempo NULL %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 0, NULL));
    for (unsigned k = 0; k < 5; k++) {
        next = -7;
        hr = IDirectMusicSegment8_GetParam(s, &GUID_TempoParam_, 0xffffffff, 0, times[k] == 767 ? 499 : times[k], &next, &tp);
        sprintf(w, "after set t%ld", times[k] == 767 ? 499 : times[k]); pr_tempo(w, hr, next, &tp);
    }
    printf("  disable tempo %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_DisableTempo_, 0xffffffff, 0, 0, NULL));
    printf("  disable tempo again %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_DisableTempo_, 0xffffffff, 0, 0, NULL));
    next = -7; hr = IDirectMusicSegment8_GetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 0, &next, &tp); pr_tempo("disabled get", hr, next, &tp);
    printf("  set tempo disabled %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 0, &tp));
    printf("  enable tempo 2 %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_EnableTempo_, 0xffffffff, 0, 0, NULL));
    printf("  disable ts %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_DisableTimeSig_, 0xffffffff, 0, 0, NULL));
    printf("  disable ts again %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_DisableTimeSig_, 0xffffffff, 0, 0, NULL));
    next = -7; hr = IDirectMusicSegment8_GetParam(s, &GUID_TimeSignature_, 0xffffffff, 0, 0, &next, &ts); pr_ts("ts disabled", hr, next, &ts);
    printf("  set ts %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_TimeSignature_, 0xffffffff, 0, 0, &ts));
    printf("  enable ts %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_EnableTimeSig_, 0xffffffff, 0, 0, NULL));
    printf("  enable ts again %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_EnableTimeSig_, 0xffffffff, 0, 0, NULL));

    IDirectMusicAudioPath *ap = NULL;
    IDirectMusicPerformance8_GetDefaultAudioPath(perf, &ap);
    printf("  download perf %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Download_, 0xffffffff, 0, 0, perf));
    printf("  download perf all %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Download_, 0xffffffff, DMUS_SEG_ALLTRACKS, 0, perf));
    printf("  download ap %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_DownloadToAudioPath_, 0xffffffff, 0, 0, ap));
    printf("  download ap perf %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_DownloadToAudioPath_, 0xffffffff, 0, 0, perf));
    printf("  download NULL %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Download_, 0xffffffff, 0, 0, NULL));
    printf("  Download ap %08lx\n", IDirectMusicSegment8_Download(s, (IUnknown *)ap));
    printf("  Download perf %08lx\n", IDirectMusicSegment8_Download(s, (IUnknown *)perf));
    printf("  Unload ap %08lx\n", IDirectMusicSegment8_Unload(s, (IUnknown *)ap));
    printf("  unload perf %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Unload_, 0xffffffff, DMUS_SEG_ALLTRACKS, 0, perf));
    printf("  unload ap %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_UnloadFromAudioPath_, 0xffffffff, 0, 0, ap));
    printf("  auto on %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Enable_Auto_Download_, 0xffffffff, 0, 0, NULL));
    printf("  auto off %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Disable_Auto_Download_, 0xffffffff, DMUS_SEG_ALLTRACKS, 0, NULL));
    printf("  smf %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_StandardMIDIFile_, 0xffffffff, 0, 0, NULL));
    printf("  download group 2 %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Download_, 2, 0, 0, perf));
    printf("  download index 1 %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Download_, 0xffffffff, 1, 0, perf));
    printf("  clear bands %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Clear_All_Bands_, 0xffffffff, 0, 0, NULL));
    printf("  download cleared %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Download_, 0xffffffff, 0, 0, perf));
    printf("  Download cleared %08lx\n", IDirectMusicSegment8_Download(s, (IUnknown *)perf));
    if (ap) IDirectMusicAudioPath_Release(ap);
    IDirectMusicSegment8_Release(s);
}

static void smfdl(const char *name)
{
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
    IDirectMusicSegment8 *s = NULL;
    if (FAILED(IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&s))) return;
    printf("smf then download %s: smf %08lx", name, IDirectMusicSegment8_SetParam(s, &GUID_StandardMIDIFile_, 0xffffffff, 0, 0, NULL));
    printf(" download %08lx", IDirectMusicSegment8_Download(s, (IUnknown *)perf));
    printf(" param %08lx\n", IDirectMusicSegment8_SetParam(s, &GUID_Download_, 0xffffffff, 0, 0, perf));
    IDirectMusicSegment8_Unload(s, (IUnknown *)perf);
    IDirectMusicSegment8_Release(s);
    IDirectMusicLoader8_ClearCache(loader, &CLSID_DirectMusicSegment);
}

static void perfparams(void)
{
    DMUS_TEMPO_PARAM tp; DMUS_TIMESIGNATURE ts; MUSIC_TIME next, now;
    printf("perf params\n");
    IDirectMusicPerformance8_GetTime(perf, NULL, &now);
    next = -7; HRESULT hr = IDirectMusicPerformance8_GetParam(perf, &GUID_TimeSignature_, 0xffffffff, 0, now, &next, &ts);
    printf("  idle ts %08lx next+now %ld %u/%u grids %u\n", hr, next + now, ts.bBeatsPerMeasure, ts.bBeat, ts.wGridsPerBeat);
    next = -7; hr = IDirectMusicPerformance8_GetParam(perf, &GUID_TempoParam_, 0xffffffff, 0, now, &next, &tp);
    printf("  idle tempo %08lx next %ld\n", hr, next);
    printf("  idle set %08lx\n", IDirectMusicPerformance8_SetParam(perf, &GUID_DisableTempo_, 0xffffffff, 0, now, NULL));
    printf("  idle bogus %08lx\n", IDirectMusicPerformance8_GetParam(perf, &guid_bogus, 0xffffffff, 0, now, &next, &tp));
    IDirectMusicSegment8 *s = load("fe_hard_04.sgt");
    if (!s) return;
    IDirectMusicSegmentState *ss = NULL;
    IDirectMusicPerformance8_PlaySegment(perf, (IDirectMusicSegment *)s, 0, 0, &ss);
    MUSIC_TIME st; IDirectMusicSegmentState_GetStartTime(ss, &st);
    static const long off[] = { 0, 1, 768, 3072, 100000 };
    for (unsigned k = 0; k < 5; k++) {
        next = -7; memset(&tp, 0, sizeof(tp));
        hr = IDirectMusicPerformance8_GetParam(perf, &GUID_TempoParam_, 0xffffffff, 0, st + off[k], &next, &tp);
        printf("  tempo +%ld %08lx next %ld t %ld tempo %.4f\n", off[k], hr, next, tp.mtTime, tp.dblTempo);
        next = -7; memset(&ts, 0, sizeof(ts));
        hr = IDirectMusicPerformance8_GetParamEx(perf, &GUID_TimeSignature_, 0, 0xffffffff, 0, st + off[k], &next, &ts);
        printf("  ts +%ld %08lx next %ld t %ld %u/%u\n", off[k], hr, next, ts.mtTime, ts.bBeatsPerMeasure, ts.bBeat);
        hr = IDirectMusicPerformance8_GetParamEx(perf, &GUID_TimeSignature_, 0x7fff0000, 0xffffffff, 0, st + off[k], &next, &ts);
        printf("  ts bogus id %08lx next %ld\n", hr, next);
    }
    tp.dblTempo = 99.0;
    printf("  set tempo %08lx\n", IDirectMusicPerformance8_SetParam(perf, &GUID_TempoParam_, 0xffffffff, 0, st + 100, &tp));
    next = -7; hr = IDirectMusicSegment8_GetParam(s, &GUID_TempoParam_, 0xffffffff, 0, 100, &next, &tp);
    printf("  seg tempo 100 %08lx next %ld t %ld tempo %.4f\n", hr, next, tp.mtTime, tp.dblTempo);
    printf("  set disable %08lx\n", IDirectMusicPerformance8_SetParam(perf, &GUID_DisableTempo_, 0xffffffff, 0, st, NULL));
    printf("  set download %08lx\n", IDirectMusicPerformance8_SetParam(perf, &GUID_Download_, 0xffffffff, 0, st, perf));
    printf("  set before %08lx\n", IDirectMusicPerformance8_SetParam(perf, &GUID_EnableTempo_, 0xffffffff, 0, st - 1000, NULL));
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(200);
    IDirectMusicSegmentState_Release(ss);
    IDirectMusicSegment8_Release(s);
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
    segparams("fe_hard_04.sgt");
    segparams("bells1.sgt");
    perfparams();
    smfdl("fe_hard_04.sgt");
    smfdl("bells1.sgt");
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    IDirectMusicLoader8_Release(loader);
    CoUninitialize();
    return 0;
}
