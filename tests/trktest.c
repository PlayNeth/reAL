/* trktest - dmime's lyrics, segment trigger and parameter control tracks, printed for comparison with Microsoft's
 * DirectMusic: the tracks' parameters and Clone, and the messages a tool in the performance's graph sees while
 * segments with them play.  trktest [lyrics|trigger] (tools/sty/cases.py tracks writes the segments); trktest standalone
 * [files.sgt]: track objects made with CoCreateInstance, loaded from their chunks, played in a segment and directly */
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
static int g_nosegnotes;      /* (the host segment's own, when tracks play directly) */
static int g_nogroup;         /* (a sequence track played directly: its group is whatever was on dmime's stack) */
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
        if (g_nosegnotes && e->type == DMUS_PMSGT_NOTIFICATION && (e->extra & ~0xffu) == (GUID_NOTIFICATION_SEGMENT.Data1 & ~0xffu)) continue;
        if (g_nogroup) printf("  %6ld type %lu pch %lu flags %lx group ? %lx '%ls'\n", e->mt, e->type, e->pch, e->flags, e->extra, e->text);
        else printf("  %6ld type %lu pch %lu flags %lx group %lx %lx '%ls'\n", e->mt, e->type, e->pch, e->flags, e->group, e->extra, e->text);
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

/* the probe's log (FXPROBE_LOG), its times relative to rt0 (in ticks at 125 bpm where they divide), without
 * Process lines */
static long g_logpos[8];
static REFERENCE_TIME g_rt0[8];
static int g_nplays;
static long probe_size(void)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    const char *path = getenv("FXPROBE_LOG");
    return path && GetFileAttributesExA(path, GetFileExInfoStandard, &a) ? (long)a.nFileSizeLow : 0;
}
static void dump_probe(void)
{
    const char *path = getenv("FXPROBE_LOG");
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) { printf("no probe log\n"); return; }
    char line[512];
    int k = -1;
    REFERENCE_TIME rt0 = 0;
    while (fgets(line, sizeof(line), f)) {
        long pos = ftell(f) - (long)strlen(line);
        while (k + 1 < g_nplays && pos >= g_logpos[k + 1]) { k++; rt0 = g_rt0[k]; printf("-- play %d\n", k); }
        if (!strncmp(line, "Process", 7)) continue;
        if (!strncmp(line, "released after", 14)) { printf("released\n"); continue; }   /* (Process calls: timing) */
        char out[600], *o = out, *p = line;
        int inb = 0;
        while (*p) {
            if (*p == ']') inb = 0;
            if (*p == '[' || (inb && *p == ' ' && p[-1] == ',')) {
                inb = 1;
                *o++ = *p++;
                char *e;
                long long v = _strtoi64(p, &e, 10);
                if (e != p) {
                    if (v == (long long)0x8000000000000000ULL) o += sprintf(o, "min");
                    else if (v == 0x7fffffffffffffffLL) o += sprintf(o, "max");
                    else if ((v - rt0) % 6250 == 0) o += sprintf(o, "t%lld", (v - rt0) / 6250);
                    else o += sprintf(o, "t%lld+%lld", (v - rt0) / 6250, (v - rt0) % 6250);
                    p = e;
                }
                continue;
            }
            *o++ = *p++;
        }
        *o = 0;
        fputs(out, stdout);
    }
    fclose(f);
}

static void show_desc(const char *what, HRESULT hr, const DMUS_OBJECTDESC *d, const WCHAR *alias)
{
    const WCHAR *f = d->wszFileName, *b = f;
    for (const WCHAR *p = f; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    printf("%s %08lx valid %lx class %08lx guid %08lx name '%ls' file '%ls'%s", what, hr, d->dwValidData, d->guidClass.Data1,
           (d->dwValidData & DMUS_OBJ_OBJECT) ? d->guidObject.Data1 : 0, (d->dwValidData & DMUS_OBJ_NAME) ? d->wszName : L"",
           (d->dwValidData & DMUS_OBJ_FILENAME) ? b : L"", (d->dwValidData & DMUS_OBJ_STREAM) ? " stream" : "");
    if (alias) printf(" alias '%ls'", alias);
    printf("\n");
}
static void enum_container(IDirectMusicContainer *c, REFGUID cls, const char *what)
{
    for (DWORD k = 0; k < 8; k++) {
        DMUS_OBJECTDESC d; WCHAR alias[DMUS_MAX_NAME];
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d); alias[0] = 0;
        HRESULT hr = IDirectMusicContainer_EnumObject(c, cls, k, &d, alias);
        char b[32]; snprintf(b, sizeof(b), "  %s %lu", what, k);
        if (hr != S_OK) { printf("%s %08lx\n", b, hr); break; }
        show_desc(b, hr, &d, alias);
    }
}
static void enum_loader(REFGUID cls, const char *what)
{
    for (DWORD k = 0; k < 12; k++) {
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        HRESULT hr = IDirectMusicLoader8_EnumObject(loader, cls, k, &d);
        char b[32]; snprintf(b, sizeof(b), "  %s %lu", what, k);
        if (hr != S_OK) { printf("%s %08lx\n", b, hr); break; }
        show_desc(b, hr, &d, NULL);
    }
}
static HRESULT get_by(REFGUID cls, const GUID *g, const WCHAR *name, const WCHAR *file, REFIID iid, void **o, const char *what)
{
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS; d.guidClass = *cls;
    if (g) { d.guidObject = *g; d.dwValidData |= DMUS_OBJ_OBJECT; }
    if (name) { lstrcpyW(d.wszName, name); d.dwValidData |= DMUS_OBJ_NAME; }
    if (file) { lstrcpyW(d.wszFileName, file); d.dwValidData |= DMUS_OBJ_FILENAME; }
    *o = NULL;
    HRESULT hr = IDirectMusicLoader8_GetObject(loader, &d, iid, o);
    printf("get %s %08lx\n", what, hr);
    if (*o) {
        IDirectMusicObject *ob = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)*o, &IID_IDirectMusicObject, (void **)&ob))) {
            DMUS_OBJECTDESC x; memset(&x, 0, sizeof(x)); x.dwSize = sizeof(x);
            HRESULT h2 = IDirectMusicObject_GetDescriptor(ob, &x);
            show_desc("    desc", h2, &x, NULL);
            IDirectMusicObject_Release(ob);
        }
    }
    return hr;
}

