/* reDM, included by dmcom.c: collections and the instruments Collection::GetInstrument hands out. */
/* ------------------------------------------------------------------ collections */
static rm_collection *col(IDirectMusicCollection *i) { return impl_from(i, rm_collection, IDirectMusicCollection_iface); }
static HRESULT WINAPI col_qi(IDirectMusicCollection *i, REFIID riid, void **out)
{
    rm_collection *c = col(i);
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicCollection)) *out = &c->IDirectMusicCollection_iface;
    else if (IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &c->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &c->IPersistStream_iface;
    else { return rm_noiface(__func__, riid, out); }
    InterlockedIncrement(&c->ref);
    return S_OK;
}
static ULONG WINAPI col_addref(IDirectMusicCollection *i) { return InterlockedIncrement(&col(i)->ref); }
static ULONG WINAPI col_release(IDirectMusicCollection *i)
{
    LONG r = InterlockedDecrement(&col(i)->ref);
    LOG_CALL("music: Collection::Release(%p) -> %ld", (void *)i, (long)r);
    if (!r) { dls_release(col(i)->c); free(col(i)); }   /* (a file's collection stays in the process-wide cache) */
    return r;
}
/* reAL: the engine collection with a reference (dls_release), read under the engine lock: a Load on another thread
 * replaces it and lets go of the old one */
static dm_collection *col_hold(rm_collection *o)
{
    dm_engine_lock();
    dm_collection *c = o->c;
    dls_addref(c);
    dm_engine_unlock();
    return c;
}
/* ------------------------------------------------------------------ instruments (Collection::GetInstrument) */
/* dmusic's CInstrument: the collection's instrument and a patch the application may change (SetPatch takes any
 * value).  The collection hands out one object per instrument while it lives; the object holds the collection. */
typedef struct rm_instrument {
    IDirectMusicInstrument IDirectMusicInstrument_iface;
    LONG ref;
    struct rm_instrument *next;         /* on its collection's list (s_inslock) */
    rm_collection *owner;               /* a reference */
    dm_collection *c;                   /* a reference: the collection as it was loaded then */
    DWORD orig;                         /* its patch in the collection */
    volatile LONG patch;                /* the one it downloads as */
    LONG serial;                        /* tells it from a later object at the same address */
} rm_instrument;
static SRWLOCK s_inslock = SRWLOCK_INIT;
static LONG s_insserial;
static IDirectMusicInstrumentVtbl ins_vtbl;
static rm_instrument *ins(IDirectMusicInstrument *i) { return impl_from(i, rm_instrument, IDirectMusicInstrument_iface); }
static HRESULT WINAPI ins_qi(IDirectMusicInstrument *i, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicInstrument)) *out = i;
    else return rm_noiface(__func__, riid, out);
    InterlockedIncrement(&ins(i)->ref);
    return S_OK;
}
static ULONG WINAPI ins_addref(IDirectMusicInstrument *i) { return InterlockedIncrement(&ins(i)->ref); }
static ULONG WINAPI ins_release(IDirectMusicInstrument *i)
{
    rm_instrument *o = ins(i);
    AcquireSRWLockExclusive(&s_inslock);        /* (GetInstrument takes a reference under it) */
    LONG r = InterlockedDecrement(&o->ref);
    if (!r) {
        rm_instrument **pp = &o->owner->ins;
        while (*pp && *pp != o) pp = &(*pp)->next;
        if (*pp) *pp = o->next;
    }
    ReleaseSRWLockExclusive(&s_inslock);
    if (!r) {
        dls_release(o->c);
        col_release(&o->owner->IDirectMusicCollection_iface);
        free(o);
    }
    return r;
}
static HRESULT WINAPI ins_getpatch(IDirectMusicInstrument *i, DWORD *patch)
{
    if (!patch) return E_POINTER;
    *patch = (DWORD)ins(i)->patch;
    return S_OK;
}
static HRESULT WINAPI ins_setpatch(IDirectMusicInstrument *i, DWORD patch)
{
    InterlockedExchange(&ins(i)->patch, (LONG)patch);
    return S_OK;
}
static IDirectMusicInstrumentVtbl ins_vtbl = { ins_qi, ins_addref, ins_release, ins_getpatch, ins_setpatch };
/* for the port's DownloadInstrument: one of ours, its collection (a reference: dls_release), the patch it has in it,
 * the patch to download it as, and its serial */
