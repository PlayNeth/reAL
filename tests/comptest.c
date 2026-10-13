/* comptest: the composer (IDirectMusicComposer8) on the compose case (tools/sty/cases.py compose: cmap1.cdm, cmap2.cdm,
 * compsty.sty, template.sgt in the current directory).  time() is fixed in dmcompos, dmstyle and dmime (their
 * srand(time(NULL)) seeds) by patching their imports; reAL-Music takes REAL_MUSIC_TIME.  COMP_RANDLOG=<file> logs
 * dmcompos's rand() calls with their callers. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <dmusici.h>
#include <dmusicf.h>

DEFINE_GUID(GUID_IDirectMusicChordMap_, 0xd2ac28ad, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static const GUID GUID_NULL_ = { 0 };
#define BAR 3072
#define FIXTIME 1234567

static long __cdecl fixed_time(long *t) { if (t) *t = FIXTIME; return FIXTIME; }
static FILE *g_randlog;
static int (__cdecl *g_rand)(void);
static int __cdecl rand_log(void)
{
    int v = g_rand();
    if (g_randlog) { fprintf(g_randlog, "R %p %d\n", __builtin_return_address(0), v); fflush(g_randlog); }
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

static void wout(const WCHAR *w, int max)
{
    for (int i = 0; i < max && w[i]; i++) if (w[i] < 128 && w[i] >= 32) putchar((char)w[i]); else printf("<%04x>", w[i]);
}

/* the chords and commands a segment answers */
static void dump(IDirectMusicSegment8 *sg, const char *tag)
{
    MUSIC_TIME len = -1;
    IDirectMusicSegment8_GetLength(sg, &len);
    printf("%s: length %ld\n", tag, len);
    for (DWORD k = 0; k < 12; k++) {
        IDirectMusicTrack *tr = NULL;
        IDirectMusicSegment8_GetTrack(sg, &GUID_NULL_, 0xffffffff, k, &tr);
        if (!tr) break;
        IPersistStream *ps = NULL;
        CLSID c = { 0 };
        DWORD grp = 0;
        IDirectMusicSegment8_GetTrackGroup(sg, tr, &grp);
        if (SUCCEEDED(IDirectMusicTrack_QueryInterface(tr, &IID_IPersistStream, (void **)&ps))) { IPersistStream_GetClassID(ps, &c); IPersistStream_Release(ps); }
        printf("  track %lu class %08lx group %lx\n", k, c.Data1, grp);
        IDirectMusicTrack_Release(tr);
    }
    for (MUSIC_TIME t = 0; t < len && t < 64 * BAR; ) {
        DMUS_CHORD_KEY ch;
        memset(&ch, 0xcc, sizeof(ch));
        MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_ChordParam, 0xffffffff, 0, t, &nx, &ch);
        printf("  chord %ld: %08lx next %ld", t, hr, nx);
        if (hr == S_OK) {
            printf(" '");
            wout(ch.wszName, 16);
            printf("' m %u b %u key %u scale %08lx subs %u", ch.wMeasure, ch.bBeat, ch.bKey, ch.dwScale, ch.bSubChordCount);
            for (int s = 0; s < ch.bSubChordCount && s < DMUS_MAXSUBCHORD; s++)
                printf(" [%lx %lx %lx %lx %u %u]", ch.SubChordList[s].dwChordPattern, ch.SubChordList[s].dwScalePattern,
                       ch.SubChordList[s].dwInversionPoints, ch.SubChordList[s].dwLevels, ch.SubChordList[s].bChordRoot,
                       ch.SubChordList[s].bScaleRoot);
        }
        printf("\n");
        if (hr != S_OK || nx <= 0) break;
        t += nx;
    }
    for (MUSIC_TIME t = 0; t < len && t < 64 * BAR; ) {
        DMUS_COMMAND_PARAM_2 cm;
        memset(&cm, 0xcc, sizeof(cm));
        MUSIC_TIME nx = 0;
        HRESULT hr = IDirectMusicSegment8_GetParam(sg, &GUID_CommandParam2, 0xffffffff, 0, t, &nx, &cm);
        printf("  command %ld: %08lx next %ld", t, hr, nx);
        if (hr == S_OK) printf(" t %ld cmd %u groove %u range %u repeat %u", cm.mtTime, cm.bCommand, cm.bGrooveLevel, cm.bGrooveRange, cm.bRepeatMode);
        printf("\n");
        if (hr != S_OK || nx <= 0) break;
        t += nx;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    LoadLibraryA("dsound.dll");
    static const char *const dlls[] = { "dmcompos.dll", "dmstyle.dll", "dmime.dll" };
    if (getenv("COMP_RANDLOG")) g_randlog = fopen(getenv("COMP_RANDLOG"), "w");
    g_rand = (void *)GetProcAddress(LoadLibraryA("msvcrt.dll"), "rand");
    for (int k = 0; k < 3; k++) {
        HMODULE m = LoadLibraryA(dlls[k]);
        if (!m) continue;
        patch_import_name(m, "msvcrt.dll", "time", (void *)fixed_time);
        if (g_randlog) patch_import_name(m, "msvcrt.dll", "rand", (void *)rand_log);
    }
    CoInitialize(NULL);
    IDirectMusicLoader8 *ld;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, dir, FALSE);
    if (argc > 1) {         /* templates (compose2): each composed with its own style and chord maps, then given ones */
        IDirectMusicComposer *cp = NULL;
        HRESULT hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
        printf("create composer %08lx\n", hr);
        IDirectMusicChordMap *ca = NULL;
        IDirectMusicStyle8 *st = NULL;
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmapa.cdm", (void **)&ca);
        IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&st);
        for (int a = 1; a < argc; a++) {
            WCHAR w[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, argv[a], -1, w, MAX_PATH);
            IDirectMusicSegment8 *tp = NULL;
            hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&tp);
            printf("load %s %08lx\n", argv[a], hr);
            if (!tp) continue;
            for (int k = 0; k < 6; k++) {
                IDirectMusicSegment *out = NULL;
                WORD act = (WORD)(k % 4);
                int given = k >= 4;
                hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, given ? (IDirectMusicStyle *)st : NULL, (IDirectMusicSegment *)tp,
                                                                     given ? (WORD)(k - 3) : act, given ? ca : NULL, &out);
                printf("%s compose %d: %08lx\n", argv[a], k, hr);
                if (out) {
                    IDirectMusicSegment8 *o8 = NULL;
                    IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
                    dump(o8, "composed");
                    IDirectMusicSegment8_Release(o8);
                    IDirectMusicSegment_Release(out);
                }
            }
            IDirectMusicSegment8_Release(tp);
        }
        if (st) IDirectMusicStyle8_Release(st);
        if (ca) IDirectMusicChordMap_Release(ca);
        if (cp) IDirectMusicComposer_Release(cp);
        IDirectMusicLoader8_Release(ld);
        return 0;
    }
    IDirectMusicChordMap *c1 = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmap1.cdm", (void **)&c1);
    printf("load cmap1 %08lx\n", hr);
    if (getenv("COMP_DEBUG") && c1) {
        static const GUID priv = { 0x93be9414, 0x5c4e, 0x11d1, { 0xa7, 0xce, 0x00, 0xa0, 0xc9, 0x13, 0xf7, 0x3c } };
        void *x = NULL;
        HRESULT h2 = IDirectMusicChordMap_QueryInterface(c1, &priv, &x);
        char mod[MAX_PATH] = "";
        HMODULE hm = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)*(void **)*(void **)c1, &hm);
        GetModuleFileNameA(hm, mod, MAX_PATH);
        fprintf(stderr, "private QI %08lx %p module %s\n", h2, x, mod);
    }
    IDirectMusicStyle8 *st = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&st);
    printf("load style %08lx\n", hr);
    IDirectMusicSegment8 *tp = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, L"template.sgt", (void **)&tp);
    printf("load template %08lx\n", hr);
    IDirectMusicComposer *cp = NULL;
    hr = CoCreateInstance(&CLSID_DirectMusicComposer, NULL, CLSCTX_INPROC, &IID_IDirectMusicComposer, (void **)&cp);
    printf("create composer %08lx\n", hr);
    if (!cp || !tp) return 1;
    for (WORD act = 0; act < 4; act++) {
        IDirectMusicSegment *out = NULL;
        hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, (IDirectMusicStyle *)st, (IDirectMusicSegment *)tp, act, c1, &out);
        printf("ComposeSegmentFromTemplate activity %u: %08lx\n", act, hr);
        if (out) {
            IDirectMusicSegment8 *o8 = NULL;
            IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
            dump(o8, "composed");
            IDirectMusicSegment8_Release(o8);
            IDirectMusicSegment_Release(out);
        }
    }
    {
        IDirectMusicSegment *out = NULL;
        hr = IDirectMusicComposer_ComposeSegmentFromTemplate(cp, NULL, (IDirectMusicSegment *)tp, 1, NULL, &out);
        printf("ComposeSegmentFromTemplate from the template's own: %08lx\n", hr);
        if (out) {
            IDirectMusicSegment8 *o8 = NULL;
            IDirectMusicSegment_QueryInterface(out, &IID_IDirectMusicSegment8, (void **)&o8);
            dump(o8, "composed");
            IDirectMusicSegment8_Release(o8);
            IDirectMusicSegment_Release(out);
        }
    }
    IDirectMusicComposer_Release(cp);
    if (st) IDirectMusicStyle8_Release(st);
    if (c1) IDirectMusicChordMap_Release(c1);
    IDirectMusicSegment8_Release(tp);
    IDirectMusicLoader8_Release(ld);
    return 0;
}
