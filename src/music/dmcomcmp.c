/* reDM: the chord map object (dmcompos CDMChordMap 77288f66), included by dmcom.c.
 * IDirectMusicChordMap8 on dmc_chordmap (dmcomp.c). */
#include "dmcomp.h"

DMG(RM_CLSID_DirectMusicChordMap, 0xd2ac288f, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(RM_IID_IDirectMusicChordMap, 0xd2ac28be, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(G_IDirectMusicChordMap, 0xd2ac28ad, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);

typedef struct rm_chordmap {
    IDirectMusicChordMap IDirectMusicChordMap_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dmc_chordmap *m;
} rm_chordmap;
static rm_chordmap *cmp(IDirectMusicChordMap *i) { return impl_from(i, rm_chordmap, IDirectMusicChordMap_iface); }
static rm_chordmap *cmo(IDirectMusicObject *i) { return impl_from(i, rm_chordmap, IDirectMusicObject_iface); }
static rm_chordmap *cms(IPersistStream *i) { return impl_from(i, rm_chordmap, IPersistStream_iface); }
static IDirectMusicChordMapVtbl cmp_vtbl;
static IDirectMusicObjectVtbl cmo_vtbl;
static IPersistStreamVtbl cms_vtbl;

/* 77287c59: IUnknown and IDirectMusicChordMap, IDirectMusicObject, IPersistStream (not IPersist) */
static HRESULT WINAPI cmp_qi(IDirectMusicChordMap *i, REFIID riid, void **out)
{
    rm_chordmap *o = cmp(i);
    if (IsBadWritePtr(out, 4) || IsBadReadPtr(riid, 16)) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &RM_IID_IDirectMusicChordMap)) *out = &o->IDirectMusicChordMap_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream)) *out = &o->IPersistStream_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &o->IDirectMusicObject_iface;
    else return rm_noiface("ChordMap::QueryInterface", riid, out);
    InterlockedIncrement(&o->ref);
    return S_OK;
}
static ULONG WINAPI cmp_addref(IDirectMusicChordMap *i) { return InterlockedIncrement(&cmp(i)->ref); }
static ULONG WINAPI cmp_release(IDirectMusicChordMap *i)
{
    rm_chordmap *o = cmp(i);
    LONG r = InterlockedDecrement(&o->ref);
    if (!r) {
        dm_engine_lock();               /* (chordmap_iface reads the wrapper under it) */
        if (o->m && o->m->wrapper == o) o->m->wrapper = NULL;
        dm_engine_unlock();
        dmc_chordmap_release(o->m);
        free(o);
    }
    return r;
}
/* 77287e92 */
static HRESULT WINAPI cmp_getscale(IDirectMusicChordMap *i, DWORD *scale)
{
    if (IsBadWritePtr(scale, 4)) return E_POINTER;
    *scale = cmp(i)->m->scale;
    return S_OK;
}
static IDirectMusicChordMapVtbl cmp_vtbl = { cmp_qi, cmp_addref, cmp_release, cmp_getscale };

static HRESULT WINAPI cmo_qi(IDirectMusicObject *i, REFIID r, void **o) { return cmp_qi(&cmo(i)->IDirectMusicChordMap_iface, r, o); }
static ULONG WINAPI cmo_addref(IDirectMusicObject *i) { return cmp_addref(&cmo(i)->IDirectMusicChordMap_iface); }
static ULONG WINAPI cmo_release(IDirectMusicObject *i) { return cmp_release(&cmo(i)->IDirectMusicChordMap_iface); }
/* 77287d50: the whole descriptor cleared (whatever dwSize says); the class, LOADED once loaded, the object when the
 * guid's first six bytes are not zero, the name once it has one */