int rm_instrument_get(IDirectMusicInstrument *i, dm_collection **c, uint32_t *orig, uint32_t *patch, LONG *serial)
{
    if (!i || IsBadReadPtr(i, sizeof(void *)) || i->lpVtbl != &ins_vtbl) return 0;
    rm_instrument *o = ins(i);
    dls_addref(o->c);
    *c = o->c;
    *orig = o->orig;
    *patch = (uint32_t)o->patch;
    *serial = o->serial;
    return 1;
}

/* 0x88781114 (DMUS_E_INVALIDPATCH) for a patch the collection lacks, as dmusic answers; asking again for one while
 * its object lives hands out the same object */
static HRESULT WINAPI col_getinst(IDirectMusicCollection *i, DWORD patch, IDirectMusicInstrument **out)
{
    rm_collection *cl = col(i);
    if (!out || IsBadWritePtr(out, sizeof(*out))) return E_POINTER;
    *out = NULL;
    dm_collection *c = col_hold(cl);
    if (!c || dls_find(c, patch) < 0) { dls_release(c); return DMUS_E_INVALIDPATCH; }
    rm_instrument *o = calloc(1, sizeof(*o));
    if (!o) { dls_release(c); return E_OUTOFMEMORY; }
    AcquireSRWLockExclusive(&s_inslock);
    for (rm_instrument *x = cl->ins; x; x = x->next)
        if (x->orig == patch && x->c == c) { InterlockedIncrement(&x->ref); *out = &x->IDirectMusicInstrument_iface; break; }
    if (!*out) {
        o->IDirectMusicInstrument_iface.lpVtbl = &ins_vtbl;
        o->ref = 1;
        o->owner = cl;
        InterlockedIncrement(&cl->ref);
        o->c = c; c = NULL;
        o->orig = patch;
        o->patch = (LONG)patch;
        o->serial = InterlockedIncrement(&s_insserial);
        o->next = cl->ins;
        cl->ins = o;
        *out = &o->IDirectMusicInstrument_iface;
        o = NULL;
    }
    ReleaseSRWLockExclusive(&s_inslock);
    free(o);
    dls_release(c);
    return S_OK;
}
static HRESULT WINAPI col_enum(IDirectMusicCollection *i, DWORD n, DWORD *patch, LPWSTR name, DWORD len)
{
    const char *nm;
    uint32_t p;
    dm_collection *c = col_hold(col(i));
    HRESULT hr = S_FALSE;
    if (c && dls_enum(c, (int)n, &p, &nm) >= 0) {
        if (patch) *patch = p;
        if (name && len) MultiByteToWideChar(CP_ACP, 0, nm, -1, name, (int)len);
        hr = S_OK;
    }
    dls_release(c);
    return hr;
}
static IDirectMusicCollectionVtbl col_vtbl = { col_qi, col_addref, col_release, col_getinst, col_enum };
static rm_collection *col_of(IUnknown *u)
{
    IDirectMusicCollection *c = (IDirectMusicCollection *)u;
    return c && c->lpVtbl == &col_vtbl ? col(c) : NULL;
}

