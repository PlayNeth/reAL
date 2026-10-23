/* reDM: DirectMusic's loader and container, ported from Microsoft's dmloader.dll (5.3.2600.5512).
 *
 * The loader (CLoader 77382ff6) keeps, for each object class it has met (CClass 773835ed), a list of entries
 * (77383f75): a descriptor - what is known of the object and where it is (a file, memory, or a stream position) -
 * and, once loaded, the object.  GetObject finds the entry a descriptor matches (77383646: object GUID, memory,
 * stream, full path, name and category, name, file name), or makes one, and loads it by creating the class
 * (CoCreateInstance) and handing its IPersistStream a stream that knows the loader (IDirectMusicGetLoader), so the
 * object's own references come back here.  SetObject, ScanDirectory and containers describe objects without loading
 * them (IDirectMusicObject::ParseDescriptor).  Loaded objects are kept as children of the object that was loading
 * when they were asked for (or of the loader itself), which CollectGarbage and ReleaseObject use to keep objects
 * referenced in cycles (scripts) alive or to let them go (7738316d, 77385086).
 *
 * The container (CContainer 77386954) loads a 'DMCN' form: it describes each object in it to the loader (embedded
 * ones by position in its stream, references by their DMRF), loads them unless DMUS_CONTAINER_NOLOADS, and when it
 * goes, releases them from the loader's cache (the KEEP flag is read but never kept, so it is ignored). */
#include "dmldr.h"
#include "dmcom.h"
#include "../real.h"
#include <dmusicf.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define E_LDR_NOCLASSID        ((HRESULT)0x88781180)
#define E_LDR_BADPATH          ((HRESULT)0x88781181)
#define E_LDR_FAILEDOPEN       ((HRESULT)0x88781182)
#define E_LDR_FORMATNOTSUPPORTED ((HRESULT)0x88781183)
#define E_LDR_FAILEDCREATE     ((HRESULT)0x88781184)
#define E_LDR_OBJECTNOTFOUND   ((HRESULT)0x88781185)
#define E_LDR_NOFILENAME       ((HRESULT)0x88781186)
#define S_PARTIALLOAD          ((HRESULT)0x08781091)
#define E_NOLOADER             ((HRESULT)0x88781150)
#define E_BADREF               ((HRESULT)0x8878122f)
#define E_CANNOTREAD           ((HRESULT)0x88781133)
#define RIFF_END               ((HRESULT)0x88781210)
#define S_ZOMBIE               ((HRESULT)0x08781216)

