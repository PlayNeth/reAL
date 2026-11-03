/* reDM, included by dmcom.c: tracks (GetTrack / InsertTrack / Clone, the standalone track objects). */
/* ------------------------------------------------------------------ tracks (GetTrack / InsertTrack / Clone) */
struct rm_track {
    IDirectMusicTrack8 IDirectMusicTrack8_iface;
    IPersistStream IPersistStream_iface;
    IDirectMusicPatternTrack IDirectMusicPatternTrack_iface;   /* the pattern track's */
    LONG ref;
    dm_track *t;
};
static IDirectMusicTrack8Vtbl trk_vtbl;
static IPersistStreamVtbl trp_vtbl;
static IDirectMusicPatternTrackVtbl ptk_vtbl;
static rm_track *trk(IDirectMusicTrack8 *i) { return impl_from(i, rm_track, IDirectMusicTrack8_iface); }

/* the COM object of a track (one per track, made on demand) */
static IDirectMusicTrack8 *track_object(dm_track *t)
{
    rm_track *r = t->wrapper;
    if (r && ref_if_alive(&r->ref)) return &r->IDirectMusicTrack8_iface;
    if (!(r = calloc(1, sizeof(*r)))) return NULL;
    r->IDirectMusicTrack8_iface.lpVtbl = &trk_vtbl;
    r->IPersistStream_iface.lpVtbl = &trp_vtbl;
    r->IDirectMusicPatternTrack_iface.lpVtbl = &ptk_vtbl;
    r->ref = 1; r->t = t;
    dm_track_addref(t);
    t->wrapper = r;
    return &r->IDirectMusicTrack8_iface;
}
static rm_track *unsafe_track(IUnknown *u)
{
    if (!u) return NULL;
    IDirectMusicTrack8 *t = (IDirectMusicTrack8 *)u;
    if (t->lpVtbl == &trk_vtbl) return trk(t);
    if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IDirectMusicTrack8, (void **)&t))) {
        IDirectMusicTrack8_Release(t);
        if (t->lpVtbl == &trk_vtbl) return trk(t);
    }
    return NULL;
}
static HRESULT WINAPI trk_qi(IDirectMusicTrack8 *i, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    /* dmime's tracks: IDirectMusicTrack8 except the time signature and marker tracks' (7731cc26); IPersist only on dmband's */
    int kind = trk(i)->t->kind;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicTrack) ||
        (IsEqualIID(riid, &IID_IDirectMusicTrack8) && kind != TRK_TIMESIG && kind != TRK_MARKER)) *out = i;
    else if (IsEqualIID(riid, &IID_IPersistStream) || (IsEqualIID(riid, &IID_IPersist) && kind == TRK_BAND))
        *out = &trk(i)->IPersistStream_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicPatternTrack) && kind == TRK_PATTERN)     /* 77413466 */
        *out = &trk(i)->IDirectMusicPatternTrack_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&trk(i)->ref);
    return S_OK;
}
static ULONG WINAPI trk_addref(IDirectMusicTrack8 *i) { return InterlockedIncrement(&trk(i)->ref); }
static ULONG WINAPI trk_release(IDirectMusicTrack8 *i)
{
    rm_track *r = trk(i);
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) {
        dm_engine_lock();
        if (r->t->wrapper == r) r->t->wrapper = NULL;
        dm_track_release(r->t);
        dm_engine_unlock();
        free(r);
    }
    return n;
}
static HRESULT WINAPI trk_init(IDirectMusicTrack8 *i, IDirectMusicSegment *s) { return S_OK; }
/* a track played by the application (InitPlay, Play, EndPlay on a state and performance of its choosing): the engine
 * keeps the track's state as a segment state would; the chord map, signpost, mute and marker tracks have none
 * (their InitPlay gives NULL), a style or pattern track without a style fails with E_POINTER, a motif track without
 * a motif with E_FAIL */
