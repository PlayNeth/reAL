/* reAL - IDirectSoundBuffer8 + IDirectSound3DBuffer + IDirectSoundNotify + IKsPropertySet
 *        + IDirectSound3DListener (primary buffer) */
#include "real.h"
#include <stdio.h>
#include <float.h>
#include <string.h>

#ifndef E_PROP_ID_UNSUPPORTED
#define E_PROP_ID_UNSUPPORTED ((HRESULT)0x80070490L)
#endif

static const GUID RL_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID RL_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static LONG s_next_id;

#define BUF(iface)   impl_from(iface, rl_buffer, IDirectSoundBuffer8_iface)
#define BUF3D(iface) impl_from(iface, rl_buffer, IDirectSound3DBuffer_iface)
#define NOTIFY(iface) impl_from(iface, rl_buffer, IDirectSoundNotify_iface)
#define PROPS(iface) impl_from(iface, rl_buffer, IKsPropertySet_iface)
#define LISTENER(iface) impl_from(iface, rl_buffer, IDirectSound3DListener_iface)

void rl_signal_event(void *ev) { if (ev) SetEvent((HANDLE)ev); }

/* ======================================================================= helpers */

/* Runs on the game's threads: some games unmask FPU exceptions or drop the x87 to 24-bit precision
 * (Direct3D does). Compute with a known FPU state and give the game its own back. */
static void update_gains(rl_buffer *b);

unsigned int rl_fpu_enter(void)
{
    unsigned int cw = _controlfp(0, 0);
    _controlfp(_MCW_EM | _PC_53 | _RC_NEAR, _MCW_EM | _MCW_PC | _MCW_RC);
    return cw;
}

void rl_fpu_leave(unsigned int cw)
{
    _clearfp();
    _controlfp(cw, _MCW_EM | _MCW_PC | _MCW_RC);
}

void rl_buffer_update_gains(rl_buffer *b)
{
    unsigned int cw = rl_fpu_enter();
    update_gains(b);
    rl_fpu_leave(cw);
}

static void update_gains(rl_buffer *b)
{
    rl_device *dev = b->dev;
    float base = rl_db100_to_gain(b->volume);
    BOOL is3d = (b->desc.dwFlags & DSBCAPS_CTRL3D) != 0;
    BOOL spatial = is3d && b->src3d.mode != RL_DS3DMODE_DISABLE;
    BOOL use_hrtf = spatial && b->hrtf && dev->hrtf.dirs;
    rl_geom geo;
    float pan[2];
    rl_spatial_geometry(&dev->listener, &b->src3d, &geo);
    if (spatial) { pan[0] = geo.pan[0]; pan[1] = geo.pan[1]; }
    else rl_pan_gains(b->pan, pan);

    /* direct path level, EAX high-frequency damping and reverb send */
    float direct = geo.dist_gain * geo.cone_gain, send = 0.0f;
    float lp_a = 1.0f, send_lp_a = 1.0f;
    if (is3d && dev->eax_used && dev->eax_version >= 2) {
        /* ALchemy's EAX 2 / 3 source model (fns 0x1004a8a0 / 0x1004c890 and the voice update 0x10040ee0) */
        eax_geom eg = { geo.x, geo.cone_factor, b->src3d.cone_inside, b->src3d.cone_outside,
                        b->src3d.cone_outside_volume, dev->listener.distance_factor, dev->listener.rolloff_factor };
        eax_gains o;
        float ref;
        if (dev->eax_version == 2) {
            eax2_source_gains(&b->eax2, &dev->eax2l, &eg, &o);
            direct = geo.dist_gain * geo.cone_gain * o.direct;       /* EAX 2 keeps DS3D's distance gain */
            ref = 5000.0f;
        } else {
            eax3_source_gains(&b->eax3, &dev->eax3l, &eg, &o);
            direct = o.direct * geo.cone_gain;                       /* EAX 3 computes the roll-off itself */
            ref = dev->eax3l.p.hf_reference;
        }
        if (direct > 1.0f) direct = 1.0f;
        if (geo.muted) direct = 0.0f;
        float rate = dev->out_rate ? (float)dev->out_rate : 48000.0f;
        float f = ref / rate;
        if (f > 0.5f) f = 0.5f;
        lp_a = eax_lp_coef(o.direct_hf, f);
        send_lp_a = eax_lp_coef(o.room_hf, f);
        send = geo.muted ? 0.0f : base * o.room;
    } else if (is3d && dev->eax_used) {
        /* EAX 1.0: EAX_REVERBMIX_USEDISTANCE (-1) currently means full send */
        float mix = b->eax1_mix < 0.0f ? 1.0f : b->eax1_mix;
        send = base * direct * mix;
    }
    /* Doppler: EAX 3 adds the buffer's own factor to the listener's (ALchemy caps the sum at 10) */
    float dop = dev->listener.doppler_factor;
    if (is3d && dev->eax_used && dev->eax_version == 3) {
        dop += b->eax3.doppler;
        if (dop > 10.0f) dop = 10.0f;
    }
    b->voice.pitch = spatial ? rl_spatial_doppler(&dev->listener, &b->src3d, dop) : 1.0f;

    b->voice.lp_a = lp_a;
    b->voice.send_lp_a = send_lp_a;
    b->voice.send_gain = send;

    if (use_hrtf) {
        /* HRTF carries the direction; only distance/cone attenuation and volume are gains */
        float az, el;
        rl_hrtf_filter f;
        rl_spatial_direction(&dev->listener, &b->src3d, &az, &el);
        rl_hrtf_lookup(&dev->hrtf, az, el, &f);
        rl_hrtf_voice_set(b->hrtf, &f);
        pan[0] = pan[1] = 1.0f;
        b->voice.hrtf = b->hrtf;
        b->voice.hrtf_set = &dev->hrtf;
    } else {
        b->voice.hrtf = NULL;
    }
    b->voice.gain[0] = pan[0] * direct * base;
    b->voice.gain[1] = pan[1] * direct * base;
}

static DWORD orig_rate(rl_buffer *b) { return b->fmt.Format.nSamplesPerSec; }

static HRESULT parse_format(const WAVEFORMATEX *wfx, WAVEFORMATEXTENSIBLE *out, rl_sfmt *sfmt)
{
    if (!wfx) return DSERR_INVALIDPARAM;
    ZeroMemory(out, sizeof(*out));
    WORD tag = wfx->wFormatTag;
    WORD bits = wfx->wBitsPerSample;
    int is_float = 0;

    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (wfx->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return DSERR_INVALIDPARAM;
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        if (IsEqualGUID(&ext->SubFormat, &RL_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) is_float = 1;
        else if (!IsEqualGUID(&ext->SubFormat, &RL_KSDATAFORMAT_SUBTYPE_PCM)) return DSERR_BADFORMAT;
        *out = *ext;
    } else if (tag == WAVE_FORMAT_PCM || tag == 3 /* WAVE_FORMAT_IEEE_FLOAT */) {
        is_float = tag == 3;
        out->Format = *wfx;
        out->Format.cbSize = 0;
    } else {
        return DSERR_BADFORMAT;
    }

    if (!wfx->nChannels || wfx->nChannels > 8) return DSERR_BADFORMAT;
    if (wfx->nSamplesPerSec < DSBFREQUENCY_MIN || wfx->nSamplesPerSec > DSBFREQUENCY_MAX) return DSERR_BADFORMAT;
    if (is_float) {
        if (bits != 32) return DSERR_BADFORMAT;
        *sfmt = RL_F32;
    } else switch (bits) {
        case 8:  *sfmt = RL_U8; break;
        case 16: *sfmt = RL_S16; break;
        case 24: *sfmt = RL_S24; break;
        case 32: *sfmt = RL_S32; break;
        default: return DSERR_BADFORMAT;
    }
    /* recompute derived fields; some games pass garbage */
    out->Format.nBlockAlign = (WORD)(wfx->nChannels * bits / 8);
    out->Format.nAvgBytesPerSec = out->Format.nBlockAlign * wfx->nSamplesPerSec;
    return DS_OK;
}

static void list_add_all(rl_device *dev, rl_buffer *b)
{
    b->prev_all = NULL;
    b->next_all = dev->all;
    if (dev->all) dev->all->prev_all = b;
    dev->all = b;
}

static void list_remove_all(rl_device *dev, rl_buffer *b)
{
    if (b->prev_all) b->prev_all->next_all = b->next_all; else if (dev->all == b) dev->all = b->next_all;
    if (b->next_all) b->next_all->prev_all = b->prev_all;
    b->next_all = b->prev_all = NULL;
}

/* =========================================================== IDirectSoundBuffer8 */

static HRESULT WINAPI dsb_QueryInterface(IDirectSoundBuffer8 *iface, REFIID riid, void **out)
{
    rl_buffer *This = BUF(iface);
    char gb[64];
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundBuffer) ||
        (!This->is_primary && IsEqualIID(riid, &IID_IDirectSoundBuffer8)))
        *out = &This->IDirectSoundBuffer8_iface;
    else if (!This->is_primary && IsEqualIID(riid, &IID_IDirectSound3DBuffer) && (This->desc.dwFlags & DSBCAPS_CTRL3D))
        *out = &This->IDirectSound3DBuffer_iface;
    else if (!This->is_primary && IsEqualIID(riid, &IID_IDirectSoundNotify) && (This->desc.dwFlags & DSBCAPS_CTRLPOSITIONNOTIFY))
        *out = &This->IDirectSoundNotify_iface;
    else if (IsEqualIID(riid, &IID_IKsPropertySet))
        *out = &This->IKsPropertySet_iface;
    else if (This->is_primary && IsEqualIID(riid, &IID_IDirectSound3DListener))
        *out = &This->IDirectSound3DListener_iface;

    if (*out) {
        InterlockedIncrement(&This->ref);
        return S_OK;
    }
    LOG_CALL("Buffer[%d]::QueryInterface unsupported %s", This->id, rl_guid_str(riid, gb));
    return E_NOINTERFACE;
}

