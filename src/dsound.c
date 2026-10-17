/* reAL - IDirectSound / IDirectSound8 */
#include "real.h"

static inline rl_dsound *impl(IDirectSound8 *iface)
{
    return impl_from(iface, rl_dsound, IDirectSound8_iface);
}

static HRESULT WINAPI ds_QueryInterface(IDirectSound8 *iface, REFIID riid, void **out)
{
    rl_dsound *This = impl(iface);
    char gb[64];
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSound) ||
        (This->is_ds8 && IsEqualIID(riid, &IID_IDirectSound8))) {
        *out = iface;
        IDirectSound8_AddRef(iface);
        return S_OK;
    }
    /* DirectMusic's synth sink asks for two private interfaces only Microsoft's DirectSound has.
     * Like ALchemy (0x1001b4ed), hand it a real system DirectSound for those, so DirectMusic plays
     * through it; disabledirectmusic=true refuses them as ALchemy does. */
    static const GUID dmusic_priv1 = { 0x2a8af120, 0xe9de, 0x4132, { 0xaa, 0xa5, 0x4b, 0xdd, 0xa5, 0xf3, 0x25, 0xb8 } };
    static const GUID dmusic_priv2 = { 0xfa82931b, 0xde98, 0x4a68, { 0xa7, 0xfe, 0xd6, 0x83, 0xe3, 0x8a, 0x65, 0x02 } };
    if ((IsEqualIID(riid, &dmusic_priv1) || IsEqualIID(riid, &dmusic_priv2)) && !g_cfg.disable_dmusic) {
        HRESULT hr = E_NOINTERFACE;
        if (!This->real_ds && rl_real_dsound()) {
            HRESULT (WINAPI *create8)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN) =
                (void *)GetProcAddress(rl_real_dsound(), "DirectSoundCreate8");
            if (create8 && SUCCEEDED(create8(NULL, &This->real_ds, NULL)))
                IDirectSound8_SetCooperativeLevel(This->real_ds, GetForegroundWindow(),
                                                  This->coop_level ? This->coop_level : DSSCL_PRIORITY);
        }
        if (This->real_ds) hr = IDirectSound8_QueryInterface(This->real_ds, riid, out);
        else *out = NULL;
        LOG_INFO("DirectSound::QueryInterface %s (DirectMusic) -> system DirectSound %08lx",
                 rl_guid_str(riid, gb), (unsigned long)hr);
        return hr;
    }
    LOG_CALL("DirectSound::QueryInterface unsupported %s", rl_guid_str(riid, gb));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ds_AddRef(IDirectSound8 *iface)
{
    return InterlockedIncrement(&impl(iface)->ref);
}