static const GUID G_AllTypes = { 0xd2ac2893, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_DefaultGM = { 0xf17e8673, 0xc3b4, 0x11d1, { 0x87, 0x0b, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_ClsCollection = { 0x480ff4b0, 0x28b2, 0x11d1, { 0xbe, 0xf7, 0x00, 0xc0, 0x4f, 0xbf, 0x8f, 0xef } };
static const GUID G_ClsScript = { 0x810b5013, 0xe88d, 0x11d2, { 0x8b, 0xc1, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xb6 } };
static const GUID G_ClsContainer = { 0x9301e380, 0x1f22, 0x11d3, { 0x82, 0x26, 0xd2, 0xfa, 0x76, 0x25, 0x5d, 0x47 } };
static const GUID G_IidContainer = { 0x9301e386, 0x1f22, 0x11d3, { 0x82, 0x26, 0xd2, 0xfa, 0x76, 0x25, 0x5d, 0x47 } };
static const GUID G_IidObject = { 0xd2ac28b5, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_IidGetLoader = { 0x68a04844, 0xd13d, 0x11d1, { 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6 } };
static const GUID G_IidLoader = { 0x2ffaaca2, 0x5dca, 0x11d2, { 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6 } };
static const GUID G_IidLoader8 = { 0x19e7c08c, 0x0a44, 0x4e6a, { 0xa1, 0x16, 0x59, 0x5a, 0x7c, 0xd5, 0xde, 0x8c } };
static const GUID G_IidLoader8P = { 0x3939facd, 0xf6ed, 0x4619, { 0xbd, 0x16, 0x56, 0x60, 0x3f, 0x01, 0x51, 0xca } };
static const GUID G_IidObjectP = { 0x6a20c217, 0xeb3e, 0x40ec, { 0x9f, 0x3a, 0x92, 0x05, 0x08, 0x70, 0x2b, 0x5e } };
static const GUID G_IidStream = { 0x0000000c, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID G_IidUnknown = { 0, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };

#define FCC(a, b, c, d) ((DWORD)(a) | ((DWORD)(b) << 8) | ((DWORD)(c) << 16) | ((DWORD)(d) << 24))
#define LOCATION (DMUS_OBJ_FILENAME | DMUS_OBJ_MEMORY | DMUS_OBJ_STREAM)
/* what keeps a let-go-of entry in its class: a place to load it from, or a name (77383409 / 77384ad1) */
#define FINDABLE (LOCATION | DMUS_OBJ_NAME)

/* ------------------------------------------------------------------ descriptors (CDescriptor 77383301) */
typedef struct {
    LONGLONG memlen;
    DWORD valid;
    GUID obj, cls;
    FILETIME date;
    DMUS_VERSION vers;
    WCHAR *name, *cat, *file;
    DWORD fsize;                    /* (ScanDirectory: the file's size) */
    BYTE *mem;
    IStream *stream, *owner;
    ULONGLONG spos;
} ldesc;

static void ld_init(ldesc *d) { memset(d, 0, sizeof(*d)); }
static void ld_drop_stream(ldesc *d)            /* 77383417 */
{
    if (d->stream) IStream_Release(d->stream);
    d->stream = NULL; d->owner = NULL; d->spos = 0;
    d->valid &= ~DMUS_OBJ_STREAM;
}
static void ld_free(ldesc *d)                   /* 7738336c */
{
    free(d->name); free(d->cat); free(d->file);
    d->name = d->cat = d->file = NULL;
    if (d->stream) IStream_Release(d->stream);
    d->stream = NULL;
}
/* 77383450: the stream kept with where it is now */
static void ld_set_stream(ldesc *d, IStream *s)
{
    ld_drop_stream(d);
    d->stream = s;
    if (s) {
        LARGE_INTEGER z = { { 0, 0 } };
        ULARGE_INTEGER p = { { 0, 0 } };
        IStream_Seek(s, z, STREAM_SEEK_CUR, &p);
        d->spos = p.QuadPart;
        IStream_AddRef(s);
        d->valid |= DMUS_OBJ_STREAM;
    }
}
/* 77383a89 / 77383b3b: at most 63 characters, or none (the flag cleared) */
static void ld_set_str64(WCHAR **slot, DWORD *valid, DWORD flag, const WCHAR *s)
{
    if (!s) return;
    free(*slot); *slot = NULL;
    *valid &= ~flag;
    size_t n = 0;
    while (n < 64 && s[n]) n++;
    if (n >= 64) return;                /* (StringCopyWorker: truncated) */
    WCHAR *c = malloc((n + 1) * sizeof(WCHAR));
    if (!c) return;
    memcpy(c, s, n * sizeof(WCHAR)); c[n] = 0;
    *slot = c;
    *valid |= flag;
}
/* 77383bed: the file name, kept as it is unless it differs */
static HRESULT ld_set_file(ldesc *d, const WCHAR *s)
{
    if (!s) return S_OK;
    size_t n = 0;
    while (n < MAX_PATH && s[n]) n++;
    if (n >= MAX_PATH) return S_OK;     /* (truncated: nothing changes) */
    if (d->file && !_wcsicmp(d->file, s)) return S_OK;
    free(d->file); d->file = NULL;
    d->valid &= ~DMUS_OBJ_FILENAME;
    WCHAR *c = malloc((n + 1) * sizeof(WCHAR));
    if (!c) return E_OUTOFMEMORY;
    memcpy(c, s, n * sizeof(WCHAR)); c[n] = 0;
    d->file = c;
    d->valid |= DMUS_OBJ_FILENAME;
    return S_OK;
}
/* 77383ce7: from an application's descriptor (whose strings get their terminators) */
static void ld_from_desc(ldesc *d, DMUS_OBJECTDESC *s, IStream *owner)
{
    d->valid = s->dwValidData; d->obj = s->guidObject; d->cls = s->guidClass; d->date = s->ftDate;
    d->vers = s->vVersion; d->memlen = s->llMemLength; d->mem = s->pbMemData;
    free(d->name); d->name = NULL;
    if (s->dwValidData & DMUS_OBJ_NAME) { s->wszName[63] = 0; ld_set_str64(&d->name, &d->valid, DMUS_OBJ_NAME, s->wszName); }
    free(d->cat); d->cat = NULL;
    if (s->dwValidData & DMUS_OBJ_CATEGORY) { s->wszCategory[63] = 0; ld_set_str64(&d->cat, &d->valid, DMUS_OBJ_CATEGORY, s->wszCategory); }
    free(d->file); d->file = NULL;
    if (s->dwValidData & DMUS_OBJ_FILENAME) { s->wszFileName[MAX_PATH - 1] = 0; d->valid &= ~DMUS_OBJ_FILENAME; ld_set_file(d, s->wszFileName); }
    ld_drop_stream(d);
    if (s->dwValidData & DMUS_OBJ_STREAM) { ld_set_stream(d, s->pStream); d->owner = owner; }
}
/* 77383dcd: a copy */
static void ld_copy(ldesc *d, const ldesc *s)
{
    d->valid = s->valid; d->obj = s->obj; d->cls = s->cls; d->date = s->date; d->vers = s->vers;
    d->memlen = s->memlen; d->mem = s->mem;
    free(d->name); d->name = NULL;
    if (s->valid & DMUS_OBJ_NAME) ld_set_str64(&d->name, &d->valid, DMUS_OBJ_NAME, s->name);
    free(d->cat); d->cat = NULL;
    if (s->valid & DMUS_OBJ_CATEGORY) ld_set_str64(&d->cat, &d->valid, DMUS_OBJ_CATEGORY, s->cat);
    free(d->file); d->file = NULL;
    if (s->valid & DMUS_OBJ_FILENAME) { d->valid &= ~DMUS_OBJ_FILENAME; ld_set_file(d, s->file); }
    ld_drop_stream(d);
    if (s->valid & DMUS_OBJ_STREAM) { ld_set_stream(d, s->stream); d->owner = s->owner; }
}
/* 77383e81: what src knows added to d (a file name only where d has none) */
static void ld_merge(ldesc *d, const ldesc *s)
{
    if (s->valid & DMUS_OBJ_OBJECT) { d->valid |= DMUS_OBJ_OBJECT; d->obj = s->obj; }
    if (s->valid & DMUS_OBJ_CLASS) { d->valid |= DMUS_OBJ_CLASS; d->cls = s->cls; }
    if (s->valid & DMUS_OBJ_NAME) { d->valid |= DMUS_OBJ_NAME; ld_set_str64(&d->name, &d->valid, DMUS_OBJ_NAME, s->name); }
    if (s->valid & DMUS_OBJ_CATEGORY) { d->valid |= DMUS_OBJ_CATEGORY; ld_set_str64(&d->cat, &d->valid, DMUS_OBJ_CATEGORY, s->cat); }
    if (s->valid & DMUS_OBJ_VERSION) { d->valid |= DMUS_OBJ_VERSION; d->vers = s->vers; }
    if (s->valid & DMUS_OBJ_DATE) { d->valid |= DMUS_OBJ_DATE; d->date = s->date; }
    if ((s->valid & DMUS_OBJ_FILENAME) && !(d->valid & DMUS_OBJ_FILENAME))
        if (SUCCEEDED(ld_set_file(d, s->file))) d->valid |= s->valid & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH | DMUS_OBJ_URL);
    if (s->valid & DMUS_OBJ_MEMORY) {
        d->mem = s->mem; d->memlen = s->memlen;
        if (!d->memlen || !d->mem) d->valid &= ~DMUS_OBJ_MEMORY; else d->valid |= DMUS_OBJ_MEMORY;
    }
    if (s->valid & DMUS_OBJ_STREAM) ld_set_stream(d, s->stream);
}
/* 77383508: into an application's descriptor (the stream left out) */
static void ld_to_desc(const ldesc *d, DMUS_OBJECTDESC *o)
{
    o->dwValidData = d->valid & ~DMUS_OBJ_STREAM;
    o->guidObject = d->obj; o->guidClass = d->cls; o->ftDate = d->date; o->vVersion = d->vers;
    o->llMemLength = d->memlen; o->pbMemData = d->mem;
    if (d->name) wcsncpy(o->wszName, d->name, DMUS_MAX_NAME);
    if (d->cat) wcsncpy(o->wszCategory, d->cat, DMUS_MAX_CATEGORY);
    if (d->file) wcsncpy(o->wszFileName, d->file, DMUS_MAX_FILENAME);
}
static BOOL ld_noloc(const ldesc *d) { return !(d->valid & FINDABLE); }   /* 77383409 */

/* ------------------------------------------------------------------ the loader's structures */
typedef struct lclass lclass;
typedef struct loader loader;
typedef struct lentry {
    struct lentry *next;
    ldesc d;
    IDirectMusicObject *obj;
    DWORD flags;                        /* 0x1000 kept for garbage collection, 0x2000 marked, 2/4/8 scan marks */
    struct { struct lentry **a; DWORD n, cap; } *kids;
    lclass *cls;
} lentry;
#define EF_GC 0x1000
#define EF_MARK 0x2000
struct lclass {
    lclass *next;
    ldesc d;                            /* the class, and its own search directory (file name, full path) */
    loader *ld;
    BOOL cache;
    lentry *list;
    DWORD eidx;
    lentry *ecur;
    BOOL scanned;
};
typedef struct ldr8p_vtbl {
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *GetDynamicallyReferencedObject)(void *, IDirectMusicObject *, DMUS_OBJECTDESC *, REFIID, void **);
    HRESULT (WINAPI *ReportDynamicallyReferencedObject)(void *, IDirectMusicObject *, IUnknown *);
    ULONG (WINAPI *AddRefP)(void *);
    ULONG (WINAPI *ReleaseP)(void *);
} ldr8p_vtbl;
struct loader {
    IDirectMusicLoader8 IDirectMusicLoader8_iface;
    const ldr8p_vtbl *p_vtbl;           /* IDirectMusicLoader8P (3939facd) */
    LONG ref, pref;                     /* +0x220 the application's, +0x224 the streams' */
    BOOL defcache;
    lclass *classes;
    WCHAR dir[MAX_PATH];
    BOOL hasdir;
    CRITICAL_SECTION cs;
    lentry *root, *parent, *garbage;    /* +0x244 the loader itself, +0x248 the object loading, +0x24c let go */
    /* reAL: objects to let go of (or zombie) once the lock is: nothing is called with the lock held but the Load in
     * GetObject (as dmloader does), so whatever a Release or Zombie does (a script's teardown, a container's
     * ReleaseObject, the performance's lock) cannot wait on a thread waiting for this loader */
    int depth;                          /* this lock's holds (by its owner) */
    struct ltrash { struct ltrash *next; IUnknown *u; BOOL zombie; } *trash, **trash_tail;
};
static loader *ldo(IDirectMusicLoader8 *i) { return CONTAINING_RECORD(i, loader, IDirectMusicLoader8_iface); }

static void ldr_lock(loader *L) { EnterCriticalSection(&L->cs); L->depth++; }
static void trash_run(struct ltrash *t)
{
    while (t) {
        struct ltrash *n = t->next;
        if (t->zombie) {                /* IDirectMusicObjectP::Zombie (77385086) */
            IUnknown *z = NULL;
            if (SUCCEEDED(IUnknown_QueryInterface(t->u, &G_IidObjectP, (void **)&z)) && z) {
                ((void (WINAPI *)(IUnknown *))((void **)z->lpVtbl)[3])(z);
                IUnknown_Release(z);
            }
        }
        IUnknown_Release(t->u);
        free(t);
        t = n;
    }
}
static void ldr_unlock(loader *L)
{
    struct ltrash *t = NULL;
    if (--L->depth == 0 && L->trash) { t = L->trash; L->trash = NULL; L->trash_tail = &L->trash; }
    LeaveCriticalSection(&L->cs);
    trash_run(t);
}
/* an object reference let go of (zombied first) once the lock is (now without a loader, or out of memory) */
static void ldr_drop(loader *L, void *obj, BOOL zombie)
{
    if (!obj) return;
    struct ltrash *t = L ? malloc(sizeof(*t)) : NULL;
    if (!t) {
        struct ltrash *x = malloc(sizeof(*x));
        if (x) { x->next = NULL; x->u = obj; x->zombie = zombie; trash_run(x); }
        else IUnknown_Release((IUnknown *)obj);
        return;
    }
    t->next = NULL; t->u = obj; t->zombie = zombie;
    if (!L->trash_tail) L->trash_tail = &L->trash;
    *L->trash_tail = t; L->trash_tail = &t->next;
}
static loader *ldp(void *i) { return CONTAINING_RECORD(i, loader, p_vtbl); }
static void ldr_addref_p(loader *L);
static void ldr_release_p(loader *L);

static lentry *entry_new(lclass *c)
{
    lentry *e = calloc(1, sizeof(*e));
    if (e) { ld_init(&e->d); e->cls = c; }
    return e;
}
static void entry_free(loader *L, lentry *e)    /* 7738452e */
{
    if (!e) return;
    ldr_drop(L, e->obj, FALSE);
    if (e->kids) { free(e->kids->a); free(e->kids); }
    ld_free(&e->d);
    free(e);
}
/* 77383f75: an entry for what d describes, not loaded (the class's when d has none) */
static lentry *entry_from(lclass *c, const ldesc *d)
{
    lentry *e = entry_new(c);
    if (!e) return NULL;
    ld_copy(&e->d, d);
    e->d.valid &= ~DMUS_OBJ_LOADED;
    if (!(e->d.valid & DMUS_OBJ_CLASS)) { e->d.cls = c->d.cls; e->d.valid |= c->d.valid & DMUS_OBJ_CLASS; }
    return e;
}
static HRESULT entry_gc(lentry *e)              /* 77384370 */
{
    e->flags |= EF_GC;
    e->kids = calloc(1, sizeof(*e->kids));
    return e->kids ? S_OK : E_OUTOFMEMORY;
}
static HRESULT entry_add_kid(lentry *p, lentry *e)     /* 77384588 */
{
    if (!e) return E_POINTER;
    if (!p || !(e->flags & EF_GC) || !p->kids) return S_OK;
    for (DWORD i = 0; i < p->kids->n; i++) if (p->kids->a[i] == e) return S_OK;
    if (p->kids->n == p->kids->cap) {
        DWORD c = p->kids->cap ? p->kids->cap * 2 : 1;
        lentry **a = realloc(p->kids->a, c * sizeof(*a));
        if (!a) return E_OUTOFMEMORY;
        p->kids->a = a; p->kids->cap = c;
    }
    p->kids->a[p->kids->n++] = e;
    return S_OK;
}
static void entry_drop_kid(lentry *p, lentry *e)       /* 773843aa */
{
    if (!p || !p->kids) return;
    for (DWORD i = 0; i < p->kids->n; i++) if (p->kids->a[i] == e) { p->kids->a[i] = NULL; return; }
}
/* 77382f47: marks (or clears) what e reaches; with a target, whether it reaches it; without, its children list
 * compacted */
static BOOL gc_walk(lentry *e, lentry *target, BOOL mark)
{
    if (!e || !(e->flags & EF_GC) || !e->kids || !!(e->flags & EF_MARK) == !!mark) return FALSE;
    if (mark) e->flags |= EF_MARK; else e->flags &= ~EF_MARK;
    DWORD n = e->kids->n, k = 0;
    for (DWORD i = 0; i < n; i++) {
        lentry *c = e->kids->a[i];
        if (!c) continue;
        if (c == target || gc_walk(c, target, mark)) return TRUE;
        if (!target) e->kids->a[k++] = c;
    }
    if (!target) e->kids->n = k;
    return FALSE;
}
static BOOL gc_cyclic(lentry *e) { BOOL r = gc_walk(e, e, TRUE); gc_walk(e, e, FALSE); return r; }  /* 773830d2 */

/* 77383646: the entry d describes (not `skip`): by object GUID, memory, stream, full path, name and category, name,
 * file name (a full path against a name compares from the last backslash, so they never match), or object */
static HRESULT class_find(lclass *c, const ldesc *d, lentry **out, const lentry *skip, IDirectMusicObject *obj)
{
    DWORD v = d->valid;
    lentry *e;
    *out = NULL;
    if (v & DMUS_OBJ_OBJECT)
        for (e = c->list; e; e = e->next)
            if (e != skip && (e->d.valid & DMUS_OBJ_OBJECT) && IsEqualGUID(&e->d.obj, &d->obj)) { *out = e; return S_OK; }
    if (v & DMUS_OBJ_MEMORY)
        for (e = c->list; e; e = e->next)
            if (e != skip && (e->d.valid & DMUS_OBJ_MEMORY) && e->d.mem == d->mem) { *out = e; return S_OK; }
    if ((v & DMUS_OBJ_STREAM) && d->stream)
        for (e = c->list; e; e = e->next)
            if (e != skip && (e->d.valid & DMUS_OBJ_STREAM) && e->d.stream == d->stream && e->d.owner == d->owner &&
                e->d.spos == d->spos) { *out = e; return S_OK; }
    if ((v & DMUS_OBJ_FILENAME) && (v & DMUS_OBJ_FULLPATH))
        for (e = c->list; e; e = e->next)
            if (e != skip && (e->d.valid & DMUS_OBJ_FILENAME) && (e->d.valid & DMUS_OBJ_FULLPATH) && !_wcsicmp(e->d.file, d->file)) { *out = e; return S_OK; }
    if ((v & DMUS_OBJ_NAME) && (v & DMUS_OBJ_CATEGORY))
        for (e = c->list; e; e = e->next)
            if (e != skip && (e->d.valid & DMUS_OBJ_NAME) && (e->d.valid & DMUS_OBJ_CATEGORY) && !_wcsicmp(e->d.cat, d->cat) &&
                !_wcsicmp(e->d.name, d->name)) { *out = e; return S_OK; }
    if (v & DMUS_OBJ_NAME)
        for (e = c->list; e; e = e->next)
            if (e != skip && (e->d.valid & DMUS_OBJ_NAME) && !_wcsicmp(e->d.name, d->name)) { *out = e; return S_OK; }
    if (v & DMUS_OBJ_FILENAME)
        for (e = c->list; e; e = e->next) {
            if (e == skip || !(e->d.valid & DMUS_OBJ_FILENAME)) continue;
            const WCHAR *a = e->d.file, *b = d->file;
            if ((v & DMUS_OBJ_FULLPATH) != (e->d.valid & DMUS_OBJ_FULLPATH)) {
                if (v & DMUS_OBJ_FULLPATH) b = wcsrchr(b, L'\\'); else a = wcsrchr(a, L'\\');
                if (!a || !b) continue;
            }
            if (!_wcsicmp(a, b)) { *out = e; return S_OK; }
        }
    if (obj)
        for (e = c->list; e; e = e->next) if (e != skip && e->obj == obj) { *out = e; return S_OK; }
    return E_LDR_OBJECTNOTFOUND;
}
/* 77384ad1: an entry taken out of the class and deleted (NULL: every one that says nowhere to load from) */
static void class_remove(lclass *c, lentry *which)
{
    lentry **pp = &c->list;
    while (*pp) {
        lentry *e = *pp;
        if (e == which || (!which && !(e->d.valid & FINDABLE))) {
            *pp = e->next;
            entry_free(c->ld, e);
            if (which) break;
        } else pp = &e->next;
    }
    c->ecur = NULL;
}
static void class_free(lclass *c)               /* 7738514e */
{
    while (c->list) { lentry *e = c->list; c->list = e->next; entry_free(NULL, e); }
    ld_free(&c->d);
    free(c);
}
/* 77381d23: the class of d (NOCLASSID without one) */
static HRESULT ldr_class(loader *L, const ldesc *d, lclass **out, BOOL create)
{
    *out = NULL;
    if (!(d->valid & DMUS_OBJ_CLASS)) return E_LDR_NOCLASSID;
    for (lclass *c = L->classes; c; c = c->next) if (IsEqualGUID(&c->d.cls, &d->cls)) { *out = c; return S_OK; }
    if (!create) return E_LDR_OBJECTNOTFOUND;
    lclass *c = calloc(1, sizeof(*c));
    if (!c) return E_OUTOFMEMORY;
    ld_init(&c->d);
    c->d.cls = d->cls; c->d.valid = DMUS_OBJ_CLASS;
    c->ld = L; c->cache = L->defcache;
    c->next = L->classes; L->classes = c;
    *out = c;
    return S_OK;
}
/* 773844c5 */
static HRESULT class_find_or_add(lclass *c, const ldesc *d, lentry **out)
{
    if (SUCCEEDED(class_find(c, d, out, NULL, NULL))) return S_OK;
    lentry *e = entry_from(c, d);
    *out = e;
    if (!e) return E_OUTOFMEMORY;
    e->next = c->list; c->list = e;
    return S_OK;
}
/* 7738389e: the directory a class's relative file names are in */
static void class_dir(lclass *c, WCHAR *out)
{
    if (c->d.valid & DMUS_OBJ_FILENAME) lstrcpynW(out, c->d.file, MAX_PATH);
    else if (c->ld->hasdir) lstrcpynW(out, c->ld->dir, MAX_PATH);
    else out[0] = 0;
}

/* ------------------------------------------------------------------ streams (CFileStream 77386061, CMemStream
 * 77385c9a, CStream 77385e58): IStream and IDirectMusicGetLoader, holding the loader */
typedef struct lstream {
    IStream IStream_iface;
    IDirectMusicGetLoader IDirectMusicGetLoader_iface;
    LONG ref;
    loader *ld;
    int kind;                           /* 0 a file, 1 memory, 2 another stream */
    HANDLE file;
    WCHAR *path;                        /* (a file's) */
    const BYTE *data;
    ULONGLONG size, pos;
    IStream *inner;
} lstream;
static const IStreamVtbl ls_vtbl;
static const IDirectMusicGetLoaderVtbl lg_vtbl;
static lstream *lso(IStream *i) { return CONTAINING_RECORD(i, lstream, IStream_iface); }
static lstream *lgo(IDirectMusicGetLoader *i) { return CONTAINING_RECORD(i, lstream, IDirectMusicGetLoader_iface); }
static lstream *ls_new(loader *L, int kind)
{
    lstream *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->IStream_iface.lpVtbl = (IStreamVtbl *)&ls_vtbl;
    s->IDirectMusicGetLoader_iface.lpVtbl = (IDirectMusicGetLoaderVtbl *)&lg_vtbl;
    s->ref = 1; s->kind = kind; s->ld = L;
    if (L) ldr_addref_p(L);
    return s;
}
/* CreateFile, retried with WOW64 file system redirection off (gm.dls in system32\drivers from a 32-bit game) */
static HANDLE open_read(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) return f;
    BOOL (WINAPI *dis)(PVOID *) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "Wow64DisableWow64FsRedirection");
    BOOL (WINAPI *rev)(PVOID) = (void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "Wow64RevertWow64FsRedirection");
    PVOID old;
    if (dis && rev && dis(&old)) {
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        rev(old);
    }
    return f;
}
/* 77385b8c: a file opened for reading */
static HRESULT ls_open_file(lstream *s, const WCHAR *path)
{
    HANDLE f = open_read(path);
    if (f == INVALID_HANDLE_VALUE) return E_LDR_FAILEDOPEN;
    if (s->file) CloseHandle(s->file);
    free(s->path);
    s->file = f;
    s->path = _wcsdup(path);
    return S_OK;
}
static HRESULT ls_open_memory(lstream *s, const BYTE *p, ULONGLONG n)     /* 77385d66 */
{
    s->data = p; s->size = n; s->pos = 0;
    if (p && n && !IsBadReadPtr(p, (UINT_PTR)n)) return S_OK;
    s->data = NULL; s->size = 0;
    return E_LDR_FAILEDOPEN;
}
static HRESULT ls_open_stream(lstream *s, IStream *in, ULONGLONG pos)      /* 77385eb5 */
{
    if (s->inner) IStream_Release(s->inner);
    s->inner = in;
    if (in) {
        LARGE_INTEGER p; p.QuadPart = (LONGLONG)pos;
        IStream_Seek(in, p, STREAM_SEEK_SET, NULL);
        IStream_AddRef(in);
    }
    return S_OK;
}
static HRESULT WINAPI ls_qi(IStream *i, REFIID r, void **o)
{
    lstream *s = lso(i);
    if (IsEqualIID(r, &G_IidUnknown) || IsEqualIID(r, &G_IidStream)) *o = &s->IStream_iface;
    else if (IsEqualIID(r, &G_IidGetLoader)) *o = &s->IDirectMusicGetLoader_iface;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG WINAPI ls_addref(IStream *i) { return InterlockedIncrement(&lso(i)->ref); }
static ULONG WINAPI ls_release(IStream *i)
{
    lstream *s = lso(i);
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        if (s->file) CloseHandle(s->file);
        free(s->path);
        if (s->inner) IStream_Release(s->inner);
        if (s->ld) ldr_release_p(s->ld);
        free(s);
    }
    return r;
}
static HRESULT WINAPI ls_read(IStream *i, void *p, ULONG n, ULONG *got)
{
    lstream *s = lso(i);
    if (s->kind == 2) return s->inner ? IStream_Read(s->inner, p, n, got) : E_FAIL;
    if (s->kind == 0) {
        DWORD r = 0;
        if (!s->file || !ReadFile(s->file, p, n, &r, NULL) || r != n) return E_FAIL;
        if (got) *got = n;
        return S_OK;
    }
    if (s->pos + n > s->size) return E_FAIL;     /* (77385648 / 773854d8: all of it or nothing) */
    memcpy(p, s->data + s->pos, n);
    s->pos += n;
    if (got) *got = n;
    return S_OK;
}
static HRESULT WINAPI ls_write(IStream *i, const void *p, ULONG n, ULONG *put)
{
    lstream *s = lso(i);
    if (s->kind == 2) return s->inner ? IStream_Write(s->inner, p, n, put) : E_FAIL;
    return s->kind == 1 ? E_NOTIMPL : STG_E_CANTSAVE;
}
static HRESULT WINAPI ls_seek(IStream *i, LARGE_INTEGER d, DWORD how, ULARGE_INTEGER *np)
{
    lstream *s = lso(i);
    if (s->kind == 2) return s->inner ? IStream_Seek(s->inner, d, how, np) : E_FAIL;
    if (s->kind == 0) {
        LARGE_INTEGER r;
        if (!s->file || !SetFilePointerEx(s->file, d, &r, how)) return E_FAIL;
        if (np) np->QuadPart = (ULONGLONG)r.QuadPart;
        return S_OK;
    }
    LONGLONG base = how == STREAM_SEEK_CUR ? (LONGLONG)s->pos : how == STREAM_SEEK_END ? (LONGLONG)s->size : 0;
    LONGLONG to = base + d.QuadPart;
    if (to < 0 || (s->kind == 1 && (ULONGLONG)to > s->size)) return E_FAIL;
    s->pos = (ULONGLONG)to;
    if (np) np->QuadPart = s->pos;
    return S_OK;
}
static HRESULT WINAPI ls_setsize(IStream *i, ULARGE_INTEGER n) { lstream *s = lso(i); return s->kind == 2 && s->inner ? IStream_SetSize(s->inner, n) : E_NOTIMPL; }
static HRESULT WINAPI ls_copyto(IStream *i, IStream *t, ULARGE_INTEGER n, ULARGE_INTEGER *r, ULARGE_INTEGER *w)
{ lstream *s = lso(i); return s->kind == 2 && s->inner ? IStream_CopyTo(s->inner, t, n, r, w) : E_NOTIMPL; }
static HRESULT WINAPI ls_commit(IStream *i, DWORD f) { lstream *s = lso(i); return s->kind == 2 && s->inner ? IStream_Commit(s->inner, f) : E_NOTIMPL; }
static HRESULT WINAPI ls_revert(IStream *i) { lstream *s = lso(i); return s->kind == 2 && s->inner ? IStream_Revert(s->inner) : E_NOTIMPL; }
static HRESULT WINAPI ls_lock(IStream *i, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD t)
{ lstream *s = lso(i); return s->kind == 2 && s->inner ? IStream_LockRegion(s->inner, o, n, t) : E_NOTIMPL; }
static HRESULT WINAPI ls_unlock(IStream *i, ULARGE_INTEGER o, ULARGE_INTEGER n, DWORD t)
{ lstream *s = lso(i); return s->kind == 2 && s->inner ? IStream_UnlockRegion(s->inner, o, n, t) : E_NOTIMPL; }
static HRESULT WINAPI ls_stat(IStream *i, STATSTG *st, DWORD f) { lstream *s = lso(i); return s->kind == 2 && s->inner ? IStream_Stat(s->inner, st, f) : E_NOTIMPL; }
static HRESULT WINAPI ls_clone(IStream *i, IStream **out)       /* 773860c8 / 77385dd2 / 77385f04 */
{
    lstream *s = lso(i);
    *out = NULL;
    lstream *n = ls_new(s->ld, s->kind);
    if (!n) return E_OUTOFMEMORY;
    if (s->kind == 2) {
        IStream *c = NULL;
        HRESULT hr = s->inner ? IStream_Clone(s->inner, &c) : E_FAIL;
        if (FAILED(hr)) { ls_release(&n->IStream_iface); return hr; }
        n->inner = c;
    } else if (s->kind == 0) {
        LARGE_INTEGER z = { { 0, 0 } }, at;
        if (!s->path || FAILED(ls_open_file(n, s->path)) || !SetFilePointerEx(s->file, z, &at, FILE_CURRENT) ||
            !SetFilePointerEx(n->file, at, NULL, FILE_BEGIN)) { ls_release(&n->IStream_iface); return E_FAIL; }
    } else {
        n->data = s->data; n->size = s->size; n->pos = s->pos;
    }
    *out = &n->IStream_iface;
    return S_OK;
}
static const IStreamVtbl ls_vtbl = { ls_qi, ls_addref, ls_release, ls_read, ls_write, ls_seek, ls_setsize, ls_copyto,
                                     ls_commit, ls_revert, ls_lock, ls_unlock, ls_stat, ls_clone };
static HRESULT WINAPI lg_qi(IDirectMusicGetLoader *i, REFIID r, void **o) { return ls_qi(&lgo(i)->IStream_iface, r, o); }
static ULONG WINAPI lg_addref(IDirectMusicGetLoader *i) { return ls_addref(&lgo(i)->IStream_iface); }
static ULONG WINAPI lg_release(IDirectMusicGetLoader *i) { return ls_release(&lgo(i)->IStream_iface); }
static HRESULT WINAPI lg_getloader(IDirectMusicGetLoader *i, IDirectMusicLoader **out)    /* 7738561e */
{
    lstream *s = lgo(i);
    if (!s->ld) { *out = NULL; return E_NOINTERFACE; }
    return IDirectMusicLoader8_QueryInterface(&s->ld->IDirectMusicLoader8_iface, &G_IidLoader, (void **)out);
}
static const IDirectMusicGetLoaderVtbl lg_vtbl = { lg_qi, lg_addref, lg_release, lg_getloader };

const WCHAR *rm_stream_path(IStream *s)
{
    if (!s || s->lpVtbl != &ls_vtbl) return NULL;
    lstream *l = lso(s);
    return l->kind == 0 ? l->path : NULL;
}
IDirectMusicLoader8 *rm_stream_loader(IStream *s)
{
    IDirectMusicGetLoader *g = NULL;
    IDirectMusicLoader *l = NULL;
    IDirectMusicLoader8 *l8 = NULL;
    if (!s || FAILED(IStream_QueryInterface(s, &G_IidGetLoader, (void **)&g)) || !g) return NULL;
    IDirectMusicGetLoader_GetLoader(g, &l);
    IDirectMusicGetLoader_Release(g);
    if (!l) return NULL;
    IDirectMusicLoader_QueryInterface(l, &G_IidLoader8, (void **)&l8);
    IDirectMusicLoader_Release(l);
    if (l8) IDirectMusicLoader8_Release(l8);    /* (lent: the stream holds the loader) */
    return l8;
}

static BOOL read_all(IStream *s, BYTE *p, DWORD n)
{
    ULONG got = 0;
    return SUCCEEDED(IStream_Read(s, p, n, &got)) && got == n;
}
/* the stream's length: Stat, else (the loader's own file and memory streams have none) its end, sought and back */
static BOOL stream_size(IStream *s, ULONGLONG at, ULONGLONG *n)
{
    STATSTG st;
    if (SUCCEEDED(IStream_Stat(s, &st, STATFLAG_NONAME))) { *n = st.cbSize.QuadPart; return TRUE; }
    LARGE_INTEGER z = { { 0, 0 } }, p;
    ULARGE_INTEGER end;
    if (FAILED(IStream_Seek(s, z, STREAM_SEEK_END, &end))) return FALSE;
    p.QuadPart = (LONGLONG)at;
    IStream_Seek(s, p, STREAM_SEEK_SET, NULL);
    *n = end.QuadPart;
    return TRUE;
}
#define FORM_MAX 0x7ffffff0u        /* (no form is near 2 GB: a bigger size is a broken one) */
BOOL rm_read_form(IStream *s, BYTE **data, DWORD *size)
{
    *data = NULL; *size = 0;
    BYTE h[8];
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER start;
    if (FAILED(IStream_Seek(s, z, STREAM_SEEK_CUR, &start)) || !read_all(s, h, 8)) return FALSE;
    DWORD n = *(DWORD *)(h + 4);
    if (!memcmp(h, "RIFF", 4) || !memcmp(h, "LIST", 4)) {
        /* a short form: what there is (as a stream reader would get); its size cut to the stream before allocating */
        ULONGLONG len;
        if (stream_size(s, start.QuadPart + 8, &len) && len >= start.QuadPart + 8 && n > len - start.QuadPart - 8)
            n = (DWORD)(len - start.QuadPart - 8);
        if (n > FORM_MAX) return FALSE;
        BYTE *b = malloc((size_t)n + 9);
        if (!b) return FALSE;
        memcpy(b, h, 8);
        if (!read_all(s, b + 8, n)) { free(b); return FALSE; }
        *data = b; *size = 8 + n;
        return TRUE;
    }
    /* anything else (a MIDI file): the rest of the stream, chunk by chunk where it says how long they are */
    STATSTG st;
    if (SUCCEEDED(IStream_Stat(s, &st, STATFLAG_NONAME)) && st.cbSize.QuadPart >= start.QuadPart + 8) {
        if (st.cbSize.QuadPart - start.QuadPart > FORM_MAX) return FALSE;
        DWORD left = (DWORD)(st.cbSize.QuadPart - start.QuadPart);
        BYTE *b = malloc(left + 1);
        if (!b) return FALSE;
        memcpy(b, h, 8);
        if (left > 8 && !read_all(s, b + 8, left - 8)) { free(b); return FALSE; }
        *data = b; *size = left;
        return TRUE;
    }
    DWORD cap = 4096, got = 8;
    BYTE *b = malloc(cap);
    if (!b) return FALSE;
    memcpy(b, h, 8);
    for (DWORD cl = __builtin_bswap32(n);; ) {          /* SMF chunks: big-endian lengths */
        /* (no MIDI chunk is 64 MB, nor a file 1 GB: junk after the tracks, not a length to grow to) */
        if (cl > 0x4000000u || got > 0x40000000u) break;
        if (got + cl + 8 > cap) { while (got + cl + 8 > cap) cap *= 2; BYTE *nb = realloc(b, cap); if (!nb) { free(b); return FALSE; } b = nb; }
        if (cl && !read_all(s, b + got, cl)) break;
        got += cl;
        if (!read_all(s, b + got, 8)) break;
        cl = __builtin_bswap32(*(DWORD *)(b + got + 4));
        got += 8;
    }
    *data = b; *size = got;
    return TRUE;
}

/* ------------------------------------------------------------------ descriptors of the objects */
HRESULT rm_desc_get(const DMUS_OBJECTDESC *mine, REFCLSID cls, DMUS_OBJECTDESC *out)
{
    if (!out || IsBadWritePtr(out, sizeof(DMUS_OBJECTDESC))) return E_POINTER;
    if (!out->dwSize) out->dwSize = sizeof(DMUS_OBJECTDESC);
    else if (out->dwSize < sizeof(DMUS_OBJECTDESC) || IsBadWritePtr(out, out->dwSize)) return E_INVALIDARG;
    out->guidClass = *cls;
    out->guidObject = mine->guidObject;
    out->ftDate = mine->ftDate;
    out->vVersion = mine->vVersion;
    lstrcpynW(out->wszName, mine->wszName, DMUS_MAX_NAME);
    lstrcpynW(out->wszCategory, mine->wszCategory, DMUS_MAX_CATEGORY);
    lstrcpynW(out->wszFileName, mine->wszFileName, DMUS_MAX_FILENAME);
    out->dwValidData = mine->dwValidData | DMUS_OBJ_CLASS;
    return S_OK;
}
HRESULT rm_desc_set(DMUS_OBJECTDESC *mine, DMUS_OBJECTDESC *in, BOOL loaded)
{
    if (!in || IsBadReadPtr(in, sizeof(DMUS_OBJECTDESC))) return E_POINTER;
    if (in->dwSize && (in->dwSize < sizeof(DMUS_OBJECTDESC) || IsBadReadPtr(in, in->dwSize))) return E_INVALIDARG;
    if (in->dwSize < sizeof(DMUS_OBJECTDESC)) return E_INVALIDARG;
    DWORD v = in->dwValidData, took = 0;
    if (v & DMUS_OBJ_CLASS) took |= DMUS_OBJ_CLASS;
    if (loaded && (v & DMUS_OBJ_LOADED)) took |= DMUS_OBJ_LOADED;
    if (v & DMUS_OBJ_OBJECT) { took |= DMUS_OBJ_OBJECT; mine->guidObject = in->guidObject; }
    if (v & DMUS_OBJ_NAME) { lstrcpynW(mine->wszName, in->wszName, DMUS_MAX_NAME); took |= DMUS_OBJ_NAME; }
    if (v & DMUS_OBJ_CATEGORY) { lstrcpynW(mine->wszCategory, in->wszCategory, DMUS_MAX_CATEGORY); took |= DMUS_OBJ_CATEGORY; }
    if (v & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH)) {
        lstrcpynW(mine->wszFileName, in->wszFileName, DMUS_MAX_FILENAME);
        took |= v & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH);
    }
    if (v & DMUS_OBJ_VERSION) { mine->vVersion = in->vVersion; took |= DMUS_OBJ_VERSION; }
    if (v & DMUS_OBJ_DATE) { mine->ftDate = in->ftDate; took |= DMUS_OBJ_DATE; }
    mine->dwValidData |= took;
    if (v & ~took) { in->dwValidData = took; return S_FALSE; }
    return S_OK;
}
void rm_desc_from_form(const BYTE *p, DWORD n, DMUS_OBJECTDESC *d)
{
    const BYTE *e = p + n;
    while (e - p >= 8) {
        DWORD id = *(const DWORD *)p, sz = *(const DWORD *)(p + 4);
        const BYTE *c = p + 8, *ce = (DWORD)(e - c) < sz ? e : c + sz;
        DWORD cn = (DWORD)(ce - c);
        if (id == FCC('L','I','S','T') && cn >= 4 && *(const DWORD *)c == FCC('U','N','F','O')) {
            const BYTE *q = c + 4;
            while (ce - q >= 8) {
                DWORD qid = *(const DWORD *)q, qsz = *(const DWORD *)(q + 4);
                const BYTE *qc = q + 8;
                DWORD qn = (DWORD)(ce - qc) < qsz ? (DWORD)(ce - qc) : qsz;
                if (qid == FCC('U','N','A','M')) {
                    DWORD m = qn < 0x80 ? qn : 0x80;
                    memset(d->wszName, 0, sizeof(d->wszName));
                    memcpy(d->wszName, qc, m);
                    d->wszName[63] = 0;
                    d->dwValidData |= DMUS_OBJ_NAME;
                }
                q = qc + qn + (qn & 1);
            }
        } else if (id == FCC('g','u','i','d') && cn >= 16) { memcpy(&d->guidObject, c, 16); d->dwValidData |= DMUS_OBJ_OBJECT; }
        else if (id == FCC('d','a','t','e') && cn >= 8) { memcpy(&d->ftDate, c, 8); d->dwValidData |= DMUS_OBJ_DATE; }
        else if (id == FCC('c','a','t',' ')) {
            DWORD m = cn < 0x80 ? cn : 0x80;
            memset(d->wszCategory, 0, sizeof(d->wszCategory));
            memcpy(d->wszCategory, c, m);
            d->wszCategory[63] = 0;
            d->dwValidData |= DMUS_OBJ_CATEGORY;
        } else if (id == FCC('v','e','r','s') && cn >= 8) { memcpy(&d->vVersion, c, 8); d->dwValidData |= DMUS_OBJ_VERSION; }
        p = c + cn + (cn & 1);
    }
}
HRESULT rm_desc_parse(IStream *s, DMUS_OBJECTDESC *d, DWORD form, REFCLSID cls)
{
    if (!s || !d) return E_POINTER;
    if (IsBadWritePtr(d, sizeof(DMUS_OBJECTDESC))) return E_POINTER;
    if (!d->dwSize) d->dwSize = sizeof(DMUS_OBJECTDESC);
    else if (d->dwSize < sizeof(DMUS_OBJECTDESC)) return E_INVALIDARG;
    d->dwValidData = 0;
    BYTE *b; DWORD n;
    if (!rm_read_form(s, &b, &n)) return E_FAIL;
    if (n < 12 || memcmp(b, "RIFF", 4) || *(DWORD *)(b + 8) != form) { free(b); return E_FAIL; }
    rm_desc_from_form(b + 12, n - 12, d);
    free(b);
    d->dwValidData |= DMUS_OBJ_CLASS;
    d->guidClass = *cls;
    return S_OK;
}

