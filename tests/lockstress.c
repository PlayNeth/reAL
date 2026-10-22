/* lockstress - reDM's locks under load from several threads at once (the loader's, the performance's, a script's).
 * Four threads share one loader and one performance for the given seconds:
 *   loader:      segments and scripts loaded again and again (a Load takes the loader's lock, then the
 *                performance's to put the new data in place);
 *   performance: those segments played and stopped (a script track's Init can load what its script references);
 *   collector:   CollectGarbage / ClearCache / ReleaseObjectByUnknown (objects zombied and let go of);
 *   script:      a script's routines called (its lock held while they touch the loader and the performance).
 * A watchdog reports a deadlock (no thread moving for 10 s) and ends the process with exit code 3.
 * Run in a folder with the content: lockstress.exe <seconds> <segment>... (scripts: *.spt found by the segments) */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <stdio.h>
#include <stdlib.h>
#include <dsound.h>
#include <shlwapi.h>

#define IDirectMusicScript_Init(p,a,b) (p)->lpVtbl->Init(p,a,b)
#define IDirectMusicScript_CallRoutine(p,a,b) (p)->lpVtbl->CallRoutine(p,a,b)
#define IDirectMusicScript_EnumRoutine(p,a,b) (p)->lpVtbl->EnumRoutine(p,a,b)
#define IDirectMusicScript_Release(p) (p)->lpVtbl->Release(p)

static IDirectMusicLoader8 *g_loader;
static IDirectMusicPerformance8 *g_perf;
static volatile LONG g_stop, g_moves[4], g_errors;
static WCHAR g_segs[16][MAX_PATH];
static int g_nsegs;
static WCHAR g_script[MAX_PATH];

static IDirectMusicSegment8 *load_seg(int k)
{
    IDirectMusicSegment8 *s = NULL;
    IDirectMusicLoader8_LoadObjectFromFile(g_loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, g_segs[k], (void **)&s);
    return s;
}

