/* reAL's winmm.dll: put next to a game's exe, it plays the game's waveOut sound (and, with midi=true, its MIDI)
 * through reAL, for games that never load DirectSound.  In a Wine prefix it may also take the place of Wine's in
 * the system directory (install.sh --winmm), for every game there: Wine's is then winmm.dll.wine beside it.
 *
 * Every export goes on to the system's winmm.dll (stubs.S, from gen.py), except the waveOut, midiOut, midiStream
 * and mciSend* ones: their first call loads reAL (dsound.dll, found the usual way: next to the game first, then the
 * system's, which is reAL when it is installed into a Wine prefix) and asks it for each of them
 * (reAL_winmm_bind).  What reAL does not take, or everything when the dsound.dll found is not reAL, stays the
 * system's.  reAL is loaded on that first call rather than here: loading it while the loader lock is held would run
 * its setup inside the game's DLL loading. */
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include "names.h"

extern void *g_fn[NFN];                 /* where each export goes (stubs.S jumps through it; each starts at a
                                         * stub that asks proxy_target) */
static void *s_sysfn[NFN];              /* the system's own, for those reAL may take */
static HMODULE s_sys, s_self;
static char s_path[MAX_PATH + 16];      /* the system's winmm, as it is being loaded */
static int s_system;                    /* this copy is the one in the system directory (a Wine prefix) */
static volatile LONG s_ready;           /* DllMain has filled the tables */
static volatile LONG s_binding, s_bound;
static volatile DWORD s_binder;

/* reAL finds this on the winmm.dll loaded: the system's winmm is the one it hooks for MIDI, not this one */
HMODULE reAL_winmm_proxy(void) { return s_sys; }

/* another of reAL's winmm.dll in the process: a copy next to the game, when this one is the system's.  (By the
 * modules themselves: which of two the name winmm.dll finds depends on what was looked up last) */
static int other_proxy(void)
{
    int found = 0;
    HANDLE snap = INVALID_HANDLE_VALUE;
    for (int k = 0; k < 8 && snap == INVALID_HANDLE_VALUE; k++) {      /* (ERROR_BAD_LENGTH: the list changed; again) */
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;
    }
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    for (BOOL ok = Module32First(snap, &me); ok && !found; ok = Module32Next(snap, &me))
        found = me.hModule != s_self && GetProcAddress(me.hModule, "reAL_winmm_proxy");
    CloseHandle(snap);
    return found;
}

static void *sys_fn(HMODULE m, int k)
{
    FARPROC f = !m || m == s_self ? NULL
              : g_names[k].name ? GetProcAddress(m, g_names[k].name)
              : GetProcAddress(m, MAKEINTRESOURCEA(g_names[k].ord));
    return f ? (void *)f : (void *)g_names[k].missing;
}

/* Two modules are called winmm.dll when this copy sits next to the game: this one and the one it loads from the
 * system directory.  A DLL loaded later that imports winmm.dll should get this one, which was loaded first; but
 * Wine's loader tries the module it last looked up before it goes down its list, and every look at the system's
 * exports (here, and in reAL as it hooks them) makes that the system's: a DLL loaded meanwhile was given the
 * system's winmm and played past reAL (four runs of six).  Under Wine the other module's name is cut short in the
 * loader's list ("winmm.dl"; its path and its file stay as they are): the name winmm.dll is then this copy's
 * alone.  Called from DllMain, which holds the loader's lock. */
static void name_is_ours(HMODULE other)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (!nt || !GetProcAddress(nt, "wine_get_version")) return;
    PEB_LDR_DATA *ldr = NtCurrentTeb()->ProcessEnvironmentBlock->Ldr;
    for (LIST_ENTRY *e = ldr->InMemoryOrderModuleList.Flink; e != &ldr->InMemoryOrderModuleList; e = e->Flink) {
        LDR_DATA_TABLE_ENTRY *d = CONTAINING_RECORD(e, LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (d->DllBase != (void *)other) continue;
        UNICODE_STRING *base = (UNICODE_STRING *)d->Reserved4;      /* (BaseDllName) */
        static const WCHAR w[] = L"winmm.dll";
        int same = base->Buffer && base->Length == 18;
        for (int k = 0; same && k < 9; k++) same = (base->Buffer[k] | 0x20) == w[k];
        if (same) base->Length = 16;
        return;
    }
}

