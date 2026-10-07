/* dstrace - DirectSound API tracer.
 *
 * Build: produces a dsound.dll that loads another dsound implementation (ALchemy, Wine builtin,
 * DSOAL, reAL...) and logs every call the game makes plus the result, including the raw data of
 * EAX property sets. Objects are traced by swapping their vtable pointer to a logging copy, so the
 * target sees its own objects (RTTI slot is preserved for dynamic_cast).
 *
 * Config (all optional), in dstrace.ini next to this dll:
 *   target=alchemy_dsound.dll     dll to forward to (relative = next to this dll). Default:
 *                                 alchemy_dsound.dll if present, else <system dir>\dsound.dll
 *   level=1                       1 = everything except per-frame calls, 2 = also Lock/Unlock/
 *                                 GetCurrentPosition/GetStatus/AddRef/Release
 *   logfile=...                   default: dstrace.log next to the game exe
 * Environment overrides: DSTRACE_TARGET, DSTRACE_LEVEL.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

static HMODULE g_self, g_target;
static int g_level = 1;
static FILE *g_log;
static CRITICAL_SECTION g_cs;
static char g_target_path[MAX_PATH];

/* ======================================================================== logging */

static void L(const char *fmt, ...)
{
    if (!g_log) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n] = 0;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "%8lu %04lx %s\n", (unsigned long)GetTickCount(), (unsigned long)GetCurrentThreadId(), buf);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}
#define L2(...) do { if (g_level >= 2) L(__VA_ARGS__); } while (0)

static const char *hrs(HRESULT hr, char *b)
{
    static const struct { HRESULT hr; const char *s; } t[] = {
        { DS_OK, "DS_OK" }, { S_FALSE, "S_FALSE" }, { DS_NO_VIRTUALIZATION, "DS_NO_VIRTUALIZATION" },
        { DSERR_ALLOCATED, "DSERR_ALLOCATED" }, { DSERR_CONTROLUNAVAIL, "DSERR_CONTROLUNAVAIL" },
        { DSERR_INVALIDPARAM, "DSERR_INVALIDPARAM" }, { DSERR_INVALIDCALL, "DSERR_INVALIDCALL" },
        { DSERR_GENERIC, "DSERR_GENERIC" }, { DSERR_PRIOLEVELNEEDED, "DSERR_PRIOLEVELNEEDED" },
        { DSERR_OUTOFMEMORY, "DSERR_OUTOFMEMORY" }, { DSERR_BADFORMAT, "DSERR_BADFORMAT" },
        { DSERR_UNSUPPORTED, "DSERR_UNSUPPORTED" }, { DSERR_NODRIVER, "DSERR_NODRIVER" },
        { DSERR_ALREADYINITIALIZED, "DSERR_ALREADYINITIALIZED" }, { DSERR_NOAGGREGATION, "DSERR_NOAGGREGATION" },
        { DSERR_BUFFERLOST, "DSERR_BUFFERLOST" }, { DSERR_OTHERAPPHASPRIO, "DSERR_OTHERAPPHASPRIO" },
        { DSERR_UNINITIALIZED, "DSERR_UNINITIALIZED" }, { DSERR_NOINTERFACE, "E_NOINTERFACE" },
        { DSERR_ACCESSDENIED, "DSERR_ACCESSDENIED" }, { DSERR_BUFFERTOOSMALL, "DSERR_BUFFERTOOSMALL" },
        { DSERR_DS8_REQUIRED, "DSERR_DS8_REQUIRED" }, { DSERR_FXUNAVAILABLE, "DSERR_FXUNAVAILABLE" },
        { DSERR_OBJECTNOTFOUND, "DSERR_OBJECTNOTFOUND" }, { E_NOTIMPL, "E_NOTIMPL" },
        { (HRESULT)0x80070490L, "E_PROP_ID_UNSUPPORTED" }, { (HRESULT)0x80070492L, "E_PROP_SET_UNSUPPORTED" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (t[i].hr == hr) return t[i].s;
    sprintf(b, "%08lx", (unsigned long)hr);
    return b;
}
#define HRS(hr) hrs(hr, hb)

static const char *guids(const GUID *g, char *b)
{
    static const struct { GUID g; const char *s; } t[] = {
        { { 0x00000000, 0x0000, 0x0000, { 0 } }, "GUID_NULL" },
        { { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } }, "IID_IUnknown" },
        { { 0x279AFA83, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IID_IDirectSound" },
        { { 0xC50A7E93, 0xF395, 0x4834, { 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66 } }, "IID_IDirectSound8" },
        { { 0x279AFA85, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IID_IDirectSoundBuffer" },
        { { 0x6825A449, 0x7524, 0x4D82, { 0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E } }, "IID_IDirectSoundBuffer8" },
        { { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IID_IDirectSound3DBuffer" },
        { { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } }, "IID_IDirectSound3DListener" },
        { { 0xB0210783, 0x89CD, 0x11D0, { 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 } }, "IID_IDirectSoundNotify" },
        { { 0x31EFAC30, 0x515C, 0x11D0, { 0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93 } }, "IID_IKsPropertySet" },
        { { 0x00000001, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } }, "IID_IClassFactory" },
        { { 0x47D4D946, 0x62E8, 0x11CF, { 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } }, "CLSID_DirectSound" },
        { { 0x3901CC3F, 0x84B5, 0x4FA4, { 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B } }, "CLSID_DirectSound8" },
        { { 0x4a4e6fc1, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } }, "EAX1_Listener" },
        { { 0x4a4e6fc0, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } }, "EAX1_Buffer" },
        { { 0x0306a6a8, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } }, "EAX2_Listener" },
        { { 0x0306a6a7, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } }, "EAX2_Buffer" },
        { { 0xa8fa6882, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } }, "EAX3_Listener" },
        { { 0xa8fa6881, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } }, "EAX3_Buffer" },
        { { 0x1d4870ad, 0x0def, 0x43c0, { 0xa4, 0x0c, 0x52, 0x36, 0x32, 0x29, 0x63, 0x42 } }, "EAX4_Context" },
        { { 0xc4d79f1e, 0xf1ac, 0x436b, { 0xa8, 0x1d, 0xa7, 0x38, 0xe7, 0x04, 0x54, 0x69 } }, "EAX4_FXSlot0" },
        { { 0x08c00e96, 0x74be, 0x4491, { 0x93, 0xaa, 0xe8, 0xad, 0x35, 0xa4, 0x91, 0x17 } }, "EAX4_FXSlot1" },
        { { 0x1d433b88, 0xf0f6, 0x4637, { 0x91, 0x9f, 0x60, 0xe7, 0xe0, 0x6b, 0x5e, 0xdd } }, "EAX4_FXSlot2" },
        { { 0xefff08ea, 0xc7d8, 0x44ab, { 0x93, 0xad, 0x6d, 0xbd, 0x5f, 0x91, 0x00, 0x64 } }, "EAX4_FXSlot3" },
        { { 0x1b86b823, 0x22df, 0x4eae, { 0x8b, 0x3c, 0x12, 0x78, 0xce, 0x54, 0x42, 0x27 } }, "EAX4_Source" },
        { { 0x57e13437, 0xb932, 0x4ab2, { 0xb8, 0xbd, 0x52, 0x66, 0xc1, 0xa8, 0x87, 0xee } }, "EAX5_Context" },
        { { 0x91f9590f, 0xc388, 0x407a, { 0x84, 0xb0, 0x1b, 0xae, 0x0e, 0xf7, 0x1a, 0xbc } }, "EAX5_FXSlot0" },
        { { 0x8f5f7aca, 0x9608, 0x4965, { 0x81, 0x37, 0x82, 0x13, 0xc7, 0xb9, 0xd9, 0xde } }, "EAX5_FXSlot1" },
        { { 0x3c0f5252, 0x9834, 0x46f0, { 0xa1, 0xd8, 0x5b, 0x95, 0xc4, 0xa0, 0x0a, 0x30 } }, "EAX5_FXSlot2" },
        { { 0xe2eb0eaa, 0xe806, 0x45e7, { 0x9f, 0x86, 0x06, 0xc1, 0x57, 0x1a, 0x6f, 0xa3 } }, "EAX5_FXSlot3" },
        { { 0x5edf82f0, 0x24a7, 0x4f38, { 0x8e, 0x64, 0x2f, 0x09, 0xca, 0x05, 0xde, 0xe1 } }, "EAX5_Source" },
        { { 0x62a69bae, 0xdf9d, 0x11d1, { 0x99, 0xa6, 0x00, 0xc0, 0x4f, 0xc9, 0x9d, 0x46 } }, "DSPROPSETID_VoiceManager" },
    };
    if (!g) return "(null)";
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (IsEqualGUID(g, &t[i].g)) return t[i].s;
    sprintf(b, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", (unsigned long)g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return b;
}
#define GS(g) guids(g, gb)