static HRESULT WINAPI cmo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    dmc_chordmap *m = cmo(i)->m;
    if (IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->dwValidData = DMUS_OBJ_CLASS;
    d->guidClass = RM_CLSID_DirectMusicChordMap;
    if (m->loaded) d->dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_LOADED;
    if (m->guid.d1 || m->guid.d2) { d->dwValidData |= DMUS_OBJ_OBJECT; memcpy(&d->guidObject, &m->guid, sizeof(GUID)); }
    if (m->hasname) { d->dwValidData |= DMUS_OBJ_NAME; lstrcpynW(d->wszName, (const WCHAR *)m->name, DMUS_MAX_NAME); }
    return S_OK;
}
/* 77287df4: the object and name taken; any other flag: S_FALSE with dwValidData cut to the two taken */
static HRESULT WINAPI cmo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    dmc_chordmap *m = cmo(i)->m;
    if (IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    if (d->dwSize < sizeof(*d)) return E_INVALIDARG;
    DWORD took = 0;
    if (d->dwValidData & DMUS_OBJ_OBJECT) { memcpy(&m->guid, &d->guidObject, sizeof(GUID)); took |= DMUS_OBJ_OBJECT; }
    if (d->dwValidData & DMUS_OBJ_NAME) {
        int k = 0;
        for (; k < 23 && d->wszName[k]; k++) m->name[k] = d->wszName[k];
        m->name[k] = 0;
        m->hasname = 1;
        took |= DMUS_OBJ_NAME;
    }
    if (!(d->dwValidData & ~took)) return S_OK;
    d->dwValidData = took;
    return S_FALSE;
}
/* 772880a8 / 77287eea: the class always; the guid and the perh name (16 characters) when there */
static HRESULT WINAPI cmo_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d)
{
    if (IsBadReadPtr(s, 4) || IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    BYTE *b; DWORD n;
    if (!read_stream(s, &b, &n)) return DMUS_E_CHUNKNOTFOUND;
    HRESULT hr = DMUS_E_CHUNKNOTFOUND;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "DMPR", 4)) {
        DWORD end = 8 + *(const DWORD *)(b + 4);
        if (end > n) end = n;
        if (end < 12) end = 12;         /* (a size under 4: no chunks, not a wrapped length) */
        dm_guid g;
        uint16_t name[17];
        d->dwValidData = DMUS_OBJ_CLASS;
        d->guidClass = RM_CLSID_DirectMusicChordMap;
        uint32_t v = dmc_chordmap_parse(b + 12, end - 12, &g, name);
        if (v & 1) { memcpy(&d->guidObject, &g, sizeof(GUID)); d->dwValidData |= DMUS_OBJ_OBJECT; }
        if (v & 4) { memcpy(d->wszName, name, sizeof(name)); d->dwValidData |= DMUS_OBJ_NAME; }
        hr = S_OK;
    }
    free(b);
    return hr;
}
static IDirectMusicObjectVtbl cmo_vtbl = { cmo_qi, cmo_addref, cmo_release, cmo_getdesc, cmo_setdesc, cmo_parse };

static HRESULT WINAPI cms_qi(IPersistStream *i, REFIID r, void **o) { return cmp_qi(&cms(i)->IDirectMusicChordMap_iface, r, o); }
static ULONG WINAPI cms_addref(IPersistStream *i) { return cmp_addref(&cms(i)->IDirectMusicChordMap_iface); }
static ULONG WINAPI cms_release(IPersistStream *i) { return cmp_release(&cms(i)->IDirectMusicChordMap_iface); }
static HRESULT WINAPI cms_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_INVALIDARG; *c = RM_CLSID_DirectMusicChordMap; return S_OK; }
static HRESULT WINAPI cms_isdirty(IPersistStream *i) { return S_OK; }      /* 77287edb: a field the constructor never sets (heap garbage, in practice not 0) */
/* 77288eae: the 'DMPR' form (its absence: DMUS_E_CHUNKNOTFOUND) */
static HRESULT chordmap_load_data(dmc_chordmap *m, const BYTE *b, DWORD n)
{
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DMPR", 4)) return DMUS_E_CHUNKNOTFOUND;
    DWORD end = 8 + *(const DWORD *)(b + 4);
    if (end > n) end = n;
    if (end < 12) end = 12;
    return dmc_chordmap_load(m, b + 12, end - 12);
}
static HRESULT WINAPI cms_load(IPersistStream *i, IStream *st)
{
    rm_chordmap *o = cms(i);
    BYTE *b; DWORD n;
    if (!st) return E_INVALIDARG;
    if (!read_stream(st, &b, &n)) return DMUS_E_CHUNKNOTFOUND;
    dm_engine_lock();                   /* (loaded in place: a playing chord map track reads it) */
    HRESULT hr = chordmap_load_data(o->m, b, n);
    dm_engine_unlock();
    free(b);
    return hr;
}
static HRESULT WINAPI cms_save(IPersistStream *i, IStream *s, BOOL c) { return E_NOTIMPL; }
static HRESULT WINAPI cms_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return E_NOTIMPL; }
static IPersistStreamVtbl cms_vtbl = { cms_qi, cms_addref, cms_release, cms_getclass, cms_isdirty, cms_load, cms_save, cms_getsize };

