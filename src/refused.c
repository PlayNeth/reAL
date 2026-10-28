/* reAL - every DirectSound call reAL refuses, in reAL.log
 *
 * A game rarely expects a call that works on Windows to fail, and when it does fail the game often goes quietly down
 * another path (Fallout 3's Lock of an odd buffer size: the sound taken as not loaded, a heap crash seconds later).
 * So with logging on (log=1 or more), each method of IDirectSound8, IDirectSoundBuffer8, IDirectSoundNotify,
 * IDirectSound3DBuffer, IDirectSound3DListener and IKsPropertySet goes through a wrapper that logs a failed HRESULT
 * with the arguments and the caller (module+offset): "refused: ...".  The first few refusals of each method are
 * logged, then every power of two; a count of each comes at the end of the log.
 *
 * The wrappers sit in the vtables themselves, filled in once at load, so objects need nothing; with log=0 nothing is
 * installed.  The arguments are taken as DWORDs: on x86 every argument of these methods is 4 bytes (floats too), so
 * the original is called with the same stack. */
#include "real.h"
#include <stdio.h>
#include <string.h>

/* (interface, vtable slot, method, argument types after `this`)
 *   p pointer, d unsigned, i signed, x hex flags, f float, g REFIID / GUID pointer, B DSBUFFERDESC pointer,
 *   W WAVEFORMATEX pointer */
