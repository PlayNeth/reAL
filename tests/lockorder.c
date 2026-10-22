/* lockorder - the loader's lock against the performance's, made to meet on purpose (no luck needed):
 *   thread A loads a segment from the application's own stream (GetObject with DMUS_OBJ_STREAM), whose Read pauses
 *            the first time: the loader's lock is held meanwhile; then the segment's Load needs the performance's
 *            lock (its style track sorts the shared style again);
 *   thread B plays a segment whose script track starts a LOADALL script: the script's first Init loads what it
 *            references, so it needs the loader's lock.
 * With the performance's lock held around that Init (reAL before the lock-order fix), A and B wait for each other.
 * With "release" as a third argument, B lets go of the script track's segment instead (caching off, so its script
 * and the script's container go too, the container calling the loader's ReleaseObject): with the performance's lock
 * held around that (reAL before the fix), the same wait.
 * lockorder.exe <segment to load from a stream> <segment with a script track> [release]: exit 0 if both finish, 3 on
 * a deadlock (neither finished in 15 s). */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dsound.h>

static IDirectMusicLoader8 *g_loader;
static IDirectMusicPerformance8 *g_perf;
static HANDLE g_inside;                 /* A is in the stream's Read, the loader's lock held */
static volatile LONG g_paused;
static BYTE *g_data; static ULONG g_size;

/* ------------------------------------------------------------------ a memory stream that pauses once */
typedef struct { IStream IStream_iface; LONG ref; ULONG pos; } mstream;
static const IStreamVtbl ms_vtbl;
static mstream *ms_new(ULONG pos)
{
    mstream *m = calloc(1, sizeof(*m));
    m->IStream_iface.lpVtbl = (IStreamVtbl *)&ms_vtbl; m->ref = 1; m->pos = pos;
    return m;
}
static HRESULT WINAPI ms_qi(IStream *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IStream) || IsEqualIID(r, &IID_ISequentialStream)) {
        *o = i; IStream_AddRef(i); return S_OK;
    }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI ms_addref(IStream *i) { return InterlockedIncrement(&((mstream *)i)->ref); }
static ULONG WINAPI ms_release(IStream *i) { LONG r = InterlockedDecrement(&((mstream *)i)->ref); if (!r) free(i); return r; }
static HRESULT WINAPI ms_read(IStream *i, void *b, ULONG n, ULONG *got)
{
    mstream *m = (mstream *)i;
    if (!InterlockedExchange(&g_paused, 1)) {      /* the first read: the loader's lock is held now */
        if (getenv("LO_TRACE")) printf("%lu A: in Read, pausing\n", GetTickCount() % 100000);
        SetEvent(g_inside);
        Sleep(1500);                                /* (B starts its play and its script's Init meanwhile) */
        if (getenv("LO_TRACE")) printf("%lu A: Read goes on\n", GetTickCount() % 100000);
    }
    ULONG k = m->pos < g_size ? g_size - m->pos : 0;
    if (k > n) k = n;
    memcpy(b, g_data + m->pos, k);
    m->pos += k;
    if (got) *got = k;
    return k == n ? S_OK : S_FALSE;
}
static HRESULT WINAPI ms_write(IStream *i, const void *b, ULONG n, ULONG *w) { return E_NOTIMPL; }
static HRESULT WINAPI ms_seek(IStream *i, LARGE_INTEGER d, DWORD how, ULARGE_INTEGER *np)
{
    mstream *m = (mstream *)i;
    LONGLONG p = how == STREAM_SEEK_SET ? d.QuadPart : how == STREAM_SEEK_CUR ? m->pos + d.QuadPart : g_size + d.QuadPart;
    if (p < 0) return STG_E_INVALIDFUNCTION;
    m->pos = (ULONG)p;
    if (np) np->QuadPart = m->pos;
    return S_OK;
}
static HRESULT WINAPI ms_setsize(IStream *i, ULARGE_INTEGER n) { return E_NOTIMPL; }
static HRESULT WINAPI ms_copyto(IStream *i, IStream *o, ULARGE_INTEGER n, ULARGE_INTEGER *r, ULARGE_INTEGER *w) { return E_NOTIMPL; }
static HRESULT WINAPI ms_commit(IStream *i, DWORD f) { return S_OK; }
static HRESULT WINAPI ms_revert(IStream *i) { return S_OK; }
static HRESULT WINAPI ms_lock(IStream *i, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD t) { return E_NOTIMPL; }
static HRESULT WINAPI ms_unlock(IStream *i, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD t) { return E_NOTIMPL; }
static HRESULT WINAPI ms_stat(IStream *i, STATSTG *s, DWORD f)
{
    memset(s, 0, sizeof(*s)); s->type = STGTY_STREAM; s->cbSize.QuadPart = g_size;
    return S_OK;
}
static HRESULT WINAPI ms_clone(IStream *i, IStream **o) { *o = &ms_new(((mstream *)i)->pos)->IStream_iface; return S_OK; }
static const IStreamVtbl ms_vtbl = { ms_qi, ms_addref, ms_release, ms_read, ms_write, ms_seek, ms_setsize, ms_copyto,
                                     ms_commit, ms_revert, ms_lock, ms_unlock, ms_stat, ms_clone };