/* ------------------------------------------------------------------ creating and loading objects */
static HRESULT create_object(REFCLSID cls, IDirectMusicObject **out)
{
    *out = NULL;
    if (rm_handles(cls)) return rm_create(cls, &G_IidObject, (void **)out);
    return CoCreateInstance(cls, NULL, CLSCTX_INPROC_SERVER, &G_IidObject, (void **)out);
}
/* the stream an entry is loaded or described from (773845ed / 77384562) */
static HRESULT entry_stream(lentry *e, lstream **out)
{
    loader *L = e->cls->ld;
    lstream *s = NULL;
    HRESULT hr;
    *out = NULL;
    if (e->d.valid & DMUS_OBJ_FILENAME) {
        WCHAR path[MAX_PATH];
        if (!(e->d.valid & DMUS_OBJ_FULLPATH)) {
            class_dir(e->cls, path);
            size_t n = wcslen(path);
            wcsncat(path, e->d.file, MAX_PATH - 1 - n);
        } else lstrcpynW(path, e->d.file, MAX_PATH);
        if (!(s = ls_new(L, 0))) return E_OUTOFMEMORY;
        hr = ls_open_file(s, path);
    } else if (e->d.valid & DMUS_OBJ_MEMORY) {
        if (!(s = ls_new(L, 1))) return E_OUTOFMEMORY;
        hr = ls_open_memory(s, e->d.mem, (ULONGLONG)e->d.memlen);
    } else if (e->d.valid & DMUS_OBJ_STREAM) {
        if (!(s = ls_new(L, 2))) return E_OUTOFMEMORY;
        hr = ls_open_stream(s, e->d.stream, e->d.spos);
    } else return E_FAIL;
    if (FAILED(hr)) { ls_release(&s->IStream_iface); return hr; }
    *out = s;
    return S_OK;
}
/* 773845ed: the object made and loaded; what it says of itself added to the entry, which it is then told */
static HRESULT entry_load(lentry *e)
{
    if (!(e->d.valid & LOCATION)) return E_LDR_NOFILENAME;
    IDirectMusicObject *obj = NULL;
    HRESULT hr = create_object(&e->d.cls, &obj);
    if (FAILED(hr)) return hr;
    lstream *s = NULL;
    loader *L = e->cls ? e->cls->ld : NULL;
    if (FAILED(hr = entry_stream(e, &s))) { ldr_drop(L, obj, FALSE); return hr; }
    IPersistStream *ps = NULL;
    if (FAILED(hr = IDirectMusicObject_QueryInterface(obj, &IID_IPersistStream, (void **)&ps))) {
        ls_release(&s->IStream_iface); ldr_drop(L, obj, FALSE);
        return hr;
    }
    ldr_drop(L, e->obj, FALSE);
    e->obj = obj;
    HRESULT lhr = IPersistStream_Load(ps, &s->IStream_iface);
    IPersistStream_Release(ps);
    if (FAILED(lhr)) {
        ldr_drop(L, e->obj, FALSE); e->obj = NULL;
        ls_release(&s->IStream_iface);
        return lhr;
    }
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    IDirectMusicObject_GetDescriptor(e->obj, &d);
    ldesc t;
    ld_init(&t);
    ld_from_desc(&t, &d, NULL);
    ld_merge(&e->d, &t);
    e->d.valid |= DMUS_OBJ_LOADED;
    ld_to_desc(&e->d, &d);
    IDirectMusicObject_SetDescriptor(e->obj, &d);
    ld_free(&t);
    ls_release(&s->IStream_iface);
    return lhr;
}
/* 77384562 (77383ffc / 77384167 / 77384272): the object's own description of what is there */
static HRESULT entry_describe(lentry *e)
{
    IDirectMusicObject *obj = NULL;
    HRESULT hr = create_object(&e->d.cls, &obj);
    if (FAILED(hr)) return hr;
    lstream *s = NULL;
    if (SUCCEEDED(hr = entry_stream(e, &s))) {
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        if (SUCCEEDED(hr = IDirectMusicObject_ParseDescriptor(obj, &s->IStream_iface, &d))) {
            ldesc t;
            ld_init(&t);
            ld_from_desc(&t, &d, NULL);
            ld_merge(&e->d, &t);
            ld_free(&t);
        }
        ls_release(&s->IStream_iface);
    }
    IDirectMusicObject_Release(obj);
    return hr;
}