#define RL_REFUSED_METHODS(X) \
    X(DS,  0, ds_QueryInterface, "IDirectSound", "QueryInterface", 2, "gp") \
    X(DS,  3, ds_CreateSoundBuffer, "IDirectSound", "CreateSoundBuffer", 3, "Bpp") \
    X(DS,  4, ds_GetCaps, "IDirectSound", "GetCaps", 1, "p") \
    X(DS,  5, ds_DuplicateSoundBuffer, "IDirectSound", "DuplicateSoundBuffer", 2, "pp") \
    X(DS,  6, ds_SetCooperativeLevel, "IDirectSound", "SetCooperativeLevel", 2, "px") \
    X(DS,  7, ds_Compact, "IDirectSound", "Compact", 0, "") \
    X(DS,  8, ds_GetSpeakerConfig, "IDirectSound", "GetSpeakerConfig", 1, "p") \
    X(DS,  9, ds_SetSpeakerConfig, "IDirectSound", "SetSpeakerConfig", 1, "x") \
    X(DS, 10, ds_Initialize, "IDirectSound", "Initialize", 1, "g") \
    X(DS, 11, ds_VerifyCertification, "IDirectSound", "VerifyCertification", 1, "p") \
    X(DSB,  0, dsb_QueryInterface, "IDirectSoundBuffer", "QueryInterface", 2, "gp") \
    X(DSB,  3, dsb_GetCaps, "IDirectSoundBuffer", "GetCaps", 1, "p") \
    X(DSB,  4, dsb_GetCurrentPosition, "IDirectSoundBuffer", "GetCurrentPosition", 2, "pp") \
    X(DSB,  5, dsb_GetFormat, "IDirectSoundBuffer", "GetFormat", 3, "pdp") \
    X(DSB,  6, dsb_GetVolume, "IDirectSoundBuffer", "GetVolume", 1, "p") \
    X(DSB,  7, dsb_GetPan, "IDirectSoundBuffer", "GetPan", 1, "p") \
    X(DSB,  8, dsb_GetFrequency, "IDirectSoundBuffer", "GetFrequency", 1, "p") \
    X(DSB,  9, dsb_GetStatus, "IDirectSoundBuffer", "GetStatus", 1, "p") \
    X(DSB, 10, dsb_Initialize, "IDirectSoundBuffer", "Initialize", 2, "pB") \
    X(DSB, 11, dsb_Lock, "IDirectSoundBuffer", "Lock", 7, "ddppppx") \
    X(DSB, 12, dsb_Play, "IDirectSoundBuffer", "Play", 3, "ddx") \
    X(DSB, 13, dsb_SetCurrentPosition, "IDirectSoundBuffer", "SetCurrentPosition", 1, "d") \
    X(DSB, 14, dsb_SetFormat, "IDirectSoundBuffer", "SetFormat", 1, "W") \
    X(DSB, 15, dsb_SetVolume, "IDirectSoundBuffer", "SetVolume", 1, "i") \
    X(DSB, 16, dsb_SetPan, "IDirectSoundBuffer", "SetPan", 1, "i") \
    X(DSB, 17, dsb_SetFrequency, "IDirectSoundBuffer", "SetFrequency", 1, "d") \
    X(DSB, 18, dsb_Stop, "IDirectSoundBuffer", "Stop", 0, "") \
    X(DSB, 19, dsb_Unlock, "IDirectSoundBuffer", "Unlock", 4, "pdpd") \
    X(DSB, 20, dsb_Restore, "IDirectSoundBuffer", "Restore", 0, "") \
    X(DSB, 21, dsb_SetFX, "IDirectSoundBuffer", "SetFX", 3, "dpp") \
    X(DSB, 22, dsb_AcquireResources, "IDirectSoundBuffer", "AcquireResources", 3, "xdp") \
    X(DSB, 23, dsb_GetObjectInPath, "IDirectSoundBuffer", "GetObjectInPath", 4, "gdgp") \
    X(NTF, 0, ntf_QueryInterface, "IDirectSoundNotify", "QueryInterface", 2, "gp") \
    X(NTF, 3, ntf_SetNotificationPositions, "IDirectSoundNotify", "SetNotificationPositions", 2, "dp") \
    X(B3D,  0, b3d_QueryInterface, "IDirectSound3DBuffer", "QueryInterface", 2, "gp") \
    X(B3D,  3, b3d_GetAllParameters, "IDirectSound3DBuffer", "GetAllParameters", 1, "p") \
    X(B3D,  4, b3d_GetConeAngles, "IDirectSound3DBuffer", "GetConeAngles", 2, "pp") \
    X(B3D,  5, b3d_GetConeOrientation, "IDirectSound3DBuffer", "GetConeOrientation", 1, "p") \
    X(B3D,  6, b3d_GetConeOutsideVolume, "IDirectSound3DBuffer", "GetConeOutsideVolume", 1, "p") \
    X(B3D,  7, b3d_GetMaxDistance, "IDirectSound3DBuffer", "GetMaxDistance", 1, "p") \
    X(B3D,  8, b3d_GetMinDistance, "IDirectSound3DBuffer", "GetMinDistance", 1, "p") \
    X(B3D,  9, b3d_GetMode, "IDirectSound3DBuffer", "GetMode", 1, "p") \
    X(B3D, 10, b3d_GetPosition, "IDirectSound3DBuffer", "GetPosition", 1, "p") \
    X(B3D, 11, b3d_GetVelocity, "IDirectSound3DBuffer", "GetVelocity", 1, "p") \
    X(B3D, 12, b3d_SetAllParameters, "IDirectSound3DBuffer", "SetAllParameters", 2, "px") \
    X(B3D, 13, b3d_SetConeAngles, "IDirectSound3DBuffer", "SetConeAngles", 3, "ddx") \
    X(B3D, 14, b3d_SetConeOrientation, "IDirectSound3DBuffer", "SetConeOrientation", 4, "fffx") \
    X(B3D, 15, b3d_SetConeOutsideVolume, "IDirectSound3DBuffer", "SetConeOutsideVolume", 2, "ix") \
    X(B3D, 16, b3d_SetMaxDistance, "IDirectSound3DBuffer", "SetMaxDistance", 2, "fx") \
    X(B3D, 17, b3d_SetMinDistance, "IDirectSound3DBuffer", "SetMinDistance", 2, "fx") \
    X(B3D, 18, b3d_SetMode, "IDirectSound3DBuffer", "SetMode", 2, "dx") \
    X(B3D, 19, b3d_SetPosition, "IDirectSound3DBuffer", "SetPosition", 4, "fffx") \
    X(B3D, 20, b3d_SetVelocity, "IDirectSound3DBuffer", "SetVelocity", 4, "fffx") \
    X(LST,  0, lst_QueryInterface, "IDirectSound3DListener", "QueryInterface", 2, "gp") \
    X(LST,  3, lst_GetAllParameters, "IDirectSound3DListener", "GetAllParameters", 1, "p") \
    X(LST,  4, lst_GetDistanceFactor, "IDirectSound3DListener", "GetDistanceFactor", 1, "p") \
    X(LST,  5, lst_GetDopplerFactor, "IDirectSound3DListener", "GetDopplerFactor", 1, "p") \
    X(LST,  6, lst_GetOrientation, "IDirectSound3DListener", "GetOrientation", 2, "pp") \
    X(LST,  7, lst_GetPosition, "IDirectSound3DListener", "GetPosition", 1, "p") \
    X(LST,  8, lst_GetRolloffFactor, "IDirectSound3DListener", "GetRolloffFactor", 1, "p") \
    X(LST,  9, lst_GetVelocity, "IDirectSound3DListener", "GetVelocity", 1, "p") \
    X(LST, 10, lst_SetAllParameters, "IDirectSound3DListener", "SetAllParameters", 2, "px") \
    X(LST, 11, lst_SetDistanceFactor, "IDirectSound3DListener", "SetDistanceFactor", 2, "fx") \
    X(LST, 12, lst_SetDopplerFactor, "IDirectSound3DListener", "SetDopplerFactor", 2, "fx") \
    X(LST, 13, lst_SetOrientation, "IDirectSound3DListener", "SetOrientation", 7, "ffffffx") \
    X(LST, 14, lst_SetPosition, "IDirectSound3DListener", "SetPosition", 4, "fffx") \
    X(LST, 15, lst_SetRolloffFactor, "IDirectSound3DListener", "SetRolloffFactor", 2, "fx") \
    X(LST, 16, lst_SetVelocity, "IDirectSound3DListener", "SetVelocity", 4, "fffx") \
    X(LST, 17, lst_CommitDeferredSettings, "IDirectSound3DListener", "CommitDeferredSettings", 0, "") \
    X(KS, 0, ks_QueryInterface, "IKsPropertySet", "QueryInterface", 2, "gp") \
    X(KS, 3, ks_Get, "IKsPropertySet", "Get", 7, "gdpdpdp") \
    X(KS, 4, ks_Set, "IKsPropertySet", "Set", 6, "gdpdpd") \
    X(KS, 5, ks_QuerySupport, "IKsPropertySet", "QuerySupport", 3, "gdp")