static rm_collection *colo(IDirectMusicObject *i) { return impl_from(i, rm_collection, IDirectMusicObject_iface); }
static HRESULT WINAPI colo_qi(IDirectMusicObject *i, REFIID r, void **o) { return col_qi(&colo(i)->IDirectMusicCollection_iface, r, o); }
static ULONG WINAPI colo_addref(IDirectMusicObject *i) { return col_addref(&colo(i)->IDirectMusicCollection_iface); }
static ULONG WINAPI colo_release(IDirectMusicObject *i) { return col_release(&colo(i)->IDirectMusicCollection_iface); }
static HRESULT WINAPI colo_getdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_get(&colo(i)->desc, &CLSID_DirectMusicCollection, d); }
static HRESULT WINAPI colo_setdesc(IDirectMusicObject *i, LPDMUS_OBJECTDESC d) { return rm_desc_set(&colo(i)->desc, d, TRUE); }
/* a DLS form's 'dlid', 'vers' and INFO list's INAM (dmusic's collection) */
static void dls_desc(const BYTE *b, DWORD n, DMUS_OBJECTDESC *d)
{
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DLS ", 4)) return;
    DWORD end = 8 + *(const DWORD *)(b + 4);
    if (end > n) end = n;
    for (DWORD off = 12; off + 8 <= end; ) {
        DWORD sz = *(const DWORD *)(b + off + 4), cn = end - (off + 8) < sz ? end - (off + 8) : sz;
        const BYTE *c = b + off + 8;
        if (!memcmp(b + off, "dlid", 4) && cn >= 16) { memcpy(&d->guidObject, c, 16); d->dwValidData |= DMUS_OBJ_OBJECT; }
        else if (!memcmp(b + off, "vers", 4) && cn >= 8) { memcpy(&d->vVersion, c, 8); d->dwValidData |= DMUS_OBJ_VERSION; }
        else if (!memcmp(b + off, "LIST", 4) && cn >= 4 && !memcmp(c, "INFO", 4)) {
            for (DWORD q = 4; q + 8 <= cn; ) {
                DWORD qs = *(const DWORD *)(c + q + 4), qn = cn - (q + 8) < qs ? cn - (q + 8) : qs;
                if (!memcmp(c + q, "INAM", 4)) {
                    char nm[DMUS_MAX_NAME];
                    DWORD k = qn < DMUS_MAX_NAME - 1 ? qn : DMUS_MAX_NAME - 1;
                    memcpy(nm, c + q + 8, k); nm[k] = 0;
                    MultiByteToWideChar(CP_ACP, 0, nm, -1, d->wszName, DMUS_MAX_NAME);
                    d->dwValidData |= DMUS_OBJ_NAME;
                }
                q += 8 + qn + (qn & 1);
            }
        }
        off += 8 + cn + (cn & 1);
    }
}
static HRESULT WINAPI colo_parse(IDirectMusicObject *i, LPSTREAM s, LPDMUS_OBJECTDESC d)
{
    if (!s || !d) return E_POINTER;
    BYTE h[12];
    ULONG got = 0;
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER at;
    if (FAILED(IStream_Seek(s, z, STREAM_SEEK_CUR, &at))) return E_FAIL;
    if (FAILED(IStream_Read(s, h, 12, &got)) || got != 12) return DMUS_E_INVALIDFILE;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "DLS ", 4)) return DMUS_E_NOTADLSCOL;
    /* the collection's header chunks only (an instrument set can be large) */
    DWORD n = *(DWORD *)(h + 4), cap = 12, len = 12;
    BYTE *b = malloc(4096);
    if (!b) return E_OUTOFMEMORY;
    cap = 4096;
    memcpy(b, h, 12);
    for (ULONGLONG used = 4; used + 8 <= n; ) {
        BYTE ch[8];
        if (FAILED(IStream_Read(s, ch, 8, &got)) || got != 8) break;
        DWORD cs = *(DWORD *)(ch + 4);
        used += 8 + (ULONGLONG)cs + (cs & 1);
        BOOL want = !memcmp(ch, "dlid", 4) || !memcmp(ch, "vers", 4) || !memcmp(ch, "LIST", 4);
        if (want && !memcmp(ch, "LIST", 4)) {
            BYTE t[4];
            if (FAILED(IStream_Read(s, t, 4, &got)) || got != 4) break;
            LARGE_INTEGER back; back.QuadPart = -4;
            IStream_Seek(s, back, STREAM_SEEK_CUR, NULL);
            want = !memcmp(t, "INFO", 4);
        }
        ULONGLONG pad = (ULONGLONG)cs + (cs & 1);
        if (want && pad > 0x100000) break;      /* (a header chunk of 1 MB is a broken size: no growing to it) */
        if (want) {
            if (len + 8 + pad > cap) { while (len + 8 + pad > cap) cap *= 2; BYTE *nb = realloc(b, cap); if (!nb) break; b = nb; }
            memcpy(b + len, ch, 8);
            if (FAILED(IStream_Read(s, b + len + 8, (ULONG)pad, &got)) || got < cs) break;
            len += 8 + pad;
        } else {
            LARGE_INTEGER sk; sk.QuadPart = (LONGLONG)pad;
            if (FAILED(IStream_Seek(s, sk, STREAM_SEEK_CUR, NULL))) break;
        }
    }
    *(DWORD *)(b + 4) = len - 8;
    d->dwValidData = DMUS_OBJ_CLASS;
    d->guidClass = CLSID_DirectMusicCollection;
    dls_desc(b, len, d);
    free(b);
    return S_OK;
}
static IDirectMusicObjectVtbl colo_vtbl = { colo_qi, colo_addref, colo_release, colo_getdesc, colo_setdesc, colo_parse };