/* ------------------------------------------------------------------ garbage collection */
/* 7738316d: an entry let go of: to the garbage, out of the loader's own children; its object released unless
 * something it reaches refers back to it (a script's never) */
static void ldr_retire(loader *L, lentry *e)
{
    e->next = L->garbage; L->garbage = e;
    entry_drop_kid(L->root, e);
    if (!IsEqualGUID(&e->d.cls, &G_ClsScript) && !gc_cyclic(e)) {
        ldr_drop(L, e->obj, FALSE);
        e->obj = NULL;
    }
}
/* 773843df: an entry with an object to let go of handed over to another (an equal one, or a new copy) so that the
 * entry itself can go to the garbage */
static HRESULT entry_replace(lentry *e)
{
    lclass *c = e->cls;
    lentry *other = NULL, *n = NULL;
    if (FAILED(class_find(c, &e->d, &other, e, e->obj)) || !other) {
        if (!(n = entry_from(c, &e->d))) return E_OUTOFMEMORY;
    }
    lentry **pp = &c->list;                 /* 773839e1 */
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp) *pp = e->next;
    e->next = NULL;
    if (n) { n->next = c->list; c->list = n; }
    c->ecur = NULL;
    return S_OK;
}
/* 77385086: the list's unmarked kept entries zombied and deleted (`scripts`: only scripts'), the marks of the
 * others cleared (not with `scripts`) */
static void gc_sweep(loader *L, lentry **list, BOOL scripts)
{
    lentry **pp = list;
    while (*pp) {
        lentry *e = *pp;
        if ((!scripts || IsEqualGUID(&e->d.cls, &G_ClsScript)) && (e->flags & EF_GC)) {
            if (!(e->flags & EF_MARK)) {
                ldr_drop(L, e->obj, TRUE);      /* zombied, then let go of (once the lock is) */
                e->obj = NULL;
                *pp = e->next;
                entry_free(L, e);
                continue;
            }
            if (!scripts) e->flags &= ~EF_MARK;
        }
        pp = &e->next;
    }
}
/* 77384b57: a class's cache emptied (streams too) */
static void class_clear(lclass *c)
{
    loader *L = c->ld;
    lentry *e = c->list;
    while (e) {
        lentry *next = e->next;
        ld_drop_stream(&e->d);
        if (e->obj) {
            if (!(e->flags & EF_GC)) {
                ldr_drop(L, e->obj, FALSE); e->obj = NULL;
                e->d.valid &= ~DMUS_OBJ_LOADED;
            } else if (SUCCEEDED(entry_replace(e))) ldr_retire(L, e);
            if (ld_noloc(&e->d)) class_remove(c, NULL);
        }
        /* (the next entry may have gone with the others that say nowhere to load from: then from the start again,
         * where the entries done have no object left to let go of) */
        lentry *x;
        for (x = c->list; x && x != next; x = x->next) {}
        e = !next ? NULL : x ? next : c->list;
    }
    c->ecur = NULL;
}