static void flagstr(DWORD f, char *out)
{
    static const struct { DWORD f; const char *s; } t[] = {
        { DSBCAPS_PRIMARYBUFFER, "PRIMARY" }, { DSBCAPS_STATIC, "STATIC" }, { DSBCAPS_LOCHARDWARE, "LOCHW" },
        { DSBCAPS_LOCSOFTWARE, "LOCSW" }, { DSBCAPS_CTRL3D, "3D" }, { DSBCAPS_CTRLFREQUENCY, "FREQ" },
        { DSBCAPS_CTRLPAN, "PAN" }, { DSBCAPS_CTRLVOLUME, "VOL" }, { DSBCAPS_CTRLPOSITIONNOTIFY, "NOTIFY" },
        { DSBCAPS_CTRLFX, "FX" }, { DSBCAPS_STICKYFOCUS, "STICKY" }, { DSBCAPS_GLOBALFOCUS, "GLOBAL" },
        { DSBCAPS_GETCURRENTPOSITION2, "GETPOS2" }, { DSBCAPS_MUTE3DATMAXDISTANCE, "MUTEMAX" }, { DSBCAPS_LOCDEFER, "LOCDEFER" },
    };
    out[0] = 0;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (f & t[i].f) { strcat(out, t[i].s); strcat(out, "|"); }
    size_t n = strlen(out);
    if (n) out[n - 1] = 0; else strcpy(out, "0");
}

static void fmtstr(const WAVEFORMATEX *w, char *out)
{
    if (!w) { strcpy(out, "(no format)"); return; }
    sprintf(out, "tag %u %lu Hz %u ch %u bit", w->wFormatTag, (unsigned long)w->nSamplesPerSec, w->nChannels, w->wBitsPerSample);
}

/* raw dump of a property payload: each dword as hex, int and float */
static void dump(const void *data, ULONG len, char *out, size_t outsz)
{
    out[0] = 0;
    if (!data || !len) return;
    const BYTE *p = data;
    size_t o = 0;
    for (ULONG i = 0; i + 4 <= len && o + 48 < outsz; i += 4) {
        DWORD d; float f;
        memcpy(&d, p + i, 4); memcpy(&f, p + i, 4);
        if ((d & 0x7f800000) && (d & 0x7f800000) != 0x7f800000 && f > -1e6f && f < 1e6f)
            o += sprintf(out + o, " %.4g", f);
        else
            o += sprintf(out + o, " %ld", (long)d);
    }
    for (ULONG i = len & ~3u; i < len && o + 8 < outsz; i++) o += sprintf(out + o, " %02x", p[i]);
}

/* ======================================================================== vtable hooking */

enum kind { K_DS, K_DSB, K_3DB, K_3DL, K_NOTIFY, K_KS, K_CF, K_COUNT };
static const int kind_slots[K_COUNT] = { 12, 24, 21, 18, 4, 6, 5 };
static const char *kind_name[K_COUNT] = { "DS", "DSB", "3DB", "LST", "NTF", "KS", "CF" };
static void *hook_tables[K_COUNT][24];   /* our logging functions per kind (NULL = pass through) */

typedef struct { void **orig; void **copy; enum kind k; } vt_entry;
static vt_entry g_vts[256];
static int g_nvts;

static void **find_orig(void *obj, int *k)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < g_nvts; i++)
        if (g_vts[i].copy == vt) { if (k) *k = g_vts[i].k; return g_vts[i].orig; }
    return vt; /* not hooked (shouldn't happen) */
}

