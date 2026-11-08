/* reAL, included by buffer.c: IDirectSound3DBuffer and the primary buffer's IDirectSound3DListener. */
/* =========================================================== IDirectSound3DBuffer */

static HRESULT WINAPI b3d_QueryInterface(IDirectSound3DBuffer *iface, REFIID riid, void **out)
{ return dsb_QueryInterface(&BUF3D(iface)->IDirectSoundBuffer8_iface, riid, out); }
static ULONG WINAPI b3d_AddRef(IDirectSound3DBuffer *iface)
{ return buf_addref(BUF3D(iface), RL_IREF_3D); }
static ULONG WINAPI b3d_Release(IDirectSound3DBuffer *iface)
{ return buf_release(BUF3D(iface), RL_IREF_3D); }

static rl_vec3 to_v(D3DVALUE x, D3DVALUE y, D3DVALUE z) { rl_vec3 v = { x, y, z }; return v; }

/* apply: DS3D_IMMEDIATE (0), or anything else deferred (ALchemy's: alprobe's 2, 3 and ffffffff defer) */
static void b3d_changed(rl_buffer *This, DWORD apply)
{
    if (apply != DS3D_IMMEDIATE) {
        This->src3d_dirty = TRUE;
    } else {
        This->src3d = This->src3d_pending;
        This->src3d_dirty = FALSE;
        rl_buffer_update_gains(This);
    }
}

#define B3D_SET_BEGIN(iface) rl_buffer *This = BUF3D(iface); rl_buffer_lock(This)
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
    if (!p || p->dwSize != sizeof(*p)) return DSERR_INVALIDPARAM;     /* (exactly: ALchemy and Windows) */
    /* ALchemy (tests/dsfuzz.c against it) takes the call whatever the values: each one as its own method would take
     * it, a value that method refuses left as it was (so none can make the 3D math NaN) */
    B3D_SET_BEGIN(iface);
    rl_source3d *s = &This->src3d_pending;
    if (finite3(p->vPosition.x, p->vPosition.y, p->vPosition.z)) s->pos = to_v(p->vPosition.x, p->vPosition.y, p->vPosition.z);
    if (finite3(p->vVelocity.x, p->vVelocity.y, p->vVelocity.z)) s->vel = to_v(p->vVelocity.x, p->vVelocity.y, p->vVelocity.z);
    if (p->dwInsideConeAngle <= 360 && p->dwOutsideConeAngle <= 360) {
        s->cone_inside = p->dwInsideConeAngle;
        s->cone_outside = p->dwOutsideConeAngle;
    }
    if (direction_ok(p->vConeOrientation.x, p->vConeOrientation.y, p->vConeOrientation.z))
        s->cone_orient = to_v(p->vConeOrientation.x, p->vConeOrientation.y, p->vConeOrientation.z);
    if (p->lConeOutsideVolume >= DSBVOLUME_MIN && p->lConeOutsideVolume <= DSBVOLUME_MAX)
        s->cone_outside_volume = p->lConeOutsideVolume;
    if (distance_ok(p->flMinDistance) && p->flMinDistance != 0.0f) s->min_dist = p->flMinDistance;
    if (distance_ok(p->flMaxDistance)) s->max_dist = p->flMaxDistance;
    if (p->dwMode <= DS3DMODE_DISABLE) s->mode = p->dwMode;
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
    if (!direction_ok(x, y, z)) return DSERR_INVALIDPARAM;
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
    if (!distance_ok(d)) return DSERR_INVALIDPARAM;
    B3D_SET_BEGIN(iface);
    This->src3d_pending.max_dist = d;
    LOG_3D("Buffer[%d]::SetMaxDistance %.3f", This->id, d);
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetMinDistance(IDirectSound3DBuffer *iface, D3DVALUE d, DWORD apply)
{
    /* (a minimum of 0: ALchemy refuses it too, tests/alprobe.c d3; a maximum of 0 only Windows and the fixes) */
    if (!distance_ok(d) || d == 0.0f) return DSERR_INVALIDPARAM;
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
    if (!finite3(x, y, z)) return DSERR_INVALIDPARAM;     /* (ALchemy 0x10042800) */
    B3D_SET_BEGIN(iface);
    This->src3d_pending.pos = to_v(x, y, z);
    LOG_3D("Buffer[%d]::SetPosition %.3f,%.3f,%.3f (%s)", This->id, x, y, z, apply ? "deferred" : "immediate");
    B3D_SET_END(apply);
}