/* ------------------------------------------------------------------ IDirectMusicLoader8 */
static void ldr_destroy(loader *L)              /* 77382d51 */
{
    while (L->classes) { lclass *c = L->classes; L->classes = c->next; class_free(c); }
    while (L->garbage) { lentry *e = L->garbage; L->garbage = e->next; entry_free(NULL, e); }
    entry_free(NULL, L->root);
    DeleteCriticalSection(&L->cs);
    free(L);
}
static void ldr_addref_p(loader *L) { InterlockedIncrement(&L->pref); }
static void ldr_release_p(loader *L)            /* 77382ded */
{
    if (!InterlockedDecrement(&L->pref) && !L->ref) ldr_destroy(L);
}
static HRESULT WINAPI ldr_qi(IDirectMusicLoader8 *i, REFIID r, void **o)      /* 77381c63 */
{
    loader *L = ldo(i);
    if (!o || IsBadWritePtr(o, 4)) return E_POINTER;
    *o = NULL;
    if (IsEqualIID(r, &G_IidUnknown) || IsEqualIID(r, &G_IidLoader) || IsEqualIID(r, &G_IidLoader8)) *o = &L->IDirectMusicLoader8_iface;
    else if (IsEqualIID(r, &G_IidLoader8P)) *o = &L->p_vtbl;
    else return E_NOINTERFACE;
    InterlockedIncrement(&L->ref);
    return S_OK;
}
static ULONG WINAPI ldr_addref(IDirectMusicLoader8 *i) { return InterlockedIncrement(&ldo(i)->ref); }
static HRESULT WINAPI ldr_clearcache(IDirectMusicLoader8 *i, REFGUID cls);
static void WINAPI ldr_gc(IDirectMusicLoader8 *i);
static ULONG WINAPI ldr_release(IDirectMusicLoader8 *i)         /* 77383069 */
{
    loader *L = ldo(i);
    LONG r = InterlockedDecrement(&L->ref);
    if (r) return r;
    InterlockedIncrement(&L->ref);
    ldr_clearcache(i, &G_AllTypes);
    ldr_gc(i);
    r = InterlockedDecrement(&L->ref);
    if (!r && !L->pref) { ldr_destroy(L); return 0; }
    return r;
}
/* 77381e5c */
static HRESULT getobject(IDirectMusicLoader8 *i, DMUS_OBJECTDESC *desc, REFIID iid, void **out)
{
    loader *L = ldo(i);
    if (!desc || IsBadReadPtr(desc, sizeof(*desc)) || IsBadWritePtr(out, 4)) return E_POINTER;
    IStream *st = (desc->dwValidData & DMUS_OBJ_STREAM) ? desc->pStream : NULL;
    if (st && (IsBadReadPtr(st, 4) || IsBadReadPtr(*(void **)st, 4))) return E_POINTER;
    if (desc->dwValidData & DMUS_OBJ_FULLPATH) desc->dwValidData |= DMUS_OBJ_FILENAME;
    if (st) {
        HRESULT hr = IStream_Clone(st, &desc->pStream);
        if (FAILED(hr)) return hr;
    }
    ldr_lock(L);
    ldesc D;
    ld_init(&D);
    ld_from_desc(&D, desc, st);
    if (st) { IStream_Release(desc->pStream); desc->pStream = st; }
    HRESULT hr, hload = S_OK;
    lclass *c = NULL;
    lentry *e = NULL;
    IDirectMusicObject *obj = NULL;
    if (FAILED(hr = ldr_class(L, &D, &c, TRUE))) goto done;
    if (FAILED(class_find_or_add(c, &D, &e))) { hr = E_OUTOFMEMORY; goto done; }
    BOOL cache = c->cache, gc = cache && L->parent && (L->parent->flags & EF_GC);
    if (!e->obj) {
        lentry *saved = L->parent;
        hr = S_OK;
        if (gc) {
            L->parent = e;
            if (!IsEqualGUID(&e->d.obj, &G_DefaultGM)) hr = entry_gc(e);
        }
        if (SUCCEEDED(hr)) hr = hload = entry_load(e);
        if (gc) L->parent = saved;
        if (FAILED(hr)) goto fail;
        obj = e->obj;
        if (!cache) { e->d.valid &= ~DMUS_OBJ_LOADED; e->obj = NULL; }
        else IDirectMusicObject_AddRef(obj);
    } else {
        obj = e->obj;
        IDirectMusicObject_AddRef(obj);
        hr = S_OK;
    }
    if (gc) hr = entry_add_kid(L->parent, e);
    if (FAILED(hr)) {
        ldr_drop(L, obj, FALSE); obj = NULL;
fail:
        class_remove(c, e);
        if (D.valid & DMUS_OBJ_URL) hr = E_LDR_FORMATNOTSUPPORTED;
        goto done;
    }
    hr = IDirectMusicObject_QueryInterface(obj, iid, out);
    ldr_drop(L, obj, FALSE);
done:
    if (hr == E_FAIL) hr = E_LDR_FAILEDCREATE;
    ldr_unlock(L);
    ld_free(&D);
    return FAILED(hr) ? hr : hload;
}
/* the call logged (log=2): what was asked for and what came of it, as a load that fails is often what a game
 * trips over later (the names bounded: an application's need not end in a NUL) */
