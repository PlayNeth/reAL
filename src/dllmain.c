/* reAL - DLL entry points / exports */
#include "real.h"
#include "music/dmcom.h"
#include <string.h>
#include <stdio.h>

HMODULE g_self;
static HMODULE s_real;
static INIT_ONCE s_real_once = INIT_ONCE_STATIC_INIT;

/* {5245414C-0001-4C41-8000-436F6D704154}  "REAL" ... */
const GUID RL_DEVICE_GUID = { 0x5245414C, 0x0001, 0x4C41, { 0x80, 0x00, 0x43, 0x6F, 0x6D, 0x70, 0x41, 0x54 } };

static BOOL CALLBACK load_real(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    char path[MAX_PATH + 16];
    HMODULE h = NULL;
    if (g_cfg.real_dsound[0]) {
        h = LoadLibraryA(g_cfg.real_dsound);
    } else {
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH) {
            lstrcatA(path, "\\dsound.dll");
            h = LoadLibraryA(path);
        }
    }
    if (h == g_self) {
        /* We *are* the system dsound (global install). There is nothing to forward to. */
        FreeLibrary(h);
        h = NULL;
        LOG_INFO("real dsound: none (reAL is installed as the system dsound.dll; capture unavailable, "
                 "set realdsound= to a builtin/original copy to enable it)");
    } else {
        LOG_INFO("real dsound: %p", (void *)h);
    }
    s_real = h;
    return TRUE;
}

HMODULE rl_real_dsound(void)
{
    InitOnceExecuteOnce(&s_real_once, load_real, NULL, NULL);
    return s_real;
}

#define REAL_PROC(name) (rl_real_dsound() ? (void *)GetProcAddress(rl_real_dsound(), name) : NULL)

/* ---------------------------------------------------------------- reAL-Music: COM hooks
 * DirectMusic is a set of COM servers, so a game gets it from CoCreateInstance (or CoCreateInstanceEx /
 * CoGetClassObject).  With music enabled those functions are hot-patched in ole32/combase themselves, so every
 * caller is covered (protected executables, dynamic lookups, other DLLs), and the DirectMusic classes reAL
 * implements (loader, performance, segment, DirectMusic) are created in place; everything else goes on to COM. */