static rm_chordmap *new_chordmap(dmc_chordmap *m)
{
    rm_chordmap *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicChordMap_iface.lpVtbl = &cmp_vtbl;
    o->IDirectMusicObject_iface.lpVtbl = &cmo_vtbl;
    o->IPersistStream_iface.lpVtbl = &cms_vtbl;
    o->ref = 1;
    o->m = m ? m : dmc_chordmap_new();
    if (!o->m) { free(o); return NULL; }
    dm_engine_lock();
    if (!o->m->wrapper) o->m->wrapper = o;
    dm_engine_unlock();
    return o;
}
/* the chord map behind an interface (ours only) */
static rm_chordmap *chordmap_of(void *p)
{
    if (!p || IsBadReadPtr(p, sizeof(void *))) return NULL;
    void *vt = *(void **)p;
    if (vt == &cmp_vtbl) return cmp(p);
    if (vt == &cmo_vtbl) return cmo(p);
    if (vt == &cms_vtbl) return cms(p);
    return NULL;
}
static dmc_chordmap *chordmap_engine_of(void *p) { rm_chordmap *o = chordmap_of(p); return o ? o->m : NULL; }
/* the interface for an engine chord map (its wrapper, made when it has none); the engine's reference handed over */
static IDirectMusicChordMap *chordmap_iface(dmc_chordmap *m)
{
    if (!m) return NULL;
    /* (not one whose last Release is under way on another thread: a new wrapper then; under the engine lock, which
     * that Release takes to clear the wrapper before the object goes) */
    dm_engine_lock();
    rm_chordmap *o = m->wrapper;
    int alive = o && ref_if_alive(&o->ref);
    dm_engine_unlock();
    if (alive) { dmc_chordmap_release(m); return &o->IDirectMusicChordMap_iface; }
    o = new_chordmap(m);
    if (!o) { dmc_chordmap_release(m); return NULL; }
    return &o->IDirectMusicChordMap_iface;
}

/* a chord map track's reference (7728bdd6: the loader's GetObject of a chord map); the engine map with a reference */
static struct dmc_chordmap *loader_getcmap(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    IDirectMusicChordMap *o = NULL;
    if (FAILED(ldr_get(ctx, &RM_CLSID_DirectMusicChordMap, &RM_IID_IDirectMusicChordMap, g, file, name, (void **)&o)) || !o) {
        LOG_INFO("music: chord map %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    dmc_chordmap *r = chordmap_engine_of(o);
    dmc_chordmap_addref(r);
    IDirectMusicChordMap_Release(o);
    return r;
}

/* ------------------------------------------------------------------ the composer (dmcompos CDMCompos 77282d2f) */
DMG(RM_CLSID_DirectMusicComposer, 0xd2ac2890, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(RM_IID_IDirectMusicComposer, 0xd2ac28bf, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
typedef struct rm_composer {
    IDirectMusicComposer IDirectMusicComposer_iface;
    LONG ref;
} rm_composer;
static rm_composer *cpo(IDirectMusicComposer *i) { return impl_from(i, rm_composer, IDirectMusicComposer_iface); }
static IDirectMusicComposerVtbl cpo_vtbl;
static HRESULT WINAPI cpo_qi(IDirectMusicComposer *i, REFIID riid, void **out)
{
    rm_composer *o = cpo(i);
    if (IsBadWritePtr(out, 4)) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &RM_IID_IDirectMusicComposer)) *out = &o->IDirectMusicComposer_iface;
    else return rm_noiface("Composer::QueryInterface", riid, out);
    InterlockedIncrement(&o->ref);
    return S_OK;
}
static ULONG WINAPI cpo_addref(IDirectMusicComposer *i) { return InterlockedIncrement(&cpo(i)->ref); }
static ULONG WINAPI cpo_release(IDirectMusicComposer *i)
{
    rm_composer *o = cpo(i);
    LONG r = InterlockedDecrement(&o->ref);
    if (!r) free(o);
    return r;
}
static rm_segment *new_segment(dm_segment *g);
static rm_segment *unsafe_segment(IUnknown *u);
static struct dms_style *style_hold_of(void *p);
/* 772821f3 / 77287974 (flags 3): a new segment from the template */
static HRESULT WINAPI cpo_fromtemplate(IDirectMusicComposer *i, IDirectMusicStyle *style, IDirectMusicSegment *tmpl,
                                       WORD activity, IDirectMusicChordMap *cm, IDirectMusicSegment **out)
{
    if ((style && IsBadWritePtr(style, 4)) || (cm && IsBadWritePtr(cm, 4)) || IsBadWritePtr(tmpl, 4) ||
        (out && IsBadWritePtr(out, 4)) || !out)
        return E_POINTER;
    rm_segment *t = unsafe_segment((IUnknown *)tmpl);
    if (!t || !t->seg) return E_INVALIDARG;
    dmc_chordmap *mx = cm ? chordmap_engine_of(cm) : NULL;
    if (cm && !mx) return rm_notimpl("Composer: a style or chord map not made by reDM");
    struct dms_style *sx = style ? style_hold_of(style) : NULL;
    if (style && !sx) return rm_notimpl("Composer: a style or chord map not made by reDM");
    dm_engine_lock();
    int hr;
    dm_segment *g = dmc_compose_template(t->seg, sx, mx, activity, &hr);
    dm_engine_unlock();
    dms_style_release(sx);
    LOG_CALL("music: Composer::ComposeSegmentFromTemplate -> %08x", hr);
    if (!g) return hr < 0 ? hr : E_FAIL;
    rm_segment *o = new_segment(g);
    if (!o) { dm_segment_release(g); return E_OUTOFMEMORY; }
    HRESULT r = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)out);
    seg_release(&o->IDirectMusicSegment8_iface);
    return FAILED(r) ? r : hr;
}
/* 77287754: the style's longest end (and, a dmstyle style's, intro, fill and break) embellishments set the lengths
 * (1 without); the measures (0 or over 0x7fff: 1) grow to hold the intro and end */
