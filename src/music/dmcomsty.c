/* reDM: the style and band objects (dmstyle CDMStyle 7740bd9d, dmband CBand), included by dmcom.c.
 * IDirectMusicStyle8 / IDirectMusicBand on dms_style / dm_band (dmstyload.c, dmseg.c). */
#include "dmsty.h"

DMG(RM_CLSID_DirectMusicStyle, 0xd2ac288a, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(RM_CLSID_DirectMusicBand, 0x79ba9e00, 0xb6ee, 0x11d1, 0x86, 0xbe, 0x00, 0xc0, 0x4f, 0xbf, 0x8f, 0xef);
DMG(RM_IID_IDirectMusicStyle, 0xd2ac28bd, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
DMG(RM_IID_IDirectMusicStyle8, 0xfd24ad8a, 0xa260, 0x453d, 0xbf, 0x50, 0x6f, 0x93, 0x84, 0xf7, 0x09, 0x85);
DMG(RM_IID_IDirectMusicBand, 0xd2ac28c0, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);

/* ------------------------------------------------------------------ bands */
typedef struct rm_band {
    IDirectMusicBand IDirectMusicBand_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dm_band *b;
    DMUS_OBJECTDESC desc;
} rm_band;
static rm_band *bnd(IDirectMusicBand *i) { return impl_from(i, rm_band, IDirectMusicBand_iface); }
static rm_band *bno(IDirectMusicObject *i) { return impl_from(i, rm_band, IDirectMusicObject_iface); }
static rm_band *bnp(IPersistStream *i) { return impl_from(i, rm_band, IPersistStream_iface); }
static IDirectMusicBandVtbl bnd_vtbl;
static IDirectMusicObjectVtbl bno_vtbl;
static IPersistStreamVtbl bnp_vtbl;

static HRESULT WINAPI bnd_qi(IDirectMusicBand *i, REFIID riid, void **out)
{
    rm_band *b = bnd(i);
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &RM_IID_IDirectMusicBand)) *out = &b->IDirectMusicBand_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &b->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &b->IPersistStream_iface;
    else return rm_noiface("Band::QueryInterface", riid, out);
    InterlockedIncrement(&b->ref);
    return S_OK;
}
static ULONG WINAPI bnd_addref(IDirectMusicBand *i) { return InterlockedIncrement(&bnd(i)->ref); }
static ULONG WINAPI bnd_release(IDirectMusicBand *i)
{
    rm_band *b = bnd(i);
    LONG r = InterlockedDecrement(&b->ref);
    if (!r) { dm_band_release(b->b); free(b); }
    return r;
}
/* 7720322d */
static HRESULT WINAPI bnd_createsegment(IDirectMusicBand *i, IDirectMusicSegment **out)
{
    rm_band *b = bnd(i);
    if (IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    *out = NULL;
    dm_segment *g = dm_band_segment(b->b);
    if (!g) return E_OUTOFMEMORY;
    rm_segment *s = new_segment(g);
    if (!s) { dm_segment_release(g); return E_OUTOFMEMORY; }
    HRESULT hr = seg_qi(&s->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)out);
    seg_release(&s->IDirectMusicSegment8_iface);
    LOG_CALL("music: Band::CreateSegment -> %08lx", (unsigned long)hr);
    return hr;
}
/* 772032ef / 7720331a: Download / Unload to a performance */
static HRESULT band_dl(rm_band *b, IUnknown *target, int unload)
{
    if (!target) return E_POINTER;
    dm_dltarget dt; void *data; dm_host *h;
    HRESULT hr = prm_data(unload ? PRM_UNLOAD_AP : PRM_DOWNLOAD_AP, target, &dt, &data, &h);
    if (FAILED(hr)) return hr;
    if (!b->b) return S_OK;
    if (h) dm_host_lock(h);
    if (unload) dm_band_unload_from(dt.pf, b->b);
    else hr = dm_band_download_to(dt.pf, b->b);
    if (h) dm_host_unlock(h);
    return hr;
}
static HRESULT WINAPI bnd_download(IDirectMusicBand *i, IDirectMusicPerformance *p) { return band_dl(bnd(i), (IUnknown *)p, 0); }
static HRESULT WINAPI bnd_unload(IDirectMusicBand *i, IDirectMusicPerformance *p) { return band_dl(bnd(i), (IUnknown *)p, 1); }
static IDirectMusicBandVtbl bnd_vtbl = { bnd_qi, bnd_addref, bnd_release, bnd_createsegment, bnd_download, bnd_unload };