static void *orig_fn(void *obj, int idx) { return find_orig(obj, NULL)[idx]; }

static void hook(void *obj, enum kind k)
{
    if (!obj) return;
    void **vt = *(void ***)obj;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nvts; i++)
        if (g_vts[i].copy == vt) { LeaveCriticalSection(&g_cs); return; }  /* already hooked */
    void **copy = NULL;
    for (int i = 0; i < g_nvts; i++)
        if (g_vts[i].orig == vt && g_vts[i].k == k) { copy = g_vts[i].copy; break; }
    if (!copy && g_nvts < 256) {
        int n = kind_slots[k];
        void **mem = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(void *));
        mem[0] = vt[-1];                  /* MSVC RTTI complete object locator */
        copy = mem + 1;
        for (int i = 0; i < n; i++) copy[i] = hook_tables[k][i] ? hook_tables[k][i] : vt[i];
        g_vts[g_nvts].orig = vt; g_vts[g_nvts].copy = copy; g_vts[g_nvts].k = k;
        g_nvts++;
    }
    if (copy) *(void ***)obj = copy;
    LeaveCriticalSection(&g_cs);
}

static enum kind kind_for_iid(REFIID riid)
{
    if (IsEqualIID(riid, &IID_IDirectSound) || IsEqualIID(riid, &IID_IDirectSound8)) return K_DS;
    if (IsEqualIID(riid, &IID_IDirectSoundBuffer) || IsEqualIID(riid, &IID_IDirectSoundBuffer8)) return K_DSB;
    if (IsEqualIID(riid, &IID_IDirectSound3DBuffer)) return K_3DB;
    if (IsEqualIID(riid, &IID_IDirectSound3DListener)) return K_3DL;
    if (IsEqualIID(riid, &IID_IDirectSoundNotify)) return K_NOTIFY;
    if (IsEqualIID(riid, &IID_IKsPropertySet)) return K_KS;
    if (IsEqualIID(riid, &IID_IClassFactory)) return K_CF;
    return K_COUNT;
}

/* generic call helper: CALL(type-of-fn, slot)(This, args...) */
#define CALL(T, slot) ((T)orig_fn(This, slot))

/* shared IUnknown hooks (slot 0-2) */
static HRESULT WINAPI h_QueryInterface(IUnknown *This, REFIID riid, void **out)
{
    char gb[96], hb[16];
    int k = 0;
    find_orig(This, &k);
    HRESULT hr = CALL(HRESULT (WINAPI *)(IUnknown *, REFIID, void **), 0)(This, riid, out);
    L("%s %p QueryInterface(%s) -> %s %p", kind_name[k], (void *)This, GS(riid), HRS(hr), out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) {
        enum kind nk = kind_for_iid(riid);
        if (nk != K_COUNT) hook(*out, nk);
    }
    return hr;
}

static ULONG WINAPI h_AddRef(IUnknown *This)
{
    int k = 0; find_orig(This, &k);
    ULONG r = CALL(ULONG (WINAPI *)(IUnknown *), 1)(This);
    L2("%s %p AddRef -> %lu", kind_name[k], (void *)This, (unsigned long)r);
    return r;
}

static ULONG WINAPI h_Release(IUnknown *This)
{
    int k = 0; find_orig(This, &k);
    ULONG (WINAPI *fn)(IUnknown *) = (ULONG (WINAPI *)(IUnknown *))orig_fn(This, 2);
    ULONG r = fn(This);
    if (g_level >= 2 || r == 0) L("%s %p Release -> %lu", kind_name[k], (void *)This, (unsigned long)r);
    return r;
}

/* ======================================================================== IDirectSound8 */

static HRESULT WINAPI ds_CreateSoundBuffer(IDirectSound8 *This, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER *out, LPUNKNOWN o)
{
    char hb[16], fl[200], wf[96], gb[96];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, LPUNKNOWN), 3)(This, d, out, o);
    flagstr(d ? d->dwFlags : 0, fl);
    fmtstr(d ? d->lpwfxFormat : NULL, wf);
    L("DS %p CreateSoundBuffer(flags %s, %lu bytes, %s, 3dalg %s) -> %s %p", (void *)This, fl,
      d ? (unsigned long)d->dwBufferBytes : 0ul, wf,
      d && d->dwSize >= sizeof(DSBUFFERDESC) ? GS(&d->guid3DAlgorithm) : "-", HRS(hr), out ? (void *)*out : NULL);
    if (SUCCEEDED(hr) && out) hook(*out, K_DSB);
    return hr;
}

static HRESULT WINAPI ds_GetCaps(IDirectSound8 *This, LPDSCAPS c)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, LPDSCAPS), 4)(This, c);
    if (SUCCEEDED(hr) && c)
        L("DS %p GetCaps -> %s flags %lx hw3d %lu/%lu hwmix %lu/%lu rate %lu-%lu hwmem %lu", (void *)This, HRS(hr),
          (unsigned long)c->dwFlags, (unsigned long)c->dwFreeHw3DAllBuffers, (unsigned long)c->dwMaxHw3DAllBuffers,
          (unsigned long)c->dwFreeHwMixingAllBuffers, (unsigned long)c->dwMaxHwMixingAllBuffers,
          (unsigned long)c->dwMinSecondarySampleRate, (unsigned long)c->dwMaxSecondarySampleRate,
          (unsigned long)c->dwTotalHwMemBytes);
    else L("DS %p GetCaps -> %s", (void *)This, HRS(hr));
    return hr;
}

static HRESULT WINAPI ds_DuplicateSoundBuffer(IDirectSound8 *This, LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER *out)
{
    char hb[16];
    /* hand the target its own vtable during the call in case it type-checks the source */
    void **hooked = src ? *(void ***)src : NULL;
    void **orig = src ? find_orig(src, NULL) : NULL;
    if (src) *(void ***)src = orig;
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER *), 5)(This, src, out);
    if (src) *(void ***)src = hooked;
    L("DS %p DuplicateSoundBuffer(%p) -> %s %p", (void *)This, (void *)src, HRS(hr), out ? (void *)*out : NULL);
    if (SUCCEEDED(hr) && out) hook(*out, K_DSB);
    return hr;
}