/* this thread is inside a DllMain, or loading a DLL: it holds the loader's lock (PEB.LoaderLock, 32-bit) */
static int in_loader(void)
{
    RTL_CRITICAL_SECTION *lock = *(RTL_CRITICAL_SECTION **)((char *)NtCurrentTeb()->ProcessEnvironmentBlock + 0xa0);
    return lock && lock->OwningThread == (HANDLE)(ULONG_PTR)GetCurrentThreadId();
}

/* reAL loaded, and each export it may take pointed at reAL's function or the system's */
static void bind_real(void)
{
    /* the system's copy, with another of reAL's in the process (a copy next to the game): reAL is that one's to
     * load.  What comes here is what it, or reAL, passes on to the system's (a copy of 1.8.1 or before passes it
     * to this one, taking it for the system's), and goes there */
    HMODULE real = s_system && other_proxy() ? NULL : LoadLibraryA("dsound.dll");
    FARPROC (WINAPI *take)(HMODULE, const char *) = NULL;
    if (real) *(FARPROC *)&take = GetProcAddress(real, "reAL_winmm_bind");
    if (!take && real) FreeLibrary(real);           /* not reAL: everything stays the system's */
    for (int k = 0; k < NFN; k++) {
        if (!g_names[k].lazy) continue;
        FARPROC f = take ? take(s_sys, g_names[k].name) : NULL;
        g_fn[k] = f ? (void *)f : s_sysfn[k];
    }
}

/* Where a call of export i goes, for an entry of g_fn not yet settled (stubs.S), any thread.
 *
 * Before DllMain is through: the real winmm is still loading, and what it loads may import winmm.dll, which is this
 * proxy, and call it from its own DllMain (Wine's msacm32 opens its drivers there when its cache is empty; with the
 * table still unset that was a jump to nothing, and no program importing winmm started).  Those calls go to the
 * real winmm's export, found on the module as it loads.
 *
 * After it, only the exports reAL may take come here, until reAL is bound, by the first of them to be called.
 * The calls made while reAL loads go to the system's on the thread that loads it (reAL's own among them); other
 * threads wait for it, so that what they open is reAL's too.  Not a thread that holds the loader's lock (a call
 * from a DllMain): loading reAL needs it, and that call goes to the system's at once.  Nor for ever: five seconds */
void *proxy_target(int i)
{
    if (!s_ready) return sys_fn(s_sys ? s_sys : GetModuleHandleA(s_path), i);
    if (s_bound) return g_fn[i];
    if (s_binder == GetCurrentThreadId()) return s_sysfn[i];
    if (in_loader()) return s_sysfn[i];             /* (a DllMain, or a WinmmProxyAttach: see above) */
    if (!InterlockedCompareExchange(&s_binding, 1, 0)) {
        s_binder = GetCurrentThreadId();
        bind_real();
        InterlockedExchange(&s_bound, 1);
        s_binder = 0;
        return g_fn[i];
    }
    for (DWORD t0 = GetTickCount(); !s_bound && GetTickCount() - t0 < 5000; ) Sleep(1);
    return s_bound ? g_fn[i] : s_sysfn[i];
}

/* Whether the 32-bit DLL at path exports the function named want, read from the file mapped as an image, so that
 * nothing in it runs */
static int dll_exports(const WCHAR *path, const char *want)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY | SEC_IMAGE, 0, 0, NULL);
    CloseHandle(f);
    if (!m) return 0;
    const BYTE *b = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    if (!b) return 0;
    int found = 0;
    const IMAGE_DOS_HEADER *dos = (const void *)b;
    const IMAGE_NT_HEADERS32 *nt = NULL;
    MEMORY_BASIC_INFORMATION mi;            /* (the headers' pages: what may be read before SizeOfImage is known) */
    if (VirtualQuery(b, &mi, sizeof(mi)) == sizeof(mi) && mi.RegionSize >= sizeof(*dos)
        && dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0
        && (SIZE_T)dos->e_lfanew <= mi.RegionSize - sizeof(IMAGE_NT_HEADERS32))
        nt = (const void *)(b + dos->e_lfanew);
    if (nt && nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386
        && (nt->FileHeader.Characteristics & IMAGE_FILE_DLL)
        && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC
        && nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
        DWORD size = nt->OptionalHeader.SizeOfImage;
        DWORD ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if (ex && ex < size && size - ex >= sizeof(IMAGE_EXPORT_DIRECTORY)) {
            const IMAGE_EXPORT_DIRECTORY *ed = (const void *)(b + ex);
            DWORD names = ed->AddressOfNames, count = ed->NumberOfNames;
            if (names && names < size && count <= (size - names) / 4) {
                const DWORD *name = (const void *)(b + names);
                for (DWORD k = 0; k < count && !found; k++) {
                    DWORD at = name[k], i = 0;
                    if (at >= size) continue;
                    const char *s = (const char *)b + at;
                    while (at + i < size && want[i] && s[i] == want[i]) i++;
                    found = !want[i] && at + i < size && !s[i];
                }
            }
        }
    }
    UnmapViewOfFile(b);
    return found;
}