static ULONG WINAPI dsb_AddRef(IDirectSoundBuffer8 *iface)
{
    return InterlockedIncrement(&BUF(iface)->ref);
}

static ULONG WINAPI dsb_Release(IDirectSoundBuffer8 *iface)
{
    rl_buffer *This = BUF(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    if (ref) return ref;

    rl_device *dev = This->dev;
    LOG_CALL("Buffer[%d] destroyed", This->id);
    EnterCriticalSection(&dev->lock);
    if (This->in_play_list) rl_device_stop_voice(dev, This);
    list_remove_all(dev, This);
    if (!This->is_primary) {
        dev->n_buffers--;
        if (!(This->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) dev->n_hw_buffers--;
    }
    LeaveCriticalSection(&dev->lock);

    if (This->shared && !InterlockedDecrement(&This->shared->ref)) {
        HeapFree(GetProcessHeap(), 0, This->shared->data);
        HeapFree(GetProcessHeap(), 0, This->shared);
    }
    HeapFree(GetProcessHeap(), 0, This->notifies);
    HeapFree(GetProcessHeap(), 0, This->hrtf);
    HeapFree(GetProcessHeap(), 0, This);
    rl_device_release(dev);
    return 0;
}

static HRESULT WINAPI dsb_GetCaps(IDirectSoundBuffer8 *iface, LPDSBCAPS caps)
{
    rl_buffer *This = BUF(iface);
    if (!caps || caps->dwSize < sizeof(*caps)) return DSERR_INVALIDPARAM;
    caps->dwFlags = This->desc.dwFlags;
    if (!(caps->dwFlags & DSBCAPS_LOCSOFTWARE)) {
        caps->dwFlags &= ~DSBCAPS_LOCDEFER;
        caps->dwFlags |= DSBCAPS_LOCHARDWARE;
    }
    caps->dwBufferBytes = This->is_primary ? 4096 : This->desc.dwBufferBytes;
    caps->dwUnlockTransferRate = 4096;
    caps->dwPlayCpuOverhead = 0;
    return DS_OK;
}

static DWORD write_lead_bytes(rl_buffer *This)
{
    /* how far ahead of the play cursor the "write" cursor sits: ~15 ms + one mix period */
    DWORD rate = This->voice.rate ? This->voice.rate : orig_rate(This);
    DWORD frames = rate * 15 / 1000;
    if (This->dev->out_rate)
        frames += (DWORD)((unsigned long long)This->dev->period_frames * rate / This->dev->out_rate);
    return frames * This->voice.block;
}

static HRESULT WINAPI dsb_GetCurrentPosition(IDirectSoundBuffer8 *iface, LPDWORD play, LPDWORD write)
{
    rl_buffer *This = BUF(iface);
    if (This->is_primary) {
        if (play) *play = 0;
        if (write) *write = 0;
        return DS_OK;
    }
    EnterCriticalSection(&This->dev->lock);
    DWORD p = rl_voice_play_bytes(&This->voice);
    DWORD w = p;
    if (This->voice.playing) {
        w = p + write_lead_bytes(This);
        if (This->voice.size) w %= This->voice.size;
    }
    LeaveCriticalSection(&This->dev->lock);
    if (play) *play = p;
    if (write) *write = w;
    LOG_TRACE("Buffer[%d]::GetCurrentPosition play %lu write %lu", This->id, (unsigned long)p, (unsigned long)w);
    return DS_OK;
}

static HRESULT WINAPI dsb_GetFormat(IDirectSoundBuffer8 *iface, LPWAVEFORMATEX wfx, DWORD size, LPDWORD written)
{
    rl_buffer *This = BUF(iface);
    const WAVEFORMATEX *src = This->is_primary ? &This->dev->primary_fmt : &This->fmt.Format;
    DWORD need = sizeof(WAVEFORMATEX) + src->cbSize;
    if (!wfx && !written) return DSERR_INVALIDPARAM;
    if (wfx) {
        if (size < need) return DSERR_INVALIDPARAM;
        memcpy(wfx, src, need);
    }
    if (written) *written = need;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetVolume(IDirectSoundBuffer8 *iface, LPLONG vol)
{
    rl_buffer *This = BUF(iface);
    if (!vol) return DSERR_INVALIDPARAM;
    if (!(This->desc.dwFlags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    *vol = This->is_primary ? This->dev->primary_volume : This->volume;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetPan(IDirectSoundBuffer8 *iface, LPLONG pan)
{
    rl_buffer *This = BUF(iface);
    if (!pan) return DSERR_INVALIDPARAM;
    if (!(This->desc.dwFlags & DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
    *pan = This->pan;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetFrequency(IDirectSoundBuffer8 *iface, LPDWORD freq)
{
    rl_buffer *This = BUF(iface);
    if (!freq) return DSERR_INVALIDPARAM;
    if (This->is_primary || !(This->desc.dwFlags & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    *freq = This->freq ? This->freq : orig_rate(This);
    return DS_OK;
}

static HRESULT WINAPI dsb_GetStatus(IDirectSoundBuffer8 *iface, LPDWORD status)
{
    rl_buffer *This = BUF(iface);
    if (!status) return DSERR_INVALIDPARAM;
    DWORD s = 0;
    EnterCriticalSection(&This->dev->lock);
    if (This->is_primary) {
        if (This->dev->primary_playing || This->dev->playing) s = DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;
    } else if (This->voice.playing) {
        s = DSBSTATUS_PLAYING;
        if (This->voice.looping) s |= DSBSTATUS_LOOPING;
    }
    LeaveCriticalSection(&This->dev->lock);
    if (!(This->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) s |= DSBSTATUS_LOCHARDWARE;
    else s |= DSBSTATUS_LOCSOFTWARE;
    *status = s;
    return DS_OK;
}

static HRESULT WINAPI dsb_Initialize(IDirectSoundBuffer8 *iface, LPDIRECTSOUND ds, LPCDSBUFFERDESC desc)
{
    (void)iface; (void)ds; (void)desc;
    return DSERR_ALREADYINITIALIZED;
}

static HRESULT WINAPI dsb_Lock(IDirectSoundBuffer8 *iface, DWORD offset, DWORD bytes,
                               LPVOID *p1, LPDWORD b1, LPVOID *p2, LPDWORD b2, DWORD flags)
{
    rl_buffer *This = BUF(iface);
    if (p1) *p1 = NULL;
    if (b1) *b1 = 0;
    if (p2) *p2 = NULL;
    if (b2) *b2 = 0;
    if (This->is_primary) return DSERR_PRIOLEVELNEEDED; /* DSSCL_WRITEPRIMARY not supported */
    if (!p1 || !b1) return DSERR_INVALIDPARAM;

    DWORD size = This->voice.size;
    if (flags & DSBLOCK_FROMWRITECURSOR) dsb_GetCurrentPosition(iface, NULL, &offset);
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
    if (offset >= size || bytes > size || !bytes) return DSERR_INVALIDPARAM;

    BYTE *data = This->shared->data;
    if (offset + bytes <= size) {
        *p1 = data + offset;
        *b1 = bytes;
    } else {
        *p1 = data + offset;
        *b1 = size - offset;
        if (p2 && b2) {
            *p2 = data;
            *b2 = bytes - (size - offset);
        }
    }
    LOG_TRACE("Buffer[%d]::Lock off %lu bytes %lu flags %lx -> %lu + %lu", This->id, (unsigned long)offset,
              (unsigned long)bytes, (unsigned long)flags, (unsigned long)*b1, (unsigned long)(b2 ? *b2 : 0));
    return DS_OK;
}

static HRESULT WINAPI dsb_Play(IDirectSoundBuffer8 *iface, DWORD res1, DWORD prio, DWORD flags)
{
    rl_buffer *This = BUF(iface);
    (void)prio;
    if (res1) return DSERR_INVALIDPARAM;
    LOG_TRACE("Buffer[%d]::Play flags %lx", This->id, (unsigned long)flags);
    EnterCriticalSection(&This->dev->lock);
    if (This->dead) {
        LeaveCriticalSection(&This->dev->lock);
        LOG_CALL("Buffer[%d]::Play on a buffer whose DirectSound object was released", This->id);
        return DSERR_BUFFERLOST;
    }
    if (This->is_primary) {
        This->dev->primary_playing = TRUE;
    } else {
        This->voice.looping = (flags & DSBPLAY_LOOPING) != 0;
        if (This->desc.dwFlags & DSBCAPS_CTRL3D) {
            /* make sure a deferred position set before Play is not lost */
            rl_buffer_update_gains(This);
        }
        rl_voice_start(&This->voice);
        if (!This->in_play_list) rl_device_start_voice(This->dev, This);
    }
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetCurrentPosition(IDirectSoundBuffer8 *iface, DWORD pos)
{
    rl_buffer *This = BUF(iface);
    if (This->is_primary) return DSERR_INVALIDCALL;
    if (pos >= This->voice.size) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&This->dev->lock);
    rl_voice_jump(&This->voice, (uint64_t)(pos / This->voice.block) << 32);
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetFormat(IDirectSoundBuffer8 *iface, LPCWAVEFORMATEX wfx)
{
    rl_buffer *This = BUF(iface);
    if (!This->is_primary) return DSERR_INVALIDCALL;
    if (!wfx) return DSERR_INVALIDPARAM;
    /* we always mix in float at the device rate; just remember what the game asked for */
    This->dev->primary_fmt = *wfx;
    This->dev->primary_fmt.cbSize = 0;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) This->dev->primary_fmt.wFormatTag = WAVE_FORMAT_PCM;
    LOG_CALL("Primary::SetFormat %lu Hz %u ch %u bits", (unsigned long)wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetVolume(IDirectSoundBuffer8 *iface, LONG vol)
{
    rl_buffer *This = BUF(iface);
    if (!(This->desc.dwFlags & DSBCAPS_CTRLVOLUME)) return DSERR_CONTROLUNAVAIL;
    if (vol < DSBVOLUME_MIN || vol > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&This->dev->lock);
    if (This->is_primary) This->dev->primary_volume = vol;
    else { This->volume = vol; rl_buffer_update_gains(This); }
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetPan(IDirectSoundBuffer8 *iface, LONG pan)
{
    rl_buffer *This = BUF(iface);
    if (!(This->desc.dwFlags & DSBCAPS_CTRLPAN)) return DSERR_CONTROLUNAVAIL;
    if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&This->dev->lock);
    This->pan = pan;
    if (!This->is_primary) rl_buffer_update_gains(This);
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetFrequency(IDirectSoundBuffer8 *iface, DWORD freq)
{
    rl_buffer *This = BUF(iface);
    if (This->is_primary || !(This->desc.dwFlags & DSBCAPS_CTRLFREQUENCY)) return DSERR_CONTROLUNAVAIL;
    if (freq != DSBFREQUENCY_ORIGINAL && (freq < DSBFREQUENCY_MIN || freq > DSBFREQUENCY_MAX)) return DSERR_INVALIDPARAM;
    EnterCriticalSection(&This->dev->lock);
    This->freq = freq;
    This->voice.rate = freq ? freq : orig_rate(This);
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_Stop(IDirectSoundBuffer8 *iface)
{
    rl_buffer *This = BUF(iface);
    LOG_TRACE("Buffer[%d]::Stop", This->id);
    EnterCriticalSection(&This->dev->lock);
    if (This->is_primary) {
        This->dev->primary_playing = FALSE;
    } else {
        BOOL was = This->voice.playing;
        This->voice.playing = 0;
        if (This->in_play_list) rl_device_stop_voice(This->dev, This);
        if (was) rl_fire_stop(&This->voice, rl_signal_event);
    }
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_Unlock(IDirectSoundBuffer8 *iface, LPVOID p1, DWORD b1, LPVOID p2, DWORD b2)
{
    (void)iface; (void)p1; (void)b1; (void)p2; (void)b2;
    /* data is written in place; nothing to do */
    return DS_OK;
}

static HRESULT WINAPI dsb_Restore(IDirectSoundBuffer8 *iface) { (void)iface; return DS_OK; }

static HRESULT WINAPI dsb_SetFX(IDirectSoundBuffer8 *iface, DWORD count, LPDSEFFECTDESC fx, LPDWORD results)
{
    rl_buffer *This = BUF(iface);
    if (!(This->desc.dwFlags & DSBCAPS_CTRLFX)) return DSERR_CONTROLUNAVAIL;
    if (This->voice.playing) return DSERR_INVALIDCALL;
    if (!count) return DS_OK;
    if (!fx) return DSERR_INVALIDPARAM;
    LOG_CALL("Buffer[%d]::SetFX(%lu) - DMO effects not supported", This->id, (unsigned long)count);
    if (results) for (DWORD i = 0; i < count; i++) results[i] = DSFXR_UNKNOWN;
    return DSERR_FXUNAVAILABLE;
}

static HRESULT WINAPI dsb_AcquireResources(IDirectSoundBuffer8 *iface, DWORD flags, DWORD count, LPDWORD results)
{
    (void)iface; (void)flags;
    if (results) for (DWORD i = 0; i < count; i++) results[i] = DSFXR_LOCHARDWARE;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetObjectInPath(IDirectSoundBuffer8 *iface, REFGUID obj, DWORD idx, REFGUID iid, LPVOID *out)
{
    (void)iface; (void)obj; (void)idx; (void)iid;
    if (out) *out = NULL;
    return DSERR_OBJECTNOTFOUND;
}

static IDirectSoundBuffer8Vtbl dsb_vtbl = {
    dsb_QueryInterface, dsb_AddRef, dsb_Release,
    dsb_GetCaps, dsb_GetCurrentPosition, dsb_GetFormat, dsb_GetVolume, dsb_GetPan, dsb_GetFrequency,
    dsb_GetStatus, dsb_Initialize, dsb_Lock, dsb_Play, dsb_SetCurrentPosition, dsb_SetFormat,
    dsb_SetVolume, dsb_SetPan, dsb_SetFrequency, dsb_Stop, dsb_Unlock, dsb_Restore,
    dsb_SetFX, dsb_AcquireResources, dsb_GetObjectInPath,
};

/* =========================================================== IDirectSoundNotify */

static HRESULT WINAPI ntf_QueryInterface(IDirectSoundNotify *iface, REFIID riid, void **out)
{ return dsb_QueryInterface(&NOTIFY(iface)->IDirectSoundBuffer8_iface, riid, out); }
static ULONG WINAPI ntf_AddRef(IDirectSoundNotify *iface)
{ return dsb_AddRef(&NOTIFY(iface)->IDirectSoundBuffer8_iface); }
static ULONG WINAPI ntf_Release(IDirectSoundNotify *iface)
{ return dsb_Release(&NOTIFY(iface)->IDirectSoundBuffer8_iface); }

static HRESULT WINAPI ntf_SetNotificationPositions(IDirectSoundNotify *iface, DWORD count, LPCDSBPOSITIONNOTIFY pos)
{
    rl_buffer *This = NOTIFY(iface);
    if (count && !pos) return DSERR_INVALIDPARAM;
    for (DWORD i = 0; i < count; i++)
        if (pos[i].dwOffset != (DWORD)DSBPN_OFFSETSTOP && pos[i].dwOffset >= This->voice.size)
            return DSERR_INVALIDPARAM;

    rl_notify *n = NULL;
    if (count) {
        n = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*n));
        if (!n) return DSERR_OUTOFMEMORY;
        for (DWORD i = 0; i < count; i++) {
            n[i].offset = pos[i].dwOffset == (DWORD)DSBPN_OFFSETSTOP ? RL_NOTIFY_STOP : pos[i].dwOffset;
            n[i].event = pos[i].hEventNotify;
        }
    }
    EnterCriticalSection(&This->dev->lock);
    if (This->voice.playing) {
        LeaveCriticalSection(&This->dev->lock);
        HeapFree(GetProcessHeap(), 0, n);
        return DSERR_INVALIDCALL;
    }
    rl_notify *old = This->notifies;
    This->notifies = n;
    This->n_notifies = count;
    This->voice.notifies = n;
    This->voice.n_notifies = count;
    LeaveCriticalSection(&This->dev->lock);
    HeapFree(GetProcessHeap(), 0, old);
    LOG_CALL("Buffer[%d]::SetNotificationPositions(%lu)", This->id, (unsigned long)count);
    return DS_OK;
}

static IDirectSoundNotifyVtbl ntf_vtbl = {
    ntf_QueryInterface, ntf_AddRef, ntf_Release, ntf_SetNotificationPositions,
};

/* =========================================================== IDirectSound3DBuffer */

static HRESULT WINAPI b3d_QueryInterface(IDirectSound3DBuffer *iface, REFIID riid, void **out)
{ return dsb_QueryInterface(&BUF3D(iface)->IDirectSoundBuffer8_iface, riid, out); }
static ULONG WINAPI b3d_AddRef(IDirectSound3DBuffer *iface)
{ return dsb_AddRef(&BUF3D(iface)->IDirectSoundBuffer8_iface); }
static ULONG WINAPI b3d_Release(IDirectSound3DBuffer *iface)
{ return dsb_Release(&BUF3D(iface)->IDirectSoundBuffer8_iface); }

static rl_vec3 to_v(D3DVALUE x, D3DVALUE y, D3DVALUE z) { rl_vec3 v = { x, y, z }; return v; }

/* apply = DS3D_IMMEDIATE (0) or DS3D_DEFERRED (1) */
static void b3d_changed(rl_buffer *This, DWORD apply)
{
    if (apply == DS3D_DEFERRED) {
        This->src3d_dirty = TRUE;
    } else {
        This->src3d = This->src3d_pending;
        This->src3d_dirty = FALSE;
        rl_buffer_update_gains(This);
    }
}

#define B3D_SET_BEGIN(iface) rl_buffer *This = BUF3D(iface); EnterCriticalSection(&This->dev->lock)
#define B3D_SET_END(apply)   b3d_changed(This, apply); LeaveCriticalSection(&This->dev->lock); return DS_OK

static HRESULT WINAPI b3d_GetAllParameters(IDirectSound3DBuffer *iface, LPDS3DBUFFER p)
{
    rl_buffer *This = BUF3D(iface);
    if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM;
    const rl_source3d *s = &This->src3d_pending;
    p->vPosition.x = s->pos.x; p->vPosition.y = s->pos.y; p->vPosition.z = s->pos.z;
    p->vVelocity.x = s->vel.x; p->vVelocity.y = s->vel.y; p->vVelocity.z = s->vel.z;
    p->dwInsideConeAngle = s->cone_inside;
    p->dwOutsideConeAngle = s->cone_outside;
    p->vConeOrientation.x = s->cone_orient.x; p->vConeOrientation.y = s->cone_orient.y; p->vConeOrientation.z = s->cone_orient.z;
    p->lConeOutsideVolume = s->cone_outside_volume;
    p->flMinDistance = s->min_dist;
    p->flMaxDistance = s->max_dist;
    p->dwMode = s->mode;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetConeAngles(IDirectSound3DBuffer *iface, LPDWORD in, LPDWORD out)
{
    rl_buffer *This = BUF3D(iface);
    if (!in || !out) return DSERR_INVALIDPARAM;
    *in = This->src3d_pending.cone_inside;
    *out = This->src3d_pending.cone_outside;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetConeOrientation(IDirectSound3DBuffer *iface, D3DVECTOR *v)
{
    rl_buffer *This = BUF3D(iface);
    if (!v) return DSERR_INVALIDPARAM;
    v->x = This->src3d_pending.cone_orient.x; v->y = This->src3d_pending.cone_orient.y; v->z = This->src3d_pending.cone_orient.z;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetConeOutsideVolume(IDirectSound3DBuffer *iface, LPLONG vol)
{
    if (!vol) return DSERR_INVALIDPARAM;
    *vol = BUF3D(iface)->src3d_pending.cone_outside_volume;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetMaxDistance(IDirectSound3DBuffer *iface, D3DVALUE *d)
{
    if (!d) return DSERR_INVALIDPARAM;
    *d = BUF3D(iface)->src3d_pending.max_dist;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetMinDistance(IDirectSound3DBuffer *iface, D3DVALUE *d)
{
    if (!d) return DSERR_INVALIDPARAM;
    *d = BUF3D(iface)->src3d_pending.min_dist;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetMode(IDirectSound3DBuffer *iface, LPDWORD mode)
{
    if (!mode) return DSERR_INVALIDPARAM;
    *mode = BUF3D(iface)->src3d_pending.mode;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetPosition(IDirectSound3DBuffer *iface, D3DVECTOR *v)
{
    rl_buffer *This = BUF3D(iface);
    if (!v) return DSERR_INVALIDPARAM;
    v->x = This->src3d_pending.pos.x; v->y = This->src3d_pending.pos.y; v->z = This->src3d_pending.pos.z;
    return DS_OK;
}

static HRESULT WINAPI b3d_GetVelocity(IDirectSound3DBuffer *iface, D3DVECTOR *v)
{
    rl_buffer *This = BUF3D(iface);
    if (!v) return DSERR_INVALIDPARAM;
    v->x = This->src3d_pending.vel.x; v->y = This->src3d_pending.vel.y; v->z = This->src3d_pending.vel.z;
    return DS_OK;
}

static HRESULT WINAPI b3d_SetAllParameters(IDirectSound3DBuffer *iface, LPCDS3DBUFFER p, DWORD apply)
{
    if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM;
    B3D_SET_BEGIN(iface);
    rl_source3d *s = &This->src3d_pending;
    s->pos = to_v(p->vPosition.x, p->vPosition.y, p->vPosition.z);
    s->vel = to_v(p->vVelocity.x, p->vVelocity.y, p->vVelocity.z);
    s->cone_inside = p->dwInsideConeAngle;
    s->cone_outside = p->dwOutsideConeAngle;
    s->cone_orient = to_v(p->vConeOrientation.x, p->vConeOrientation.y, p->vConeOrientation.z);
    s->cone_outside_volume = p->lConeOutsideVolume;
    s->min_dist = p->flMinDistance;
    s->max_dist = p->flMaxDistance;
    s->mode = p->dwMode;
    LOG_3D("Buffer[%d]::SetAllParameters pos %.3f,%.3f,%.3f min %.3f max %.3f mode %lu (%s)", This->id,
           s->pos.x, s->pos.y, s->pos.z, s->min_dist, s->max_dist, (unsigned long)s->mode, apply ? "deferred" : "immediate");
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetConeAngles(IDirectSound3DBuffer *iface, DWORD in, DWORD out, DWORD apply)
{
    if (in > 360 || out > 360) return DSERR_INVALIDPARAM;
    B3D_SET_BEGIN(iface);
    This->src3d_pending.cone_inside = in;
    This->src3d_pending.cone_outside = out;
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetConeOrientation(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    B3D_SET_BEGIN(iface);
    This->src3d_pending.cone_orient = to_v(x, y, z);
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetConeOutsideVolume(IDirectSound3DBuffer *iface, LONG vol, DWORD apply)
{
    if (vol < DSBVOLUME_MIN || vol > DSBVOLUME_MAX) return DSERR_INVALIDPARAM;
    B3D_SET_BEGIN(iface);
    This->src3d_pending.cone_outside_volume = vol;
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetMaxDistance(IDirectSound3DBuffer *iface, D3DVALUE d, DWORD apply)
{
    B3D_SET_BEGIN(iface);
    This->src3d_pending.max_dist = d;
    LOG_3D("Buffer[%d]::SetMaxDistance %.3f", This->id, d);
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetMinDistance(IDirectSound3DBuffer *iface, D3DVALUE d, DWORD apply)
{
    B3D_SET_BEGIN(iface);
    This->src3d_pending.min_dist = d;
    LOG_3D("Buffer[%d]::SetMinDistance %.3f", This->id, d);
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetMode(IDirectSound3DBuffer *iface, DWORD mode, DWORD apply)
{
    if (mode > DS3DMODE_DISABLE) return DSERR_INVALIDPARAM;
    B3D_SET_BEGIN(iface);
    This->src3d_pending.mode = mode;
    LOG_3D("Buffer[%d]::SetMode %lu", This->id, (unsigned long)mode);
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetPosition(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    B3D_SET_BEGIN(iface);
    This->src3d_pending.pos = to_v(x, y, z);
    LOG_3D("Buffer[%d]::SetPosition %.3f,%.3f,%.3f (%s)", This->id, x, y, z, apply ? "deferred" : "immediate");
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetVelocity(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    B3D_SET_BEGIN(iface);
    This->src3d_pending.vel = to_v(x, y, z);
    B3D_SET_END(apply);
}

static IDirectSound3DBufferVtbl b3d_vtbl = {
    b3d_QueryInterface, b3d_AddRef, b3d_Release,
    b3d_GetAllParameters, b3d_GetConeAngles, b3d_GetConeOrientation, b3d_GetConeOutsideVolume,
    b3d_GetMaxDistance, b3d_GetMinDistance, b3d_GetMode, b3d_GetPosition, b3d_GetVelocity,
    b3d_SetAllParameters, b3d_SetConeAngles, b3d_SetConeOrientation, b3d_SetConeOutsideVolume,
    b3d_SetMaxDistance, b3d_SetMinDistance, b3d_SetMode, b3d_SetPosition, b3d_SetVelocity,
};

/* =========================================================== IDirectSound3DListener (primary) */

static HRESULT WINAPI lst_QueryInterface(IDirectSound3DListener *iface, REFIID riid, void **out)
{ return dsb_QueryInterface(&LISTENER(iface)->IDirectSoundBuffer8_iface, riid, out); }
static ULONG WINAPI lst_AddRef(IDirectSound3DListener *iface)
{ return dsb_AddRef(&LISTENER(iface)->IDirectSoundBuffer8_iface); }
static ULONG WINAPI lst_Release(IDirectSound3DListener *iface)
{ return dsb_Release(&LISTENER(iface)->IDirectSoundBuffer8_iface); }

#define LST_DEV(iface) (LISTENER(iface)->dev)

static void lst_changed(rl_device *dev, DWORD apply)
{
    dev->listener_dirty = TRUE;
    if (apply != DS3D_DEFERRED) rl_device_commit_3d(dev);
}

static HRESULT WINAPI lst_GetAllParameters(IDirectSound3DListener *iface, LPDS3DLISTENER p)
{
    const rl_listener3d *l = &LST_DEV(iface)->listener_pending;
    if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM;
    p->vPosition.x = l->pos.x; p->vPosition.y = l->pos.y; p->vPosition.z = l->pos.z;
    p->vVelocity.x = l->vel.x; p->vVelocity.y = l->vel.y; p->vVelocity.z = l->vel.z;
    p->vOrientFront.x = l->front.x; p->vOrientFront.y = l->front.y; p->vOrientFront.z = l->front.z;
    p->vOrientTop.x = l->top.x; p->vOrientTop.y = l->top.y; p->vOrientTop.z = l->top.z;
    p->flDistanceFactor = l->distance_factor;
    p->flRolloffFactor = l->rolloff_factor;
    p->flDopplerFactor = l->doppler_factor;
    return DS_OK;
}

static HRESULT WINAPI lst_GetDistanceFactor(IDirectSound3DListener *iface, D3DVALUE *v)
{ if (!v) return DSERR_INVALIDPARAM; *v = LST_DEV(iface)->listener_pending.distance_factor; return DS_OK; }
static HRESULT WINAPI lst_GetDopplerFactor(IDirectSound3DListener *iface, D3DVALUE *v)
{ if (!v) return DSERR_INVALIDPARAM; *v = LST_DEV(iface)->listener_pending.doppler_factor; return DS_OK; }
static HRESULT WINAPI lst_GetRolloffFactor(IDirectSound3DListener *iface, D3DVALUE *v)
{ if (!v) return DSERR_INVALIDPARAM; *v = LST_DEV(iface)->listener_pending.rolloff_factor; return DS_OK; }

static HRESULT WINAPI lst_GetOrientation(IDirectSound3DListener *iface, D3DVECTOR *front, D3DVECTOR *top)
{
    const rl_listener3d *l = &LST_DEV(iface)->listener_pending;
    if (!front || !top) return DSERR_INVALIDPARAM;
    front->x = l->front.x; front->y = l->front.y; front->z = l->front.z;
    top->x = l->top.x; top->y = l->top.y; top->z = l->top.z;
    return DS_OK;
}

static HRESULT WINAPI lst_GetPosition(IDirectSound3DListener *iface, D3DVECTOR *v)
{
    const rl_listener3d *l = &LST_DEV(iface)->listener_pending;
    if (!v) return DSERR_INVALIDPARAM;
    v->x = l->pos.x; v->y = l->pos.y; v->z = l->pos.z;
    return DS_OK;
}

static HRESULT WINAPI lst_GetVelocity(IDirectSound3DListener *iface, D3DVECTOR *v)
{
    const rl_listener3d *l = &LST_DEV(iface)->listener_pending;
    if (!v) return DSERR_INVALIDPARAM;
    v->x = l->vel.x; v->y = l->vel.y; v->z = l->vel.z;
    return DS_OK;
}

#define LST_SET_BEGIN(iface) rl_device *dev = LST_DEV(iface); EnterCriticalSection(&dev->lock); rl_listener3d *l = &dev->listener_pending
#define LST_SET_END(apply)   lst_changed(dev, apply); LeaveCriticalSection(&dev->lock); return DS_OK

static HRESULT WINAPI lst_SetAllParameters(IDirectSound3DListener *iface, LPCDS3DLISTENER p, DWORD apply)
{
    if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM;
    LST_SET_BEGIN(iface);
    l->pos = to_v(p->vPosition.x, p->vPosition.y, p->vPosition.z);
    l->vel = to_v(p->vVelocity.x, p->vVelocity.y, p->vVelocity.z);
    l->front = to_v(p->vOrientFront.x, p->vOrientFront.y, p->vOrientFront.z);
    l->top = to_v(p->vOrientTop.x, p->vOrientTop.y, p->vOrientTop.z);
    l->distance_factor = p->flDistanceFactor;
    l->rolloff_factor = p->flRolloffFactor;
    l->doppler_factor = p->flDopplerFactor;
    LOG_3D("Listener::SetAllParameters (%s)", apply ? "deferred" : "immediate");
    LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetDistanceFactor(IDirectSound3DListener *iface, D3DVALUE v, DWORD apply)
{
    if (v < DS3D_MINDISTANCEFACTOR || v > DS3D_MAXDISTANCEFACTOR) return DSERR_INVALIDPARAM;
    LST_SET_BEGIN(iface); l->distance_factor = v; LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetDopplerFactor(IDirectSound3DListener *iface, D3DVALUE v, DWORD apply)
{
    if (v < DS3D_MINDOPPLERFACTOR || v > DS3D_MAXDOPPLERFACTOR) return DSERR_INVALIDPARAM;
    LST_SET_BEGIN(iface); l->doppler_factor = v; LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetOrientation(IDirectSound3DListener *iface, D3DVALUE fx, D3DVALUE fy, D3DVALUE fz,
                                         D3DVALUE tx, D3DVALUE ty, D3DVALUE tz, DWORD apply)
{
    LST_SET_BEGIN(iface);
    l->front = to_v(fx, fy, fz);
    l->top = to_v(tx, ty, tz);
    LOG_3D("Listener::SetOrientation front %.3f,%.3f,%.3f top %.3f,%.3f,%.3f (%s)", fx, fy, fz, tx, ty, tz,
           apply ? "deferred" : "immediate");
    LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetPosition(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    LST_SET_BEGIN(iface);
    l->pos = to_v(x, y, z);
    LOG_3D("Listener::SetPosition %.3f,%.3f,%.3f (%s)", x, y, z, apply ? "deferred" : "immediate");
    LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetRolloffFactor(IDirectSound3DListener *iface, D3DVALUE v, DWORD apply)
{
    if (v < DS3D_MINROLLOFFFACTOR || v > DS3D_MAXROLLOFFFACTOR) return DSERR_INVALIDPARAM;
    LST_SET_BEGIN(iface); l->rolloff_factor = v; LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetVelocity(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    LST_SET_BEGIN(iface); l->vel = to_v(x, y, z); LST_SET_END(apply);
}

static HRESULT WINAPI lst_CommitDeferredSettings(IDirectSound3DListener *iface)
{
    rl_device *dev = LST_DEV(iface);
    EnterCriticalSection(&dev->lock);
    dev->listener_dirty = TRUE;
    rl_device_commit_3d(dev);
    LeaveCriticalSection(&dev->lock);
    return DS_OK;
}

static IDirectSound3DListenerVtbl lst_vtbl = {
    lst_QueryInterface, lst_AddRef, lst_Release,
    lst_GetAllParameters, lst_GetDistanceFactor, lst_GetDopplerFactor, lst_GetOrientation,
    lst_GetPosition, lst_GetRolloffFactor, lst_GetVelocity,
    lst_SetAllParameters, lst_SetDistanceFactor, lst_SetDopplerFactor, lst_SetOrientation,
    lst_SetPosition, lst_SetRolloffFactor, lst_SetVelocity, lst_CommitDeferredSettings,
};

/* =========================================================== IKsPropertySet (EAX entry point) */

static HRESULT WINAPI ks_QueryInterface(IKsPropertySet *iface, REFIID riid, void **out)
{ return dsb_QueryInterface(&PROPS(iface)->IDirectSoundBuffer8_iface, riid, out); }
static ULONG WINAPI ks_AddRef(IKsPropertySet *iface)
{ return dsb_AddRef(&PROPS(iface)->IDirectSoundBuffer8_iface); }
static ULONG WINAPI ks_Release(IKsPropertySet *iface)
{ return dsb_Release(&PROPS(iface)->IDirectSoundBuffer8_iface); }

/* EAX 1.0 property sets (what ALchemy answers, see dstrace of NFS4) */
static const GUID RL_EAX1_LISTENER =
    { 0x4a4e6fc1, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID RL_EAX1_BUFFER =
    { 0x4a4e6fc0, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID RL_DSPROPSETID_VoiceManager =
    { 0x62a69bae, 0xdf9d, 0x11d1, { 0x99, 0xa6, 0x00, 0xc0, 0x4f, 0xc9, 0x9d, 0x46 } };

/* EAX 2.0 and 3.0 property sets */
static const GUID RL_EAX2_LISTENER =
    { 0x0306a6a8, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } };
static const GUID RL_EAX2_BUFFER =
    { 0x0306a6a7, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } };
static const GUID RL_EAX3_LISTENER =
    { 0xa8fa6882, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } };
static const GUID RL_EAX3_BUFFER =
    { 0xa8fa6881, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } };

/* which EAX 2/3 set a GUID is: 2/3 listener -> 2/3, buffer -> 12/13, else 0 */
static int eax23_set(REFGUID set)
{
    if (IsEqualGUID(set, &RL_EAX2_LISTENER)) return 2;
    if (IsEqualGUID(set, &RL_EAX3_LISTENER)) return 3;
    if (IsEqualGUID(set, &RL_EAX2_BUFFER)) return 12;
    if (IsEqualGUID(set, &RL_EAX3_BUFFER)) return 13;
    return 0;
}

static HRESULT eax_hr(int r)
{
    return r == EAX_OK ? DS_OK : r == EAX_BADID ? E_PROP_ID_UNSUPPORTED : DSERR_INVALIDPARAM;
}

static void log_eax23(rl_buffer *b, const char *op, int which, ULONG id, const void *data, ULONG len, HRESULT hr)
{
    if (!g_cfg.log_eax) return;
    static const char *names[] = { [2] = "EAX2 listener", [3] = "EAX3 listener", [12] = "EAX2 buffer", [13] = "EAX3 buffer" };
    char v[64] = "";
    if (data && len >= 4) {
        int32_t i; float f;
        memcpy(&i, data, 4); memcpy(&f, data, 4);
        snprintf(v, sizeof(v), " %ld / %g", (long)i, f);
    }
    rl_log(0, "Buffer[%d]::KsProp %s %s id %lu%s len %lu%s -> %08lx", b->id, op, names[which],
           (unsigned long)(id & ~EAX_DEFERRED), (id & EAX_DEFERRED) ? " (deferred)" : "", (unsigned long)len, v,
           (unsigned long)hr);
}

enum { EAX1_L_ALL, EAX1_L_ENVIRONMENT, EAX1_L_VOLUME, EAX1_L_DECAYTIME, EAX1_L_DAMPING, EAX1_L_COUNT };
enum { EAX1_B_ALL, EAX1_B_REVERBMIX, EAX1_B_COUNT };
typedef struct { DWORD environment; float volume, decay, damping; } rl_eax1_reverb;

static HRESULT ks_get(IKsPropertySet *iface, REFGUID set, ULONG id, LPVOID inst, ULONG inst_len,
                             LPVOID data, ULONG data_len, PULONG ret)
{
    rl_buffer *This = PROPS(iface);
    rl_device *dev = This->dev;
    char gb[64];
    (void)inst; (void)inst_len;
    if (ret) *ret = 0;
    HRESULT hr = E_PROP_ID_UNSUPPORTED;

    int which = eax23_set(set);
    if (which) {
        if (which >= 12 && !(This->desc.dwFlags & DSBCAPS_CTRL3D)) return E_PROP_ID_UNSUPPORTED;
        uint32_t got = 0;
        int r;
        EnterCriticalSection(&dev->lock);
        rl_device_eax_use(dev, which % 10);
        switch (which) {
        case 2:  r = eax2l_get(&dev->eax2l, id, data, data_len, &got); break;
        case 3:  r = eax3l_get(&dev->eax3l, id, data, data_len, &got); break;
        case 12: r = eax2b_get(&This->eax2, id, data, data_len, &got); break;
        default: r = eax3b_get(&This->eax3, id, data, data_len, &got); break;
        }
        LeaveCriticalSection(&dev->lock);
        if (ret) *ret = got;
        hr = eax_hr(r);
        log_eax23(This, "Get", which, id, r == EAX_OK ? data : NULL, got, hr);
        return hr;
    }

    EnterCriticalSection(&dev->lock);
    if (IsEqualGUID(set, &RL_EAX1_LISTENER)) {
        rl_device_eax_use(dev, 1);
        switch (id) {
        case EAX1_L_ALL:
            if (data_len < sizeof(rl_eax1_reverb) || !data) { hr = DSERR_INVALIDPARAM; break; }
            ((rl_eax1_reverb *)data)->environment = dev->eax1_env;
            ((rl_eax1_reverb *)data)->volume = dev->eax1_volume;
            ((rl_eax1_reverb *)data)->decay = dev->eax1_decay;
            ((rl_eax1_reverb *)data)->damping = dev->eax1_damping;
            if (ret) *ret = sizeof(rl_eax1_reverb);
            hr = DS_OK; break;
        case EAX1_L_ENVIRONMENT: case EAX1_L_VOLUME: case EAX1_L_DECAYTIME: case EAX1_L_DAMPING:
            if (data_len < 4 || !data) { hr = DSERR_INVALIDPARAM; break; }
            if (id == EAX1_L_ENVIRONMENT) *(DWORD *)data = dev->eax1_env;
            else *(float *)data = id == EAX1_L_VOLUME ? dev->eax1_volume : id == EAX1_L_DECAYTIME ? dev->eax1_decay : dev->eax1_damping;
            if (ret) *ret = 4;
            hr = DS_OK; break;
        }
    } else if (IsEqualGUID(set, &RL_EAX1_BUFFER) && (This->desc.dwFlags & DSBCAPS_CTRL3D)) {
        rl_device_eax_use(dev, 1);
        if (id < EAX1_B_COUNT) {
            if (data_len < 4 || !data) hr = DSERR_INVALIDPARAM;
            else { *(float *)data = This->eax1_mix; if (ret) *ret = 4; hr = DS_OK; }
        }
    }
    LeaveCriticalSection(&dev->lock);
    LOG_EAX("Buffer[%d]::KsProp Get %s id %lu -> %08lx", This->id, rl_guid_str(set, gb), (unsigned long)id, (unsigned long)hr);
    return hr;
}

static HRESULT ks_set(IKsPropertySet *iface, REFGUID set, ULONG id, LPVOID inst, ULONG inst_len,
                             LPVOID data, ULONG data_len)
{
    rl_buffer *This = PROPS(iface);
    rl_device *dev = This->dev;
    char gb[64];
    (void)inst; (void)inst_len;
    HRESULT hr = E_PROP_ID_UNSUPPORTED;

    int which = eax23_set(set);
    if (which) {
        if (which >= 12 && !(This->desc.dwFlags & DSBCAPS_CTRL3D)) return E_PROP_ID_UNSUPPORTED;
        int r;
        BOOL now = !(id & EAX_DEFERRED);
        EnterCriticalSection(&dev->lock);
        rl_device_eax_use(dev, which % 10);
        switch (which) {
        case 2:  r = eax2l_set(&dev->eax2l, id, data, data_len); break;
        case 3:  r = eax3l_set(&dev->eax3l, id, data, data_len); break;
        case 12: r = eax2b_set(&This->eax2, id, data, data_len); break;
        default: r = eax3b_set(&This->eax3, id, data, data_len); break;
        }
        /* deferred settings take effect with the next immediate one (or a commit, id 0) */
        if (r == EAX_OK && now) {
            if (which < 10) rl_device_eax_listener_changed(dev);
            else rl_buffer_update_gains(This);
        }
        LeaveCriticalSection(&dev->lock);
        hr = eax_hr(r);
        log_eax23(This, "Set", which, id, data, data_len, hr);
        return hr;
    }

    EnterCriticalSection(&dev->lock);
    if (IsEqualGUID(set, &RL_EAX1_LISTENER) && id < EAX1_L_COUNT) {
        rl_device_eax_use(dev, 1);
        if (!data || data_len < (id == EAX1_L_ALL ? sizeof(rl_eax1_reverb) : 4)) hr = DSERR_INVALIDPARAM;
        else {
            hr = DS_OK;
            switch (id) {
            case EAX1_L_ALL: {
                const rl_eax1_reverb *r = data;
                if (r->volume != r->volume || r->decay != r->decay || r->damping != r->damping) {
                    hr = DSERR_INVALIDPARAM; break;   /* ALchemy rejects NaNs */
                }
                dev->eax1_env = r->environment > 25 ? 25 : r->environment;
                dev->eax1_volume = r->volume;
                dev->eax1_decay = r->decay;
                dev->eax1_damping = r->damping;
                break;
            }
            case EAX1_L_ENVIRONMENT: {
                DWORD env = *(const DWORD *)data;
                if (env > 25) env = 25;               /* ALchemy clamps */
                if (env == dev->eax1_env) break;      /* ALchemy: same env keeps tweaked values */
                dev->eax1_env = env;
                dev->eax1_volume = rl_eax1_presets[env].volume;
                dev->eax1_decay = rl_eax1_presets[env].decay;
                dev->eax1_damping = rl_eax1_presets[env].damping;
                break;
            }
            case EAX1_L_VOLUME: {
                float v = *(const float *)data;
                dev->eax1_volume = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
                break;
            }
            case EAX1_L_DECAYTIME: dev->eax1_decay = *(const float *)data; break;
            case EAX1_L_DAMPING:   dev->eax1_damping = *(const float *)data; break;
            }
            if (hr == DS_OK) rl_device_eax1_changed(dev);
        }
        LOG_EAX("Buffer[%d]::KsProp Set EAX1 listener id %lu -> env %lu vol %.3f decay %.3f damp %.3f (%08lx)", This->id,
                (unsigned long)id, (unsigned long)dev->eax1_env, dev->eax1_volume, dev->eax1_decay, dev->eax1_damping,
                (unsigned long)hr);
    } else if (IsEqualGUID(set, &RL_EAX1_BUFFER) && (This->desc.dwFlags & DSBCAPS_CTRL3D) && id < EAX1_B_COUNT) {
        rl_device_eax_use(dev, 1);
        if (!data || data_len < 4) hr = DSERR_INVALIDPARAM;
        else {
            float mix = *(const float *)data;
            if (mix != -1.0f && (mix < 0.0f || mix > 1.0f)) hr = DSERR_INVALIDPARAM;
            else { This->eax1_mix = mix; rl_buffer_update_gains(This); hr = DS_OK; }
        }
        LOG_EAX("Buffer[%d]::KsProp Set EAX1 reverb mix %.3f (%08lx)", This->id, This->eax1_mix, (unsigned long)hr);
    } else {
        LOG_EAX("Buffer[%d]::KsProp Set %s id %lu len %lu -> unsupported", This->id, rl_guid_str(set, gb),
                (unsigned long)id, (unsigned long)data_len);
    }
    LeaveCriticalSection(&dev->lock);
    return hr;
}

static HRESULT WINAPI ks_Get(IKsPropertySet *iface, REFGUID set, ULONG id, LPVOID inst, ULONG inst_len,
                             LPVOID data, ULONG data_len, PULONG ret)
{
    unsigned int cw = rl_fpu_enter();
    HRESULT hr = ks_get(iface, set, id, inst, inst_len, data, data_len, ret);
    rl_fpu_leave(cw);
    return hr;
}

static HRESULT WINAPI ks_Set(IKsPropertySet *iface, REFGUID set, ULONG id, LPVOID inst, ULONG inst_len,
                             LPVOID data, ULONG data_len)
{
    unsigned int cw = rl_fpu_enter();
    HRESULT hr = ks_set(iface, set, id, inst, inst_len, data, data_len);
    rl_fpu_leave(cw);
    return hr;
}

static HRESULT WINAPI ks_QuerySupport(IKsPropertySet *iface, REFGUID set, ULONG id, PULONG support)
{
    rl_buffer *This = PROPS(iface);
    char gb[64];
    HRESULT hr = E_PROP_ID_UNSUPPORTED;
    ULONG s = 0;
    if (IsEqualGUID(set, &RL_DSPROPSETID_VoiceManager)) {
        hr = DS_OK;                               /* ALchemy: S_OK, support 0 */
    } else if (IsEqualGUID(set, &RL_EAX1_LISTENER) && id < EAX1_L_COUNT) {
        hr = DS_OK; s = KSPROPERTY_SUPPORT_GET | KSPROPERTY_SUPPORT_SET;
    } else if (IsEqualGUID(set, &RL_EAX1_BUFFER) && id < EAX1_B_COUNT && (This->desc.dwFlags & DSBCAPS_CTRL3D)) {
        hr = DS_OK; s = KSPROPERTY_SUPPORT_GET | KSPROPERTY_SUPPORT_SET;
    } else {
        static const ULONG count[] = { [2] = EAX2L_COUNT, [3] = EAX3L_COUNT, [12] = EAX2B_COUNT, [13] = EAX3B_COUNT };
        int which = eax23_set(set);
        if (which && (id & ~EAX_DEFERRED) < count[which] && (which < 12 || (This->desc.dwFlags & DSBCAPS_CTRL3D))) {
            hr = DS_OK; s = KSPROPERTY_SUPPORT_GET | KSPROPERTY_SUPPORT_SET;
        }
    }
    if (support) *support = s;
    LOG_EAX("Buffer[%d]::KsProp QuerySupport %s id %lu -> %08lx support %lu", This->id, rl_guid_str(set, gb),
            (unsigned long)id, (unsigned long)hr, (unsigned long)s);
    return hr;
}

static IKsPropertySetVtbl ks_vtbl = {
    ks_QueryInterface, ks_AddRef, ks_Release, ks_Get, ks_Set, ks_QuerySupport,
};

/* =========================================================== creation */

static rl_buffer *alloc_buffer(rl_device *dev)
{
    rl_buffer *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b));
    if (!b) return NULL;
    b->IDirectSoundBuffer8_iface.lpVtbl = &dsb_vtbl;
    b->IDirectSound3DBuffer_iface.lpVtbl = &b3d_vtbl;
    b->IDirectSoundNotify_iface.lpVtbl = &ntf_vtbl;
    b->IKsPropertySet_iface.lpVtbl = &ks_vtbl;
    b->IDirectSound3DListener_iface.lpVtbl = &lst_vtbl;
    b->ref = 1;
    b->id = InterlockedIncrement(&s_next_id);
    b->dev = dev;
    rl_source_defaults(&b->src3d);
    b->src3d_pending = b->src3d;
    b->eax1_mix = -1.0f;   /* EAX_REVERBMIX_USEDISTANCE */
    eax2b_defaults(&b->eax2);
    eax3b_defaults(&b->eax3);
    return b;
}

static void flags_str(DWORD f, char *out, size_t n)
{
    static const struct { DWORD f; const char *s; } t[] = {
        { DSBCAPS_PRIMARYBUFFER, "PRIMARY" }, { DSBCAPS_STATIC, "STATIC" }, { DSBCAPS_LOCHARDWARE, "LOCHARDWARE" },
        { DSBCAPS_LOCSOFTWARE, "LOCSOFTWARE" }, { DSBCAPS_CTRL3D, "CTRL3D" }, { DSBCAPS_CTRLFREQUENCY, "CTRLFREQUENCY" },
        { DSBCAPS_CTRLPAN, "CTRLPAN" }, { DSBCAPS_CTRLVOLUME, "CTRLVOLUME" }, { DSBCAPS_CTRLPOSITIONNOTIFY, "CTRLPOSITIONNOTIFY" },
        { DSBCAPS_CTRLFX, "CTRLFX" }, { DSBCAPS_STICKYFOCUS, "STICKYFOCUS" }, { DSBCAPS_GLOBALFOCUS, "GLOBALFOCUS" },
        { DSBCAPS_GETCURRENTPOSITION2, "GETCURRENTPOSITION2" }, { DSBCAPS_MUTE3DATMAXDISTANCE, "MUTE3DATMAXDISTANCE" },
        { DSBCAPS_LOCDEFER, "LOCDEFER" },
    };
    size_t len = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if ((f & t[i].f) && len + strlen(t[i].s) + 2 < n) {
            lstrcatA(out, t[i].s); lstrcatA(out, " ");
            len += strlen(t[i].s) + 1;
        }
}

HRESULT rl_buffer_create(rl_device *dev, rl_dsound *owner, LPCDSBUFFERDESC desc, rl_buffer **out)
{
    char fl[400];
    flags_str(desc->dwFlags, fl, sizeof(fl));
    *out = NULL;

    rl_buffer *b = alloc_buffer(dev);
    if (!b) return DSERR_OUTOFMEMORY;
    b->owner = owner;
    memset(&b->desc, 0, sizeof(b->desc));
    memcpy(&b->desc, desc, desc->dwSize);
    b->desc.dwSize = sizeof(b->desc);
    b->desc.lpwfxFormat = NULL;

    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
        b->is_primary = TRUE;
        rl_device_addref(dev);
        EnterCriticalSection(&dev->lock);
        list_add_all(dev, b);
        LeaveCriticalSection(&dev->lock);
        LOG_CALL("CreateSoundBuffer[%d] primary: %s", b->id, fl);
        *out = b;
        return DS_OK;
    }

    rl_sfmt sfmt;
    HRESULT hr = parse_format(desc->lpwfxFormat, &b->fmt, &sfmt);
    if (FAILED(hr)) {
        LOG_CALL("CreateSoundBuffer: bad format (tag %u)", desc->lpwfxFormat ? desc->lpwfxFormat->wFormatTag : 0);
        HeapFree(GetProcessHeap(), 0, b);
        return hr;
    }
    if ((desc->dwFlags & DSBCAPS_CTRL3D) && b->fmt.Format.nChannels != 1) {
        LOG_CALL("CreateSoundBuffer: 3D buffer must be mono");
        HeapFree(GetProcessHeap(), 0, b);
        return DSERR_INVALIDPARAM;
    }
    DWORD block = b->fmt.Format.nBlockAlign;
    DWORD bytes = desc->dwBufferBytes;
    if (bytes < DSBSIZE_MIN || bytes > DSBSIZE_MAX) {
        HeapFree(GetProcessHeap(), 0, b);
        return DSERR_INVALIDPARAM;
    }
    bytes -= bytes % block;
    if (!bytes) bytes = block;
    b->desc.dwBufferBytes = bytes;

    b->shared = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b->shared));
    BYTE *data = b->shared ? HeapAlloc(GetProcessHeap(), 0, bytes) : NULL;
    if (!data) {
        HeapFree(GetProcessHeap(), 0, b->shared);
        HeapFree(GetProcessHeap(), 0, b);
        return DSERR_OUTOFMEMORY;
    }
    memset(data, sfmt == RL_U8 ? 0x80 : 0, bytes);
    b->shared->ref = 1;
    b->shared->data = data;
    b->shared->size = bytes;

    b->voice.data = data;
    b->voice.interp = g_cfg.resampler;
    b->voice.size = bytes;
    b->voice.channels = b->fmt.Format.nChannels;
    b->voice.block = (uint16_t)block;
    b->voice.sfmt = sfmt;
    b->voice.rate = b->fmt.Format.nSamplesPerSec;
    b->src3d.mute_at_max = b->src3d_pending.mute_at_max = (desc->dwFlags & DSBCAPS_MUTE3DATMAXDISTANCE) != 0;

    if (desc->dwFlags & DSBCAPS_CTRL3D)
        b->hrtf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(rl_hrtf_voice));

    rl_device_addref(dev);
    EnterCriticalSection(&dev->lock);
    list_add_all(dev, b);
    dev->n_buffers++;
    if (!(b->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) dev->n_hw_buffers++;
    rl_buffer_update_gains(b);
    LeaveCriticalSection(&dev->lock);

    LOG_CALL("CreateSoundBuffer[%d] %lu bytes, %lu Hz %u ch %u bit: %s", b->id, (unsigned long)bytes,
             (unsigned long)b->fmt.Format.nSamplesPerSec, b->fmt.Format.nChannels, b->fmt.Format.wBitsPerSample, fl);
    *out = b;
    return DS_OK;
}

HRESULT rl_buffer_duplicate(rl_device *dev, rl_dsound *owner, rl_buffer *src, rl_buffer **out)
{
    *out = NULL;
    if (src->desc.dwFlags & DSBCAPS_CTRLFX) return DSERR_INVALIDCALL;
    rl_buffer *b = alloc_buffer(dev);
    if (!b) return DSERR_OUTOFMEMORY;
    b->owner = owner;

    EnterCriticalSection(&dev->lock);
    b->desc = src->desc;
    b->fmt = src->fmt;
    b->shared = src->shared;
    InterlockedIncrement(&b->shared->ref);
    b->voice = src->voice;
    b->voice.pos = 0;
    b->voice.playing = 0;
    b->voice.looping = 0;
    b->voice.notifies = NULL;
    b->voice.n_notifies = 0;
    b->volume = src->volume;
    b->pan = src->pan;
    b->freq = src->freq;
    b->src3d = src->src3d;
    b->src3d_pending = src->src3d_pending;
    b->eax1_mix = src->eax1_mix;
    b->eax2 = src->eax2;
    b->eax3 = src->eax3;
    b->voice.hrtf = NULL;
    if (b->desc.dwFlags & DSBCAPS_CTRL3D)
        b->hrtf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(rl_hrtf_voice));
    rl_buffer_update_gains(b);
    list_add_all(dev, b);
    dev->n_buffers++;
    if (!(b->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) dev->n_hw_buffers++;
    LeaveCriticalSection(&dev->lock);
    rl_device_addref(dev);

    LOG_CALL("DuplicateSoundBuffer[%d] from [%d]", b->id, src->id);
    *out = b;
    return DS_OK;
}

void rl_buffers_orphan(rl_device *dev, rl_dsound *owner)
{
    int n = 0, playing = 0;
    EnterCriticalSection(&dev->lock);
    for (rl_buffer *b = dev->all; b; b = b->next_all) {
        if (b->owner != owner) continue;
        b->owner = NULL;
        if (b->is_primary) continue;      /* released by the owner itself */
        b->dead = TRUE;
        if (b->voice.playing) playing++;
        b->voice.playing = 0;
        if (b->in_play_list) rl_device_stop_voice(dev, b);
        n++;
    }
    LeaveCriticalSection(&dev->lock);
    if (n) LOG_CALL("DirectSound released with %d buffer(s) still referenced by the game (%d playing): stopped", n, playing);
}