static HRESULT WINAPI ds_SetCooperativeLevel(IDirectSound8 *This, HWND h, DWORD lvl)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, HWND, DWORD), 6)(This, h, lvl);
    L("DS %p SetCooperativeLevel(%lu) -> %s", (void *)This, (unsigned long)lvl, HRS(hr));
    return hr;
}

static HRESULT WINAPI ds_GetSpeakerConfig(IDirectSound8 *This, LPDWORD c)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, LPDWORD), 8)(This, c);
    L("DS %p GetSpeakerConfig -> %s %lx", (void *)This, HRS(hr), c ? (unsigned long)*c : 0ul);
    return hr;
}

static HRESULT WINAPI ds_SetSpeakerConfig(IDirectSound8 *This, DWORD c)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, DWORD), 9)(This, c);
    L("DS %p SetSpeakerConfig(%lx) -> %s", (void *)This, (unsigned long)c, HRS(hr));
    return hr;
}

static HRESULT WINAPI ds_Initialize(IDirectSound8 *This, LPCGUID g)
{
    char hb[16], gb[96];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, LPCGUID), 10)(This, g);
    L("DS %p Initialize(%s) -> %s", (void *)This, GS(g), HRS(hr));
    return hr;
}

static HRESULT WINAPI ds_VerifyCertification(IDirectSound8 *This, LPDWORD c)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound8 *, LPDWORD), 11)(This, c);
    L("DS %p VerifyCertification -> %s %lu", (void *)This, HRS(hr), c ? (unsigned long)*c : 0ul);
    return hr;
}

/* ======================================================================== IDirectSoundBuffer8 */

static HRESULT WINAPI dsb_GetCaps(IDirectSoundBuffer8 *This, LPDSBCAPS c)
{
    char hb[16], fl[200];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, LPDSBCAPS), 3)(This, c);
    flagstr(SUCCEEDED(hr) && c ? c->dwFlags : 0, fl);
    L("DSB %p GetCaps -> %s flags %s bytes %lu", (void *)This, HRS(hr), fl, c ? (unsigned long)c->dwBufferBytes : 0ul);
    return hr;
}

static HRESULT WINAPI dsb_GetCurrentPosition(IDirectSoundBuffer8 *This, LPDWORD p, LPDWORD w)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, LPDWORD, LPDWORD), 4)(This, p, w);
    L2("DSB %p GetCurrentPosition -> %s play %lu write %lu", (void *)This, HRS(hr),
       p ? (unsigned long)*p : 0ul, w ? (unsigned long)*w : 0ul);
    return hr;
}

static HRESULT WINAPI dsb_GetFormat(IDirectSoundBuffer8 *This, LPWAVEFORMATEX f, DWORD sz, LPDWORD wr)
{
    char hb[16], wf[96];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, LPWAVEFORMATEX, DWORD, LPDWORD), 5)(This, f, sz, wr);
    fmtstr(SUCCEEDED(hr) ? f : NULL, wf);
    L("DSB %p GetFormat -> %s %s", (void *)This, HRS(hr), wf);
    return hr;
}

#define DSB_GET_LONG(name, slot, T, fmt) \
static HRESULT WINAPI dsb_##name(IDirectSoundBuffer8 *This, T *v) \
{ \
    char hb[16]; \
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, T *), slot)(This, v); \
    L("DSB %p " #name " -> %s " fmt, (void *)This, HRS(hr), v ? *v : 0); \
    return hr; \
}
DSB_GET_LONG(GetVolume, 6, LONG, "%ld")
DSB_GET_LONG(GetPan, 7, LONG, "%ld")
DSB_GET_LONG(GetFrequency, 8, DWORD, "%lu")

static HRESULT WINAPI dsb_GetStatus(IDirectSoundBuffer8 *This, LPDWORD s)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, LPDWORD), 9)(This, s);
    L2("DSB %p GetStatus -> %s %lx", (void *)This, HRS(hr), s ? (unsigned long)*s : 0ul);
    return hr;
}

static HRESULT WINAPI dsb_Lock(IDirectSoundBuffer8 *This, DWORD off, DWORD n, LPVOID *p1, LPDWORD b1, LPVOID *p2, LPDWORD b2, DWORD f)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, DWORD, DWORD, LPVOID *, LPDWORD, LPVOID *, LPDWORD, DWORD), 11)
        (This, off, n, p1, b1, p2, b2, f);
    L2("DSB %p Lock(%lu, %lu, %lx) -> %s %lu+%lu", (void *)This, (unsigned long)off, (unsigned long)n, (unsigned long)f,
       HRS(hr), b1 ? (unsigned long)*b1 : 0ul, b2 ? (unsigned long)*b2 : 0ul);
    return hr;
}

static HRESULT WINAPI dsb_Play(IDirectSoundBuffer8 *This, DWORD r, DWORD prio, DWORD f)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, DWORD, DWORD, DWORD), 12)(This, r, prio, f);
    L("DSB %p Play(prio %lu, flags %lx) -> %s", (void *)This, (unsigned long)prio, (unsigned long)f, HRS(hr));
    return hr;
}

#define DSB_SET(name, slot, T, fmt) \
static HRESULT WINAPI dsb_##name(IDirectSoundBuffer8 *This, T v) \
{ \
    char hb[16]; \
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, T), slot)(This, v); \
    L("DSB %p " #name "(" fmt ") -> %s", (void *)This, v, HRS(hr)); \
    return hr; \
}
DSB_SET(SetCurrentPosition, 13, DWORD, "%lu")
DSB_SET(SetVolume, 15, LONG, "%ld")
DSB_SET(SetPan, 16, LONG, "%ld")
DSB_SET(SetFrequency, 17, DWORD, "%lu")

static HRESULT WINAPI dsb_SetFormat(IDirectSoundBuffer8 *This, LPCWAVEFORMATEX f)
{
    char hb[16], wf[96];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, LPCWAVEFORMATEX), 14)(This, f);
    fmtstr(f, wf);
    L("DSB %p SetFormat(%s) -> %s", (void *)This, wf, HRS(hr));
    return hr;
}

static HRESULT WINAPI dsb_Stop(IDirectSoundBuffer8 *This)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *), 18)(This);
    L("DSB %p Stop -> %s", (void *)This, HRS(hr));
    return hr;
}

