/* reDM, included by dmcom.c: segments, track parameters (Get/SetParam) and waves. */
/* ------------------------------------------------------------------ segments */
static IDirectMusicSegment8Vtbl seg_vtbl;
static rm_segment *sgm(IDirectMusicSegment8 *i) { return impl_from(i, rm_segment, IDirectMusicSegment8_iface); }
static rm_segment *unsafe_segment(IUnknown *u)
{
    IDirectMusicSegment8 *s = rm_ours(u, &IID_IDirectMusicSegment8, &seg_vtbl);
    return s ? sgm(s) : NULL;
}

static HRESULT WINAPI seg_qi(IDirectMusicSegment8 *i, REFIID riid, void **out)
{
    rm_segment *s = sgm(i);
    if (IsEqualIID(riid, &IID_IDirectMusicSegment2)) { s->ver = 2; *out = &s->IDirectMusicSegment8_iface; }
    else if (IsEqualIID(riid, &IID_IDirectMusicSegment8)) { s->ver = 8; *out = &s->IDirectMusicSegment8_iface; }
    else if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicSegment)) *out = &s->IDirectMusicSegment8_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &s->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &s->IPersistStream_iface;
    else if (IsEqualIID(riid, &IID_IDispatch)) {
        if (!s->autod && !(s->autod = rm_auto_new((IUnknown *)&s->IDirectMusicSegment8_iface, RM_AUTO_SEGMENT))) { *out = NULL; return E_OUTOFMEMORY; }
        *out = s->autod;
    }
    else { return rm_noiface(__func__, riid, out); }
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG WINAPI seg_addref(IDirectMusicSegment8 *i) { return InterlockedIncrement(&sgm(i)->ref); }
static ULONG WINAPI seg_release(IDirectMusicSegment8 *i)
{
    rm_segment *s = sgm(i);
    LONG r = InterlockedDecrement(&s->ref);
    LOG_CALL("music: Segment::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) {
        /* under the lock: the engine thread reads the wrapper (and its graph) and counts the segment under it */
        dm_engine_lock();
        if (s->seg && s->seg->wrapper == s) s->seg->wrapper = NULL;
        dm_segment_release(s->seg);
        dm_engine_unlock();
        if (s->graph) IDirectMusicGraph_Release(s->graph);
        rm_auto_free(s->autod);
        free(s);
    }
    return r;
}
/* reAL: the engine segment read with the engine lock held: a Load on another thread replaces (and may free) it */
#define SEG_GET(i, field, out) do { dm_engine_lock(); dm_segment *g_ = sgm(i)->seg; (out) = g_ ? g_->field : 0; dm_engine_unlock(); } while (0)
#define SEG_SET(i, field, v) do { dm_engine_lock(); dm_segment *g_ = sgm(i)->seg; if (g_) g_->field = (v); dm_engine_unlock(); } while (0)
static HRESULT WINAPI seg_getlength(IDirectMusicSegment8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; SEG_GET(i, length, *t); return S_OK; }
static HRESULT WINAPI seg_setlength(IDirectMusicSegment8 *i, MUSIC_TIME t) { if (t <= 0) return E_INVALIDARG; SEG_SET(i, length, t); return S_OK; }
static HRESULT WINAPI seg_getrepeats(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; SEG_GET(i, repeats, *r); return S_OK; }
static HRESULT WINAPI seg_setrepeats(IDirectMusicSegment8 *i, DWORD r) { SEG_SET(i, repeats, r); return S_OK; }
static HRESULT WINAPI seg_getres(IDirectMusicSegment8 *i, DWORD *r) { if (!r) return E_POINTER; SEG_GET(i, resolution, *r); return S_OK; }
static HRESULT WINAPI seg_setres(IDirectMusicSegment8 *i, DWORD r) { SEG_SET(i, resolution, r); return S_OK; }
static HRESULT WINAPI seg_gettrack(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, IDirectMusicTrack **t);
static HRESULT WINAPI seg_gettrackgroup(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD *g);
static HRESULT WINAPI seg_inserttrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t, DWORD g);
static HRESULT WINAPI seg_removetrack(IDirectMusicSegment8 *i, IDirectMusicTrack *t);
static HRESULT WINAPI seg_initplay(IDirectMusicSegment8 *i, IDirectMusicSegmentState **s, IDirectMusicPerformance *p, DWORD f);
/* 77311edc GetGraph, 77311f48 SetGraph: a segment state starts with a clone of the graph (tops_state_graph) */
static HRESULT WINAPI seg_getgraph(IDirectMusicSegment8 *i, IDirectMusicGraph **g)
{
    if (IsBadWritePtr(g, sizeof(*g))) return E_POINTER;
    rm_segment *s = sgm(i);
    dm_engine_lock();
    IDirectMusicGraph *x = s->graph;
    if (x) { *g = x; IDirectMusicGraph_AddRef(x); }
    dm_engine_unlock();
    return x ? S_OK : DMUS_E_NOT_FOUND;
}
static HRESULT WINAPI seg_setgraph(IDirectMusicSegment8 *i, IDirectMusicGraph *g)
{
    if (g && (IsBadReadPtr(g, 4) || IsBadReadPtr(*(void **)g, 4) || IsBadCodePtr(**(FARPROC **)g))) return E_POINTER;
    rm_segment *s = sgm(i);
    if (g) IDirectMusicGraph_AddRef(g);
    dm_engine_lock();
    IDirectMusicGraph *o = s->graph;
    s->graph = g;
    dm_engine_unlock();
    if (o) IDirectMusicGraph_Release(o);
    return S_OK;
}
/* the segment's notification types are read by the performance thread: changed under the engine lock */
static HRESULT seg_notif(IDirectMusicSegment8 *i, REFGUID g, int add)
{
    rm_segment *s = sgm(i);
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!s->seg) return S_OK;
    dm_engine_lock();
    int hr = add ? dm_segment_add_notification(s->seg, (const dm_guid *)g, 0) : dm_segment_remove_notification(s->seg, (const dm_guid *)g, 0);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_addnotif(IDirectMusicSegment8 *i, REFGUID g) { return seg_notif(i, g, 1); }
