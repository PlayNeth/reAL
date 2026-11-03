/* reDM, included by dmcom.c: IDirectMusicPatternTrack (774019c8). */
/* ------------------------------------------------------------------ IDirectMusicPatternTrack (774019c8) */
static rm_track *ptk(IDirectMusicPatternTrack *i) { return impl_from(i, rm_track, IDirectMusicPatternTrack_iface); }
static HRESULT WINAPI ptk_qi(IDirectMusicPatternTrack *i, REFIID r, void **o) { return trk_qi(&ptk(i)->IDirectMusicTrack8_iface, r, o); }
static ULONG WINAPI ptk_addref(IDirectMusicPatternTrack *i) { return trk_addref(&ptk(i)->IDirectMusicTrack8_iface); }
static ULONG WINAPI ptk_release(IDirectMusicPatternTrack *i) { return trk_release(&ptk(i)->IDirectMusicTrack8_iface); }
static BOOL bad_iface(void *p) { return IsBadReadPtr(p, 4) || IsBadReadPtr(*(void **)p, 4) || IsBadCodePtr(**(FARPROC **)p); }
/* 77415094 */
static HRESULT WINAPI ptk_createsegment(IDirectMusicPatternTrack *i, IDirectMusicStyle *style, IDirectMusicSegment **out)
{
    if (bad_iface(style) || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    struct dms_style *st = style_hold_of(style);
    if (!st) return E_NOINTERFACE;      /* (dmstyle's private style interface) */
    dm_engine_lock();
    dm_segment *g = NULL;
    HRESULT hr = dms_patterntrack_create_segment(ptk(i)->t, st, &g);
    dm_engine_unlock();
    dms_style_release(st);
    if (FAILED(hr)) return hr;
    rm_segment *n = new_segment(g);
    if (!n) { dm_segment_release(g); return E_OUTOFMEMORY; }
    *out = (IDirectMusicSegment *)&n->IDirectMusicSegment8_iface;
    return S_OK;
}
/* 774143af */
static HRESULT WINAPI ptk_setvariation(IDirectMusicPatternTrack *i, IDirectMusicSegmentState *ss, DWORD flags, DWORD part)
{
    if (ss && bad_iface(ss)) return E_POINTER;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    dm_engine_lock();
    HRESULT hr = dms_patterntrack_set_variation(ptk(i)->t, st ? st->ss : NULL, flags, part);
    dm_engine_unlock();
    return hr;
}
/* 77413c30: the style's pattern (0) or motif (1) of that name (DMUS_E_NOT_FOUND without one) */
static HRESULT WINAPI ptk_setpatternbyname(IDirectMusicPatternTrack *i, IDirectMusicSegmentState *ss, WCHAR *name,
                                           IDirectMusicStyle *style, DWORD type, DWORD *length)
{
    if ((ss && bad_iface(ss)) || IsBadReadPtr(name, 4) || bad_iface(style) || IsBadWritePtr(length, 4)) return E_POINTER;
    struct dms_style *sty = style_hold_of(style);
    if (!sty) return E_NOINTERFACE;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    dm_engine_lock();
    const dms_pattern *p = NULL;
    dms_pattern **list = type == 0 ? sty->patterns : type == 1 ? sty->motifs : NULL;
    int n = type == 0 ? sty->npatterns : type == 1 ? sty->nmotifs : 0;
    for (int k = 0; k < n && !p; k++) if (!_wcsicmp((const WCHAR *)list[k]->name, name)) p = list[k];     /* 7740ac84 */
    HRESULT hr = (int32_t)0x88781161;
    if (p) {
        mtime_t len = 0;
        hr = dms_patterntrack_set_pattern(ptk(i)->t, st ? st->ss : NULL, p, &len);
        if (SUCCEEDED(hr) || hr != (int32_t)0x88781162) *length = (DWORD)len;
    }
    dm_engine_unlock();
    dms_style_release(sty);
    return hr;
}
static IDirectMusicPatternTrackVtbl ptk_vtbl = { ptk_qi, ptk_addref, ptk_release, ptk_createsegment, ptk_setvariation, ptk_setpatternbyname };
static dm_band *band_hold_of(void *p)        /* (ours only: no call into the object) */
{
    if (!p || IsBadReadPtr(p, sizeof(void *))) return NULL;
    void *vt = *(void **)p;
    rm_band *b = vt == &bnd_vtbl ? bnd(p) : vt == &bno_vtbl ? bno(p) : vt == &bnp_vtbl ? bnp(p) : NULL;
    return b ? band_hold(b) : NULL;
}
static void prm_out(int prm, int hr, void *p)
{
    if ((prm == PRM_STYLE || prm == PRM_PATSTYLE) && hr >= 0 && p) *(void **)p = style_iface(*(dms_style **)p);
    if (prm == PRM_CHORDMAP && hr >= 0 && p) *(void **)p = chordmap_iface(*(dmc_chordmap **)p);
    if (prm == PRM_BANDPARAM && hr >= 0 && p) {         /* the band copy as a band object (its own reference) */
        DMUS_BAND_PARAM *bp = p;
        dm_band *b = (dm_band *)bp->pBand;
        rm_band *o = b ? new_band(b) : NULL;
        if (b) dm_band_release(b);
        bp->pBand = o ? &o->IDirectMusicBand_iface : NULL;
    }
}

/* a wave track item's reference (773248af: the loader's GetObject of a DirectSoundWave by guid, file or name) */
static dm_wave *loader_getwave(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    IDirectMusicObject *o = NULL;
    if (FAILED(ldr_get(ctx, &RM_CLSID_DirectSoundWave, &IID_IDirectMusicObject, g, file, name, (void **)&o)) || !o) {
        LOG_INFO("music: wave track: wave %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    rm_wave *w = o->lpVtbl == &wvo_vtbl ? wvo(o) : NULL;
    dm_engine_lock();                   /* (a Load on another thread may replace it) */
    dm_wave *r = w ? w->w : NULL;
    dm_wave_addref(r);
    dm_engine_unlock();
    IDirectMusicObject_Release(o);
    return r;
}

/* a segment trigger track's reference (77323220: the loader's GetObject of a segment); the engine segment with a
 * reference */
static dm_segment *loader_getseg(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    IDirectMusicSegment8 *o = NULL;
    if (FAILED(ldr_get(ctx, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, g, file, name, (void **)&o)) || !o) {
        LOG_INFO("music: segment trigger track: segment %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    rm_segment *r = o->lpVtbl == &seg_vtbl ? sgm(o) : NULL;
    dm_engine_lock();                       /* (the engine thread counts it too; a Load may replace it) */
    dm_segment *x = r ? r->seg : NULL;
    if (x) x->refs++;
    dm_engine_unlock();
    IDirectMusicSegment8_Release(o);
    return x;
}

/* a script track's reference (0040f6fd: the loader's GetObject of a script; its failure is the track's) */
static void *loader_getscript(void *ctx, const dm_guid *g, const char *file, const char *name, int32_t *hr)
{
    IDirectMusicScript *o = NULL;
    *hr = ldr_get(ctx, &CLSID_DirectMusicScript, &IID_IDirectMusicScript, g, file, name, (void **)&o);
    if (FAILED(*hr) || !o) {
        LOG_INFO("music: script track: script %s not found", file ? file : name ? name : "(by guid)");
        if (o) IDirectMusicScript_Release(o);
        if (SUCCEEDED(*hr)) *hr = E_FAIL;
        return NULL;
    }
    return o;
}
static void resort_hook(int enter)
{
    if (enter) dm_engine_lock();
    else dm_engine_unlock();
}
/* (a script track's scripts: let go of once the performance's lock is, as a script's last release can call the loader) */
static void unk_ref(void *u, int add) { if (add) IUnknown_AddRef((IUnknown *)u); else dm_host_release_later(u); }

/* a style track's reference (774123a0: the loader's GetObject of a style); the engine style with a reference */
static struct dms_style *loader_getstyle(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    IDirectMusicStyle8 *o = NULL;
    if (FAILED(ldr_get(ctx, &RM_CLSID_DirectMusicStyle, &RM_IID_IDirectMusicStyle8, g, file, name, (void **)&o)) || !o) {
        LOG_INFO("music: style track: style %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    dms_style *r = style_hold_of(o);
    IDirectMusicStyle8_Release(o);
    return r;
}
