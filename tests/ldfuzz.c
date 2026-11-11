/* ldfuzz <dir> <iterations> <seed> [play|noplay] [ext]: mutated DirectMusic files through reDM's COM layer, in one
 * process: each is loaded from memory by the loader (the files it names found in <dir>) and used - a segment's
 * tracks, parameters, Clone and Compose (and with `play`, Download and PlaySegmentEx for a moment); a style's bands,
 * motifs and patterns; a chord map through the composer; a script's Init, every routine and variable; a container's
 * objects; an audiopath configuration as an audiopath; a collection's instruments.  <dir>: segments, styles, chord
 * maps, scripts, containers, configurations and collections (tools/sty/cases.py <dir> writes a set; `ext` keeps to
 * one kind, "spt" say).  Run with reAL's dsound.dll next to it and music=1, the fixes on.
 * A crash prints "CRASH ..." with the module and reAL's return addresses, a call that takes over 25 s "HANG", a
 * damaged heap "HEAP" (WINEDEBUG=warn+heap makes Wine check every block), and the process ends; the input in hand is
 * cur.bin, its seed file in cur.txt.  tools/fuzz/wine.sh runs it in rounds and keeps those.  The file parsers alone,
 * under AddressSanitizer: tools/fuzz/native.py. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <dmusici.h>
#include <dmusicf.h>
#define IDirectMusicScript_Init(p,a,b) (p)->lpVtbl->Init(p,a,b)
#define IDirectMusicScript_CallRoutine(p,a,b) (p)->lpVtbl->CallRoutine(p,a,b)
#define IDirectMusicScript_GetVariableVariant(p,a,b,c) (p)->lpVtbl->GetVariableVariant(p,a,b,c)
#define IDirectMusicScript_SetVariableNumber(p,a,b,c) (p)->lpVtbl->SetVariableNumber(p,a,b,c)
#define IDirectMusicScript_GetVariableNumber(p,a,b,c) (p)->lpVtbl->GetVariableNumber(p,a,b,c)
#define IDirectMusicScript_EnumRoutine(p,a,b) (p)->lpVtbl->EnumRoutine(p,a,b)
#define IDirectMusicScript_EnumVariable(p,a,b) (p)->lpVtbl->EnumVariable(p,a,b)
static const GUID GUID_NULL_ = { 0 };

static IDirectMusicPerformance8 *perf;
static IDirectMusicLoader8 *loader;
static unsigned s_r;
static unsigned rnd(void) { s_r = s_r * 1103515245u + 12345u; return (s_r >> 8) & 0xffffff; }
static unsigned rn(unsigned n) { return n ? rnd() % n : 0; }
static volatile LONG s_iter, s_tick; static int s_ok;

static LONG WINAPI crash(EXCEPTION_POINTERS *e)
{
    char m[200];
    HMODULE mod = NULL; char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, e->ExceptionRecord->ExceptionAddress, &mod);
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    const char *b = strrchr(name, '\\');
    sprintf(m, "CRASH iter %ld code %08lx at %s+%lx access %lx\n", s_iter, e->ExceptionRecord->ExceptionCode,
            b ? b + 1 : name, (unsigned long)((char *)e->ExceptionRecord->ExceptionAddress - (char *)mod),
            e->ExceptionRecord->NumberParameters > 1 ? (unsigned long)e->ExceptionRecord->ExceptionInformation[1] : 0);
    DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), m, strlen(m), &w, NULL);
    /* a short stack: return addresses in dsound.dll */
    DWORD *sp = (DWORD *)e->ContextRecord->Esp; HMODULE ds = GetModuleHandleA("dsound.dll");
    MEMORY_BASIC_INFORMATION mi; int k = 0;
    for (int i = 0; i < 400 && k < 12; i++) {
        if (IsBadReadPtr(sp + i, 4)) break;
        DWORD v = sp[i];
        if (ds && v > (DWORD)ds && v < (DWORD)ds + 0x400000 && VirtualQuery((void *)v, &mi, sizeof(mi)) && (mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE))) {
            sprintf(m, "  dsound+%lx\n", v - (DWORD)ds); WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), m, strlen(m), &w, NULL); k++;
        }
    }
    TerminateProcess(GetCurrentProcess(), 77);
    return EXCEPTION_EXECUTE_HANDLER;
}
static DWORD WINAPI watchdog(LPVOID p)
{
    LONG last = -1; int stuck = 0;
    for (;;) {
        Sleep(1000);
        if (s_tick == last) { if (++stuck >= 25) { printf("HANG iter %ld\n", s_iter); fflush(stdout); TerminateProcess(GetCurrentProcess(), 78); } }
        else { last = s_tick; stuck = 0; }
    }
}

