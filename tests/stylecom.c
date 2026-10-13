/* stylecom: IDirectMusicStyle8 and IDirectMusicBand through the loader, and a style track's parameters
 * (comstyle.sgt / comstyle.sty / mix1.sty from tools/sty/cases.py in the current directory). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <dmusici.h>
#include <dmusicf.h>

DEFINE_GUID(GUID_SeedVariations_, 0x65b76fa5, 0xff37, 0x11d2, 0x81, 0x4e, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DEFINE_GUID(GUID_IDirectMusicStyle_, 0xd2ac28a1, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static const GUID GUID_NULL_ = { 0 };
#define BAR 3072

static void wout(const WCHAR *w)
{
    for (int i = 0; w[i] && i < 300; i++) putchar(w[i] < 128 ? (char)w[i] : '?');
}

static void desc(const char *tag, IUnknown *u)
{
    IDirectMusicObject *o = NULL;
    HRESULT hr = IUnknown_QueryInterface(u, &IID_IDirectMusicObject, (void **)&o);
    printf("%s object %08lx", tag, hr);
    if (o) {
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        hr = IDirectMusicObject_GetDescriptor(o, &d);
        printf(" desc %08lx valid %lx class %08lx name '", hr, d.dwValidData & ~DMUS_OBJ_FILENAME & ~DMUS_OBJ_FULLPATH,
               d.guidClass.Data1);
        wout(d.wszName);
        printf("' cat '");
        wout(d.wszCategory);
        printf("' obj %08lx vers %lx.%lx", d.guidObject.Data1, d.vVersion.dwVersionMS, d.vVersion.dwVersionLS);
        IDirectMusicObject_Release(o);
    }
    IPersistStream *ps = NULL;
    hr = IUnknown_QueryInterface(u, &IID_IPersistStream, (void **)&ps);
    printf(" persist %08lx", hr);
    if (ps) {
        CLSID c;
        hr = IPersistStream_GetClassID(ps, &c);
        printf(" class %08lx %08lx", hr, c.Data1);
        IPersistStream_Release(ps);
    }
    printf("\n");
}

static void style(IDirectMusicStyle8 *s)
{
    DMUS_TIMESIGNATURE ts;
    memset(&ts, 0xee, sizeof(ts));
    HRESULT hr = IDirectMusicStyle8_GetTimeSignature(s, &ts);
    printf("timesig %08lx %ld %u/%u %u\n", hr, ts.mtTime, ts.bBeatsPerMeasure, ts.bBeat, ts.wGridsPerBeat);
    double t = -1;
    hr = IDirectMusicStyle8_GetTempo(s, &t);
    printf("tempo %08lx %.3f\n", hr, t);
    WCHAR w[0x104];
    for (DWORD i = 0; i < 4; i++) {
        memset(w, 0, sizeof(w));
        hr = IDirectMusicStyle8_EnumBand(s, i, w);
        printf("band %lu %08lx '", i, hr); wout(w); printf("'\n");
    }
    for (DWORD i = 0; i < 4; i++) {
        memset(w, 0, sizeof(w));
        hr = IDirectMusicStyle8_EnumMotif(s, i, w);
        printf("motif %lu %08lx '", i, hr); wout(w); printf("'\n");
    }
    for (DWORD k = 0; k < 3; k++)
        for (DWORD i = 0; i < 12; i++) {
            memset(w, 0, sizeof(w));
            hr = IDirectMusicStyle8_EnumPattern(s, i, k, w);
            printf("pattern %lu %lu %08lx '", k, i, hr); wout(w); printf("'\n");
        }
    for (DWORD i = 0; i < 4; i++) {
        memset(w, 0, sizeof(w));
        hr = IDirectMusicStyle8_EnumChordMap(s, i, w);
        printf("chordmap %lu %08lx\n", i, hr);
    }
    static const DWORD types[] = { 0, 1, 2, 3, 4, 5, 7, 8, 0x10 };
    for (unsigned k = 0; k < sizeof(types) / sizeof(types[0]); k++)
        for (DWORD lv = 0; lv <= 100; lv += 25) {
            DWORD mn = 77, mx = 77;
            hr = IDirectMusicStyle8_GetEmbellishmentLength(s, types[k], lv, &mn, &mx);
            printf("emblen %lu %lu: %08lx %lu %lu\n", types[k], lv, hr, mn, mx);
        }
    IDirectMusicBand *b = NULL;
    hr = IDirectMusicStyle8_GetDefaultBand(s, &b);
    printf("default band %08lx %s\n", hr, b ? "yes" : "no");
    if (b) {
        desc("default band", (IUnknown *)b);
        IDirectMusicSegment *sg = NULL;
        hr = IDirectMusicBand_CreateSegment(b, &sg);
        printf("CreateSegment %08lx\n", hr);
        if (sg) {
            MUSIC_TIME len = -1;
            DWORD rep = 99;
            IDirectMusicSegment_GetLength(sg, &len);
            IDirectMusicSegment_GetRepeats(sg, &rep);
            printf("band segment length %ld repeats %lu\n", len, rep);
            for (DWORD i = 0; i < 3; i++) {
                IDirectMusicTrack *tr = NULL;
                hr = IDirectMusicSegment_GetTrack(sg, &GUID_NULL_, 0xffffffff, i, &tr);
                DWORD grp = 0;
                if (tr) IDirectMusicSegment_GetTrackGroup(sg, tr, &grp);
                printf("  track %lu %08lx group %lx", i, hr, grp);
                if (tr) {
                    IPersistStream *ps = NULL;
                    if (SUCCEEDED(IDirectMusicTrack_QueryInterface(tr, &IID_IPersistStream, (void **)&ps))) {
                        CLSID c;
                        IPersistStream_GetClassID(ps, &c);
                        printf(" class %08lx", c.Data1);
                        IPersistStream_Release(ps);
                    }
                    static const GUID *const ps2[] = { &GUID_BandParam, &GUID_IDirectMusicBand, &GUID_Download, &GUID_Enable_Auto_Download };
                    for (unsigned k = 0; k < 4; k++) printf(" %08lx", IDirectMusicTrack_IsParamSupported(tr, ps2[k]));
                    IDirectMusicTrack_Release(tr);
                }
                printf("\n");
            }
            IDirectMusicSegment_Release(sg);
        }
        IDirectMusicBand_Release(b);
    }
    static const WCHAR *const names[] = { L"Default Band", L"second band", L"Third", L"" };
    for (int i = 0; i < 4; i++) {
        b = NULL;
        hr = IDirectMusicStyle8_GetBand(s, (WCHAR *)names[i], &b);
        printf("GetBand %d %08lx %s\n", i, hr, b ? "yes" : "no");
        if (b) IDirectMusicBand_Release(b);
    }
    IDirectMusicChordMap *cm = (void *)1;
    hr = IDirectMusicStyle8_GetDefaultChordMap(s, &cm);
    printf("default chordmap %08lx %s\n", hr, cm == (void *)1 ? "untouched" : cm ? "yes" : "no");
}

int main(void)
{
    LoadLibraryA("dsound.dll");
    CoInitialize(NULL);
    IDirectMusicLoader8 *ld;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, dir, FALSE);
    IDirectMusicStyle8 *st = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"comstyle.sty", (void **)&st);
    printf("load style %08lx\n", hr);
    if (st) { desc("style", (IUnknown *)st); style(st); }
    IDirectMusicStyle *st7 = NULL;
    if (st) printf("QI IDirectMusicStyle %08lx\n", IDirectMusicStyle8_QueryInterface(st, &IID_IDirectMusicStyle, (void **)&st7));
    if (st7) IDirectMusicStyle_Release(st7);

    IDirectMusicSegment8 *sg = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, L"comstyle.sgt", (void **)&sg);
    printf("load segment %08lx\n", hr);
    if (sg) {
        for (MUSIC_TIME t = -10; t < 9 * BAR; t += BAR / 2) {
            IDirectMusicStyle8 *s = NULL;
            MUSIC_TIME nx = -1;
            hr = IDirectMusicSegment8_GetParam(sg, &GUID_IDirectMusicStyle_, 0xffffffff, 0, t, &nx, &s);
            printf("style at %ld: %08lx next %ld %s", t, hr, nx, !s ? "none" : s == st ? "the loaded one" : "another");
            if (s) {
                double tp = 0;
                IDirectMusicStyle8_GetTempo(s, &tp);
                printf(" tempo %.1f", tp);
                IDirectMusicStyle8_Release(s);
            }
            DMUS_TIMESIGNATURE ts;
            memset(&ts, 0xee, sizeof(ts));
            nx = -1;
            hr = IDirectMusicSegment8_GetParam(sg, &GUID_TimeSignature, 0xffffffff, 0, t, &nx, &ts);
            printf(" | timesig %08lx next %ld %ld %u/%u %u\n", hr, nx, ts.mtTime, ts.bBeatsPerMeasure, ts.bBeat, ts.wGridsPerBeat);
        }
        IDirectMusicTrack *tr = NULL;
        hr = IDirectMusicSegment8_GetTrack(sg, &CLSID_DirectMusicStyleTrack, 0xffffffff, 0, &tr);
        printf("style track %08lx\n", hr);
        if (tr) {
            static const GUID *const gs[] = { &GUID_IDirectMusicStyle_, &GUID_SeedVariations_, &GUID_TimeSignature,
                                              &GUID_EnableTimeSig, &GUID_DisableTimeSig, &GUID_ChordParam, &GUID_NULL_ };
            for (unsigned k = 0; k < sizeof(gs) / sizeof(gs[0]); k++)
                printf("  supported %u %08lx\n", k, IDirectMusicTrack_IsParamSupported(tr, gs[k]));
            DWORD seed = 1234;
            printf("  seed %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_SeedVariations_, 0, &seed));
            printf("  seed null %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_SeedVariations_, 0, NULL));
            printf("  disable %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_DisableTimeSig, 0, NULL));
            printf("  disable %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_DisableTimeSig, 0, NULL));
            for (unsigned k = 2; k < 5; k++) printf("  supported %u %08lx\n", k, IDirectMusicTrack_IsParamSupported(tr, gs[k]));
            printf("  enable %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_EnableTimeSig, 0, NULL));
            printf("  enable %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_EnableTimeSig, 0, NULL));
            for (unsigned k = 2; k < 5; k++) printf("  supported %u %08lx\n", k, IDirectMusicTrack_IsParamSupported(tr, gs[k]));
            printf("  chord set %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_ChordParam, 0, &seed));
            printf("  notify mb %08lx\n", IDirectMusicTrack_AddNotificationType(tr, &GUID_NOTIFICATION_MEASUREANDBEAT));
            printf("  notify chord %08lx\n", IDirectMusicTrack_AddNotificationType(tr, &GUID_NOTIFICATION_CHORD));
            printf("  remove mb %08lx\n", IDirectMusicTrack_RemoveNotificationType(tr, &GUID_NOTIFICATION_MEASUREANDBEAT));
            if (st) {
                printf("  set style %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_IDirectMusicStyle_, 2 * BAR, st));
                for (MUSIC_TIME t = 0; t < 9 * BAR; t += BAR) {
                    IDirectMusicStyle8 *s = NULL;
                    MUSIC_TIME nx = -1;
                    hr = IDirectMusicTrack_GetParam(tr, &GUID_IDirectMusicStyle_, t, &nx, &s);
                    printf("  track style at %ld: %08lx next %ld %s\n", t, hr, nx, !s ? "none" : s == st ? "the loaded one" : "another");
                    if (s) IDirectMusicStyle8_Release(s);
                }
            }
            IDirectMusicTrack *cl = NULL;
            hr = IDirectMusicTrack_Clone(tr, BAR, 6 * BAR, &cl);
            printf("  clone %08lx\n", hr);
            if (cl) {
                for (MUSIC_TIME t = 0; t < 6 * BAR; t += BAR) {
                    IDirectMusicStyle8 *s = NULL;
                    MUSIC_TIME nx = -1;
                    hr = IDirectMusicTrack_GetParam(cl, &GUID_IDirectMusicStyle_, t, &nx, &s);
                    printf("  clone style at %ld: %08lx next %ld %s\n", t, hr, nx, !s ? "none" : s == st ? "the loaded one" : "another");
                    if (s) IDirectMusicStyle8_Release(s);
                }
                IDirectMusicTrack_Release(cl);
            }
            IDirectMusicTrack_Release(tr);
        }
        IDirectMusicSegment8_Release(sg);
    }
    IDirectMusicBand *bd = NULL;
    hr = CoCreateInstance(&CLSID_DirectMusicBand, NULL, CLSCTX_INPROC, &IID_IDirectMusicBand, (void **)&bd);
    printf("create band %08lx\n", hr);
    if (bd) { desc("new band", (IUnknown *)bd); IDirectMusicBand_Release(bd); }
    IDirectMusicStyle8 *ns = NULL;
    hr = CoCreateInstance(&CLSID_DirectMusicStyle, NULL, CLSCTX_INPROC, &IID_IDirectMusicStyle8, (void **)&ns);
    printf("create style %08lx\n", hr);
    if (ns) {
        IDirectMusicObject *o = NULL;
        IDirectMusicStyle8_QueryInterface(ns, &IID_IDirectMusicObject, (void **)&o);
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        if (o) { printf("new style desc %08lx", IDirectMusicObject_GetDescriptor(o, &d)); IDirectMusicObject_Release(o); }
        printf(" valid %lx\n", d.dwValidData);     /* (Microsoft's object GUID is uninitialised) */
        IDirectMusicStyle8_Release(ns);
    }
    if (st) IDirectMusicStyle8_Release(st);
    IDirectMusicLoader8_Release(ld);
    return 0;
}