static HRESULT WINAPI seg_remnotif(IDirectMusicSegment8 *i, REFGUID g) { return seg_notif(i, g, 0); }
/* ------------------------------------------------------------------ Get/SetParam */
#define DMG(n, a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) static const GUID n = { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
DMG(G_TempoParam, 0xd2ac28a5, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_TimeSignature, 0xd2ac28a4, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_EnableTempo, 0x45fc707e, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_DisableTempo, 0x45fc707d, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_EnableTimeSig, 0x45fc707c, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_DisableTimeSig, 0x45fc707b, 0x1db4, 0x11d2, 0xbc, 0xac, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
DMG(G_Download, 0xd2ac28a7, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_Unload, 0xd2ac28a8, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_DownloadToAudioPath, 0x9f2c0341, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DMG(G_UnloadFromAudioPath, 0x9f2c0342, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DMG(G_EnableAutoDownload, 0xd2ac28a9, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_DisableAutoDownload, 0xd2ac28aa, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_ClearAllBands, 0xd2ac28ab, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_IDirectMusicBand, 0xd2ac28ac, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_BandParam, 0x02bb1938, 0xcb8b, 0x11d2, 0x8b, 0xb9, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6);
DMG(G_StandardMIDIFile, 0x06621075, 0xe92e, 0x11d1, 0xa8, 0xc5, 0x00, 0xc0, 0x4f, 0xa3, 0x72, 0x6e);
DMG(G_ConnectToDLSCollection, 0x1db1ae6b, 0xe92e, 0x11d1, 0xa8, 0xc5, 0x00, 0xc0, 0x4f, 0xa3, 0x72, 0x6e);
DMG(G_MuteParam, 0xd2ac28af, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_ChordParam, 0xd2ac289e, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_RhythmParam, 0xd2ac289f, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_CommandParam, 0xd2ac289d, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_CommandParam2, 0x28f97ef7, 0x9538, 0x11d2, 0x97, 0xa9, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DMG(G_CommandParamNext, 0x472afe7a, 0x281b, 0x11d3, 0x81, 0x7d, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DMG(G_IDirectMusicStyle, 0xd2ac28a1, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_SeedVariations, 0x65b76fa5, 0xff37, 0x11d2, 0x81, 0x4e, 0x00, 0xc0, 0x4f, 0xa3, 0x6e, 0x58);
DMG(G_StyleTimeSig, 0x76612507, 0x4f37, 0x4b35, 0x80, 0x92, 0x50, 0x48, 0x4e, 0xd4, 0xba, 0x92);
DMG(G_ValidStartTime, 0x7f6b1760, 0x1fdb, 0x11d3, 0x82, 0x26, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
DMG(G_PlayMarker, 0xd8761a41, 0x801a, 0x11d3, 0x9b, 0xd1, 0xda, 0xf7, 0xe1, 0xc3, 0xd8, 0x34);
DMG(G_IDirectMusicChordMap_, 0xd2ac28ad, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_PerfMasterGrooveLevel, 0xd2ac28b2, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_Variations, 0x11f72cce, 0x26e6, 0x4ecd, 0xaf, 0x2e, 0xd6, 0x68, 0xe6, 0x67, 0x07, 0xd8);
DMG(G_PatternStyle, 0x689821f4, 0xb3bc, 0x44dd, 0x80, 0xd4, 0x0c, 0xf3, 0x2f, 0xe4, 0xd2, 0x1b);    /* dmstyle's private */
DMG(G_ScriptTrackNotify, 0x1cc7e0bf, 0x981c, 0x4b9f, 0xbe, 0x17, 0xd5, 0x72, 0xfc, 0x5f, 0xa9, 0x33);     /* dmscript's */
DMG(G_PatternPart, 0x09f25101, 0x505d, 0x4c7c, 0xbe, 0x15, 0x66, 0x2b, 0x33, 0xe7, 0x8a, 0x7c);     /* dmstyle's private */
DMG(G_PrivateTempo, 0xe8dbd832, 0xbcf0, 0x4c8c, 0xa0, 0x75, 0xa3, 0xf1, 0x5e, 0x67, 0xfd, 0x63);
static const struct { const GUID *g; int prm; } s_params[] = {
    { &G_TempoParam, PRM_TEMPO }, { &G_TimeSignature, PRM_TIMESIG }, { &G_EnableTempo, PRM_ENABLETEMPO },
    { &G_DisableTempo, PRM_DISABLETEMPO }, { &G_EnableTimeSig, PRM_ENABLETIMESIG }, { &G_DisableTimeSig, PRM_DISABLETIMESIG },
    { &G_Download, PRM_DOWNLOAD }, { &G_Unload, PRM_UNLOAD }, { &G_DownloadToAudioPath, PRM_DOWNLOAD_AP },
    { &G_UnloadFromAudioPath, PRM_UNLOAD_AP }, { &G_EnableAutoDownload, PRM_ENABLE_AUTODL },
    { &G_DisableAutoDownload, PRM_DISABLE_AUTODL }, { &G_ClearAllBands, PRM_CLEARBANDS }, { &G_IDirectMusicBand, PRM_IBAND },
    { &G_BandParam, PRM_BANDPARAM }, { &G_StandardMIDIFile, PRM_SMF }, { &G_ConnectToDLSCollection, PRM_CONNECTDLS },
    { &G_MuteParam, PRM_MUTE }, { &G_PrivateTempo, PRM_PRIVTEMPO }, { &G_ChordParam, PRM_CHORD }, { &G_RhythmParam, PRM_RHYTHM },
    { &G_CommandParam, PRM_COMMAND }, { &G_CommandParam2, PRM_COMMAND2 }, { &G_CommandParamNext, PRM_COMMANDNEXT },
    { &G_IDirectMusicStyle, PRM_STYLE }, { &G_SeedVariations, PRM_SEEDVAR }, { &G_StyleTimeSig, PRM_STYLETS }, { &G_ValidStartTime, PRM_VALIDSTART }, { &G_PlayMarker, PRM_PLAYMARKER }, { &G_IDirectMusicChordMap_, PRM_CHORDMAP },
    { &G_Variations, PRM_VARIATIONS }, { &G_PatternStyle, PRM_PATSTYLE }, { &G_PatternPart, PRM_PATPART }, { &G_ScriptTrackNotify, PRM_SCRIPTNOTIFY }, { &GUID_NULL, PRM_ANY },
};
static int prm_of(REFGUID g)
{
    for (size_t k = 0; k < sizeof(s_params) / sizeof(s_params[0]); k++) if (IsEqualGUID(g, s_params[k].g)) return s_params[k].prm;
    return PRM_OTHER;
}
static int prm_takes_target(int prm)
{
    return prm == PRM_DOWNLOAD || prm == PRM_UNLOAD || prm == PRM_DOWNLOAD_AP || prm == PRM_UNLOAD_AP;
}
static rm_collection *col_of(IUnknown *u);
static struct dms_style *style_hold_of(void *p);       /* dmcomsty.c: with a reference */
void dms_style_release(struct dms_style *s);            /* (dmsty.h) */
static dm_band *band_hold_of(void *p);
static struct dmc_chordmap *chordmap_engine_of(void *p);     /* dmcomcmp.c */
static void prm_out(int prm, int hr, void *p);
static rm_perf *perf_from_unknown(IUnknown *u);
static rm_path *unsafe_path(IUnknown *u);

/* the engine data a SetParam passes: a performance / audiopath for the download parameters, a collection for
 * ConnectToDLSCollection, a style, chord map or band (used under the engine lock); the collection, style and band
 * with a reference: prm_drop once the engine took its own (a Load on another thread may replace them) */
static HRESULT prm_data(int prm, void *p, dm_dltarget *dt, void **data)
{
    *data = p;
    if (prm_takes_target(prm)) {
        if (!p) return E_POINTER;
        rm_perf *pf = perf_from_unknown(p);
        if (!pf || !pf->host) return E_NOINTERFACE;
        rm_path *a = unsafe_path(p);
        dt->pf = dm_host_perf(pf->host); dt->ap = a ? a->ap : NULL;
        *data = dt;
    } else if (prm == PRM_CONNECTDLS) {
        if (!p) return E_POINTER;
        rm_collection *c = col_of(p);
        if (!c || !(*data = col_hold(c))) return E_NOINTERFACE;
    } else if (prm == PRM_STYLE) {
        if (!p) return E_POINTER;
        if (!(*data = style_hold_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_CHORDMAP) {
        if (!p) return E_POINTER;
        if (!(*data = chordmap_engine_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_IBAND) {
        if (!p) return E_POINTER;
        if (!(*data = band_hold_of(p))) return E_NOINTERFACE;
    } else if (prm == PRM_BANDPARAM) {
        if (!p || IsBadReadPtr(p, 8)) return E_POINTER;
        const DMUS_BAND_PARAM *bp = p;
        if (!(dt->bp.band = band_hold_of(bp->pBand))) return E_NOINTERFACE;
        dt->bp.physical = bp->mtTimePhysical;
        *data = &dt->bp;
    }
    return S_OK;
}
static void prm_drop(int prm, dm_dltarget *dt, void *data)
{
    if (prm == PRM_CONNECTDLS) dls_release(data);
    else if (prm == PRM_STYLE) dms_style_release(data);
    else if (prm == PRM_IBAND) dm_band_release(data);
    else if (prm == PRM_BANDPARAM) dm_band_release(dt->bp.band);
}

static HRESULT WINAPI seg_getparam(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, MUSIC_TIME *nx, void *p)
{
    char b[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    int prm = prm_of(g);
    if ((prm == PRM_TEMPO || prm == PRM_TIMESIG) && !p) return E_POINTER;
    mtime_t next = nx ? *nx : 0;      /* untouched when no track answers */
    dm_engine_lock();
    int hr = dm_segment_getparam(sgm(i)->seg, prm, gb, n, t, nx ? &next : NULL, p);
    dm_engine_unlock();
    prm_out(prm, hr, p);
    if (nx) *nx = next;
    LOG_CALL("music: Segment::GetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, hr);
    return hr;
}
static HRESULT WINAPI seg_setparam(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, MUSIC_TIME t, void *p)
{
    char b[64];
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    int prm = prm_of(g);
    dm_dltarget dt; void *data;
    HRESULT hr = prm_data(prm, p, &dt, &data);
    if (FAILED(hr)) return hr;
    dm_engine_lock();
    hr = dm_segment_setparam(sgm(i)->seg, prm, gb, n, t, data);
    dm_engine_unlock();
    prm_drop(prm, &dt, data);
    if (prm == PRM_OTHER) LOG_INFO("music: Segment::SetParam(%s) -> %08x (not supported)", guidstr(g, b), (unsigned)hr);
    else LOG_CALL("music: Segment::SetParam(%s, %lx, %lx, %ld) -> %08x", guidstr(g, b), (unsigned long)gb, (unsigned long)n, (long)t, (unsigned)hr);
    return hr;
}
static HRESULT WINAPI seg_clone(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b, IDirectMusicSegment **o);
static HRESULT WINAPI seg_setstart(IDirectMusicSegment8 *i, MUSIC_TIME t)
{
    HRESULT hr = S_OK;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    if (!s) hr = E_FAIL;
    else if (t < 0 || t >= s->length) hr = DMUS_E_OUT_OF_RANGE;
    else s->playstart = t;
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_getstart(IDirectMusicSegment8 *i, MUSIC_TIME *t) { if (!t) return E_POINTER; SEG_GET(i, playstart, *t); return S_OK; }
static HRESULT WINAPI seg_setloop(IDirectMusicSegment8 *i, MUSIC_TIME a, MUSIC_TIME b)
{
    HRESULT hr = S_OK;
    dm_engine_lock();
    dm_segment *s = sgm(i)->seg;
    if (!s) hr = E_FAIL;
    else if ((a || b) && (a < 0 || b > s->length || a >= b)) hr = DMUS_E_OUT_OF_RANGE;
    else { s->loopstart = a; s->loopend = b; }
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_getloop(IDirectMusicSegment8 *i, MUSIC_TIME *a, MUSIC_TIME *b)
{
    if (!a || !b) return E_POINTER;
    SEG_GET(i, loopstart, *a); SEG_GET(i, loopend, *b);
    return S_OK;
}
static HRESULT WINAPI seg_setpch(IDirectMusicSegment8 *i, DWORD n, DWORD *p)
{
    if (!n) return S_OK;
    if (!p) return E_INVALIDARG;
    if (n > 0x3fffffff || IsBadReadPtr(p, n * sizeof(DWORD))) return E_POINTER;    /* (a count whose size wraps) */
    if (!sgm(i)->seg) return S_OK;
    dm_engine_lock();
    HRESULT hr = dm_segment_add_pchannels(sgm(i)->seg, (const uint32_t *)p, n);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_settrackcfg(IDirectMusicSegment8 *i, REFGUID g, DWORD gb, DWORD n, DWORD on, DWORD off)
{
    if (!g || IsBadReadPtr(g, sizeof(GUID))) return E_POINTER;
    if (!sgm(i)->seg) return DMUS_E_TRACK_NOT_FOUND;
    dm_engine_lock();
    HRESULT hr = dm_segment_set_trackconfig(sgm(i)->seg, (const dm_guid *)g, gb, n, on, off);
    dm_engine_unlock();
    return hr;
}
static HRESULT WINAPI seg_getapconfig(IDirectMusicSegment8 *i, IUnknown **c) { if (c) *c = NULL; return DMUS_E_NO_AUDIOPATH_CONFIG; }
static rm_segment *new_segment(dm_segment *g);
int dmc_segment_compose(dm_segment *s);
/* 773150d0: the composing tracks composed (into a clone of the segment when one is asked for) */
/* Segment.Recompose (dmcompos 77287974 with flags 0, on the application's thread and rand()) */
HRESULT rm_segment_recompose(IUnknown *u)
{
    rm_segment *s = unsafe_segment(u);
    if (!s || !s->seg) return E_INVALIDARG;
    dm_engine_lock();
    int r = dmc_recompose(s->seg, dm_app_rand_state(), 1);
    dm_engine_unlock();
    return r;
}
static HRESULT WINAPI seg_compose(IDirectMusicSegment8 *i, MUSIC_TIME t, IDirectMusicSegment *a, IDirectMusicSegment *b, IDirectMusicSegment **c)
{
    if ((a && (IsBadReadPtr(a, 4) || IsBadReadPtr(*(void **)a, 4))) || (b && (IsBadReadPtr(b, 4) || IsBadReadPtr(*(void **)b, 4))) ||
        (c && IsBadWritePtr(c, 4)))
        return E_POINTER;
    rm_segment *s = sgm(i), *fa = a ? unsafe_segment((IUnknown *)a) : NULL, *fb = b ? unsafe_segment((IUnknown *)b) : NULL;
    if ((a && !fa) || (b && !fb)) return rm_notimpl("Segment::Compose from or to a segment not made by reDM");
    if (!s->seg) return DMUS_E_NOT_INIT;
    dm_engine_lock();
    int hr = DM_OK;
    dm_segment *g = s->seg;
    if (c) { g = dm_segment_clone(s->seg, 0, s->seg->length, &hr); if (!g) hr = E_OUTOFMEMORY; }
    if (g) hr = dmc_segment_compose_ex(g, t, fa ? fa->seg : NULL, fb ? fb->seg : NULL);
    /* (a failed composition still hands out its segment, as dmime's: comptest join) */
    if (c && g) {
        rm_segment *o = new_segment(g);
        if (!o) dm_segment_release(g);  /* (under the lock, as the engine's other releases) */
        dm_engine_unlock();
        if (!o) return E_OUTOFMEMORY;
        HRESULT r = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)c);
        seg_release(&o->IDirectMusicSegment8_iface);
        if (FAILED(r)) return r;
    } else dm_engine_unlock();
    LOG_CALL("music: Segment::Compose -> %08x", hr);
    return hr;
}
static rm_perf *perf_from_unknown(IUnknown *u);
static rm_path *unsafe_path(IUnknown *u);
/* 77312338 / 773123e7: SetParam(GUID_DownloadToAudioPath / GUID_UnloadFromAudioPath) on every track; a segment
 * without such tracks succeeds */
static HRESULT WINAPI seg_download(IDirectMusicSegment8 *i, IUnknown *target)
{
    if (!target) return E_POINTER;
    HRESULT hr = seg_setparam(i, &G_DownloadToAudioPath, 0xffffffff, DM_SEG_ALLTRACKS, 0, target);
    if (hr == DMUS_E_TRACK_NOT_FOUND) hr = S_OK;
    LOG_CALL("music: Segment::Download -> %08x", (unsigned)hr);
    return hr;
}
static HRESULT WINAPI seg_unload(IDirectMusicSegment8 *i, IUnknown *target)
{
    if (!target) return E_POINTER;
    HRESULT hr = seg_setparam(i, &G_UnloadFromAudioPath, 0xffffffff, DM_SEG_ALLTRACKS, 0, target);
    return hr == DMUS_E_TRACK_NOT_FOUND ? S_OK : hr;
}
static IDirectMusicSegment8Vtbl seg_vtbl = { seg_qi, seg_addref, seg_release, seg_getlength, seg_setlength, seg_getrepeats,
    seg_setrepeats, seg_getres, seg_setres, seg_gettrack, seg_gettrackgroup, seg_inserttrack, seg_removetrack, seg_initplay,
    seg_getgraph, seg_setgraph, seg_addnotif, seg_remnotif, seg_getparam, seg_setparam, seg_clone, seg_setstart, seg_getstart,
    seg_setloop, seg_getloop, seg_setpch, seg_settrackcfg, seg_getapconfig, seg_compose, seg_download, seg_unload };

static rm_segment *sgo(IDirectMusicObject *i) { return impl_from(i, rm_segment, IDirectMusicObject_iface); }
static HRESULT WINAPI sgo_qi(IDirectMusicObject *i, REFIID r, void **o) { return seg_qi(&sgo(i)->IDirectMusicSegment8_iface, r, o); }
static ULONG WINAPI sgo_addref(IDirectMusicObject *i) { return seg_addref(&sgo(i)->IDirectMusicSegment8_iface); }
static ULONG WINAPI sgo_release(IDirectMusicObject *i) { return seg_release(&sgo(i)->IDirectMusicSegment8_iface); }
static HRESULT WINAPI sgo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_get(&sgo(i)->desc, &CLSID_DirectMusicSegment, d); }
static HRESULT WINAPI sgo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_set(&sgo(i)->desc, d, TRUE); }
/* 773136a4: a segment form's descriptor ('DMSG' or 'AASE' chunks), a wave's (what the wave class says of it), a MIDI
 * file's (the class only); the flags start from none */
static void wave_desc_of(const BYTE *b, DWORD n, DMUS_OBJECTDESC *d);
static HRESULT WINAPI sgo_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d)
{
    if (!s || !d) return E_POINTER;
    BYTE *b; DWORD n;
    if (!read_stream(s, &b, &n)) return DMUS_E_CHUNKNOTFOUND;
    HRESULT hr = S_OK;
    d->dwValidData = 0;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && (!memcmp(b + 8, "DMSG", 4) || !memcmp(b + 8, "AASE", 4)))
        rm_desc_from_form(b + 12, n - 12, d);
    else if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) wave_desc_of(b, n, d);
    else if (!dm_is_midi(b, n)) hr = DMUS_E_CHUNKNOTFOUND;
    free(b);
    if (FAILED(hr)) return hr;
    d->dwValidData |= DMUS_OBJ_CLASS;
    d->guidClass = CLSID_DirectMusicSegment;
    return S_OK;
}
static IDirectMusicObjectVtbl sgo_vtbl = { sgo_qi, sgo_addref, sgo_release, sgo_getdesc, sgo_setdesc, sgo_parse };

static dm_collection *loader_getcoll(void *ctx, const dm_guid *g, const char *file);
static dm_wave *loader_getwave(void *ctx, const dm_guid *g, const char *file, const char *name);
static struct dms_style *loader_getstyle(void *ctx, const dm_guid *g, const char *file, const char *name);
static struct dmc_chordmap *loader_getcmap(void *ctx, const dm_guid *g, const char *file, const char *name);
static dm_segment *loader_getseg(void *ctx, const dm_guid *g, const char *file, const char *name);
static void *loader_getscript(void *ctx, const dm_guid *g, const char *file, const char *name, int32_t *hr);
static dm_segment *segment_from_data(const BYTE *b, DWORD n, IDirectMusicLoader8 *l)
{
    if (dm_is_midi(b, n)) return dm_segment_from_midi(b, n, loader_getcoll, l);
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {      /* a wave as a segment */
        dm_wave *w = dm_wave_load(b, n);
        if (!w) return NULL;
        dm_segment *g = dm_segment_from_wave(w);
        dm_wave_release(w);
        return g;
    }
    dm_loadcb cb = { loader_getcoll, loader_getwave, loader_getstyle, l, loader_getcmap, loader_getseg, loader_getscript };
    /* (without the performance's lock: the loader is called; the style tracks sort a shared style again under it,
     * through dms_resort_hook) */
    return dm_segment_load_cb(b, n, &cb);
}
static rm_segment *sgp(IPersistStream *i) { return impl_from(i, rm_segment, IPersistStream_iface); }
static HRESULT WINAPI sgp_qi(IPersistStream *i, REFIID r, void **o) { return seg_qi(&sgp(i)->IDirectMusicSegment8_iface, r, o); }
static ULONG WINAPI sgp_addref(IPersistStream *i) { return seg_addref(&sgp(i)->IDirectMusicSegment8_iface); }
static ULONG WINAPI sgp_release(IPersistStream *i) { return seg_release(&sgp(i)->IDirectMusicSegment8_iface); }
static HRESULT WINAPI sgp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = CLSID_DirectMusicSegment; return S_OK; }
static HRESULT WINAPI sgp_isdirty(IPersistStream *i) { return S_FALSE; }
/* 77314148 / 77312ab9: a segment file's tool graph (a 'DMTG' form in the 'DMSG' form): a new graph loads it (its
 * tools are created by class, so they must be registered) and becomes the segment's; if it fails, so does the
 * segment's Load */
static IDirectMusicGraph *segment_graph_from_data(const BYTE *b, DWORD n, HRESULT *res)
{
    *res = S_OK;
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DMSG", 4)) return NULL;
    DWORD fsz = *(const DWORD *)(b + 4), end = fsz > n - 8 ? n : 8 + fsz;
    for (DWORD off = 12; off + 12 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4);
        if (sz > end - off - 8) break;          /* (a chunk past the form's end: the last; no wrapping round) */
        if (!memcmp(b + off, "RIFF", 4) && !memcmp(b + off + 8, "DMTG", 4)) {
            IDirectMusicGraph *g = NULL;
            HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, 8 + sz);
            IStream *st = NULL;
            if (!hg || FAILED(rm_graph_create(&IID_IDirectMusicGraph, (void **)&g))) { if (hg) GlobalFree(hg); return NULL; }
            memcpy(GlobalLock(hg), b + off, 8 + sz);
            GlobalUnlock(hg);
            if (SUCCEEDED(CreateStreamOnHGlobal(hg, TRUE, &st))) {
                HRESULT hr = rm_graph_load(g, st);
                if (FAILED(hr)) LOG_INFO("music: a segment's tool graph: Load %08lx", hr);
                *res = hr;
                IStream_Release(st);
            } else GlobalFree(hg);
            return g;
        }
        off += 8 + sz + (sz & 1);
    }
    return NULL;
}
/* 77313c61: a segment form's embedded containers ('DMCN' forms), loaded through the loader from the segment's own
 * stream so that the objects in them can be found while the tracks load; let go of when the segment is loaded */
static int segment_containers(IStream *st, ULONGLONG at, const BYTE *b, DWORD n, IDirectMusicLoader8 *l,
                              IDirectMusicObject ***out)
{
    *out = NULL;
    if (!l || n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DMSG", 4)) return 0;
    DWORD fsz = *(const DWORD *)(b + 4), end = fsz > n - 8 ? n : 8 + fsz;
    int k = 0;
    for (DWORD off = 12; off + 12 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4);
        if (!memcmp(b + off, "RIFF", 4) && !memcmp(b + off + 8, "DMCN", 4)) {
            LARGE_INTEGER p; p.QuadPart = (LONGLONG)(at + off);
            DMUS_OBJECTDESC d;
            memset(&d, 0, sizeof(d));
            d.dwSize = sizeof(d);
            d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_STREAM;
            d.guidClass = CLSID_DirectMusicContainer;
            d.pStream = st;
            IDirectMusicObject *o = NULL;
            if (SUCCEEDED(IStream_Seek(st, p, STREAM_SEEK_SET, NULL)) &&
                SUCCEEDED(IDirectMusicLoader8_GetObject(l, &d, &IID_IDirectMusicObject, (void **)&o)) && o) {
                IDirectMusicObject **a = realloc(*out, (k + 1) * sizeof(*a));
                if (a) { *out = a; a[k++] = o; } else IDirectMusicObject_Release(o);
            }
        }
        if (sz > end - off - 8) break;          /* (past the form's end: the last; no wrapping round) */
        off += 8 + sz + (sz & 1);
    }
    return k;
}
static HRESULT WINAPI sgp_load(IPersistStream *i, IStream *st)
{
    rm_segment *s = sgp(i);
    BYTE *b; DWORD n;
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER at;
    if (!st) return E_POINTER;
    if (FAILED(IStream_Seek(st, z, STREAM_SEEK_CUR, &at)) || !read_stream(st, &b, &n)) return E_FAIL;
    IDirectMusicLoader8 *l = rm_stream_loader(st);
    IDirectMusicObject **cont;
    int ncont = segment_containers(st, at.QuadPart, b, n, l, &cont);
    dm_segment *g = segment_from_data(b, n, l);
    HRESULT ghr;
    IDirectMusicGraph *gr = g ? segment_graph_from_data(b, n, &ghr) : NULL;
    if (g && n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "DMSG", 4)) rm_desc_from_form(b + 12, n - 12, &s->desc);
    else if (g && n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) wave_desc_of(b, n, &s->desc);
    free(b);
    for (int k = 0; k < ncont; k++) {
        IDirectMusicLoader8_ReleaseObject(l, cont[k]);
        IDirectMusicObject_Release(cont[k]);
    }
    free(cont);
    if (!g) return DMUS_E_UNSUPPORTED_STREAM;
    if (gr && FAILED(ghr)) { IDirectMusicGraph_Release(gr); dm_segment_release(g); return ghr; }
    dm_engine_lock();                       /* (the old segment may be playing: its count and wrapper under the lock) */
    if (s->seg) { if (s->seg->wrapper == s) s->seg->wrapper = NULL; dm_segment_release(s->seg); }
    s->seg = g;
    g->wrapper = s;
    IDirectMusicGraph *og = NULL;
    if (gr) { og = s->graph; s->graph = gr; }
    dm_engine_unlock();
    if (og) IDirectMusicGraph_Release(og);
    s->desc.dwValidData |= DMUS_OBJ_LOADED;
    return g->partial ? DMUS_S_PARTIALLOAD : S_OK;
}
static HRESULT WINAPI sgp_save(IPersistStream *i, IStream *s, BOOL c) { return rm_notimpl(__func__); }
static HRESULT WINAPI sgp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl sgp_vtbl = { sgp_qi, sgp_addref, sgp_release, sgp_getclass, sgp_isdirty, sgp_load, sgp_save, sgp_getsize };

static rm_segment *new_segment(dm_segment *g)
{
    rm_segment *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->IDirectMusicSegment8_iface.lpVtbl = &seg_vtbl;
    s->IDirectMusicObject_iface.lpVtbl = &sgo_vtbl;
    s->IPersistStream_iface.lpVtbl = &sgp_vtbl;
    s->ref = 1;
    s->seg = g;
    if (g) g->wrapper = s;
    s->desc.dwSize = sizeof(s->desc);
    s->desc.dwValidData = g ? DMUS_OBJ_LOADED : 0;
    s->desc.guidClass = CLSID_DirectMusicSegment;
    return s;
}


/* ------------------------------------------------------------------ waves (dswave CDirectSoundWave): IDirectMusicObject
 * and IPersistStream; segments and wave tracks take the dm_wave inside */
static rm_wave *wvo(IDirectMusicObject *i) { return impl_from(i, rm_wave, IDirectMusicObject_iface); }
static rm_wave *wvp(IPersistStream *i) { return impl_from(i, rm_wave, IPersistStream_iface); }
static IDirectMusicObjectVtbl wvo_vtbl;
static HRESULT WINAPI wvo_qi(IDirectMusicObject *i, REFIID riid, void **out)
{
    rm_wave *w = wvo(i);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &w->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &w->IPersistStream_iface;
    else return rm_noiface("Wave::QueryInterface", riid, out);
    InterlockedIncrement(&w->ref);
    return S_OK;
}
static ULONG WINAPI wvo_addref(IDirectMusicObject *i) { return InterlockedIncrement(&wvo(i)->ref); }
static ULONG WINAPI wvo_release(IDirectMusicObject *i)
{
    rm_wave *w = wvo(i);
    LONG r = InterlockedDecrement(&w->ref);
    if (!r) { if (w->w) { if (w->w->wrapper == w) w->w->wrapper = NULL; dm_wave_release(w->w); } free(w); }
    return r;
}
/* 6c782466 */
static HRESULT WINAPI wvo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    if (!d) return E_POINTER;
    DWORD sz = d->dwSize;
    memcpy(d, &wvo(i)->desc, sz < sizeof(*d) ? sz : sizeof(*d));
    d->dwSize = sz;
    return S_OK;
}
static HRESULT WINAPI wvo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    rm_wave *w = wvo(i);
    if (!d) return E_POINTER;
    if (d->dwValidData & DMUS_OBJ_OBJECT) w->desc.guidObject = d->guidObject;
    if (d->dwValidData & DMUS_OBJ_NAME) memcpy(w->desc.wszName, d->wszName, sizeof(d->wszName));
    if (d->dwValidData & DMUS_OBJ_VERSION) w->desc.vVersion = d->vVersion;
    w->desc.dwValidData |= d->dwValidData & (DMUS_OBJ_OBJECT | DMUS_OBJ_NAME | DMUS_OBJ_VERSION);
    return (d->dwValidData & ~(DMUS_OBJ_OBJECT | DMUS_OBJ_NAME | DMUS_OBJ_VERSION | DMUS_OBJ_CLASS)) ? S_FALSE : S_OK;
}
/* what dswave says of a wave file: its guid, name and version */
static void wave_desc_of(const BYTE *b, DWORD n, DMUS_OBJECTDESC *d)
{
    dm_wave *w = dm_wave_load(b, n);
    if (!w) return;
    if (w->hasguid) { memcpy(&d->guidObject, &w->guid, sizeof(GUID)); d->dwValidData |= DMUS_OBJ_OBJECT; }
    if (w->name[0]) { MultiByteToWideChar(CP_ACP, 0, w->name, -1, d->wszName, DMUS_MAX_NAME); d->dwValidData |= DMUS_OBJ_NAME; }
    if (w->hasvers) { d->vVersion.dwVersionMS = w->vers[0]; d->vVersion.dwVersionLS = w->vers[1]; d->dwValidData |= DMUS_OBJ_VERSION; }
    dm_wave_release(w);
}
static HRESULT WINAPI wvo_parse(IDirectMusicObject *i, LPSTREAM st, LPDMUS_OBJECTDESC d)
{
    BYTE *b; DWORD n;
    if (!st || !d) return E_POINTER;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    dm_wave *w = dm_wave_load(b, n);
    free(b);
    if (!w) return DMUS_E_UNSUPPORTED_STREAM;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwValidData = DMUS_OBJ_CLASS;
    d->guidClass = RM_CLSID_DirectSoundWave;
    if (w->hasguid) { memcpy(&d->guidObject, &w->guid, sizeof(GUID)); d->dwValidData |= DMUS_OBJ_OBJECT; }
    if (w->name[0]) { MultiByteToWideChar(CP_ACP, 0, w->name, -1, d->wszName, DMUS_MAX_NAME); d->dwValidData |= DMUS_OBJ_NAME; }
    if (w->hasvers) { d->vVersion.dwVersionMS = w->vers[0]; d->vVersion.dwVersionLS = w->vers[1]; d->dwValidData |= DMUS_OBJ_VERSION; }
    dm_wave_release(w);
    return S_OK;
}
static IDirectMusicObjectVtbl wvo_vtbl = { wvo_qi, wvo_addref, wvo_release, wvo_getdesc, wvo_setdesc, wvo_parse };
static HRESULT WINAPI wvp_qi(IPersistStream *i, REFIID r, void **o) { return wvo_qi(&wvp(i)->IDirectMusicObject_iface, r, o); }
static ULONG WINAPI wvp_addref(IPersistStream *i) { return wvo_addref(&wvp(i)->IDirectMusicObject_iface); }
static ULONG WINAPI wvp_release(IPersistStream *i) { return wvo_release(&wvp(i)->IDirectMusicObject_iface); }
static HRESULT WINAPI wvp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = RM_CLSID_DirectSoundWave; return S_OK; }
static HRESULT WINAPI wvp_isdirty(IPersistStream *i) { return S_FALSE; }
static void wave_desc(rm_wave *o)
{
    dm_wave *w = o->w;
    o->desc.dwValidData |= DMUS_OBJ_LOADED;
    if (w->hasguid) { memcpy(&o->desc.guidObject, &w->guid, sizeof(GUID)); o->desc.dwValidData |= DMUS_OBJ_OBJECT; }
    if (w->name[0]) { MultiByteToWideChar(CP_ACP, 0, w->name, -1, o->desc.wszName, DMUS_MAX_NAME); o->desc.dwValidData |= DMUS_OBJ_NAME; }
    if (w->hasvers) { o->desc.vVersion.dwVersionMS = w->vers[0]; o->desc.vVersion.dwVersionLS = w->vers[1]; o->desc.dwValidData |= DMUS_OBJ_VERSION; }
}
static HRESULT WINAPI wvp_load(IPersistStream *i, IStream *st)
{
    rm_wave *o = wvp(i);
    BYTE *b; DWORD n;
    if (!st) return E_INVALIDARG;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    dm_wave *w = dm_wave_load(b, n);
    free(b);
    if (!w) return E_INVALIDARG;
    dm_engine_lock();                   /* (loader_getwave reads o->w under it) */
    dm_wave *old = o->w;
    if (old && old->wrapper == o) old->wrapper = NULL;
    o->w = w;
    w->wrapper = o;
    wave_desc(o);
    dm_engine_unlock();
    dm_wave_release(old);
    return S_OK;
}
static HRESULT WINAPI wvp_save(IPersistStream *i, IStream *s, BOOL c) { return rm_notimpl(__func__); }
static HRESULT WINAPI wvp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl wvp_vtbl = { wvp_qi, wvp_addref, wvp_release, wvp_getclass, wvp_isdirty, wvp_load, wvp_save, wvp_getsize };

static rm_wave *new_wave(dm_wave *w)
{
    rm_wave *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicObject_iface.lpVtbl = &wvo_vtbl;
    o->IPersistStream_iface.lpVtbl = &wvp_vtbl;
    o->ref = 1;
    o->desc.dwSize = sizeof(o->desc);
    o->desc.dwValidData = DMUS_OBJ_CLASS;
    o->desc.guidClass = RM_CLSID_DirectSoundWave;
    if (w) { o->w = w; w->wrapper = o; wave_desc(o); }
    return o;
}

/* compressed waves (dswave decodes through ACM: 6c783750): the whole file to PCM, in the format 'wavu' named or
 * acmFormatSuggest picks */
static int acm_decode(const uint8_t *wfx, uint32_t wfxlen, const uint8_t *in, uint32_t inlen, uint8_t **out,
                      uint32_t *outlen, uint16_t *ch, uint32_t *rate, uint16_t *bits)
{
    uint32_t need = wfxlen < 18 ? 18 : wfxlen;
    WAVEFORMATEX *src = calloc(1, need + 64);
    if (!src) return 0;
    memcpy(src, wfx, wfxlen);
    if (wfxlen < 18) src->cbSize = 0;
    else if (src->cbSize > wfxlen - 18) src->cbSize = (WORD)(wfxlen - 18);    /* (the codec reads only what the file has) */
    WAVEFORMATEX dst;
    memset(&dst, 0, sizeof(dst));
    dst.wFormatTag = WAVE_FORMAT_PCM;
    if (*ch) {
        dst.nChannels = *ch; dst.nSamplesPerSec = *rate; dst.wBitsPerSample = *bits;
        dst.nBlockAlign = (WORD)(dst.nChannels * dst.wBitsPerSample / 8);
        dst.nAvgBytesPerSec = dst.nSamplesPerSec * dst.nBlockAlign;
    } else if (acmFormatSuggest(NULL, src, &dst, sizeof(dst), ACM_FORMATSUGGESTF_WFORMATTAG)) { free(src); return 0; }
    HACMSTREAM has;
    if (acmStreamOpen(&has, NULL, src, &dst, NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME)) { free(src); return 0; }
    DWORD osize = 0;
    if (acmStreamSize(has, inlen, &osize, ACM_STREAMSIZEF_SOURCE) || !osize) { acmStreamClose(has, 0); free(src); return 0; }
    uint8_t *ib = malloc(inlen ? inlen : 1), *ob = malloc(osize + dst.nBlockAlign);
    if (!ib || !ob) { free(ib); free(ob); acmStreamClose(has, 0); free(src); return 0; }
    memcpy(ib, in, inlen);
    ACMSTREAMHEADER hd;
    memset(&hd, 0, sizeof(hd));
    hd.cbStruct = sizeof(hd); hd.pbSrc = ib; hd.cbSrcLength = inlen; hd.pbDst = ob; hd.cbDstLength = osize;
    int ok = 0;
    if (!acmStreamPrepareHeader(has, &hd, 0)) {
        ok = !acmStreamConvert(has, &hd, ACM_STREAMCONVERTF_BLOCKALIGN | ACM_STREAMCONVERTF_START | ACM_STREAMCONVERTF_END);
        acmStreamUnprepareHeader(has, &hd, 0);
    }
    acmStreamClose(has, 0);
    free(ib); free(src);
    if (!ok) { free(ob); return 0; }
    *out = ob; *outlen = hd.cbDstLengthUsed;
    *ch = dst.nChannels; *rate = dst.nSamplesPerSec; *bits = dst.wBitsPerSample;
    return 1;
}