typedef HRESULT (WINAPI *cocreate_fn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT (WINAPI *cocreateex_fn)(REFCLSID, IUnknown *, DWORD, COSERVERINFO *, DWORD, MULTI_QI *);
typedef HRESULT (WINAPI *cogetclass_fn)(REFCLSID, DWORD, COSERVERINFO *, REFIID, LPVOID *);
static cocreate_fn s_cocreate;
static cocreateex_fn s_cocreateex;
static cogetclass_fn s_cogetclass;

static HRESULT WINAPI hook_cocreate(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID riid, LPVOID *out)
{
    if (clsid && rm_handles(clsid)) {
        char gb[64];
        LOG_CALL("CoCreateInstance(%s): reAL-Music", rl_guid_str(clsid, gb));
        if (!out) return E_POINTER;
        *out = NULL;
        if (outer) return CLASS_E_NOAGGREGATION;
        return rm_create(clsid, riid, out);
    }
    return s_cocreate(clsid, outer, ctx, riid, out);
}

static HRESULT WINAPI hook_cocreateex(REFCLSID clsid, IUnknown *outer, DWORD ctx, COSERVERINFO *si, DWORD n, MULTI_QI *qi)
{
    if (clsid && rm_handles(clsid) && n && qi) {
        char gb[64];
        LOG_CALL("CoCreateInstanceEx(%s): reAL-Music", rl_guid_str(clsid, gb));
        if (outer) return CLASS_E_NOAGGREGATION;
        IUnknown *u = NULL;
        HRESULT hr = rm_create(clsid, &IID_IUnknown, (void **)&u);
        DWORD ok = 0;
        for (DWORD i = 0; i < n; i++) {
            qi[i].pItf = NULL;
            qi[i].hr = FAILED(hr) ? hr : IUnknown_QueryInterface(u, qi[i].pIID, (void **)&qi[i].pItf);
            if (SUCCEEDED(qi[i].hr)) ok++;
        }
        if (u) IUnknown_Release(u);
        if (FAILED(hr)) return hr;
        return ok == n ? S_OK : ok ? CO_S_NOTALLINTERFACES : E_NOINTERFACE;
    }
    return s_cocreateex(clsid, outer, ctx, si, n, qi);
}

/* class factory for CoGetClassObject */
typedef struct { IClassFactory iface; CLSID clsid; LONG refs; } rm_factory;
static HRESULT WINAPI rmf_qi(IClassFactory *i, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) { *out = i; i->lpVtbl->AddRef(i); return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI rmf_addref(IClassFactory *i) { return InterlockedIncrement(&((rm_factory *)i)->refs); }
static ULONG WINAPI rmf_release(IClassFactory *i)
{
    LONG r = InterlockedDecrement(&((rm_factory *)i)->refs);
    if (!r) HeapFree(GetProcessHeap(), 0, i);
    return r;
}
static HRESULT WINAPI rmf_create(IClassFactory *i, IUnknown *outer, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    return rm_create(&((rm_factory *)i)->clsid, riid, out);
}
static HRESULT WINAPI rmf_lock(IClassFactory *i, BOOL l) { return S_OK; }
static IClassFactoryVtbl rmf_vtbl = { rmf_qi, rmf_addref, rmf_release, rmf_create, rmf_lock };

static HRESULT WINAPI hook_cogetclass(REFCLSID clsid, DWORD ctx, COSERVERINFO *si, REFIID riid, LPVOID *out)
{
    if (clsid && rm_handles(clsid)) {
        char gb[64];
        LOG_CALL("CoGetClassObject(%s): reAL-Music", rl_guid_str(clsid, gb));
        if (!out) return E_POINTER;
        *out = NULL;
        rm_factory *f = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*f));
        if (!f) return E_OUTOFMEMORY;
        f->iface.lpVtbl = &rmf_vtbl;
        f->clsid = *clsid;
        f->refs = 1;
        HRESULT hr = rmf_qi(&f->iface, riid, out);
        rmf_release(&f->iface);
        return hr;
    }
    return s_cogetclass(clsid, ctx, si, riid, out);
}

/* follow import thunks / jump stubs to the function body */
static BYTE *resolve_code(BYTE *p)
{
    for (int i = 0; i < 4 && p; i++) {
        if (p[0] == 0xE9) p = p + 5 + *(INT32 *)(p + 1);
        else if (p[0] == 0xEB) p = p + 2 + (signed char)p[1];
        else if (p[0] == 0xFF && p[1] == 0x25) p = **(BYTE ***)(p + 2);
        else break;
    }
    return p;
}

/* length of one instruction among those that start ordinary x86 function prologues; 0 = unknown */
static int insn_len(const BYTE *p)
{
    switch (p[0]) {
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57: return 1;   /* push reg */
    case 0x6A: return 2;                                                                    /* push imm8 */
    case 0x68: return 5;                                                                    /* push imm32 */
    case 0x8B: case 0x89: case 0x33: case 0x31:                                             /* mov/xor r,r/m */
        if ((p[1] & 0xC0) == 0xC0) return 2;
        if ((p[1] & 0xC7) == 0x45 || (p[1] & 0xC7) == 0x44) return (p[1] & 7) == 4 ? 4 : 3; /* [ebp+d8] / [esp+d8] */
        return 0;
    case 0x83: if ((p[1] & 0xC0) == 0xC0) return 3; return 0;                                 /* op r, imm8 */
    case 0x81: if ((p[1] & 0xC0) == 0xC0) return 6; return 0;                                 /* op r, imm32 */
    case 0xB8: case 0xB9: case 0xBA: return 5;                                              /* mov r, imm32 */
    default: return 0;
    }
}