static HRESULT WINAPI cpo_fromshape(IDirectMusicComposer *i, IDirectMusicStyle *s, WORD n, WORD shape, WORD act, BOOL intro,
                                    BOOL end, IDirectMusicChordMap *cm, IDirectMusicSegment **out)
{
    if (IsBadWritePtr(s, 4) || IsBadWritePtr(cm, 4) || IsBadWritePtr(out, 4)) return E_POINTER;
    dmc_chordmap *mx = chordmap_engine_of(cm);
    if (!mx || !style_of(s)) return rm_notimpl("Composer: a style or chord map not made by reDM");
    IDirectMusicStyle8 *s8 = (IDirectMusicStyle8 *)s;
    DWORD mn, len;
    int introlen = 1, fill = 1, brk = 1, endlen = 1;
    HRESULT hr = IDirectMusicStyle8_GetEmbellishmentLength(s8, 4, 0, &mn, &len);
    if (FAILED(hr)) return hr;
    if (hr == S_OK) endlen = (int)len;
    if (IDirectMusicStyle8_GetEmbellishmentLength(s8, 2, 0, &mn, &len) == S_OK) introlen = (int)len;
    if (IDirectMusicStyle8_GetEmbellishmentLength(s8, 1, 0, &mn, &len) == S_OK) fill = (int)len;
    if (IDirectMusicStyle8_GetEmbellishmentLength(s8, 3, 0, &mn, &len) == S_OK) brk = (int)len;
    int m = (n & 0x8000) || !n ? 1 : n;
    if (shape > 8) shape = 6;
    if (intro) { if (introlen <= 0) introlen = 1; if (m <= introlen) m = introlen; }
    if (end) {
        if (endlen <= 0) endlen = 1;
        if (m <= endlen) { m = endlen; if (intro) m = endlen + introlen; }
    }
    struct dms_style *sx = style_hold_of(s);
    if (!sx) return rm_notimpl("Composer: a style or chord map not made by reDM");
    dm_engine_lock();
    int r;
    dm_segment *g = dmc_segment_from_shape(sx, mx, (unsigned)m, shape, act, intro, end, introlen, fill, brk, endlen, &r);
    dm_engine_unlock();
    dms_style_release(sx);
    LOG_CALL("music: Composer::ComposeSegmentFromShape(%u measures, shape %u, activity %u, intro %d, end %d) -> %08x",
             n, shape, act, intro, end, r);
    if (!g) return r < 0 ? r : E_FAIL;
    rm_segment *o = new_segment(g);
    if (!o) { dm_segment_release(g); return E_OUTOFMEMORY; }
    HRESULT q = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)out);
    seg_release(&o->IDirectMusicSegment8_iface);
    return FAILED(q) ? q : r;
}
/* the transition's inputs from segments (77282111 / 77282156 / 7728219d and the segment's GetParam) */
static dmc_chordmap *seg_chordmap_at(dm_segment *s, mtime_t t)      /* 77282156: the first chord map track's */
{
    if (!s) return NULL;
    for (int i = 0; i < s->ntracks; i++) {
        if (s->tracks[i].t->kind != TRK_CHORDMAP) continue;
        dmc_chordmap *m = NULL;
        return dmc_cmtrack_getparam(s->tracks[i].t->data, t, NULL, &m) >= 0 ? m : NULL;
    }
    return NULL;
}
static void key_default(dm_chord_key *k)    /* a C major seventh, when the to segment answers no chord */
{
    memset(k, 0, sizeof(*k));
    k->name[0] = 'M'; k->name[1] = '7';
    k->nsub = 1; k->key = 12; k->scale = 0xab5ab5;
    k->sub[0].chord = 0x891; k->sub[0].scale = 0xab5ab5; k->sub[0].inversion = 0xffffff; k->sub[0].levels = 0xffffffff;
    k->sub[0].chordroot = 12;
}
/* Microsoft sets only the name here; the rest of its buffer is what the stack held, which in practice is the
 * default chord a previous call left there.  The default chord is taken. */