static HRESULT WINAPI bno_qi(IDirectMusicObject *i, REFIID r, void **o) { return bnd_qi(&bno(i)->IDirectMusicBand_iface, r, o); }
static ULONG WINAPI bno_addref(IDirectMusicObject *i) { return bnd_addref(&bno(i)->IDirectMusicBand_iface); }
static ULONG WINAPI bno_release(IDirectMusicObject *i) { return bnd_release(&bno(i)->IDirectMusicBand_iface); }
static HRESULT WINAPI bno_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    if (!d) return E_POINTER;
    DWORD sz = d->dwSize;
    memcpy(d, &bno(i)->desc, sz < sizeof(*d) ? sz : sizeof(*d));
    d->dwSize = sz;
    return S_OK;
}
static HRESULT WINAPI bno_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    rm_band *b = bno(i);
    if (!d) return E_POINTER;
    if (d->dwValidData & DMUS_OBJ_OBJECT) b->desc.guidObject = d->guidObject;
    if (d->dwValidData & DMUS_OBJ_NAME) memcpy(b->desc.wszName, d->wszName, sizeof(d->wszName));
    b->desc.dwValidData |= d->dwValidData & (DMUS_OBJ_OBJECT | DMUS_OBJ_NAME);
    return S_OK;
}
static HRESULT WINAPI bno_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d) { return rm_desc_parse(s, d, mmioFOURCC('D','M','B','D'), &RM_CLSID_DirectMusicBand); }
static IDirectMusicObjectVtbl bno_vtbl = { bno_qi, bno_addref, bno_release, bno_getdesc, bno_setdesc, bno_parse };

static void band_desc(rm_band *o)
{
    if (o->b->hasguid) { memcpy(&o->desc.guidObject, &o->b->guid, sizeof(GUID)); o->desc.dwValidData |= DMUS_OBJ_OBJECT; }
    if (o->b->name[0]) { memcpy(o->desc.wszName, o->b->name, sizeof(o->b->name)); o->desc.dwValidData |= DMUS_OBJ_NAME; }
}
static HRESULT WINAPI bnp_qi(IPersistStream *i, REFIID r, void **o) { return bnd_qi(&bnp(i)->IDirectMusicBand_iface, r, o); }
static ULONG WINAPI bnp_addref(IPersistStream *i) { return bnd_addref(&bnp(i)->IDirectMusicBand_iface); }
static ULONG WINAPI bnp_release(IPersistStream *i) { return bnd_release(&bnp(i)->IDirectMusicBand_iface); }
static HRESULT WINAPI bnp_getclass(IPersistStream *i, CLSID *c) { return E_NOTIMPL; }       /* (dmband's) */
static HRESULT WINAPI bnp_isdirty(IPersistStream *i) { return S_FALSE; }
static dm_collection *loader_getcoll(void *ctx, const dm_guid *g, const char *file);
static HRESULT WINAPI bnp_load(IPersistStream *i, IStream *st)
{
    rm_band *o = bnp(i);
    BYTE *b; DWORD n;
    if (!st) return E_POINTER;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    dm_band *x = NULL;
    if (n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "DMBD", 4)) x = dm_band_load(b + 12, n - 12, loader_getcoll, rm_stream_loader(st));
    free(b);
    if (!x) return DMUS_E_UNSUPPORTED_STREAM;
    dm_band_release(o->b);
    o->b = x;
    band_desc(o);
    return S_OK;
}
static HRESULT WINAPI bnp_save(IPersistStream *i, IStream *s, BOOL c) { return rm_notimpl(__func__); }
static HRESULT WINAPI bnp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl bnp_vtbl = { bnp_qi, bnp_addref, bnp_release, bnp_getclass, bnp_isdirty, bnp_load, bnp_save, bnp_getsize };

