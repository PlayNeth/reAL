/* reAL - DLL entry points / exports */
#include "real.h"
#include <string.h>

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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        rl_config_load();
    }
    return TRUE;
}

static BOOL is_playback_guid(LPCGUID g)
{
    return !g || IsEqualGUID(g, &GUID_NULL) || IsEqualGUID(g, &DSDEVID_DefaultPlayback) ||
           IsEqualGUID(g, &DSDEVID_DefaultVoicePlayback) || IsEqualGUID(g, &RL_DEVICE_GUID);
}

/* ---------------------------------------------------------------- playback */

HRESULT WINAPI DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND *out, LPUNKNOWN outer)
{
    char gb[64];
    LOG_CALL("DirectSoundCreate(%s)", rl_guid_str(guid, gb));
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
    if (g_cfg.enabled && is_playback_guid(src)) { *dst = RL_DEVICE_GUID; return DS_OK; }
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
