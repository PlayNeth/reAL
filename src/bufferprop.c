/* reAL, included by buffer.c: IKsPropertySet, the EAX entry point. */
/* =========================================================== IKsPropertySet (EAX entry point) */

static HRESULT WINAPI ks_QueryInterface(IKsPropertySet *iface, REFIID riid, void **out)
{ return dsb_QueryInterface(&PROPS(iface)->IDirectSoundBuffer8_iface, riid, out); }
static ULONG WINAPI ks_AddRef(IKsPropertySet *iface)
{ return buf_addref(PROPS(iface), RL_IREF_KS); }
static ULONG WINAPI ks_Release(IKsPropertySet *iface)
{ return buf_release(PROPS(iface), RL_IREF_KS); }

/* EAX 1.0 property sets (what ALchemy answers, see dstrace of NFS4) */
static const GUID RL_EAX1_LISTENER =
    { 0x4a4e6fc1, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID RL_EAX1_BUFFER =
    { 0x4a4e6fc0, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

/* EAX 2.0 and 3.0 property sets */
static const GUID RL_EAX2_LISTENER =
    { 0x0306a6a8, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } };
static const GUID RL_EAX2_BUFFER =
    { 0x0306a6a7, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } };
static const GUID RL_EAX3_LISTENER =
    { 0xa8fa6882, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } };
static const GUID RL_EAX3_BUFFER =
    { 0xa8fa6881, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } };

static const GUID RL_EAX4_CONTEXT =
    { 0x1d4870ad, 0x0def, 0x43c0, { 0xa4, 0x0c, 0x52, 0x36, 0x32, 0x29, 0x63, 0x42 } };
static const GUID RL_EAX4_SOURCE =
    { 0x1b86b823, 0x22df, 0x4eae, { 0x8b, 0x3c, 0x12, 0x78, 0xce, 0x54, 0x42, 0x27 } };

/* which EAX 2/3 set a GUID is: 2/3 listener -> 2/3, buffer -> 12/13, else 0 */
static int eax23_set(REFGUID set)
{
    if (IsEqualGUID(set, &RL_EAX2_LISTENER)) return 2;
    if (IsEqualGUID(set, &RL_EAX3_LISTENER)) return 3;
    if (IsEqualGUID(set, &RL_EAX2_BUFFER)) return 12;
    if (IsEqualGUID(set, &RL_EAX3_BUFFER)) return 13;
    return 0;
}

/* EAX 4.0 property sets: context 40, FX slots 41..44, source 45 */
static int eax4_which(REFGUID set)
{
    eax_guid g; memcpy(&g, set, 16);
    if (!memcmp(set, &RL_EAX4_CONTEXT, 16)) return 40;
    if (!memcmp(set, &RL_EAX4_SOURCE, 16)) return 45;
    int k = eax4_slot_index(&g);
    return k >= 0 ? 41 + k : 0;
}

static HRESULT eax_hr(int r)
{
    /* (ALchemy: E_INVALIDARG for any property it refuses, an unknown id too: tests/dsfuzz.c against it) */
    return r == EAX_OK ? DS_OK : DSERR_INVALIDPARAM;
}

/* property names for the EAX log; a leading '#' marks an integer (mB / enum / flags) property */
static const char *const k_eax2l_names[] = { "none", "ALL", "#room", "#roomHF", "roomRolloff", "decayTime",
    "decayHFRatio", "#reflections", "reflectionsDelay", "#reverb", "reverbDelay", "#environment", "environmentSize",
    "environmentDiffusion", "airAbsorptionHF", "#flags" };
static const char *const k_eax3l_names[] = { "none", "ALL", "#environment", "environmentSize",
    "environmentDiffusion", "#room", "#roomHF", "#roomLF", "decayTime", "decayHFRatio", "decayLFRatio",
    "#reflections", "reflectionsDelay", "reflectionsPan", "#reverb", "reverbDelay", "reverbPan", "echoTime",
    "echoDepth", "modulationTime", "modulationDepth", "airAbsorptionHF", "HFReference", "LFReference",
    "roomRolloff", "#flags" };