/* a band object for a band (a reference taken) */
static rm_band *new_band(dm_band *x)
{
    rm_band *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicBand_iface.lpVtbl = &bnd_vtbl;
    o->IDirectMusicObject_iface.lpVtbl = &bno_vtbl;
    o->IPersistStream_iface.lpVtbl = &bnp_vtbl;
    o->ref = 1;
    o->desc.dwSize = sizeof(o->desc);
    o->desc.dwValidData = DMUS_OBJ_CLASS;
    o->desc.guidClass = RM_CLSID_DirectMusicBand;
    if (x) { dm_band_addref(x); o->b = x; band_desc(o); }
    return o;
}

/* ------------------------------------------------------------------ styles */
typedef struct rm_style {
    IDirectMusicStyle8 IDirectMusicStyle8_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    dms_style *s;
    rm_band **bands; int nbands;        /* +0x78 (the style's order) */
    rm_band *defband;                   /* +0x7c */
    IDirectMusicChordMap **cmaps; int ncmaps;   /* +0x84 (newest first) */
    IDirectMusicChordMap *defcmap;      /* +0x88 (the list's reference) */
    DMUS_OBJECTDESC desc;
} rm_style;
static rm_style *sty(IDirectMusicStyle8 *i) { return impl_from(i, rm_style, IDirectMusicStyle8_iface); }
static rm_style *sto(IDirectMusicObject *i) { return impl_from(i, rm_style, IDirectMusicObject_iface); }
static rm_style *stp(IPersistStream *i) { return impl_from(i, rm_style, IPersistStream_iface); }
static IDirectMusicStyle8Vtbl sty_vtbl;
static IDirectMusicObjectVtbl sto_vtbl;
static IPersistStreamVtbl stp_vtbl;