static HRESULT WINAPI dsb_Unlock(IDirectSoundBuffer8 *This, LPVOID p1, DWORD b1, LPVOID p2, DWORD b2)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, LPVOID, DWORD, LPVOID, DWORD), 19)(This, p1, b1, p2, b2);
    L2("DSB %p Unlock(%lu+%lu) -> %s", (void *)This, (unsigned long)b1, (unsigned long)b2, HRS(hr));
    return hr;
}

static HRESULT WINAPI dsb_Restore(IDirectSoundBuffer8 *This)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *), 20)(This);
    L("DSB %p Restore -> %s", (void *)This, HRS(hr));
    return hr;
}

static HRESULT WINAPI dsb_SetFX(IDirectSoundBuffer8 *This, DWORD n, LPDSEFFECTDESC fx, LPDWORD res)
{
    char hb[16], gb[96];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, DWORD, LPDSEFFECTDESC, LPDWORD), 21)(This, n, fx, res);
    L("DSB %p SetFX(%lu, first %s) -> %s", (void *)This, (unsigned long)n, n && fx ? GS(&fx[0].guidDSFXClass) : "-", HRS(hr));
    return hr;
}

static HRESULT WINAPI dsb_AcquireResources(IDirectSoundBuffer8 *This, DWORD f, DWORD n, LPDWORD res)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundBuffer8 *, DWORD, DWORD, LPDWORD), 22)(This, f, n, res);
    L("DSB %p AcquireResources(%lx, %lu) -> %s", (void *)This, (unsigned long)f, (unsigned long)n, HRS(hr));
    return hr;
}

/* ======================================================================== IDirectSound3DBuffer */

static const char *ap(DWORD a) { return a == DS3D_DEFERRED ? "deferred" : "immediate"; }

static HRESULT WINAPI b3d_GetAllParameters(IDirectSound3DBuffer *This, LPDS3DBUFFER p)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DBuffer *, LPDS3DBUFFER), 3)(This, p);
    if (SUCCEEDED(hr) && p)
        L("3DB %p GetAllParameters -> pos %.3f,%.3f,%.3f min %.3f max %.3f mode %lu cone %lu/%lu %ld", (void *)This,
          p->vPosition.x, p->vPosition.y, p->vPosition.z, p->flMinDistance, p->flMaxDistance, (unsigned long)p->dwMode,
          (unsigned long)p->dwInsideConeAngle, (unsigned long)p->dwOutsideConeAngle, p->lConeOutsideVolume);
    else L("3DB %p GetAllParameters -> %s", (void *)This, HRS(hr));
    return hr;
}

static HRESULT WINAPI b3d_SetAllParameters(IDirectSound3DBuffer *This, LPCDS3DBUFFER p, DWORD a)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DBuffer *, LPCDS3DBUFFER, DWORD), 12)(This, p, a);
    if (p)
        L("3DB %p SetAllParameters(pos %.3f,%.3f,%.3f vel %.3f,%.3f,%.3f min %.3f max %.3f mode %lu cone %lu/%lu %ld %s) -> %s",
          (void *)This, p->vPosition.x, p->vPosition.y, p->vPosition.z, p->vVelocity.x, p->vVelocity.y, p->vVelocity.z,
          p->flMinDistance, p->flMaxDistance, (unsigned long)p->dwMode, (unsigned long)p->dwInsideConeAngle,
          (unsigned long)p->dwOutsideConeAngle, p->lConeOutsideVolume, ap(a), HRS(hr));
    return hr;
}

static HRESULT WINAPI b3d_SetConeAngles(IDirectSound3DBuffer *This, DWORD in, DWORD out, DWORD a)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DBuffer *, DWORD, DWORD, DWORD), 13)(This, in, out, a);
    L("3DB %p SetConeAngles(%lu, %lu, %s) -> %s", (void *)This, (unsigned long)in, (unsigned long)out, ap(a), HRS(hr));
    return hr;
}

#define B3D_SET_V(name, slot) \
static HRESULT WINAPI b3d_##name(IDirectSound3DBuffer *This, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD a) \
{ \
    char hb[16]; \
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DBuffer *, D3DVALUE, D3DVALUE, D3DVALUE, DWORD), slot)(This, x, y, z, a); \
    L("3DB %p " #name "(%.3f, %.3f, %.3f, %s) -> %s", (void *)This, x, y, z, ap(a), HRS(hr)); \
    return hr; \
}
B3D_SET_V(SetConeOrientation, 14)
B3D_SET_V(SetPosition, 19)
B3D_SET_V(SetVelocity, 20)

static HRESULT WINAPI b3d_SetConeOutsideVolume(IDirectSound3DBuffer *This, LONG v, DWORD a)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DBuffer *, LONG, DWORD), 15)(This, v, a);
    L("3DB %p SetConeOutsideVolume(%ld, %s) -> %s", (void *)This, v, ap(a), HRS(hr));
    return hr;
}

#define B3D_SET_F(name, slot) \
static HRESULT WINAPI b3d_##name(IDirectSound3DBuffer *This, D3DVALUE v, DWORD a) \
{ \
    char hb[16]; \
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DBuffer *, D3DVALUE, DWORD), slot)(This, v, a); \
    L("3DB %p " #name "(%.3f, %s) -> %s", (void *)This, v, ap(a), HRS(hr)); \
    return hr; \
}
B3D_SET_F(SetMaxDistance, 16)
B3D_SET_F(SetMinDistance, 17)

static HRESULT WINAPI b3d_SetMode(IDirectSound3DBuffer *This, DWORD m, DWORD a)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DBuffer *, DWORD, DWORD), 18)(This, m, a);
    L("3DB %p SetMode(%lu, %s) -> %s", (void *)This, (unsigned long)m, ap(a), HRS(hr));
    return hr;
}

/* ======================================================================== IDirectSound3DListener */

