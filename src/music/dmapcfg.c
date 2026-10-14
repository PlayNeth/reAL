/* reDM: audiopath configurations, see dmapcfg.h.
 *
 * The 'DMAP' form (dmime 773204da):
 *   'guid' 'vers' 'catg' 'date', LIST 'UNFO' ('UNAM')  the descriptor
 *   LIST 'dbfl' ('ddah' [RIFF 'DSBC'])                 a buffer, created before the port configurations' buffers
 *   RIFF 'DSBC'                                        a buffer with no attributes
 *   LIST 'pcsl' (LIST 'pcfl'...)                        port configurations (77320245):
 *       'pcfh' 'pprh' (both required), LIST 'dbfl' / RIFF 'DSBC' (the port's buffers),
 *       LIST 'pchl' ('pchh': DMUS_IO_PCHANNELTOBUFFER_HEADER and the buffers' GUIDs)
 *   RIFF 'DMTG'                                        a tool graph (not kept here)
 * A 'DSBC' form (dsound 6efdb7de): 'guid', 'dsbd' (DSOUND_IO_DSBUFFERDESC), 'bsid' (bus functions), 'ds3d'
 * (DSOUND_IO_3D), LIST 'fxls' of RIFF 'DSFX' ('fxhr' DSOUND_IO_DXDMO_HEADER, 'data': the effect's IPersistStream data,
 * or a send's level).  An effect only counts after 'dsbd'; its object is made at load. */
#include "../real.h"
#include "dmapcfg.h"
#include "dmgraph.h"
#include <string.h>

#define FCC(a, b, c, d) ((DWORD)(a) | ((DWORD)(b) << 8) | ((DWORD)(c) << 16) | ((DWORD)(d) << 24))
#define E_DMAP_NOTDMAP ((HRESULT)0x88781210)
#define E_DMAP_PORTHDR ((HRESULT)0x88781117)
#define E_DSBC_RESERVED ((HRESULT)0x8878122d)