static HRESULT WINAPI sty_qi(IDirectMusicStyle8 *i, REFIID riid, void **out)
{
    rm_style *s = sty(i);
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &RM_IID_IDirectMusicStyle) ||
        IsEqualIID(riid, &RM_IID_IDirectMusicStyle8)) *out = &s->IDirectMusicStyle8_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &s->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &s->IPersistStream_iface;
    else return rm_noiface("Style::QueryInterface", riid, out);
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG WINAPI sty_addref(IDirectMusicStyle8 *i) { return InterlockedIncrement(&sty(i)->ref); }
static void style_drop(rm_style *s)
{
    for (int k = 0; k < s->nbands; k++) bnd_release(&s->bands[k]->IDirectMusicBand_iface);
    free(s->bands); s->bands = NULL; s->nbands = 0;
    if (s->defband) bnd_release(&s->defband->IDirectMusicBand_iface);
    s->defband = NULL;
    if (s->s) { if (s->s->wrapper == s) s->s->wrapper = NULL; dms_style_release(s->s); }
    s->s = NULL;
    for (int k = 0; k < s->ncmaps; k++) IDirectMusicChordMap_Release(s->cmaps[k]);
    free(s->cmaps); s->cmaps = NULL; s->ncmaps = 0;
    s->defcmap = NULL;
}
static ULONG WINAPI sty_release(IDirectMusicStyle8 *i)
{
    rm_style *s = sty(i);
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) { style_drop(s); free(s); }
    return r;
}
static int name_eq(const WCHAR *a, const uint16_t *b)
{
    WCHAR w[64];
    int k;
    for (k = 0; k < 63 && b[k]; k++) w[k] = b[k];
    w[k] = 0;
    return !lstrcmpiW(a, w);
}
static void name_out(WCHAR *o, const uint16_t *n)
{
    int k;
    for (k = 0; k < 0x103 && n[k]; k++) o[k] = n[k];
    o[k] = 0;
}
/* 774083a1: the band of that name (not found: S_FALSE) */
static HRESULT WINAPI sty_getband(IDirectMusicStyle8 *i, WCHAR *name, IDirectMusicBand **out)
{
    rm_style *s = sty(i);
    if (IsBadReadPtr(name, 4) || IsBadWritePtr(out, 4)) return E_POINTER;
    for (int k = 0; k < s->nbands; k++) {
        rm_band *b = s->bands[k];
        if (!(b->desc.dwValidData & DMUS_OBJ_NAME)) return E_FAIL;
        if (name_eq(name, b->b->name)) { bnd_addref(&b->IDirectMusicBand_iface); *out = &b->IDirectMusicBand_iface; return S_OK; }
    }
    return S_FALSE;
}
/* 774084d2: the name of the index-th band */
static HRESULT WINAPI sty_enumband(IDirectMusicStyle8 *i, DWORD n, WCHAR *name)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(name, 0x104)) return E_POINTER;
    if (n >= (DWORD)s->nbands) return S_FALSE;
    rm_band *b = s->bands[n];
    if (b->desc.dwValidData & DMUS_OBJ_NAME) name_out(name, b->b->name);
    return S_OK;
}
/* 77406dc2 */
static HRESULT WINAPI sty_getdefaultband(IDirectMusicStyle8 *i, IDirectMusicBand **out)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(out, 4)) return E_POINTER;
    if (!s->defband) return S_FALSE;
    bnd_addref(&s->defband->IDirectMusicBand_iface);
    *out = &s->defband->IDirectMusicBand_iface;
    return S_OK;
}
/* 774080f2: the index-th motif's name (DMUS_S_STRING_TRUNCATED past 0x103 characters) */
static HRESULT WINAPI sty_enummotif(IDirectMusicStyle8 *i, DWORD n, WCHAR *name)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(name, 0x104)) return E_POINTER;
    if (!s->s || n >= (DWORD)s->s->nmotifs) return S_FALSE;
    name_out(name, s->s->motifs[n]->name);
    return S_OK;
}
/* 77409919: a segment playing the motif of that name (not found: S_FALSE) */
static HRESULT WINAPI sty_getmotif(IDirectMusicStyle8 *i, WCHAR *name, IDirectMusicSegment **out)
{
    rm_style *s = sty(i);
    if (IsBadReadPtr(name, 4) || IsBadWritePtr(out, 4)) return E_POINTER;
    if (!s->s) return S_FALSE;
    for (int k = 0; k < s->s->nmotifs; k++) {
        if (!name_eq(name, s->s->motifs[k]->name)) continue;
        dm_host *h = any_host();
        if (h) dm_host_lock(h);
        dm_segment *g = dms_motif_segment(s->s, s->s->motifs[k]);
        if (h) dm_host_unlock(h);
        if (!g) return E_OUTOFMEMORY;
        rm_segment *o = new_segment(g);
        if (!o) { dm_segment_release(g); return E_OUTOFMEMORY; }
        HRESULT hr = seg_qi(&o->IDirectMusicSegment8_iface, &IID_IDirectMusicSegment, (void **)out);
        seg_release(&o->IDirectMusicSegment8_iface);
        return hr;
    }
    return S_FALSE;
}
/* a chord map's descriptor name (FALSE when it has none or the calls fail; *hr their result) */
static BOOL cmap_name(IDirectMusicChordMap *m, WCHAR *out, HRESULT *hr)
{
    IDirectMusicObject *o = NULL;
    *hr = IDirectMusicChordMap_QueryInterface(m, &IID_IDirectMusicObject, (void **)&o);
    if (FAILED(*hr)) return FALSE;
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    *hr = IDirectMusicObject_GetDescriptor(o, &d);
    IDirectMusicObject_Release(o);
    if (FAILED(*hr)) return FALSE;
    if (!(d.dwValidData & DMUS_OBJ_NAME)) { *hr = E_FAIL; return FALSE; }
    lstrcpynW(out, d.wszName, DMUS_MAX_NAME);
    return TRUE;
}
/* 77406d8a */
static HRESULT WINAPI sty_getdefaultchordmap(IDirectMusicStyle8 *i, IDirectMusicChordMap **out)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(out, 4)) return E_POINTER;
    if (!s->defcmap) return S_FALSE;
    *out = s->defcmap;
    IDirectMusicChordMap_AddRef(s->defcmap);
    return S_OK;
}
/* 774082a0: the index-th chord map's name (no name: DMUS_E_TYPE_UNSUPPORTED; past 0x103 characters cut, with
 * DMUS_S_STRING_TRUNCATED) */