static HRESULT WINAPI lst_SetAllParameters(IDirectSound3DListener *This, LPCDS3DLISTENER p, DWORD a)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DListener *, LPCDS3DLISTENER, DWORD), 10)(This, p, a);
    if (p)
        L("LST %p SetAllParameters(pos %.3f,%.3f,%.3f front %.3f,%.3f,%.3f top %.3f,%.3f,%.3f dist %.3f roll %.3f dop %.3f %s) -> %s",
          (void *)This, p->vPosition.x, p->vPosition.y, p->vPosition.z, p->vOrientFront.x, p->vOrientFront.y,
          p->vOrientFront.z, p->vOrientTop.x, p->vOrientTop.y, p->vOrientTop.z, p->flDistanceFactor,
          p->flRolloffFactor, p->flDopplerFactor, ap(a), HRS(hr));
    return hr;
}

#define LST_SET_F(name, slot) \
static HRESULT WINAPI lst_##name(IDirectSound3DListener *This, D3DVALUE v, DWORD a) \
{ \
    char hb[16]; \
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DListener *, D3DVALUE, DWORD), slot)(This, v, a); \
    L("LST %p " #name "(%.3f, %s) -> %s", (void *)This, v, ap(a), HRS(hr)); \
    return hr; \
}
LST_SET_F(SetDistanceFactor, 11)
LST_SET_F(SetDopplerFactor, 12)
LST_SET_F(SetRolloffFactor, 15)

static HRESULT WINAPI lst_SetOrientation(IDirectSound3DListener *This, D3DVALUE fx, D3DVALUE fy, D3DVALUE fz,
                                         D3DVALUE tx, D3DVALUE ty, D3DVALUE tz, DWORD a)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DListener *, D3DVALUE, D3DVALUE, D3DVALUE, D3DVALUE, D3DVALUE, D3DVALUE, DWORD), 13)
        (This, fx, fy, fz, tx, ty, tz, a);
    L("LST %p SetOrientation(%.3f,%.3f,%.3f / %.3f,%.3f,%.3f, %s) -> %s", (void *)This, fx, fy, fz, tx, ty, tz, ap(a), HRS(hr));
    return hr;
}

#define LST_SET_V(name, slot) \
static HRESULT WINAPI lst_##name(IDirectSound3DListener *This, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD a) \
{ \
    char hb[16]; \
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DListener *, D3DVALUE, D3DVALUE, D3DVALUE, DWORD), slot)(This, x, y, z, a); \
    L("LST %p " #name "(%.3f, %.3f, %.3f, %s) -> %s", (void *)This, x, y, z, ap(a), HRS(hr)); \
    return hr; \
}
LST_SET_V(SetPosition, 14)
LST_SET_V(SetVelocity, 16)

static HRESULT WINAPI lst_CommitDeferredSettings(IDirectSound3DListener *This)
{
    char hb[16];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSound3DListener *), 17)(This);
    L("LST %p CommitDeferredSettings -> %s", (void *)This, HRS(hr));
    return hr;
}

/* ======================================================================== IDirectSoundNotify */

static HRESULT WINAPI ntf_SetNotificationPositions(IDirectSoundNotify *This, DWORD n, LPCDSBPOSITIONNOTIFY p)
{
    char hb[16], s[512] = "";
    HRESULT hr = CALL(HRESULT (WINAPI *)(IDirectSoundNotify *, DWORD, LPCDSBPOSITIONNOTIFY), 3)(This, n, p);
    size_t o = 0;
    for (DWORD i = 0; p && i < n && o < sizeof(s) - 16; i++)
        o += sprintf(s + o, " %ld", (long)p[i].dwOffset);
    L("NTF %p SetNotificationPositions(%lu:%s) -> %s", (void *)This, (unsigned long)n, s, HRS(hr));
    return hr;
}

/* ======================================================================== IKsPropertySet (EAX) */

static HRESULT WINAPI ks_Get(IKsPropertySet *This, REFGUID set, ULONG id, LPVOID inst, ULONG ilen, LPVOID data, ULONG len, PULONG ret)
{
    char hb[16], gb[96], d[600];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IKsPropertySet *, REFGUID, ULONG, LPVOID, ULONG, LPVOID, ULONG, PULONG), 3)
        (This, set, id, inst, ilen, data, len, ret);
    dump(SUCCEEDED(hr) ? data : NULL, SUCCEEDED(hr) && ret ? *ret : 0, d, sizeof(d));
    L("KS %p Get(%s, id %lu, len %lu) -> %s ret %lu:%s", (void *)This, GS(set), (unsigned long)id, (unsigned long)len,
      HRS(hr), ret ? (unsigned long)*ret : 0ul, d);
    return hr;
}

static HRESULT WINAPI ks_Set(IKsPropertySet *This, REFGUID set, ULONG id, LPVOID inst, ULONG ilen, LPVOID data, ULONG len)
{
    char hb[16], gb[96], d[600];
    dump(data, len, d, sizeof(d));
    HRESULT hr = CALL(HRESULT (WINAPI *)(IKsPropertySet *, REFGUID, ULONG, LPVOID, ULONG, LPVOID, ULONG), 4)
        (This, set, id, inst, ilen, data, len);
    L("KS %p Set(%s, id %lu, len %lu:%s) -> %s", (void *)This, GS(set), (unsigned long)id, (unsigned long)len, d, HRS(hr));
    return hr;
}

static HRESULT WINAPI ks_QuerySupport(IKsPropertySet *This, REFGUID set, ULONG id, PULONG sup)
{
    char hb[16], gb[96];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IKsPropertySet *, REFGUID, ULONG, PULONG), 5)(This, set, id, sup);
    L("KS %p QuerySupport(%s, id %lu) -> %s support %lx", (void *)This, GS(set), (unsigned long)id, HRS(hr),
      SUCCEEDED(hr) && sup ? (unsigned long)*sup : 0ul);
    return hr;
}

/* ======================================================================== IClassFactory */

static HRESULT WINAPI cf_CreateInstance(IClassFactory *This, IUnknown *outer, REFIID riid, void **out)
{
    char hb[16], gb[96];
    HRESULT hr = CALL(HRESULT (WINAPI *)(IClassFactory *, IUnknown *, REFIID, void **), 3)(This, outer, riid, out);
    L("CF %p CreateInstance(%s) -> %s %p", (void *)This, GS(riid), HRS(hr), out ? *out : NULL);
    if (SUCCEEDED(hr) && out && *out) {
        enum kind k = kind_for_iid(riid);
        if (k != K_COUNT) hook(*out, k);
    }
    return hr;
}