static const DWORD ints[] = { 0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 0x7fff, 0x8000, 0xffff, 0x10000,
    0x7fffffff, 0x80000000, 0xffffffff, 0xfffffffe, 0xfffffff0, 0x40000000, 0x20000000 };
static DWORD mutate(BYTE *d, DWORD n, DWORD cap)
{
    static const int cnt[] = { 1, 1, 1, 2, 2, 3, 5, 8 };
    int m = cnt[rn(8)];
    while (m-- && n >= 16) {
        DWORD p = 12 + rn(n - 12), k = rn(10);
        if (k == 0) d[p] ^= 1 << rn(8);
        else if (k == 1) { static const BYTE v[] = { 0, 1, 0x7f, 0x80, 0xff }; d[p] = rn(6) < 5 ? v[rn(5)] : (BYTE)rn(256); }
        else if ((k == 2 || k == 3) && p + 4 <= n) { if (rn(5)) p &= ~1u; DWORD v = ints[rn(sizeof(ints) / 4)]; memcpy(d + p, &v, 4); }
        else if (k == 4 && p + 2 <= n) { WORD v = (WORD)ints[rn(sizeof(ints) / 4)]; memcpy(d + p, &v, 2); }
        else if (k == 5) { DWORD c = 1 + rn(64); if (c > n - p) c = n - p; memmove(d + p, d + p + c, n - p - c); n -= c; }
        else if (k == 6) { DWORD c = 1 << rn(4); if (n + c <= cap) { memmove(d + p + c, d + p, n - p); for (DWORD i = 0; i < c; i++) d[p + i] = (BYTE)rn(256); n += c; } }
        else if (k == 7) n = p;
        else if (k == 8) { DWORD q = 12 + rn(n - 12), c = 4 << rn(4); if (p + c <= n && q + c <= n) memmove(d + p, d + q, c); }
        else if (k == 9 && p + 4 <= n) { static const int dl[] = { -1, 1, -4, 4, -8, 8, 0x100, -0x100 }; p &= ~1u; DWORD v; memcpy(&v, d + p, 4); v += dl[rn(8)]; memcpy(d + p, &v, 4); }
    }
    return n;
}

