/* reAL-Music: the chord map object (dmcompos CDMChordMap 77288f66), included by dmcom.c.
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