static HRESULT WINAPI sty_enumchordmap(IDirectMusicStyle8 *i, DWORD n, WCHAR *name)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(name, 0x104 * sizeof(WCHAR) / 2)) return E_POINTER;
    if (n >= (DWORD)s->ncmaps) return S_FALSE;
    WCHAR w[DMUS_MAX_NAME];
    HRESULT hr;
    if (!cmap_name(s->cmaps[n], w, &hr)) return hr == E_FAIL ? DMUS_E_TYPE_UNSUPPORTED : hr;
    lstrcpyW(name, w);
    return S_OK;
}
/* 77408176: the chord map of that name (case-insensitive; not found: S_FALSE; one without a name: E_FAIL) */
static HRESULT WINAPI sty_getchordmap(IDirectMusicStyle8 *i, WCHAR *name, IDirectMusicChordMap **out)
{
    rm_style *s = sty(i);
    if (IsBadReadPtr(name, 4) || IsBadWritePtr(out, 4)) return E_POINTER;
    for (int k = 0; k < s->ncmaps; k++) {
        WCHAR w[DMUS_MAX_NAME];
        HRESULT hr;
        if (!cmap_name(s->cmaps[k], w, &hr)) return hr;
        if (lstrcmpiW(w, name)) continue;
        IDirectMusicChordMap_AddRef(s->cmaps[k]);
        *out = s->cmaps[k];
        return S_OK;
    }
    return S_FALSE;
}
/* 7740af13 / 7740a17d / 7740a031: the style's 'prrf' list: each named reference's chord map from the loader, put
 * first unless one with its name (the map's own) is there already; the first reference's is the default */
