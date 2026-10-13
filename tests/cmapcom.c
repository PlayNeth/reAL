/* cmapcom: IDirectMusicChordMap through the loader, a style's chord maps, and the chord map and signpost tracks
 * (cmap1.cdm / cmap2.cdm / compsty.sty / template.sgt from tools/sty/cases.py compose, in the current directory). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <dmusici.h>
#include <dmusicf.h>

DEFINE_GUID(GUID_IDirectMusicChordMap_, 0xd2ac28ad, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DEFINE_GUID(GUID_NOTIFICATION_RECOMPOSE_, 0xd348372b, 0x945b, 0x45ae, 0xa5, 0x22, 0x45, 0x0f, 0x12, 0x5b, 0x84, 0xa5);
DEFINE_GUID(CLSID_ChordMapTrack_, 0xd2ac2896, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DEFINE_GUID(CLSID_SignPostTrack_, 0xf17e8672, 0xc3b4, 0x11d1, 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static const GUID GUID_NULL_ = { 0 };
#define BAR 3072

static void wout(const WCHAR *w)
{
    for (int i = 0; w[i] && i < 300; i++) if (w[i] < 128) putchar((char)w[i]); else printf("<%04x>", w[i]);
}

static void desc(const char *tag, IUnknown *u)
{
    IDirectMusicObject *o = NULL;
    HRESULT hr = IUnknown_QueryInterface(u, &IID_IDirectMusicObject, (void **)&o);
    printf("%s object %08lx", tag, hr);
    if (o) {
        DMUS_OBJECTDESC d;
        memset(&d, 0xcc, sizeof(d));
        d.dwSize = sizeof(d);
        hr = IDirectMusicObject_GetDescriptor(o, &d);
        printf(" desc %08lx size %lx valid %lx class %08lx name '", hr, d.dwSize,
               d.dwValidData & ~DMUS_OBJ_FILENAME & ~DMUS_OBJ_FULLPATH, d.guidClass.Data1);
        wout(d.wszName);
        printf("' cat0 %04x obj %08lx vers %lx.%lx", d.wszCategory[0], d.guidObject.Data1, d.vVersion.dwVersionMS, d.vVersion.dwVersionLS);
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
    IUnknown *x = NULL;
    printf(" persist0 %08lx", hr = IUnknown_QueryInterface(u, &IID_IPersist, (void **)&x));
    if (x) IUnknown_Release(x);
    printf("\n");
}

static IStream *file_stream(const WCHAR *name)
{
    HANDLE f = CreateFileW(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    DWORD n = GetFileSize(f, NULL), got = 0;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    ReadFile(f, GlobalLock(g), n, &got, NULL);
    GlobalUnlock(g);
    CloseHandle(f);
    IStream *s = NULL;
    CreateStreamOnHGlobal(g, TRUE, &s);
    return s;
}

static void chordmap(IDirectMusicChordMap *cm)
{
    DWORD sc = 0xeeeeeeee;
    HRESULT hr = IDirectMusicChordMap_GetScale(cm, &sc);
    printf("  scale %08lx %08lx\n", hr, sc);
    desc("  chordmap", (IUnknown *)cm);
}

static void cmtrack(IDirectMusicTrack *tr, IDirectMusicChordMap *c1, const char *tag)
{
    for (MUSIC_TIME t = -100; t < 12 * BAR; t += BAR) {
        IDirectMusicChordMap *m = NULL;
        MUSIC_TIME nx = -1;
        HRESULT hr = IDirectMusicTrack_GetParam(tr, &GUID_IDirectMusicChordMap_, t, &nx, &m);
        printf("  %s chordmap at %ld: %08lx next %ld %s", tag, t, hr, nx, !m ? "none" : m == c1 ? "cmap1" : "another");
        if (m) {
            DWORD sc = 0;
            IDirectMusicChordMap_GetScale(m, &sc);
            printf(" scale %08lx", sc);
            IDirectMusicChordMap_Release(m);
        }
        printf("\n");
    }
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

    IDirectMusicChordMap *c1 = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicChordMap, &IID_IDirectMusicChordMap, L"cmap1.cdm", (void **)&c1);
    printf("load cmap1 %08lx\n", hr);
    if (c1) chordmap(c1);

    /* a new chord map: its descriptor, Load and ParseDescriptor from a stream */
    IDirectMusicChordMap *nc = NULL;
    hr = CoCreateInstance(&CLSID_DirectMusicChordMap, NULL, CLSCTX_INPROC, &IID_IDirectMusicChordMap, (void **)&nc);
    printf("create chordmap %08lx\n", hr);
    if (nc) {
        desc("  new", (IUnknown *)nc);
        IDirectMusicObject *o = NULL;
        IDirectMusicChordMap_QueryInterface(nc, &IID_IDirectMusicObject, (void **)&o);
        IStream *s = file_stream(L"cmap2.cdm");
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        hr = IDirectMusicObject_ParseDescriptor(o, s, &d);
        printf("  parse %08lx valid %lx class %08lx obj %08lx name '", hr, d.dwValidData, d.guidClass.Data1, d.guidObject.Data1);
        wout(d.wszName);
        printf("'\n");
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwValidData = DMUS_OBJ_NAME | DMUS_OBJ_OBJECT | DMUS_OBJ_CATEGORY;
        lstrcpyW(d.wszName, L"renamed");
        d.guidObject.Data1 = 0x1234;
        hr = IDirectMusicObject_SetDescriptor(o, &d);
        printf("  setdesc %08lx valid now %lx\n", hr, d.dwValidData);
        d.dwSize = 8;
        printf("  setdesc small %08lx\n", IDirectMusicObject_SetDescriptor(o, &d));
        desc("  renamed", (IUnknown *)nc);
        IPersistStream *ps = NULL;
        IDirectMusicChordMap_QueryInterface(nc, &IID_IPersistStream, (void **)&ps);
        LARGE_INTEGER z = { 0 };
        IStream_Seek(s, z, STREAM_SEEK_SET, NULL);
        hr = IPersistStream_Load(ps, s);
        printf("  load stream %08lx\n", hr);
        chordmap(nc);
        IStream_Release(s);
        s = file_stream(L"compsty.sty");
        printf("  load a style %08lx\n", IPersistStream_Load(ps, s));
        IStream_Release(s);
        printf("  isdirty %08lx\n", IPersistStream_IsDirty(ps));
        IPersistStream_Release(ps);
        IDirectMusicObject_Release(o);
        IDirectMusicChordMap_Release(nc);
    }

    IDirectMusicStyle8 *st = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicStyle, &IID_IDirectMusicStyle8, L"compsty.sty", (void **)&st);
    printf("load style %08lx\n", hr);
    if (st) {
        WCHAR w[0x104];
        for (DWORD i = 0; i < 4; i++) {
            memset(w, 0, sizeof(w));
            hr = IDirectMusicStyle8_EnumChordMap(st, i, w);
            printf("  enum %lu %08lx '", i, hr); wout(w); printf("'\n");
        }
        IDirectMusicChordMap *m = (void *)1;
        hr = IDirectMusicStyle8_GetDefaultChordMap(st, &m);
        printf("  default %08lx %s\n", hr, m == (void *)1 ? "untouched" : m == c1 ? "cmap1" : m ? "another" : "none");
        if (hr == S_OK && m) IDirectMusicChordMap_Release(m);
        static const WCHAR *const names[] = { L"cmap1", L"CMAP1", L"cmap2", L"Second Map", L"second map", L"x" };
        for (int k = 0; k < 6; k++) {
            m = (void *)1;
            hr = IDirectMusicStyle8_GetChordMap(st, (WCHAR *)names[k], &m);
            printf("  get '"); wout(names[k]); printf("' %08lx %s", hr, m == (void *)1 ? "untouched" : m == c1 ? "cmap1" : m ? "another" : "none");
            if (hr == S_OK && m) {
                DWORD sc = 0;
                IDirectMusicChordMap_GetScale(m, &sc);
                printf(" scale %08lx", sc);
                desc("", (IUnknown *)m);
                IDirectMusicChordMap_Release(m);
            } else printf("\n");
        }
    }

    IDirectMusicSegment8 *sg = NULL;
    hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, L"template.sgt", (void **)&sg);
    printf("load template %08lx\n", hr);
    if (sg) {
        for (MUSIC_TIME t = -10; t < 12 * BAR; t += BAR) {
            IDirectMusicChordMap *m = NULL;
            MUSIC_TIME nx = -1;
            hr = IDirectMusicSegment8_GetParam(sg, &GUID_IDirectMusicChordMap_, 0xffffffff, 0, t, &nx, &m);
            printf("segment chordmap at %ld: %08lx next %ld %s\n", t, hr, nx, !m ? "none" : m == c1 ? "cmap1" : "another");
            if (m) IDirectMusicChordMap_Release(m);
        }
        for (DWORD k = 0; k < 8; k++) {
            IDirectMusicTrack *tr = NULL;
            hr = IDirectMusicSegment8_GetTrack(sg, &GUID_NULL_, 0xffffffff, k, &tr);
            if (!tr) break;
            IPersistStream *ps = NULL;
            CLSID c = { 0 };
            if (SUCCEEDED(IDirectMusicTrack_QueryInterface(tr, &IID_IPersistStream, (void **)&ps))) {
                IPersistStream_GetClassID(ps, &c);
                IPersistStream_Release(ps);
            }
            printf("track %lu class %08lx\n", k, c.Data1);
            IDirectMusicTrack_Release(tr);
        }
        IDirectMusicTrack *tr = NULL;
        hr = IDirectMusicSegment8_GetTrack(sg, &CLSID_ChordMapTrack_, 0xffffffff, 0, &tr);
        printf("chordmap track %08lx\n", hr);
        if (tr) {
            static const GUID *const gs[] = { &GUID_IDirectMusicChordMap_, &GUID_ChordParam, &GUID_IDirectMusicStyle, &GUID_NULL_ };
            for (unsigned k = 0; k < 4; k++) printf("  supported %u %08lx\n", k, IDirectMusicTrack_IsParamSupported(tr, gs[k]));
            cmtrack(tr, c1, "track");
            MUSIC_TIME nx = -1;
            printf("  get null %08lx\n", IDirectMusicTrack_GetParam(tr, &GUID_IDirectMusicChordMap_, 0, &nx, NULL));
            IDirectMusicChordMap *m = NULL;
            printf("  get chord %08lx\n", IDirectMusicTrack_GetParam(tr, &GUID_ChordParam, 0, &nx, &m));
            printf("  notify %08lx %08lx\n", IDirectMusicTrack_AddNotificationType(tr, &GUID_NOTIFICATION_RECOMPOSE_),
                   IDirectMusicTrack_RemoveNotificationType(tr, &GUID_NOTIFICATION_RECOMPOSE_));
            IDirectMusicTrack *cl = NULL;
            hr = IDirectMusicTrack_Clone(tr, 2 * BAR, 9 * BAR, &cl);
            printf("  clone %08lx\n", hr);
            if (cl) { cmtrack(cl, c1, "clone"); IDirectMusicTrack_Release(cl); }
            if (c1) {
                printf("  set %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_IDirectMusicChordMap_, 6 * BAR, c1));
                printf("  set late %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_IDirectMusicChordMap_, 9 * BAR + 5, c1));
                printf("  set null %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_IDirectMusicChordMap_, 0, NULL));
                printf("  set chord %08lx\n", IDirectMusicTrack_SetParam(tr, &GUID_ChordParam, 0, c1));
                cmtrack(tr, c1, "after set");
            }
            IDirectMusicTrack_Release(tr);
        }
        hr = IDirectMusicSegment8_GetTrack(sg, &CLSID_SignPostTrack_, 0xffffffff, 0, &tr);
        printf("signpost track %08lx\n", hr);
        if (tr) {
            static const GUID *const gs[] = { &GUID_IDirectMusicChordMap_, &GUID_ChordParam, &GUID_NULL_ };
            for (unsigned k = 0; k < 3; k++) printf("  supported %u %08lx\n", k, IDirectMusicTrack_IsParamSupported(tr, gs[k]));
            MUSIC_TIME nx = -1;
            DWORD x = 0;
            printf("  get %08lx set %08lx\n", IDirectMusicTrack_GetParam(tr, &GUID_ChordParam, 0, &nx, &x),
                   IDirectMusicTrack_SetParam(tr, &GUID_ChordParam, 0, &x));
            printf("  notify %08lx %08lx %08lx %08lx\n", IDirectMusicTrack_AddNotificationType(tr, &GUID_NOTIFICATION_RECOMPOSE_),
                   IDirectMusicTrack_AddNotificationType(tr, &GUID_NOTIFICATION_CHORD),
                   IDirectMusicTrack_RemoveNotificationType(tr, &GUID_NOTIFICATION_RECOMPOSE_),
                   IDirectMusicTrack_RemoveNotificationType(tr, &GUID_NOTIFICATION_CHORD));
            IDirectMusicTrack *cl = NULL;
            printf("  clone %08lx\n", IDirectMusicTrack_Clone(tr, 3 * BAR, 9 * BAR, &cl));
            if (cl) IDirectMusicTrack_Release(cl);
            IDirectMusicTrack_Release(tr);
        }
        IDirectMusicSegment8_Release(sg);
    }
    if (st) IDirectMusicStyle8_Release(st);
    if (c1) IDirectMusicChordMap_Release(c1);
    IDirectMusicLoader8_Release(ld);
    return 0;
}