static void key_m7(dm_chord_key *k) { key_default(k); }
static rm_segment *transition_segment(dm_segment *g, int side, rm_segment *from, rm_segment *to)
{
    rm_segment *o = new_segment(g);
    if (!o) return NULL;
    rm_segment *src = side == 1 ? from : side == 2 ? to : NULL;      /* 77282219: a clone of its graph */
    if (src && src->graph) { IDirectMusicGraph *c = NULL; if (SUCCEEDED(rm_graph_clone(src->graph, &c))) o->graph = c; }
    return o;
}
/* 77285b86 */
static HRESULT WINAPI cpo_transition(IDirectMusicComposer *i, IDirectMusicSegment *from, IDirectMusicSegment *to, MUSIC_TIME t,
                                     WORD cmd, DWORD fl, IDirectMusicChordMap *cm, IDirectMusicSegment **out)
{
    if ((to && IsBadWritePtr(to, 4)) || (from && IsBadWritePtr(from, 4)) || (cm && IsBadWritePtr(cm, 4)) || IsBadWritePtr(out, 4))
        return E_POINTER;
    int isend = cmd == 4 || cmd == 5, isintro = cmd == 2 || cmd == 5;
    if (!to && !isend && (fl & 0x1000)) return E_INVALIDARG;
    rm_segment *fo = from ? unsafe_segment((IUnknown *)from) : NULL, *tob = to ? unsafe_segment((IUnknown *)to) : NULL;
    if ((from && !fo) || (to && !tob)) return rm_notimpl("Composer::ComposeTransition (a segment not made by reDM)");
    dmc_chordmap *mx = NULL;
    if (cm && !(mx = chordmap_engine_of(cm))) return rm_notimpl("Composer::ComposeTransition (a chord map not made by reDM)");
    dm_engine_lock();
    dm_segment *fs = fo ? fo->seg : NULL, *ts = tob ? tob->seg : NULL;     /* (under the lock: a Load may replace them) */
    dms_style *fst = NULL;
    if (fs && dm_segment_getparam(fs, PRM_STYLE, 0xffffffff, 0, t, NULL, &fst) < 0) fst = NULL;
    int owncm = 0;
    if (!mx) {
        if (!isend && ts) mx = seg_chordmap_at(ts, 0);
        if (!mx && fs) mx = seg_chordmap_at(fs, t);
        owncm = mx != NULL;
    }
    double tempo = 120.0;
    const double *tp = NULL;
    if (fs) {
        for (int k = 0; k < fs->ntracks; k++) {
            if (fs->tracks[k].t->kind != TRK_TEMPO) continue;
            struct { mtime_t t; double tempo; } x;
            if (dm_track_getparam(fs->tracks[k].t, PRM_TEMPO, t, NULL, &x) >= 0) { tempo = x.tempo; tp = &tempo; }
            break;
        }
    }
    dmc_cmdparam cp = { 0, 0, 62, 0, 0 };
    if (fs && dm_segment_getparam(fs, PRM_COMMAND2, 0xffffffff, DM_SEG_ANYTRACK, t, NULL, &cp) < 0) { memset(&cp, 0, sizeof(cp)); cp.groove = 62; }
    else if (!fs) { memset(&cp, 0, sizeof(cp)); cp.groove = 62; }
    dm_chord_key fk, tk;
    if (!fs || dm_segment_getparam(fs, PRM_CHORD, 0xffffffff, DM_SEG_ANYTRACK, t, NULL, &fk) < 0) key_m7(&fk);    /* (see key_m7) */
    dm_segment *tks = ((!(fl & 0x1000) && !isintro) || !ts) ? fs : ts;
    if (!tks || dm_segment_getparam(tks, PRM_CHORD, 0xffffffff, DM_SEG_ANYTRACK, 0, NULL, &tk) < 0) key_default(&tk);
    int side = 0, hr;
    dm_segment *g = dmc_compose_transition(fst, NULL, tp, &cp, &fk, &tk, ts, cmd, fl, mx, &side, &hr);
    dm_engine_unlock();
    if (fst) dms_style_release(fst);
    if (owncm) dmc_chordmap_release(mx);
    LOG_CALL("music: Composer::ComposeTransition(%ld, command %u, flags %lx) -> %08x", (long)t, cmd, (unsigned long)fl, hr);
    *out = NULL;
    if (!g) return hr;
    rm_segment *o = transition_segment(g, side, fo, tob);
    if (!o) { dm_segment_release(g); return E_OUTOFMEMORY; }
    HRESULT q = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)out);
    seg_release(&o->IDirectMusicSegment8_iface);
    return FAILED(q) ? q : hr;
}
/* 77285f1c: the transition from what the performance plays at the next time the flags allow, played there, and the
 * to segment queued after it (or played, or the playing segment stopped, when there is no transition) */