DMG(RM_IID_IDirectMusicChordMap_, 0xd2ac28be, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static void style_add_cmap(rm_style *o, IDirectMusicChordMap *m, const WCHAR *refname, int first)
{
    for (int k = 0; k < o->ncmaps; k++) {
        WCHAR w[DMUS_MAX_NAME];
        HRESULT hr;
        if (!cmap_name(o->cmaps[k], w, &hr)) { if (FAILED(hr)) { IDirectMusicChordMap_Release(m); return; } continue; }
        if (!lstrcmpiW(w, refname)) { IDirectMusicChordMap_Release(m); return; }
    }
    IDirectMusicChordMap **n = realloc(o->cmaps, (o->ncmaps + 1) * sizeof(*n));
    if (!n) { IDirectMusicChordMap_Release(m); return; }
    o->cmaps = n;
    memmove(n + 1, n, o->ncmaps * sizeof(*n));
    n[0] = m;
    o->ncmaps++;
    if (first) o->defcmap = m;
}
static void style_load_cmaps(rm_style *o, const BYTE *b, DWORD n, IDirectMusicLoader8 *l)
{
    if (!l || n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DMST", 4)) return;
    DWORD end = 8 + *(const DWORD *)(b + 4);
    if (end > n) end = n;
    for (DWORD off = 12; off + 8 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4), dend = off + 8 + sz;
        if (dend > end) dend = end;
        if (!memcmp(b + off, "LIST", 4) && sz >= 4 && !memcmp(b + off + 8, "prrf", 4)) {
            int first = 1;
            for (DWORD q = off + 12; q + 8 <= dend; ) {
                DWORD qs = *(const DWORD *)(b + q + 4), qend = q + 8 + qs;
                if (qend > dend) qend = dend;
                if (!memcmp(b + q, "LIST", 4) && qs >= 4 && !memcmp(b + q + 8, "DMRF", 4)) {
                    DMUS_OBJECTDESC d;
                    memset(&d, 0, sizeof(d));
                    BOOL bad = FALSE;
                    for (DWORD r = q + 12; r + 8 <= qend; ) {
                        DWORD rs = *(const DWORD *)(b + r + 4);
                        const BYTE *rd = b + r + 8;
                        DWORD avail = qend - (r + 8) < rs ? qend - (r + 8) : rs;
                        if (!memcmp(b + r, "refh", 4)) {
                            if (avail != 20) bad = TRUE;
                            else { memcpy(&d.guidClass, rd, 16); d.dwValidData |= *(const DWORD *)(rd + 16) | DMUS_OBJ_CLASS; }
                        } else if (!memcmp(b + r, "guid", 4)) {
                            if (avail != 16) bad = TRUE; else { memcpy(&d.guidObject, rd, 16); d.dwValidData |= DMUS_OBJ_OBJECT; }
                        } else if (!memcmp(b + r, "name", 4)) {
                            DWORD k = avail < 0x80 ? avail : 0x80;
                            memcpy(d.wszName, rd, k); d.wszName[DMUS_MAX_NAME - 1] = 0; d.dwValidData |= DMUS_OBJ_NAME;
                        } else if (!memcmp(b + r, "file", 4)) {
                            DWORD k = avail < 0x208 ? avail : 0x208;
                            memcpy(d.wszFileName, rd, k); d.wszFileName[DMUS_MAX_FILENAME - 1] = 0; d.dwValidData |= DMUS_OBJ_FILENAME;
                        } else if (!memcmp(b + r, "catg", 4)) {
                            DWORD k = avail < 0x80 ? avail : 0x80;
                            memcpy(d.wszCategory, rd, k); d.wszCategory[DMUS_MAX_CATEGORY - 1] = 0; d.dwValidData |= DMUS_OBJ_CATEGORY;
                        } else if (!memcmp(b + r, "vers", 4)) {
                            if (avail != 8) bad = TRUE; else { memcpy(&d.vVersion, rd, 8); d.dwValidData |= DMUS_OBJ_VERSION; }
                        } else if (!memcmp(b + r, "date", 4)) {
                            if (avail != 8) bad = TRUE; else { memcpy(&d.ftDate, rd, 8); d.dwValidData |= DMUS_OBJ_DATE; }
                        }
                        r += 8 + rs + (rs & 1);
                    }
                    if (!bad && (d.dwValidData & DMUS_OBJ_NAME)) {
                        d.dwSize = sizeof(d);
                        IDirectMusicChordMap *m = NULL;
                        if (SUCCEEDED(IDirectMusicLoader8_GetObject(l, &d, &RM_IID_IDirectMusicChordMap_, (void **)&m)) && m)
                            style_add_cmap(o, m, d.wszName, first);
                    }
                    first = 0;
                }
                q += 8 + qs + (qs & 1);
            }
        }
        off += 8 + sz + (sz & 1);
    }
}
/* 77406df7 */
static HRESULT WINAPI sty_gettimesig(IDirectMusicStyle8 *i, DMUS_TIMESIGNATURE *ts)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(ts, 4)) return E_POINTER;
    const uint8_t *t = s->s ? s->s->ts : (const uint8_t *)"\0\0\0\0";
    ts->mtTime = 0; ts->wGridsPerBeat = (WORD)(t[2] | t[3] << 8); ts->bBeatsPerMeasure = t[0]; ts->bBeat = t[1];
    return S_OK;
}
/* 774085da: the shortest and longest pattern of an embellishment (groove level for DMUS_EMBELLISHT_NORMAL);
 * S_FALSE when there is none */