static rm_collection *colp(IPersistStream *i) { return impl_from(i, rm_collection, IPersistStream_iface); }
static HRESULT WINAPI colp_qi(IPersistStream *i, REFIID r, void **o) { return col_qi(&colp(i)->IDirectMusicCollection_iface, r, o); }
static ULONG WINAPI colp_addref(IPersistStream *i) { return col_addref(&colp(i)->IDirectMusicCollection_iface); }
static ULONG WINAPI colp_release(IPersistStream *i) { return col_release(&colp(i)->IDirectMusicCollection_iface); }
static HRESULT WINAPI colp_getclass(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = CLSID_DirectMusicCollection; return S_OK; }
static HRESULT WINAPI colp_isdirty(IPersistStream *i) { return S_FALSE; }
/* a collection from the loader's file is read through the process-wide collection cache (a SoundFont converted, the
 * default GM set marked as such); any other stream's DLS form is parsed as it is */
static HRESULT WINAPI colp_load(IPersistStream *i, IStream *st)
{
    rm_collection *o = colp(i);
    if (!st) return E_POINTER;
    const WCHAR *path = rm_stream_path(st);
    dm_collection *c = NULL;
    if (path) {
        char a[MAX_PATH], full[MAX_PATH], gm[MAX_PATH];
        if (!WideCharToMultiByte(CP_ACP, 0, path, -1, a, MAX_PATH, NULL, NULL)) return DMUS_E_LOADER_FAILEDOPEN;
        if (!GetFullPathNameA(a, MAX_PATH, full, NULL)) lstrcpynA(full, a, MAX_PATH);
        const char *g = dm_gm_dls_path();
        if (!g || !GetFullPathNameA(g, MAX_PATH, gm, NULL)) gm[0] = 0;
        c = dm_collection_from_file(full, gm[0] && !lstrcmpiA(full, gm));
        if (!c) return DMUS_E_NOTADLSCOL;
        /* the descriptor from the form's header */
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        if (SUCCEEDED(colo_parse(&o->IDirectMusicObject_iface, st, &d)))
            o->desc.dwValidData |= d.dwValidData & ~DMUS_OBJ_CLASS, o->desc.guidObject = d.guidObject,
            o->desc.vVersion = d.vVersion, memcpy(o->desc.wszName, d.wszName, sizeof(d.wszName));
    } else {
        BYTE *b; DWORD n;
        if (!read_stream(st, &b, &n)) return DMUS_E_INVALIDFILE;
        if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "DLS ", 4)) { free(b); return DMUS_E_NOTADLSCOL; }
        dls_desc(b, n, &o->desc);
        c = dls_load_owned(b, n, 0);
        if (!c) return DMUS_E_NOTADLSCOL;
    }
    dm_engine_lock();                   /* (col_hold reads o->c under it) */
    dm_collection *old = o->c;           /* (bands and downloads made from it keep their own references) */
    o->c = c;
    dm_engine_unlock();
    dls_release(old);
    o->desc.dwValidData |= DMUS_OBJ_LOADED;
    return S_OK;
}
static HRESULT WINAPI colp_save(IPersistStream *i, IStream *s, BOOL c) { return E_NOTIMPL; }
static HRESULT WINAPI colp_getsize(IPersistStream *i, ULARGE_INTEGER *s) { return E_NOTIMPL; }
static IPersistStreamVtbl colp_vtbl = { colp_qi, colp_addref, colp_release, colp_getclass, colp_isdirty, colp_load, colp_save, colp_getsize };

static rm_collection *new_collection(dm_collection *c)
{
    rm_collection *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->IDirectMusicCollection_iface.lpVtbl = &col_vtbl;
    o->IDirectMusicObject_iface.lpVtbl = &colo_vtbl;
    o->IPersistStream_iface.lpVtbl = &colp_vtbl;
    o->ref = 1;
    o->c = c;
    o->desc.dwSize = sizeof(o->desc);
    o->desc.dwValidData = DMUS_OBJ_CLASS | (c ? DMUS_OBJ_LOADED : 0);
    o->desc.guidClass = CLSID_DirectMusicCollection;
    return o;
}