static void build_tables(void)
{
    for (int k = 0; k < K_COUNT; k++) {
        hook_tables[k][0] = (void *)h_QueryInterface;
        hook_tables[k][1] = (void *)h_AddRef;
        hook_tables[k][2] = (void *)h_Release;
    }
    void **t = hook_tables[K_DS];
    t[3] = ds_CreateSoundBuffer; t[4] = ds_GetCaps; t[5] = ds_DuplicateSoundBuffer; t[6] = ds_SetCooperativeLevel;
    t[8] = ds_GetSpeakerConfig; t[9] = ds_SetSpeakerConfig; t[10] = ds_Initialize; t[11] = ds_VerifyCertification;

    t = hook_tables[K_DSB];
    t[3] = dsb_GetCaps; t[4] = dsb_GetCurrentPosition; t[5] = dsb_GetFormat; t[6] = dsb_GetVolume; t[7] = dsb_GetPan;
    t[8] = dsb_GetFrequency; t[9] = dsb_GetStatus; t[11] = dsb_Lock; t[12] = dsb_Play; t[13] = dsb_SetCurrentPosition;
    t[14] = dsb_SetFormat; t[15] = dsb_SetVolume; t[16] = dsb_SetPan; t[17] = dsb_SetFrequency; t[18] = dsb_Stop;
    t[19] = dsb_Unlock; t[20] = dsb_Restore; t[21] = dsb_SetFX; t[22] = dsb_AcquireResources;
    /* slots 21-23 only exist on IDirectSoundBuffer8 objects; hook() copies them from the target
     * table anyway and only DS8-aware callers will ever call them */

    t = hook_tables[K_3DB];
    t[3] = b3d_GetAllParameters; t[12] = b3d_SetAllParameters; t[13] = b3d_SetConeAngles; t[14] = b3d_SetConeOrientation;
    t[15] = b3d_SetConeOutsideVolume; t[16] = b3d_SetMaxDistance; t[17] = b3d_SetMinDistance; t[18] = b3d_SetMode;
    t[19] = b3d_SetPosition; t[20] = b3d_SetVelocity;

    t = hook_tables[K_3DL];
    t[10] = lst_SetAllParameters; t[11] = lst_SetDistanceFactor; t[12] = lst_SetDopplerFactor; t[13] = lst_SetOrientation;
    t[14] = lst_SetPosition; t[15] = lst_SetRolloffFactor; t[16] = lst_SetVelocity; t[17] = lst_CommitDeferredSettings;

    hook_tables[K_NOTIFY][3] = ntf_SetNotificationPositions;
    t = hook_tables[K_KS];
    t[3] = ks_Get; t[4] = ks_Set; t[5] = ks_QuerySupport;
    hook_tables[K_CF][3] = cf_CreateInstance;
}

/* ======================================================================== setup */

static void dir_of(char *p) { char *s = strrchr(p, '\\'); if (s) s[1] = 0; else p[0] = 0; }

static void load_config(void)
{
    char dir[MAX_PATH], ini[MAX_PATH + 16], exe[MAX_PATH], logf[MAX_PATH] = "", tgt[MAX_PATH] = "", v[MAX_PATH];
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    dir_of(dir);
    lstrcpyA(ini, dir); lstrcatA(ini, "dstrace.ini");

    FILE *f = fopen(ini, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            char *s = line; while (*s == ' ') s++;
            char *eq = strchr(s, '=');
            if (!eq || *s == ';' || *s == '#') continue;
            *eq = 0;
            char *k = s, *val = eq + 1;
            for (char *q = eq - 1; q >= k && *q == ' '; q--) *q = 0;
            while (*val == ' ') val++;
            if (!_stricmp(k, "target")) lstrcpynA(tgt, val, MAX_PATH);
            else if (!_stricmp(k, "level")) g_level = atoi(val);
            else if (!_stricmp(k, "logfile")) lstrcpynA(logf, val, MAX_PATH);
        }
        fclose(f);
    }
    if (GetEnvironmentVariableA("DSTRACE_TARGET", v, MAX_PATH)) lstrcpynA(tgt, v, MAX_PATH);
    if (GetEnvironmentVariableA("DSTRACE_LEVEL", v, MAX_PATH)) g_level = atoi(v);

    if (!logf[0]) {
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        lstrcpyA(logf, exe); dir_of(logf); lstrcatA(logf, "dstrace.log");
    }
    g_log = fopen(logf, "w");

    if (tgt[0]) {
        if (!strchr(tgt, '\\') && !strchr(tgt, ':')) { lstrcpyA(g_target_path, dir); lstrcatA(g_target_path, tgt); }
        else lstrcpynA(g_target_path, tgt, MAX_PATH);
    } else {
        lstrcpyA(g_target_path, dir); lstrcatA(g_target_path, "alchemy_dsound.dll");
        if (GetFileAttributesA(g_target_path) == INVALID_FILE_ATTRIBUTES) {
            GetSystemDirectoryA(g_target_path, MAX_PATH);
            lstrcatA(g_target_path, "\\dsound.dll");
        }
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    L("dstrace: process %s", exe);
    L("dstrace: target %s, level %d", g_target_path, g_level);
}

static HMODULE target(void)
{
    static LONG once;
    if (InterlockedCompareExchange(&once, 1, 0) == 0) {
        g_target = LoadLibraryA(g_target_path);
        if (g_target == g_self) { FreeLibrary(g_target); g_target = NULL; }
        L("dstrace: LoadLibrary(%s) -> %p (err %lu)", g_target_path, (void *)g_target,
          g_target ? 0ul : (unsigned long)GetLastError());
        InterlockedExchange(&once, 2);
    }
    while (once != 2) Sleep(0);
    return g_target;
}

static FARPROC tp(const char *name) { return target() ? GetProcAddress(target(), name) : NULL; }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_cs);
        build_tables();
        load_config();
    }
    return TRUE;
}

/* ======================================================================== exports */