static HRESULT WINAPI ldr_getobject(IDirectMusicLoader8 *i, DMUS_OBJECTDESC *desc, REFIID iid, void **out)
{
    HRESULT hr = getobject(i, desc, iid, out);
    if (g_cfg.log_level >= 2 && desc && !IsBadReadPtr(desc, sizeof(*desc))) {
        char cls[64], obj[64];
        DWORD v = desc->dwValidData;
        LOG_CALL("music: Loader::GetObject(class %s, valid %lx%s%s%s%.260ls%s%s%.64ls%s%s, memory %lu) -> %08lx",
                 v & DMUS_OBJ_CLASS ? rl_guid_str(&desc->guidClass, cls) : "-", (unsigned long)v,
                 v & DMUS_OBJ_OBJECT ? ", object " : "", v & DMUS_OBJ_OBJECT ? rl_guid_str(&desc->guidObject, obj) : "",
                 v & DMUS_OBJ_FILENAME ? ", file '" : "", v & DMUS_OBJ_FILENAME ? desc->wszFileName : L"",
                 v & DMUS_OBJ_FILENAME ? "'" : "",
                 v & DMUS_OBJ_NAME ? ", name '" : "", v & DMUS_OBJ_NAME ? desc->wszName : L"", v & DMUS_OBJ_NAME ? "'" : "",
                 v & DMUS_OBJ_STREAM ? ", stream" : "",
                 (unsigned long)(v & DMUS_OBJ_MEMORY ? desc->llMemLength : 0), (unsigned long)hr);
    }
    return hr;
}
/* 773820f9 */
static HRESULT WINAPI ldr_setobject(IDirectMusicLoader8 *i, DMUS_OBJECTDESC *desc)
{
    loader *L = ldo(i);
    if (!desc || IsBadReadPtr(desc, 4) || desc->dwSize < sizeof(*desc) || IsBadReadPtr(desc, desc->dwSize)) return E_INVALIDARG;
    IStream *st = NULL;
    if (desc->dwValidData & DMUS_OBJ_STREAM) {
        st = desc->pStream;
        if (!st || IsBadReadPtr(st, 4) || IsBadReadPtr(*(void **)st, 4)) return E_POINTER;
        HRESULT hr = IStream_Clone(st, &desc->pStream);
        if (FAILED(hr)) return hr;
    }
    if (desc->dwValidData & DMUS_OBJ_FULLPATH) desc->dwValidData |= DMUS_OBJ_FILENAME;
    ldr_lock(L);
    ldesc D;
    ld_init(&D);
    ld_from_desc(&D, desc, st);
    lclass *c = NULL;
    lentry *e = NULL;
    HRESULT hr = ldr_class(L, &D, &c, TRUE);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = class_find_or_add(c, &D, &e))) {
        if (D.valid & LOCATION) e->d.valid &= ~(DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH | DMUS_OBJ_MEMORY | DMUS_OBJ_STREAM);
        ld_merge(&e->d, &D);
        if (e->d.valid & LOCATION) entry_describe(e);
        ld_to_desc(&e->d, desc);
    }
    if (st) {
        desc->dwValidData |= DMUS_OBJ_STREAM;
        IStream_Release(desc->pStream);
        desc->pStream = st;
    }
    ldr_unlock(L);
    ld_free(&D);
    return hr;
}
/* 77382274 */
static HRESULT setdir(IDirectMusicLoader8 *i, REFGUID cls, WCHAR *path, BOOL clear);
static HRESULT WINAPI ldr_setdir(IDirectMusicLoader8 *i, REFGUID cls, WCHAR *path, BOOL clear)
{
    HRESULT hr = setdir(i, cls, path, clear);
    char b[64];
    LOG_CALL("music: Loader::SetSearchDirectory(%s, '%.260ls', clear %d) -> %08lx", cls && !IsBadReadPtr(cls, sizeof(GUID)) ?
             rl_guid_str(cls, b) : "(null)", path && !IsBadReadPtr(path, 2) ? path : L"(null)", clear, (unsigned long)hr);
    return hr;
}
static HRESULT setdir(IDirectMusicLoader8 *i, REFGUID cls, WCHAR *path, BOOL clear)
{
    loader *L = ldo(i);
    if (path && IsBadReadPtr(path, 2)) return E_LDR_BADPATH;
    if (!path) return E_LDR_BADPATH;
    WCHAR dir[MAX_PATH + 2];
    lstrcpynW(dir, path, MAX_PATH);
    size_t n = wcslen(dir);
    if (!n || dir[n - 1] != L'\\') wcscat(dir, L"\\");
    DWORD a = GetFileAttributesW(dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return E_LDR_BADPATH;
    ldr_lock(L);
    HRESULT hr = S_OK;
    lclass *cs[2] = { NULL, NULL };
    if (IsEqualGUID(cls, &G_AllTypes)) {
        for (lclass *c = L->classes; c; c = c->next) {
            ld_set_file(&c->d, dir); c->d.valid |= DMUS_OBJ_FULLPATH;      /* 773849cc */
            if (clear) for (lentry **pp = &c->list; *pp; ) {
                lentry *e = *pp;
                if (!e->obj && !IsEqualGUID(&e->d.obj, &G_DefaultGM)) { *pp = e->next; entry_free(L, e); }
                else pp = &e->next;
            }
            c->ecur = NULL;
        }
        lstrcpynW(L->dir, dir, MAX_PATH);
        L->hasdir = TRUE;
    } else {
        ldesc D;
        ld_init(&D);
        D.cls = *cls; D.valid = DMUS_OBJ_CLASS;
        if (SUCCEEDED(hr = ldr_class(L, &D, &cs[0], TRUE))) {
            lclass *c = cs[0];
            ld_set_file(&c->d, dir); c->d.valid |= DMUS_OBJ_FULLPATH;
            if (clear) for (lentry **pp = &c->list; *pp; ) {
                lentry *e = *pp;
                if (!e->obj && !IsEqualGUID(&e->d.obj, &G_DefaultGM)) { *pp = e->next; entry_free(L, e); }
                else pp = &e->next;
            }
            c->ecur = NULL;
        }
        ld_free(&D);
    }
    ldr_unlock(L);
    return hr;
}
/* 77384bfc: the class's directory searched for *.ext; each file described (again only if it changed and is not
 * loaded); S_FALSE if none was */
static HRESULT class_scan(lclass *c, const WCHAR *ext)
{
    IDirectMusicObject *obj = NULL;
    HRESULT hr = create_object(&c->d.cls, &obj);
    if (FAILED(hr)) return hr;
    WCHAR pat[MAX_PATH * 2], dir[MAX_PATH];
    class_dir(c, dir);
    lstrcpynW(pat, dir, MAX_PATH);
    wcscat(pat, L"*.");
    if (ext && wcslen(ext) < 0x207 - wcslen(pat)) wcscat(pat, ext);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    hr = S_FALSE;
    if (f == INVALID_HANDLE_VALUE) { IDirectMusicObject_Release(obj); return hr; }
    for (lentry **pp = &c->list; *pp; ) {           /* 773848dc: what an earlier scan found for this extension */
        lentry *e = *pp;
        const WCHAR *dot = e->d.file ? wcschr(e->d.file, L'.') : NULL;
        if (!IsEqualGUID(&e->d.obj, &G_DefaultGM) && !e->obj && dot && dot[1] && ext && !_wcsicmp(ext, dot + 1)) {
            *pp = e->next; entry_free(c->ld, e);
        } else pp = &e->next;
    }
    c->ecur = NULL;
    lentry *found = NULL;
    do {
        ldesc D;
        ld_init(&D);
        WCHAR path[MAX_PATH * 2];
        lstrcpynW(path, dir, MAX_PATH);
        wcsncat(path, fd.cFileName, MAX_PATH - 1 - wcslen(path));
        ld_set_file(&D, path);
        D.valid = DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH | DMUS_OBJ_DATE;
        D.date = fd.ftLastWriteTime;
        lentry *e = NULL;
        BOOL parse = TRUE;
        if (SUCCEEDED(class_find(c, &D, &e, NULL, NULL)) && e) {
            parse = fd.nFileSizeLow != e->d.fsize || memcmp(&fd.ftLastWriteTime, &e->d.date, sizeof(FILETIME));
            if (e->obj) parse = FALSE;
        }
        lstream *s = parse ? ls_new(c->ld, 0) : NULL;
        if (s && SUCCEEDED(ls_open_file(s, path))) {
            DMUS_OBJECTDESC d;
            memset(&d, 0, sizeof(d));
            d.dwSize = sizeof(d);
            if (SUCCEEDED(IDirectMusicObject_ParseDescriptor(obj, &s->IStream_iface, &d))) {
                hr = S_OK;
                ldesc t;
                ld_init(&t);
                ld_from_desc(&t, &d, NULL);
                ld_merge(&D, &t);
                ld_free(&t);
                D.fsize = fd.nFileSizeLow;
                D.date = fd.ftLastWriteTime;
                if (!e) {
                    lentry *n = entry_from(c, &D);
                    if (n) { n->d.fsize = D.fsize; n->flags |= 0xc; n->next = found; found = n; }
                } else {
                    ld_copy(&e->d, &D);
                    e->d.fsize = D.fsize;
                    e->flags |= 0xc;
                }
            }
        }
        if (s) ls_release(&s->IStream_iface);
        ld_free(&D);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    while (found) { lentry *n = found->next; found->next = c->list; c->list = found; found = n; }
    c->ecur = NULL;
    c->scanned = TRUE;
    IDirectMusicObject_Release(obj);
    return hr;
}
/* 77382e25 (a cache file, 77382ad7, is not written) */
static HRESULT WINAPI ldr_scandir(IDirectMusicLoader8 *i, REFGUID cls, WCHAR *ext, WCHAR *cachefile)
{
    loader *L = ldo(i);
    if ((ext && IsBadReadPtr(ext, 2)) || (cachefile && IsBadReadPtr(cachefile, 2))) return S_OK;
    ldr_lock(L);
    for (lclass *c = L->classes; c; c = c->next)
        for (lentry *e = c->list; e; e = e->next) e->flags = (e->flags & ~0xdu) | 2;     /* 773839c2 */
    if (IsEqualGUID(cls, &G_AllTypes)) { ldr_unlock(L); return S_OK; }
    ldesc D;
    ld_init(&D);
    D.cls = *cls; D.valid = DMUS_OBJ_CLASS;
    lclass *c = NULL;
    HRESULT hr = S_OK;
    ldr_class(L, &D, &c, TRUE);
    if (c) hr = class_scan(c, ext ? ext : L"");
    ldr_unlock(L);
    ld_free(&D);
    if (cachefile) LOG_INFO("music: Loader::ScanDirectory: the cache file is not written");
    return hr;
}
/* 77382443: an object's entry (S_FALSE if the entry its descriptor finds holds another), in two steps: the object's
 * descriptor asked for before the loader's lock is taken (it is the object's code), the entry found with it held */
static HRESULT ldr_desc_of(IDirectMusicObject *obj, ldesc *D)
{
    ld_init(D);
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    HRESULT hr = IDirectMusicObject_GetDescriptor(obj, &d);
    if (SUCCEEDED(hr)) ld_from_desc(D, &d, NULL);
    return hr;
}
static HRESULT ldr_entry_find(loader *L, const ldesc *D, IDirectMusicObject *obj, lentry **out)
{
    *out = NULL;
    lclass *c = NULL;
    lentry *e = NULL;
    HRESULT hr;
    if (SUCCEEDED(hr = ldr_class(L, D, &c, FALSE)) && SUCCEEDED(hr = class_find(c, D, &e, NULL, obj))) {
        if (e->obj == obj) { *out = e; hr = S_OK; }
        else hr = S_FALSE;
    }
    return hr;
}
/* 77382510 */
static HRESULT WINAPI ldr_cacheobj(IDirectMusicLoader8 *i, IDirectMusicObject *obj)
{
    loader *L = ldo(i);
    if (!obj || IsBadReadPtr(obj, 4) || IsBadReadPtr(*(void **)obj, 4)) return S_OK;
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    if (FAILED(IDirectMusicObject_GetDescriptor(obj, &d))) return S_OK;
    ldesc D;
    ld_init(&D);
    ld_from_desc(&D, &d, NULL);
    ldr_lock(L);
    lclass *c = NULL;
    lentry *e = NULL;
    HRESULT hr;
    if (SUCCEEDED(hr = ldr_class(L, &D, &c, FALSE)) && SUCCEEDED(hr = class_find(c, &D, &e, NULL, NULL))) {
        if (e->obj && e->obj != obj) { ldr_drop(L, e->obj, FALSE); e->obj = NULL; }
        if (e->obj != obj) { e->obj = obj; e->d.valid |= DMUS_OBJ_LOADED; IDirectMusicObject_AddRef(obj); }
    }
    ldr_unlock(L);
    ld_free(&D);
    return hr;
}
/* 773831d2 */
static HRESULT WINAPI ldr_releaseobj(IDirectMusicLoader8 *i, IDirectMusicObject *obj)
{
    loader *L = ldo(i);
    if (!obj || IsBadReadPtr(obj, 4) || IsBadReadPtr(*(void **)obj, 4)) return E_POINTER;
    ldesc D;
    HRESULT hr = ldr_desc_of(obj, &D);
    if (FAILED(hr)) { ld_free(&D); return hr; }
    ldr_lock(L);
    lentry *e = NULL;
    hr = ldr_entry_find(L, &D, obj, &e);
    if (hr == S_OK && e) {
        lclass *c = e->cls;
        ld_drop_stream(&e->d);
        if (!(e->flags & EF_GC)) {
            IDirectMusicObject_Release(e->obj); e->obj = NULL;
            e->d.valid &= ~DMUS_OBJ_LOADED;
        } else if (SUCCEEDED(hr = entry_replace(e))) ldr_retire(L, e);
        if (ld_noloc(&e->d)) class_remove(c, NULL);
    }
    ldr_unlock(L);
    ld_free(&D);
    return hr;
}
/* 77382642 */
static HRESULT WINAPI ldr_clearcache(IDirectMusicLoader8 *i, REFGUID cls)
{
    loader *L = ldo(i);
    ldr_lock(L);
    if (IsEqualGUID(cls, &G_AllTypes)) for (lclass *c = L->classes; c; c = c->next) class_clear(c);
    else {
        ldesc D;
        ld_init(&D);
        D.cls = *cls; D.valid = DMUS_OBJ_CLASS;
        lclass *c = NULL;
        if (SUCCEEDED(ldr_class(L, &D, &c, FALSE)) && c) class_clear(c);
        ld_free(&D);
    }
    ldr_unlock(L);
    return S_OK;
}
/* 77382710 */
static HRESULT WINAPI ldr_enablecache(IDirectMusicLoader8 *i, REFGUID cls, BOOL on)
{
    loader *L = ldo(i);
    BOOL same = FALSE;
    ldr_lock(L);
    if (IsEqualGUID(cls, &G_AllTypes)) {
        for (lclass *c = L->classes; c; c = c->next) {
            if (!on) class_clear(c);
            if (c->cache == on) same = TRUE; else c->cache = on;
        }
        L->defcache = on;
    } else {
        ldesc D;
        ld_init(&D);
        D.cls = *cls; D.valid = DMUS_OBJ_CLASS;
        lclass *c = NULL;
        ldr_class(L, &D, &c, TRUE);
        if (c) {
            if (!on) class_clear(c);
            if (c->cache == on) same = TRUE; else c->cache = on;
        }
        ld_free(&D);
    }
    ldr_unlock(L);
    return same ? S_FALSE : S_OK;
}
/* 773827c9 / 77384452 */
static HRESULT WINAPI ldr_enum(IDirectMusicLoader8 *i, REFGUID cls, DWORD idx, DMUS_OBJECTDESC *desc)
{
    loader *L = ldo(i);
    if (!desc || IsBadReadPtr(desc, 4) || desc->dwSize < sizeof(*desc) || IsBadWritePtr(desc, desc->dwSize)) return E_INVALIDARG;
    ldesc D, t;
    ld_init(&D); ld_init(&t);
    D.cls = *cls; D.valid = DMUS_OBJ_CLASS;
    ldr_lock(L);
    lclass *c = NULL;
    HRESULT hr = ldr_class(L, &D, &c, TRUE);
    if (SUCCEEDED(hr)) {
        if (idx < c->eidx || !c->ecur) { c->eidx = 0; c->ecur = c->list; }
        while (c->eidx < idx && c->ecur) { c->eidx++; c->ecur = c->ecur->next; }
        if (!c->ecur) hr = S_FALSE;
        else { ld_copy(&t, &c->ecur->d); hr = S_OK; }
        if (hr == S_OK) ld_to_desc(&t, desc);
    }
    ldr_unlock(L);
    ld_free(&D); ld_free(&t);
    return hr;
}
/* 773830f7 */
static void WINAPI ldr_gc(IDirectMusicLoader8 *i)
{
    loader *L = ldo(i);
    ldr_lock(L);
    if (L->root) {
        gc_walk(L->root, NULL, TRUE);
        gc_sweep(L, &L->garbage, TRUE);
        for (lclass *c = L->classes; c; c = c->next) { gc_sweep(L, &c->list, FALSE); c->ecur = NULL; }
        gc_sweep(L, &L->garbage, FALSE);
        L->root->flags &= ~EF_MARK;
    }
    ldr_unlock(L);
}
/* 77382898 */
static HRESULT WINAPI ldr_releasebyunk(IDirectMusicLoader8 *i, IUnknown *u)
{
    if (!u || IsBadReadPtr(u, 4) || IsBadReadPtr(*(void **)u, 4)) return E_POINTER;
    IDirectMusicObject *o = NULL;
    HRESULT hr = IUnknown_QueryInterface(u, &G_IidObject, (void **)&o);
    if (FAILED(hr)) return hr;
    hr = ldr_releaseobj(i, o);
    IDirectMusicObject_Release(o);
    return hr;
}
/* 77381db5: as a full path, then as a file name */
static HRESULT WINAPI ldr_loadfile(IDirectMusicLoader8 *i, REFGUID cls, REFIID iid, WCHAR *file, void **out)
{
    if (!file || IsBadReadPtr(file, 2)) return E_POINTER;
    DMUS_OBJECTDESC d;
    memset(&d, 0, sizeof(d));
    d.dwSize = sizeof(d);
    d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH;
    lstrcpynW(d.wszFileName, file, DMUS_MAX_FILENAME);
    d.guidClass = *cls;
    HRESULT hr = ldr_getobject(i, &d, iid, out);
    if (FAILED(hr)) {
        d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_FILENAME;
        hr = ldr_getobject(i, &d, iid, out);
    }
    return hr;
}
static const IDirectMusicLoader8Vtbl ldr_vtbl = { ldr_qi, ldr_addref, ldr_release, ldr_getobject, ldr_setobject,
    ldr_setdir, ldr_scandir, ldr_cacheobj, ldr_releaseobj, ldr_clearcache, ldr_enablecache, ldr_enum, ldr_gc,
    ldr_releasebyunk, ldr_loadfile };

/* IDirectMusicLoader8P (scripts): dynamic references */
static HRESULT WINAPI lp_qi(void *i, REFIID r, void **o) { return ldr_qi(&ldp(i)->IDirectMusicLoader8_iface, r, o); }
static ULONG WINAPI lp_addref(void *i) { return ldr_addref(&ldp(i)->IDirectMusicLoader8_iface); }
static ULONG WINAPI lp_release(void *i) { return ldr_release(&ldp(i)->IDirectMusicLoader8_iface); }
static HRESULT WINAPI lp_getdyn(void *i, IDirectMusicObject *src, DMUS_OBJECTDESC *d, REFIID iid, void **out)  /* 77382903 */
{
    loader *L = ldp(i);
    if (!src || IsBadReadPtr(src, 4)) return E_POINTER;
    ldesc D;
    HRESULT hr = ldr_desc_of(src, &D);
    if (FAILED(hr)) { ld_free(&D); return hr; }
    ldr_lock(L);
    lentry *e = NULL;
    hr = ldr_entry_find(L, &D, src, &e);
    ld_free(&D);
    if (SUCCEEDED(hr)) {
        if (!e) hr = E_LDR_OBJECTNOTFOUND;
        else {
            lentry *saved = L->parent;
            L->parent = e;
            hr = ldr_getobject(&L->IDirectMusicLoader8_iface, d, iid, out);
            L->parent = saved;
        }
    }
    ldr_unlock(L);
    return hr;
}
static HRESULT WINAPI lp_reportdyn(void *i, IDirectMusicObject *src, IUnknown *obj)    /* 773829a1 */
{
    loader *L = ldp(i);
    if (!src || IsBadReadPtr(src, 4) || !obj || IsBadReadPtr(obj, 4)) return E_POINTER;
    /* (the objects asked first, the entries found with the lock held) */
    ldesc SD, OD;
    ld_init(&OD);
    HRESULT hr = ldr_desc_of(src, &SD), ohr = E_FAIL;
    IDirectMusicObject *o = NULL;
    if (SUCCEEDED(hr)) {
        ohr = IUnknown_QueryInterface(obj, &G_IidObject, (void **)&o);
        if (SUCCEEDED(ohr)) { ld_free(&OD); ohr = ldr_desc_of(o, &OD); }
    }
    ldr_lock(L);
    lentry *se = NULL, *oe = NULL;
    if (SUCCEEDED(hr)) hr = ldr_entry_find(L, &SD, src, &se);
    if (hr == S_FALSE) hr = E_LDR_OBJECTNOTFOUND;
    if (SUCCEEDED(hr)) {
        if (!o) { hr = ohr == E_NOINTERFACE ? S_OK : ohr; }
        else if (FAILED(ohr)) hr = ohr;
        else {
            hr = ldr_entry_find(L, &OD, o, &oe);
            if (hr == S_FALSE) hr = E_LDR_OBJECTNOTFOUND;
            if (SUCCEEDED(hr)) hr = entry_add_kid(se, oe);
        }
    }
    ldr_unlock(L);
    if (o) IDirectMusicObject_Release(o);
    ld_free(&SD); ld_free(&OD);
    return hr;
}
static ULONG WINAPI lp_addrefp(void *i) { loader *L = ldp(i); ldr_addref_p(L); return L->pref; }
static ULONG WINAPI lp_releasep(void *i) { loader *L = ldp(i); LONG r = L->pref - 1; ldr_release_p(L); return r; }
static const ldr8p_vtbl lp_vtbl = { lp_qi, lp_addref, lp_release, lp_getdyn, lp_reportdyn, lp_addrefp, lp_releasep };

const char *dm_gm_dls_path(void);
/* 77382ff6 / 77382bfd: the default GM collection described (the loader's GM file), the loader's own entry */
HRESULT rm_loader_create(REFIID riid, void **out)
{
    *out = NULL;
    loader *L = calloc(1, sizeof(*L));
    if (!L) return E_OUTOFMEMORY;
    L->IDirectMusicLoader8_iface.lpVtbl = (IDirectMusicLoader8Vtbl *)&ldr_vtbl;
    L->p_vtbl = &lp_vtbl;
    L->ref = 1;
    L->defcache = TRUE;
    InitializeCriticalSection(&L->cs);
    L->trash_tail = &L->trash;
    const char *gm = dm_gm_dls_path();
    if (gm && gm[0]) {
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwValidData = DMUS_OBJ_OBJECT | DMUS_OBJ_CLASS | DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH;
        d.guidClass = G_ClsCollection;
        d.guidObject = G_DefaultGM;
        MultiByteToWideChar(CP_ACP, 0, gm, -1, d.wszFileName, DMUS_MAX_FILENAME);
        ldr_setobject(&L->IDirectMusicLoader8_iface, &d);
    }
    L->root = entry_new(NULL);
    if (L->root && SUCCEEDED(entry_gc(L->root))) L->parent = L->root;
    else { entry_free(NULL, L->root); L->root = NULL; }
    HRESULT hr = ldr_qi(&L->IDirectMusicLoader8_iface, riid, out);
    ldr_release(&L->IDirectMusicLoader8_iface);
    return hr;
}

/* ------------------------------------------------------------------ the container (77386954) */
typedef struct citem {
    struct citem *next;
    IDirectMusicObject *obj;
    DWORD flags;                        /* (never set: the KEEP flag of 'cobh' is not kept) */
    DMUS_OBJECTDESC desc;
    BOOL embedded;
    WCHAR *alias;
} citem;
typedef struct container {
    IDirectMusicContainer IDirectMusicContainer_iface;
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    IUnknown IDirectMusicObjectP_iface;
    LONG ref;
    IStream *stream;
    citem *items;
    DWORD flags;                        /* 'cnth' */
    DMUS_OBJECTDESC desc;
    BOOL zombie;
} container;
static container *cto(IDirectMusicContainer *i) { return CONTAINING_RECORD(i, container, IDirectMusicContainer_iface); }
static container *cob(IDirectMusicObject *i) { return CONTAINING_RECORD(i, container, IDirectMusicObject_iface); }
static container *cps(IPersistStream *i) { return CONTAINING_RECORD(i, container, IPersistStream_iface); }
static container *czo(IUnknown *i) { return CONTAINING_RECORD(i, container, IDirectMusicObjectP_iface); }

static void item_free(citem *it)                /* 773868ee */
{
    if (it->obj) IDirectMusicObject_Release(it->obj);
    if ((it->desc.dwValidData & DMUS_OBJ_STREAM) && it->desc.pStream) IStream_Release(it->desc.pStream);
    free(it->alias);
    free(it);
}
/* 773869d3: the objects released from the loader's cache, then let go */
static void ct_release_items(container *c)
{
    IDirectMusicLoader *l = NULL;
    IDirectMusicGetLoader *g = NULL;
    if (c->stream && SUCCEEDED(IStream_QueryInterface(c->stream, &G_IidGetLoader, (void **)&g)) && g) {
        IDirectMusicGetLoader_GetLoader(g, &l);
        IDirectMusicGetLoader_Release(g);
    }
    while (c->items) {
        citem *it = c->items;
        c->items = it->next;
        if (it->obj) {
            if (l && !(it->flags & DMUS_CONTAINED_OBJF_KEEP)) IDirectMusicLoader_ReleaseObject(l, it->obj);
            IDirectMusicObject_Release(it->obj);
            it->obj = NULL;
        }
        item_free(it);
    }
    if (l) IDirectMusicLoader_Release(l);
}
static void ct_destroy(container *c)            /* 77386ccf */
{
    ct_release_items(c);
    if (c->stream) IStream_Release(c->stream);
    free(c);
}
static HRESULT WINAPI ct_qi(IDirectMusicContainer *i, REFIID r, void **o)      /* 77386293 */
{
    container *c = cto(i);
    if (IsEqualIID(r, &G_IidUnknown) || IsEqualIID(r, &G_IidContainer)) *o = &c->IDirectMusicContainer_iface;
    else if (IsEqualIID(r, &G_IidObject)) *o = &c->IDirectMusicObject_iface;
    else if (IsEqualIID(r, &G_IidObjectP)) *o = &c->IDirectMusicObjectP_iface;
    else if (IsEqualIID(r, &IID_IPersistStream) || IsEqualIID(r, &IID_IPersist)) *o = &c->IPersistStream_iface;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&c->ref);
    return S_OK;
}
static ULONG WINAPI ct_addref(IDirectMusicContainer *i) { return InterlockedIncrement(&cto(i)->ref); }
static ULONG WINAPI ct_release(IDirectMusicContainer *i)
{
    container *c = cto(i);
    LONG r = InterlockedDecrement(&c->ref);
    if (!r) ct_destroy(c);
    return r;
}
/* 7738633b: the idx'th object of a class (GUID_DirectMusicAllTypes: any), its descriptor as the loader filled it
 * in, and its alias */
static HRESULT WINAPI ct_enum(IDirectMusicContainer *i, REFGUID cls, DWORD idx, DMUS_OBJECTDESC *d, WCHAR *alias)
{
    container *c = cto(i);
    if ((d && IsBadWritePtr(d, 4)) || (alias && IsBadWritePtr(alias, MAX_PATH)) || IsBadReadPtr(cls, sizeof(GUID))) return E_POINTER;
    if (c->zombie) return S_ZOMBIE;
    DWORD k = 0;
    for (citem *it = c->items; it; it = it->next) {
        if (!IsEqualGUID(cls, &G_AllTypes) && !IsEqualGUID(cls, &it->desc.guidClass)) continue;
        if (k++ != idx) continue;
        if (d) {
            DWORD n = it->desc.dwSize < d->dwSize ? it->desc.dwSize : d->dwSize;
            memcpy(d, &it->desc, n);
            if ((d->dwValidData & DMUS_OBJ_STREAM) && d->pStream) IStream_AddRef(d->pStream);
        }
        if (!alias) return S_OK;
        const WCHAR *a = it->alias ? it->alias : L"";
        for (DWORD j = 0; j < MAX_PATH; j++) { alias[j] = a[j]; if (!a[j]) return S_OK; }
        alias[MAX_PATH - 1] = 0;
        return RIFF_END & 0x7fffffff;           /* (77386218: truncated) */
    }
    return S_FALSE;
}
static const IDirectMusicContainerVtbl ct_vtbl = { ct_qi, ct_addref, ct_release, ct_enum };

static HRESULT WINAPI co_qi(IDirectMusicObject *i, REFIID r, void **o) { return ct_qi(&cob(i)->IDirectMusicContainer_iface, r, o); }
static ULONG WINAPI co_addref(IDirectMusicObject *i) { return ct_addref(&cob(i)->IDirectMusicContainer_iface); }
static ULONG WINAPI co_release(IDirectMusicObject *i) { return ct_release(&cob(i)->IDirectMusicContainer_iface); }
static HRESULT WINAPI co_getdesc(IDirectMusicObject *i, DMUS_OBJECTDESC *d)    /* 77386497 */
{
    container *c = cob(i);
    if (!d || IsBadWritePtr(d, sizeof(*d))) return E_POINTER;
    if (c->zombie) return S_ZOMBIE;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    d->guidClass = G_ClsContainer;
    d->guidObject = c->desc.guidObject; d->ftDate = c->desc.ftDate; d->vVersion = c->desc.vVersion;
    memcpy(d->wszName, c->desc.wszName, sizeof(d->wszName));
    memcpy(d->wszCategory, c->desc.wszCategory, sizeof(d->wszCategory));
    memcpy(d->wszFileName, c->desc.wszFileName, sizeof(d->wszFileName));
    d->dwValidData = c->desc.dwValidData | DMUS_OBJ_CLASS;
    return S_OK;
}
static HRESULT WINAPI co_setdesc(IDirectMusicObject *i, DMUS_OBJECTDESC *d)    /* 77386548 */
{
    container *c = cob(i);
    if (!d || IsBadReadPtr(d, sizeof(*d))) return E_POINTER;
    if (c->zombie) return S_ZOMBIE;
    DWORD v = d->dwValidData, took = 0;
    if (v & DMUS_OBJ_OBJECT) { c->desc.guidObject = d->guidObject; took |= DMUS_OBJ_OBJECT; }
    if (v & DMUS_OBJ_NAME) { memcpy(c->desc.wszName, d->wszName, sizeof(d->wszName)); took |= DMUS_OBJ_NAME; }
    if (v & DMUS_OBJ_CATEGORY) { memcpy(c->desc.wszCategory, d->wszCategory, sizeof(d->wszCategory)); took |= DMUS_OBJ_CATEGORY; }
    if (v & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH)) {
        memcpy(c->desc.wszFileName, d->wszFileName, sizeof(d->wszFileName));
        took |= v & (DMUS_OBJ_FILENAME | DMUS_OBJ_FULLPATH);
    }
    if (v & DMUS_OBJ_VERSION) { c->desc.vVersion = d->vVersion; took |= DMUS_OBJ_VERSION; }
    if (v & DMUS_OBJ_DATE) { c->desc.ftDate = d->ftDate; took |= DMUS_OBJ_DATE; }
    c->desc.dwValidData |= took;
    if (v & ~took) { d->dwValidData = took; return S_FALSE; }
    return S_OK;
}