/* patch fn so that it jumps to repl; returns a pointer that runs the original */
static void *hotpatch(void *fn, void *repl, const char *name)
{
    BYTE *p = resolve_code(fn);
    DWORD old;
    if (!p) return NULL;
    if (p[0] == 0x8B && p[1] == 0xFF) {             /* mov edi,edi with 5 bytes of padding before: hot-patch point */
        int pad = 1;
        for (int i = 1; i <= 5; i++) if (p[-i] != 0x90 && p[-i] != 0xCC) pad = 0;
        if (pad && VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old)) {
            p[-5] = 0xE9;
            *(INT32 *)(p - 4) = (INT32)((BYTE *)repl - p);
            *(volatile WORD *)p = 0xF9EB;           /* jmp short -7 */
            VirtualProtect(p - 5, 7, old, &old);
            FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
            LOG_INFO("music: hooked %s (hot-patch at %p)", name, (void *)p);
            return p + 2;
        }
    }
    /* generic: move whole prologue instructions (>= 5 bytes) into a trampoline */
    int len = 0;
    while (len < 5) { int n = insn_len(p + len); if (!n) break; len += n; }
    if (len < 5) {
        LOG_INFO("music: cannot hook %s: unknown prologue %02x %02x %02x %02x %02x %02x", name, p[0], p[1], p[2], p[3], p[4], p[5]);
        return NULL;
    }
    BYTE *t = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, p, len);
    t[len] = 0xE9;
    *(INT32 *)(t + len + 1) = (INT32)((p + len) - (t + len + 5));
    if (!VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &old)) return NULL;
    p[0] = 0xE9;
    *(INT32 *)(p + 1) = (INT32)((BYTE *)repl - (p + 5));
    for (int i = 5; i < len; i++) p[i] = 0x90;
    VirtualProtect(p, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, len);
    LOG_INFO("music: hooked %s (trampoline, %d bytes)", name, len);
    return t;
}

/* called at load (ole32 already mapped) and again from DirectSoundCreate, where ole32 can be loaded */
void rl_music_hook(int can_load)
{
    static LONG done, reported;
    if (!g_cfg.enabled || !g_cfg.music || done) return;
    static HANDLE mine;
    if (!mine) {   /* only one copy of reAL in a process hooks COM */
        char name[64];
        snprintf(name, sizeof(name), "Local\\reAL.music.%lu", (unsigned long)GetCurrentProcessId());
        HANDLE m = CreateMutexA(NULL, FALSE, name);
        if (m && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(m);
            done = 1;
            LOG_INFO("music: another reAL in this process already serves DirectMusic");
            return;
        }
        mine = m;
    }
    HMODULE ole = GetModuleHandleA("ole32.dll");
    if (!ole && can_load) ole = LoadLibraryA("ole32.dll");
    if (!ole) {
        if (!reported) { reported = 1; LOG_INFO("music: ole32 not loaded yet, hooking later"); }
        return;
    }
    done = 1;
    void *a = (void *)GetProcAddress(ole, "CoCreateInstance");
    void *b = (void *)GetProcAddress(ole, "CoCreateInstanceEx");
    void *c = (void *)GetProcAddress(ole, "CoGetClassObject");
    if (a) s_cocreate = (cocreate_fn)hotpatch(a, (void *)hook_cocreate, "CoCreateInstance");
    if (b) s_cocreateex = (cocreateex_fn)hotpatch(b, (void *)hook_cocreateex, "CoCreateInstanceEx");
    if (c) s_cogetclass = (cogetclass_fn)hotpatch(c, (void *)hook_cogetclass, "CoGetClassObject");
    if (!s_cocreate) LOG_INFO("music: CoCreateInstance not hooked; DirectMusic stays with Windows");
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        rl_config_load();
        rl_music_hook(0);
    }
    return TRUE;
}


/* ---------------------------------------------------------------- playback */

HRESULT WINAPI DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND *out, LPUNKNOWN outer)
{
    char gb[64];
    LOG_CALL("DirectSoundCreate(%s)", rl_guid_str(guid, gb));
    rl_music_hook(1);
    if (!g_cfg.enabled) {
        HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN) = REAL_PROC("DirectSoundCreate");
        return fn ? fn(guid, out, outer) : DSERR_NODRIVER;
    }
    if (!out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (outer) return DSERR_NOAGGREGATION;
    IDirectSound *ds;
    HRESULT hr = rl_dsound_create(&IID_IDirectSound, (void **)&ds, FALSE);
    if (FAILED(hr)) return hr;
    hr = IDirectSound_Initialize(ds, guid);
    if (FAILED(hr)) { IDirectSound_Release(ds); return hr; }
    *out = ds;
    return DS_OK;
}