static const GUID s_GUID_DSFX_SEND = { 0xef602176, 0xbcbb, 0x49e0, { 0x8c, 0xca, 0xe0, 0x9a, 0x5a, 0x15, 0x2b, 0x33 } };
static const GUID s_IID_IPersistStream = { 0x00000109, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
/* the standard buffers' GUIDs (dmime 773016a4..773016e4) */
static const GUID s_Buffer_Reverb = { 0x186cc541, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID s_Buffer_EnvReverb = { 0x186cc542, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID s_Buffer_Stereo = { 0x186cc545, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID s_Buffer_3D_Dry = { 0x186cc546, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID s_Buffer_Mono = { 0x186cc547, 0xdb29, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };
static const GUID s_CLSID_AudioPathConfig = { 0xee0b9ca0, 0xa81e, 0x11d3, { 0x9b, 0xd1, 0x00, 0x80, 0xc7, 0x15, 0x0a, 0x74 } };

/* ------------------------------------------------------------------ RIFF reading */
typedef struct { const BYTE *p; DWORD n; } span;
typedef struct { DWORD id, type; span d; } ck;
/* the next chunk of s (advanced); FALSE at its end or on a truncated chunk */
static BOOL next(span *s, ck *c)
{
    if (s->n < 8) return FALSE;
    DWORD id, len;
    memcpy(&id, s->p, 4); memcpy(&len, s->p + 4, 4);
    if (len > s->n - 8) return FALSE;
    c->id = id; c->d.p = s->p + 8; c->d.n = len; c->type = 0;
    if ((id == FCC('R','I','F','F') || id == FCC('L','I','S','T')) && len >= 4) {
        memcpy(&c->type, c->d.p, 4);
        c->d.p += 4; c->d.n -= 4;
    }
    DWORD adv = 8 + len + (len & 1);
    if (adv > s->n) adv = s->n;
    s->p += adv; s->n -= adv;
    return TRUE;
}
static BOOL is_list(const ck *c) { return c->id == FCC('R','I','F','F') || c->id == FCC('L','I','S','T'); }
/* a fixed-size chunk read as dmime's 7731df88 does: what is there, the rest zero */
static void get(const ck *c, void *out, DWORD n)
{
    memset(out, 0, n);
    memcpy(out, c->d.p, c->d.n < n ? c->d.n : n);
}

/* ------------------------------------------------------------------ the buffer configuration (DSBC) */
static void buffer_free(apc_buffer *b)
{
    for (DWORD i = 0; i < b->nfx; i++) if (b->fx[i].obj) IUnknown_Release(b->fx[i].obj);
    free(b->fx); free(b->bus);
    memset(b, 0, sizeof(*b));
}

/* an effect's data: its IPersistStream::Load on a stream of the chunk */
static HRESULT load_data(IUnknown *obj, const span *d)
{
    IPersistStream *ps = NULL;
    HRESULT hr = IUnknown_QueryInterface(obj, &s_IID_IPersistStream, (void **)&ps);
    if (FAILED(hr)) return hr;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, d->n ? d->n : 1);
    IStream *st = NULL;
    if (g) {
        void *p = GlobalLock(g);
        memcpy(p, d->p, d->n);
        GlobalUnlock(g);
        hr = CreateStreamOnHGlobal(g, TRUE, &st);
        if (FAILED(hr)) GlobalFree(g);
    } else hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) { hr = IPersistStream_Load(ps, st); IStream_Release(st); }
    IPersistStream_Release(ps);
    return hr;
}

/* 6efdb574: one 'DSFX' form */
static HRESULT parse_dsfx(apc_buffer *b, span s)
{
    ck c;
    HRESULT hr = S_OK;
    rl_fxsrc *cur = NULL;
    while (SUCCEEDED(hr) && next(&s, &c)) {
        if (c.id == FCC('f','x','h','r')) {
            DSOUND_IO_DXDMO_HEADER h;
            get(&c, &h, sizeof(h));
            static const GUID null = { 0 };
            if (h.dwReserved || !IsEqualGUID(&h.guidReserved, &null)) hr = E_DSBC_RESERVED;
            BOOL send = IsEqualGUID(&h.guidDSFXClass, &s_GUID_DSFX_SEND);
            if (SUCCEEDED(hr) && send == IsEqualGUID(&h.guidSendBuffer, &null)) hr = E_DSBC_RESERVED;
            cur = NULL;
            if (SUCCEEDED(hr) && b->hasdesc) {
                rl_fxsrc *n = realloc(b->fx, (b->nfx + 1) * sizeof(*n));
                if (!n) return E_OUTOFMEMORY;
                b->fx = n;
                rl_fxsrc *e = &b->fx[b->nfx];
                memset(e, 0, sizeof(*e));
                e->cls = h.guidDSFXClass;
                e->send = h.guidSendBuffer;
                if (!send) hr = rl_fx_create_config(&h.guidDSFXClass, &e->obj);
                if (SUCCEEDED(hr)) { b->nfx++; cur = e; }
                else e->obj = NULL;
            }
        } else if (c.id == FCC('d','a','t','a') && cur) {
            if (cur->obj) hr = load_data(cur->obj, &c.d);
            else {
                LONG l = 0;
                get(&c, &l, 4);
                cur->level = l;
                if (l > 0 || l < -10000) { cur->level = 0; hr = E_INVALIDARG; }
            }
            cur = NULL;
        }
    }
    return hr;
}

/* 6efdb7de: a 'DSBC' form's contents; its own 'guid' is the buffer's (dmime 7731e8dc) */
static HRESULT parse_dsbc(apc_buffer *b, span s)
{
    ck c;
    HRESULT hr = S_OK;
    b->dsbc = TRUE;
    while (SUCCEEDED(hr) && next(&s, &c)) {
        if (c.id == FCC('g','u','i','d')) get(&c, &b->guid, sizeof(GUID));
        else if (c.id == FCC('d','s','b','d')) {
            DSOUND_IO_DSBUFFERDESC d;
            get(&c, &d, sizeof(d));
            b->dsflags = d.dwFlags; b->nch = d.nChannels; b->vol = d.lVolume; b->pan = d.lPan;
            b->hasdesc = TRUE;
        } else if (c.id == FCC('b','s','i','d')) {
            free(b->bus);
            b->nbus = c.d.n / 4;
            b->bus = malloc(b->nbus * 4 + 4);
            if (!b->bus) return E_OUTOFMEMORY;
            memcpy(b->bus, c.d.p, b->nbus * 4);
        } else if (c.id == FCC('d','s','3','d')) {
            DSOUND_IO_3D t;
            get(&c, &t, sizeof(t));
            b->alg = t.guid3DAlgorithm; b->ds3d = t.ds3d; b->has3d = TRUE;
        } else if (is_list(&c) && c.type == FCC('f','x','l','s')) {
            span l = c.d;
            ck f;
            while (SUCCEEDED(hr) && next(&l, &f))
                if (f.id == FCC('R','I','F','F') && f.type == FCC('D','S','F','X')) hr = parse_dsfx(b, f.d);
        }
    }
    return hr;
}

/* 7731e84e: a defined buffer's type from its GUID */
static int buffer_type(const GUID *g)
{
    if (IsEqualGUID(g, &s_Buffer_Reverb)) return 1;
    if (IsEqualGUID(g, &s_Buffer_EnvReverb)) return 2;
    if (IsEqualGUID(g, &s_Buffer_3D_Dry)) return 4;
    if (IsEqualGUID(g, &s_Buffer_Mono)) return 6;
    if (IsEqualGUID(g, &s_Buffer_Stereo)) return 7;
    return 0;
}

/* a 'dbfl' list (or, with ddah == FALSE, a bare 'DSBC' form) */
static HRESULT parse_buffer(apc_buffer **list, DWORD *n, const ck *c, BOOL dbfl)
{
    apc_buffer b;
    memset(&b, 0, sizeof(b));
    HRESULT hr = S_OK;
    if (dbfl) {
        span s = c->d;
        ck k;
        while (SUCCEEDED(hr) && next(&s, &k)) {
            if (k.id == FCC('d','d','a','h')) {
                DMUS_IO_BUFFER_ATTRIBUTES_HEADER h;
                get(&k, &h, sizeof(h));
                b.guid = h.guidBufferID; b.flags = h.dwFlags;
                b.type = buffer_type(&b.guid);
            } else if (is_list(&k) && k.type == FCC('D','S','B','C')) hr = parse_dsbc(&b, k.d);
        }
    } else hr = parse_dsbc(&b, c->d);
    if (FAILED(hr)) { buffer_free(&b); return hr; }
    apc_buffer *nl = realloc(*list, (*n + 1) * sizeof(*nl));
    if (!nl) { buffer_free(&b); return E_OUTOFMEMORY; }
    *list = nl;
    nl[(*n)++] = b;
    return S_OK;
}

/* 77320245: a 'pcfl' list */
static HRESULT parse_port(apc *a, const ck *c)
{
    apc_port p;
    memset(&p, 0, sizeof(p));
    span s = c->d;
    ck k;
    HRESULT hr = S_OK;
    int have = 0;
    while (SUCCEEDED(hr) && next(&s, &k)) {
        if (k.id == FCC('p','c','f','h')) { get(&k, &p.h, sizeof(p.h)); have |= 1; }
        else if (k.id == FCC('p','p','r','h')) { get(&k, &p.pp, sizeof(p.pp)); have |= 2; }
        else if (is_list(&k) && k.type == FCC('d','b','f','l')) hr = parse_buffer(&p.bufs, &p.nbufs, &k, TRUE);
        else if (is_list(&k) && k.type == FCC('D','S','B','C')) hr = parse_buffer(&p.bufs, &p.nbufs, &k, FALSE);
        else if (is_list(&k) && k.type == FCC('p','c','h','l')) {
            span l = k.d;
            ck m;
            while (next(&l, &m)) {
                if (m.id != FCC('p','c','h','h')) continue;
                DMUS_IO_PCHANNELTOBUFFER_HEADER h;
                get(&m, &h, sizeof(h));
                apc_map map = { h.dwPChannelBase, h.dwPChannelCount, h.dwBufferCount, h.dwFlags, NULL };
                map.bufs = calloc(map.nbuf + 1, sizeof(GUID));
                apc_map *nm = realloc(p.maps, (p.nmaps + 1) * sizeof(*nm));
                if (!map.bufs || !nm) { free(map.bufs); hr = E_OUTOFMEMORY; break; }
                DWORD avail = m.d.n > sizeof(h) ? (m.d.n - (DWORD)sizeof(h)) / sizeof(GUID) : 0;
                memcpy(map.bufs, m.d.p + sizeof(h), (map.nbuf < avail ? map.nbuf : avail) * sizeof(GUID));
                p.maps = nm;
                p.maps[p.nmaps++] = map;
            }
        }
    }
    if (SUCCEEDED(hr) && have != 3) hr = E_DMAP_PORTHDR;
    /* 7732048e: the channel groups the PChannels need */
    p.pp.dwValidParams |= DMUS_PORTPARAMS_CHANNELGROUPS;
    p.pp.dwChannelGroups = (p.h.dwPChannelCount + 15) / 16;
    if (SUCCEEDED(hr)) {
        apc_port *np = realloc(a->ports, (a->nports + 1) * sizeof(*np));
        if (np) { a->ports = np; a->ports[a->nports++] = p; return S_OK; }
        hr = E_OUTOFMEMORY;
    }
    for (DWORD i = 0; i < p.nbufs; i++) buffer_free(&p.bufs[i]);
    for (DWORD i = 0; i < p.nmaps; i++) free(p.maps[i].bufs);
    free(p.bufs); free(p.maps);
    return hr;
}

static void apc_clear(apc *a)
{
    for (DWORD i = 0; i < a->nbufs; i++) buffer_free(&a->bufs[i]);
    free(a->bufs);
    for (DWORD k = 0; k < a->nports; k++) {
        apc_port *p = &a->ports[k];
        for (DWORD i = 0; i < p->nbufs; i++) buffer_free(&p->bufs[i]);
        for (DWORD i = 0; i < p->nmaps; i++) free(p->maps[i].bufs);
        free(p->bufs); free(p->maps);
    }
    free(a->ports);
    if (a->graph) IDirectMusicGraph_Release(a->graph);
    memset(a, 0, sizeof(*a));
}

/* ------------------------------------------------------------------ the object */
typedef struct {
    IDirectMusicObject IDirectMusicObject_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    DMUS_OBJECTDESC desc;
    apc cfg;
} rm_apcfg;

static rm_apcfg *from_obj(IDirectMusicObject *i) { return impl_from(i, rm_apcfg, IDirectMusicObject_iface); }
static rm_apcfg *from_ps(IPersistStream *i) { return impl_from(i, rm_apcfg, IPersistStream_iface); }

static HRESULT ac_qi(rm_apcfg *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectMusicObject)) *out = &This->IDirectMusicObject_iface;
    else if (IsEqualIID(riid, &IID_IPersistStream) || IsEqualIID(riid, &IID_IPersist)) *out = &This->IPersistStream_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&This->ref);
    return S_OK;
}
static ULONG ac_release(rm_apcfg *This)
{
    LONG r = InterlockedDecrement(&This->ref);
    if (!r) {
        apc_clear(&This->cfg);
        DeleteCriticalSection(&This->cs);
        free(This);
    }
    return r;
}