static const char *const k_eax2b_names[] = { "none", "ALL", "#direct", "#directHF", "#room", "#roomHF",
    "roomRolloff", "#obstruction", "obstructionLF", "#occlusion", "occlusionLF", "occlusionRoom",
    "#outsideVolumeHF", "airAbsorption", "#flags" };
static const char *const k_eax3b_names[] = { "none", "ALL", "obstructionParams", "occlusionParams",
    "exclusionParams", "#direct", "#directHF", "#room", "#roomHF", "#obstruction", "obstructionLF", "#occlusion",
    "occlusionLF", "occlusionRoom", "occlusionDirect", "#exclusion", "exclusionLF", "#outsideVolumeHF",
    "doppler", "rolloff", "roomRolloff", "airAbsorption", "#flags" };

static void log_eax23(rl_buffer *b, const char *op, int which, ULONG id, const void *data, ULONG len, HRESULT hr)
{
    if (!g_cfg.log_eax) return;
    static const char *names[] = { [2] = "EAX2 listener", [3] = "EAX3 listener", [12] = "EAX2 buffer", [13] = "EAX3 buffer" };
    const char *const *tab = which == 2 ? k_eax2l_names : which == 3 ? k_eax3l_names : which == 12 ? k_eax2b_names : k_eax3b_names;
    const ULONG ntab = which == 2 ? EAX2L_COUNT : which == 3 ? EAX3L_COUNT : which == 12 ? EAX2B_COUNT : EAX3B_COUNT;
    const ULONG pid = id & ~EAX_DEFERRED;
    const char *pname = pid < ntab ? tab[pid] : "?";
    const int isint = pname[0] == '#';
    if (isint) pname++;
    char v[96] = "";
    if (data && len == 4) {
        int32_t i; float f;
        memcpy(&i, data, 4); memcpy(&f, data, 4);
        if (isint) snprintf(v, sizeof(v), " = %ld", (long)i);
        else snprintf(v, sizeof(v), " = %g", f);
    } else if (data && len == 12 && !isint) {
        float f[3]; memcpy(f, data, 12);
        snprintf(v, sizeof(v), " = (%g, %g, %g)", f[0], f[1], f[2]);
    } else if (data && len == 8 && !isint) {
        int32_t i; float f;
        memcpy(&i, data, 4); memcpy(&f, (const char *)data + 4, 4);
        snprintf(v, sizeof(v), " = (%ld, %g)", (long)i, f);
    }
    rl_log(0, "Buffer[%d]::KsProp %s %s %s (id %lu)%s len %lu%s -> %08lx", b->id, op, names[which], pname,
           (unsigned long)pid, (id & EAX_DEFERRED) ? " (deferred)" : "", (unsigned long)len, v, (unsigned long)hr);
}

static const char *const k_eax4c_names[] = { "none", "ALL", "primaryFXSlot", "distanceFactor", "airAbsorptionHF",
    "HFReference", "lastError" };
static const char *const k_eax4fxs_names[] = { "none", "ALL", "loadEffect", "#volume", "#lock", "#flags" };
static const char *const k_eax4s_names[] = { "none", "ALL", "obstructionParams", "occlusionParams",
    "exclusionParams", "#direct", "#directHF", "#room", "#roomHF", "#obstruction", "obstructionLF", "#occlusion",
    "occlusionLF", "occlusionRoomRatio", "occlusionDirectRatio", "#exclusion", "exclusionLF", "#outsideVolumeHF",
    "doppler", "rolloff", "roomRolloff", "airAbsorption", "#flags", "sendParams", "allSendParams",
    "occlusionSendParams", "exclusionSendParams", "activeFXSlots" };