static HRESULT WINAPI sty_getemblength(IDirectMusicStyle8 *i, DWORD type, DWORD level, DWORD *mn, DWORD *mx)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(mn, 4) || IsBadWritePtr(mx, 4)) return E_POINTER;
    *mx = 0; *mn = 0;
    if (!s->s || !s->s->npatterns) return S_FALSE;
    for (int k = 0; k < s->s->npatterns; k++) {
        const dms_pattern *p = s->s->patterns[k];
        int ok;
        switch (type) {
        case 0: ok = p->embellish == 0 && p->groovelo <= (uint8_t)level && (uint8_t)level <= p->groovehi; break;
        case 1: ok = p->embellish == 1; break;
        case 2: ok = p->embellish == 4; break;
        case 3: ok = p->embellish == 2; break;
        case 4: ok = p->embellish == 8; break;
        default: ok = (p->embellish >> 8) && (uint16_t)(p->embellish >> 8) == (uint16_t)type; break;
        }
        if (!ok) continue;
        if (*mx < p->measures) *mx = p->measures;
        if (p->measures < *mn || *mn == 0) *mn = p->measures;
    }
    return *mn && *mx ? S_OK : S_FALSE;
}
/* 77406e31 */
static HRESULT WINAPI sty_gettempo(IDirectMusicStyle8 *i, double *t)
{
    rm_style *s = sty(i);
    if (IsBadWritePtr(t, 4)) return E_POINTER;
    *t = s->s ? s->s->tempo : 0;
    return S_OK;
}
/* 774099d9: patterns (0) or motifs (1) */
static HRESULT WINAPI sty_enumpattern(IDirectMusicStyle8 *i, DWORD n, DWORD type, WCHAR *name)
{
    rm_style *s = sty(i);
    if (type == 1) return sty_enummotif(i, n, name);
    if (type) return E_INVALIDARG;
    if (IsBadWritePtr(name, 0x104)) return E_POINTER;
    if (!s->s || n >= (DWORD)s->s->npatterns) return S_FALSE;
    name_out(name, s->s->patterns[n]->name);
    return S_OK;
}
static IDirectMusicStyle8Vtbl sty_vtbl = { sty_qi, sty_addref, sty_release, sty_getband, sty_enumband, sty_getdefaultband,
    sty_enummotif, sty_getmotif, sty_getdefaultchordmap, sty_enumchordmap, sty_getchordmap, sty_gettimesig,
    sty_getemblength, sty_gettempo, sty_enumpattern };

static HRESULT WINAPI sto_qi(IDirectMusicObject *i, REFIID r, void **o) { return sty_qi(&sto(i)->IDirectMusicStyle8_iface, r, o); }
static ULONG WINAPI sto_addref(IDirectMusicObject *i) { return sty_addref(&sto(i)->IDirectMusicStyle8_iface); }
static ULONG WINAPI sto_release(IDirectMusicObject *i) { return sty_release(&sto(i)->IDirectMusicStyle8_iface); }
static HRESULT WINAPI sto_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    if (!d) return E_POINTER;
    DWORD sz = d->dwSize;
    memcpy(d, &sto(i)->desc, sz < sizeof(*d) ? sz : sizeof(*d));
    d->dwSize = sz;
    return S_OK;
}
static HRESULT WINAPI sto_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    rm_style *s = sto(i);
    if (!d) return E_POINTER;
    if (d->dwValidData & DMUS_OBJ_OBJECT) s->desc.guidObject = d->guidObject;
    if (d->dwValidData & DMUS_OBJ_NAME) memcpy(s->desc.wszName, d->wszName, sizeof(d->wszName));
    s->desc.dwValidData |= d->dwValidData & (DMUS_OBJ_OBJECT | DMUS_OBJ_NAME);
    return S_OK;
}
static HRESULT WINAPI sto_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d) { return rm_desc_parse(s, d, mmioFOURCC('D','M','S','T'), &RM_CLSID_DirectMusicStyle); }
static IDirectMusicObjectVtbl sto_vtbl = { sto_qi, sto_addref, sto_release, sto_getdesc, sto_setdesc, sto_parse };