static HRESULT WINAPI ob_QueryInterface(IDirectMusicObject *i, REFIID riid, void **out) { return ac_qi(from_obj(i), riid, out); }
static ULONG WINAPI ob_AddRef(IDirectMusicObject *i) { return InterlockedIncrement(&from_obj(i)->ref); }
static ULONG WINAPI ob_Release(IDirectMusicObject *i) { return ac_release(from_obj(i)); }
static HRESULT WINAPI ob_GetDescriptor(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    rm_apcfg *This = from_obj(i);
    if (!d) return E_POINTER;
    DWORD size = d->dwSize;
    *d = This->desc;
    if (size) d->dwSize = size;
    return S_OK;
}
static HRESULT WINAPI ob_SetDescriptor(IDirectMusicObject *i, LPDMUS_OBJECTDESC d)
{
    rm_apcfg *This = from_obj(i);
    if (!d) return E_POINTER;
    if (d->dwValidData & DMUS_OBJ_OBJECT) This->desc.guidObject = d->guidObject;
    if (d->dwValidData & DMUS_OBJ_NAME) memcpy(This->desc.wszName, d->wszName, sizeof(d->wszName));
    if (d->dwValidData & DMUS_OBJ_CATEGORY) memcpy(This->desc.wszCategory, d->wszCategory, sizeof(d->wszCategory));
    if (d->dwValidData & DMUS_OBJ_FILENAME) memcpy(This->desc.wszFileName, d->wszFileName, sizeof(d->wszFileName));
    if (d->dwValidData & DMUS_OBJ_VERSION) This->desc.vVersion = d->vVersion;
    if (d->dwValidData & DMUS_OBJ_DATE) This->desc.ftDate = d->ftDate;
    This->desc.dwValidData |= d->dwValidData & (DMUS_OBJ_OBJECT | DMUS_OBJ_NAME | DMUS_OBJ_CATEGORY | DMUS_OBJ_FILENAME |
                                               DMUS_OBJ_FULLPATH | DMUS_OBJ_VERSION | DMUS_OBJ_DATE);
    return S_OK;
}
/* the descriptor chunks of a 'DMAP' form */
static void read_desc(span s, DMUS_OBJECTDESC *d)
{
    ck c;
    while (next(&s, &c)) {
        if (c.id == FCC('g','u','i','d')) { get(&c, &d->guidObject, sizeof(GUID)); d->dwValidData |= DMUS_OBJ_OBJECT; }
        else if (c.id == FCC('v','e','r','s')) { get(&c, &d->vVersion, 8); d->dwValidData |= DMUS_OBJ_VERSION; }
        else if (c.id == FCC('c','a','t','g')) { get(&c, d->wszCategory, sizeof(d->wszCategory)); d->dwValidData |= DMUS_OBJ_CATEGORY; }
        else if (c.id == FCC('d','a','t','e')) { get(&c, &d->ftDate, 8); d->dwValidData |= DMUS_OBJ_DATE; }
        else if (is_list(&c) && c.type == FCC('U','N','F','O')) {
            span u = c.d;
            ck n;
            while (next(&u, &n))
                if (n.id == FCC('U','N','A','M')) { get(&n, d->wszName, sizeof(d->wszName)); d->dwValidData |= DMUS_OBJ_NAME; }
        }
    }
}
static BOOL read_stream_all(IStream *st, BYTE **data, DWORD *size)
{
    STATSTG ss;
    LARGE_INTEGER zero = { { 0, 0 } };
    ULARGE_INTEGER pos;
    if (FAILED(IStream_Seek(st, zero, STREAM_SEEK_CUR, &pos)) || FAILED(IStream_Stat(st, &ss, STATFLAG_NONAME))) return FALSE;
    DWORD n = (DWORD)(ss.cbSize.QuadPart - pos.QuadPart), got = 0;
    BYTE *b = malloc(n ? n : 1);
    if (!b || FAILED(IStream_Read(st, b, n, &got))) { free(b); return FALSE; }
    *data = b; *size = got;
    return TRUE;
}
static HRESULT WINAPI ob_ParseDescriptor(IDirectMusicObject *i, LPSTREAM st, LPDMUS_OBJECTDESC d)
{
    if (!st || !d) return E_POINTER;
    BYTE *b; DWORD n;
    if (!read_stream_all(st, &b, &n)) return E_FAIL;
    span s = { b, n };
    ck c;
    HRESULT hr = E_DMAP_NOTDMAP;
    if (next(&s, &c) && c.type == FCC('D','M','A','P')) {
        memset(d, 0, sizeof(*d));
        d->dwSize = sizeof(*d);
        d->dwValidData = DMUS_OBJ_CLASS;
        d->guidClass = s_CLSID_AudioPathConfig;
        read_desc(c.d, d);
        hr = S_OK;
    }
    free(b);
    return hr;
}
static const IDirectMusicObjectVtbl ob_vtbl = { ob_QueryInterface, ob_AddRef, ob_Release, ob_GetDescriptor, ob_SetDescriptor,
                                                ob_ParseDescriptor };