static void log_eax4(rl_buffer *b, const char *op, int which, ULONG id, const void *data, ULONG len, HRESULT hr)
{
    if (!g_cfg.log_eax) return;
    const ULONG pid = id & ~EAX_DEFERRED;
    char where[32];
    const char *pname = "?";
    if (which == 40) { strcpy(where, "EAX4 context"); if (pid < EAX4C_COUNT) pname = k_eax4c_names[pid]; }
    else if (which == 45) { strcpy(where, "EAX4 source"); if (pid < EAX4S_COUNT) pname = k_eax4s_names[pid]; }
    else {
        snprintf(where, sizeof(where), "EAX4 FX slot %d", which - 41);
        const eax4_slot *s = &b->dev->eax4s[which - 41];
        if (pid >= 0x10000 && pid <= 0x10005) pname = k_eax4fxs_names[pid - 0x10000];
        else if (s->type == EAX4FX_REVERB && pid < EAX3L_COUNT) pname = k_eax3l_names[pid];
        else pname = "effect";
    }
    const int isint = pname[0] == '#';
    if (isint) pname++;
    char v[96] = "";
    if (data && len == 4) {
        int32_t i; float f;
        memcpy(&i, data, 4); memcpy(&f, data, 4);
        if (isint) snprintf(v, sizeof(v), " = %ld", (long)i);
        else snprintf(v, sizeof(v), " = %g", f);
    }
    rl_log(0, "Buffer[%d]::KsProp %s %s %s (id %#lx)%s len %lu%s -> %08lx", b->id, op, where, pname,
           (unsigned long)pid, (id & EAX_DEFERRED) ? " (deferred)" : "", (unsigned long)len, v, (unsigned long)hr);
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
    /* (ALchemy: an unknown set E_INVALIDARG; *ret written only when the call succeeds) */
    HRESULT hr = DSERR_INVALIDPARAM;

    int w4 = eax4_which(set);
    if (w4) {
        if (w4 == 45 && !(This->desc.dwFlags & DSBCAPS_CTRL3D)) return DSERR_INVALIDPARAM;
        uint32_t got = 0;
        int r;
        EnterCriticalSection(&dev->lock);
        rl_device_eax_use(dev, 4);
        if (w4 == 40) r = eax4c_get(&dev->eax4c, id, data, data_len, &got);
        else if (w4 == 45) r = eax4s_get(&This->eax4, &dev->eax4c, id, data, data_len, &got);
        else r = eax4fx_get(&dev->eax4s[w4 - 41], &dev->eax4c, id, data, data_len, &got);
        LeaveCriticalSection(&dev->lock);
        hr = eax_hr(r);
        if (ret && hr == DS_OK) *ret = got;
        log_eax4(This, "Get", w4, id, r == EAX_OK ? data : NULL, got, hr);
        return hr;
    }

    int which = eax23_set(set);
    if (which) {
        if (which >= 12 && !(This->desc.dwFlags & DSBCAPS_CTRL3D)) return DSERR_INVALIDPARAM;
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
        hr = eax_hr(r);
        if (ret && hr == DS_OK) *ret = got;
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
    HRESULT hr = DSERR_INVALIDPARAM;                /* (an unknown set: ALchemy's E_INVALIDARG) */

    int w4 = eax4_which(set);
    if (w4) {
        if (w4 == 45 && !(This->desc.dwFlags & DSBCAPS_CTRL3D)) return DSERR_INVALIDPARAM;
        int r;
        EnterCriticalSection(&dev->lock);
        rl_device_eax_use(dev, 4);
        if (w4 == 40) r = eax4c_set(&dev->eax4c, id, data, data_len);
        else if (w4 == 45) r = eax4s_set(&This->eax4, &dev->eax4c, id, data, data_len);
        else r = eax4fx_set(&dev->eax4s[w4 - 41], &dev->eax4c, id, data, data_len);
        /* ALchemy stores every value at once; an immediate set commits: the mixer recomputes all sources
         * (and the slots' effects), so a batch of deferred sets ends with one commit */
        if (r == EAX_OK && !(id & EAX_DEFERRED)) {
            if (w4 != 45) rl_device_eax_listener_changed(dev);
            dev->eax_commit = TRUE;
        }
        LeaveCriticalSection(&dev->lock);
        hr = eax_hr(r);
        log_eax4(This, "Set", w4, id, data, data_len, hr);
        return hr;
    }

    int which = eax23_set(set);
    if (which) {
        if (which >= 12 && !(This->desc.dwFlags & DSBCAPS_CTRL3D)) return DSERR_INVALIDPARAM;
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
                if (is_nan(r->volume) || is_nan(r->decay) || is_nan(r->damping)) {
                    hr = DSERR_INVALIDPARAM; break;   /* ALchemy rejects NaNs (this test used to fold away) */
                }
                dev->eax1_env = r->environment > 25 ? 25 : r->environment;
                dev->eax1_volume = clampf(r->volume, 0.0f, 1.0f);    /* (clamped as each value alone) */
                dev->eax1_decay = clampf(r->decay, 0.1f, 20.0f);
                dev->eax1_damping = clampf(r->damping, 0.0f, 2.0f);
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
            /* each value: a NaN refused, the rest clamped to its range (ALchemy: tests/alprobe.c eaxval) */
            case EAX1_L_VOLUME: case EAX1_L_DECAYTIME: case EAX1_L_DAMPING: {
                float v = *(const float *)data;
                if (is_nan(v)) { hr = DSERR_INVALIDPARAM; break; }
                if (id == EAX1_L_VOLUME) dev->eax1_volume = clampf(v, 0.0f, 1.0f);
                else if (id == EAX1_L_DECAYTIME) dev->eax1_decay = clampf(v, 0.1f, 20.0f);
                else dev->eax1_damping = clampf(v, 0.0f, 2.0f);
                break;
            }
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
            /* -1 (by distance) or 0..1; more is taken as 1 (ALchemy: tests/alprobe.c eaxval), a NaN refused */
            float mix = *(const float *)data;
            if (is_nan(mix) || (mix != -1.0f && mix < 0.0f)) hr = DSERR_INVALIDPARAM;
            else { This->eax1_mix = mix > 1.0f ? 1.0f : mix; rl_buffer_update_gains(This); hr = DS_OK; }
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

/* EAX 5.0 property sets (GUIDs as in ALchemy's dsound.dll): not implemented yet, but named in the log so
 * a game asking for them is easy to spot */
static const struct { GUID g; const char *name; } k_eax5_sets[] = {
    { { 0x57e13437, 0xb932, 0x4ab2, { 0xb8, 0xbd, 0x52, 0x66, 0xc1, 0xa8, 0x87, 0xee } }, "EAX 5.0 context" },
    { { 0x91f9590f, 0xc388, 0x407a, { 0x84, 0xb0, 0x1b, 0xae, 0x0e, 0xf7, 0x1a, 0xbc } }, "EAX 5.0 FX slot 0" },
    { { 0x5edf82f0, 0x24a7, 0x4f38, { 0x8e, 0x64, 0x2f, 0x09, 0xca, 0x05, 0xde, 0xe1 } }, "EAX 5.0 source" },
};

static const char *eax_set_name(REFGUID set)
{
    static const char *n23[] = { [2] = "EAX 2.0 listener", [3] = "EAX 3.0 listener", [12] = "EAX 2.0 buffer", [13] = "EAX 3.0 buffer" };
    static const char *n4[] = { "EAX 4.0 context", "EAX 4.0 FX slot 0", "EAX 4.0 FX slot 1", "EAX 4.0 FX slot 2",
                                "EAX 4.0 FX slot 3", "EAX 4.0 source" };
    if (IsEqualGUID(set, &RL_EAX1_LISTENER)) return "EAX 1.0 listener";
    if (IsEqualGUID(set, &RL_EAX1_BUFFER)) return "EAX 1.0 buffer";
    int w = eax23_set(set);
    if (w) return n23[w];
    w = eax4_which(set);
    if (w) return n4[w - 40];
    for (size_t i = 0; i < sizeof(k_eax5_sets) / sizeof(k_eax5_sets[0]); i++)
        if (IsEqualGUID(set, &k_eax5_sets[i].g)) {
            static LONG said;
            if (!InterlockedExchange(&said, 1))
                LOG_INFO("game asked for EAX 5.0 (not supported yet; it should fall back to EAX 4.0)");
            return k_eax5_sets[i].name;
        }
    return NULL;
}

static HRESULT WINAPI ks_QuerySupport(IKsPropertySet *iface, REFGUID set, ULONG id, PULONG support)
{
    rl_buffer *This = PROPS(iface);
    char gb[64];
    /* ALchemy (tests/alprobe.c eax, tests/dsfuzz.c against it), on any buffer whatever its flags: a known set's
     * property GET | SET; an id the set doesn't have E_NOTIMPL, *support untouched; a set it doesn't know (the voice
     * manager's too) S_OK with no support.  The deferred flag is taken off the id for EAX 2 to 4, not for EAX 1. */
    ULONG pid = id & ~EAX_DEFERRED;
    int w4 = eax4_which(set), which = eax23_set(set), known = 1;
    BOOL ok;
    if (IsEqualGUID(set, &RL_EAX1_LISTENER)) ok = id < EAX1_L_COUNT;
    else if (IsEqualGUID(set, &RL_EAX1_BUFFER)) ok = id < EAX1_B_COUNT;
    else if (w4 == 40) ok = pid < EAX4C_COUNT;
    else if (w4 == 45) ok = pid < EAX4S_COUNT;
    else if (w4) ok = pid < EAX3L_COUNT || (pid >= EAX4FXS_NONE && pid <= EAX4FXS_FLAGS);   /* (a slot: its effect's ids) */
    else if (which) {
        static const ULONG count[] = { [2] = EAX2L_COUNT, [3] = EAX3L_COUNT, [12] = EAX2B_COUNT, [13] = EAX3B_COUNT };
        ok = pid < count[which];
    } else { known = 0; ok = 0; }
    HRESULT hr = !known ? DS_OK : ok ? DS_OK : E_NOTIMPL;
    ULONG s = known && ok ? KSPROPERTY_SUPPORT_GET | KSPROPERTY_SUPPORT_SET : 0;
    if (known && !ok) {
        LOG_EAX("Buffer[%d]::KsProp QuerySupport %s id %lu -> %08lx", This->id, rl_guid_str(set, gb), (unsigned long)id,
                (unsigned long)hr);
        return hr;
    }
    if (support) *support = s;
    const char *sn = eax_set_name(set);
    LOG_EAX("Buffer[%d]::KsProp QuerySupport %s%s%s id %lu -> %08lx support %lu", This->id, rl_guid_str(set, gb),
            sn ? " = " : "", sn ? sn : "", (unsigned long)id, (unsigned long)hr, (unsigned long)s);
    return hr;
}

static IKsPropertySetVtbl ks_vtbl = {
    ks_QueryInterface, ks_AddRef, ks_Release, ks_Get, ks_Set, ks_QuerySupport,
};

/* the vtables, for refused.c's wrappers */
void **rl_vtable(int which)
{
    switch (which) {
    case RL_VT_DS:  return rl_dsound_vtable();
    case RL_VT_DSB: return (void **)&dsb_vtbl;
    case RL_VT_NTF: return (void **)&ntf_vtbl;
    case RL_VT_B3D: return (void **)&b3d_vtbl;
    case RL_VT_LST: return (void **)&lst_vtbl;
    case RL_VT_KS:  return (void **)&ks_vtbl;
    }
    return NULL;
}
