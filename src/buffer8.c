/* reAL, included by buffer.c: IDirectSoundBuffer8 and IDirectSoundNotify. */
/* =========================================================== IDirectSoundBuffer8 */

static ULONG buf_addref(rl_buffer *b, int k);
static HRESULT WINAPI dsb_QueryInterface(IDirectSoundBuffer8 *iface, REFIID riid, void **out)
{
    rl_buffer *This = BUF(iface);
    char gb[64];
    if (!out) return E_POINTER;
    *out = NULL;
    /* ALchemy (tests/dsfuzz.c against it): IDirectSoundBuffer8 and IDirectSoundNotify on any buffer, the primary too
     * and without CTRLPOSITIONNOTIFY, and IDirectSound3DBuffer on any CTRL3D one; Windows on secondaries only, with
     * the flags */
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundBuffer) || IsEqualIID(riid, &IID_IDirectSoundBuffer8))
        *out = &This->IDirectSoundBuffer8_iface;
    else if (IsEqualIID(riid, &IID_IDirectSound3DBuffer) && (This->desc.dwFlags & DSBCAPS_CTRL3D))
        *out = &This->IDirectSound3DBuffer_iface;
    else if (IsEqualIID(riid, &IID_IDirectSoundNotify))
        *out = &This->IDirectSoundNotify_iface;
    else if (IsEqualIID(riid, &IID_IKsPropertySet))
        *out = &This->IKsPropertySet_iface;
    /* the listener: on a primary buffer made with CTRL3D (buffer.c listener_ok), and on a CTRL3D secondary, as
     * ALchemy */
    else if ((This->is_primary ? This->listener_ok : (This->desc.dwFlags & DSBCAPS_CTRL3D) != 0) &&
             IsEqualIID(riid, &IID_IDirectSound3DListener))
        *out = &This->IDirectSound3DListener_iface;

    if (*out) {
        int k = *out == &This->IDirectSoundBuffer8_iface ? RL_IREF_BUF : *out == &This->IDirectSound3DBuffer_iface ? RL_IREF_3D :
                *out == &This->IDirectSoundNotify_iface ? RL_IREF_NOTIFY : *out == &This->IKsPropertySet_iface ? RL_IREF_KS :
                RL_IREF_LISTENER;
        buf_addref(This, k);
        return S_OK;
    }
    LOG_CALL("Buffer[%d]::QueryInterface unsupported %s", This->id, rl_guid_str(riid, gb));
    return E_NOINTERFACE;
}

/* Each interface counts its own references, and AddRef / Release answer that count, as ALchemy's (0x1001db50,
 * 0x1001fbf0) and Windows' do; the object goes when all of them are released.  A game that releases a buffer until
 * Release answers 0 while it still holds the buffer's 3D interface (Fallout 3, Fallout: New Vegas) then keeps the
 * buffer: with one count for all, the loop released the 3D interface's reference too, and the game's next call on
 * it crashed.  An interface released more often than it was referenced answers 0 (ALchemy's). */
static ULONG buf_addref(rl_buffer *b, int k)
{
    InterlockedIncrement(&b->ref);
    return InterlockedIncrement(&b->iref[k]);
}
static ULONG buf_release(rl_buffer *b, int k);
static void zombify(rl_buffer *b);
static ULONG WINAPI dsb_AddRef(IDirectSoundBuffer8 *iface) { return buf_addref(BUF(iface), RL_IREF_BUF); }
static ULONG WINAPI dsb_Release(IDirectSoundBuffer8 *iface) { return buf_release(BUF(iface), RL_IREF_BUF); }