static HRESULT WINAPI cpo_autotransition(IDirectMusicComposer *i, IDirectMusicPerformance *p, IDirectMusicSegment *to, WORD cmd,
                                         DWORD fl, IDirectMusicChordMap *cm, IDirectMusicSegment **trans,
                                         IDirectMusicSegmentState **tostate, IDirectMusicSegmentState **trstate)
{
    if ((to && IsBadWritePtr(to, 4)) || (cm && IsBadWritePtr(cm, 4)) || IsBadWritePtr(p, 4) || (trans && IsBadWritePtr(trans, 4)) ||
        (tostate && IsBadWritePtr(tostate, 4)) || (trstate && IsBadWritePtr(trstate, 4)))
        return E_POINTER;
    DWORD segf = (fl & 0x20) ? 0x2000 : (fl & 0x10) ? 0x1000 : (fl & 8) ? 0x800 : (fl & 0x80000) ? 0x4000 : ((~fl & 4) << 11);
    if (fl & 1) segf = (segf & 0xffff) | 0x10000;
    if (fl & 0x40000) segf |= 0x08000000;
    if (fl & 0x80) segf |= 0x20000;
    if (fl & 0x100) segf |= 0x40000;
    if (fl & 0x200) segf |= 0x80000;
    if (fl & 0x40) segf |= 0x400;
    if (fl & 0x100000) segf |= 0x8000;
    if (fl & 0x400000) segf |= 0x10000000;
    if (fl & 0x200000) segf |= 0x4000000;
    DWORD queuef = segf & 0x4000000;
    IDirectMusicSegment *mytrans = NULL;
    if (!trans) trans = &mytrans;
    int isend = cmd == 4 || cmd == 5, isintro = cmd == 2 || cmd == 5;
    if (!to && !isend && (fl & 0x1000)) return E_INVALIDARG;
    IDirectMusicPerformance8 *pf = (IDirectMusicPerformance8 *)p;
    REFERENCE_TIME rq, rr;
    MUSIC_TIME mt, mq;
    HRESULT hr;
    if (FAILED(hr = IDirectMusicPerformance8_GetQueueTime(pf, &rq))) return hr;
    if (FAILED(hr = IDirectMusicPerformance8_GetResolvedTime(pf, rq, &rr, segf))) return hr;
    if (FAILED(hr = IDirectMusicPerformance8_ReferenceToMusicTime(pf, rr, &mt))) return hr;
    if (FAILED(hr = IDirectMusicPerformance8_ReferenceToMusicTime(pf, rq, &mq))) return hr;
    if (rr > rq) mt--;
    IDirectMusicSegmentState *st = NULL;
    IDirectMusicSegment *fromseg = NULL;
    MUSIC_TIME start = 0;
    if (SUCCEEDED(IDirectMusicPerformance8_GetSegmentState(pf, &st, mt))) {
        if (FAILED(hr = IDirectMusicSegmentState_GetStartTime(st, &start)) || FAILED(hr = IDirectMusicSegmentState_GetSegment(st, &fromseg))) {
            IDirectMusicSegmentState_Release(st);
            return hr;
        }
        MUSIC_TIME len;
        if ((fl & 0x400) && SUCCEEDED(IDirectMusicSegment_GetLength(fromseg, &len))) { mt = len - 1; start = 0; }
    } else st = NULL;
    IDirectMusicStyle *style = NULL;
    if (FAILED(IDirectMusicPerformance8_GetParam(pf, &GUID_IDirectMusicStyle, 0xffffffff, DMUS_SEG_ANYTRACK, mt, NULL, &style))) style = NULL;
    rm_segment *fo = fromseg ? unsafe_segment((IUnknown *)fromseg) : NULL, *tob = to ? unsafe_segment((IUnknown *)to) : NULL;
    dmc_chordmap *mx = cm ? chordmap_engine_of(cm) : NULL;
    int owncm = 0;
    if (!cm) {
        dm_engine_lock();
        if (!isend && tob && tob->seg) mx = seg_chordmap_at(tob->seg, 0);
        if (!mx && fo && fo->seg) mx = seg_chordmap_at(fo->seg, 0);
        dm_engine_unlock();
        owncm = mx != NULL;
    }
    DMUS_COMMAND_PARAM_2 c2;
    if (FAILED(IDirectMusicPerformance8_GetParam(pf, &GUID_CommandParam2, 0xffffffff, DMUS_SEG_ANYTRACK, mt, NULL, &c2))) {
        memset(&c2, 0, sizeof(c2)); c2.bGrooveLevel = 62;
    }
    dm_chord_key fk, tk;
    if (FAILED(IDirectMusicPerformance8_GetParam(pf, &GUID_ChordParam, 0xffffffff, DMUS_SEG_ANYTRACK, mt, NULL, (DMUS_CHORD_KEY *)&fk))) key_m7(&fk);
    IDirectMusicSegment *tks = (((fl & 0x1000) || isintro) && to) ? to : fromseg;
    if (!tks || FAILED(IDirectMusicSegment_GetParam(tks, &GUID_ChordParam, 0xffffffff, DMUS_SEG_ANYTRACK, 0, NULL, (DMUS_CHORD_KEY *)&tk))) key_default(&tk);
    struct dms_style *sx = style ? style_hold_of(style) : NULL;
    if (!sx || (fl & 0x400)) {
        hr = cpo_transition(i, st ? fromseg : NULL, to, st ? mt - start : 0, cmd, fl, cm, trans);
    } else {
        DMUS_TEMPO_PARAM tpp;
        double tempo = 120.0;
        const double *tp = NULL;
        if (SUCCEEDED(IDirectMusicPerformance8_GetParam(pf, &GUID_TempoParam, 0xffffffff, DMUS_SEG_ANYTRACK, mt, NULL, &tpp))) { tempo = tpp.dblTempo; tp = &tempo; }
        DMUS_BAND_PARAM bp;
        IDirectMusicBand *band = NULL;
        if (SUCCEEDED(IDirectMusicPerformance8_GetParam(pf, &GUID_BandParam, 0xffffffff, DMUS_SEG_ANYTRACK, mt, NULL, &bp))) band = bp.pBand;
        dmc_cmdparam cp = { c2.mtTime, c2.bCommand, c2.bGrooveLevel, c2.bGrooveRange, c2.bRepeatMode };
        int side = 0, r;
        dm_band *bx = band ? band_hold_of(band) : NULL;
        dm_engine_lock();
        dm_segment *g = dmc_compose_transition(sx, bx, tp, &cp, &fk, &tk, tob ? tob->seg : NULL, cmd, fl, mx, &side, &r);
        dm_engine_unlock();
        dm_band_release(bx);
        if (band) IDirectMusicBand_Release(band);
        *trans = NULL;
        hr = r;
        if (g) {
            rm_segment *o = transition_segment(g, side, fo, tob);
            if (!o) { dm_segment_release(g); hr = E_OUTOFMEMORY; }
            else {
                HRESULT q = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)trans);
                seg_release(&o->IDirectMusicSegment8_iface);
                if (FAILED(q)) hr = q;
            }
        }
    }
    dms_style_release(sx);
    if (SUCCEEDED(hr)) {
        if (*trans && to) {
            DMUS_COMMAND_PARAM_2 nx;
            if (SUCCEEDED(IDirectMusicSegment_GetParam(to, &GUID_CommandParam2, 0xffffffff, DMUS_SEG_ANYTRACK, 0, NULL, &nx)))
                IDirectMusicSegment_SetParam(*trans, &GUID_CommandParamNext, 0xffffffff, 0, 0, &nx);
            DWORD res;
            if (SUCCEEDED(IDirectMusicSegment_GetDefaultResolution(to, &res))) IDirectMusicSegment_SetDefaultResolution(*trans, res);
        }
        if (fl & 0x400) segf |= 0x800000;
        if (fl & 0x800) segf |= 0x1000000;
        if (!*trans) {
            if (!to) { if (!(fl & 0x400)) hr = IDirectMusicPerformance8_Stop(pf, fromseg, NULL, 0, segf); }
            else hr = IDirectMusicPerformance8_PlaySegment(pf, to, segf, 0, tostate);
        } else {
            hr = IDirectMusicPerformance8_PlaySegment(pf, *trans, segf, 0, trstate);
            if (SUCCEEDED(hr) && to) hr = IDirectMusicPerformance8_PlaySegment(pf, to, queuef | 0x100, 0, tostate);
        }
    }
    LOG_CALL("music: Composer::AutoTransition(cmd %u, flags %lx, trans %p) -> %08lx", cmd, (unsigned long)fl, (void *)*trans, (unsigned long)hr);
    if (style) IDirectMusicStyle_Release(style);
    if (mytrans) IDirectMusicSegment_Release(mytrans);
    if (st) IDirectMusicSegmentState_Release(st);
    if (fromseg) IDirectMusicSegment_Release(fromseg);
    if (owncm) dmc_chordmap_release(mx);
    (void)mq;
    return hr;
}
/* 77286b27 / 77286629: intro, fill and break lengths of 1 */
static HRESULT WINAPI cpo_templatefromshape(IDirectMusicComposer *i, WORD n, WORD shape, BOOL intro, BOOL end, WORD endlen,
                                            IDirectMusicSegment **out)
{
    if (IsBadWritePtr(out, 4)) return E_POINTER;
    dm_engine_lock();
    int r;
    dm_segment *g = dmc_template_from_shape(n, shape, intro, end, 1, 1, 1, endlen, &r);
    dm_engine_unlock();
    LOG_CALL("music: Composer::ComposeTemplateFromShape(%u measures, shape %u, intro %d, end %d, end length %u) -> %08x",
             n, shape, intro, end, endlen, r);
    if (!g) return r;
    rm_segment *o = new_segment(g);
    if (!o) { dm_segment_release(g); return E_OUTOFMEMORY; }
    HRESULT q = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)out);
    seg_release(&o->IDirectMusicSegment8_iface);
    return FAILED(q) ? q : r;
}
/* 77284b30 */
static HRESULT WINAPI cpo_changechordmap(IDirectMusicComposer *i, IDirectMusicSegment *s, BOOL trackscale, IDirectMusicChordMap *cm)
{
    if (IsBadWritePtr(s, 4) || IsBadWritePtr(cm, 4)) return E_POINTER;
    rm_segment *t = unsafe_segment((IUnknown *)s);
    dmc_chordmap *mx = chordmap_engine_of(cm);
    if (!t || !mx) return rm_notimpl("Composer::ChangeChordMap (a segment or chord map not made by reDM)");
    if (!t->seg) return DMUS_E_NOT_FOUND;
    dm_engine_lock();
    int r = dmc_change_chordmap(t->seg, trackscale, mx);
    dm_engine_unlock();
    LOG_CALL("music: Composer::ChangeChordMap(track scale %d) -> %08x", trackscale, r);
    return r;
}
static IDirectMusicComposerVtbl cpo_vtbl = { cpo_qi, cpo_addref, cpo_release, cpo_fromtemplate, cpo_fromshape, cpo_transition,
    cpo_autotransition, cpo_templatefromshape, cpo_changechordmap };
/* 77282d2f: the constructor seeds the application thread's rand() with time() */
static HRESULT composer_create(REFIID riid, void **out)
{
    rm_composer *o = calloc(1, sizeof(*o));
    if (!o) return E_OUTOFMEMORY;
    o->IDirectMusicComposer_iface.lpVtbl = &cpo_vtbl;
    o->ref = 1;
    dm_style_srand();
    dmc_set_fixes(!GetEnvironmentVariableA("REDM_NOFIXES", NULL, 0));
    HRESULT hr = cpo_qi(&o->IDirectMusicComposer_iface, riid, out);
    cpo_release(&o->IDirectMusicComposer_iface);
    return hr;
}
