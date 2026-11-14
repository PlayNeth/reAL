/* reAL's winmm.dll: put next to a game's exe, it plays the game's waveOut sound (and, with midi=true, its MIDI)
 * through reAL, for games that never load DirectSound.
 *
 * Every export goes on to the system's winmm.dll (stubs.S, from gen.py), except the waveOut, midiOut, midiStream
 * and mciSend* ones: their first call loads reAL (dsound.dll, found the usual way: next to the game first, then the
 * system's, which is reAL when it is installed into a Wine prefix) and asks it for each of them
 * (reAL_winmm_bind).  What reAL does not take, or everything when the dsound.dll found is not reAL, stays the
 * system's.  reAL is loaded on that first call rather than here: loading it while the loader lock is held would run
 * its setup inside the game's DLL loading. */
#include <windows.h>
#include "names.h"

void *g_fn[NFN];                        /* where each export goes (stubs.S jumps through it) */
static void *s_sysfn[NFN];              /* the system's own, for those reAL may take */
static HMODULE s_sys;
static CRITICAL_SECTION s_cs;
static volatile LONG s_bound;

/* reAL finds this on the winmm.dll loaded: the system's winmm is the one it hooks for MIDI, not this one */
HMODULE reAL_winmm_proxy(void) { return s_sys; }

/* lazy (stubs.S), on the first call of an export reAL may take, any thread */
void proxy_bind(int i)
{
    (void)i;
    if (s_bound) return;
    EnterCriticalSection(&s_cs);
    if (!s_bound) {
        /* calls made while reAL loads (its own included, on this thread) go to the system's meanwhile */
        for (int k = 0; k < NFN; k++) if (g_names[k].lazy) g_fn[k] = s_sysfn[k];
        HMODULE real = LoadLibraryA("dsound.dll");
        FARPROC (WINAPI *bind)(HMODULE, const char *) = NULL;
        if (real) *(FARPROC *)&bind = GetProcAddress(real, "reAL_winmm_bind");
        if (bind) {
            for (int k = 0; k < NFN; k++) {
                if (!g_names[k].lazy) continue;
                FARPROC f = bind(s_sys, g_names[k].name);
                if (f) g_fn[k] = (void *)f;
            }
        } else if (real) FreeLibrary(real);         /* not reAL: everything stays the system's */
        InterlockedExchange(&s_bound, 1);
    }
    LeaveCriticalSection(&s_cs);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&s_cs);
    char path[MAX_PATH + 16];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;
    lstrcpyA(path + n, "\\winmm.dll");
    s_sys = LoadLibraryA(path);
    if (!s_sys || s_sys == (HMODULE)inst) return FALSE;     /* (this copy put in the system directory) */
    for (int k = 0; k < NFN; k++) {
        FARPROC f = g_names[k].name ? GetProcAddress(s_sys, g_names[k].name)
                                    : GetProcAddress(s_sys, MAKEINTRESOURCEA(g_names[k].ord));
        s_sysfn[k] = f ? (void *)f : (void *)g_names[k].missing;
        g_fn[k] = g_names[k].lazy ? (void *)g_names[k].lazy : s_sysfn[k];
    }
    return TRUE;
}