/* Another wrapper (a DirectDraw or GDI layer, say) can come into a game through this proxy too, for a game that
 * would never load it: a DLL beside this copy, not loaded already, that exports WinmmProxyAttach, as void WINAPI WinmmProxyAttach(void)
 * under that plain name, is loaded and that is called.  Every .dll beside it is looked at, in the order the
 * directory lists them; the export is looked for before a DLL is loaded, so that none that hasn't it (another
 * wrapper the game may load itself, or not at all) runs here.  Not from the system directory's copy (a Wine
 * prefix's), which every program there has.  From DllMain, so that they are in before the game starts:
 * WinmmProxyAttach runs inside the loader's lock, as a DllMain does, and should do no more than one may */
static void attach_beside(void)
{
    WCHAR path[MAX_PATH];
    if (s_system) return;
    DWORD n = GetModuleFileNameW(s_self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    while (n && path[n - 1] != L'\\') n--;
    if (!n || n + 6 > MAX_PATH) return;
    path[n] = L'*'; path[n + 1] = L'.'; path[n + 2] = L'd'; path[n + 3] = L'l'; path[n + 4] = L'l'; path[n + 5] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(path, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        DWORD k = 0;
        while (fd.cFileName[k] && n + k < MAX_PATH - 1) { path[n + k] = fd.cFileName[k]; k++; }
        if (fd.cFileName[k]) continue;                  /* (too long a path) */
        path[n + k] = 0;
        if (k < 5 || lstrcmpiW(path + n + k - 4, L".dll")) continue;   /* (*.dll finds foo.dllx by its short name) */
        if (GetModuleHandleW(path)) continue;          /* (the game loads it itself) */
        if (!dll_exports(path, "WinmmProxyAttach")) continue;
        HMODULE m = LoadLibraryW(path);
        if (!m) continue;
        void (WINAPI *attach)(void) = NULL;
        *(FARPROC *)&attach = GetProcAddress(m, "WinmmProxyAttach");
        if (attach) attach();
        else FreeLibrary(m);    /* (not the file looked at: under Wine a builtin can be loaded in its place) */
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    s_self = (HMODULE)inst;
    char *path = s_path;
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;
    lstrcpyA(path + n, "\\winmm.dll");
    HMODULE held = s_sys = LoadLibraryA(path);
    if (s_sys == (HMODULE)inst) {
        /* this copy is the system's: installed for a whole Wine prefix (install.sh --winmm), with Wine's own kept
         * beside it as winmm.dll.wine */
        FreeLibrary(s_sys);
        s_sys = NULL;           /* (for a proxy that asks while that one loads: see below) */
        s_system = 1;
        lstrcpyA(path + n, "\\winmm.dll.wine");
        held = s_sys = LoadLibraryA(path);
    }
    /* the system's may be reAL's too (this copy next to a game in a prefix that has reAL's as the system's, or
     * winmm.dll.wine a copy of reAL's put there by hand): the real one is the one that proxy goes to.  Going to
     * the proxy instead, reAL would be handed its exports as the system's functions and call itself through them */
    for (int k = 0; s_sys && s_sys != (HMODULE)inst && k < 4; k++) {
        HMODULE (*through)(void) = NULL;
        *(FARPROC *)&through = GetProcAddress(s_sys, "reAL_winmm_proxy");
        if (!through) break;
        s_sys = through();      /* (NULL: that copy is still loading, and it is this one that it loads) */
    }
    if (!s_sys || s_sys == (HMODULE)inst || GetProcAddress(s_sys, "reAL_winmm_proxy")) {    /* (only proxies) */
        if (held && held != (HMODULE)inst) FreeLibrary(held);
        s_sys = NULL;
        return FALSE;
    }
    for (int k = 0; k < NFN; k++) {
        s_sysfn[k] = sys_fn(s_sys, k);
        if (!g_names[k].lazy) g_fn[k] = s_sysfn[k];     /* (the others when reAL is bound: proxy_target) */
    }
    if (held != s_self) name_is_ours(held);
    InterlockedExchange(&s_ready, 1);
    attach_beside();            /* (with the tables filled: what it loads may import winmm) */
    return TRUE;
}