/* ------------------------------------------------------------------ the two threads */
static volatile LONG g_done_a, g_done_b;
static HRESULT g_hr_a, g_hr_b;
static IDirectMusicSegment8 *g_trkseg;
static int g_release;

static DWORD WINAPI th_a(void *p)
{
    CoInitialize(NULL);
    mstream *m = ms_new(0);
    DMUS_OBJECTDESC d = { sizeof(d) };
    d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_STREAM;
    d.guidClass = CLSID_DirectMusicSegment;
    d.pStream = &m->IStream_iface;
    IDirectMusicSegment8 *s = NULL;
    g_hr_a = IDirectMusicLoader8_GetObject(g_loader, &d, &IID_IDirectMusicSegment8, (void **)&s);
    if (getenv("LO_TRACE")) printf("%lu A: GetObject returned\n", GetTickCount() % 100000);
    if (s) IDirectMusicSegment8_Release(s);
    IStream_Release(&m->IStream_iface);
    g_done_a = 1;
    CoUninitialize();
    return 0;
}
static DWORD WINAPI th_b(void *p)
{
    CoInitialize(NULL);
    WaitForSingleObject(g_inside, INFINITE);
    Sleep(200);
    IDirectMusicSegmentState *st = NULL;
    if (getenv("LO_PROBE")) {
        printf("%lu B: probe ClearCache\n", GetTickCount() % 100000);
        IDirectMusicLoader8_ClearCache(g_loader, &CLSID_DirectMusicSegment);
        printf("%lu B: probe returned\n", GetTickCount() % 100000);
    }
    if (g_release) {
        /* the segment's last reference: its script track's script goes with it (the loader keeps nothing: caching
         * is off), and the script's container lets its objects go from the loader */
        if (getenv("LO_TRACE")) printf("%lu B: release\n", GetTickCount() % 100000);
        IDirectMusicSegment8_Release(g_trkseg); g_trkseg = NULL;
        if (getenv("LO_TRACE")) printf("%lu B: release returned\n", GetTickCount() % 100000);
    } else {
        if (getenv("LO_TRACE")) printf("%lu B: play\n", GetTickCount() % 100000);
        g_hr_b = IDirectMusicPerformance8_PlaySegmentEx(g_perf, (IUnknown *)g_trkseg, NULL, NULL, DMUS_SEGF_SECONDARY, 0, &st, NULL, NULL);
        if (getenv("LO_TRACE")) printf("%lu B: play returned\n", GetTickCount() % 100000);
    }
    if (st) IDirectMusicSegmentState_Release(st);
    g_done_b = 1;
    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------------ "vbs": a VBScript script's last release against a
 * routine of another one running on the scripts' thread and calling the performance */
#define IDirectMusicScript_Init(p,a,b) (p)->lpVtbl->Init(p,a,b)
#define IDirectMusicScript_CallRoutine(p,a,b) (p)->lpVtbl->CallRoutine(p,a,b)
#define IDirectMusicScript_Release(p) (p)->lpVtbl->Release(p)
static BYTE *put(BYTE *p, const void *d, DWORD n) { memcpy(p, d, n); return p + n; }
static BYTE *chunk(BYTE *p, const char *id, const void *d, DWORD n)
{
    p = put(p, id, 4); p = put(p, &n, 4); p = put(p, d, n);
    if (n & 1) *p++ = 0;
    return p;
}
static BYTE *make_script(const char *src, const WCHAR *lang, DWORD *size)      /* (as scrtest's) */
{
    size_t n = strlen(src);
    BYTE *b = calloc(1, 4096 + n * 6);
    WCHAR *w = calloc(n * 2 + 2, sizeof(WCHAR));
    size_t k = 0;
    for (const char *c = src; *c; c++) { if (*c == '\n') w[k++] = '\r'; w[k++] = (BYTE)*c; }
    BYTE *p = b + 12;
    DWORD flags = 0, scve[2] = { 0x00080000, 0 };
    p = chunk(p, "schd", &flags, 4);
    p = chunk(p, "scve", scve, 8);
    BYTE cn[32], *q = cn;
    DWORD sz = 4 + 12 + 12;
    q = put(q, "RIFF", 4); q = put(q, &sz, 4); q = put(q, "DMCN", 4);
    q = chunk(q, "cnth", &flags, 4);
    sz = 4;
    q = put(q, "LIST", 4); q = put(q, &sz, 4); q = put(q, "cosl", 4);
    p = put(p, cn, (DWORD)(q - cn));
    p = chunk(p, "scla", lang, (lstrlenW(lang) + 1) * 2);
    p = chunk(p, "scsr", w, (DWORD)(k + 1) * 2);
    DWORD total = (DWORD)(p - b) - 8;
    memcpy(b, "RIFF", 4); memcpy(b + 4, &total, 4); memcpy(b + 8, "DMSC", 4);
    *size = (DWORD)(p - b);
    free(w);
    return b;
}
static DWORD WINAPI th_release(void *p)
{
    CoInitialize(NULL);
    IDirectMusicSegment8_Release(g_trkseg); g_trkseg = NULL;
    if (getenv("LO_TRACE")) printf("%lu A: release returned\n", GetTickCount() % 100000);
    g_done_a = 1;
    CoUninitialize();
    return 0;
}
static IDirectMusicScript *g_busy;
static HANDLE g_running;
static DWORD WINAPI th_busy(void *p)
{
    CoInitialize(NULL);
    DMUS_SCRIPT_ERRORINFO e = { sizeof(e) };
    SetEvent(g_running);
    if (getenv("LO_TRACE")) printf("%lu B: routine\n", GetTickCount() % 100000);
    g_hr_b = IDirectMusicScript_CallRoutine(g_busy, L"Busy", &e);
    if (getenv("LO_TRACE")) printf("%lu B: routine returned %08lx\n", GetTickCount() % 100000, (unsigned long)g_hr_b);
    g_done_b = 1;
    CoUninitialize();
    return 0;
}
static int vbs_mode(const char *segfile)
{
    static const char src[] = "Dim i\nSub Busy\n  For i = 1 To 3000\n    SetMasterVolume -100\n    SetMasterVolume 0\n  Next\nEnd Sub\n";
    DWORD n;
    BYTE *b = make_script(src, L"VBScript", &n);
    DMUS_OBJECTDESC d = { sizeof(d) };
    d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_MEMORY;
    d.guidClass = CLSID_DirectMusicScript;
    d.pbMemData = b; d.llMemLength = n;
    HRESULT hr = IDirectMusicLoader8_GetObject(g_loader, &d, &IID_IDirectMusicScript, (void **)&g_busy);
    printf("busy script %08lx\n", hr);
    if (!g_busy) return 1;
    DMUS_SCRIPT_ERRORINFO e = { sizeof(e) };
    printf("init %08lx\n", IDirectMusicScript_Init(g_busy, (IDirectMusicPerformance *)g_perf, &e));
    /* the segment whose script track's VBScript script will go with it: played once (its engine started) */
    IDirectMusicSegmentState *st = NULL;
    hr = IDirectMusicPerformance8_PlaySegmentEx(g_perf, (IUnknown *)g_trkseg, NULL, NULL, DMUS_SEGF_SECONDARY, 0, &st, NULL, NULL);
    printf("play %08lx\n", hr);
    Sleep(600);
    IDirectMusicPerformance8_StopEx(g_perf, (IUnknown *)g_trkseg, 0, 0);
    if (st) IDirectMusicSegmentState_Release(st);
    Sleep(600);
    g_running = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th = CreateThread(NULL, 0, th_busy, NULL, 0, NULL);
    WaitForSingleObject(g_running, INFINITE);
    Sleep(100);
    if (getenv("LO_TRACE")) printf("%lu A: last release of the segment\n", GetTickCount() % 100000);
    HANDLE rel = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(void *)th_release, NULL, 0, NULL);
    HANDLE both[2] = { th, rel };
    if (WaitForMultipleObjects(2, both, TRUE, 20000) != WAIT_OBJECT_0) {
        printf("DEADLOCK: routine %s, release %s\n", g_done_b ? "done" : "waiting", g_done_a ? "done" : "waiting");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    printf("ok: routine %08lx, release done\n", (unsigned long)g_hr_b);
    IDirectMusicScript_Release(g_busy);
    return 0;
}

int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;
    if (argc < 3) { fprintf(stderr, "usage: lockorder <segment for the stream> <segment with a script track>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }      /* ("vbs": any file; not used) */
    fseek(f, 0, SEEK_END); g_size = ftell(f); fseek(f, 0, SEEK_SET);
    g_data = malloc(g_size);
    if (fread(g_data, 1, g_size, f) != g_size) return 2;
    fclose(f);
    CoInitialize(NULL);
    WCHAR dir[MAX_PATH], w[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&g_loader);
    CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&g_perf);
    if (!g_loader || !g_perf) return 1;
    HRESULT hr = IDirectMusicPerformance8_InitAudio(g_perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    printf("InitAudio %08lx\n", hr);
    IDirectMusicLoader8_SetSearchDirectory(g_loader, &GUID_DirectMusicAllTypes, dir, FALSE);
    g_release = argc > 3 && !strcmp(argv[3], "release");
    if (argc > 3) IDirectMusicLoader8_EnableCache(g_loader, &GUID_DirectMusicAllTypes, FALSE);
    MultiByteToWideChar(CP_ACP, 0, argv[2], -1, w, MAX_PATH);
    hr = IDirectMusicLoader8_LoadObjectFromFile(g_loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&g_trkseg);
    printf("load %s %08lx\n", argv[2], hr);
    if (!g_trkseg) return 1;
    if (argc > 3 && !strcmp(argv[3], "vbs")) {
        int r = vbs_mode(argv[1]);
        IDirectMusicPerformance8_CloseDown(g_perf);
        return r;
    }
    g_inside = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE th[2] = { CreateThread(NULL, 0, th_a, NULL, 0, NULL), CreateThread(NULL, 0, th_b, NULL, 0, NULL) };
    if (WaitForMultipleObjects(2, th, TRUE, 15000) != WAIT_OBJECT_0) {
        printf("DEADLOCK: stream load %s, play %s\n", g_done_a ? "done" : "waiting", g_done_b ? "done" : "waiting");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    printf("ok: stream load %08lx, play %08lx\n", (unsigned long)g_hr_a, (unsigned long)g_hr_b);
    Sleep(300);
    IDirectMusicPerformance8_CloseDown(g_perf);
    if (g_trkseg) IDirectMusicSegment8_Release(g_trkseg);
    IDirectMusicPerformance8_Release(g_perf);
    IDirectMusicLoader8_Release(g_loader);
    CoUninitialize();
    return 0;
}