/* a RIFF reader over a stream (dmloader's 77385716 / 77385832): chunks by absolute positions */
typedef struct { DWORD id, size, type; ULONGLONG start; } rchunk;     /* start: where the data begins */
static HRESULT r_descend(IStream *s, const rchunk *parent, rchunk *c)
{
    BYTE h[8];
    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER p;
    if (FAILED(IStream_Seek(s, z, STREAM_SEEK_CUR, &p))) return RIFF_END;
    if (parent && p.QuadPart >= parent->start + parent->size) return RIFF_END;
    if (!read_all(s, h, 8)) return RIFF_END;
    c->id = *(DWORD *)h; c->size = *(DWORD *)(h + 4); c->type = 0;
    c->start = p.QuadPart + 8;
    if (c->id == FCC('R','I','F','F') || c->id == FCC('L','I','S','T')) {
        if (!read_all(s, h, 4)) return RIFF_END;
        c->type = *(DWORD *)h;
    }
    return S_OK;
}
static HRESULT r_ascend(IStream *s, const rchunk *c)
{
    LARGE_INTEGER p;
    p.QuadPart = (LONGLONG)(c->start + c->size + (c->size & 1));
    return SUCCEEDED(IStream_Seek(s, p, STREAM_SEEK_SET, NULL)) ? S_OK : (HRESULT)0x88781115;
}
static HRESULT r_read(IStream *s, void *p, DWORD n) { return read_all(s, p, n) ? S_OK : E_FAIL; }

/* 77386a8d: a DMRF reference as a descriptor (it must name an object, a name or a file) */
static HRESULT ref_parse(IStream *s, const rchunk *list, DMUS_OBJECTDESC *d)
{
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    HRESULT hr = S_OK;
    rchunk c;
    while (r_descend(s, list, &c) == S_OK) {
        DWORD n;
        switch (c.id) {
        case FCC('g','u','i','d'): if (SUCCEEDED(hr = r_read(s, &d->guidObject, 16))) d->dwValidData |= DMUS_OBJ_OBJECT; break;
        case FCC('f','i','l','e'):
            n = c.size < 0x208 ? c.size : 0x208;
            if (SUCCEEDED(hr = r_read(s, d->wszFileName, n))) { d->dwValidData |= DMUS_OBJ_FILENAME; d->wszFileName[MAX_PATH - 1] = 0; }
            break;
        case FCC('n','a','m','e'):
            n = c.size < 0x80 ? c.size : 0x80;
            if (SUCCEEDED(hr = r_read(s, d->wszName, n))) { d->dwValidData |= DMUS_OBJ_NAME; d->wszName[63] = 0; }
            break;
        case FCC('d','a','t','e'): if (SUCCEEDED(hr = r_read(s, &d->ftDate, 8))) d->dwValidData |= DMUS_OBJ_DATE; break;
        case FCC('c','a','t',' '):
            n = c.size < 0x80 ? c.size : 0x80;
            if (SUCCEEDED(hr = r_read(s, d->wszCategory, n))) { d->dwValidData |= DMUS_OBJ_CATEGORY; d->wszCategory[63] = 0; }
            break;
        case FCC('r','e','f','h'): {
            BYTE h[20];
            if (SUCCEEDED(hr = r_read(s, h, 20))) { memcpy(&d->guidClass, h, 16); d->dwValidData |= *(DWORD *)(h + 16) | DMUS_OBJ_CLASS; }
            break;
        }
        case FCC('v','e','r','s'): {
            DMUS_VERSION v;
            if (SUCCEEDED(hr = r_read(s, &v, 8))) { d->dwValidData |= DMUS_OBJ_VERSION; d->vVersion = v; }
            break;
        }
        }
        if (SUCCEEDED(hr) && FAILED(r_ascend(s, &c))) hr = E_CANNOTREAD;
    }
    if (!(d->dwValidData & (DMUS_OBJ_OBJECT | DMUS_OBJ_NAME | DMUS_OBJ_FILENAME))) hr = E_BADREF;
    return hr;
}
/* 77386d1d: a 'cobl' list: the alias ('coba'), the header ('cobh'), then the object (embedded, as a stream
 * position) or its DMRF reference; described to the loader */