/* the band objects of a loaded style, the descriptor */
static void style_take(rm_style *o, dms_style *x)
{
    style_drop(o);
    o->s = x;
    x->wrapper = o;
    o->bands = calloc(x->nbands ? x->nbands : 1, sizeof(*o->bands));
    for (int k = 0; o->bands && k < x->nbands; k++) {
        rm_band *b = new_band(x->bands[k]);
        if (!b) continue;
        o->bands[o->nbands++] = b;
        if (x->bands[k] == x->defband && !o->defband) { o->defband = b; bnd_addref(&b->IDirectMusicBand_iface); }
    }
    o->desc.dwValidData |= DMUS_OBJ_LOADED;
    if (x->hasguid) { memcpy(&o->desc.guidObject, &x->guid, sizeof(GUID)); o->desc.dwValidData |= DMUS_OBJ_OBJECT; }
    if (x->name[0]) { memcpy(o->desc.wszName, x->name, sizeof(x->name)); o->desc.dwValidData |= DMUS_OBJ_NAME; }
    if (x->category[0]) { memcpy(o->desc.wszCategory, x->category, sizeof(x->category)); o->desc.dwValidData |= DMUS_OBJ_CATEGORY; }
    o->desc.vVersion.dwVersionMS = x->vers[0]; o->desc.vVersion.dwVersionLS = x->vers[1];
    o->desc.dwValidData |= DMUS_OBJ_VERSION;
}
static HRESULT WINAPI stp_qi(IPersistStream *i, REFIID r, void **o) { return sty_qi(&stp(i)->IDirectMusicStyle8_iface, r, o); }
static ULONG WINAPI stp_addref(IPersistStream *i) { return sty_addref(&stp(i)->IDirectMusicStyle8_iface); }
static ULONG WINAPI stp_release(IPersistStream *i) { return sty_release(&stp(i)->IDirectMusicStyle8_iface); }
static HRESULT WINAPI stp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = RM_CLSID_DirectMusicStyle; return S_OK; }
static HRESULT WINAPI stp_isdirty(IPersistStream *i) { return S_FALSE; }
static dms_style *style_from_data(const BYTE *b, DWORD n, IDirectMusicLoader8 *l, HRESULT *hr)
{
    *hr = DMUS_E_UNSUPPORTED_STREAM;
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DMST", 4)) return NULL;
    int32_t r;
    dms_style *x = dms_style_load(b + 12, n - 12, loader_getcoll, l, &r);
    *hr = r;
    return x;
}
static HRESULT WINAPI stp_load(IPersistStream *i, IStream *st)
{
    rm_style *o = stp(i);
    BYTE *b; DWORD n;
    if (!st) return E_POINTER;
    if (!read_stream(st, &b, &n)) return E_FAIL;
    HRESULT hr;
    IDirectMusicLoader8 *l = rm_stream_loader(st);
    dms_style *x = style_from_data(b, n, l, &hr);
    if (!x) { free(b); return FAILED(hr) ? hr : E_FAIL; }
    style_take(o, x);
    style_load_cmaps(o, b, n, l);
    free(b);
    return S_OK;
}
static HRESULT WINAPI stp_save(IPersistStream *i, IStream *s, BOOL c) { return rm_notimpl(__func__); }
static HRESULT WINAPI stp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return rm_notimpl(__func__); }
static IPersistStreamVtbl stp_vtbl = { stp_qi, stp_addref, stp_release, stp_getclass, stp_isdirty, stp_load, stp_save, stp_getsize };

static rm_style *new_style(dms_style *x)
{
    rm_style *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicStyle8_iface.lpVtbl = &sty_vtbl;
    o->IDirectMusicObject_iface.lpVtbl = &sto_vtbl;
    o->IPersistStream_iface.lpVtbl = &stp_vtbl;
    o->ref = 1;
    o->desc.dwSize = sizeof(o->desc);
    o->desc.dwValidData = DMUS_OBJ_CLASS;       /* (dmstyle's constructor leaves DMUS_OBJ_OBJECT and the GUID uninitialised) */
    o->desc.guidClass = RM_CLSID_DirectMusicStyle;
    if (x) style_take(o, x);
    return o;
}
/* the style behind an interface (ours only) */
static rm_style *style_of(void *p)
{
    if (!p || IsBadReadPtr(p, sizeof(void *))) return NULL;
    void *vt = *(void **)p;
    if (vt == &sty_vtbl) return sty(p);
    if (vt == &sto_vtbl) return sto(p);
    if (vt == &stp_vtbl) return stp(p);
    return NULL;
}
/* the interface for an engine style (its wrapper, made when it has none); the engine's reference handed over */
static IDirectMusicStyle8 *style_iface(dms_style *x)
{
    if (!x) return NULL;
    rm_style *o = x->wrapper;
    if (o) { sty_addref(&o->IDirectMusicStyle8_iface); dms_style_release(x); return &o->IDirectMusicStyle8_iface; }
    o = new_style(NULL);
    if (!o) { dms_style_release(x); return NULL; }
    style_take(o, x);
    return &o->IDirectMusicStyle8_iface;
}