enum {
#define X(tab, slot, id, iface, name, n, types) W_##id,
    RL_REFUSED_METHODS(X)
#undef X
    W_COUNT
};

typedef struct {
    void *real;
    const char *iface, *name, *types;
    int tab, slot;
    LONG count;
} rl_wrapped;

static rl_wrapped s_w[W_COUNT] = {
#define X(tab, slot, id, iface, name, n, types) { NULL, iface, name, types, RL_VT_##tab, slot, 0 },
    RL_REFUSED_METHODS(X)
#undef X
};

static void fmt_arg(char t, DWORD v, char *out, size_t n)
{
    char g[64];
    switch (t) {
    case 'd': snprintf(out, n, "%lu", (unsigned long)v); break;
    case 'i': snprintf(out, n, "%ld", (long)v); break;
    case 'x': snprintf(out, n, "0x%lx", (unsigned long)v); break;
    case 'f': { float f; memcpy(&f, &v, 4); snprintf(out, n, "%.3f", (double)f); break; }
    case 'g': snprintf(out, n, "%s", v && !IsBadReadPtr((void *)v, sizeof(GUID)) ? rl_guid_str((const GUID *)v, g) : (v ? "(bad)" : "NULL")); break;
    case 'B': {
        const DSBUFFERDESC *d = (const DSBUFFERDESC *)v;
        if (!d) { snprintf(out, n, "NULL"); break; }
        if (IsBadReadPtr(d, sizeof(DWORD) * 4)) { snprintf(out, n, "(bad)"); break; }
        const WAVEFORMATEX *w = d->dwSize >= sizeof(DWORD) * 5 && !IsBadReadPtr(d, sizeof(DWORD) * 5) ? d->lpwfxFormat : NULL;
        if (w && !IsBadReadPtr(w, 16))
            snprintf(out, n, "{size %lu flags 0x%lx bytes %lu tag %u %lu Hz %u ch %u bit align %u}", (unsigned long)d->dwSize,
                     (unsigned long)d->dwFlags, (unsigned long)d->dwBufferBytes, w->wFormatTag, (unsigned long)w->nSamplesPerSec,
                     w->nChannels, w->wBitsPerSample, w->nBlockAlign);
        else
            snprintf(out, n, "{size %lu flags 0x%lx bytes %lu}", (unsigned long)d->dwSize, (unsigned long)d->dwFlags,
                     (unsigned long)d->dwBufferBytes);
        break;
    }
    case 'W': {
        const WAVEFORMATEX *w = (const WAVEFORMATEX *)v;
        if (!w) snprintf(out, n, "NULL");
        else if (IsBadReadPtr(w, 16)) snprintf(out, n, "(bad)");
        else snprintf(out, n, "{tag %u %lu Hz %u ch %u bit align %u}", w->wFormatTag, (unsigned long)w->nSamplesPerSec,
                      w->nChannels, w->wBitsPerSample, w->nBlockAlign);
        break;
    }
    default: snprintf(out, n, v ? "%08lx" : "NULL", (unsigned long)v); break;
    }
}