static HRESULT ct_item(container *c, IStream *s, const rchunk *list, IDirectMusicLoader *l)
{
    WCHAR *alias = NULL;
    rchunk k, prev;
    HRESULT hr = r_descend(s, list, &k);
    if (FAILED(hr)) return S_OK;
    if (k.id == FCC('c','o','b','a')) {
        if (!(k.size & 1)) {
            alias = calloc(k.size / 2 + 1, sizeof(WCHAR));
            if (!alias) return S_OK;
            if (FAILED(r_read(s, alias, k.size))) { free(alias); return S_OK; }
        }
        r_ascend(s, &k);
        if (FAILED(r_descend(s, list, &k))) { free(alias); return S_OK; }
    }
    if (k.id != FCC('c','o','b','h')) { free(alias); return S_OK; }
    BYTE h[0x1c];
    if (FAILED(r_read(s, h, sizeof(h)))) { free(alias); return S_OK; }
    DWORD ckid = *(DWORD *)(h + 20), fcc = *(DWORD *)(h + 24);
    if (!ckid && !fcc) { free(alias); return S_OK; }
    r_ascend(s, &k);
    prev = k;
    hr = S_OK;
    while (r_descend(s, list, &k) == S_OK) {
        if (((k.id == FCC('L','I','S','T') || k.id == FCC('R','I','F','F')) && k.type == fcc) || k.id == ckid) {
            BOOL ref = k.id == FCC('L','I','S','T') && k.type == FCC('D','M','R','F');
            citem *it = calloc(1, sizeof(*it));
            if (!it) { hr = E_OUTOFMEMORY; break; }
            it->desc.dwSize = sizeof(it->desc);
            it->embedded = !ref;
            if (ref) hr = ref_parse(s, &k, &it->desc);
            else {
                r_ascend(s, &prev);                 /* (back to where the object's form starts) */
                it->desc.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_STREAM;
                memcpy(&it->desc.guidClass, h, 16);
                it->desc.pStream = s;
                IStream_AddRef(s);
            }
            if (FAILED(hr)) item_free(it);
            else {
                IDirectMusicLoader_SetObject(l, &it->desc);
                if (it->desc.dwValidData & DMUS_OBJ_STREAM) {
                    it->desc.dwValidData &= ~DMUS_OBJ_STREAM;
                    if (it->desc.pStream) IStream_Release(it->desc.pStream);
                    it->desc.pStream = NULL;
                }
                it->alias = alias; alias = NULL;
                citem **pp = &c->items;
                while (*pp) pp = &(*pp)->next;
                *pp = it;
            }
        }
        if (FAILED(hr)) break;
        r_ascend(s, &k);
        prev = k;
    }
    free(alias);
    return hr;
}
/* 7738708e */
static HRESULT ct_load(container *c, IStream *s, IDirectMusicLoader *l)
{
    rchunk form, k;
    HRESULT hr;
    do { if (FAILED(hr = r_descend(s, NULL, &form))) return hr; } while (0);
    if (form.id != FCC('R','I','F','F') || form.type != FCC('D','M','C','N')) return RIFF_END;
    c->desc.dwValidData = 0;
    while ((hr = r_descend(s, &form, &k)) == S_OK) {
        if (k.id == FCC('R','I','F','F') || k.id == FCC('L','I','S','T')) {
            if (k.type == FCC('U','N','F','O')) {
                rchunk u;
                while (r_descend(s, &k, &u) == S_OK) {
                    if (u.id == FCC('U','N','A','M')) {
                        DWORD n = u.size < 0x80 ? u.size : 0x80;
                        if (SUCCEEDED(r_read(s, c->desc.wszName, n))) c->desc.dwValidData |= DMUS_OBJ_NAME;
                        c->desc.wszName[DMUS_MAX_NAME - 1] = 0;     /* (64 characters fill it: no terminator) */
                    }
                    r_ascend(s, &u);
                }
            } else if (k.type == FCC('c','o','s','l')) {
                rchunk b;
                while ((hr = r_descend(s, &k, &b)) == S_OK) {
                    if ((b.id == FCC('R','I','F','F') || b.id == FCC('L','I','S','T')) && b.type == FCC('c','o','b','l'))
                        hr = ct_item(c, s, &b, l);
                    if (FAILED(hr)) break;
                    hr = r_ascend(s, &b);
                    if (FAILED(hr)) break;
                }
                if (hr == RIFF_END) hr = S_OK;
            }
        } else if (k.id == FCC('g','u','i','d')) {
            if (k.size == 16) {
                if (FAILED(hr = r_read(s, &c->desc.guidObject, 16))) break;
                c->desc.dwValidData |= DMUS_OBJ_OBJECT;
            }
        } else if (k.id == FCC('d','a','t','e')) {
            if (FAILED(hr = r_read(s, &c->desc.ftDate, 8))) break;
            c->desc.dwValidData |= DMUS_OBJ_DATE;
        } else if (k.id == FCC('c','a','t',' ')) {
            if (FAILED(hr = r_read(s, c->desc.wszCategory, k.size < 0x80 ? k.size : 0x80))) break;
            c->desc.wszCategory[DMUS_MAX_CATEGORY - 1] = 0;
            c->desc.dwValidData |= DMUS_OBJ_CATEGORY;
        } else if (k.id == FCC('c','n','t','h')) {
            DWORD f = 0;
            if (FAILED(hr = r_read(s, &f, k.size < 4 ? k.size : 4))) break;
            c->flags = f;
        } else if (k.id == FCC('v','e','r','s')) {
            if (FAILED(hr = r_read(s, &c->desc.vVersion, 8))) break;
            c->desc.dwValidData |= DMUS_OBJ_VERSION;
        }
        if (FAILED(hr)) break;
        if (FAILED(hr = r_ascend(s, &k))) break;
    }
    if (hr == RIFF_END) hr = S_OK;
    if (FAILED(hr)) return hr;
    hr = r_ascend(s, &form);
    if (!(c->flags & DMUS_CONTAINER_NOLOADS))
        for (citem *it = c->items; it; it = it->next)
            if (FAILED(IDirectMusicLoader_GetObject(l, &it->desc, &G_IidObject, (void **)&it->obj))) hr = S_PARTIALLOAD;
    if (c->stream) IStream_Release(c->stream);
    c->stream = s;
    IStream_AddRef(s);
    return hr;
}
/* 7738663c */
static HRESULT WINAPI co_parse(IDirectMusicObject *i, IStream *s, DMUS_OBJECTDESC *d)
{
    container *c = cob(i);
    if (!s || IsBadReadPtr(s, 4)) return E_POINTER;
    if (!d || IsBadReadPtr(d, 4) || d->dwSize < sizeof(*d) || IsBadWritePtr(d, d->dwSize)) return E_INVALIDARG;
    if (c->zombie) return S_ZOMBIE;
    rchunk form, k;
    HRESULT hr = r_descend(s, NULL, &form);
    if (FAILED(hr)) return hr;
    if (form.id != FCC('R','I','F','F') || form.type != FCC('D','M','C','N')) return RIFF_END;
    d->dwValidData = DMUS_OBJ_CLASS;
    d->guidClass = G_ClsContainer;
    while ((hr = r_descend(s, &form, &k)) == S_OK) {
        if (k.id == FCC('L','I','S','T') && k.type == FCC('U','N','F','O')) {
            rchunk u;
            while (r_descend(s, &k, &u) == S_OK) {
                if (u.id == FCC('U','N','A','M') && SUCCEEDED(r_read(s, d->wszName, u.size < 0x80 ? u.size : 0x80))) d->dwValidData |= DMUS_OBJ_NAME;
                r_ascend(s, &u);
            }
        } else if (k.id == FCC('g','u','i','d')) {
            if (k.size == 16) { if (FAILED(hr = r_read(s, &d->guidObject, 16))) break; d->dwValidData |= DMUS_OBJ_OBJECT; }
        } else if (k.id == FCC('d','a','t','e')) {
            if (FAILED(hr = r_read(s, &d->ftDate, 8))) break;
            d->dwValidData |= DMUS_OBJ_DATE;
        } else if (k.id == FCC('c','a','t',' ')) {
            if (FAILED(hr = r_read(s, d->wszCategory, k.size < 0x80 ? k.size : 0x80))) break;
            d->dwValidData |= DMUS_OBJ_CATEGORY;
        } else if (k.id == FCC('v','e','r','s')) {
            if (FAILED(hr = r_read(s, &d->vVersion, 8))) break;
            d->dwValidData |= DMUS_OBJ_VERSION;
        }
        if (FAILED(hr = r_ascend(s, &k))) break;
    }
    return hr == RIFF_END ? S_OK : hr;
}
static const IDirectMusicObjectVtbl co_vtbl = { co_qi, co_addref, co_release, co_getdesc, co_setdesc, co_parse };

static HRESULT WINAPI cp_qi(IPersistStream *i, REFIID r, void **o) { return ct_qi(&cps(i)->IDirectMusicContainer_iface, r, o); }
static ULONG WINAPI cp_addref(IPersistStream *i) { return ct_addref(&cps(i)->IDirectMusicContainer_iface); }
static ULONG WINAPI cp_release(IPersistStream *i) { return ct_release(&cps(i)->IDirectMusicContainer_iface); }
static HRESULT WINAPI cp_getclass(IPersistStream *i, CLSID *cls)       /* 7738644b */
{
    if (cps(i)->zombie) return S_ZOMBIE;
    if (!cls) return E_POINTER;
    *cls = G_ClsContainer;
    return S_OK;
}
static HRESULT WINAPI cp_isdirty(IPersistStream *i) { return cps(i)->zombie ? S_ZOMBIE : S_FALSE; }
static HRESULT WINAPI cp_load(IPersistStream *i, IStream *s)           /* 77387370 */
{
    container *c = cps(i);
    if (!s || IsBadReadPtr(s, 4) || IsBadReadPtr(*(void **)s, 4)) return E_POINTER;
    if (c->zombie) return S_ZOMBIE;
    IDirectMusicGetLoader *g = NULL;
    IDirectMusicLoader *l = NULL;
    IStream_QueryInterface(s, &G_IidGetLoader, (void **)&g);
    if (g) { IDirectMusicGetLoader_GetLoader(g, &l); IDirectMusicGetLoader_Release(g); }
    if (!l) return E_NOLOADER;
    HRESULT hr = ct_load(c, s, l);
    IDirectMusicLoader_Release(l);
    return hr;
}
static HRESULT WINAPI cp_save(IPersistStream *i, IStream *s, BOOL clear) { return E_NOTIMPL; }
static HRESULT WINAPI cp_getsize(IPersistStream *i, ULARGE_INTEGER *n) { return E_NOTIMPL; }
static const IPersistStreamVtbl cp_vtbl = { cp_qi, cp_addref, cp_release, cp_getclass, cp_isdirty, cp_load, cp_save, cp_getsize };

static HRESULT WINAPI cz_qi(IUnknown *i, REFIID r, void **o) { return ct_qi(&czo(i)->IDirectMusicContainer_iface, r, o); }
static ULONG WINAPI cz_addref(IUnknown *i) { return ct_addref(&czo(i)->IDirectMusicContainer_iface); }
static ULONG WINAPI cz_release(IUnknown *i) { return ct_release(&czo(i)->IDirectMusicContainer_iface); }
static HRESULT WINAPI cz_zombie(IUnknown *i)                           /* 77386a64 */
{
    container *c = czo(i);
    ct_release_items(c);
    if (c->stream) { IStream_Release(c->stream); c->stream = NULL; }
    c->zombie = TRUE;
    return S_OK;
}
static const struct { void *qi, *addref, *release, *zombie; } cz_vtbl = { cz_qi, cz_addref, cz_release, cz_zombie };

HRESULT rm_container_create(REFIID riid, void **out)
{
    *out = NULL;
    container *c = calloc(1, sizeof(*c));
    if (!c) return E_OUTOFMEMORY;
    c->IDirectMusicContainer_iface.lpVtbl = (IDirectMusicContainerVtbl *)&ct_vtbl;
    c->IDirectMusicObject_iface.lpVtbl = (IDirectMusicObjectVtbl *)&co_vtbl;
    c->IPersistStream_iface.lpVtbl = (IPersistStreamVtbl *)&cp_vtbl;
    c->IDirectMusicObjectP_iface.lpVtbl = (IUnknownVtbl *)&cz_vtbl;
    c->ref = 1;
    c->desc.dwSize = sizeof(c->desc);
    HRESULT hr = ct_qi(&c->IDirectMusicContainer_iface, riid, out);
    ct_release(&c->IDirectMusicContainer_iface);
    return hr;
}