static HRESULT WINAPI b3d_SetVelocity(IDirectSound3DBuffer *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    if (!finite3(x, y, z)) return DSERR_INVALIDPARAM;
    B3D_SET_BEGIN(iface);
    This->src3d_pending.vel = to_v(x, y, z);
    LOG_3D("Buffer[%d]::SetVelocity %.3f,%.3f,%.3f (%s)", This->id, x, y, z, apply ? "deferred" : "immediate");
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
{ return buf_addref(LISTENER(iface), RL_IREF_LISTENER); }
static ULONG WINAPI lst_Release(IDirectSound3DListener *iface)
{ return buf_release(LISTENER(iface), RL_IREF_LISTENER); }

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

#define LST_SET_BEGIN(iface) rl_device *dev = rl_buffer_lock(LISTENER(iface)); rl_listener3d *l = &dev->listener_pending
#define LST_SET_END(apply)   lst_changed(dev, apply); LeaveCriticalSection(&dev->lock); return DS_OK

static HRESULT WINAPI lst_SetAllParameters(IDirectSound3DListener *iface, LPCDS3DLISTENER p, DWORD apply)
{
    if (!p || p->dwSize < sizeof(*p)) return DSERR_INVALIDPARAM;
    /* ALchemy (tests/dsfuzz.c against it) takes the call whatever the values: each one as its own method would take
     * it, a value that method refuses left as it was (so none can make the 3D math NaN) */
    LST_SET_BEGIN(iface);
    if (finite3(p->vPosition.x, p->vPosition.y, p->vPosition.z)) l->pos = to_v(p->vPosition.x, p->vPosition.y, p->vPosition.z);
    if (finite3(p->vVelocity.x, p->vVelocity.y, p->vVelocity.z)) l->vel = to_v(p->vVelocity.x, p->vVelocity.y, p->vVelocity.z);
    if (direction_ok(p->vOrientFront.x, p->vOrientFront.y, p->vOrientFront.z) &&
        direction_ok(p->vOrientTop.x, p->vOrientTop.y, p->vOrientTop.z)) {
        l->front = to_v(p->vOrientFront.x, p->vOrientFront.y, p->vOrientFront.z);
        l->top = to_v(p->vOrientTop.x, p->vOrientTop.y, p->vOrientTop.z);
    }
    if (is_finite(p->flDistanceFactor) && p->flDistanceFactor >= DS3D_MINDISTANCEFACTOR && p->flDistanceFactor <= DS3D_MAXDISTANCEFACTOR)
        l->distance_factor = p->flDistanceFactor;
    if (is_finite(p->flRolloffFactor) && p->flRolloffFactor >= DS3D_MINROLLOFFFACTOR && p->flRolloffFactor <= DS3D_MAXROLLOFFFACTOR)
        l->rolloff_factor = p->flRolloffFactor;
    if (is_finite(p->flDopplerFactor) && p->flDopplerFactor >= DS3D_MINDOPPLERFACTOR && p->flDopplerFactor <= DS3D_MAXDOPPLERFACTOR)
        l->doppler_factor = p->flDopplerFactor;
    LOG_3D("Listener::SetAllParameters pos %.3f,%.3f,%.3f vel %.3f,%.3f,%.3f distance %g rolloff %g doppler %g (%s)",
           l->pos.x, l->pos.y, l->pos.z, l->vel.x, l->vel.y, l->vel.z, l->distance_factor, l->rolloff_factor,
           l->doppler_factor, apply ? "deferred" : "immediate");
    LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetDistanceFactor(IDirectSound3DListener *iface, D3DVALUE v, DWORD apply)
{
    if (!is_finite(v) || v < DS3D_MINDISTANCEFACTOR || v > DS3D_MAXDISTANCEFACTOR) return DSERR_INVALIDPARAM;
    LOG_3D("Listener::SetDistanceFactor %g (%s)", v, apply ? "deferred" : "immediate");
    LST_SET_BEGIN(iface); l->distance_factor = v; LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetDopplerFactor(IDirectSound3DListener *iface, D3DVALUE v, DWORD apply)
{
    if (!is_finite(v) || v < DS3D_MINDOPPLERFACTOR || v > DS3D_MAXDOPPLERFACTOR) return DSERR_INVALIDPARAM;
    LOG_3D("Listener::SetDopplerFactor %g (%s)", v, apply ? "deferred" : "immediate");
    LST_SET_BEGIN(iface); l->doppler_factor = v; LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetOrientation(IDirectSound3DListener *iface, D3DVALUE fx, D3DVALUE fy, D3DVALUE fz,
                                         D3DVALUE tx, D3DVALUE ty, D3DVALUE tz, DWORD apply)
{
    /* ALchemy (0x10043740): every component finite, and neither vector all zero, else DSERR_INVALIDPARAM and the
     * orientation kept (Fallout 3 sets 0,0,0 / 0,0,0 until the player has one; taken, it left the 3D math NaN) */
    if (!direction_ok(fx, fy, fz) || !direction_ok(tx, ty, tz)) {
        LOG_3D("Listener::SetOrientation front %.3f,%.3f,%.3f top %.3f,%.3f,%.3f: a zero or non-finite vector, refused", fx, fy, fz, tx, ty, tz);
        return DSERR_INVALIDPARAM;
    }
    LST_SET_BEGIN(iface);
    l->front = to_v(fx, fy, fz);
    l->top = to_v(tx, ty, tz);
    LOG_3D("Listener::SetOrientation front %.3f,%.3f,%.3f top %.3f,%.3f,%.3f (%s)", fx, fy, fz, tx, ty, tz,
           apply ? "deferred" : "immediate");
    LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetPosition(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    if (!finite3(x, y, z)) return DSERR_INVALIDPARAM;     /* (ALchemy 0x100435e0) */
    LST_SET_BEGIN(iface);
    l->pos = to_v(x, y, z);
    LOG_3D("Listener::SetPosition %.3f,%.3f,%.3f (%s)", x, y, z, apply ? "deferred" : "immediate");
    LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetRolloffFactor(IDirectSound3DListener *iface, D3DVALUE v, DWORD apply)
{
    if (!is_finite(v) || v < DS3D_MINROLLOFFFACTOR || v > DS3D_MAXROLLOFFFACTOR) return DSERR_INVALIDPARAM;
    LOG_3D("Listener::SetRolloffFactor %g (%s)", v, apply ? "deferred" : "immediate");
    LST_SET_BEGIN(iface); l->rolloff_factor = v; LST_SET_END(apply);
}

static HRESULT WINAPI lst_SetVelocity(IDirectSound3DListener *iface, D3DVALUE x, D3DVALUE y, D3DVALUE z, DWORD apply)
{
    if (!finite3(x, y, z)) return DSERR_INVALIDPARAM;     /* (ALchemy 0x10043690) */
    LOG_3D("Listener::SetVelocity %.3f,%.3f,%.3f (%s)", x, y, z, apply ? "deferred" : "immediate");
    LST_SET_BEGIN(iface); l->vel = to_v(x, y, z); LST_SET_END(apply);
}

static HRESULT WINAPI lst_CommitDeferredSettings(IDirectSound3DListener *iface)
{
    LOG_3D("Listener::CommitDeferredSettings");
    rl_device *dev = rl_buffer_lock(LISTENER(iface));
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