static int worth_logging(LONG k)       /* the first 8, then every power of two */
{
    return k <= 8 || !(k & (k - 1));
}

static void refused(int id, const DWORD *a, HRESULT hr, void *caller)
{
    rl_wrapped *w = &s_w[id];
    LONG k = InterlockedIncrement(&w->count);
    if (!worth_logging(k)) return;
    char line[700], arg[160], from[MAX_PATH + 32];
    int len = snprintf(line, sizeof(line), "%s::%s(", w->iface, w->name);
    for (int i = 0; w->types[i] && len < (int)sizeof(line) - 1; i++) {
        fmt_arg(w->types[i], a[i + 1], arg, sizeof(arg));
        len += snprintf(line + len, sizeof(line) - len, "%s%s", i ? ", " : "", arg);
    }
    rl_addr_str((DWORD_PTR)caller, from, sizeof(from));
    rl_log(1, "refused: %s) on %08lx -> %08lx, from %s%s", line, (unsigned long)a[0], (unsigned long)hr, from,
           k > 8 ? " (and more; counted at the end)" : "");
}

/* the wrappers: one per method, with its own count of DWORD arguments */
#define AL0
#define AL1 , DWORD a1
#define AL2 AL1, DWORD a2
#define AL3 AL2, DWORD a3
#define AL4 AL3, DWORD a4
#define AL5 AL4, DWORD a5
#define AL6 AL5, DWORD a6
#define AL7 AL6, DWORD a7
#define AV0
#define AV1 , a1
#define AV2 AV1, a2
#define AV3 AV2, a3
#define AV4 AV3, a4
#define AV5 AV4, a5
#define AV6 AV5, a6
#define AV7 AV6, a7
#define X(tab, slot, id, iface, name, n, types) \
    static HRESULT WINAPI w_##id(void *t AL##n) \
    { \
        HRESULT hr = ((HRESULT (WINAPI *)(void * AL##n))s_w[W_##id].real)(t AV##n); \
        if (FAILED(hr)) { \
            const DWORD a[] = { (DWORD)(DWORD_PTR)t AV##n }; \
            refused(W_##id, a, hr, __builtin_return_address(0)); \
        } \
        return hr; \
    }
RL_REFUSED_METHODS(X)
#undef X

static void *const s_wrapper[W_COUNT] = {
#define X(tab, slot, id, iface, name, n, types) (void *)w_##id,
    RL_REFUSED_METHODS(X)
#undef X
};

void rl_refused_install(void)
{
    if (g_cfg.log_level < 1) return;
    for (int i = 0; i < W_COUNT; i++) {
        void **vt = rl_vtable(s_w[i].tab);
        if (!vt || s_w[i].real) continue;
        s_w[i].real = vt[s_w[i].slot];
        vt[s_w[i].slot] = s_wrapper[i];
    }
}

/* at the end of the log: how often each method was refused */
void rl_refused_summary(int process_exit)
{
    int any = 0;
    for (int i = 0; i < W_COUNT; i++) {
        LONG k = s_w[i].count;
        if (!k) continue;
        char line[160];
        snprintf(line, sizeof(line), "%s%s::%s refused %ld time%s", any ? "  " : "refused calls: ", s_w[i].iface,
                 s_w[i].name, (long)k, k == 1 ? "" : "s");
        if (!rl_log_try(line, process_exit)) return;
        any = 1;
    }
}

/* a call on an interface of a buffer already released (Windows answers E_INVALIDARG, as reAL's zombie does) */
void rl_refused_zombie(void *iface, void *caller)
{
    static LONG count;
    LONG k = InterlockedIncrement(&count);
    if (!worth_logging(k)) return;
    char from[MAX_PATH + 32];
    rl_addr_str((DWORD_PTR)caller, from, sizeof(from));
    rl_log(1, "refused: a call on %08lx, an interface of a released buffer -> 80070057, from %s%s", (unsigned long)(DWORD_PTR)iface,
           from, k > 8 ? " (and more)" : "");
}