HRESULT WINAPI DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8 *out, LPUNKNOWN outer)
{
    char gb[64];
    LOG_CALL("DirectSoundCreate8(%s)", rl_guid_str(guid, gb));
    rl_music_hook(1);
    if (!g_cfg.enabled) {
        HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN) = REAL_PROC("DirectSoundCreate8");
        return fn ? fn(guid, out, outer) : DSERR_NODRIVER;
    }
    if (!out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (outer) return DSERR_NOAGGREGATION;
    IDirectSound8 *ds;
    HRESULT hr = rl_dsound_create(&IID_IDirectSound8, (void **)&ds, TRUE);
    if (FAILED(hr)) return hr;
    hr = IDirectSound8_Initialize(ds, guid);
    if (FAILED(hr)) { IDirectSound8_Release(ds); return hr; }
    *out = ds;
    return DS_OK;
}

static const char  DEV_DESC_A[] = "reAL (Creative ALchemy compatible)";
static const WCHAR DEV_DESC_W[] = L"reAL (Creative ALchemy compatible)";

HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx)
{
    LOG_CALL("DirectSoundEnumerateA");
    if (!g_cfg.enabled) {
        HRESULT (WINAPI *fn)(LPDSENUMCALLBACKA, LPVOID) = REAL_PROC("DirectSoundEnumerateA");
        return fn ? fn(cb, ctx) : DSERR_NODRIVER;
    }
    if (!cb) return DSERR_INVALIDPARAM;
    if (cb(NULL, "Primary Sound Driver", "", ctx))
        cb((LPGUID)&RL_DEVICE_GUID, DEV_DESC_A, "dsound.dll", ctx);
    return DS_OK;
}

HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx)
{
    LOG_CALL("DirectSoundEnumerateW");
    if (!g_cfg.enabled) {
        HRESULT (WINAPI *fn)(LPDSENUMCALLBACKW, LPVOID) = REAL_PROC("DirectSoundEnumerateW");
        return fn ? fn(cb, ctx) : DSERR_NODRIVER;
    }
    if (!cb) return DSERR_INVALIDPARAM;
    if (cb(NULL, L"Primary Sound Driver", L"", ctx))
        cb((LPGUID)&RL_DEVICE_GUID, DEV_DESC_W, L"dsound.dll", ctx);
    return DS_OK;
}

/* ---------------------------------------------------------------- capture: forwarded */

HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID guid, LPDIRECTSOUNDCAPTURE *out, LPUNKNOWN outer)
{
    HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUNDCAPTURE *, LPUNKNOWN) = REAL_PROC("DirectSoundCaptureCreate");
    if (out) *out = NULL;
    return fn ? fn(guid, out, outer) : DSERR_NODRIVER;
}

HRESULT WINAPI DirectSoundCaptureCreate8(LPCGUID guid, LPDIRECTSOUNDCAPTURE8 *out, LPUNKNOWN outer)
{
    HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUNDCAPTURE8 *, LPUNKNOWN) = REAL_PROC("DirectSoundCaptureCreate8");
    if (out) *out = NULL;
    return fn ? fn(guid, out, outer) : DSERR_NODRIVER;
}

HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx)
{
    HRESULT (WINAPI *fn)(LPDSENUMCALLBACKA, LPVOID) = REAL_PROC("DirectSoundCaptureEnumerateA");
    return fn ? fn(cb, ctx) : DS_OK; /* no capture devices */
}

HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx)
{
    HRESULT (WINAPI *fn)(LPDSENUMCALLBACKW, LPVOID) = REAL_PROC("DirectSoundCaptureEnumerateW");
    return fn ? fn(cb, ctx) : DS_OK;
}