static ULONG WINAPI ds_Release(IDirectSound8 *iface)
{
    rl_dsound *This = impl(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    if (!ref) {
        LOG_CALL("DirectSound %p destroyed", (void *)This);
        /* real DirectSound takes all buffers created through this object down with it */
        if (This->dev) rl_buffers_orphan(This->dev, This);
        if (This->primary) IDirectSoundBuffer8_Release(&This->primary->IDirectSoundBuffer8_iface);
        if (This->dev) rl_device_release(This->dev);
        if (This->real_ds) IDirectSound8_Release(This->real_ds);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return ref;
}

static HRESULT WINAPI ds_CreateSoundBuffer(IDirectSound8 *iface, LPCDSBUFFERDESC desc,
                                           LPDIRECTSOUNDBUFFER *out, LPUNKNOWN outer)
{
    rl_dsound *This = impl(iface);
    if (!This->dev) return DSERR_UNINITIALIZED;
    if (!desc || !out) return DSERR_INVALIDPARAM;
    *out = NULL;
    if (outer) return DSERR_NOAGGREGATION;
    if (desc->dwSize != sizeof(DSBUFFERDESC) && desc->dwSize != sizeof(DSBUFFERDESC1))
        return DSERR_INVALIDPARAM;

    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
        if (desc->lpwfxFormat || desc->dwBufferBytes || (desc->dwFlags & DSBCAPS_CTRLFX)) return DSERR_INVALIDPARAM;
        /* one primary object per device object; hand out new references */
        if (!This->primary) {
            HRESULT hr = rl_buffer_create(This->dev, This, desc, &This->primary);
            if (FAILED(hr)) return hr;
        }
        IDirectSoundBuffer8_AddRef(&This->primary->IDirectSoundBuffer8_iface);
        *out = (LPDIRECTSOUNDBUFFER)&This->primary->IDirectSoundBuffer8_iface;
        return DS_OK;
    }

    rl_buffer *b;
    HRESULT hr = rl_buffer_create(This->dev, This, desc, &b);
    if (FAILED(hr)) return hr;
    *out = (LPDIRECTSOUNDBUFFER)&b->IDirectSoundBuffer8_iface;
    return DS_OK;
}

static HRESULT WINAPI ds_GetCaps(IDirectSound8 *iface, LPDSCAPS caps)
{
    rl_dsound *This = impl(iface);
    if (!This->dev) return DSERR_UNINITIALIZED;
    if (!caps || caps->dwSize < sizeof(*caps)) return DSERR_INVALIDPARAM;
    /* mirrors ALchemy: maxvoicecount + 1 voices, only non-LOCSOFTWARE secondaries use one up */
    LONG used = This->dev->n_hw_buffers;
    DWORD maxv = (DWORD)g_cfg.max_voices + 1;
    DWORD freev = used >= (LONG)maxv ? 0 : maxv - (DWORD)used;

    ZeroMemory(caps, sizeof(*caps));
    caps->dwSize = sizeof(*caps);
    /* Advertise a hardware-accelerated (certified) device like ALchemy does, so games
     * enable their DS3D hardware / EAX code paths. */
    caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                    DSCAPS_CONTINUOUSRATE | DSCAPS_CERTIFIED | DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO |
                    DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT;
    caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    caps->dwMaxSecondarySampleRate = 192000;
    caps->dwPrimaryBuffers = 1;
    caps->dwMaxHwMixingAllBuffers = caps->dwMaxHwMixingStaticBuffers = caps->dwMaxHwMixingStreamingBuffers = maxv;
    caps->dwFreeHwMixingAllBuffers = caps->dwFreeHwMixingStaticBuffers = caps->dwFreeHwMixingStreamingBuffers = freev;
    caps->dwMaxHw3DAllBuffers = caps->dwMaxHw3DStaticBuffers = caps->dwMaxHw3DStreamingBuffers = maxv;
    caps->dwFreeHw3DAllBuffers = caps->dwFreeHw3DStaticBuffers = caps->dwFreeHw3DStreamingBuffers = freev;
    caps->dwTotalHwMemBytes = caps->dwFreeHwMemBytes = 0;
    caps->dwMaxContigFreeHwMemBytes = 0;
    caps->dwUnlockTransferRateHwBuffers = 4096;
    caps->dwPlayCpuOverheadSwBuffers = 0;
    return DS_OK;
}

static HRESULT WINAPI ds_DuplicateSoundBuffer(IDirectSound8 *iface, LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER *out)
{
    rl_dsound *This = impl(iface);
    if (!This->dev) return DSERR_UNINITIALIZED;
    if (!src || !out) return DSERR_INVALIDPARAM;
    *out = NULL;
    rl_buffer *s = impl_from(src, rl_buffer, IDirectSoundBuffer8_iface);
    if (s->is_primary) return DSERR_INVALIDCALL;
    rl_buffer *b;
    HRESULT hr = rl_buffer_duplicate(This->dev, This, s, &b);
    if (FAILED(hr)) return hr;
    *out = (LPDIRECTSOUNDBUFFER)&b->IDirectSoundBuffer8_iface;
    return DS_OK;
}

static HRESULT WINAPI ds_SetCooperativeLevel(IDirectSound8 *iface, HWND hwnd, DWORD level)
{
    rl_dsound *This = impl(iface);
    (void)hwnd;
    LOG_CALL("DirectSound::SetCooperativeLevel(%lu)", (unsigned long)level);
    if (!This->dev) return DSERR_UNINITIALIZED;
    if (level < DSSCL_NORMAL || level > DSSCL_WRITEPRIMARY) return DSERR_INVALIDPARAM;
    This->coop_level = level;
    return DS_OK;
}

static HRESULT WINAPI ds_Compact(IDirectSound8 *iface)
{
    return impl(iface)->dev ? DS_OK : DSERR_UNINITIALIZED;
}

static HRESULT WINAPI ds_GetSpeakerConfig(IDirectSound8 *iface, LPDWORD cfg)
{
    rl_dsound *This = impl(iface);
    if (!This->dev) return DSERR_UNINITIALIZED;
    if (!cfg) return DSERR_INVALIDPARAM;
    *cfg = This->dev->speaker_config;
    return DS_OK;
}

static HRESULT WINAPI ds_SetSpeakerConfig(IDirectSound8 *iface, DWORD cfg)
{
    rl_dsound *This = impl(iface);
    if (!This->dev) return DSERR_UNINITIALIZED;
    This->dev->speaker_config = cfg;
    This->dev->speaker_config_set = TRUE;
    return DS_OK;
}

static HRESULT WINAPI ds_Initialize(IDirectSound8 *iface, LPCGUID guid)
{
    rl_dsound *This = impl(iface);
    char gb[64];
    LOG_CALL("DirectSound::Initialize(%s)", rl_guid_str(guid, gb));
    if (This->dev) return DSERR_ALREADYINITIALIZED;
    /* any playback device GUID is reAL's device (see is_playback_guid in dllmain.c) */
    if (guid && (IsEqualGUID(guid, &DSDEVID_DefaultCapture) || IsEqualGUID(guid, &DSDEVID_DefaultVoiceCapture)))
        return DSERR_NODRIVER;
    return rl_device_acquire(&This->dev);
}

static HRESULT WINAPI ds_VerifyCertification(IDirectSound8 *iface, LPDWORD cert)
{
    if (!impl(iface)->dev) return DSERR_UNINITIALIZED;
    if (!cert) return DSERR_INVALIDPARAM;
    *cert = DS_CERTIFIED;
    return DS_OK;
}

static IDirectSound8Vtbl ds_vtbl = {
    ds_QueryInterface, ds_AddRef, ds_Release,
    ds_CreateSoundBuffer, ds_GetCaps, ds_DuplicateSoundBuffer, ds_SetCooperativeLevel,
    ds_Compact, ds_GetSpeakerConfig, ds_SetSpeakerConfig, ds_Initialize,
    ds_VerifyCertification,
};

HRESULT rl_dsound_create(REFIID riid, void **out, BOOL ds8)
{
    rl_dsound *This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This) return DSERR_OUTOFMEMORY;
    This->IDirectSound8_iface.lpVtbl = &ds_vtbl;
    This->ref = 1;
    This->is_ds8 = ds8;
    HRESULT hr = ds_QueryInterface(&This->IDirectSound8_iface, riid, out);
    ds_Release(&This->IDirectSound8_iface);
    return hr;
}