/* plays a segment on an audiopath from a start point, until the music time is `len` past its start; rt0 gets the
 * reference time of the segment's time 0 */
static void play_on(IDirectMusicSegment8 *sg, IDirectMusicAudioPath *ap, MUSIC_TIME len, REFERENCE_TIME *rt0)
{
    REFERENCE_TIME now = 0;
    IDirectMusicPerformance8_GetTime(perf, &now, NULL);
    IDirectMusicSegmentState *ss = NULL;
    g_logpos[g_nplays] = probe_size();
    HRESULT hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)sg, NULL, NULL, DMUS_SEGF_REFTIME, now + 10000000, &ss, NULL, (IUnknown *)ap);
    printf("play %08lx\n", hr);
    if (!ss) return;
    MUSIC_TIME st = 0, sp = 0;
    IDirectMusicSegmentState_GetStartTime(ss, &st);
    IDirectMusicSegmentState_GetStartPoint(ss, &sp);
    IDirectMusicPerformance8_MusicToReferenceTime(perf, st - sp, rt0);
    g_rt0[g_nplays++] = *rt0;
    MUSIC_TIME mt = 0;
    DWORD deadline = GetTickCount() + 60000;
    do { Sleep(20); IDirectMusicPerformance8_GetTime(perf, NULL, &mt); } while (mt < st + len && (LONG)(deadline - GetTickCount()) > 0);
    if (getenv("TRK_TIMING")) fprintf(stderr, "waited %lu ms\n", (unsigned long)(GetTickCount() - (deadline - 60000)));
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    if (getenv("TRK_TIMING")) fprintf(stderr, "stopped\n");
    Sleep(300);
    IDirectMusicSegmentState_Release(ss);
    if (!getenv("TRK_KEEPSTATES")) { for (int k = 0; k < nstates; k++) IDirectMusicSegmentState_Release(states[k]); nstates = 0; }
    if (getenv("TRK_TIMING")) fprintf(stderr, "released\n");
}