static HRESULT WINAPI ps_QueryInterface(IPersistStream *i, REFIID riid, void **out) { return ac_qi(from_ps(i), riid, out); }
static ULONG WINAPI ps_AddRef(IPersistStream *i) { return InterlockedIncrement(&from_ps(i)->ref); }
static ULONG WINAPI ps_Release(IPersistStream *i) { return ac_release(from_ps(i)); }
static HRESULT WINAPI ps_GetClassID(IPersistStream *i, CLSID *c) { if (!c) return E_POINTER; *c = s_CLSID_AudioPathConfig; return S_OK; }
static HRESULT WINAPI ps_IsDirty(IPersistStream *i) { return S_FALSE; }

/* 773204da: the form's contents, into a configuration (the old one replaced first, as 77321167 does) */
static HRESULT parse(rm_apcfg *This, span s)
{
    ck c;
    HRESULT hr = S_OK;
    apc_clear(&This->cfg);
    while (SUCCEEDED(hr) && next(&s, &c)) {
        if (!is_list(&c)) continue;
        if (c.type == FCC('d','b','f','l')) hr = parse_buffer(&This->cfg.bufs, &This->cfg.nbufs, &c, TRUE);
        else if (c.type == FCC('D','S','B','C')) hr = parse_buffer(&This->cfg.bufs, &This->cfg.nbufs, &c, FALSE);
        else if (c.type == FCC('p','c','s','l')) {
            span l = c.d;
            ck p;
            while (SUCCEEDED(hr) && next(&l, &p))
                if (p.id == FCC('L','I','S','T') && p.type == FCC('p','c','f','l')) hr = parse_port(&This->cfg, &p);
        } else if (c.type == FCC('D','M','T','G')) {        /* 77320782: a graph loaded from it */
            IDirectMusicGraph *g = NULL;
            hr = rm_graph_create(&IID_IDirectMusicGraph, (void **)&g);
            if (SUCCEEDED(hr)) {
                DWORD n = c.d.n + 12;
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, n);
                IStream *st = NULL;
                if (hg) {
                    BYTE *q = GlobalLock(hg);
                    memcpy(q, c.d.p - 12, n);
                    GlobalUnlock(hg);
                    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &st))) { GlobalFree(hg); st = NULL; }
                }
                hr = st ? rm_graph_load(g, st) : E_OUTOFMEMORY;
                if (st) IStream_Release(st);
                if (This->cfg.graph) IDirectMusicGraph_Release(This->cfg.graph);
                This->cfg.graph = g;
            }
        }
    }
    return hr;
}
static HRESULT load(rm_apcfg *This, const BYTE *data, DWORD size)
{
    span s = { data, size };
    ck c;
    if (!next(&s, &c) || c.type != FCC('D','M','A','P')) return E_DMAP_NOTDMAP;
    EnterCriticalSection(&This->cs);
    read_desc(c.d, &This->desc);
    HRESULT hr = parse(This, c.d);
    LeaveCriticalSection(&This->cs);
    return hr;
}
static HRESULT WINAPI ps_Load(IPersistStream *i, IStream *st)
{
    rm_apcfg *This = from_ps(i);
    if (!st) return E_POINTER;
    BYTE *b; DWORD n;
    if (!read_stream_all(st, &b, &n)) return E_FAIL;
    HRESULT hr = load(This, b, n);
    free(b);
    LOG_CALL("music: AudioPathConfig::Load -> %08lx (%lu buffers, %lu ports)", (unsigned long)hr,
             (unsigned long)This->cfg.nbufs, (unsigned long)This->cfg.nports);
    return hr;
}
static HRESULT WINAPI ps_Save(IPersistStream *i, IStream *st, BOOL clear) { return E_NOTIMPL; }
static HRESULT WINAPI ps_GetSizeMax(IPersistStream *i, ULARGE_INTEGER *size) { return E_NOTIMPL; }
static const IPersistStreamVtbl ps_vtbl = { ps_QueryInterface, ps_AddRef, ps_Release, ps_GetClassID, ps_IsDirty, ps_Load,
                                            ps_Save, ps_GetSizeMax };