static void use_segment(IUnknown *u, int play)
{
    IDirectMusicSegment8 *s = NULL;
    if (FAILED(IUnknown_QueryInterface(u, &IID_IDirectMusicSegment8, (void **)&s))) return;
    MUSIC_TIME len = 0; IDirectMusicSegment8_GetLength(s, &len);
    for (DWORD i = 0; i < 12; i++) {
        IDirectMusicTrack *t = NULL;
        if (FAILED(IDirectMusicSegment8_GetTrack(s, &GUID_NULL_, 0xffffffff, i, &t))) break;
        IDirectMusicTrack_Release(t);
    }
    BYTE prm[1024]; MUSIC_TIME nx;
    static const GUID *g[] = { &GUID_ChordParam, &GUID_CommandParam, &GUID_TempoParam, &GUID_TimeSignature, &GUID_RhythmParam, &GUID_IDirectMusicStyle, &GUID_IDirectMusicBand, &GUID_MuteParam };
    for (int k = 0; k < 8; k++)
        for (int t = 0; t < 3; t++) {
            memset(prm, 0, sizeof(prm));
            HRESULT hr = IDirectMusicSegment8_GetParam(s, g[k], 0xffffffff, 0, t * 1000, &nx, prm);
            if (SUCCEEDED(hr) && (k == 5 || k == 6) && *(IUnknown **)prm) IUnknown_Release(*(IUnknown **)prm);
        }
    IDirectMusicSegment *c = NULL;
    if (SUCCEEDED(IDirectMusicSegment8_Clone(s, 0, 0, &c)) && c) IDirectMusicSegment_Release(c);
    if (rn(3) == 0) { IDirectMusicSegment8 *cs = NULL; if (SUCCEEDED(IDirectMusicSegment8_Compose(s, 0, NULL, NULL, (IDirectMusicSegment **)&cs)) && cs) IDirectMusicSegment8_Release(cs); }
    if (play && perf) {
        IDirectMusicSegmentState *ss = NULL;
        IDirectMusicSegment8_Download(s, (IUnknown *)perf);
        static const DWORD fl[] = { 0, DMUS_SEGF_SECONDARY, DMUS_SEGF_MEASURE, DMUS_SEGF_AFTERPREPARETIME, DMUS_SEGF_CONTROL };
        if (SUCCEEDED(IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)s, NULL, NULL, fl[rn(5)], 0, &ss, NULL, NULL))) {
            Sleep(60 + rn(200));
            IDirectMusicPerformance8_StopEx(perf, NULL, 0, 0);
            if (ss) IDirectMusicSegmentState_Release(ss);
        }
        IDirectMusicSegment8_Unload(s, (IUnknown *)perf);
    }
    IDirectMusicSegment8_Release(s);
}
static void use_style(IUnknown *u)
{
    IDirectMusicStyle8 *s = NULL;
    if (FAILED(IUnknown_QueryInterface(u, &IID_IDirectMusicStyle8, (void **)&s))) return;
    WCHAR nm[MAX_PATH];
    for (DWORD i = 0; i < 8; i++) {
        if (IDirectMusicStyle8_EnumBand(s, i, nm) != S_OK) break;
        IDirectMusicBand *b = NULL; if (SUCCEEDED(IDirectMusicStyle8_GetBand(s, nm, &b)) && b) IDirectMusicBand_Release(b);
    }
    for (DWORD i = 0; i < 8; i++) {
        if (IDirectMusicStyle8_EnumMotif(s, i, nm) != S_OK) break;
        IDirectMusicSegment *m = NULL;
        if (SUCCEEDED(IDirectMusicStyle8_GetMotif(s, nm, &m)) && m) { use_segment((IUnknown *)m, 0); IDirectMusicSegment_Release(m); }
    }
    for (DWORD i = 0; i < 8; i++) {
        if (IDirectMusicStyle8_EnumPattern(s, i, DMUS_STYLET_PATTERN, nm) != S_OK) break;
    }
    DMUS_TIMESIGNATURE ts; double tempo; IDirectMusicStyle8_GetTimeSignature(s, &ts); IDirectMusicStyle8_GetTempo(s, &tempo);
    IDirectMusicChordMap *cm = NULL; if (SUCCEEDED(IDirectMusicStyle8_GetDefaultChordMap(s, &cm)) && cm) IDirectMusicChordMap_Release(cm);
    IDirectMusicBand *b = NULL; if (SUCCEEDED(IDirectMusicStyle8_GetDefaultBand(s, &b)) && b) IDirectMusicBand_Release(b);
    DWORD em[8]; IDirectMusicStyle8_GetEmbellishmentLength(s, DMUS_COMMANDT_FILL, 50, &em[0], &em[1]);
    IDirectMusicStyle8_Release(s);
}
static void use_chordmap(IUnknown *u, IDirectMusicStyle8 *style)
{
    IDirectMusicChordMap *c = NULL;
    if (FAILED(IUnknown_QueryInterface(u, &IID_IDirectMusicChordMap, (void **)&c))) return;
    DWORD sc; IDirectMusicChordMap_GetScale(c, &sc);
    IDirectMusicComposer *comp = NULL;
    if (style && SUCCEEDED(CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&comp))) {
        static const WORD shapes[] = { DMUS_SHAPET_FALLING, DMUS_SHAPET_LEVEL, DMUS_SHAPET_LOOPABLE, DMUS_SHAPET_LOUD, DMUS_SHAPET_SONG, DMUS_SHAPET_RANDOM };
        IDirectMusicSegment *seg = NULL;
        if (SUCCEEDED(IDirectMusicComposer_ComposeSegmentFromShape(comp, (IDirectMusicStyle *)style, 4 + rn(12), shapes[rn(6)], rn(4), rn(2), rn(2), c, &seg)) && seg) {
            use_segment((IUnknown *)seg, 0);
            IDirectMusicSegment *tr = NULL;
            if (SUCCEEDED(IDirectMusicComposer_ComposeTransition(comp, seg, seg, 768, DMUS_COMMANDT_FILL, DMUS_COMPOSEF_MEASURE, c, &tr)) && tr) IDirectMusicSegment_Release(tr);
            IDirectMusicSegment_Release(seg);
        }
        IDirectMusicComposer_Release(comp);
    }
    IDirectMusicChordMap_Release(c);
}
static void use_script(IUnknown *u)
{
    IDirectMusicScript *s = NULL;
    if (FAILED(IUnknown_QueryInterface(u, &IID_IDirectMusicScript, (void **)&s))) return;
    DMUS_SCRIPT_ERRORINFO e; memset(&e, 0, sizeof(e)); e.dwSize = sizeof(e);
    if (SUCCEEDED(IDirectMusicScript_Init(s, (IDirectMusicPerformance *)perf, &e))) {
        WCHAR nm[MAX_PATH];
        for (DWORD i = 0; i < 24; i++) {
            if (IDirectMusicScript_EnumRoutine(s, i, nm) != S_OK) break;
            memset(&e, 0, sizeof(e)); e.dwSize = sizeof(e);
            IDirectMusicScript_CallRoutine(s, nm, &e);
            InterlockedIncrement(&s_tick);
        }
        for (DWORD i = 0; i < 24; i++) {
            if (IDirectMusicScript_EnumVariable(s, i, nm) != S_OK) break;
            VARIANT v; VariantInit(&v);
            memset(&e, 0, sizeof(e)); e.dwSize = sizeof(e);
            if (SUCCEEDED(IDirectMusicScript_GetVariableVariant(s, nm, &v, &e))) VariantClear(&v);
            LONG l; IDirectMusicScript_GetVariableNumber(s, nm, &l, &e);
            IDirectMusicScript_SetVariableNumber(s, nm, 3, &e);
        }
    }
    if (perf) IDirectMusicPerformance8_StopEx(perf, NULL, 0, 0);
    IDirectMusicScript_Release(s);
}
static void use_container(IUnknown *u)
{
    IDirectMusicContainer *c = NULL;
    if (FAILED(IUnknown_QueryInterface(u, &IID_IDirectMusicContainer, (void **)&c))) return;
    for (DWORD i = 0; i < 16; i++) {
        DMUS_OBJECTDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        WCHAR alias[MAX_PATH];
        if (IDirectMusicContainer_EnumObject(c, &GUID_DirectMusicAllTypes, i, &d, alias) != S_OK) break;
        IUnknown *o = NULL;
        if (SUCCEEDED(IDirectMusicLoader8_GetObject(loader, &d, &IID_IUnknown, (void **)&o)) && o) {
            use_segment(o, 0); use_style(o);
            IUnknown_Release(o);
        }
    }
    IDirectMusicContainer_Release(c);
}
static void use_apconfig(IUnknown *u)
{
    IDirectMusicAudioPath *p = NULL;
    if (!perf) return;
    if (SUCCEEDED(IDirectMusicPerformance8_CreateAudioPath(perf, u, TRUE, &p)) && p) {
        IUnknown *o = NULL;
        if (SUCCEEDED(IDirectMusicAudioPath_GetObjectInPath(p, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, 0, &GUID_NULL_, 0, &IID_IDirectSoundBuffer, (void **)&o)) && o) IUnknown_Release(o);
        IDirectMusicAudioPath_SetVolume(p, -600, 0);
        IDirectMusicAudioPath_Activate(p, FALSE);
        IDirectMusicAudioPath_Release(p);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(crash);
    int iters = atoi(argv[2]), play = argc > 4 && !strcmp(argv[4], "play");
    s_r = (unsigned)atoi(argv[3]);
    const char *only = argc > 5 ? argv[5] : NULL;
    LoadLibraryA("dsound.dll");
    CoInitialize(NULL);
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    if (FAILED(hr)) { printf("perf %08lx\n", hr); return 1; }
    hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    printf("initaudio %08lx\n", hr);
    hr = CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    if (FAILED(hr)) { printf("loader %08lx\n", hr); return 1; }
    WCHAR dir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dir, MAX_PATH);
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, dir, FALSE);
    /* the seeds */
    static char names[400][64]; static BYTE *data[400]; static DWORD size[400]; int nf = 0;
    char pat[MAX_PATH]; sprintf(pat, "%s\\*.*", argv[1]);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) do {
        const char *x = strrchr(fd.cFileName, '.');
        if (!x || nf >= 400 || strlen(fd.cFileName) > 60) continue;
        if (strcmp(x, ".sgt") && strcmp(x, ".sty") && strcmp(x, ".cdm") && strcmp(x, ".spt") && strcmp(x, ".aud") && strcmp(x, ".con") && strcmp(x, ".dls") && strcmp(x, ".wav")) continue;
        if (only && strcmp(x + 1, only)) continue;
        char p[MAX_PATH]; sprintf(p, "%s\\%s", argv[1], fd.cFileName);
        FILE *f = fopen(p, "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 300000 && strcmp(x, ".dls")) { fclose(f); continue; }
        if (n > 2000000) { fclose(f); continue; }
        data[nf] = malloc(n); size[nf] = (DWORD)fread(data[nf], 1, n, f); fclose(f);
        strcpy(names[nf++], fd.cFileName);
    } while (FindNextFileA(h, &fd));
    printf("seeds %d\n", nf);
    if (!nf) return 1;
    IDirectMusicStyle8 *style = NULL;
    IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, (WCHAR *)L"basic.sty", (void **)&style);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    for (int it = 0; it < iters; it++) {
        s_iter = it; InterlockedIncrement(&s_tick);
        int k = rn(nf);
        const char *x = strrchr(names[k], '.');
        DWORD cap = size[k] + 64, n;
        BYTE *b = malloc(cap);
        memcpy(b, data[k], size[k]);
        n = mutate(b, size[k], cap);
        FILE *f = fopen("cur.bin", "wb"); if (f) { fwrite(b, 1, n, f); fclose(f); }
        f = fopen("cur.txt", "w"); if (f) { fprintf(f, "%s iter %d\n", names[k], it); fclose(f); }
        DMUS_OBJECTDESC d; memset(&d, 0, sizeof(d)); d.dwSize = sizeof(d);
        d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_MEMORY;
        d.guidClass = !strcmp(x, ".sty") ? CLSID_DirectMusicStyle : !strcmp(x, ".cdm") ? CLSID_DirectMusicChordMap :
                      !strcmp(x, ".spt") ? CLSID_DirectMusicScript : !strcmp(x, ".aud") ? CLSID_DirectMusicAudioPathConfig :
                      !strcmp(x, ".con") ? CLSID_DirectMusicContainer : !strcmp(x, ".dls") ? CLSID_DirectMusicCollection :
                      CLSID_DirectMusicSegment;
        d.pbMemData = b; d.llMemLength = n;
        IUnknown *u = NULL;
        hr = IDirectMusicLoader8_GetObject(loader, &d, &IID_IUnknown, (void **)&u);
        if (SUCCEEDED(hr) && u) {
            s_ok++;
            if (!strcmp(x, ".sty")) use_style(u);
            else if (!strcmp(x, ".cdm")) use_chordmap(u, style);
            else if (!strcmp(x, ".spt")) use_script(u);
            else if (!strcmp(x, ".aud")) use_apconfig(u);
            else if (!strcmp(x, ".con")) use_container(u);
            else if (!strcmp(x, ".dls")) {
                IDirectMusicCollection *c = NULL;
                if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicCollection, (void **)&c))) {
                    DWORD patch; WCHAR nm[64];
                    for (DWORD i = 0; i < 6; i++) {
                        if (IDirectMusicCollection_EnumInstrument(c, i, &patch, nm, 64) != S_OK) break;
                        IDirectMusicInstrument *ins = NULL;
                        if (SUCCEEDED(IDirectMusicCollection_GetInstrument(c, patch, &ins)) && ins) IDirectMusicInstrument_Release(ins);
                    }
                    IDirectMusicCollection_Release(c);
                }
            } else use_segment(u, play);
            IDirectMusicLoader8_ReleaseObjectByUnknown(loader, u);
            IUnknown_Release(u);
        }
        IDirectMusicLoader8_ClearCache(loader, &GUID_DirectMusicAllTypes);
        free(b);
        if (!HeapValidate(GetProcessHeap(), 0, NULL)) { printf("HEAP iter %d\n", it); TerminateProcess(GetCurrentProcess(), 79); }
        if (it % 200 == 199) printf("iter %d\n", it + 1);
    }
    printf("loaded %d of %d\n", s_ok, iters);
    printf("DONE\n");
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
