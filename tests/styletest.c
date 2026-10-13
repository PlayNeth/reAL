/* styletest: dmstyle's chord, command and mute tracks through COM (params.sgt from tools/sty/cases.py): GetParam
 * over time, IsParamSupported, notifications, SetParam, Clone. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <dmusici.h>
#include <dmusicf.h>
static const GUID GUID_NULL_ = { 0 };

DEFINE_GUID(GUID_CommandParam2_, 0x28f97ef7, 0x9538, 0x11d2, 0x97, 0xa9, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DEFINE_GUID(GUID_CommandParamNext_, 0x472afe7a, 0x281b, 0x11d3, 0x81, 0x7d, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
#define BAR 3072

static void pkey(const char *tag, HRESULT hr, MUSIC_TIME nx, const DMUS_CHORD_KEY *k)
{
    char name[40];
    int i;
    for (i = 0; i < 16 && k->wszName[i]; i++) name[i] = (char)k->wszName[i];
    name[i] = 0;
    printf("%s: %08lx next %ld '%s' m%u b%u n%u scale %06lx key %u flags %u", tag, hr, nx, name, k->wMeasure, k->bBeat,
           k->bSubChordCount, k->dwScale, k->bKey, k->bFlags);
    for (i = 0; i < k->bSubChordCount && i < 8; i++)
        printf(" [%lx %lx %lx %lx %u %u]", k->SubChordList[i].dwChordPattern, k->SubChordList[i].dwScalePattern,
               k->SubChordList[i].dwInversionPoints, k->SubChordList[i].dwLevels, k->SubChordList[i].bChordRoot,
               k->SubChordList[i].bScaleRoot);
    printf("\n");
}

static void chords(const char *tag, IUnknown *o, int track)
{
    for (MUSIC_TIME t = -100; t < 8 * BAR + 200; t += 384) {
        DMUS_CHORD_KEY k;
        memset(&k, 0, sizeof(k));
        MUSIC_TIME nx = -1;
        HRESULT hr = track ? IDirectMusicTrack8_GetParam((IDirectMusicTrack8 *)o, &GUID_ChordParam, t, &nx, &k)
                           : IDirectMusicSegment8_GetParam((IDirectMusicSegment8 *)o, &GUID_ChordParam, 0xffffffff, 0, t, &nx, &k);
        char b[64];
        sprintf(b, "%s chord %ld", tag, t);
        pkey(b, hr, nx, &k);
    }
}

static void commands(const char *tag, IDirectMusicSegment8 *s)
{
    for (MUSIC_TIME t = -100; t < 8 * BAR + 200; t += 192) {
        DMUS_COMMAND_PARAM c;
        DMUS_COMMAND_PARAM_2 c2;
        MUSIC_TIME nx = -1, nx2 = -1;
        memset(&c, 0xee, sizeof(c));
        memset(&c2, 0xee, sizeof(c2));
        HRESULT hr = IDirectMusicSegment8_GetParam(s, &GUID_CommandParam, 0xffffffff, 0, t, &nx, &c);
        HRESULT hr2 = IDirectMusicSegment8_GetParam(s, &GUID_CommandParam2_, 0xffffffff, 0, t, &nx2, &c2);
        printf("%s cmd %ld: %08lx next %ld %u %u %u %u | %08lx next %ld t %ld %u %u %u %u\n", tag, t, hr, nx, c.bCommand,
               c.bGrooveLevel, c.bGrooveRange, c.bRepeatMode, hr2, nx2, c2.mtTime, c2.bCommand, c2.bGrooveLevel,
               c2.bGrooveRange, c2.bRepeatMode);
    }
}

static void mutes(const char *tag, IUnknown *o, int track)
{
    for (DWORD p = 0; p < 4; p++)
        for (MUSIC_TIME t = -100; t < 4 * BAR; t += 768) {
            DMUS_MUTE_PARAM m = { p, 0xeeee, 0xeeee };
            MUSIC_TIME nx = -1;
            HRESULT hr = track ? IDirectMusicTrack8_GetParam((IDirectMusicTrack8 *)o, &GUID_MuteParam, t, &nx, &m)
                               : IDirectMusicSegment8_GetParam((IDirectMusicSegment8 *)o, &GUID_MuteParam, 0xffffffff, 0, t, &nx, &m);
            printf("%s mute %lu %ld: %08lx next %ld map %lu mute %d\n", tag, p, t, hr, nx, m.dwPChannelMap, m.fMute);
        }
}

static void rhythm(IDirectMusicSegment8 *s)
{
    for (int ts = 0; ts < 3; ts++)
        for (MUSIC_TIME t = -100; t < 7 * BAR; t += 1000) {
            DMUS_RHYTHM_PARAM r;
            memset(&r, 0, sizeof(r));
            r.TimeSig.bBeatsPerMeasure = ts == 1 ? 3 : 4;
            r.TimeSig.bBeat = ts == 2 ? 0 : 4;
            r.TimeSig.wGridsPerBeat = 4;
            r.dwRhythmPattern = 0xeeee;
            MUSIC_TIME nx = -1;
            HRESULT hr = IDirectMusicSegment8_GetParam(s, &GUID_RhythmParam, 0xffffffff, 0, t, &nx, &r);
            printf("rhythm %d %ld: %08lx next %ld %lx\n", ts, t, hr, nx, r.dwRhythmPattern);
        }
}

int main(void)
{
    LoadLibraryA("dsound.dll");
    CoInitialize(NULL);
    IDirectMusicLoader8 *ld;
    IDirectMusicSegment8 *sg;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, dir, FALSE);
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, L"params.sgt", (void **)&sg);
    printf("load %08lx\n", hr);
    if (FAILED(hr)) return 1;
    chords("seg", (IUnknown *)sg, 0);
    rhythm(sg);
    commands("seg", sg);
    mutes("seg", (IUnknown *)sg, 0);

    /* the tracks themselves */
    static const GUID *const gs[] = { &GUID_ChordParam, &GUID_RhythmParam, &GUID_CommandParam, &GUID_CommandParam2_,
                                      &GUID_CommandParamNext_, &GUID_MuteParam, &GUID_TimeSignature, &GUID_NULL_,
                                      &GUID_NOTIFICATION_CHORD, &GUID_NOTIFICATION_COMMAND, &GUID_NOTIFICATION_MEASUREANDBEAT };
    static const GUID *const cls[] = { &CLSID_DirectMusicChordTrack, &CLSID_DirectMusicCommandTrack, &CLSID_DirectMusicMuteTrack };
    IDirectMusicTrack8 *tr[3] = { 0 };
    for (int c = 0; c < 3; c++) {
        IDirectMusicTrack *t = NULL;
        hr = IDirectMusicSegment8_GetTrack(sg, cls[c], 0xffffffff, 0, &t);
        printf("track %d: %08lx\n", c, hr);
        if (!t) continue;
        IDirectMusicTrack_QueryInterface(t, &IID_IDirectMusicTrack8, (void **)&tr[c]);
        IUnknown *u = NULL;
        printf("  persist %08lx", IDirectMusicTrack_QueryInterface(t, &IID_IPersistStream, (void **)&u));
        if (u) IUnknown_Release(u), u = NULL;
        printf(" persist0 %08lx\n", IDirectMusicTrack_QueryInterface(t, &IID_IPersist, (void **)&u));
        if (u) IUnknown_Release(u);
        IDirectMusicTrack_Release(t);
        for (int g = 0; g < (int)(sizeof(gs) / sizeof(gs[0])); g++)
            printf("  isparam %d %08lx add %08lx remove %08lx\n", g, IDirectMusicTrack8_IsParamSupported(tr[c], gs[g]),
                   IDirectMusicTrack8_AddNotificationType(tr[c], gs[g]), IDirectMusicTrack8_RemoveNotificationType(tr[c], gs[g]));
        BYTE junk[0x100];
        MUSIC_TIME nx;
        for (int g = 0; g < 7; g++) {
            memset(junk, 0, sizeof(junk));
            printf("  getparam %d %08lx null %08lx\n", g, IDirectMusicTrack8_GetParam(tr[c], gs[g], 0, &nx, junk),
                   IDirectMusicTrack8_GetParam(tr[c], gs[g], 0, &nx, NULL));
        }
    }
    if (tr[0]) {
        IDirectMusicTrack *cl = NULL;
        hr = IDirectMusicTrack8_Clone(tr[0], BAR + 100, 4 * BAR, &cl);
        printf("chord clone %08lx\n", hr);
        if (cl) { chords("clone", (IUnknown *)cl, 1); IDirectMusicTrack_Release(cl); }
        hr = IDirectMusicTrack8_Clone(tr[0], BAR, 3 * BAR, &cl);
        printf("chord clone2 %08lx\n", hr);
        if (cl) { chords("clone2", (IUnknown *)cl, 1); IDirectMusicTrack_Release(cl); }
        DMUS_CHORD_KEY k;
        memset(&k, 0, sizeof(k));
        lstrcpyW(k.wszName, L"set");
        k.wMeasure = 9; k.bBeat = 3; k.bSubChordCount = 2; k.dwScale = 0x123456; k.bKey = 30; k.bFlags = 3;
        k.SubChordList[0].dwChordPattern = 0x91; k.SubChordList[0].bChordRoot = 4; k.SubChordList[0].dwLevels = 1;
        k.SubChordList[1].dwChordPattern = 0x89; k.SubChordList[1].bChordRoot = 5; k.SubChordList[1].dwLevels = 2;
        printf("set chord %08lx\n", IDirectMusicTrack8_SetParam(tr[0], &GUID_ChordParam, BAR, &k));
        printf("set chord2 %08lx\n", IDirectMusicTrack8_SetParam(tr[0], &GUID_ChordParam, 100, &k));
        printf("set chord3 %08lx\n", IDirectMusicTrack8_SetParam(tr[0], &GUID_ChordParam, 20 * BAR, &k));
        printf("set rhythm %08lx\n", IDirectMusicTrack8_SetParam(tr[0], &GUID_RhythmParam, 0, &k));
        chords("set", (IUnknown *)tr[0], 1);
    }
    if (tr[1]) {
        DMUS_COMMAND_PARAM c = { DMUS_COMMANDT_FILL, 33, 4, 2 };
        printf("set cmd %08lx %08lx\n", IDirectMusicTrack8_SetParam(tr[1], &GUID_CommandParam, BAR + 1536, &c),
               IDirectMusicTrack8_SetParam(tr[1], &GUID_CommandParam, 5 * BAR, &c));
        DMUS_COMMAND_PARAM_2 c2 = { 77, 1, 2, 3, 4 };
        MUSIC_TIME nx = -1;
        printf("next get %08lx", IDirectMusicTrack8_GetParam(tr[1], &GUID_CommandParamNext_, 0, &nx, &c2));
        printf(" set %08lx", IDirectMusicTrack8_SetParam(tr[1], &GUID_CommandParamNext_, 0, &c2));
        memset(&c2, 0, sizeof(c2));
        printf(" get %08lx next %ld %ld %u %u %u %u", IDirectMusicTrack8_GetParam(tr[1], &GUID_CommandParamNext_, 99, &nx, &c2),
               nx, c2.mtTime, c2.bCommand, c2.bGrooveLevel, c2.bGrooveRange, c2.bRepeatMode);
        printf(" clear %08lx get %08lx\n", IDirectMusicTrack8_SetParam(tr[1], &GUID_CommandParamNext_, 0, NULL),
               IDirectMusicTrack8_GetParam(tr[1], &GUID_CommandParamNext_, 99, &nx, &c2));
        commands("setcmd", sg);
        IDirectMusicTrack *cl = NULL;
        hr = IDirectMusicTrack8_Clone(tr[1], BAR + 100, 4 * BAR, &cl);
        printf("cmd clone %08lx\n", hr);
        if (cl) {
            for (MUSIC_TIME t = -100; t < 4 * BAR; t += 192) {
                DMUS_COMMAND_PARAM_2 x;
                memset(&x, 0xee, sizeof(x));
                hr = IDirectMusicTrack_GetParam(cl, &GUID_CommandParam2_, t, &nx, &x);
                printf("clone cmd %ld: %08lx next %ld t %ld %u %u %u %u\n", t, hr, nx, x.mtTime, x.bCommand, x.bGrooveLevel,
                       x.bGrooveRange, x.bRepeatMode);
            }
            IDirectMusicTrack_Release(cl);
        }
    }
    if (tr[2]) {
        DMUS_MUTE_PARAM m = { 3, 7, FALSE };
        printf("set mute %08lx", IDirectMusicTrack8_SetParam(tr[2], &GUID_MuteParam, 2 * BAR, &m));
        m.dwPChannel = 0; m.dwPChannelMap = 9; m.fMute = TRUE;
        printf(" %08lx\n", IDirectMusicTrack8_SetParam(tr[2], &GUID_MuteParam, 100, &m));
        mutes("setmute", (IUnknown *)tr[2], 1);
        IDirectMusicTrack *cl = NULL;
        hr = IDirectMusicTrack8_Clone(tr[2], BAR / 2, 3 * BAR, &cl);
        printf("mute clone %08lx\n", hr);
        if (cl) { mutes("clone", (IUnknown *)cl, 1); IDirectMusicTrack_Release(cl); }
    }
    for (int c = 0; c < 3; c++) if (tr[c]) IDirectMusicTrack8_Release(tr[c]);
    IDirectMusicSegment8_Release(sg);
    IDirectMusicLoader8_Release(ld);
    return 0;
}