static ULONG buf_release(rl_buffer *b, int k)
{
    LONG own, was;
    do {                                    /* (the interface's own count stops at 0) */
        was = b->iref[k];
        own = was > 0 ? was - 1 : 0;
    } while (was > 0 && InterlockedCompareExchange(&b->iref[k], own, was) != was);
    rl_buffer *This = b;
    ULONG ref;
    if (This->is_primary) {
        /* its owner hands it out again while it is alive (under the lock): it forgets it as it goes */
        rl_device *d = rl_buffer_lock(This);
        ref = InterlockedDecrement(&This->ref);
        if (!ref && This->owner && This->owner->primary == This) This->owner->primary = NULL;
        LeaveCriticalSection(&d->lock);
    } else ref = InterlockedDecrement(&This->ref);
    if (ref) return (ULONG)own;

    LOG_CALL("Buffer[%d] destroyed", This->id);
    rl_device *dev = rl_buffer_lock(This);
    if (This->in_play_list) rl_device_stop_voice(dev, This);
    list_remove_all(dev, This);
    if (!This->is_primary) {
        dev->n_buffers--;
        if (!(This->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) dev->n_hw_buffers--;
    }
    LeaveCriticalSection(&dev->lock);

    rl_fxchain_free(This->fx);
    HeapFree(GetProcessHeap(), 0, This->fxdata);
    if (This->shared && !InterlockedDecrement(&This->shared->ref)) {
        HeapFree(GetProcessHeap(), 0, This->shared->data);
        HeapFree(GetProcessHeap(), 0, This->shared->wfx);
        HeapFree(GetProcessHeap(), 0, This->shared);
    }
    HeapFree(GetProcessHeap(), 0, This->notifies);
    HeapFree(GetProcessHeap(), 0, This->hrtf);
    cmss_free(&This->voice);
    zombify(This);                          /* (not freed: see zombify) */
    rl_device_release(dev);
    return (ULONG)own;
}

static HRESULT WINAPI dsb_GetCaps(IDirectSoundBuffer8 *iface, LPDSBCAPS caps)
{
    rl_buffer *This = BUF(iface);
    if (!caps || caps->dwSize < sizeof(*caps)) return DSERR_INVALIDPARAM;
    caps->dwFlags = This->desc.dwFlags;
    /* hardware unless asked for software, effects buffers too (ALchemy: tests/alprobe.c create; LOCDEFER kept, as
     * ALchemy 0x1001dbd0 and Windows keep it) */
    if (!(caps->dwFlags & DSBCAPS_LOCSOFTWARE)) caps->dwFlags |= DSBCAPS_LOCHARDWARE;
    if (This->is_primary) {
        rl_buffer_lock(This);
        caps->dwBufferBytes = writeprimary(This) ? PRIMARY_BYTES : 4096;
        LeaveCriticalSection(&This->dev->lock);
    } else caps->dwBufferBytes = This->desc.dwBufferBytes;
    if (This->sink) { caps->dwFlags = This->sink_caps; caps->dwBufferBytes = 0; }   /* as DirectSound's sink buffers */
    caps->dwUnlockTransferRate = 4096;
    caps->dwPlayCpuOverhead = 0;
    return DS_OK;
}

static DWORD write_lead_bytes(rl_buffer *This)
{
    /* how far ahead of the play cursor the "write" cursor sits: ~15 ms + one mix period */
    unsigned long long rate = This->voice.rate ? This->voice.rate : orig_rate(This);
    unsigned long long frames = rate * 15 / 1000;
    if (This->dev->out_rate) frames += (unsigned long long)This->dev->period_frames * rate / This->dev->out_rate;
    unsigned long long bytes = frames * This->voice.block;    /* (64-bit: any frequency is taken; at most the buffer) */
    return bytes < This->voice.size ? (DWORD)bytes : This->voice.size;
}

/* effects: a fresh start at the play cursor (Play, SetCurrentPosition call Discontinuity).  DirectSound's mixer then
 * runs the chain on the 32 frames before the cursor first (from the end of the buffer at 0, looping or not) */
#define FX_PREROLL 32
static void fx_chunk(rl_buffer *b, uint32_t at, uint32_t n)
{
    uint32_t block = b->voice.block;
    BYTE *d = b->fxdata + (size_t)at * block;
    memcpy(d, b->shared->data + (size_t)at * block, (size_t)n * block);
    rl_fxchain_process(b->fx, d, n * block);
}
static void fx_restart(rl_buffer *b)
{
    uint32_t frames = rl_voice_frames(&b->voice), play = (uint32_t)(b->voice.pos >> 32);
    rl_fxchain_discontinuity(b->fx);
    b->fx_head = b->fx_last = play;
    b->fx_lead = 0;
    if (!frames || play >= frames || !b->fxdata) return;
    uint32_t pre = FX_PREROLL < frames ? FX_PREROLL : frames - 1, at = (play + frames - pre) % frames;
    unsigned int cw = rl_fpu_enter();
    while (pre) {
        uint32_t n = pre < frames - at ? pre : frames - at;
        fx_chunk(b, at, n);
        at = (at + n) % frames;
        pre -= n;
    }
    rl_fpu_leave(cw);
}

/* before the mixer reads the voice: the effects run on the next frames of the application's data, copied to the
 * buffer the voice plays, until DirectSound's 150 ms (in 20 ms pieces) are processed ahead - or at least what this
 * mix will read */
void rl_buffer_fx_run(rl_buffer *b, UINT32 out_frames, UINT32 out_rate)
{
    if (!b->fx || !b->fxdata) return;
    uint32_t frames = rl_voice_frames(&b->voice);
    uint32_t play = (uint32_t)(b->voice.pos >> 32);
    if (!frames || play >= frames) return;
    uint32_t moved = (play + frames - b->fx_last) % frames;
    b->fx_last = play;
    if (moved > b->fx_lead) {           /* the mix got past what was processed: carry on from the cursor */
        b->fx_head = play;
        b->fx_lead = 0;
    } else b->fx_lead -= moved;
    uint32_t rate = orig_rate(b);
    double speed = (double)b->voice.rate * (b->voice.pitch > 0.0f ? b->voice.pitch : 1.0f) / (out_rate ? out_rate : 44100);
    /* 150 ms on from where the pre-roll started */
    uint32_t want = rate * 150 / 1000, need = (uint32_t)(out_frames * speed) + 64, chunk = rate / 50;
    want = want > FX_PREROLL ? want - FX_PREROLL : 0;      /* (below about 210 Hz the pre-roll is longer than that) */
    if (want < need) want = need;
    if (want > frames - 1) want = frames - 1;
    if (!chunk) chunk = 1;
    unsigned int cw = rl_fpu_enter();
    while (b->fx_lead < want) {
        if (b->fx_head >= frames) {
            if (!b->voice.looping) break;
            b->fx_head = 0;
        }
        uint32_t n = chunk;
        if (n > frames - b->fx_head) n = frames - b->fx_head;
        if (n > want - b->fx_lead + chunk) n = want - b->fx_lead + chunk;
        fx_chunk(b, b->fx_head, n);
        b->fx_head += n;
        b->fx_lead += n;
        if (b->fx_head >= frames && b->voice.looping) b->fx_head = 0;
    }
    rl_fpu_leave(cw);
}

/* (lock held) the primary buffer's play cursor p (where the last mix left it) moved back to the start of what that
 * mix read, then on through it at the playback rate for the time since */
static DWORD primary_cursor(const rl_buffer *b, DWORD p)
{
    static LONGLONG freq;
    const DWORD size = b->voice.size, block = b->voice.block;
    if (!size || !block) return p;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG ticks = now.QuadPart - b->mix_qpc;
    const DWORD chunk = (p + size - b->mix_from) % size / block;     /* frames the last mix read */
    if (ticks <= 0) return b->mix_from;
    const ULONGLONG frames = (ULONGLONG)ticks * b->voice.rate / (ULONGLONG)freq;
    if (frames >= chunk) return p;
    return (b->mix_from + (DWORD)frames * block) % size;
}

static HRESULT WINAPI dsb_GetCurrentPosition(IDirectSoundBuffer8 *iface, LPDWORD play, LPDWORD write)
{
    rl_buffer *This = BUF(iface);
    rl_buffer_lock(This);
    if (This->lost) {                       /* (Windows: Lock, Play, Stop and the positions answer that) */
        LeaveCriticalSection(&This->dev->lock);
        return DSERR_BUFFERLOST;
    }
    if (This->is_primary && !writeprimary(This)) {
        LeaveCriticalSection(&This->dev->lock);
        if (play) *play = 0;
        if (write) *write = 0;
        return DS_OK;
    }
    /* the play cursor is the first frame a mix still reads (the sinc resampler's taps reach back of the position),
     * playing or stopped: Play goes on from it, the notifications between it and the position still to come */
    DWORD at = rl_voice_play_bytes(&This->voice), p = rl_voice_cursor_bytes(&This->voice);
    DWORD w = p;
    /* the primary buffer a game writes itself: the play cursor moves on between mixes with the time since the last
     * one, through the frames that mix read, as a sound card's did.  A step a mix (10 ms, Windows 7's) is too coarse
     * for a game that mixes as far as the cursor moved: Need for Speed II's sound and videos skipped ahead.  It never
     * passes the frames mixed already, and the write cursor stays where it was */
    if (This->is_primary && g_cfg.fixes && This->voice.playing && This->mix_qpc && p == This->mix_to)
        p = primary_cursor(This, p);
    if (This->voice.playing) {
        DWORD lead = write_lead_bytes(This);
        /* with effects, the cursor is past what is processed already (DirectSound's write-ahead) */
        if (This->fx && This->fx_lead * This->voice.block > lead) lead = This->fx_lead * This->voice.block;
        w = at + lead;
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
    const WAVEFORMATEX *src = This->is_primary ? &This->dev->primary_fmt :
                              This->shared && This->shared->wfx ? This->shared->wfx : &This->fmt.Format;
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
    *vol = This->is_primary ? This->dev->primary_volume : This->volume;
    return DS_OK;
}

static HRESULT WINAPI dsb_GetPan(IDirectSoundBuffer8 *iface, LPLONG pan)
{
    rl_buffer *This = BUF(iface);
    if (!pan) return DSERR_INVALIDPARAM;
    *pan = This->pan;          /* (ALchemy answers pan * 65536 / 100, which no game could use: the pan as set) */
    return DS_OK;
}

static HRESULT WINAPI dsb_GetFrequency(IDirectSoundBuffer8 *iface, LPDWORD freq)
{
    rl_buffer *This = BUF(iface);
    if (!freq) return DSERR_INVALIDPARAM;
    if (This->is_primary) { *freq = This->dev->primary_fmt.nSamplesPerSec; return DS_OK; }
    *freq = This->freq ? This->freq : orig_rate(This);
    return DS_OK;
}

static HRESULT WINAPI dsb_GetStatus(IDirectSoundBuffer8 *iface, LPDWORD status)
{
    rl_buffer *This = BUF(iface);
    if (!status) return DSERR_INVALIDPARAM;
    DWORD s = 0;
    rl_buffer_lock(This);
    if (This->is_primary) {
        /* no location (ALchemy 0x1001dda0, and Windows') */
        if (This->dev->primary_playing || This->dev->playing) s = DSBSTATUS_PLAYING | DSBSTATUS_LOOPING;
        LeaveCriticalSection(&This->dev->lock);
        *status = s;
        return DS_OK;
    }
    if (This->lost) {
        LeaveCriticalSection(&This->dev->lock);
        *status = DSBSTATUS_BUFFERLOST;
        return DS_OK;
    }
    if (This->voice.playing) {
        s = DSBSTATUS_PLAYING;
        if (This->voice.looping) s |= DSBSTATUS_LOOPING;
    }
    LeaveCriticalSection(&This->dev->lock);
    /* DirectMusic's buffers (Microsoft DirectSound's under ALchemy too): in software where DirectMusic deferred them
     * (its standard ones).  ALchemy's own answer no location (tests/alprobe.c play) */
    if (This->sink && (This->sink_caps & DSBCAPS_LOCDEFER)) s |= DSBSTATUS_LOCSOFTWARE;
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
    if (This->is_primary || This->lost) {
        rl_buffer_lock(This);
        BOOL lost = This->lost;
        LeaveCriticalSection(&This->dev->lock);
        if (lost) return DSERR_BUFFERLOST;
        /* the primary buffer below DSSCL_WRITEPRIMARY: ALchemy locks it (what is written there plays nowhere);
         * Windows answers DSERR_PRIOLEVELNEEDED */
    }
    if (!p1 || !b1) return DSERR_INVALIDPARAM;

    DWORD size = This->voice.size;
    if (This->is_primary) {                 /* (the size GetCaps answers) */
        rl_buffer_lock(This);
        size = writeprimary(This) ? PRIMARY_BYTES : 4096;
        LeaveCriticalSection(&This->dev->lock);
    }
    if (flags & DSBLOCK_FROMWRITECURSOR) dsb_GetCurrentPosition(iface, NULL, &offset);
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = size;
    /* (no bytes: ALchemy locks nothing at the offset, tests/alprobe.c lock; Windows refuses) */
    if (offset >= size || bytes > size) return DSERR_INVALIDPARAM;

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
    InterlockedIncrement(&This->locks);
    LOG_TRACE("Buffer[%d]::Lock off %lu bytes %lu flags %lx -> %lu + %lu", This->id, (unsigned long)offset,
              (unsigned long)bytes, (unsigned long)flags, (unsigned long)*b1, (unsigned long)(b2 ? *b2 : 0));
    return DS_OK;
}

static HRESULT WINAPI dsb_Play(IDirectSoundBuffer8 *iface, DWORD res1, DWORD prio, DWORD flags)
{
    rl_buffer *This = BUF(iface);
    (void)prio; (void)res1;                     /* (ALchemy takes any reserved value: tests/alprobe.c play) */
    LOG_TRACE("Buffer[%d]::Play flags %lx", This->id, (unsigned long)flags);
    rl_buffer_lock(This);
    if (This->dead) {
        LeaveCriticalSection(&This->dev->lock);
        LOG_CALL("Buffer[%d]::Play on a buffer whose DirectSound object was released", This->id);
        return DSERR_BUFFERLOST;
    }
    if (This->lost) {
        LeaveCriticalSection(&This->dev->lock);
        return DSERR_BUFFERLOST;
    }
    if (This->is_primary) {
        if (writeprimary(This)) {
            /* its owner's own mix: played looping only (Windows refuses Play without DSBPLAY_LOOPING then) */
            if (!(flags & DSBPLAY_LOOPING)) {
                LeaveCriticalSection(&This->dev->lock);
                return DSERR_INVALIDPARAM;
            }
            if (This->voice.block) {
                This->voice.looping = 1;
                rl_voice_start(&This->voice);
                rl_device_start_voice(This->dev, This);
            }
        }
        This->dev->primary_playing = TRUE;
    } else {
        This->voice.looping = (flags & DSBPLAY_LOOPING) != 0;
        if (This->desc.dwFlags & DSBCAPS_CTRL3D) {
            /* Play of a 3D buffer commits what is deferred, its own and the listener's, as ALchemy (tests/applyprobe.c
             * cross: a deferred move of either is heard after Stop and Play; Windows keeps it for the commit); not a
             * DirectMusic sink buffer's, Microsoft's, and not on a buffer already playing, where Play only changes
             * the flags.  Its gains, as it is not in the play list yet */
            if (!This->sink && !This->voice.playing) rl_device_commit_3d_buffers(This->dev);
            rl_buffer_update_gains(This);
        }
        if (This->fx && !This->voice.playing) {     /* Play on a playing buffer only changes the flags */
            if (This->desc.dwFlags & DSBCAPS_LOCDEFER) {
                rl_fxchain_place(This->fx, TRUE);
                This->fx_placed = TRUE;
            }
            fx_restart(This);
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
    LOG_TRACE("Buffer[%d]::SetCurrentPosition %lu", This->id, (unsigned long)pos);
    rl_buffer_lock(This);
    if (This->lost) {
        LeaveCriticalSection(&This->dev->lock);
        return DSERR_BUFFERLOST;
    }
    rl_voice_jump(&This->voice, (uint64_t)(pos / This->voice.block) << 32);
    if (This->fx && This->voice.playing) fx_restart(This);    /* stopped: Play makes the fresh start */
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetFormat(IDirectSoundBuffer8 *iface, LPCWAVEFORMATEX wfx)
{
    rl_buffer *This = BUF(iface);
    if (!This->is_primary) return DSERR_INVALIDCALL;
    if (!wfx) return DSERR_INVALIDPARAM;
    rl_device *dev = rl_buffer_lock(This);
    if (writeprimary(This) && wfx->nChannels > 2) {     /* (Windows: at most stereo for the application's own mix) */
        LeaveCriticalSection(&dev->lock);
        return DSERR_INVALIDPARAM;
    }
    /* we always mix in float at the device rate; just remember what the game asked for, and play a
     * DSSCL_WRITEPRIMARY primary buffer in it */
    dev->primary_fmt = *wfx;
    dev->primary_fmt.cbSize = 0;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)wfx;
        dev->primary_fmt.wFormatTag = wfx->cbSize >= 22 && IsEqualGUID(&x->SubFormat, &RL_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) ?
                                      3 /* WAVE_FORMAT_IEEE_FLOAT */ : WAVE_FORMAT_PCM;
    }
    for (rl_buffer *b = dev->all; b; b = b->next_all)
        if (b->is_primary) { prim_format(b); b->mix_qpc = 0; }    /* (its last mix was in the old format) */
    LeaveCriticalSection(&dev->lock);
    LOG_CALL("Primary::SetFormat %lu Hz %u ch %u bits", (unsigned long)wfx->nSamplesPerSec, wfx->nChannels, wfx->wBitsPerSample);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetVolume(IDirectSoundBuffer8 *iface, LONG vol)
{
    rl_buffer *This = BUF(iface);
    /* ALchemy (tests/alprobe.c ctrl): no DSBCAPS_CTRLVOLUME needed (as for pan and frequency), below DSBVOLUME_MIN
     * taken as the minimum.  It takes a volume above 0 too and amplifies by it (+100 dB at 10000): taken as 0 here,
     * fixes or not (the mixer never amplifies; Windows refuses it) */
    if (vol < DSBVOLUME_MIN) vol = DSBVOLUME_MIN;
    if (vol > DSBVOLUME_MAX) vol = DSBVOLUME_MAX;
    LOG_TRACE("Buffer[%d]::SetVolume %ld", This->id, (long)vol);
    rl_buffer_lock(This);
    if (This->is_primary) This->dev->primary_volume = vol;
    else { This->volume = vol; rl_buffer_update_gains(This); }
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetPan(IDirectSoundBuffer8 *iface, LONG pan)
{
    rl_buffer *This = BUF(iface);
    if (pan < DSBPAN_LEFT || pan > DSBPAN_RIGHT) return DSERR_INVALIDPARAM;
    LOG_TRACE("Buffer[%d]::SetPan %ld", This->id, (long)pan);
    rl_buffer_lock(This);
    This->pan = pan;
    if (!This->is_primary) rl_buffer_update_gains(This);
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_SetFrequency(IDirectSoundBuffer8 *iface, DWORD freq)
{
    rl_buffer *This = BUF(iface);
    /* ALchemy: any frequency (0 the original), on the primary buffer too (nothing changes there) */
    if (This->is_primary) return DS_OK;
    LOG_TRACE("Buffer[%d]::SetFrequency %lu", This->id, (unsigned long)freq);
    rl_buffer_lock(This);
    This->freq = freq;
    if (This->sink) rl_buffer_update_gains(This);
    else This->voice.rate = freq ? freq : orig_rate(This);
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

/* (lock held) a secondary buffer stopped: Stop, or lost */
static void stop_locked(rl_buffer *b)
{
    BOOL was = b->voice.playing;
    b->voice.playing = 0;
    if ((b->desc.dwFlags & (DSBCAPS_CTRLFX | DSBCAPS_LOCDEFER)) == (DSBCAPS_CTRLFX | DSBCAPS_LOCDEFER)) {
        b->fx_placed = FALSE;               /* a deferred buffer gives its effects' resources up */
        rl_fxchain_place(b->fx, FALSE);
    }
    if (b->in_play_list) rl_device_stop_voice(b->dev, b);
    if (was) rl_fire_stop(&b->voice, rl_signal_event);
}

static HRESULT WINAPI dsb_Stop(IDirectSoundBuffer8 *iface)
{
    rl_buffer *This = BUF(iface);
    LOG_TRACE("Buffer[%d]::Stop", This->id);
    rl_buffer_lock(This);
    if (This->lost) {                       /* (stopped when it was lost) */
        LeaveCriticalSection(&This->dev->lock);
        return DSERR_BUFFERLOST;
    }
    if (This->is_primary) {
        This->dev->primary_playing = FALSE;
        This->voice.playing = 0;            /* (the DSSCL_WRITEPRIMARY voice: Play goes on from here) */
        if (This->in_play_list) rl_device_stop_voice(This->dev, This);
    } else {
        /* Stop of a playing 3D buffer commits what is deferred, as its Play does (ALchemy, tests/applyprobe.c cross:
         * a deferred move of another buffer is heard after it; not a 2D buffer's, nor a sink buffer's) */
        BOOL commit = (This->desc.dwFlags & DSBCAPS_CTRL3D) && !This->sink && This->voice.playing;
        stop_locked(This);
        if (commit) rl_device_commit_3d_buffers(This->dev);
    }
    LeaveCriticalSection(&This->dev->lock);
    return DS_OK;
}

static HRESULT WINAPI dsb_Unlock(IDirectSoundBuffer8 *iface, LPVOID p1, DWORD b1, LPVOID p2, DWORD b2)
{
    rl_buffer *This = BUF(iface);
    (void)b1; (void)p2; (void)b2;
    /* (the primary buffer at any level, as its Lock: ALchemy) */
    if (!p1) return DSERR_INVALIDPARAM;     /* (ALchemy 0x1001e6e0) */
    /* data is written in place; only the count of open locks is kept (SetFX is refused while one is open) */
    if (InterlockedDecrement(&This->locks) < 0) InterlockedIncrement(&This->locks);
    return DS_OK;
}

/* a lost buffer is found again once no object writes the primary buffer */
static HRESULT WINAPI dsb_Restore(IDirectSoundBuffer8 *iface)
{
    rl_buffer *This = BUF(iface);
    HRESULT hr = DS_OK;
    rl_buffer_lock(This);
    if (This->lost) {
        if (device_writeprimary(This->dev)) hr = DSERR_BUFFERLOST;
        else This->lost = FALSE;
    }
    LeaveCriticalSection(&This->dev->lock);
    return hr;
}

static HRESULT WINAPI dsb_SetFX(IDirectSoundBuffer8 *iface, DWORD count, LPDSEFFECTDESC fx, LPDWORD results)
{
    rl_buffer *This = BUF(iface);
    /* an effects buffer only ALchemy makes: ALchemy's answer, whatever the arguments */
    if ((This->desc.dwFlags & DSBCAPS_CTRLFX) && !This->fx_ok) return DSERR_CONTROLUNAVAIL;
    /* DirectSound's order (tests/fxtest.c): the arguments, the buffer's caps, its state */
    if (count ? !fx : (fx || results)) return DSERR_INVALIDPARAM;
    for (DWORD i = 0; i < count; i++)
        if (fx[i].dwSize != sizeof(DSEFFECTDESC) || fx[i].dwReserved1 || fx[i].dwReserved2 ||
            (fx[i].dwFlags & ~(DSFX_LOCHARDWARE | DSFX_LOCSOFTWARE))) return DSERR_INVALIDPARAM;
    if (This->is_primary || !(This->desc.dwFlags & DSBCAPS_CTRLFX)) return DSERR_CONTROLUNAVAIL;
    rl_buffer_lock(This);
    if (This->voice.playing || This->locks > 0) {
        LeaveCriticalSection(&This->dev->lock);
        LOG_CALL("Buffer[%d]::SetFX(%lu): playing or locked", This->id, (unsigned long)count);
        return DSERR_INVALIDCALL;
    }
    /* the old chain goes first, whatever becomes of the new one */
    rl_fxchain *old = This->fx;
    This->fx = NULL;
    This->voice.data = This->shared->data;
    BYTE *olddata = NULL;               /* (no effects: the processed copy of the buffer goes too, taken under the lock) */
    if (!count) { olddata = This->fxdata; This->fxdata = NULL; }
    LeaveCriticalSection(&This->dev->lock);
    rl_fxchain_free(old);
    HeapFree(GetProcessHeap(), 0, olddata);
    LOG_CALL("Buffer[%d]::SetFX(%lu)", This->id, (unsigned long)count);
    if (!count) return DS_OK;

    HRESULT hr;
    BOOL defer = (This->desc.dwFlags & DSBCAPS_LOCDEFER) != 0;     /* a new chain is not placed yet */
    This->fx_placed = FALSE;
    rl_fxchain *c = rl_fxchain_create(This->shared->wfx ? This->shared->wfx : &This->fmt.Format, count, fx, results,
                                      defer, &hr);
    if (!c) return hr;
    /* the processed copy: made outside the lock, put in place under it (another SetFX may race this one) */
    rl_buffer_lock(This);
    BOOL have = This->fxdata != NULL;
    LeaveCriticalSection(&This->dev->lock);
    BYTE *fresh = have ? NULL : HeapAlloc(GetProcessHeap(), 0, This->voice.size);
    if (!have && !fresh) { rl_fxchain_free(c); return DSERR_OUTOFMEMORY; }
    rl_buffer_lock(This);
    if (!This->fxdata) { This->fxdata = fresh; fresh = NULL; }
    if (!This->fxdata) {                /* (let go by a SetFX(0) meanwhile, and none made here) */
        LeaveCriticalSection(&This->dev->lock);
        rl_fxchain_free(c);
        return DSERR_OUTOFMEMORY;
    }
    memset(This->fxdata, This->voice.sfmt == RL_U8 ? 0x80 : 0, This->voice.size);
    This->fx = c;
    This->voice.data = This->fxdata;
    This->fx_head = This->fx_last = (uint32_t)(This->voice.pos >> 32);
    This->fx_lead = 0;
    LeaveCriticalSection(&This->dev->lock);
    HeapFree(GetProcessHeap(), 0, fresh);   /* (one made here and not needed) */
    return hr;
}

static HRESULT WINAPI dsb_AcquireResources(IDirectSoundBuffer8 *iface, DWORD flags, DWORD count, LPDWORD results)
{
    rl_buffer *This = BUF(iface);
    (void)flags;
    rl_buffer_lock(This);
    HRESULT hr = DS_OK;
    if (!(This->desc.dwFlags & DSBCAPS_LOCDEFER)) {
        /* not a deferred buffer: nothing to place (ALchemy DS_OK, tests/alprobe.c misc; Windows refuses); the
         * effects' status still given when asked for in full */
        if (count && results && count == rl_fxchain_count(This->fx)) rl_fxchain_status(This->fx, results, count);
    } else if (count != rl_fxchain_count(This->fx) || (count && !results)) hr = DSERR_INVALIDPARAM;
    else {
        if (This->fx) { rl_fxchain_place(This->fx, TRUE); This->fx_placed = TRUE; }
        rl_fxchain_status(This->fx, results, count);
    }
    LeaveCriticalSection(&This->dev->lock);
    return hr;
}

static HRESULT WINAPI dsb_GetObjectInPath(IDirectSoundBuffer8 *iface, REFGUID obj, DWORD idx, REFGUID iid, LPVOID *out)
{
    rl_buffer *This = BUF(iface);
    if (!(This->desc.dwFlags & DSBCAPS_CTRLFX) || !This->fx_ok) return DSERR_CONTROLUNAVAIL;
    if (!obj || !iid || !out) return DSERR_INVALIDPARAM;
    rl_buffer_lock(This);
    HRESULT hr = rl_fxchain_get_object(This->fx, obj, idx, iid, out);
    LeaveCriticalSection(&This->dev->lock);
    return hr;
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
{ return buf_addref(NOTIFY(iface), RL_IREF_NOTIFY); }
static ULONG WINAPI ntf_Release(IDirectSoundNotify *iface)
{ return buf_release(NOTIFY(iface), RL_IREF_NOTIFY); }

static HRESULT WINAPI ntf_SetNotificationPositions(IDirectSoundNotify *iface, DWORD count, LPCDSBPOSITIONNOTIFY pos)
{
    rl_buffer *This = NOTIFY(iface);
    if (count && !pos) return DSERR_INVALIDPARAM;
    /* more than DSBNOTIFICATIONS_MAX taken, as ALchemy (tests/alprobe.c notify; Windows refuses them), but none
     * whose size overflows the allocation below */
    if (count > MAXDWORD / sizeof(rl_notify)) return DSERR_INVALIDPARAM;
    /* (offsets past the end: ALchemy takes them, tests/alprobe.c notify; past the end never fires, at the end
     * fires as the end) */

    rl_notify *n = NULL;
    if (count) {
        n = HeapAlloc(GetProcessHeap(), 0, count * sizeof(*n));
        if (!n) return DSERR_OUTOFMEMORY;
        for (DWORD i = 0; i < count; i++) {
            n[i].offset = pos[i].dwOffset == (DWORD)DSBPN_OFFSETSTOP ? RL_NOTIFY_STOP : pos[i].dwOffset;
            n[i].event = pos[i].hEventNotify;
        }
    }
    rl_buffer_lock(This);
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
