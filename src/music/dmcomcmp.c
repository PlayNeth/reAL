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
        if (o->m && o->m->wrapper == o) o->m->wrapper = NULL;
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
    return dmc_chordmap_load(m, b + 12, end - 12);
}
static HRESULT WINAPI cms_load(IPersistStream *i, IStream *st)
{
    rm_chordmap *o = cms(i);
    BYTE *b; DWORD n;
    if (!st) return E_INVALIDARG;
    if (!read_stream(st, &b, &n)) return DMUS_E_CHUNKNOTFOUND;
    HRESULT hr = chordmap_load_data(o->m, b, n);
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
    if (!o->m->wrapper) o->m->wrapper = o;
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
    rm_chordmap *o = m->wrapper;
    if (o) { cmp_addref(&o->IDirectMusicChordMap_iface); dmc_chordmap_release(m); return &o->IDirectMusicChordMap_iface; }
    o = new_chordmap(m);
    if (!o) { dmc_chordmap_release(m); return NULL; }
    return &o->IDirectMusicChordMap_iface;
}

/* a chord map track's reference (7728bdd6: the loader's GetObject of a chord map); the engine map with a reference */
static HRESULT WINAPI ld_getobject(IDirectMusicLoader8 *i, LPDMUS_OBJECTDESC d, REFIID riid, LPVOID *out);
static struct dmc_chordmap *loader_getcmap(void *ctx, const dm_guid *g, const char *file, const char *name)
{
    rm_loader *l = ctx;
    if (!l) return NULL;
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS;
    d.guidClass = RM_CLSID_DirectMusicChordMap;
    if (g) { memcpy(&d.guidObject, g, sizeof(GUID)); d.dwValidData |= DMUS_OBJ_OBJECT; }
    if (file) {
        MultiByteToWideChar(CP_ACP, 0, file, -1, d.wszFileName, DMUS_MAX_FILENAME);
        d.dwValidData |= DMUS_OBJ_FILENAME;
        if (file[0] && (file[1] == ':' || file[0] == '\\')) d.dwValidData |= DMUS_OBJ_FULLPATH;
    }
    if (name) { MultiByteToWideChar(CP_ACP, 0, name, -1, d.wszName, DMUS_MAX_NAME); d.dwValidData |= DMUS_OBJ_NAME; }
    IDirectMusicChordMap *o = NULL;
    if (FAILED(ld_getobject(&l->IDirectMusicLoader8_iface, &d, &RM_IID_IDirectMusicChordMap, (void **)&o)) || !o) {
        LOG_INFO("music: chord map %s not found", file ? file : name ? name : "(by guid)");
        return NULL;
    }
    dmc_chordmap *r = chordmap_engine_of(o);
    dmc_chordmap_addref(r);
    IDirectMusicChordMap_Release(o);
    return r;
}

/* the loader's GetObject of a chord map from file, memory or stream data */
static HRESULT chordmap_from_data(const BYTE *b, DWORD n, IUnknown **out)
{
    rm_chordmap *o = new_chordmap(NULL);
    if (!o) return E_OUTOFMEMORY;
    HRESULT hr = chordmap_load_data(o->m, b, n);
    if (FAILED(hr)) { cmp_release(&o->IDirectMusicChordMap_iface); return hr; }
    *out = (IUnknown *)&o->IDirectMusicChordMap_iface;
    return S_OK;
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
static struct dms_style *style_engine_of(void *p);
/* 772821f3 / 77287974 (flags 3): a new segment from the template */
static HRESULT WINAPI cpo_fromtemplate(IDirectMusicComposer *i, IDirectMusicStyle *style, IDirectMusicSegment *tmpl,
                                       WORD activity, IDirectMusicChordMap *cm, IDirectMusicSegment **out)
{
    if ((style && IsBadWritePtr(style, 4)) || (cm && IsBadWritePtr(cm, 4)) || IsBadWritePtr(tmpl, 4) ||
        (out && IsBadWritePtr(out, 4)) || !out)
        return E_POINTER;
    rm_segment *t = unsafe_segment((IUnknown *)tmpl);
    if (!t || !t->seg) return E_INVALIDARG;
    struct dms_style *sx = style ? style_engine_of(style) : NULL;
    dmc_chordmap *mx = cm ? chordmap_engine_of(cm) : NULL;
    if ((style && !sx) || (cm && !mx)) return rm_notimpl("Composer: a style or chord map not made by reDM");
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    int hr;
    dm_segment *g = dmc_compose_template(t->seg, sx, mx, activity, &hr);
    if (h) dm_host_unlock(h);
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
    struct dms_style *sx = style_engine_of(s);
    dmc_chordmap *mx = chordmap_engine_of(cm);
    if (!sx || !mx) return rm_notimpl("Composer: a style or chord map not made by reDM");
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
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    int r;
    dm_segment *g = dmc_segment_from_shape(sx, mx, (unsigned)m, shape, act, intro, end, introlen, fill, brk, endlen, &r);
    if (h) dm_host_unlock(h);
    LOG_CALL("music: Composer::ComposeSegmentFromShape(%u measures, shape %u, activity %u, intro %d, end %d) -> %08x",
             n, shape, act, intro, end, r);
    if (!g) return r < 0 ? r : E_FAIL;
    rm_segment *o = new_segment(g);
    if (!o) { dm_segment_release(g); return E_OUTOFMEMORY; }
    HRESULT q = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)out);
    seg_release(&o->IDirectMusicSegment8_iface);
    return FAILED(q) ? q : r;
}
static HRESULT WINAPI cpo_transition(IDirectMusicComposer *i, IDirectMusicSegment *from, IDirectMusicSegment *to, MUSIC_TIME t,
                                     WORD cmd, DWORD fl, IDirectMusicChordMap *cm, IDirectMusicSegment **out)
{ return rm_notimpl("Composer::ComposeTransition"); }
static HRESULT WINAPI cpo_autotransition(IDirectMusicComposer *i, IDirectMusicPerformance *p, IDirectMusicSegment *to, WORD cmd,
                                         DWORD fl, IDirectMusicChordMap *cm, IDirectMusicSegment **out,
                                         IDirectMusicSegmentState **tostate, IDirectMusicSegmentState **trstate)
{ return rm_notimpl("Composer::AutoTransition"); }
/* 77286b27 / 77286629: intro, fill and break lengths of 1 */
static HRESULT WINAPI cpo_templatefromshape(IDirectMusicComposer *i, WORD n, WORD shape, BOOL intro, BOOL end, WORD endlen,
                                            IDirectMusicSegment **out)
{
    if (IsBadWritePtr(out, 4)) return E_POINTER;
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    int r;
    dm_segment *g = dmc_template_from_shape(n, shape, intro, end, 1, 1, 1, endlen, &r);
    if (h) dm_host_unlock(h);
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
    dm_host *h = any_host();
    if (h) dm_host_lock(h);
    int r = dmc_change_chordmap(t->seg, trackscale, mx);
    if (h) dm_host_unlock(h);
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