HRESULT WINAPI DirectSoundCreate(LPCGUID g, LPDIRECTSOUND *out, LPUNKNOWN o)
{
    char hb[16], gb[96];
    HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN) = (void *)tp("DirectSoundCreate");
    HRESULT hr = fn ? fn(g, out, o) : DSERR_NODRIVER;
    L("DirectSoundCreate(%s) -> %s %p", GS(g), HRS(hr), out ? (void *)*out : NULL);
    if (SUCCEEDED(hr) && out) hook(*out, K_DS);
    return hr;
}

HRESULT WINAPI DirectSoundCreate8(LPCGUID g, LPDIRECTSOUND8 *out, LPUNKNOWN o)
{
    char hb[16], gb[96];
    HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN) = (void *)tp("DirectSoundCreate8");
    HRESULT hr = fn ? fn(g, out, o) : DSERR_NODRIVER;
    L("DirectSoundCreate8(%s) -> %s %p", GS(g), HRS(hr), out ? (void *)*out : NULL);
    if (SUCCEEDED(hr) && out) hook(*out, K_DS);
    return hr;
}

typedef struct { LPDSENUMCALLBACKA a; LPDSENUMCALLBACKW w; LPVOID ctx; } enum_ctx;

static BOOL CALLBACK enum_a(LPGUID g, LPCSTR desc, LPCSTR mod, LPVOID c)
{
    char gb[96];
    enum_ctx *e = c;
    L("  enum: %s \"%s\" \"%s\"", GS(g), desc, mod);
    return e->a(g, desc, mod, e->ctx);
}

static BOOL CALLBACK enum_w(LPGUID g, LPCWSTR desc, LPCWSTR mod, LPVOID c)
{
    char gb[96];
    enum_ctx *e = c;
    L("  enum: %s \"%ls\" \"%ls\"", GS(g), desc, mod);
    return e->w(g, desc, mod, e->ctx);
}

HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx)
{
    char hb[16];
    HRESULT (WINAPI *fn)(LPDSENUMCALLBACKA, LPVOID) = (void *)tp("DirectSoundEnumerateA");
    L("DirectSoundEnumerateA");
    enum_ctx e = { cb, NULL, ctx };
    HRESULT hr = fn && cb ? fn(enum_a, &e) : fn ? fn(cb, ctx) : DSERR_NODRIVER;
    L("DirectSoundEnumerateA -> %s", HRS(hr));
    return hr;
}

HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx)
{
    char hb[16];
    HRESULT (WINAPI *fn)(LPDSENUMCALLBACKW, LPVOID) = (void *)tp("DirectSoundEnumerateW");
    L("DirectSoundEnumerateW");
    enum_ctx e = { NULL, cb, ctx };
    HRESULT hr = fn && cb ? fn(enum_w, &e) : fn ? fn(cb, ctx) : DSERR_NODRIVER;
    L("DirectSoundEnumerateW -> %s", HRS(hr));
    return hr;
}

HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID g, LPDIRECTSOUNDCAPTURE *out, LPUNKNOWN o)
{
    HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUNDCAPTURE *, LPUNKNOWN) = (void *)tp("DirectSoundCaptureCreate");
    L("DirectSoundCaptureCreate");
    return fn ? fn(g, out, o) : DSERR_NODRIVER;
}

HRESULT WINAPI DirectSoundCaptureCreate8(LPCGUID g, LPDIRECTSOUNDCAPTURE8 *out, LPUNKNOWN o)
{
    HRESULT (WINAPI *fn)(LPCGUID, LPDIRECTSOUNDCAPTURE8 *, LPUNKNOWN) = (void *)tp("DirectSoundCaptureCreate8");
    L("DirectSoundCaptureCreate8");
    return fn ? fn(g, out, o) : DSERR_NODRIVER;
}

HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx)
{
    HRESULT (WINAPI *fn)(LPDSENUMCALLBACKA, LPVOID) = (void *)tp("DirectSoundCaptureEnumerateA");
    return fn ? fn(cb, ctx) : DS_OK;
}

HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx)
{
    HRESULT (WINAPI *fn)(LPDSENUMCALLBACKW, LPVOID) = (void *)tp("DirectSoundCaptureEnumerateW");
    return fn ? fn(cb, ctx) : DS_OK;
}

HRESULT WINAPI DirectSoundFullDuplexCreate(LPCGUID a, LPCGUID b, LPCDSCBUFFERDESC c, LPCDSBUFFERDESC d, HWND h, DWORD l,
    LPDIRECTSOUNDFULLDUPLEX *e, LPDIRECTSOUNDCAPTUREBUFFER8 *f, LPDIRECTSOUNDBUFFER8 *g, LPUNKNOWN o)
{
    HRESULT (WINAPI *fn)(LPCGUID, LPCGUID, LPCDSCBUFFERDESC, LPCDSBUFFERDESC, HWND, DWORD, LPDIRECTSOUNDFULLDUPLEX *,
        LPDIRECTSOUNDCAPTUREBUFFER8 *, LPDIRECTSOUNDBUFFER8 *, LPUNKNOWN) = (void *)tp("DirectSoundFullDuplexCreate");
    L("DirectSoundFullDuplexCreate (not traced further)");
    return fn ? fn(a, b, c, d, h, l, e, f, g, o) : DSERR_NODRIVER;
}

HRESULT WINAPI GetDeviceID(LPCGUID src, LPGUID dst)
{
    char hb[16], gb[96], gb2[96];
    HRESULT (WINAPI *fn)(LPCGUID, LPGUID) = (void *)tp("GetDeviceID");
    HRESULT hr = fn ? fn(src, dst) : DSERR_NODRIVER;
    L("GetDeviceID(%s) -> %s %s", GS(src), HRS(hr), SUCCEEDED(hr) && dst ? guids(dst, gb2) : "-");
    return hr;
}

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out)
{
    char hb[16], gb[96], gb2[96];
    HRESULT (WINAPI *fn)(REFCLSID, REFIID, LPVOID *) = (void *)tp("DllGetClassObject");
    HRESULT hr = fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
    L("DllGetClassObject(%s, %s) -> %s", GS(clsid), guids(riid, gb2), HRS(hr));
    if (SUCCEEDED(hr) && out && *out && IsEqualIID(riid, &IID_IClassFactory)) hook(*out, K_CF);
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_FALSE; }