HRESULT WINAPI DirectSoundFullDuplexCreate(LPCGUID cap_dev, LPCGUID ren_dev, LPCDSCBUFFERDESC cdesc,
    LPCDSBUFFERDESC rdesc, HWND hwnd, DWORD level, LPDIRECTSOUNDFULLDUPLEX *fd,
    LPDIRECTSOUNDCAPTUREBUFFER8 *cbuf, LPDIRECTSOUNDBUFFER8 *rbuf, LPUNKNOWN outer)
{
    HRESULT (WINAPI *fn)(LPCGUID, LPCGUID, LPCDSCBUFFERDESC, LPCDSBUFFERDESC, HWND, DWORD,
        LPDIRECTSOUNDFULLDUPLEX *, LPDIRECTSOUNDCAPTUREBUFFER8 *, LPDIRECTSOUNDBUFFER8 *, LPUNKNOWN)
        = REAL_PROC("DirectSoundFullDuplexCreate");
    return fn ? fn(cap_dev, ren_dev, cdesc, rdesc, hwnd, level, fd, cbuf, rbuf, outer) : DSERR_NODRIVER;
}

HRESULT WINAPI GetDeviceID(LPCGUID src, LPGUID dst)
{
    if (!dst) return DSERR_INVALIDPARAM;
    /* only the default playback aliases and reAL's own GUID map here; other GUIDs may be capture devices */
    if (g_cfg.enabled && (!src || IsEqualGUID(src, &GUID_NULL) || IsEqualGUID(src, &DSDEVID_DefaultPlayback) ||
                          IsEqualGUID(src, &DSDEVID_DefaultVoicePlayback) || IsEqualGUID(src, &RL_DEVICE_GUID))) {
        *dst = RL_DEVICE_GUID; return DS_OK;
    }
    HRESULT (WINAPI *fn)(LPCGUID, LPGUID) = REAL_PROC("GetDeviceID");
    return fn ? fn(src, dst) : DSERR_NODRIVER;
}

/* ---------------------------------------------------------------- COM class factory */

typedef struct { IClassFactory IClassFactory_iface; BOOL ds8; } rl_factory;

static HRESULT WINAPI cf_QueryInterface(IClassFactory *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI cf_AddRef(IClassFactory *iface) { (void)iface; return 2; }
static ULONG WINAPI cf_Release(IClassFactory *iface) { (void)iface; return 1; }
static HRESULT WINAPI cf_CreateInstance(IClassFactory *iface, IUnknown *outer, REFIID riid, void **out)
{
    rl_factory *f = impl_from(iface, rl_factory, IClassFactory_iface);
    if (!out) return E_POINTER;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    return rl_dsound_create(riid, out, f->ds8);
}
static HRESULT WINAPI cf_LockServer(IClassFactory *iface, BOOL lock) { (void)iface; (void)lock; return S_OK; }

static IClassFactoryVtbl cf_vtbl = { cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance, cf_LockServer };
static rl_factory s_cf_ds  = { { &cf_vtbl }, FALSE };
static rl_factory s_cf_ds8 = { { &cf_vtbl }, TRUE };

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out)
{
    char gb[64];
    LOG_CALL("DllGetClassObject(%s)", rl_guid_str(clsid, gb));
    /* DirectSound's private device-enumeration class: ALchemy does not offer it (CLASS_E_CLASSNOTAVAILABLE).
     * Forwarding it hands games the real dsound's device GUIDs instead of reAL's */
    static const CLSID clsid_private = { 0x11ab3ec0, 0x25ec, 0x11d1, { 0xa4, 0xd8, 0x00, 0xc0, 0x4f, 0xc2, 0x8a, 0xca } };
    if (g_cfg.enabled && IsEqualCLSID(clsid, &clsid_private)) {
        if (out) *out = NULL;
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    if (!g_cfg.enabled || (!IsEqualCLSID(clsid, &CLSID_DirectSound) && !IsEqualCLSID(clsid, &CLSID_DirectSound8))) {
        HRESULT (WINAPI *fn)(REFCLSID, REFIID, LPVOID *) = REAL_PROC("DllGetClassObject");
        if (out) *out = NULL;
        return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
    }
    rl_factory *f = IsEqualCLSID(clsid, &CLSID_DirectSound8) ? &s_cf_ds8 : &s_cf_ds;
    return cf_QueryInterface(&f->IClassFactory_iface, riid, out);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}