static DWORD WINAPI th_loader(void *p)
{
    CoInitialize(NULL);
    for (int n = 0; !g_stop; n++) {
        IDirectMusicSegment8 *s = load_seg(n % g_nsegs);
        if (s) {
            /* the (cached, maybe playing) segment loaded again from its file */
            IPersistStream *ps = NULL;
            IStream *f = NULL;
            if ((n & 3) == 0 && SUCCEEDED(IDirectMusicSegment8_QueryInterface(s, &IID_IPersistStream, (void **)&ps)) &&
                SUCCEEDED(SHCreateStreamOnFileW(g_segs[n % g_nsegs], STGM_READ, &f)))
                IPersistStream_Load(ps, f);
            if (f) IStream_Release(f);
            if (ps) IPersistStream_Release(ps);
            IDirectMusicSegment8_Release(s);
        }
        if (g_script[0]) {
            IUnknown *o = NULL;
            IDirectMusicLoader8_LoadObjectFromFile(g_loader, &CLSID_DirectMusicScript, &IID_IUnknown, g_script, (void **)&o);
            if (o) IUnknown_Release(o);
        }
        InterlockedIncrement(&g_moves[0]);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_perf(void *p)
{
    CoInitialize(NULL);
    for (int n = 0; !g_stop; n++) {
        IDirectMusicSegment8 *s = load_seg((n * 7 + 3) % g_nsegs);
        if (s) {
            IDirectMusicSegment8_Download(s, (IUnknown *)g_perf);
            IDirectMusicSegmentState *st = NULL;
            IDirectMusicPerformance8_PlaySegmentEx(g_perf, (IUnknown *)s, NULL, NULL, (n & 1) ? DMUS_SEGF_SECONDARY : 0, 0, &st, NULL, NULL);
            Sleep(n % 5);
            if (n % 3 == 0) IDirectMusicPerformance8_StopEx(g_perf, (IUnknown *)s, 0, 0);
            if (st) IDirectMusicSegmentState_Release(st);
            IDirectMusicSegment8_Unload(s, (IUnknown *)g_perf);
            IDirectMusicSegment8_Release(s);
        }
        InterlockedIncrement(&g_moves[1]);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_collector(void *p)
{
    CoInitialize(NULL);
    for (int n = 0; !g_stop; n++) {
        switch (n % 4) {
        case 0: IDirectMusicLoader8_CollectGarbage(g_loader); break;
        case 1: IDirectMusicLoader8_ClearCache(g_loader, &CLSID_DirectMusicSegment); break;
        case 2: IDirectMusicLoader8_ClearCache(g_loader, &GUID_DirectMusicAllTypes); break;
        case 3: {
            IDirectMusicSegment8 *s = load_seg(n % g_nsegs);
            if (s) { IDirectMusicLoader8_ReleaseObjectByUnknown(g_loader, (IUnknown *)s); IDirectMusicSegment8_Release(s); }
            break;
        }
        }
        Sleep(1);
        InterlockedIncrement(&g_moves[2]);
    }
    CoUninitialize();
    return 0;
}

static DWORD WINAPI th_script(void *p)
{
    CoInitialize(NULL);
    for (int n = 0; !g_stop; n++) {
        IDirectMusicScript *s = NULL;
        if (g_script[0])
            IDirectMusicLoader8_LoadObjectFromFile(g_loader, &CLSID_DirectMusicScript, &IID_IDirectMusicScript, g_script, (void **)&s);
        if (s) {
            DMUS_SCRIPT_ERRORINFO e = { sizeof(e) };
            IDirectMusicScript_Init(s, (IDirectMusicPerformance *)g_perf, &e);
            WCHAR name[64];
            for (DWORD k = 0; k < 8 && IDirectMusicScript_EnumRoutine(s, k, name) == S_OK; k++) {
                e.dwSize = sizeof(e);
                IDirectMusicScript_CallRoutine(s, name, &e);
            }
            IDirectMusicScript_Release(s);
        } else Sleep(1);
        InterlockedIncrement(&g_moves[3]);
    }
    CoUninitialize();
    return 0;
}

int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;     /* reAL loaded as a DirectSound game loads it */
    int secs = argc > 1 ? atoi(argv[1]) : 20;
    for (int i = 2; i < argc && g_nsegs < 16; i++) {
        const char *a = argv[i];
        size_t n = strlen(a);
        if (n > 4 && !_stricmp(a + n - 4, ".spt")) MultiByteToWideChar(CP_ACP, 0, a, -1, g_script, MAX_PATH);
        else MultiByteToWideChar(CP_ACP, 0, a, -1, g_segs[g_nsegs++], MAX_PATH);
    }
    if (!g_nsegs) { fprintf(stderr, "usage: lockstress seconds segment... [script.spt]\n"); return 2; }
    CoInitialize(NULL);
    WCHAR dir[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, dir);
    if (FAILED(CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&g_loader)) ||
        FAILED(CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&g_perf)))
        return 1;
    HRESULT hr = IDirectMusicPerformance8_InitAudio(g_perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    printf("InitAudio %08lx\n", hr);
    IDirectMusicLoader8_SetSearchDirectory(g_loader, &GUID_DirectMusicAllTypes, dir, FALSE);
    LPTHREAD_START_ROUTINE fn[4] = { th_loader, th_perf, th_collector, th_script };
    HANDLE th[4];
    for (int k = 0; k < 4; k++) th[k] = CreateThread(NULL, 0, fn[k], NULL, 0, NULL);
    LONG last[4] = { 0 };
    DWORD still = GetTickCount(), end = GetTickCount() + secs * 1000;
    while ((LONG)(GetTickCount() - end) < 0) {
        Sleep(250);
        int moved = 0;
        for (int k = 0; k < 4; k++) if (g_moves[k] != last[k]) { moved = 1; last[k] = g_moves[k]; }
        if (moved) still = GetTickCount();
        else if (GetTickCount() - still > 10000) {
            printf("DEADLOCK: no thread moved for 10 s (loads %ld, plays %ld, collections %ld, script calls %ld)\n",
                   g_moves[0], g_moves[1], g_moves[2], g_moves[3]);
            fflush(stdout);
            TerminateProcess(GetCurrentProcess(), 3);
        }
    }
    g_stop = 1;
    if (WaitForMultipleObjects(4, th, TRUE, 15000) != WAIT_OBJECT_0) {
        printf("DEADLOCK at the end: a thread did not finish (loads %ld, plays %ld, collections %ld, script calls %ld)\n",
               g_moves[0], g_moves[1], g_moves[2], g_moves[3]);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 3);
    }
    printf("ok: loads %ld, plays %ld, collections %ld, script calls %ld\n", g_moves[0], g_moves[1], g_moves[2], g_moves[3]);
    IDirectMusicPerformance8_CloseDown(g_perf);
    IDirectMusicPerformance8_Release(g_perf);
    IDirectMusicLoader8_Release(g_loader);
    CoUninitialize();
    return 0;
}