static rm_apcfg *apcfg_new(void)
{
    rm_apcfg *This = calloc(1, sizeof(*This));
    if (!This) return NULL;
    This->IDirectMusicObject_iface.lpVtbl = (IDirectMusicObjectVtbl *)&ob_vtbl;
    This->IPersistStream_iface.lpVtbl = (IPersistStreamVtbl *)&ps_vtbl;
    This->ref = 1;
    InitializeCriticalSection(&This->cs);
    This->desc.dwSize = sizeof(This->desc);
    This->desc.dwValidData = DMUS_OBJ_CLASS;
    This->desc.guidClass = s_CLSID_AudioPathConfig;
    return This;
}

HRESULT rm_apcfg_create(REFIID riid, void **out)
{
    rm_apcfg *This = apcfg_new();
    if (!This) return E_OUTOFMEMORY;
    HRESULT hr = ac_qi(This, riid, out);
    ac_release(This);
    return hr;
}

HRESULT rm_apcfg_load(const BYTE *data, DWORD size, IUnknown **out)
{
    *out = NULL;
    rm_apcfg *This = apcfg_new();
    if (!This) return E_OUTOFMEMORY;
    HRESULT hr = load(This, data, size);
    if (FAILED(hr)) { ac_release(This); return hr; }
    *out = (IUnknown *)&This->IDirectMusicObject_iface;
    return S_OK;
}

const apc *rm_apcfg_data(IUnknown *u, IUnknown **holder)
{
    IDirectMusicObject *o = NULL;
    *holder = NULL;
    if (!u || FAILED(IUnknown_QueryInterface(u, &IID_IDirectMusicObject, (void **)&o))) return NULL;
    if (o->lpVtbl != (IDirectMusicObjectVtbl *)&ob_vtbl) { IDirectMusicObject_Release(o); return NULL; }
    *holder = (IUnknown *)o;
    return &from_obj(o)->cfg;
}