/* ------------------------------------------------------------------ standalone tracks */
#define TG(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { #n, { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } } }
static const struct { const char *name; GUID g; } g_tracks[] = {
    TG(tempo, 0xd2ac2885, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(seq, 0xd2ac2886, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(sysex, 0xd2ac2887, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(timesig, 0xd2ac2888, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(chord, 0xd2ac288b, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(command, 0xd2ac288c, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(style, 0xd2ac288d, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(motif, 0xd2ac288e, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(band, 0xd2ac2894, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(chordmap, 0xd2ac2896, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(pattern, 0xd2ac2897, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(mute, 0xd2ac2898, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(marker, 0x55a8fd00, 0x4288, 0x11d3, 0x9b, 0xd1, 0x8a, 0x0d, 0x61, 0xc8, 0x88, 0x35),
    TG(signpost, 0xf17e8672, 0xc3b4, 0x11d1, 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd),
    TG(lyrics, 0x995c1cf5, 0x54ff, 0x11d3, 0x8b, 0xda, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6),
    TG(segtrigger, 0xbae4d665, 0x4ea1, 0x11d3, 0x8b, 0xda, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6),
    TG(paramctl, 0x4be0537b, 0x5c19, 0x11d3, 0x8b, 0xdc, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6),
    TG(wave, 0xeed36461, 0x9ea5, 0x11d3, 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74),
};
static const char *track_name(REFGUID g)
{
    for (size_t k = 0; k < sizeof(g_tracks) / sizeof(*g_tracks); k++) if (IsEqualGUID(g, &g_tracks[k].g)) return g_tracks[k].name;
    return "?";
}

/* a stream on memory that hands out the loader (IDirectMusicGetLoader), as the loader's own streams do */
typedef struct { IStream IStream_iface; IDirectMusicGetLoader gl; LONG ref; IStream *in; } gstream;
#define GS(i) CONTAINING_RECORD(i, gstream, IStream_iface)
#define GL(i) CONTAINING_RECORD(i, gstream, gl)
static HRESULT WINAPI gs_qi(IStream *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IStream) || IsEqualIID(r, &IID_ISequentialStream)) *o = i;
    else if (IsEqualIID(r, &IID_IDirectMusicGetLoader)) *o = &GS(i)->gl;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&GS(i)->ref);
    return S_OK;
}
static ULONG WINAPI gs_addref(IStream *i) { return InterlockedIncrement(&GS(i)->ref); }
static ULONG WINAPI gs_release(IStream *i)
{
    LONG r = InterlockedDecrement(&GS(i)->ref);
    if (!r) { IStream_Release(GS(i)->in); free(GS(i)); }
    return r;
}
static HRESULT WINAPI gs_read(IStream *i, void *p, ULONG n, ULONG *g) { return IStream_Read(GS(i)->in, p, n, g); }
static HRESULT WINAPI gs_write(IStream *i, const void *p, ULONG n, ULONG *g) { return IStream_Write(GS(i)->in, p, n, g); }
static HRESULT WINAPI gs_seek(IStream *i, LARGE_INTEGER d, DWORD o, ULARGE_INTEGER *n) { return IStream_Seek(GS(i)->in, d, o, n); }
static HRESULT WINAPI gs_setsize(IStream *i, ULARGE_INTEGER n) { return E_NOTIMPL; }
static HRESULT WINAPI gs_copyto(IStream *i, IStream *t, ULARGE_INTEGER n, ULARGE_INTEGER *a, ULARGE_INTEGER *b) { return E_NOTIMPL; }
static HRESULT WINAPI gs_commit(IStream *i, DWORD f) { return E_NOTIMPL; }
static HRESULT WINAPI gs_revert(IStream *i) { return E_NOTIMPL; }
static HRESULT WINAPI gs_lock(IStream *i, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD c) { return E_NOTIMPL; }
static HRESULT WINAPI gs_stat(IStream *i, STATSTG *s, DWORD f) { return IStream_Stat(GS(i)->in, s, f); }
static IStream *gstream_new(const BYTE *p, DWORD n, BOOL withloader);
static HRESULT WINAPI gs_clone(IStream *i, IStream **o)
{
    IStream *c = NULL;
    HRESULT hr = IStream_Clone(GS(i)->in, &c);
    if (FAILED(hr)) return hr;
    gstream *g = calloc(1, sizeof(*g));
    *g = *GS(i);
    g->ref = 1; g->in = c;
    *o = &g->IStream_iface;
    return S_OK;
}
static IStreamVtbl gs_vtbl = { gs_qi, gs_addref, gs_release, gs_read, gs_write, gs_seek, gs_setsize, gs_copyto, gs_commit,
                               gs_revert, gs_lock, gs_lock, gs_stat, gs_clone };
static HRESULT WINAPI gl_qi(IDirectMusicGetLoader *i, REFIID r, void **o) { return gs_qi(&GL(i)->IStream_iface, r, o); }
static ULONG WINAPI gl_addref(IDirectMusicGetLoader *i) { return gs_addref(&GL(i)->IStream_iface); }
static ULONG WINAPI gl_release(IDirectMusicGetLoader *i) { return gs_release(&GL(i)->IStream_iface); }
static HRESULT WINAPI gl_get(IDirectMusicGetLoader *i, IDirectMusicLoader **o)
{
    return IDirectMusicLoader8_QueryInterface(loader, &IID_IDirectMusicLoader, (void **)o);
}
static IDirectMusicGetLoaderVtbl gl_vtbl = { gl_qi, gl_addref, gl_release, gl_get };
static IStream *gstream_new(const BYTE *p, DWORD n, BOOL withloader)
{
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
    memcpy(GlobalLock(h), p, n);
    GlobalUnlock(h);
    IStream *in = NULL;
    CreateStreamOnHGlobal(h, TRUE, &in);
    if (!withloader) return in;
    gstream *g = calloc(1, sizeof(*g));
    g->IStream_iface.lpVtbl = &gs_vtbl; g->gl.lpVtbl = &gl_vtbl; g->ref = 1; g->in = in;
    return &g->IStream_iface;
}

/* what a track says of itself: interfaces, class, the parameters it supports, a few GetParams, Clone */
static void track_show(IDirectMusicTrack *t, const char *what)
{
    static const struct { const char *n; const GUID *g; } ps[] = {
        { "tempo", &GUID_TempoParam }, { "timesig", &GUID_TimeSignature }, { "disabletempo", &GUID_DisableTempo },
        { "enabletempo", &GUID_EnableTempo }, { "disablets", &GUID_DisableTimeSig }, { "enablets", &GUID_EnableTimeSig },
        { "chord", &GUID_ChordParam }, { "rhythm", &GUID_RhythmParam }, { "command", &GUID_CommandParam },
        { "style", &GUID_IDirectMusicStyle }, { "chordmap", &GUID_IDirectMusicChordMap }, { "band", &GUID_BandParam },
        { "iband", &GUID_IDirectMusicBand }, { "mute", &GUID_MuteParam }, { "download", &GUID_Download },
        { "connectdls", &GUID_ConnectToDLSCollection }, { "validstart", &GUID_Valid_Start_Time },
        { "playmarker", &GUID_Play_Marker }, { "variations", &GUID_Variations }, { "seqvar", &GUID_SeedVariations },
        { "stdnotes", &GUID_StandardMIDIFile }, { "clearall", &GUID_Clear_All_Bands }, { "autodl", &GUID_Enable_Auto_Download },
        { "mastertempo", &GUID_PerfMasterTempo },
    };
    IUnknown *u = NULL;
    printf("%s\n", what);
    printf("  qi track8 %08lx", IDirectMusicTrack_QueryInterface(t, &IID_IDirectMusicTrack8, (void **)&u)); if (u) { IUnknown_Release(u); u = NULL; }
    printf(" persist %08lx", IDirectMusicTrack_QueryInterface(t, &IID_IPersistStream, (void **)&u));
    if (u) { CLSID c; HRESULT h = IPersistStream_GetClassID((IPersistStream *)u, &c); printf(" classid %08lx %s", h, SUCCEEDED(h) ? track_name(&c) : "-"); IUnknown_Release(u); u = NULL; }
    printf(" object %08lx\n", IDirectMusicTrack_QueryInterface(t, &IID_IDirectMusicObject, (void **)&u)); if (u) { IUnknown_Release(u); u = NULL; }
    printf("  params");
    for (size_t k = 0; k < sizeof(ps) / sizeof(*ps); k++) {
        HRESULT h = IDirectMusicTrack_IsParamSupported(t, ps[k].g);
        if (h == S_OK) printf(" %s", ps[k].n);
        else if (h != DMUS_E_TYPE_UNSUPPORTED) printf(" %s:%08lx", ps[k].n, h);
    }
    printf("\n");
    BYTE buf[512];
    MUSIC_TIME nx = -1;
    memset(buf, 0, sizeof(buf));
    HRESULT h = IDirectMusicTrack_GetParam(t, &GUID_TempoParam, 0, &nx, buf);
    printf("  tempo %08lx", h);
    if (h == S_OK) printf(" %ld %.3f next %ld", *(MUSIC_TIME *)buf, *(double *)(buf + 8), nx);
    nx = -1; memset(buf, 0, sizeof(buf));
    h = IDirectMusicTrack_GetParam(t, &GUID_TimeSignature, 0, &nx, buf);
    printf(" timesig %08lx", h);
    if (h == S_OK) { DMUS_TIMESIGNATURE *ts = (DMUS_TIMESIGNATURE *)buf; printf(" %ld %u/%u %u next %ld", ts->mtTime, ts->bBeatsPerMeasure, ts->bBeat, ts->wGridsPerBeat, nx); }
    nx = -1; memset(buf, 0, sizeof(buf));
    h = IDirectMusicTrack_GetParam(t, &GUID_Valid_Start_Time, 0, &nx, buf);
    printf(" validstart %08lx", h);
    if (h == S_OK) printf(" %ld next %ld", ((DMUS_VALID_START_PARAM *)buf)->mtTime, nx);
    printf("\n");
    IDirectMusicTrack *c = NULL;
    h = IDirectMusicTrack_Clone(t, 0, 3072, &c);
    printf("  clone %08lx", h);
    if (c) {
        IUnknown *x = NULL;
        printf(" persist %08lx", IDirectMusicTrack_QueryInterface(c, &IID_IPersistStream, (void **)&x));
        if (x) { CLSID cl; if (SUCCEEDED(IPersistStream_GetClassID((IPersistStream *)x, &cl))) printf(" %s", track_name(&cl)); IUnknown_Release(x); }
        IDirectMusicTrack_Release(c);
    }
    printf("\n");
}

/* a track played directly (InitPlay, Play over [0, len) with the state's start as the offset, EndPlay) on the
 * state of a silent segment */
static void track_direct(IDirectMusicTrack *t, MUSIC_TIME len)
{
    IDirectMusicSegment8 *host = NULL;
    CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC, &IID_IDirectMusicSegment8, (void **)&host);
    if (!host) return;
    IDirectMusicSegment8_SetLength(host, 100 * 3072);
    REFERENCE_TIME now = 0;
    IDirectMusicPerformance8_GetTime(perf, &now, NULL);
    IDirectMusicSegmentState *ss = NULL;
    HRESULT hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)host, NULL, NULL, DMUS_SEGF_REFTIME, now + 5000000, &ss, NULL, NULL);
    if (!ss) { printf("  host play %08lx\n", hr); IDirectMusicSegment8_Release(host); return; }
    IDirectMusicSegmentState_GetStartTime(ss, &s0);
    Sleep(100);
    EnterCriticalSection(&logcs); nlog = 0; LeaveCriticalSection(&logcs);
    void *data = NULL;
    hr = IDirectMusicTrack_InitPlay(t, ss, (IDirectMusicPerformance *)perf, &data, 1, 0);
    printf("  initplay %08lx data %d\n", hr, data != NULL);
    if (SUCCEEDED(hr)) {
        HRESULT p1 = IDirectMusicTrack_Play(t, data, 0, len / 2, s0, DMUS_TRACKF_START | DMUS_TRACKF_SEEK, (IDirectMusicPerformance *)perf, ss, 1);
        HRESULT p2 = IDirectMusicTrack_Play(t, data, len / 2, len, s0, 0, (IDirectMusicPerformance *)perf, ss, 1);
        printf("  play %08lx %08lx\n", p1, p2);
        MUSIC_TIME mt = 0;
        DWORD deadline = GetTickCount() + 30000;
        do { Sleep(20); IDirectMusicPerformance8_GetTime(perf, NULL, &mt); } while (mt < s0 + len && (LONG)(deadline - GetTickCount()) > 0);
        printf("  endplay %08lx\n", IDirectMusicTrack_EndPlay(t, data));
    }
    IDirectMusicPerformance8_Stop(perf, NULL, NULL, 0, 0);
    Sleep(300);
    CLSID cls = { 0 };
    IPersistStream *ps = NULL;
    if (SUCCEEDED(IDirectMusicTrack_QueryInterface(t, &IID_IPersistStream, (void **)&ps))) { IPersistStream_GetClassID(ps, &cls); IPersistStream_Release(ps); }
    g_nogroup = !strcmp(track_name(&cls), "seq");
    g_nosegnotes = 1;
    dump_log(len + 768);
    g_nosegnotes = g_nogroup = 0;
    for (int k = 0; k < nstates; k++) IDirectMusicSegmentState_Release(states[k]);
    nstates = 0;
    IDirectMusicSegmentState_Release(ss);
    IDirectMusicSegment8_Release(host);
}

/* each track of a segment file loaded on its own from its chunk (with or without the loader), and played in a new
 * segment and directly */
static void tracks_of_file(const WCHAR *file, MUSIC_TIME len)
{
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("no %ls\n", file); return; }
    DWORD n = GetFileSize(f, NULL), got = 0;
    BYTE *b = malloc(n);
    ReadFile(f, b, n, &got, NULL);
    CloseHandle(f);
    printf("file %ls\n", file);
    /* RIFF DMSG > LIST trkl > RIFF DMTK > trkh, [trkx], the track's chunk */
    for (DWORD off = 12; off + 8 <= n; ) {
        DWORD sz = *(DWORD *)(b + off + 4);
        if (!memcmp(b + off, "LIST", 4) && !memcmp(b + off + 8, "trkl", 4)) {
            for (DWORD q = off + 12; q + 8 <= off + 8 + sz; ) {
                DWORD qs = *(DWORD *)(b + q + 4);
                if (!memcmp(b + q, "RIFF", 4) && !memcmp(b + q + 8, "DMTK", 4)) {
                    GUID cls = { 0 };
                    DWORD group = 1;
                    for (DWORD r = q + 12; r + 8 <= q + 8 + qs; ) {
                        DWORD rs = *(DWORD *)(b + r + 4);
                        if (!memcmp(b + r, "trkh", 4)) { memcpy(&cls, b + r + 8, 16); group = *(DWORD *)(b + r + 8 + 20); }
                        else if (memcmp(b + r, "trkx", 4)) {
                            for (int wl = 0; wl < 2; wl++) {
                                IDirectMusicTrack *t = NULL;
                                HRESULT hr = CoCreateInstance(&cls, NULL, CLSCTX_INPROC, &IID_IDirectMusicTrack, (void **)&t);
                                char what[96];
                                snprintf(what, sizeof(what), "  %s track %.4s%s", track_name(&cls), b + r, wl ? " (loader)" : "");
                                if (!t) { printf("%s create %08lx\n", what, hr); break; }
                                IPersistStream *ps = NULL;
                                IDirectMusicTrack_QueryInterface(t, &IID_IPersistStream, (void **)&ps);
                                IStream *st = gstream_new(b + r, 8 + rs, wl);
                                hr = ps ? IPersistStream_Load(ps, st) : E_NOINTERFACE;
                                ULARGE_INTEGER pos = { { 0, 0 } };
                                LARGE_INTEGER z = { { 0, 0 } };
                                IStream_Seek(st, z, STREAM_SEEK_CUR, &pos);
                                printf("%s load %08lx at %lu of %lu\n", what, hr, (unsigned long)pos.QuadPart, (unsigned long)(8 + rs));
                                IStream_Release(st);
                                if (ps) IPersistStream_Release(ps);
                                if (wl || getenv("TRK_BOTH")) {
                                    track_show(t, "   loaded");
                                    IDirectMusicSegment8 *sg = NULL;
                                    CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC, &IID_IDirectMusicSegment8, (void **)&sg);
                                    IDirectMusicSegment8_SetLength(sg, len);
                                    printf("   insert %08lx\n", IDirectMusicSegment8_InsertTrack(sg, t, group));
                                    play(sg, len);
                                    IDirectMusicSegment8_Release(sg);
                                    track_direct(t, len);
                                }
                                ULONG left = IDirectMusicTrack_Release(t);     /* (a failed wave InitPlay keeps it: how many times is dmime's) */
                                if (left) printf("   release: kept\n"); else printf("   release 0\n");
                            }
                        }
                        r += 8 + rs + (rs & 1);
                    }
                }
                q += 8 + qs + (qs & 1);
            }
        }
        off += 8 + sz + (sz & 1);
    }
    free(b);
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
    } else if (!strcmp(mode, "container")) {
        static const GUID g_inner = { 0x8f6d1a10, 0x1400, 0x4000, { 0x80, 0, 0, 0, 0, 0, 0, 0x01 } };
        static const GUID g_kept = { 0x8f6d1a10, 0x1400, 0x4000, { 0x80, 0, 0, 0, 0, 0, 0, 0x02 } };
        static const GUID g_lazy = { 0x8f6d1a10, 0x1400, 0x4000, { 0x80, 0, 0, 0, 0, 0, 0, 0x03 } };
        static const GUID g_hidden = { 0x8f6d1a10, 0x1400, 0x4000, { 0x80, 0, 0, 0, 0, 0, 0, 0x11 } };
        IDirectMusicContainer *c = NULL;
        IUnknown *u = NULL;
        get_by(&CLSID_DirectMusicContainer, NULL, NULL, L"box.con", &IID_IDirectMusicContainer, (void **)&c, "box.con");
        if (c) {
            enum_container(c, &GUID_DirectMusicAllTypes, "all");
            enum_container(c, &CLSID_DirectMusicSegment, "segments");
            enum_container(c, &CLSID_DirectMusicStyle, "styles");
            DMUS_OBJECTDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
            printf("  enum null desc %08lx\n", IDirectMusicContainer_EnumObject(c, &GUID_DirectMusicAllTypes, 0, NULL, NULL));
            IPersistStream *ps = NULL;
            printf("  qi persist %08lx\n", IDirectMusicContainer_QueryInterface(c, &IID_IPersistStream, (void **)&ps));
            if (ps) { CLSID cl; printf("  classid %08lx %08lx\n", IPersistStream_GetClassID(ps, &cl), cl.Data1); IPersistStream_Release(ps); }
        }
        enum_loader(&CLSID_DirectMusicSegment, "loader segments");
        enum_loader(&CLSID_DirectMusicContainer, "loader containers");
        IDirectMusicSegment8 *a = NULL, *b2 = NULL, *k = NULL;
        get_by(&CLSID_DirectMusicSegment, &g_inner, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&a, "inner by guid");
        get_by(&CLSID_DirectMusicSegment, NULL, L"inner", NULL, &IID_IDirectMusicSegment8, (void **)&b2, "inner by name");
        printf("  same object %d\n", a && a == b2);
        get_by(&CLSID_DirectMusicSegment, NULL, L"alias one", NULL, &IID_IDirectMusicSegment8, (void **)&u, "by alias");
        if (u) { IUnknown_Release(u); u = NULL; }
        get_by(&CLSID_DirectMusicSegment, &g_kept, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&k, "kept by guid");
        get_by(&CLSID_DirectMusicSegment, NULL, NULL, L"trgA.sgt", &IID_IUnknown, (void **)&u, "trgA by file");
        if (u) { IUnknown_Release(u); u = NULL; }
        if (c) printf("container released %lu\n", IDirectMusicContainer_Release(c));
        IDirectMusicSegment8 *a2 = NULL, *k2 = NULL;
        get_by(&CLSID_DirectMusicSegment, &g_inner, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&a2, "inner after");
        printf("  same as before %d\n", a2 && a2 == a);
        get_by(&CLSID_DirectMusicSegment, &g_kept, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&k2, "kept after");
        printf("  same as before %d\n", k2 && k2 == k);
        enum_loader(&CLSID_DirectMusicSegment, "loader segments after");
        for (IDirectMusicSegment8 **p = (IDirectMusicSegment8 *[]){ a, b2, k, a2, k2 }, **e = p + 5; p < e; p++) if (*p) IDirectMusicSegment8_Release(*p);
        /* NOLOADS: described, loaded when asked */
        get_by(&CLSID_DirectMusicContainer, NULL, NULL, L"lazy.con", &IID_IDirectMusicContainer, (void **)&c, "lazy.con");
        if (c) enum_container(c, &GUID_DirectMusicAllTypes, "lazy");
        get_by(&CLSID_DirectMusicSegment, &g_lazy, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&a, "lazy by guid");
        if (a) IDirectMusicSegment8_Release(a);
        if (c) printf("container released %lu\n", IDirectMusicContainer_Release(c));
        /* a segment with its own container */
        IDirectMusicSegment8 *h = load(L"host.sgt");
        if (h) {
            IDirectMusicTrack *t = NULL;
            printf("  trigger track %08lx\n", IDirectMusicSegment8_GetTrack(h, &CLSID_DirectMusicSegTriggerTrack, 0xffffffff, 0, &t));
            if (t) IDirectMusicTrack_Release(t);
            get_by(&CLSID_DirectMusicSegment, &g_hidden, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&a, "hidden by guid");
            if (a) IDirectMusicSegment8_Release(a);
            IDirectMusicSegment8_Release(h);
            get_by(&CLSID_DirectMusicSegment, &g_hidden, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&a, "hidden after");
            if (a) IDirectMusicSegment8_Release(a);
        }
        enum_loader(&GUID_DirectMusicAllTypes, "loader all");
        /* a container let go of from the cache too */
        get_by(&CLSID_DirectMusicContainer, NULL, NULL, L"box.con", &IID_IDirectMusicContainer, (void **)&c, "box.con again");
        IDirectMusicSegment8 *i3 = NULL;
        get_by(&CLSID_DirectMusicSegment, &g_inner, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&i3, "inner");
        if (c) {
            printf("release container from cache %08lx\n", IDirectMusicLoader8_ReleaseObjectByUnknown(loader, (IUnknown *)c));
            printf("container released %lu\n", IDirectMusicContainer_Release(c));
        }
        enum_loader(&CLSID_DirectMusicSegment, "loader segments now");
        enum_loader(&CLSID_DirectMusicContainer, "loader containers now");
        get_by(&CLSID_DirectMusicSegment, &g_inner, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&a, "inner now");
        printf("  same as before %d\n", a && a == i3);
        if (a) IDirectMusicSegment8_Release(a);
        get_by(&CLSID_DirectMusicSegment, &g_kept, NULL, NULL, &IID_IDirectMusicSegment8, (void **)&a, "kept now");
        if (a) IDirectMusicSegment8_Release(a);
        if (i3) IDirectMusicSegment8_Release(i3);
    } else if (!strcmp(mode, "loader")) {
        static const GUID g_trgA = { 0x8f6d1a10, 0x1200, 0x4000, { 0x80, 0, 0, 0, 0, 0, 0, 0x03 } };
        IUnknown *x = NULL, *y = NULL;
        get_by(&CLSID_DirectMusicSegment, NULL, NULL, L"trgA.sgt", &IID_IUnknown, (void **)&x, "trgA by file");
        get_by(&CLSID_DirectMusicSegment, NULL, NULL, L"trgA.sgt", &IID_IUnknown, (void **)&y, "trgA by file again");
        printf("  same %d\n", x == y); if (y) IUnknown_Release(y);
        get_by(&CLSID_DirectMusicSegment, &g_trgA, NULL, NULL, &IID_IUnknown, (void **)&y, "trgA by guid");
        printf("  same %d\n", x == y); if (y) IUnknown_Release(y);
        get_by(&CLSID_DirectMusicSegment, NULL, L"trgA", NULL, &IID_IUnknown, (void **)&y, "trgA by name");
        printf("  same %d\n", x == y); if (y) IUnknown_Release(y);
        enum_loader(&CLSID_DirectMusicSegment, "segments");
        printf("release %08lx\n", IDirectMusicLoader8_ReleaseObjectByUnknown(loader, x));
        printf("release again %08lx\n", IDirectMusicLoader8_ReleaseObjectByUnknown(loader, x));
        enum_loader(&CLSID_DirectMusicSegment, "segments released");
        get_by(&CLSID_DirectMusicSegment, &g_trgA, NULL, NULL, &IID_IUnknown, (void **)&y, "trgA by guid after release");
        printf("  same %d\n", x == y); if (y) IUnknown_Release(y);
        IUnknown_Release(x);
        printf("scan %08lx\n", IDirectMusicLoader8_ScanDirectory(loader, &CLSID_DirectMusicSegment, L"sgt", NULL));
        enum_loader(&CLSID_DirectMusicSegment, "segments scanned");
        printf("scan none %08lx\n", IDirectMusicLoader8_ScanDirectory(loader, &CLSID_DirectMusicSegment, L"xyz", NULL));
        get_by(&CLSID_DirectMusicSegment, NULL, L"pctl", NULL, &IID_IUnknown, (void **)&y, "pctl by name after scan");
        if (y) IUnknown_Release(y);
        printf("clear %08lx\n", IDirectMusicLoader8_ClearCache(loader, &CLSID_DirectMusicSegment));
        enum_loader(&CLSID_DirectMusicSegment, "segments cleared");
        DMUS_OBJECTDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_FILENAME; d.guidClass = CLSID_DirectMusicSegment; lstrcpyW(d.wszFileName, L"lyr.sgt");
        HRESULT h = IDirectMusicLoader8_SetObject(loader, &d);
        show_desc("setobject", h, &d, NULL);
        get_by(&CLSID_DirectMusicSegment, NULL, L"lyr", NULL, &IID_IUnknown, (void **)&y, "lyr by name");
        if (y) IUnknown_Release(y);
        printf("enable cache off %08lx\n", IDirectMusicLoader8_EnableCache(loader, &CLSID_DirectMusicSegment, FALSE));
        get_by(&CLSID_DirectMusicSegment, NULL, NULL, L"trgA.sgt", &IID_IUnknown, (void **)&x, "trgA uncached");
        get_by(&CLSID_DirectMusicSegment, NULL, NULL, L"trgA.sgt", &IID_IUnknown, (void **)&y, "trgA uncached again");
        printf("  same %d\n", x == y); if (y) IUnknown_Release(y); if (x) IUnknown_Release(x);
        enum_loader(&CLSID_DirectMusicSegment, "segments uncached");
        enum_loader(&GUID_DirectMusicAllTypes, "all types");
    } else if (!strcmp(mode, "param")) {
        if (getenv("FXPROBE_LOG")) DeleteFileA(getenv("FXPROBE_LOG"));
        IDirectMusicSegment8 *sg = load(L"pctl.sgt");
        IUnknown *cfg = NULL;
        DMUS_OBJECTDESC d = { sizeof(d) };
        d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_FILENAME;
        d.guidClass = CLSID_DirectMusicAudioPathConfig;
        lstrcpyW(d.wszFileName, L"pctl.aud");
        hr = IDirectMusicLoader8_GetObject(loader, &d, &IID_IUnknown, (void **)&cfg);
        printf("config %08lx\n", hr);
        IDirectMusicAudioPath *ap = NULL;
        if (cfg) printf("audiopath %08lx\n", IDirectMusicPerformance8_CreateAudioPath(perf, cfg, TRUE, &ap));
        REFERENCE_TIME rt[4] = { 0 };
        if (sg && ap && !getenv("TRK_NOPLAY")) {
            track_params(sg, &CLSID_DirectMusicParamControlTrack, "param");
            play_on(sg, ap, 4 * 3072 + 768, &rt[0]);
            IDirectMusicSegment8_SetStartPoint(sg, 2500);           /* in the middle of a curve: what is left of it */
            play_on(sg, ap, 3 * 3072, &rt[1]);
            IDirectMusicSegment8_SetStartPoint(sg, 0);
            IDirectMusicSegment8_SetRepeats(sg, 1);
            IDirectMusicSegment8_SetLoopPoints(sg, 1200, 7000);     /* a loop back into the second curve */
            play_on(sg, ap, 4 * 3072, &rt[2]);
            IDirectMusicSegment8_SetRepeats(sg, 0);
            IDirectMusicSegment8_SetLoopPoints(sg, 0, 0);   /* (dmime spins forever on a clone shorter than its loop) */
            IDirectMusicSegment *c = NULL;
            hr = IDirectMusicSegment8_Clone(sg, 1600, 7000, &c);
            printf("clone %08lx\n", hr);
            if (c && !getenv("TRK_NOCLONEPLAY")) play_on((IDirectMusicSegment8 *)c, ap, 3 * 3072, &rt[3]);
            if (getenv("TRK_TIMING")) fprintf(stderr, "clone played\n");
            if (c) IDirectMusicSegment_Release(c);
        }
        if (getenv("TRK_TIMING")) fprintf(stderr, "plays done\n");
        IDirectMusicSegment8 *bad = load(L"pctlbad.sgt");
        if (getenv("TRK_TIMING")) fprintf(stderr, "bad loaded\n");
        if (bad) {
            IDirectMusicTrack *t = NULL;
            printf("  bad param track %08lx\n", IDirectMusicSegment8_GetTrack(bad, &CLSID_DirectMusicParamControlTrack, 0xffffffff, 0, &t));
            if (t) IDirectMusicTrack_Release(t);
            IDirectMusicSegment8_Release(bad);
        }
        if (sg) IDirectMusicSegment8_Release(sg);
        if (ap) IDirectMusicAudioPath_Release(ap);
        if (cfg) IUnknown_Release(cfg);
        IDirectMusicPerformance8_CloseDown(perf);
        IDirectMusicPerformance8_Release(perf);
        dump_probe();
        return 0;
    } else if (!strcmp(mode, "standalone")) {
        /* a new segment */
        IDirectMusicSegment8 *ns = NULL;
        CoCreateInstance(&CLSID_DirectMusicSegment, NULL, CLSCTX_INPROC, &IID_IDirectMusicSegment8, (void **)&ns);
        if (ns) {
            MUSIC_TIME l = -1, sp = -1, la = -1, lb = -1; DWORD rp = 99, rs = 99;
            IDirectMusicSegment8_GetLength(ns, &l); IDirectMusicSegment8_GetRepeats(ns, &rp); IDirectMusicSegment8_GetDefaultResolution(ns, &rs);
            IDirectMusicSegment8_GetStartPoint(ns, &sp); IDirectMusicSegment8_GetLoopPoints(ns, &la, &lb);
            printf("new segment length %ld repeats %lu res %lx start %ld loop %ld %ld\n", l, rp, rs, sp, la, lb);
            IDirectMusicTrack *t = NULL;
            printf("  gettrack %08lx\n", IDirectMusicSegment8_GetTrack(ns, &(GUID){ 0 }, 0xffffffff, 0, &t));
            BYTE buf[64]; MUSIC_TIME nx = 0;
            printf("  getparam tempo %08lx\n", IDirectMusicSegment8_GetParam(ns, &GUID_TempoParam, 0xffffffff, 0, 0, &nx, buf));
            IDirectMusicSegment8_Release(ns);
        }
        /* every track class made empty */
        for (size_t k = 0; k < sizeof(g_tracks) / sizeof(*g_tracks) && !getenv("TRK_NOEMPTY"); k++) {
            IDirectMusicTrack *t = NULL;
            hr = CoCreateInstance(&g_tracks[k].g, NULL, CLSCTX_INPROC, &IID_IDirectMusicTrack, (void **)&t);
            char what[64];
            snprintf(what, sizeof(what), "empty %s %08lx", g_tracks[k].name, hr);
            if (!t) { printf("%s\n", what); continue; }
            track_show(t, what);
            if (!getenv("TRK_NODIRECT")) track_direct(t, 3072);
            printf("  release %lu\n", IDirectMusicTrack_Release(t));
        }
        for (int k = 2; k < argc; k++) {
            WCHAR w[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, argv[k], -1, w, MAX_PATH);
            tracks_of_file(w, 2 * 3072);
        }
    }
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    return 0;
}