static rm_state *unsafe_state(IUnknown *u);
static rm_perf *perf_from_unknown(IUnknown *u);
static BYTE s_nostate;                      /* (what a failed motif InitPlay leaves: never played or ended) */
static HRESULT WINAPI trk_initplay(IDirectMusicTrack8 *i, IDirectMusicSegmentState *ss, IDirectMusicPerformance *p, void **d, DWORD id, DWORD f)
{
    dm_track *t = trk(i)->t;
    if (!d || IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    *d = NULL;
    dm_engine_lock();
    int ok = dm_track_can_play(t);
    dm_engine_unlock();
    if (ok == DM_E_FAIL) *d = &s_nostate;
    if (ok < 0) return ok;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    rm_perf *pf = p ? perf_from_unknown((IUnknown *)p) : NULL;
    if (!st || !st->ss || !pf || !pf->host) return rm_notimpl("Track::InitPlay (a state or performance not made by reDM)");
    dm_host_lock(pf->host);
    if ((ok = dm_track_can_play(t)) < 0) {          /* (again: a Load on another thread may have replaced the data) */
        dm_host_unlock(pf->host);
        if (ok == DM_E_FAIL) *d = &s_nostate;
        return ok;
    }
    int ihr;
    void *x = dm_track_direct_init(dm_host_perf(pf->host), st->ss, t, f, id, &ihr);
    if (x && (t->kind == TRK_CHORDMAP || t->kind == TRK_SIGNPOST || t->kind == TRK_MUTE || t->kind == TRK_MARKER)) {
        dm_track_direct_end(x);
        x = NULL;
    } else if (!x) { dm_host_unlock(pf->host); return E_OUTOFMEMORY; }
    dm_host_unlock(pf->host);
    *d = x;
    /* (dmime's wave track hands out its state even so; nothing ever ends it, and it keeps the track) */
    if (ihr < 0) trk_addref(i);
    return ihr;
}
static HRESULT WINAPI trk_endplay(IDirectMusicTrack8 *i, void *d)
{
    if (!d || d == &s_nostate) return S_OK;
    dm_engine_lock();
    dm_track_direct_end(d);             /* (on the performance it started on) */
    dm_engine_unlock();
    return S_OK;
}
static HRESULT trk_play_any(IDirectMusicTrack8 *i, void *d, int64_t a, int64_t b, int64_t o, DWORD f, IDirectMusicPerformance *p,
                            IDirectMusicSegmentState *ss, int clock)
{
    if (d == &s_nostate) return E_POINTER;
    rm_state *st = ss ? unsafe_state((IUnknown *)ss) : NULL;
    rm_perf *pf = p ? perf_from_unknown((IUnknown *)p) : NULL;
    if (!st || !st->ss || !pf || !pf->host) return rm_notimpl("Track::Play (a state or performance not made by reDM)");
    dm_host_lock(pf->host);
    int hr = dm_track_direct_play(dm_host_perf(pf->host), st->ss, d, trk(i)->t, a, b, o, f, clock);
    dm_host_unlock(pf->host);
    return hr;
}
static HRESULT WINAPI trk_play(IDirectMusicTrack8 *i, void *d, MUSIC_TIME a, MUSIC_TIME b, MUSIC_TIME o, DWORD f, IDirectMusicPerformance *p, IDirectMusicSegmentState *ss, DWORD id)
{
    return trk_play_any(i, d, a, b, o, f, p, ss, 0);
}
static HRESULT WINAPI trk_getparam(IDirectMusicTrack8 *i, REFGUID g, MUSIC_TIME t, MUSIC_TIME *nx, void *p)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int prm = prm_of(g);
    mtime_t next = nx ? *nx : 0;
    dm_engine_lock();
    HRESULT hr = dm_track_getparam(trk(i)->t, prm, t, nx ? &next : NULL, p);
    prm_out(prm, hr, p);
    dm_engine_unlock();
    if (nx) *nx = next;
    return hr;
}
static HRESULT WINAPI trk_setparam(IDirectMusicTrack8 *i, REFGUID g, MUSIC_TIME t, void *p)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    int prm = prm_of(g);
    dm_dltarget dt; void *data;
    HRESULT hr = prm_data(prm, p, &dt, &data);
    if (FAILED(hr)) return hr;
    dm_engine_lock();
    hr = dm_track_setparam(trk(i)->t, prm, t, data);
    dm_engine_unlock();
    prm_drop(prm, &dt, data);
    return hr;
}
static HRESULT WINAPI trk_isparam(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_engine_lock();                   /* (a Load on another thread may replace the track's data) */
    HRESULT hr = dm_track_isparam(trk(i)->t, prm_of(g));
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI trk_addnotif(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_engine_lock();
    HRESULT hr = dm_track_add_notification(trk(i)->t, (const dm_guid *)g);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI trk_remnotif(IDirectMusicTrack8 *i, REFGUID g)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    dm_engine_lock();
    HRESULT hr = dm_track_remove_notification(trk(i)->t, (const dm_guid *)g);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI trk_clone(IDirectMusicTrack8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicTrack **o)
{
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    int hr;
    dm_engine_lock();
    dm_track *c = dm_track_clone(trk(i)->t, a, b, &hr);
    dm_engine_unlock();
    if (!c) return hr;
    *o = (IDirectMusicTrack *)track_object(c);
    dm_track_release(c);
    return *o ? S_OK : E_OUTOFMEMORY;
}
/* PlayEx: clock time (DMUS_TRACKF_CLOCK) for the wave track; music time otherwise */
static HRESULT WINAPI trk_playex(IDirectMusicTrack8 *i, void *d, REFERENCE_TIME a, REFERENCE_TIME b, REFERENCE_TIME o, DWORD f, IDirectMusicPerformance *p, IDirectMusicSegmentState *ss, DWORD id)
{
    int clock = (f & DMUS_TRACKF_CLOCK) != 0;
    if (clock && trk(i)->t->kind != TRK_WAVE) { a /= 10000; b /= 10000; o /= 10000; }
    return trk_play_any(i, d, a, b, o, f & ~DMUS_TRACKF_CLOCK, p, ss, clock && trk(i)->t->kind == TRK_WAVE);
}
/* GetParamEx / SetParamEx (7731bded / 77324130): DMUS_TRACK_PARAMF_CLOCK times are reference time, used in 1/10000 */
static HRESULT WINAPI trk_getparamex(IDirectMusicTrack8 *i, REFGUID g, REFERENCE_TIME t, REFERENCE_TIME *nx, void *p, void *sd, DWORD f)
{
    MUSIC_TIME n = 0;
    HRESULT hr = trk_getparam(i, g, (MUSIC_TIME)((f & 1) ? t / 10000 : t), &n, p);
    if (nx) *nx = (f & 1) ? (REFERENCE_TIME)n * 10000 : n;
    return hr;
}
static HRESULT WINAPI trk_setparamex(IDirectMusicTrack8 *i, REFGUID g, REFERENCE_TIME t, void *p, void *sd, DWORD f)
{
    return trk_setparam(i, g, (MUSIC_TIME)((f & 1) ? t / 10000 : t), p);
}
static HRESULT WINAPI trk_compose(IDirectMusicTrack8 *i, IUnknown *c, DWORD id, IDirectMusicTrack **o) { return E_NOTIMPL; }
/* Join (tempo 7731caa4, chord 77402abe, command 77404040, style 7741305c, band 772027cc, chord map 7728c90c,
 * signpost 7728b802; E_NOTIMPL for the others) */
static rm_segment *unsafe_segment(IUnknown *u);
static HRESULT WINAPI trk_join(IDirectMusicTrack8 *i, IDirectMusicTrack *t, MUSIC_TIME j, IUnknown *c, DWORD id, IDirectMusicTrack **o)
{
    dm_track *me = trk(i)->t;
    switch (me->kind) {
    case TRK_TEMPO: case TRK_CHORD: case TRK_COMMAND: case TRK_STYLE: case TRK_BAND: case TRK_CHORDMAP: case TRK_SIGNPOST: break;
    default: return E_NOTIMPL;
    }
    if (!t || IsBadReadPtr(t, 4) || IsBadReadPtr(*(void **)t, 4) || (o && IsBadWritePtr(o, 4)) ||
        (c && (IsBadReadPtr(c, 4) || IsBadReadPtr(*(void **)c, 4))))
        return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    rm_segment *g = c ? unsafe_segment(c) : NULL;
    if (!r || (c && !g)) return rm_notimpl("Track::Join (a track or segment not made by reDM)");
    dm_engine_lock();
    dm_track *res = NULL;
    int hr = dm_track_join(me, r->t, j, g ? g->seg : NULL, id, o ? &res : NULL);
    dm_engine_unlock();
    if (o) {
        *o = NULL;
        if (res) { *o = (IDirectMusicTrack *)track_object(res); dm_track_release(res); if (!*o) return E_OUTOFMEMORY; }
    }
    return hr;
}
static IDirectMusicTrack8Vtbl trk_vtbl = { trk_qi, trk_addref, trk_release, trk_init, trk_initplay, trk_endplay, trk_play,
    trk_getparam, trk_setparam, trk_isparam, trk_addnotif, trk_remnotif, trk_clone, trk_playex, trk_getparamex, trk_setparamex,
    trk_compose, trk_join };

static rm_track *trp(IPersistStream *i) { return impl_from(i, rm_track, IPersistStream_iface); }
static HRESULT WINAPI trp_qi(IPersistStream *i, REFIID riid, void **out) { return trk_qi(&trp(i)->IDirectMusicTrack8_iface, riid, out); }
static ULONG WINAPI trp_addref(IPersistStream *i) { return trk_addref(&trp(i)->IDirectMusicTrack8_iface); }
static ULONG WINAPI trp_release(IPersistStream *i) { return trk_release(&trp(i)->IDirectMusicTrack8_iface); }
static HRESULT WINAPI trp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; memcpy(c, &trp(i)->t->clsid, sizeof(*c)); return S_OK; }
static HRESULT WINAPI trp_isdirty(IPersistStream *i) { return S_FALSE; }
/* the track's chunk (its header and size, then that much) */
static BOOL read_chunk(IStream *s, BYTE **data, DWORD *size)
{
    BYTE h[8];
    ULONG got = 0;
    *data = NULL; *size = 0;
    if (FAILED(IStream_Read(s, h, 8, &got)) || got != 8) return FALSE;
    DWORD n = *(DWORD *)(h + 4);
    if (n > 0x7ffffff0u) return FALSE;      /* (a broken size: 8 + n + 1 would wrap) */
    BYTE *b = malloc(8 + (size_t)n + 1);
    if (!b) return FALSE;
    memcpy(b, h, 8);
    if (n && (FAILED(IStream_Read(s, b + 8, n, &got)) || got != n)) { free(b); return FALSE; }
    *data = b; *size = 8 + n;
    return TRUE;
}
/* the end of a wave track chunk's first item, and the next one's header: where dmime's Load stops when it has no loader
 * to give the item's reference to */
static DWORD wavetrack_noloader_stop(const BYTE *b, DWORD n)
{
    for (DWORD off = 12; off + 12 <= n; ) {
        DWORD sz = *(const DWORD *)(b + off + 4);
        if (sz > n - off - 8) sz = n - off - 8;     /* (sizes past the data cut to it: no reading beyond, no wrapping) */
        if (!memcmp(b + off, "LIST", 4) && !memcmp(b + off + 8, "wavp", 4)) {
            for (DWORD q = off + 12; q + 12 <= off + 8 + sz; ) {
                DWORD qs = *(const DWORD *)(b + q + 4);
                if (!memcmp(b + q, "LIST", 4) && !memcmp(b + q + 8, "wavi", 4)) {
                    DWORD r = q + 12;
                    if (r + 8 <= n) { DWORD rs = *(const DWORD *)(b + r + 4); ULONGLONG e = (ULONGLONG)r + 16 + rs + (rs & 1); r = e < n ? (DWORD)e : n; }
                    return r < n ? r : n;
                }
                if (qs > off + 8 + sz - q - 8) break;
                q += 8 + qs + (qs & 1);
            }
        }
        off += 8 + sz + (sz & 1);
    }
    return n;
}
/* Load of a track made on its own: its chunk; references through the stream's loader (the segment trigger track
 * asks for it before it reads, the style, chord map and band tracks fail after reading without one, the wave track
 * at its first item's reference) */
static HRESULT WINAPI trp_load(IPersistStream *i, IStream *s)
{
    dm_track *t = trp(i)->t;
    if (!s || IsBadReadPtr(s, 4)) return E_POINTER;
    IDirectMusicLoader8 *l = rm_stream_loader(s);
    if ((t->kind == TRK_SEGTRIG || t->kind == TRK_SCRIPT) && !l) return E_NOINTERFACE;
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER at;
    if (FAILED(IStream_Seek(s, z, STREAM_SEEK_CUR, &at))) at.QuadPart = 0;
    BYTE *b; DWORD n;
    if (!read_chunk(s, &b, &n)) return DMUS_E_CHUNKNOTFOUND;
    HRESULT hr = S_OK;
    if (!l && (t->kind == TRK_STYLE || t->kind == TRK_CHORDMAP)) hr = E_NOINTERFACE;
    else if (!l && t->kind == TRK_BAND) hr = (HRESULT)0x88781150;
    else if (!l && t->kind == TRK_WAVE) {
        LARGE_INTEGER p; p.QuadPart = (LONGLONG)(at.QuadPart + wavetrack_noloader_stop(b, n));
        IStream_Seek(s, p, STREAM_SEEK_SET, NULL);
        hr = E_NOINTERFACE;
    }
    if (SUCCEEDED(hr)) {
        dm_loadcb cb = { loader_getcoll, loader_getwave, loader_getstyle, l, loader_getcmap, loader_getseg, loader_getscript };
        /* parsed without the performance's lock (the loader is called), put in place with it (states playing the
         * track move to the new data) */
        void *d = NULL;
        hr = dm_track_parse_chunk(t, b, n, &cb, &d);
        if (d) {
            dm_engine_lock();
            dm_track_set_data(t, d);
            dm_engine_unlock();
        }
    }
    free(b);
    return hr;
}
/* the signpost track's (7728af59: its 'sgnp' chunk); the others are not written */
static HRESULT WINAPI trp_save(IPersistStream *i, IStream *s, BOOL c)
{
    dm_track *t = trp(i)->t;
    if (t->kind != TRK_SIGNPOST) return rm_notimpl(__func__);
    size_t n;
    dm_engine_lock();
    uint8_t *b = dmc_sptrack_save(t->data, &n);
    dm_engine_unlock();
    if (!b) return E_OUTOFMEMORY;
    ULONG w = 0;
    HRESULT hr = IStream_Write(s, b, (ULONG)n, &w);
    free(b);
    return FAILED(hr) ? hr : w == n ? S_OK : STG_E_MEDIUMFULL;
}
static HRESULT WINAPI trp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl trp_vtbl = { trp_qi, trp_addref, trp_release, trp_getclass, trp_isdirty, trp_load, trp_save, trp_getsize };

/* 77313113 GetTrack: the index-th track of the class (GUID_NULL: any) in the group */
static HRESULT WINAPI seg_gettrack(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, IDirectMusicTrack **t)
{
    if (!t || IsBadWritePtr(t, sizeof(*t))) return E_POINTER;
    if (!g) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    static const GUID null_guid;
    for (int k = 0; s && k < s->ntracks; k++) {
        dm_segtrack *e = &s->tracks[k];
        if (!(e->group & gb) || !(IsEqualGUID(g, &null_guid) || !memcmp(g, &e->t->clsid, sizeof(GUID)))) continue;
        if (n--) continue;
        *t = (IDirectMusicTrack *)track_object(e->t);
        hr = *t ? S_OK : E_OUTOFMEMORY;
        break;
    }
    dm_engine_unlock();
    return hr;
}
/* 773119c5 GetTrackGroup */
static HRESULT WINAPI seg_gettrackgroup(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD *g)
{
    if (!t || !g || IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    if (!r) return DMUS_E_NOT_FOUND;
    HRESULT hr = DMUS_E_NOT_FOUND;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    for (int k = 0; s && k < s->ntracks; k++) if (s->tracks[k].t == r->t) { *g = s->tracks[k].group; hr = S_OK; break; }
    dm_engine_unlock();
    return hr;
}
/* 773131cb / 77311b2c InsertTrack: config DMUS_TRACKCONFIG_DEFAULT, position and priority 0, in track order; the track
 * takes the segment's notification types.  Only reDM's own tracks can go in. */
static HRESULT WINAPI seg_inserttrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD g)
{
    if (!t || IsBadReadPtr(t, sizeof(void *))) return E_POINTER;
    if (!g) return E_INVALIDARG;
    rm_track *r = unsafe_track((IUnknown *)t);
    if (!r) return rm_notimpl("Segment::InsertTrack (a track not made by reDM)");
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    if (!s) { dm_engine_unlock(); return DMUS_E_NOT_INIT; }
    dm_track_addref(r->t);
    int ok = dm_segment_insert_track(s, r->t, 0, g, 0x38, 0);
    dm_engine_unlock();
    return ok ? S_OK : E_OUTOFMEMORY;
}
/* 77311c54 RemoveTrack: S_FALSE when the track is not in the segment */
static HRESULT WINAPI seg_removetrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t)
{
    if (!t || IsBadReadPtr(t, sizeof(void *))) return E_POINTER;
    rm_track *r = unsafe_track((IUnknown *)t);
    if (!r) return S_FALSE;
    HRESULT hr = S_FALSE;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    for (int k = 0; s && k < s->ntracks; k++)
        if (s->tracks[k].t == r->t) {
            dm_track *x = s->tracks[k].t;
            memmove(s->tracks + k, s->tracks + k + 1, (s->ntracks - k - 1) * sizeof(*s->tracks));
            s->ntracks--;
            dm_track_release(x);
            hr = S_OK;
            break;
        }
    dm_engine_unlock();
    return hr;
}
/* 77313a27 Clone */
static HRESULT WINAPI seg_clone(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicSegment **o)
{
    if (!o || IsBadWritePtr(o, sizeof(*o))) return E_POINTER;
    int hr = E_FAIL;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    dm_segment *c = s ? dm_segment_clone(s, a, b, &hr) : NULL;
    IDirectMusicGraph *gr = c ? sgm(i)->graph : NULL;
    if (gr) IDirectMusicGraph_AddRef(gr);       /* (the application's graph object: AddRef only, no call into it) */
    dm_engine_unlock();
    if (!c) { *o = NULL; return hr; }
    rm_segment *n = new_segment(c);
    if (!n) { if (gr) IDirectMusicGraph_Release(gr); dm_engine_lock(); dm_segment_release(c); dm_engine_unlock(); *o = NULL; return E_OUTOFMEMORY; }
    n->graph = gr;                              /* 77313acd: the same graph */
    n->ver = sgm(i)->ver;                       /* 77313be0 */
    *o = (IDirectMusicSegment *)&n->IDirectMusicSegment8_iface;
    return hr;
}
