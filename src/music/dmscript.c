/* reDM: the script object (dmscript.dll's CDirectMusicScript, 00408e45), the "DirectMusic" item its scripts see
 * (00402534), the content it carries (a container's objects, as proxies loaded on demand, 00407303), the engine
 * host (CActiveScriptManager, 00404609) and the performance's automation (CAutDirectMusicPerformance, 004063cc).
 *
 * A script file is a RIFF 'DMSC' form: 'schd' (flags: load all content, download it), the descriptor's chunks,
 * 'scve', an embedded container (RIFF 'DMCN', loaded through the loader), 'scla' (the language) and 'scsr' (the
 * source text).  The language picks the engine through the registry (HKCR\<language>\DMScript: an in-process
 * DirectMusic engine); AudioVBScript, dmscript's own, is reDM's (dmavs.c). */
#include "../real.h"
#include <dmusici.h>
#include <dmusicf.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "dmscript.h"
#include "dmavs.h"
#include "dmldr.h"
#include "dmcom.h"
#include <activscp.h>
#include <process.h>
#include <objsafe.h>

#define S_GC ((HRESULT)0x08781216)                      /* DMUS_S_GARBAGE_COLLECTED */
#define E_NOTINIT ((HRESULT)0x88781162)                 /* DMUS_E_NOT_INIT */
#define E_NOTLOADED ((HRESULT)0x88781211)               /* DMUS_E_SCRIPT_NOT_A_REFERENCE's neighbour: never loaded */
#define E_SCRIPTERR ((HRESULT)0x88781215)               /* DMUS_E_SCRIPT_ERROR_IN_SCRIPT */
#define E_BADFILE ((HRESULT)0x88781218)                 /* DMUS_E_SCRIPT_INVALID_FILE */
#define E_NOVAR ((HRESULT)0x8878121a)
#define E_NOROUTINE ((HRESULT)0x8878121b)
#define E_READONLY ((HRESULT)0x8878121c)
#define E_NOTREF ((HRESULT)0x8878121d)
#define E_NOTVALUE ((HRESULT)0x8878121e)
#define E_LANGUAGE ((HRESULT)0x88781213)
#define S_TRUNC ((HRESULT)0x08781210)                   /* DMUS_S_STRING_TRUNCATED */
#define LOAD_DISPID 10000000                            /* an unloaded item's (and a script's) "Load" */

static const GUID G_IidObjectP = { 0x6a20c217, 0xeb3e, 0x40ec, { 0x9f, 0x3a, 0x92, 0x05, 0x08, 0x70, 0x2b, 0x5e } };
static const GUID G_IidLoader8P = { 0x3939facd, 0xf6ed, 0x4619, { 0xbd, 0x16, 0x56, 0x60, 0x3f, 0x01, 0x51, 0xca } };
static const GUID G_ClsidAudioVBScript = { 0x4ee17959, 0x931e, 0x49e4, { 0xa2, 0xc6, 0x97, 0x7e, 0xcf, 0x36, 0x28, 0xf3 } };
static const GUID G_ClsidContainer = { 0x9301e380, 0x1f22, 0x11d3, { 0x82, 0x26, 0xd2, 0xfa, 0x76, 0x25, 0x5d, 0x47 } };
static const GUID G_IidContainer = { 0x9301e386, 0x1f22, 0x11d3, { 0x82, 0x26, 0xd2, 0xfa, 0x76, 0x25, 0x5d, 0x47 } };
static const GUID G_AllTypes = { 0xd2ac2893, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_ClsidSegment = { 0xd2ac2882, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_IidSegment8 = { 0xc6784488, 0x41a3, 0x418f, { 0xaa, 0x15, 0xb3, 0x50, 0x93, 0xba, 0x42, 0xd4 } };
static const GUID G_IidPerf8 = { 0x679c4137, 0xc62e, 0x4147, { 0xb2, 0xb4, 0x9d, 0x56, 0x9a, 0xcb, 0x25, 0x4c } };
static const GUID G_IidGraph = { 0x2befc277, 0x5497, 0x11d2, { 0xbc, 0xcb, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb } };
static const GUID G_ClsidComposer = { 0xd2ac2890, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_IidComposer8 = { 0xd2ac28bf, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_MasterTempo = { 0xd2ac28b0, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const GUID G_MasterGroove = { 0xd2ac28b2, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };

/* the loader's private interface (IDirectMusicLoader8P): references the loader's garbage collector follows */
typedef struct ldr8p_vtbl {
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *GetDynamicallyReferencedObject)(void *, IDirectMusicObject *, DMUS_OBJECTDESC *, REFIID, void **);
    HRESULT (WINAPI *ReportDynamicallyReferencedObject)(void *, IDirectMusicObject *, IUnknown *);
    ULONG (WINAPI *AddRefP)(void *);
    ULONG (WINAPI *ReleaseP)(void *);
} ldr8p_vtbl;
typedef struct { const ldr8p_vtbl *lpVtbl; } ldr8p;
static ldr8p *loader_p(IUnknown *l)             /* the private interface, held by its private count */
{
    ldr8p *p = NULL;
    if (!l || FAILED(IUnknown_QueryInterface(l, &G_IidLoader8P, (void **)&p)) || !p) return NULL;
    p->lpVtbl->AddRefP(p);
    p->lpVtbl->Release(p);
    return p;
}

static WCHAR *wdup(const WCHAR *s) { if (!s) return NULL; size_t n = (lstrlenW(s) + 1) * sizeof(WCHAR); WCHAR *d = malloc(n); if (d) memcpy(d, s, n); return d; }
/* 00403be0: up to n characters with their terminator; DMUS_S_STRING_TRUNCATED when it did not fit */
static HRESULT wcopy(WCHAR *d, const WCHAR *s, UINT n)
{
    for (UINT i = 0; i < n; i++) { d[i] = s[i]; if (!s[i]) return S_OK; }
    d[n - 1] = 0;
    return S_TRUNC;
}

/* ===================================================================== the objects' automation (dmscript's
 * CAutDirectMusic* classes, aggregated by the objects: the performance 004063cc, the segment, the segment state,
 * the audiopath 00404f86 and the audiopath configuration 00405111) */
/* the methods (004018b8 ...): id, name, result (0 none, 1 long, 2 object), parameters (type 1 long, 2 object,
 * 3 string; optional; the object's interface) */
typedef struct { int type, optional; const GUID *iid; } aparam;
typedef struct { int id; const WCHAR *name; int ret; aparam p[5]; } amethod;
static const amethod s_perf_methods[] = {                         /* 004018b8 */
    { 1, L"SetMasterTempo", 0, { { 1, 0 } } },
    { 2, L"GetMasterTempo", 1, { { 0 } } },
    { 3, L"SetMasterVolume", 0, { { 1, 0 }, { 1, 1 } } },
    { 4, L"GetMasterVolume", 1, { { 0 } } },
    { 5, L"SetMasterGrooveLevel", 0, { { 1, 0 } } },
    { 6, L"GetMasterGrooveLevel", 1, { { 0 } } },
    { 7, L"SetMasterTranspose", 0, { { 1, 0 } } },
    { 8, L"GetMasterTranspose", 1, { { 0 } } },
    { 9, L"Trace", 0, { { 3, 0 } } },
    { 10, L"Rand", 1, { { 1, 0 } } },
    { -1 }
};
static const amethod s_segment_methods[] = {                      /* 00401dc0 */
    { 1, L"Load", 0, { { 0 } } },
    { 2, L"Play", 2, { { 1, 1 }, { 2, 1, &IID_IDirectMusicAudioPath }, { 2, 1, &IID_IDirectMusicSegment },
                       { 2, 1, &IID_IDirectMusicSegmentState } } },
    { 3, L"Stop", 0, { { 1, 1 } } },
    { 4, L"DownloadSoundData", 0, { { 2, 1, &IID_IDirectMusicAudioPath } } },
    { 5, L"UnloadSoundData", 0, { { 2, 1, &IID_IDirectMusicAudioPath } } },
    { 6, L"Recompose", 0, { { 0 } } },
    { -1 }
};
static const amethod s_state_methods[] = {                        /* 00402200 */
    { 1, L"IsPlaying", 1, { { 0 } } },
    { 2, L"Stop", 0, { { 1, 1 } } },
    { -1 }
};
static const amethod s_path_methods[] = {                         /* 004012b8 */
    { 1, L"SetVolume", 0, { { 1, 0 }, { 1, 1 } } },
    { 2, L"GetVolume", 1, { { 0 } } },
    { -1 }
};
static const amethod s_config_methods[] = {                       /* 00401420 */
    { 1, L"Load", 0, { { 0 } } },
    { 2, L"Create", 2, { { 0 } } },
    { -1 }
};
static const amethod *const s_tables[] = { s_perf_methods, s_segment_methods, s_state_methods, s_path_methods, s_config_methods };

/* the script flags (DirectMusic.AtMeasure ...) as segment flags, composition commands and flags (00405907 over
 * a table of pairs) */
static const DWORD s_play_flags[] = {                             /* 00401ff0 */
    0x1, DMUS_SEGF_SECONDARY, 0x2, DMUS_SEGF_SECONDARY | DMUS_SEGF_CONTROL, 0x4, DMUS_SEGF_QUEUE,
    0x8, DMUS_SEGF_GRID, 0x10, DMUS_SEGF_BEAT, 0x20, DMUS_SEGF_MEASURE, 0x40, DMUS_SEGF_MARKER, 0x80, DMUS_SEGF_DEFAULT,
    0x100, 0x32000, 0x200, 0x51000, 0x400, 0x8810000, 0x20000, DMUS_SEGF_NOINVALIDATE, 0 };
static const DWORD s_play_commands[] = {                          /* 00402058 */
    0x800, DMUS_COMMANDT_FILL, 0x1000, DMUS_COMMANDT_INTRO, 0x2000, DMUS_COMMANDT_BREAK, 0x4000, DMUS_COMMANDT_END,
    0x8000, DMUS_COMMANDT_ENDANDINTRO, 0 };
static const DWORD s_play_compose[] = {                           /* 00402088 */
    0x4, 0x400, 0x8, 0x8, 0x10, 0x10, 0x20, 0x20, 0x40, 0x800, 0x80, 0x4, 0x100, 0x21, 0x200, 0x11, 0x400, 0x401,
    0x10000, 0x1000, 0 };
static const DWORD s_stop_flags[] = {                             /* 004020e0 */
    0x8, DMUS_SEGF_GRID, 0x10, DMUS_SEGF_BEAT, 0x20, DMUS_SEGF_MEASURE, 0x40, DMUS_SEGF_MARKER, 0x80, DMUS_SEGF_DEFAULT, 0 };
static const DWORD s_stop_compose[] = {                           /* 00402110 */
    0x8, 0x8, 0x10, 0x10, 0x20, 0x20, 0x40, 0x800, 0x80, 0x4, 0 };
static const DWORD s_state_stop_flags[] = {                       /* 004022f0 */
    0x8, DMUS_SEGF_GRID, 0x10, DMUS_SEGF_BEAT, 0x20, DMUS_SEGF_MEASURE, 0x80, DMUS_SEGF_DEFAULT, 0 };
static DWORD map_flags(DWORD f, const DWORD *t)
{
    DWORD r = 0;
    for (; *t; t += 2) if (f & t[0]) r |= t[1];
    return r;
}

typedef struct perfnode { struct perfnode *next; IDirectMusicPerformance8 *perf; } perfnode;
typedef struct {
    IDispatch IDispatch_iface;
    IUnknown *outer;
    int kind;
    IUnknown *obj;                      /* +0x08 the object, without a reference (it is part of it) */
    IDirectMusicGraph *graph;           /* +0x1c (the performance's) */
    perfnode *played;                   /* +0x1c (a segment's: the other performances it was played on) */
    LONG volume;                        /* +0x1c (an audiopath's) */
    short transpose, mvolume;           /* +0x20 +0x22 (the performance's) */
    DWORD seed;                         /* +0x24 */
} rm_auto;
static rm_auto *aut(IDispatch *i) { return CONTAINING_RECORD(i, rm_auto, IDispatch_iface); }
#define A_PERF(a) ((IDirectMusicPerformance8 *)(a)->obj)

/* the script running on this thread: its performance, composer, and the time and flags its script track calls
 * it with (00404229 / 00404251 / 00404262) */
typedef struct { IDirectMusicPerformance8 *perf; IDirectMusicComposer *composer; REFERENCE_TIME t; DWORD flags; } script_ctx;
static int script_context(script_ctx *c);

static HRESULT WINAPI au_qi(IDispatch *i, REFIID r, void **o) { return IUnknown_QueryInterface(aut(i)->outer, r, o); }
static ULONG WINAPI au_addref(IDispatch *i) { return IUnknown_AddRef(aut(i)->outer); }
static ULONG WINAPI au_release(IDispatch *i) { return IUnknown_Release(aut(i)->outer); }
static HRESULT WINAPI au_typecount(IDispatch *i, UINT *n) { if (!n) return E_POINTER; *n = 0; return S_OK; }
static HRESULT WINAPI au_typeinfo(IDispatch *i, UINT n, LCID l, ITypeInfo **o) { if (o) *o = NULL; return E_NOTIMPL; }

/* 0040549a: a name of a method table (one name; IID_NULL) */
static HRESULT table_ids(const amethod *t, REFIID r, LPOLESTR *names, UINT n, DISPID *ids)
{
    if ((n && !names) || (n && !ids)) return E_POINTER;
    if (!IsEqualIID(r, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if (!n) return S_OK;
    for (UINT k = 0; k < n; k++) ids[k] = -1;
    for (; t->id != -1; t++) if (!_wcsicmp(names[0], t->name)) { ids[0] = t->id; break; }
    return ids[0] == -1 || n > 1 ? DISP_E_UNKNOWNNAME : S_OK;
}
static HRESULT WINAPI au_ids(IDispatch *i, REFIID r, LPOLESTR *names, UINT n, LCID l, DISPID *ids)
{
    return table_ids(s_tables[aut(i)->kind], r, names, n, ids);
}

/* a pmsg of the size given, stamped through the performance's graph and sent (00405f0d / 00405f56) */
static HRESULT send_pmsg(rm_auto *a, DMUS_PMSG *m)
{
    HRESULT hr = IDirectMusicGraph_StampPMsg(a->graph, m);
    if (SUCCEEDED(hr)) hr = IDirectMusicPerformance8_SendPMsg(A_PERF(a), m);
    if (FAILED(hr)) IDirectMusicPerformance8_FreePMsg(A_PERF(a), m);
    return hr;
}
static HRESULT alloc_pmsg(rm_auto *a, ULONG size, DMUS_PMSG **out)
{
    *out = NULL;
    HRESULT hr = IDirectMusicPerformance8_AllocPMsg(A_PERF(a), size, out);
    if (SUCCEEDED(hr)) { memset(*out, 0, size); (*out)->dwSize = size; }
    return hr;
}
static LONG clampl(LONG v, LONG lo, LONG hi) { return v < lo ? lo : v > hi ? hi : v; }   /* 00404d7b */
/* 00405924: a volume (hundredths of a decibel) as a MIDI volume (the 1/4 power of the gain, 127 at 0 dB) */
static BYTE volume_midi(LONG vol)
{
    static LONG table[0x61];
    if (!table[0]) {
        for (int i = 1; i < 0x61; i++) table[i] = (LONG)(127.0 * sqrt(sqrt(pow(10.0, -i * 0.1))));
        table[0] = 0x7f;
    }
    LONG d = -vol / 100;
    LONG f = ((table[d + 1] - table[d]) * (-vol % 100)) / 100;
    return (BYTE)((BYTE)f + (BYTE)table[d]);
}
static HRESULT global_param(rm_auto *a, REFGUID g, void *p, DWORD n)   /* 00405ee5 */
{
    HRESULT hr = IDirectMusicPerformance8_GetGlobalParam(A_PERF(a), g, p, n);
    return SUCCEEDED(hr) || hr == E_INVALIDARG ? S_OK : hr;
}

typedef struct { LONG num; IUnknown *obj; BSTR str; } aval;
/* the performance's methods (00412090: 00405e1a ...); res NULL when the caller wants no result */
static HRESULT perf_method(rm_auto *a, int id, aval *v, VARIANT *res)
{
    HRESULT hr = S_OK;
    DMUS_PMSG *m;
    IDirectMusicPerformance8 *pf = A_PERF(a);
    switch (id) {
    case 1: {                                   /* 00405e1a */
        float t = (float)v[0].num / 100.0f;
        if (t < 0.01f) t = 0.01f; else if (t > 100.0f) t = 100.0f;
        return IDirectMusicPerformance8_SetGlobalParam(pf, &G_MasterTempo, &t, 4);
    }
    case 2: {                                   /* 004060cd */
        if (!res) return S_OK;
        float t = 1.0f;
        hr = global_param(a, &G_MasterTempo, &t, 4);
        if (SUCCEEDED(hr)) { LONG x = (LONG)(100.0 * (double)t + 0.5); V_I4(res) = x ? x : 1; }
        return hr;
    }
    case 3: {                                   /* 00406111 / 00405ce1 */
        if (!a->graph) return E_FAIL;
        LONG vol = clampl(v[0].num, -9600, 0);
        BYTE midi = volume_midi(vol);
        if (FAILED(hr = alloc_pmsg(a, sizeof(DMUS_CURVE_PMSG), &m))) return hr;
        DMUS_CURVE_PMSG *c = (DMUS_CURVE_PMSG *)m;
        REFERENCE_TIME rt = 0;
        if (FAILED(hr = IDirectMusicPerformance8_GetLatencyTime(pf, &rt))) { IDirectMusicPerformance8_FreePMsg(pf, m); return hr; }
        c->rtTime = rt;
        c->dwFlags = DMUS_PMSGF_REFTIME | DMUS_PMSGF_LOCKTOREFTIME | DMUS_PMSGF_DX8;
        c->dwPChannel = DMUS_PCHANNEL_BROADCAST_PERFORMANCE;
        c->dwType = DMUS_PMSGT_CURVE;
        c->dwGroupID = 0xffffffff;
        c->mtDuration = v[1].num;
        c->nEndValue = midi;
        c->bType = DMUS_CURVET_CCCURVE;
        c->bCurveShape = v[1].num == 0 ? DMUS_CURVES_INSTANT : DMUS_CURVES_LINEAR;
        c->bCCData = 7;
        c->bFlags = DMUS_CURVE_START_FROM_CURRENT;
        c->wMergeIndex = 0xffff;
        if (SUCCEEDED(hr = send_pmsg(a, m))) a->mvolume = (short)vol;
        return hr;
    }
    case 4: if (res) V_I4(res) = a->mvolume; return S_OK;     /* 00405e74 */
    case 5: {                                   /* 00406141 */
        char g = (char)clampl(v[0].num, -128, 127);
        IDirectMusicPerformance8_SetGlobalParam(pf, &G_MasterGroove, &g, 1);
        return S_OK;
    }
    case 6: {                                   /* 00406175 */
        if (!res) return S_OK;
        char g = 0;
        hr = global_param(a, &G_MasterGroove, &g, 1);
        if (SUCCEEDED(hr)) V_I4(res) = g;
        return hr;
    }
    case 7: {                                   /* 004061a9 */
        short t = (short)clampl(v[0].num, -0x8000, 0x7fff);
        if (FAILED(hr = alloc_pmsg(a, 0x40, &m))) return hr;
        DMUS_TRANSPOSE_PMSG *p = (DMUS_TRANSPOSE_PMSG *)m;
        if (FAILED(hr = IDirectMusicPerformance8_GetTime(pf, &p->rtTime, NULL))) { IDirectMusicPerformance8_FreePMsg(pf, m); return hr; }
        p->dwFlags = DMUS_PMSGF_REFTIME | DMUS_PMSGF_LOCKTOREFTIME | DMUS_PMSGF_DX8;
        p->dwType = DMUS_PMSGT_TRANSPOSE;
        p->dwPChannel = DMUS_PCHANNEL_BROADCAST_PERFORMANCE;
        p->dwVirtualTrackID = 0;
        p->dwGroupID = 0xffffffff;
        p->nTranspose = t;
        p->wMergeIndex = 0xffff;
        if (SUCCEEDED(hr = send_pmsg(a, m))) a->transpose = t;
        return hr;
    }
    case 8: if (res) V_I4(res) = a->transpose; return S_OK;   /* 00405e89 */
    case 9: {                                   /* 0040626c: the text to the application, as a script lyric */
        BSTR str = v[0].str;
        size_t n = str ? lstrlenW(str) : 0;
        if (FAILED(hr = alloc_pmsg(a, (ULONG)(0x40 + n * 2), &m))) return hr;
        if (FAILED(hr = IDirectMusicPerformance8_GetTime(pf, &m->rtTime, NULL))) { IDirectMusicPerformance8_FreePMsg(pf, m); return hr; }
        m->dwFlags = DMUS_PMSGF_REFTIME | DMUS_PMSGF_LOCKTOREFTIME;
        m->dwType = DMUS_PMSGT_SCRIPTLYRIC;
        m->dwPChannel = 0;
        m->dwVirtualTrackID = 0;
        m->dwGroupID = 0xffffffff;
        memcpy((BYTE *)m + 0x38, str ? str : L"", (n + 1) * sizeof(WCHAR));
        return send_pmsg(a, m);
    }
    case 10: {                                  /* 00405e9e: 1 + a random number below n (msvcrt's generator) */
        LONG n = v[0].num;
        if (n < 1 || n > 0x7fff) return E_INVALIDARG;
        a->seed = a->seed * 214013 + 2531011;
        if (res) V_I4(res) = (LONG)((a->seed >> 16) & 0x7fff) % n + 1;
        return S_OK;
    }
    }
    return DISP_E_MEMBERNOTFOUND;
}

/* 004064b0: the segment stopped on a performance (PlayEnd: an ending composed into the music) */
static HRESULT segment_stop_on(rm_auto *a, IDirectMusicPerformance8 *pf, DWORD f, const script_ctx *c)
{
    if (!(f & 0x4000))
        return IDirectMusicPerformance8_Stop(pf, (IDirectMusicSegment *)a->obj, NULL, (MUSIC_TIME)c->t,
                                             c->flags | (map_flags(f, s_stop_flags) ^ DMUS_SEGF_DEFAULT));
    if (!c->composer) return E_FAIL;
    return IDirectMusicComposer_AutoTransition(c->composer, (IDirectMusicPerformance *)pf, NULL, DMUS_COMMANDT_END,
                                               map_flags(f, s_stop_compose) | 0x204000, NULL, NULL, NULL, NULL);
}
/* the segment's methods (00412140) */
static HRESULT segment_method(rm_auto *a, int id, aval *v, VARIANT *res)
{
    script_ctx c;
    IDirectMusicSegment8 *sg = (IDirectMusicSegment8 *)a->obj;
    HRESULT hr;
    switch (id) {
    case 1: return S_OK;                        /* Load: it is */
    case 2: {                                   /* 004067b2 */
        DWORD f = (DWORD)v[0].num;
        IDirectMusicAudioPath *path = (IDirectMusicAudioPath *)v[1].obj;
        if ((f & 3) && (f & 0xf800)) return E_INVALIDARG;
        if (!script_context(&c)) return E_FAIL;
        IDirectMusicPerformance8 *pf = c.perf;
        if (path) {                             /* the audiopath's performance, remembered to stop it there */
            IDirectMusicPerformance8 *pp = NULL;
            if (SUCCEEDED(IDirectMusicAudioPath_GetObjectInPath(path, 0, DMUS_PATH_PERFORMANCE, 0, &CLSID_DirectMusicPerformance,
                                                                0, &IID_IDirectMusicPerformance8, (void **)&pp)) && pp) {
                if (pp != pf) {
                    pf = pp;
                    perfnode *n;
                    for (n = a->played; n && n->perf != pp; n = n->next) ;
                    if (!n && (n = calloc(1, sizeof(*n)))) { n->perf = pp; n->next = a->played; a->played = n; pp = NULL; }
                }
                if (pp) IDirectMusicPerformance8_Release(pp);
            }
        }
        IDirectMusicSegmentState *ss = NULL;
        if (!(f & 0xf800)) {
            DWORD sf = map_flags(f, s_play_flags) ^ DMUS_SEGF_DEFAULT;
            if (v[2].obj) sf |= DMUS_SEGF_AUTOTRANSITION;
            hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)sg, NULL, v[2].obj, c.flags | sf, c.t,
                                                        res ? &ss : NULL, v[3].obj, (IUnknown *)path);
        } else {
            if (!c.composer) return E_FAIL;
            hr = IDirectMusicComposer_AutoTransition(c.composer, (IDirectMusicPerformance *)pf, (IDirectMusicSegment *)sg,
                                                     (WORD)map_flags(f, s_play_commands), map_flags(f, s_play_compose) | 0x204000,
                                                     NULL, NULL, res ? &ss : NULL, NULL);
        }
        if (res && ss) { V_VT(res) = VT_UNKNOWN; V_UNKNOWN(res) = (IUnknown *)ss; }
        return SUCCEEDED(hr) ? S_OK : hr;
    }
    case 3: {                                   /* 0040690d: where it was played */
        if (!script_context(&c)) return E_FAIL;
        if (!a->played) return segment_stop_on(a, c.perf, (DWORD)v[0].num, &c);
        HRESULT last = S_OK;
        BOOL any = FALSE;
        while (a->played) {
            perfnode *n = a->played;
            a->played = n->next;
            hr = segment_stop_on(a, n->perf, (DWORD)v[0].num, &c);
            if (SUCCEEDED(hr)) any = TRUE; else last = hr;
            IDirectMusicPerformance8_Release(n->perf);
            free(n);
        }
        return any && FAILED(last) ? S_FALSE : last;
    }
    case 4: case 5: {                           /* 0040657b: on the audiopath, else the script's performance */
        IUnknown *to = v[0].obj;
        if (!to) { if (!script_context(&c)) return E_FAIL; to = (IUnknown *)c.perf; }
        return id == 4 ? IDirectMusicSegment8_Download(sg, to) : IDirectMusicSegment8_Unload(sg, to);
    }
    case 6: return rm_segment_recompose(a->obj);                  /* 0040652f */
    }
    return DISP_E_MEMBERNOTFOUND;
}
static HRESULT state_method(rm_auto *a, int id, aval *v, VARIANT *res)
{
    script_ctx c;
    HRESULT hr;
    if (id == 1) {                              /* 00406a9a */
        if (!res) return S_OK;
        if (!script_context(&c)) return E_FAIL;
        hr = IDirectMusicPerformance8_IsPlaying(c.perf, NULL, (IDirectMusicSegmentState *)a->obj);
        V_I4(res) = hr == S_OK ? -1 : 0;
        return hr;
    }
    if (id == 2) {                              /* 00406ace: on the performance the state plays on */
        DWORD sf = map_flags((DWORD)v[0].num, s_state_stop_flags) ^ DMUS_SEGF_DEFAULT;
        IDirectMusicSegmentState8 *s8 = NULL;
        IDirectMusicPerformance8 *pf = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(a->obj, &IID_IDirectMusicSegmentState8, (void **)&s8))) {
            if (FAILED(IDirectMusicSegmentState8_GetObjectInPath(s8, 0, DMUS_PATH_PERFORMANCE, 0, &CLSID_DirectMusicPerformance, 0,
                                                                 &IID_IDirectMusicPerformance8, (void **)&pf))) pf = NULL;
            IDirectMusicSegmentState8_Release(s8);
        }
        BOOL got = script_context(&c);
        if (!pf) { if (!got) return E_FAIL; pf = c.perf; IDirectMusicPerformance8_AddRef(pf); }
        if (!got) { c.t = 0; c.flags = 0; }
        hr = IDirectMusicPerformance8_Stop(pf, NULL, (IDirectMusicSegmentState *)a->obj, (MUSIC_TIME)c.t, c.flags | sf);
        IDirectMusicPerformance8_Release(pf);
        return hr;
    }
    return DISP_E_MEMBERNOTFOUND;
}
static HRESULT path_method(rm_auto *a, int id, aval *v, VARIANT *res)
{
    if (id == 1) {                              /* 00404d97 */
        a->volume = clampl(v[0].num, -9600, 0);
        return IDirectMusicAudioPath_SetVolume((IDirectMusicAudioPath *)a->obj, a->volume, (DWORD)v[1].num);
    }
    if (id == 2) { if (res) V_I4(res) = a->volume; return S_OK; }  /* 00404dc7 */
    return DISP_E_MEMBERNOTFOUND;
}
static HRESULT config_method(rm_auto *a, int id, aval *v, VARIANT *res)
{
    if (id == 1) return S_OK;                   /* Load */
    if (id == 2) {                              /* 00405006: an audiopath on the script's performance */
        if (!res) return S_OK;
        script_ctx c;
        if (!script_context(&c)) return E_FAIL;
        IDirectMusicAudioPath *p = NULL;
        HRESULT hr = IDirectMusicPerformance8_CreateAudioPath(c.perf, a->obj, TRUE, &p);
        if (FAILED(hr)) return hr;
        V_VT(res) = VT_UNKNOWN; V_UNKNOWN(res) = (IUnknown *)p;
        return S_OK;
    }
    return DISP_E_MEMBERNOTFOUND;
}

/* 00405565: a parameter as the method's type: an absent one (or DISP_E_PARAMNOTFOUND) when optional is 0 */
static HRESULT convert_arg(BOOL ole, VARIANT *v, const aparam *p, aval *out)
{
    if (!v) return p->optional ? S_OK : DISP_E_PARAMNOTOPTIONAL;
    VARIANT t;
    VariantInit(&t);
    VARTYPE vt = p->type == 1 ? VT_I4 : p->type == 3 ? VT_BSTR : VT_UNKNOWN;
    HRESULT hr = ole ? VariantChangeType(&t, v, 0, vt) : avs_convert(&t, v, vt);
    if (SUCCEEDED(hr)) {
        if (vt == VT_I4) out->num = V_I4(&t);
        else if (vt == VT_BSTR) { out->str = V_BSTR(&t); VariantInit(&t); }
        else if (V_UNKNOWN(&t) && FAILED(IUnknown_QueryInterface(V_UNKNOWN(&t), p->iid ? p->iid : &IID_IUnknown, (void **)&out->obj)))
            hr = DISP_E_TYPEMISMATCH;
    }
    VariantClear(&t);
    if (FAILED(hr) && hr != DISP_E_OVERFLOW) hr = DISP_E_TYPEMISMATCH;
    return hr;
}
static void free_args(aval *v)                  /* 00405a04 */
{
    for (int k = 0; k < 5; k++) { if (v[k].obj) IUnknown_Release(v[k].obj); SysFreeString(v[k].str); }
}
/* 0040601d / 00405a7a / 0040565f */
static HRESULT method_invoke(rm_auto *a, const amethod *table, DISPID id, REFIID r, WORD f, DISPPARAMS *p,
                             VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    if (!p) return E_POINTER;
    BOOL ole = IsEqualIID(r, &IID_NULL);
    if (!ole && !IsEqualIID(r, &AVS_IID_PRIVATE)) return DISP_E_UNKNOWNINTERFACE;
    if (!(f & DISPATCH_METHOD)) return DISP_E_MEMBERNOTFOUND;
    if (p->cNamedArgs) return DISP_E_NONAMEDARGS;
    if (argerr) *argerr = 0;
    if (res) VariantInit(res);
    const amethod *mt = table;
    while (mt->id != -1 && mt->id != id) mt++;
    if (mt->id == -1) return DISP_E_MEMBERNOTFOUND;
    UINT total = 0, required = 0;
    while (total < 5 && mt->p[total].type) { if (!mt->p[total].optional) required = total + 1; total++; }
    if (p->cArgs < required || p->cArgs > total) return DISP_E_BADPARAMCOUNT;
    aval v[5];
    memset(v, 0, sizeof(v));
    for (UINT k = 0; k < total; k++) {
        int idx = (int)p->cArgs - (int)k - 1;
        VARIANT *x = idx >= 0 ? &p->rgvarg[idx] : NULL;
        if (x && V_VT(x) == VT_ERROR && V_ERROR(x) == DISP_E_PARAMNOTFOUND) x = NULL;
        HRESULT hr = convert_arg(ole, x, &mt->p[k], &v[k]);
        if (FAILED(hr)) {
            if (argerr) *argerr = idx;
            free_args(v);
            return hr;
        }
    }
    VARIANT out;
    VariantInit(&out);
    if (mt->ret == 1) { V_VT(&out) = VT_I4; V_I4(&out) = 0; }
    VARIANT *o = res && mt->ret ? &out : NULL;
    HRESULT hr;
    switch (a->kind) {
    case RM_AUTO_PERFORMANCE: hr = perf_method(a, id, v, o); break;
    case RM_AUTO_SEGMENT: hr = segment_method(a, id, v, o); break;
    case RM_AUTO_SEGMENTSTATE: hr = state_method(a, id, v, o); break;
    case RM_AUTO_AUDIOPATH: hr = path_method(a, id, v, o); break;
    default: hr = config_method(a, id, v, o); break;
    }
    free_args(v);
    if (FAILED(hr)) {
        VariantClear(&out);
        if (ei) {
            memset(ei, 0, sizeof(*ei));
            ei->bstrSource = SysAllocString(L"Microsoft DirectMusic Runtime Error");
            WCHAR b[128];
            lstrcpyW(b, L"An error occurred in a call to ");
            lstrcatW(b, mt->name);
            ei->bstrDescription = SysAllocString(b);
            ei->scode = hr;
        }
        return DISP_E_EXCEPTION;
    }
    if (res && mt->ret) *res = out;
    return hr;
}
static HRESULT WINAPI au_invoke(IDispatch *i, DISPID id, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *res,
                                EXCEPINFO *ei, UINT *argerr)
{
    rm_auto *a = aut(i);
    return method_invoke(a, s_tables[a->kind], id, r, f, p, res, ei, argerr);
}
static const IDispatchVtbl au_vtbl = { au_qi, au_addref, au_release, au_typecount, au_typeinfo, au_ids, au_invoke };

IDispatch *rm_auto_new(IUnknown *outer, int kind)
{
    rm_auto *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->IDispatch_iface.lpVtbl = (IDispatchVtbl *)&au_vtbl;
    a->outer = outer;
    a->kind = kind;
    a->obj = outer;
    if (kind == RM_AUTO_PERFORMANCE) {
        /* the performance and its graph, without references (the object they are part of) */
        IUnknown *u;
        if (SUCCEEDED(IUnknown_QueryInterface(outer, &G_IidPerf8, (void **)&u))) { a->obj = u; IUnknown_Release(outer); }
        if (SUCCEEDED(IUnknown_QueryInterface(outer, &G_IidGraph, (void **)&a->graph))) IUnknown_Release(outer);
        a->seed = GetTickCount();
    }
    return &a->IDispatch_iface;
}
void rm_auto_free(IDispatch *d)
{
    if (!d) return;
    rm_auto *a = aut(d);
    while (a->played) { perfnode *n = a->played; a->played = n->next; IDirectMusicPerformance8_Release(n->perf); free(n); }
    free(a);
}

/* ===================================================================== the script */
typedef struct rm_script rm_script;
typedef struct proxy proxy;

/* the engine host (CActiveScriptManager): the engine, its dispatch once started, and the error it last reported */
typedef struct {
    IActiveScriptSite IActiveScriptSite_iface;  /* +0x04 (an OLE engine's site) */
    avs_engine *eng;                    /* AudioVBScript, reDM's own */
    IActiveScript *as;                  /* +0x14 an Active Scripting engine (VBScript, JScript): ole */
    BOOL ole;                           /* +0x10 BSTRs and IID_NULL; run on the scripts' thread (0040eb69) */
    IDispatch *disp;                    /* +0x18 */
    rm_script *s;                       /* +0x0c */
    BOOL err;                           /* +0x1c */
    HRESULT scode;
    ULONG line, col;
    WCHAR *source, *desc, *text;
    REFERENCE_TIME tstart;              /* +0x40 the script track's time and flags for what it plays (0 otherwise) */
    DWORD tflags;                       /* +0x48 */
    WCHAR **names[2];                   /* +0x48 routines, +0x54 variables (EnumRoutine / EnumVariable) */
    UINT nnames[2];
    BOOL built[2];
} manager;

struct proxy {                          /* 00407303: one of the container's objects */
    IDispatch IDispatch_iface;
    LONG ref;
    WCHAR *name;                        /* +0x0c its alias */
    IUnknown *loader;                   /* +0x10 */
    ldr8p *loaderp;                     /* +0x14 */
    DMUS_OBJECTDESC desc;               /* +0x18 */
    BOOL loaded;                        /* +0x368 */
    IDispatch *disp;                    /* +0x36c the object, once loaded (when it has IDispatch) */
    BOOL download;                      /* +0x370 */
    IDirectMusicPerformance8 *dlperf;   /* +0x374 where it was downloaded */
};

typedef struct {                        /* the "DirectMusic" item (00402534) */
    IDispatch IDispatch_iface;
    rm_script *s;
} named_item;

struct rm_script {
    IDirectMusicScript IDirectMusicScript_iface;
    IPersistStream IPersistStream_iface;            /* +0x08 */
    IDirectMusicObject IDirectMusicObject_iface;    /* +0x0c */
    IUnknown IDirectMusicObjectP_iface;             /* +0x10 */
    IDispatch IDispatch_iface;                      /* +0x14 */
    CRITICAL_SECTION cs;
    LONG ref;                           /* +0x34 */
    BOOL zombie;                        /* +0x38 */
    IDirectMusicPerformance8 *perf;     /* +0x3c */
    ldr8p *loader;                      /* +0x40 */
    IDispatch *perfdisp;                /* +0x44 */
    IUnknown *composer;                 /* +0x48 */
    WCHAR name[64];                     /* +0x4c the descriptor */
    GUID guid;                          /* +0xcc */
    DMUS_VERSION vers;                  /* +0xdc */
    WCHAR *file;                        /* +0xe4 */
    BOOL loaded;                        /* +0xe8 */
    DWORD flags;                        /* +0xec 'schd' */
    WCHAR *lang;                        /* +0xf0 'scla' */
    DMUS_VERSION dmvers;                /* +0xf4 'scve' */
    manager *mgr;                       /* +0x100 */
    proxy **items;                      /* +0x104 the content */
    UINT nitems;
    BOOL dlall;
    named_item *named;                  /* +0x108 */
    BOOL synerr;                        /* +0x10c */
    DMUS_SCRIPT_ERRORINFO err;          /* +0x110 the syntax error Init reports */
};
static rm_script *scr(IDirectMusicScript *i) { return CONTAINING_RECORD(i, rm_script, IDirectMusicScript_iface); }
static rm_script *sps(IPersistStream *i) { return CONTAINING_RECORD(i, rm_script, IPersistStream_iface); }
static rm_script *sob(IDirectMusicObject *i) { return CONTAINING_RECORD(i, rm_script, IDirectMusicObject_iface); }
static rm_script *szo(IUnknown *i) { return CONTAINING_RECORD(i, rm_script, IDirectMusicObjectP_iface); }
static rm_script *sdi(IDispatch *i) { return CONTAINING_RECORD(i, rm_script, IDispatch_iface); }

/* ---------------------------------------------------------------- the script running on this thread (00404390) */
static DWORD s_tls = TLS_OUT_OF_INDEXES;
static manager *current_set(manager *m)
{
    if (s_tls == TLS_OUT_OF_INDEXES) {
        DWORD t = TlsAlloc();
        if (InterlockedCompareExchange((LONG *)&s_tls, (LONG)t, (LONG)TLS_OUT_OF_INDEXES) != (LONG)TLS_OUT_OF_INDEXES) TlsFree(t);
    }
    manager *old = TlsGetValue(s_tls);
    TlsSetValue(s_tls, m);
    return old;
}
static manager *current(void) { return s_tls == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(s_tls); }
static int script_context(script_ctx *c)
{
    manager *m = current();
    if (!m || !m->s->perf) return 0;
    c->perf = m->s->perf;
    c->composer = (IDirectMusicComposer *)m->s->composer;
    c->t = m->tstart;
    c->flags = m->tflags;
    return 1;
}

/* ---------------------------------------------------------------- the content */
/* 00406e07: a segment downloaded to (or unloaded from) a performance */
static HRESULT proxy_segment_dl(proxy *x, BOOL load, IDirectMusicPerformance8 *perf)
{
    if (!IsEqualGUID(&x->desc.guidClass, &G_ClsidSegment)) return S_FALSE;
    IDirectMusicSegment8 *g = NULL;
    HRESULT hr = IDispatch_QueryInterface(x->disp, &G_IidSegment8, (void **)&g);
    if (FAILED(hr)) return hr;
    hr = load ? IDirectMusicSegment8_Download(g, (IUnknown *)perf) : IDirectMusicSegment8_Unload(g, (IUnknown *)perf);
    IDirectMusicSegment8_Release(g);
    return hr;
}
/* 00406fc3 */
static void proxy_drop_loader(proxy *x)
{
    if (x->loaderp) { x->loaderp->lpVtbl->ReleaseP(x->loaderp); x->loaderp = NULL; }
    x->loader = NULL;
}
/* 00406fe8: loaded through the loader (the application's, or, for a script's "Load", the running script's) */
static HRESULT proxy_load(proxy *x, BOOL dynamic)
{
    IUnknown *o = NULL;
    HRESULT hr;
    manager *m = current();
    if (dynamic && x->loaderp) {
        IDirectMusicObject *from = m ? &m->s->IDirectMusicObject_iface : NULL;
        hr = x->loaderp->lpVtbl->GetDynamicallyReferencedObject(x->loaderp, from, &x->desc, &IID_IUnknown, (void **)&o);
    } else {
        if (!x->loader) return E_FAIL;
        hr = IDirectMusicLoader8_GetObject((IDirectMusicLoader8 *)x->loader, &x->desc, &IID_IUnknown, (void **)&o);
    }
    if (FAILED(hr)) return hr;
    proxy_drop_loader(x);
    x->loaded = TRUE;
    IUnknown_QueryInterface(o, &IID_IDispatch, (void **)&x->disp);
    IUnknown_Release(o);
    return hr;
}
/* 00406eeb: downloaded to the performance (a segment, when the script says so), or initialized (a script);
 * *what: 1 the download failed, 2 the script's Init did */
static HRESULT proxy_prepare(proxy *x, IDirectMusicPerformance8 *perf, int *what)
{
    if (!x->loaded || !perf || !what) return E_FAIL;
    *what = 0;
    if (!x->disp) return S_OK;
    if (x->download) {
        HRESULT hr = proxy_segment_dl(x, TRUE, perf);
        if (hr == S_OK) { x->dlperf = perf; IDirectMusicPerformance8_AddRef(perf); }
        if (FAILED(hr)) { *what = 1; return hr; }
    }
    if (IsEqualGUID(&x->desc.guidClass, &CLSID_DirectMusicScript)) {
        IDirectMusicScript *s = NULL;
        HRESULT hr = IDispatch_QueryInterface(x->disp, &IID_IDirectMusicScript, (void **)&s);
        if (SUCCEEDED(hr)) {
            hr = s->lpVtbl->Init(s, (IDirectMusicPerformance *)perf, NULL);
            IDirectMusicScript_Release(s);
            if (SUCCEEDED(hr)) return S_OK;
        }
        *what = 2;
        return hr;
    }
    return S_OK;
}
static proxy *prx(IDispatch *i) { return CONTAINING_RECORD(i, proxy, IDispatch_iface); }
static HRESULT WINAPI px_qi(IDispatch *i, REFIID r, void **o)         /* 00410af3 */
{
    if (!o) return E_POINTER;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDispatch)) { *o = i; IDispatch_AddRef(i); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI px_addref(IDispatch *i) { return InterlockedIncrement(&prx(i)->ref); }
static ULONG WINAPI px_release(IDispatch *i)       /* 004073dd */
{
    proxy *x = prx(i);
    LONG r = InterlockedDecrement(&x->ref);
    if (!r) {
        if (x->dlperf) { proxy_segment_dl(x, FALSE, x->dlperf); IDirectMusicPerformance8_Release(x->dlperf); }
        proxy_drop_loader(x);
        if (x->disp) IDispatch_Release(x->disp);
        free(x->name);
        free(x);
    }
    return r;
}
static HRESULT WINAPI px_typecount(IDispatch *i, UINT *n) { return E_NOTIMPL; }
static HRESULT WINAPI px_typeinfo(IDispatch *i, UINT n, LCID l, ITypeInfo **o) { return E_NOTIMPL; }
/* 0040578b: an object not yet loaded knows "Load" */
static HRESULT load_ids(REFIID r, LPOLESTR *names, UINT n, DISPID *ids)
{
    if ((n && !names) || (n && !ids)) return E_POINTER;
    if (!IsEqualIID(r, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if (!n) return S_OK;
    for (UINT k = 0; k < n; k++) ids[k] = -1;
    if (!_wcsicmp(names[0], L"Load")) ids[0] = LOAD_DISPID;
    return ids[0] == -1 || n > 1 ? DISP_E_UNKNOWNNAME : S_OK;
}
/* 0040581c: "Load" (S_OK: to be done; the caller does it) */
static HRESULT load_invoke(DISPID id, REFIID r, WORD f, DISPPARAMS *p, VARIANT *res, UINT *argerr)
{
    if (!p) return E_POINTER;
    if (!IsEqualIID(r, &IID_NULL) && !IsEqualIID(r, &AVS_IID_PRIVATE)) return DISP_E_UNKNOWNINTERFACE;
    if (f & DISPATCH_METHOD) {
        if (p->cNamedArgs) return DISP_E_NONAMEDARGS;
        if (argerr) *argerr = 0;
        if (res) VariantInit(res);
        if (id == LOAD_DISPID) return p->cArgs ? DISP_E_BADPARAMCOUNT : S_OK;
    }
    return DISP_E_MEMBERNOTFOUND;
}
static HRESULT WINAPI px_ids(IDispatch *i, REFIID r, LPOLESTR *names, UINT n, LCID l, DISPID *ids)
{
    proxy *x = prx(i);
    if (x->disp) return IDispatch_GetIDsOfNames(x->disp, r, names, n, l, ids);
    return load_ids(r, names, n, ids);
}
static HRESULT WINAPI px_invoke(IDispatch *i, DISPID id, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *res,
                                EXCEPINFO *ei, UINT *argerr)     /* 0040743d */
{
    proxy *x = prx(i);
    if (x->disp) return IDispatch_Invoke(x->disp, id, r, l, f, p, res, ei, argerr);
    HRESULT hr = load_invoke(id, r, f, p, res, argerr);
    if (FAILED(hr)) return hr;
    int what = 0;
    if (!x->loaded) hr = proxy_load(x, TRUE);
    if (SUCCEEDED(hr)) {
        manager *m = current();
        IDirectMusicPerformance8 *perf = m ? m->s->perf : NULL;
        hr = perf ? proxy_prepare(x, perf, &what) : E_FAIL;
        if (SUCCEEDED(hr)) return hr;
    }
    if (ei) {
        memset(ei, 0, sizeof(*ei));
        ei->bstrSource = SysAllocString(L"Microsoft DirectMusic Runtime Error");
        const WCHAR *pre = what == 1 ? L"Unable to download the requested content (" :
                           what == 2 && hr == E_SCRIPTERR ? L"Syntax error loading the requested script (" :
                           L"Unable to load the requested content (";
        size_t n = lstrlenW(pre) + lstrlenW(x->name) + 8;
        WCHAR *b = malloc(n * sizeof(WCHAR));
        if (b) { lstrcpyW(b, pre); lstrcatW(b, x->name); lstrcatW(b, L".Load)"); ei->bstrDescription = SysAllocString(b); free(b); }
        ei->scode = hr;
    }
    return DISP_E_EXCEPTION;
}
static const IDispatchVtbl px_vtbl = { px_qi, px_addref, px_release, px_typecount, px_typeinfo, px_ids, px_invoke };

/* 004075d0: the container's objects, each by its alias; loaded now when the script says so */
static HRESULT items_build(rm_script *s, IDirectMusicContainer *c, IUnknown *loader)
{
    HRESULT hr;
    s->dlall = (s->flags & DMUS_SCRIPTIOF_LOAD_ALL_CONTENT) && (s->flags & DMUS_SCRIPTIOF_DOWNLOAD_ALL_SEGMENTS);
    for (DWORD k = 0;; k++) {
        DMUS_OBJECTDESC d;
        WCHAR alias[MAX_PATH];
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        alias[0] = 0;
        hr = IDirectMusicContainer_EnumObject(c, &G_AllTypes, k, &d, alias);
        if (FAILED(hr)) return hr;
        if (hr == S_FALSE) return S_OK;
        proxy *x = calloc(1, sizeof(*x));
        if (!x) return E_OUTOFMEMORY;
        x->IDispatch_iface.lpVtbl = (IDispatchVtbl *)&px_vtbl;
        x->ref = 1;
        x->name = wdup(alias);
        x->loader = loader;
        x->desc = d;
        x->download = (s->flags & DMUS_SCRIPTIOF_DOWNLOAD_ALL_SEGMENTS) != 0;
        x->loaderp = loader_p(loader);
        if (!x->loaderp) x->loader = NULL;
        hr = S_OK;
        if (s->flags & DMUS_SCRIPTIOF_LOAD_ALL_CONTENT) hr = proxy_load(x, FALSE);
        if (FAILED(hr) || !x->name) { px_release(&x->IDispatch_iface); return x->name ? hr : E_OUTOFMEMORY; }
        proxy **a = realloc(s->items, (s->nitems + 1) * sizeof(*a));
        if (!a) { px_release(&x->IDispatch_iface); return E_OUTOFMEMORY; }
        s->items = a;
        s->items[s->nitems++] = x;
    }
}
static void items_free(rm_script *s)
{
    for (UINT k = 0; k < s->nitems; k++) px_release(&s->items[k]->IDispatch_iface);
    free(s->items);
    s->items = NULL;
    s->nitems = 0;
}
/* 00407292: an object by its alias (the loaded object's IDispatch, or the proxy) */
static HRESULT item_find(rm_script *s, const WCHAR *name, IDispatch **out)
{
    for (UINT k = 0; k < s->nitems; k++)
        if (!_wcsicmp(name, s->items[k]->name)) {
            *out = s->items[k]->disp ? s->items[k]->disp : &s->items[k]->IDispatch_iface;
            IDispatch_AddRef(*out);
            return S_OK;
        }
    return E_NOVAR;
}

/* ---------------------------------------------------------------- the "DirectMusic" item */
static const struct { int id; const WCHAR *name; LONG value; } s_constants[] = {    /* 00401560 */
    { 1, L"IsSecondary", 0x1 }, { 2, L"IsControl", 0x2 }, { 3, L"AtFinish", 0x4 }, { 4, L"AtGrid", 0x8 },
    { 5, L"AtBeat", 0x10 }, { 6, L"AtMeasure", 0x20 }, { 7, L"AtMarker", 0x40 }, { 8, L"AtImmediate", 0x80 },
    { 9, L"AlignToBar", 0x100 }, { 10, L"AlignToBeat", 0x200 }, { 11, L"AlignToSegment", 0x400 },
    { 12, L"PlayFill", 0x800 }, { 13, L"PlayIntro", 0x1000 }, { 14, L"PlayBreak", 0x2000 }, { 15, L"PlayEnd", 0x4000 },
    { 16, L"PlayEndAndIntro", 0x8000 }, { 17, L"PlayModulate", 0x10000 }, { 19, L"NoCutoff", 0x20000 }, { -1 }
};
static named_item *nmi(IDispatch *i) { return CONTAINING_RECORD(i, named_item, IDispatch_iface); }
static HRESULT WINAPI nm_qi(IDispatch *i, REFIID r, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDispatch)) { *o = i; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI nm_addref(IDispatch *i) { return 2; }
static ULONG WINAPI nm_release(IDispatch *i) { return 2; }
static HRESULT WINAPI nm_typecount(IDispatch *i, UINT *n) { if (!n) return E_POINTER; *n = 0; return S_OK; }
static HRESULT WINAPI nm_typeinfo(IDispatch *i, UINT n, LCID l, ITypeInfo **o) { if (o) *o = NULL; return E_NOTIMPL; }
/* 0040db51: the constants (1000 on), the performance's methods, the content (2000 on, 1-based) */
static HRESULT WINAPI nm_ids(IDispatch *i, REFIID r, LPOLESTR *names, UINT n, LCID l, DISPID *ids)
{
    rm_script *s = nmi(i)->s;
    if ((n && !names) || (n && !ids)) return E_POINTER;
    if (!IsEqualIID(r, &IID_NULL)) return DISP_E_UNKNOWNINTERFACE;
    if (!n) return S_OK;
    for (UINT k = 0; k < n; k++) ids[k] = -1;
    for (int k = 0; s_constants[k].id != -1; k++)
        if (!_wcsicmp(names[0], s_constants[k].name)) { ids[0] = s_constants[k].id; break; }
    if (ids[0] != -1) { if (n > 1) return DISP_E_UNKNOWNNAME; ids[0] += 1000; return S_OK; }
    HRESULT hr = table_ids(s_perf_methods, r, names, n, ids);
    if (hr != DISP_E_UNKNOWNNAME) return hr;
    for (UINT k = 0; k < s->nitems; k++)
        if (!_wcsicmp(names[0], s->items[k]->name)) { ids[0] = k + 1; break; }
    if (ids[0] == -1 || n > 1) return DISP_E_UNKNOWNNAME;
    ids[0] += 2000;
    return S_OK;
}
/* 004052e1 / 0040716e: a constant's value or an object, as properties */
static HRESULT property_check(REFIID r, WORD f, DISPPARAMS *p, VARIANT *res, UINT *argerr)
{
    if (!p) return E_POINTER;
    if (!IsEqualIID(r, &IID_NULL) && !IsEqualIID(r, &AVS_IID_PRIVATE)) return DISP_E_UNKNOWNINTERFACE;
    if (!(f & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
    if (p->cArgs) return DISP_E_BADPARAMCOUNT;
    if (p->cNamedArgs) return DISP_E_NONAMEDARGS;
    if (argerr) *argerr = 0;
    if (res) VariantInit(res);
    return S_OK;
}
static HRESULT WINAPI nm_invoke(IDispatch *i, DISPID id, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *res,
                                EXCEPINFO *ei, UINT *argerr)     /* 0040dbc9 */
{
    rm_script *s = nmi(i)->s;
    if (!s->perfdisp) return E_NOTINIT;
    if (id < 1000) return IDispatch_Invoke(s->perfdisp, id, r, l, f, p, res, ei, argerr);
    HRESULT hr = property_check(r, f, p, res, argerr);
    if (FAILED(hr)) return hr;
    if (id < 2000) {
        for (int k = 0; s_constants[k].id != -1; k++)
            if (s_constants[k].id == id - 1000) {
                if (res) { V_VT(res) = VT_I4; V_I4(res) = s_constants[k].value; }
                return S_OK;
            }
        return DISP_E_MEMBERNOTFOUND;
    }
    UINT k = (UINT)(id - 2000);
    if (k < 1 || k > s->nitems) return DISP_E_MEMBERNOTFOUND;
    if (res) {
        proxy *x = s->items[k - 1];
        V_VT(res) = VT_DISPATCH;
        V_DISPATCH(res) = x->disp ? x->disp : &x->IDispatch_iface;
        IDispatch_AddRef(V_DISPATCH(res));
    }
    return S_OK;
}
static const IDispatchVtbl nm_vtbl = { nm_qi, nm_addref, nm_release, nm_typecount, nm_typeinfo, nm_ids, nm_invoke };

/* ---------------------------------------------------------------- the engine host */
static void mgr_clear(manager *m)               /* 00403e7e */
{
    m->err = FALSE;
    free(m->source); free(m->desc); free(m->text);
    m->source = m->desc = m->text = NULL;
}
/* 00403ee2 / 00403f23: the error the engine reported, unless one is kept already */
static void mgr_store(manager *m, HRESULT scode, ULONG line, ULONG col, const WCHAR *src, const WCHAR *desc, const WCHAR *text)
{
    if (m->err) return;
    m->err = TRUE;
    m->scode = scode;
    m->line = line;
    m->col = col;
    m->source = wdup(src);
    m->desc = wdup(desc);
    m->text = wdup(text);
}
static void mgr_store_ei(manager *m, EXCEPINFO *ei)
{
    mgr_store(m, ei->scode, 0, 0, ei->bstrSource, ei->bstrDescription, NULL);
    SysFreeString(ei->bstrSource); SysFreeString(ei->bstrDescription); SysFreeString(ei->bstrHelpFile);
    memset(ei, 0, sizeof(*ei));
}
/* 0040413b: as much of the error information as the caller's structure holds (its dwSize kept) */
static void errinfo_copy(DMUS_SCRIPT_ERRORINFO *d, const DMUS_SCRIPT_ERRORINFO *s)
{
    DWORD keep = d->dwSize, n = keep <= s->dwSize ? keep : s->dwSize;
    memcpy(d, s, n);
    d->dwSize = keep;
}
static void errinfo_init(DMUS_SCRIPT_ERRORINFO *e) { memset(e, 0, sizeof(*e)); e->dwSize = sizeof(*e); }
/* 00404299: the kept error, as DMUS_SCRIPT_ERRORINFO: the description's "file»line»" split off, the line and
 * column 1-based, the script's file name when the error names none */
static HRESULT mgr_report(manager *m, HRESULT hr, DMUS_SCRIPT_ERRORINFO *out)
{
    if (!m->err) return hr;
    if (out) {
        DMUS_SCRIPT_ERRORINFO e;
        errinfo_init(&e);
        e.hr = m->scode;
        e.ulLineNumber = m->line;
        e.ichCharPosition = (LONG)m->col;
        if (m->desc) {                          /* 0040e5d7 */
            const WCHAR *d = m->desc, *rest = d;
            const WCHAR *a = wcschr(d, 0xbb), *b = a ? wcschr(a + 1, 0xbb) : NULL;
            if (a && b) {
                if (a > d) { UINT n = (UINT)(a - d) + 1; if (n > 0x104) n = 0x104; wcopy(e.wszSourceFile, d, n); e.wszSourceFile[n - 1] = 0; }
                if (b > a + 1) { WCHAR num[32]; UINT n = (UINT)(b - a - 1); if (n > 31) n = 31; memcpy(num, a + 1, n * 2); num[n] = 0; e.ulLineNumber = wcstoul(num, NULL, 10); }
                rest = b + 1;
            }
            wcopy(e.wszDescription, rest, 0x104);
        }
        e.ulLineNumber++;
        e.ichCharPosition++;
        if (!e.wszSourceFile[0] && m->s->file) wcopy(e.wszSourceFile, m->s->file, 0x104);
        if (m->source) wcopy(e.wszSourceComponent, m->source, 0x104);
        if (m->text) wcopy(e.wszSourceLineText, m->text, 0x104);
        errinfo_copy(out, &e);
    }
    mgr_clear(m);
    return E_SCRIPTERR;
}
static void mgr_free_do(manager *m)
{
    if (!m) return;
    if (m->disp) IDispatch_Release(m->disp);
    if (m->as) { IActiveScript_Close(m->as); IActiveScript_Release(m->as); }      /* 00404006 */
    avs_free(m->eng);
    mgr_clear(m);
    for (int k = 0; k < 2; k++) {
        for (UINT j = 0; j < m->nnames[k]; j++) SysFreeString(m->names[k][j]);
        free(m->names[k]);
    }
    free(m);
}
/* 00404609: the engine with the script's text parsed against the "DirectMusic" item */
static manager *mgr_new_avs(rm_script *s, const WCHAR *src, HRESULT *hr, DMUS_SCRIPT_ERRORINFO *err)
{
    manager *m = calloc(1, sizeof(*m));
    if (!m) { *hr = E_OUTOFMEMORY; return NULL; }
    m->s = s;
    avs_error e;
    *hr = avs_parse(src, &s->named->IDispatch_iface, &m->eng, &e);
    if (FAILED(*hr)) {
        if (e.desc) mgr_store(m, e.scode, e.line, e.col, e.source, e.desc, e.text);
        *hr = mgr_report(m, *hr, err);
    }
    avs_error_free(&e);         /* (an error the parse went on after: kept by dmscript, never reported) */
    return m;
}
/* 00404814: the script run (its variables set up) and its dispatch */
static HRESULT mgr_start_do(manager *m, DMUS_SCRIPT_ERRORINFO *err)
{
    if (m->as) {                        /* SetScriptState(started): the script's own code runs */
        manager *old = current_set(m);
        HRESULT hr = IActiveScript_SetScriptState(m->as, SCRIPTSTATE_STARTED);
        current_set(old);
        if (hr == S_OK) hr = IActiveScript_GetScriptDispatch(m->as, NULL, &m->disp);
        else if (SUCCEEDED(hr)) hr = E_LANGUAGE;
        if (SUCCEEDED(hr)) return S_OK;
        IActiveScript_Close(m->as);
        IActiveScript_Release(m->as);
        m->as = NULL;
        if (m->disp) { IDispatch_Release(m->disp); m->disp = NULL; }
        return mgr_report(m, hr, err);
    }
    if (!m->eng) return E_NOTINIT;
    HRESULT hr = avs_dispatch(m->eng, &m->disp);
    if (FAILED(hr)) { m->disp = NULL; hr = mgr_report(m, hr, err); }
    return hr;
}
static HRESULT mgr_id(manager *m, const WCHAR *name, DISPID *id)  /* 00403e27 */
{
    if (!name || !id) return E_POINTER;
    if (!m->disp) return E_NOTINIT;
    LPOLESTR n = (LPOLESTR)name;
    return IDispatch_GetIDsOfNames(m->disp, &IID_NULL, &n, 1, 0x409, id);
}
static HRESULT mgr_call_do(manager *m, const WCHAR *name, DMUS_SCRIPT_ERRORINFO *err)   /* 004048ae */
{
    DISPID id;
    if (!m->disp) return E_NOTINIT;
    HRESULT hr = mgr_id(m, name, &id);
    if (hr == DISP_E_UNKNOWNNAME) return E_NOROUTINE;
    if (FAILED(hr)) return hr;
    mgr_clear(m);
    EXCEPINFO ei;
    memset(&ei, 0, sizeof(ei));
    DISPPARAMS p = { NULL, NULL, 0, 0 };
    manager *old = current_set(m);
    hr = IDispatch_Invoke(m->disp, id, m->ole ? &IID_NULL : &AVS_IID_PRIVATE, 0x409, DISPATCH_METHOD, &p, NULL, &ei, NULL);
    current_set(old);
    if (hr == DISP_E_EXCEPTION) mgr_store_ei(m, &ei);
    return mgr_report(m, hr, err);
}
static HRESULT mgr_set_do(manager *m, const WCHAR *name, VARIANT *v, BOOL ref, DMUS_SCRIPT_ERRORINFO *err)  /* 0040498a */
{
    DISPID id;
    if (!m->disp) return E_NOTINIT;
    HRESULT hr = mgr_id(m, name, &id);
    if (hr == DISP_E_UNKNOWNNAME) return E_NOVAR;
    if (FAILED(hr)) return hr;
    mgr_clear(m);
    EXCEPINFO ei;
    memset(&ei, 0, sizeof(ei));
    DISPID put = DISPID_PROPERTYPUT;
    DISPPARAMS p = { v, &put, 1, 1 };
    hr = IDispatch_Invoke(m->disp, id, m->ole ? &IID_NULL : &AVS_IID_PRIVATE, 0x409, ref ? DISPATCH_PROPERTYPUTREF : DISPATCH_PROPERTYPUT,
                          &p, NULL, &ei, NULL);
    if (hr == DISP_E_EXCEPTION) {
        mgr_store_ei(m, &ei);
        BOOL obj = V_VT(v) == VT_DISPATCH || V_VT(v) == VT_UNKNOWN;
        if (obj != !!ref) { mgr_clear(m); return obj ? E_NOTVALUE : E_NOTREF; }
    }
    return mgr_report(m, hr, err);
}
static HRESULT mgr_get_do(manager *m, const WCHAR *name, VARIANT *v, DMUS_SCRIPT_ERRORINFO *err)   /* 00404a99 */
{
    DISPID id;
    if (!m->disp) return E_NOTINIT;
    HRESULT hr = mgr_id(m, name, &id);
    if (hr == DISP_E_UNKNOWNNAME) return E_NOVAR;
    if (FAILED(hr)) return hr;
    mgr_clear(m);
    EXCEPINFO ei;
    memset(&ei, 0, sizeof(ei));
    DISPPARAMS p = { NULL, NULL, 0, 0 };
    hr = IDispatch_Invoke(m->disp, id, m->ole ? &IID_NULL : &AVS_IID_PRIVATE, 0x409, DISPATCH_PROPERTYGET, &p, v, &ei, NULL);
    if (hr == DISP_E_EXCEPTION) mgr_store_ei(m, &ei);
    return mgr_report(m, hr, err);
}
/* 0040440b: the routines' (or variables') names, from the dispatch's type information */
static HRESULT mgr_names(manager *m, int routines)
{
    if (!m->disp) return E_NOTINIT;
    if (m->built[routines]) return S_OK;
    UINT tc = 0;
    HRESULT hr = IDispatch_GetTypeInfoCount(m->disp, &tc);
    if (FAILED(hr)) return hr;
    if (!tc) return E_NOTIMPL;
    ITypeInfo *ti = NULL;
    if (FAILED(hr = IDispatch_GetTypeInfo(m->disp, 0, 0x409, &ti))) return hr;
    TYPEATTR *ta = NULL;
    if (SUCCEEDED(hr = ITypeInfo_GetTypeAttr(ti, &ta))) {
        UINT n = routines ? ta->cFuncs : ta->cVars;
        WCHAR **list = calloc(n ? n : 1, sizeof(*list));
        if (!list) hr = E_OUTOFMEMORY;
        else {
            UINT got = 0;
            for (UINT k = 0; k < n; k++) {
                MEMBERID mid = -1;
                if (routines) {
                    FUNCDESC *fd = NULL;
                    if (FAILED(hr = ITypeInfo_GetFuncDesc(ti, k, &fd))) break;
                    if (fd->funckind == FUNC_DISPATCH && fd->invkind == INVOKE_FUNC && fd->cParams == 0) mid = fd->memid;
                    ITypeInfo_ReleaseFuncDesc(ti, fd);
                } else {
                    VARDESC *vd = NULL;
                    hr = ITypeInfo_GetVarDesc(ti, k, &vd);
                    if (SUCCEEDED(hr) && vd->varkind == VAR_DISPATCH) mid = vd->memid;
                    if (vd) ITypeInfo_ReleaseVarDesc(ti, vd);
                }
                if (mid == -1) continue;
                BSTR nm = NULL;
                UINT c = 0;
                HRESULT h = ITypeInfo_GetNames(ti, mid, &nm, 1, &c);
                if (FAILED(h) || c != 1 || (!routines && !lstrcmpW(nm, L"DirectMusic"))) SysFreeString(nm);
                else list[got++] = nm;
            }
            m->names[routines] = list;
            m->nnames[routines] = n;
            m->built[routines] = TRUE;
        }
        ITypeInfo_ReleaseTypeAttr(ti, ta);
    }
    ITypeInfo_Release(ti);
    return hr;
}
static HRESULT mgr_enum_do(manager *m, int routines, DWORD idx, WCHAR *name, UINT *count)    /* 00404b58 */
{
    HRESULT hr = mgr_names(m, routines);
    if (FAILED(hr)) return hr;
    UINT n = m->nnames[routines];
    if (n && !m->names[routines][n - 1]) n--;
    if (count) *count = n;
    if (idx < n && m->names[routines][idx]) return wcopy(name, m->names[routines][idx], 0x104);
    return S_FALSE;
}

/* ---------------------------------------------------------------- the scripts' thread (00412918: one for every
 * script of an Active Scripting engine, COM initialized, running what the scripts are asked one call at a time) */
typedef struct wjob { struct wjob *next; void (*fn)(void *); void *arg; HANDLE done; } wjob;
static struct { HANDLE thread, wake; DWORD tid; CRITICAL_SECTION cs; wjob *head; HRESULT co; } s_worker;
static INIT_ONCE s_worker_once = INIT_ONCE_STATIC_INIT;
static unsigned __stdcall worker_main(void *arg)            /* 00410bac */
{
    s_worker.co = CoInitialize(NULL);
    for (;;) {
        WaitForSingleObject(s_worker.wake, INFINITE);
        EnterCriticalSection(&s_worker.cs);
        wjob *j = s_worker.head;
        s_worker.head = NULL;
        LeaveCriticalSection(&s_worker.cs);
        while (j) {
            wjob *n = j->next;
            j->fn(j->arg);
            SetEvent(j->done);
            j = n;
        }
    }
    return 0;
}
static BOOL CALLBACK worker_start(INIT_ONCE *o, void *p, void **c)   /* 00410c71 */
{
    InitializeCriticalSection(&s_worker.cs);
    s_worker.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_worker.co = E_FAIL;
    unsigned tid = 0;
    s_worker.thread = (HANDLE)_beginthreadex(NULL, 0, worker_main, NULL, 0, &tid);
    s_worker.tid = tid;
    return TRUE;
}
/* 00410cbb: fn(arg) on the scripts' thread, waiting for it (at once when already there) */
static HRESULT worker_run(void (*fn)(void *), void *arg)
{
    InitOnceExecuteOnce(&s_worker_once, worker_start, NULL, NULL);
    if (!s_worker.thread) return E_FAIL;
    if (GetCurrentThreadId() == s_worker.tid) { fn(arg); return S_OK; }
    wjob j = { NULL, fn, arg, CreateEventA(NULL, FALSE, FALSE, NULL) };
    if (!j.done) return E_OUTOFMEMORY;
    EnterCriticalSection(&s_worker.cs);
    j.next = s_worker.head;
    s_worker.head = &j;
    SetEvent(s_worker.wake);
    LeaveCriticalSection(&s_worker.cs);
    WaitForSingleObject(j.done, INFINITE);
    CloseHandle(j.done);
    return FAILED(s_worker.co) ? s_worker.co : S_OK;
}

/* the host's operations, on the scripts' thread for an Active Scripting engine (the proxy 00403858) */
enum { OP_START, OP_CALL, OP_SET, OP_GET, OP_ENUM, OP_FREE, OP_IDS, OP_INVOKE, OP_NEW };
typedef struct {
    int op;
    manager *m;
    const WCHAR *name;
    VARIANT *v;
    BOOL ref;
    DMUS_SCRIPT_ERRORINFO *err;
    int routines;
    DWORD idx;
    WCHAR *out;
    UINT *count;
    HRESULT hr;
    /* IDs / Invoke */
    const IID *riid; LPOLESTR *names; UINT n; DISPID *ids; DISPID id; WORD flags; DISPPARAMS *dp; VARIANT *res;
    EXCEPINFO *ei; UINT *argerr;
    /* New */
    rm_script *s; const WCHAR *src; CLSID cls;
} mop;
static HRESULT mgr_ids_do(manager *m, REFIID r, LPOLESTR *names, UINT n, DISPID *ids);
static HRESULT mgr_invoke_do(manager *m, DISPID id, REFIID r, WORD f, DISPPARAMS *p, VARIANT *res, EXCEPINFO *ei, UINT *argerr);
static manager *mgr_new_ole(rm_script *s, const WCHAR *src, REFCLSID cls, HRESULT *hr, DMUS_SCRIPT_ERRORINFO *err);
static void mop_run(void *arg)
{
    mop *o = arg;
    switch (o->op) {
    case OP_START: o->hr = mgr_start_do(o->m, o->err); break;
    case OP_CALL: o->hr = mgr_call_do(o->m, o->name, o->err); break;
    case OP_SET: o->hr = mgr_set_do(o->m, o->name, o->v, o->ref, o->err); break;
    case OP_GET: o->hr = mgr_get_do(o->m, o->name, o->v, o->err); break;
    case OP_ENUM: o->hr = mgr_enum_do(o->m, o->routines, o->idx, o->out, o->count); break;
    case OP_FREE: mgr_free_do(o->m); o->hr = S_OK; break;
    case OP_IDS: o->hr = mgr_ids_do(o->m, o->riid, o->names, o->n, o->ids); break;
    case OP_INVOKE: o->hr = mgr_invoke_do(o->m, o->id, o->riid, o->flags, o->dp, o->res, o->ei, o->argerr); break;
    case OP_NEW: o->m = mgr_new_ole(o->s, o->src, &o->cls, &o->hr, o->err); break;
    }
}
static HRESULT mop_go(mop *o)
{
    if (o->op != OP_NEW && !o->m->ole) { mop_run(o); return o->hr; }
    HRESULT hr = worker_run(mop_run, o);
    return FAILED(hr) ? hr : o->hr;
}
static HRESULT mgr_start(manager *m, DMUS_SCRIPT_ERRORINFO *err) { mop o = { OP_START, m }; o.err = err; return mop_go(&o); }
static HRESULT mgr_call(manager *m, const WCHAR *name, DMUS_SCRIPT_ERRORINFO *err) { mop o = { OP_CALL, m, name }; o.err = err; return mop_go(&o); }
static HRESULT mgr_set(manager *m, const WCHAR *name, VARIANT *v, BOOL ref, DMUS_SCRIPT_ERRORINFO *err)
{ mop o = { OP_SET, m, name, v, ref, err }; return mop_go(&o); }
static HRESULT mgr_get(manager *m, const WCHAR *name, VARIANT *v, DMUS_SCRIPT_ERRORINFO *err)
{ mop o = { OP_GET, m, name, v, FALSE, err }; return mop_go(&o); }
static HRESULT mgr_enum(manager *m, int routines, DWORD idx, WCHAR *name, UINT *count)
{ mop o = { OP_ENUM, m }; o.routines = routines; o.idx = idx; o.out = name; o.count = count; return mop_go(&o); }
static void mgr_free(manager *m) { if (!m) return; mop o = { OP_FREE, m }; mop_go(&o); }
static HRESULT mgr_ids(manager *m, REFIID r, LPOLESTR *names, UINT n, DISPID *ids)
{ mop o = { OP_IDS, m }; o.riid = r; o.names = names; o.n = n; o.ids = ids; return mop_go(&o); }
static HRESULT mgr_invoke(manager *m, DISPID id, REFIID r, WORD f, DISPPARAMS *p, VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    mop o = { OP_INVOKE, m };
    o.id = id; o.riid = r; o.flags = f; o.dp = p; o.res = res; o.ei = ei; o.argerr = argerr;
    return mop_go(&o);
}

/* ---------------------------------------------------------------- the site an Active Scripting engine reports to
 * (00401204: 00403ce5 ...) */
static manager *msite(IActiveScriptSite *i) { return CONTAINING_RECORD(i, manager, IActiveScriptSite_iface); }
static HRESULT WINAPI site_qi(IActiveScriptSite *i, REFIID r, void **o)
{
    if (!o) return E_POINTER;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IActiveScriptSite)) { *o = i; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI site_addref(IActiveScriptSite *i) { return 2; }
static ULONG WINAPI site_release(IActiveScriptSite *i) { return 1; }
static HRESULT WINAPI site_lcid(IActiveScriptSite *i, LCID *l) { if (!l) return E_POINTER; *l = 0x409; return S_OK; }
static HRESULT WINAPI site_iteminfo(IActiveScriptSite *i, LPCOLESTR name, DWORD mask, IUnknown **unk, ITypeInfo **ti)  /* 00403d8b */
{
    if (unk) *unk = NULL;
    if (ti) *ti = NULL;
    if (!name || lstrcmpW(L"DirectMusic", name)) return TYPE_E_ELEMENTNOTFOUND;
    if (unk) { *unk = (IUnknown *)&msite(i)->s->named->IDispatch_iface; IUnknown_AddRef(*unk); }
    return S_OK;
}
static HRESULT WINAPI site_docversion(IActiveScriptSite *i, BSTR *v) { return E_NOTIMPL; }
static HRESULT WINAPI site_terminate(IActiveScriptSite *i, const VARIANT *res, const EXCEPINFO *ei)    /* 0040403f */
{
    if (ei) { EXCEPINFO c = *ei; mgr_store_ei(msite(i), &c); }
    return S_OK;
}
static HRESULT WINAPI site_statechange(IActiveScriptSite *i, SCRIPTSTATE st) { return S_OK; }
/* 00404062: the error the engine reports (the last one wins) */
static HRESULT WINAPI site_error(IActiveScriptSite *i, IActiveScriptError *e)
{
    manager *m = msite(i);
    if (!e) return E_POINTER;
    BSTR text = NULL;
    DWORD ctx = 0;
    ULONG line = 0;
    LONG col = 0;
    EXCEPINFO ei;
    memset(&ei, 0, sizeof(ei));
    IActiveScriptError_GetSourceLineText(e, &text);
    IActiveScriptError_GetSourcePosition(e, &ctx, &line, &col);
    IActiveScriptError_GetExceptionInfo(e, &ei);
    mgr_clear(m);
    mgr_store(m, ei.scode, line, (ULONG)col, ei.bstrSource, ei.bstrDescription, text);
    SysFreeString(ei.bstrSource); SysFreeString(ei.bstrDescription); SysFreeString(ei.bstrHelpFile);
    SysFreeString(text);
    return S_OK;
}
static HRESULT WINAPI site_enter(IActiveScriptSite *i) { return S_OK; }
static HRESULT WINAPI site_leave(IActiveScriptSite *i) { return S_OK; }
static const IActiveScriptSiteVtbl site_vtbl = { site_qi, site_addref, site_release, site_lcid, site_iteminfo,
    site_docversion, site_terminate, site_statechange, site_error, site_enter, site_leave };

/* 00404609 with an Active Scripting engine (on the scripts' thread): made safe for scripting, given the site and
 * the "DirectMusic" item, the text parsed */
static manager *mgr_new_ole(rm_script *s, const WCHAR *src, REFCLSID cls, HRESULT *hr, DMUS_SCRIPT_ERRORINFO *err)
{
    manager *m = calloc(1, sizeof(*m));
    if (!m) { *hr = E_OUTOFMEMORY; return NULL; }
    m->s = s;
    m->ole = TRUE;
    m->IActiveScriptSite_iface.lpVtbl = (IActiveScriptSiteVtbl *)&site_vtbl;
    IActiveScriptParse *ps = NULL;
    *hr = CoCreateInstance(cls, NULL, CLSCTX_INPROC_SERVER, &IID_IActiveScript, (void **)&m->as);
    if (FAILED(*hr)) { m->as = NULL; goto fail; }
    IObjectSafety *os = NULL;
    if (SUCCEEDED(IActiveScript_QueryInterface(m->as, &IID_IObjectSafety, (void **)&os))) {
        DWORD sup = 0, en = 0;
        *hr = IObjectSafety_GetInterfaceSafetyOptions(os, &IID_IActiveScript, &sup, &en);
        if (SUCCEEDED(*hr)) *hr = IObjectSafety_SetInterfaceSafetyOptions(os, &IID_IActiveScript, sup, en | 0xf);
        IObjectSafety_Release(os);
        if (FAILED(*hr)) goto fail;
    }
    if (FAILED(*hr = IActiveScript_SetScriptSite(m->as, &m->IActiveScriptSite_iface))) goto fail;
    if (FAILED(*hr = IActiveScript_AddNamedItem(m->as, L"DirectMusic", SCRIPTITEM_ISVISIBLE | SCRIPTITEM_GLOBALMEMBERS | SCRIPTITEM_NOCODE))) goto fail;
    *hr = IActiveScript_QueryInterface(m->as, &IID_IActiveScriptParse, (void **)&ps);
    if (FAILED(*hr)) { if (*hr == E_NOINTERFACE) *hr = E_LANGUAGE; ps = NULL; goto fail; }
    if (FAILED(*hr = ps->lpVtbl->InitNew(ps))) goto fail;
    EXCEPINFO ei;
    memset(&ei, 0, sizeof(ei));
    *hr = ps->lpVtbl->ParseScriptText(ps, src, NULL, NULL, NULL, 0, 0, 0, NULL, &ei);
    if (*hr == DISP_E_EXCEPTION) mgr_store_ei(m, &ei);
    if (SUCCEEDED(*hr)) { ps->lpVtbl->Release(ps); return m; }
fail:
    if (ps) ps->lpVtbl->Release(ps);
    if (m->as) { IActiveScript_Close(m->as); IActiveScript_Release(m->as); m->as = NULL; }
    *hr = mgr_report(m, *hr, err);
    return m;
}
static manager *mgr_new(rm_script *s, const WCHAR *src, REFCLSID cls, HRESULT *hr, DMUS_SCRIPT_ERRORINFO *err)
{
    if (IsEqualCLSID(cls, &G_ClsidAudioVBScript)) return mgr_new_avs(s, src, hr, err);
    mop o = { OP_NEW };
    o.s = s; o.src = src; o.cls = *cls; o.err = err;
    HRESULT h = worker_run(mop_run, &o);
    if (FAILED(h)) { *hr = h; mgr_free(o.m); return NULL; }
    *hr = o.hr;
    return o.m;
}
/* 00403c9d / 00404bbd: another script's names and calls ("Load" is the script's own) */
static HRESULT mgr_ids_do(manager *m, REFIID r, LPOLESTR *names, UINT n, DISPID *ids)
{
    if (!m->disp) return E_NOTINIT;
    HRESULT hr = load_ids(r, names, n, ids);
    if (FAILED(hr)) hr = IDispatch_GetIDsOfNames(m->disp, r, names, n, 0x409, ids);
    return hr;
}
static HRESULT mgr_invoke_do(manager *m, DISPID id, REFIID r, WORD f, DISPPARAMS *p, VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    rm_script *s = m->s;
    if (!m->disp) return E_NOTINIT;
    HRESULT hr = load_invoke(id, r, f, p, res, argerr);
    if (SUCCEEDED(hr)) return hr;
    manager *old = current_set(m);
    if ((f & DISPATCH_PROPERTYPUTREF) && p && p->cArgs == 1 && s->loader &&
        (V_VT(&p->rgvarg[0]) == VT_UNKNOWN || V_VT(&p->rgvarg[0]) == VT_DISPATCH)) {
        hr = s->loader->lpVtbl->ReportDynamicallyReferencedObject(s->loader, &s->IDirectMusicObject_iface, V_UNKNOWN(&p->rgvarg[0]));
        if (FAILED(hr)) { current_set(old); return hr; }
    }
    hr = IDispatch_Invoke(m->disp, id, r, 0x409, f, p, res, ei, argerr);
    if (hr == (HRESULT)SCRIPT_E_REPORTED && ei && m->err) {     /* the engine's report, as the exception */
        memset(ei, 0, sizeof(*ei));
        ei->scode = m->scode;
        ei->bstrSource = m->source ? SysAllocString(m->source) : NULL;
        ei->bstrDescription = m->desc ? SysAllocString(m->desc) : NULL;
        hr = DISP_E_EXCEPTION;
    }
    if (hr == DISP_E_EXCEPTION) avs_stamp(ei, s->file, m->err ? &m->line : NULL);
    current_set(old);
    return hr;
}

/* ---------------------------------------------------------------- the object */
static void script_reset(rm_script *s)          /* 00408c7a */
{
    if (s->mgr) { mgr_free(s->mgr); s->mgr = NULL; }
    if (s->perf) { IDirectMusicPerformance8_Release(s->perf); s->perf = NULL; }
    if (s->perfdisp) { IDispatch_Release(s->perfdisp); s->perfdisp = NULL; }
    if (s->loader) { s->loader->lpVtbl->ReleaseP(s->loader); s->loader = NULL; }
    if (s->composer) { IUnknown_Release(s->composer); s->composer = NULL; }
    items_free(s);
    free(s->named);
    s->named = NULL;
}

static HRESULT WINAPI sc_qi(IDirectMusicScript *i, REFIID r, void **o)       /* 004081a2 */
{
    rm_script *s = scr(i);
    if (!o) return E_POINTER;
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDirectMusicScript)) *o = &s->IDirectMusicScript_iface;
    else if (IsEqualIID(r, &IID_IDirectMusicObject)) *o = &s->IDirectMusicObject_iface;
    else if (IsEqualIID(r, &G_IidObjectP)) *o = &s->IDirectMusicObjectP_iface;
    else if (IsEqualIID(r, &IID_IPersistStream) || IsEqualIID(r, &IID_IPersist)) *o = &s->IPersistStream_iface;
    else if (IsEqualIID(r, &IID_IDispatch)) *o = &s->IDispatch_iface;
    else { *o = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&s->ref);
    return S_OK;
}
static ULONG WINAPI sc_addref(IDirectMusicScript *i) { return InterlockedIncrement(&scr(i)->ref); }
static ULONG WINAPI sc_release(IDirectMusicScript *i)      /* 00408fbc */
{
    rm_script *s = scr(i);
    LONG r = InterlockedDecrement(&s->ref);
    if (!r) {
        s->zombie = TRUE;
        script_reset(s);
        DeleteCriticalSection(&s->cs);
        free(s->file);
        free(s->lang);
        free(s);
    }
    return r;
}
/* 004095f5 */
static HRESULT WINAPI sc_init(IDirectMusicScript *i, IDirectMusicPerformance *perf, DMUS_SCRIPT_ERRORINFO *err)
{
    rm_script *s = scr(i);
    if (!perf) return E_POINTER;
    if (s->zombie) return S_GC;
    IDirectMusicPerformance8 *p8 = NULL;
    HRESULT hr = IDirectMusicPerformance_QueryInterface(perf, &G_IidPerf8, (void **)&p8);
    if (FAILED(hr)) return hr;
    if (s->perf) { hr = s->perf == p8 ? S_OK : S_FALSE; IDirectMusicPerformance8_Release(p8); return hr; }
    EnterCriticalSection(&s->cs);
    if (s->synerr) {
        if (err) {
            if (!s->err.wszSourceFile[0] && s->file) wcopy(s->err.wszSourceFile, s->file, 0x104);
            errinfo_copy(err, &s->err);
        }
        hr = E_SCRIPTERR;
    } else if (!s->loaded) hr = E_NOTLOADED;
    else {
        IDispatch *pd = NULL;
        hr = IDirectMusicPerformance8_QueryInterface(p8, &IID_IDispatch, (void **)&pd);
        if (SUCCEEDED(hr)) hr = rm_create(&G_ClsidComposer, &G_IidComposer8, (void **)&s->composer);
        if (SUCCEEDED(hr)) {
            s->perfdisp = pd; pd = NULL;
            s->perf = p8; p8 = NULL;
            if (!s->mgr) hr = E_FAIL;      /* (dmscript has no engine for the language and crashes) */
            else hr = mgr_start(s->mgr, err);
            if (SUCCEEDED(hr) && s->dlall)  /* 00407094 */
                for (UINT k = 0; k < s->nitems; k++) { int what; proxy_prepare(s->items[k], s->perf, &what); }
        }
        if (pd) IDispatch_Release(pd);
    }
    LeaveCriticalSection(&s->cs);
    if (p8) IDirectMusicPerformance8_Release(p8);
    return hr;
}
static HRESULT ready(rm_script *s) { return s->zombie ? S_GC : !s->mgr || !s->perf ? E_NOTINIT : S_OK; }
static HRESULT WINAPI sc_call(IDirectMusicScript *i, WCHAR *name, DMUS_SCRIPT_ERRORINFO *err)   /* 00408500 */
{
    rm_script *s = scr(i);
    if (!name) return E_POINTER;
    if (s->zombie) return S_GC;
    EnterCriticalSection(&s->cs);
    HRESULT hr = ready(s);
    if (hr == S_OK) hr = mgr_call(s->mgr, name, err);
    LeaveCriticalSection(&s->cs);
    return hr;
}
static HRESULT WINAPI sc_setvar(IDirectMusicScript *i, WCHAR *name, VARIANT v, BOOL ref, DMUS_SCRIPT_ERRORINFO *err)   /* 0040857f */
{
    rm_script *s = scr(i);
    if (!name) return E_POINTER;
    if ((V_VT(&v) == VT_DISPATCH || V_VT(&v) == VT_UNKNOWN) && V_UNKNOWN(&v) && !V_UNKNOWN(&v)->lpVtbl) return E_POINTER;
    if (s->zombie) return S_GC;
    EnterCriticalSection(&s->cs);
    HRESULT hr = ready(s);
    if (hr == S_OK) {
        hr = mgr_set(s->mgr, name, &v, ref, err);
        if (hr == E_NOVAR) {                    /* the content is the script's, not to be set */
            IDispatch *d;
            if (SUCCEEDED(item_find(s, name, &d))) { IDispatch_Release(d); hr = E_READONLY; }
        }
    }
    LeaveCriticalSection(&s->cs);
    return hr;
}
static HRESULT WINAPI sc_getvar(IDirectMusicScript *i, WCHAR *name, VARIANT *v, DMUS_SCRIPT_ERRORINFO *err)    /* 0040869a */
{
    rm_script *s = scr(i);
    if (!name || !v) return E_POINTER;
    VariantInit(v);
    if (s->zombie) return S_GC;
    EnterCriticalSection(&s->cs);
    HRESULT hr = ready(s);
    if (hr == S_OK) {
        hr = mgr_get(s->mgr, name, v, err);
        if (hr == E_NOVAR) {
            IDispatch *d;
            hr = item_find(s, name, &d);
            if (SUCCEEDED(hr)) { V_VT(v) = VT_UNKNOWN; V_UNKNOWN(v) = (IUnknown *)d; }
        }
    }
    LeaveCriticalSection(&s->cs);
    return hr;
}
static HRESULT WINAPI sc_setnum(IDirectMusicScript *i, WCHAR *name, LONG n, DMUS_SCRIPT_ERRORINFO *err)
{
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_I4; V_I4(&v) = n;
    return i->lpVtbl->SetVariableVariant(i, name, v, FALSE, err);
}
static HRESULT WINAPI sc_getnum(IDirectMusicScript *i, WCHAR *name, LONG *n, DMUS_SCRIPT_ERRORINFO *err)   /* 004087e6 */
{
    if (!n) return E_POINTER;
    *n = 0;
    VARIANT v;
    HRESULT hr = i->lpVtbl->GetVariableVariant(i, name, &v, err);
    if (SUCCEEDED(hr) && hr != S_FALSE && hr != S_GC) {
        rm_script *sc = scr(i);
        hr = sc->mgr && sc->mgr->ole ? VariantChangeType(&v, &v, 0, VT_I4) : avs_convert(&v, &v, VT_I4);   /* 0040deec */
        if (SUCCEEDED(hr)) *n = V_I4(&v);
        VariantClear(&v);
    }
    return hr;
}
static HRESULT WINAPI sc_setobj(IDirectMusicScript *i, WCHAR *name, IUnknown *u, DMUS_SCRIPT_ERRORINFO *err)
{
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_UNKNOWN; V_UNKNOWN(&v) = u;
    return i->lpVtbl->SetVariableVariant(i, name, v, TRUE, err);
}
static HRESULT WINAPI sc_getobj(IDirectMusicScript *i, WCHAR *name, REFIID r, void **o, DMUS_SCRIPT_ERRORINFO *err)   /* 004088b5 */
{
    if (!o) return E_POINTER;
    *o = NULL;
    VARIANT v;
    HRESULT hr = i->lpVtbl->GetVariableVariant(i, name, &v, err);
    if (SUCCEEDED(hr) && hr != S_GC) {
        rm_script *sc = scr(i);
        hr = sc->mgr && sc->mgr->ole ? VariantChangeType(&v, &v, 0, VT_UNKNOWN) : avs_convert(&v, &v, VT_UNKNOWN);
        if (SUCCEEDED(hr)) hr = V_UNKNOWN(&v) ? IUnknown_QueryInterface(V_UNKNOWN(&v), r, o) : E_POINTER;
        VariantClear(&v);
    }
    return hr;
}
static HRESULT WINAPI sc_enumroutine(IDirectMusicScript *i, DWORD idx, WCHAR *name)      /* 0040893f */
{
    rm_script *s = scr(i);
    if (!name) return E_POINTER;
    name[0] = 0;
    HRESULT hr = ready(s);
    if (hr != S_OK) return hr;
    return mgr_enum(s->mgr, 1, idx, name, NULL);
}
static HRESULT WINAPI sc_enumvariable(IDirectMusicScript *i, DWORD idx, WCHAR *name)     /* 00408997: then the content */
{
    rm_script *s = scr(i);
    if (!name) return E_POINTER;
    name[0] = 0;
    HRESULT hr = ready(s);
    if (hr != S_OK) return hr;
    UINT n = 0;
    hr = mgr_enum(s->mgr, 0, idx, name, &n);
    if (hr == S_FALSE) {
        DWORD k = idx - n;
        hr = k < s->nitems ? wcopy(name, s->items[k]->name, 0x104) : S_FALSE;
    }
    return hr;
}
static const IDirectMusicScriptVtbl sc_vtbl = {
    sc_qi, sc_addref, sc_release, sc_init, sc_call, sc_setvar, sc_getvar, sc_setnum, sc_getnum, sc_setobj, sc_getobj,
    sc_enumroutine, sc_enumvariable,
};

/* ---------------------------------------------------------------- IDirectMusicObject: the descriptor */
static HRESULT WINAPI so_qi(IDirectMusicObject *i, REFIID r, void **o) { return sc_qi(&sob(i)->IDirectMusicScript_iface, r, o); }
static ULONG WINAPI so_addref(IDirectMusicObject *i) { return sc_addref(&sob(i)->IDirectMusicScript_iface); }
static ULONG WINAPI so_release(IDirectMusicObject *i) { return sc_release(&sob(i)->IDirectMusicScript_iface); }
/* 004082e5: the class and version always; the name, object and file name when known; loaded once it was */
static HRESULT WINAPI so_getdesc(IDirectMusicObject *i, DMUS_OBJECTDESC *d)
{
    rm_script *s = sob(i);
    if (!d) return E_POINTER;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    if (s->zombie) return S_GC;
    if (s->name[0]) { d->dwValidData |= DMUS_OBJ_NAME; wcsncpy(d->wszName, s->name, 64); d->wszName[63] = 0; }
    if (!IsEqualGUID(&s->guid, &GUID_NULL)) { d->guidObject = s->guid; d->dwValidData |= DMUS_OBJ_OBJECT; }
    d->vVersion = s->vers;
    d->dwValidData |= DMUS_OBJ_VERSION;
    d->guidClass = CLSID_DirectMusicScript;
    d->dwValidData |= DMUS_OBJ_CLASS;
    if (s->file) { wcsncpy(d->wszFileName, s->file, 0x104); d->wszFileName[0x103] = 0; d->dwValidData |= DMUS_OBJ_FILENAME; }
    if (s->loaded) d->dwValidData |= DMUS_OBJ_LOADED;
    return S_OK;
}
/* 004083e4: the object GUID, name, file name and version kept; the flags of what is not taken off the caller's */
static HRESULT WINAPI so_setdesc(IDirectMusicObject *i, DMUS_OBJECTDESC *d)
{
    rm_script *s = sob(i);
    if (!d || d->dwSize < sizeof(*d)) return E_INVALIDARG;
    if (s->zombie) return S_GC;
    DWORD v = d->dwValidData;
    if (v & DMUS_OBJ_OBJECT) s->guid = d->guidObject;
    if (v & DMUS_OBJ_NAME) { wcsncpy(s->name, d->wszName, 64); s->name[63] = 0; }
    if (v & DMUS_OBJ_FILENAME) { free(s->file); s->file = wdup(d->wszFileName); }
    if (v & DMUS_OBJ_VERSION) s->vers = d->vVersion;
    d->dwValidData &= ~(DMUS_OBJ_CLASS | DMUS_OBJ_CATEGORY | DMUS_OBJ_FULLPATH | DMUS_OBJ_URL | DMUS_OBJ_DATE | DMUS_OBJ_LOADED);
    return v != d->dwValidData ? S_FALSE : S_OK;
}

/* ---------------------------------------------------------------- the file */
typedef struct { const BYTE *p; DWORD n; } span;
typedef struct { int type; DWORD id; span data; DWORD off; } chunk;    /* type 0 RIFF, 1 LIST, 2 a chunk */
/* the chunk at `off` of a list's data (its type and id: the form's or list's type for RIFF and LIST) */
static BOOL chunk_at(span s, DWORD off, chunk *c)
{
    if (off + 8 > s.n) return FALSE;
    DWORD id = *(const DWORD *)(s.p + off), sz = *(const DWORD *)(s.p + off + 4);
    if (sz > s.n - off - 8) sz = s.n - off - 8;
    c->off = off;
    if ((id == FOURCC_RIFF || id == FOURCC_LIST) && sz >= 4) {
        c->type = id == FOURCC_RIFF ? 0 : 1;
        c->id = *(const DWORD *)(s.p + off + 8);
        c->data.p = s.p + off + 12; c->data.n = sz - 4;
    } else { c->type = 2; c->id = id; c->data.p = s.p + off + 8; c->data.n = sz; }
    return TRUE;
}
static DWORD chunk_next(const chunk *c, span s)
{
    DWORD sz = *(const DWORD *)(s.p + c->off + 4);
    return c->off + 8 + sz + (sz & 1);
}
/* 0040ee97: from `off` on, the first of this type and id */
static BOOL chunk_find(span s, DWORD *off, int type, DWORD id, chunk *c)
{
    while (chunk_at(s, *off, c)) {
        if (c->type == type && c->id == id) return TRUE;
        *off = chunk_next(c, s);
    }
    return FALSE;
}
static void chunk_read(const chunk *c, void *out, DWORD n)
{
    memset(out, 0, n);
    memcpy(out, c->data.p, c->data.n < n ? c->data.n : n);
}
static WCHAR *chunk_string(const chunk *c)      /* 0040ef33 */
{
    if (c->data.n & 1) return NULL;
    WCHAR *w = malloc(c->data.n + 2);
    if (!w) return NULL;
    memcpy(w, c->data.p, c->data.n);
    w[c->data.n / 2] = 0;
    return w;
}
/* 0040f2eb: the descriptor's chunks up to 'scve' (the object GUID, version and UNFO's name) into the script's
 * own descriptor; E_FAIL without 'scve' */
static HRESULT desc_parse(rm_script *s, span f, DWORD *off)
{
    s->name[0] = 0;
    s->guid = GUID_NULL;
    memset(&s->vers, 0, sizeof(s->vers));
    chunk c;
    while (chunk_at(f, *off, &c)) {
        if (c.type == 2 && c.id == mmioFOURCC('s', 'c', 'v', 'e')) return S_OK;
        if (c.type == 2 && c.id == mmioFOURCC('g', 'u', 'i', 'd')) chunk_read(&c, &s->guid, 16);
        else if (c.type == 2 && c.id == mmioFOURCC('v', 'e', 'r', 's')) chunk_read(&c, &s->vers, 8);
        else if (c.type == 1 && c.id == mmioFOURCC('U', 'N', 'F', 'O')) {
            DWORD o = 0;
            chunk u;
            if (chunk_find(c.data, &o, 2, mmioFOURCC('U', 'N', 'A', 'M'), &u) && !(u.data.n & 1)) {
                DWORD n = u.data.n < 126 ? u.data.n : 126;
                memcpy(s->name, u.data.p, n);
                s->name[63] = 0;
            }
        }
        *off = chunk_next(&c, f);
    }
    return E_FAIL;
}
/* 00408d07: the stream's descriptor, parsed into the script's own, then the script's */
static HRESULT WINAPI so_parsedesc(IDirectMusicObject *i, IStream *st, DMUS_OBJECTDESC *d)
{
    rm_script *s = sob(i);
    if (!st || !d) return E_POINTER;
    memset(d, 0, sizeof(*d));
    d->dwSize = sizeof(*d);
    if (s->zombie) return S_GC;
    BYTE *b; DWORD n;
    if (!rm_read_form(st, &b, &n)) return E_BADFILE;
    span all = { b, n };
    chunk c;
    DWORD off = 0;
    HRESULT hr = E_BADFILE;
    EnterCriticalSection(&s->cs);
    if (chunk_find(all, &off, 0, mmioFOURCC('D', 'M', 'S', 'C'), &c)) {
        DWORD o = 0;
        hr = desc_parse(s, c.data, &o);
        if (SUCCEEDED(hr)) hr = so_getdesc(i, d);
    }
    LeaveCriticalSection(&s->cs);
    free(b);
    return hr;
}
static const IDirectMusicObjectVtbl so_vtbl = { so_qi, so_addref, so_release, so_getdesc, so_setdesc, so_parsedesc };

/* ---------------------------------------------------------------- IDirectMusicObjectP: Zombie (00408e32) */
static HRESULT WINAPI sz_qi(IUnknown *i, REFIID r, void **o) { return sc_qi(&szo(i)->IDirectMusicScript_iface, r, o); }
static ULONG WINAPI sz_addref(IUnknown *i) { return sc_addref(&szo(i)->IDirectMusicScript_iface); }
static ULONG WINAPI sz_release(IUnknown *i) { return sc_release(&szo(i)->IDirectMusicScript_iface); }
static void WINAPI sz_zombie(IUnknown *i) { rm_script *s = szo(i); s->zombie = TRUE; script_reset(s); }
static const struct { void *qi, *ar, *rl, *zb; } sz_vtbl = { sz_qi, sz_addref, sz_release, sz_zombie };

/* ---------------------------------------------------------------- IPersistStream: Load (00408ffe) */
static HRESULT WINAPI sp_qi(IPersistStream *i, REFIID r, void **o) { return sc_qi(&sps(i)->IDirectMusicScript_iface, r, o); }
static ULONG WINAPI sp_addref(IPersistStream *i) { return sc_addref(&sps(i)->IDirectMusicScript_iface); }
static ULONG WINAPI sp_release(IPersistStream *i) { return sc_release(&sps(i)->IDirectMusicScript_iface); }
static HRESULT WINAPI sp_classid(IPersistStream *i, CLSID *c) { return E_NOTIMPL; }
static HRESULT WINAPI sp_isdirty(IPersistStream *i) { return S_FALSE; }
static HRESULT WINAPI sp_save(IPersistStream *i, IStream *s, BOOL c) { return E_NOTIMPL; }
static HRESULT WINAPI sp_sizemax(IPersistStream *i, ULARGE_INTEGER *s) { return E_NOTIMPL; }

/* the engine for the language: HKCR\<language> with a DMScript key names an in-process DirectMusic engine
 * (AudioVBScript is reDM's own, also when the language is not registered) */
static int language_engine(const WCHAR *lang)
{
    HKEY k = NULL, e = NULL;
    int r = 0;
    if (lang && RegOpenKeyExW(HKEY_CLASSES_ROOT, lang, 0, KEY_READ, &k) == ERROR_SUCCESS && k) {
        if (RegOpenKeyExA(k, "DMScript", 0, KEY_READ, &e) == ERROR_SUCCESS && e) r = 1;
        else if (RegOpenKeyExA(k, "OLEScript", 0, KEY_READ, &e) == ERROR_SUCCESS && e) r = 2;
        if (e) RegCloseKey(e);
        RegCloseKey(k);
    } else if (lang && (!_wcsicmp(lang, L"AudioVBScript") || !_wcsicmp(lang, L"AudioVBScript.1"))) r = 1;
    return r;
}

static HRESULT WINAPI sp_load(IPersistStream *i, IStream *st)
{
    rm_script *s = sps(i);
    if (!st) return E_POINTER;
    if (s->zombie) return S_GC;
    EnterCriticalSection(&s->cs);
    script_reset(s);
    s->loaded = FALSE;
    s->name[0] = 0; s->guid = GUID_NULL; memset(&s->vers, 0, sizeof(s->vers));
    memset(&s->dmvers, 0, sizeof(s->dmvers));
    free(s->lang); s->lang = NULL;
    s->synerr = FALSE;
    errinfo_init(&s->err);

    BYTE *b = NULL; DWORD n = 0;
    WCHAR *src = NULL;
    IDirectMusicContainer *cont = NULL;
    IDirectMusicLoader8 *ldr = NULL;
    IDirectMusicGetLoader *gl = NULL;
    HRESULT hr = IStream_QueryInterface(st, &IID_IDirectMusicGetLoader, (void **)&gl);
    if (FAILED(hr)) goto out;
    hr = IDirectMusicGetLoader_GetLoader(gl, (IDirectMusicLoader **)&ldr);
    IDirectMusicGetLoader_Release(gl);
    if (FAILED(hr)) goto out;
    s->loader = loader_p((IUnknown *)ldr);

    LARGE_INTEGER z = { { 0, 0 } };
    ULARGE_INTEGER at;
    if (FAILED(hr = IStream_Seek(st, z, STREAM_SEEK_CUR, &at))) goto out;
    if (!rm_read_form(st, &b, &n)) { hr = E_BADFILE; goto out; }
    span all = { b, n };
    chunk form, c;
    DWORD off = 0;
    hr = E_BADFILE;
    if (!chunk_find(all, &off, 0, mmioFOURCC('D', 'M', 'S', 'C'), &form)) goto out;
    span f = form.data;
    off = 0;
    if (!chunk_find(f, &off, 2, mmioFOURCC('s', 'c', 'h', 'd'), &c)) goto out;
    chunk_read(&c, &s->flags, 4);
    if (FAILED(hr = desc_parse(s, f, &off))) goto out;
    chunk_at(f, off, &c);
    chunk_read(&c, &s->dmvers, 8);
    /* 0040f003: the embedded container, through the loader from this stream */
    hr = E_BADFILE;
    if (!chunk_find(f, &off, 0, mmioFOURCC('D', 'M', 'C', 'N'), &c)) goto out;
    {
        LARGE_INTEGER p;
        p.QuadPart = (LONGLONG)(at.QuadPart + (DWORD)(f.p - b) + c.off);
        DMUS_OBJECTDESC d;
        memset(&d, 0, sizeof(d));
        d.dwSize = sizeof(d);
        d.dwValidData = DMUS_OBJ_CLASS | DMUS_OBJ_STREAM;
        d.guidClass = G_ClsidContainer;
        d.pStream = st;
        if (FAILED(hr = IStream_Seek(st, p, STREAM_SEEK_SET, NULL))) goto out;
        hr = IDirectMusicLoader8_GetObject(ldr, &d, &G_IidContainer, (void **)&cont);
        if (FAILED(hr) || !cont) { if (SUCCEEDED(hr)) hr = E_FAIL; goto out; }
    }
    off = chunk_next(&c, f);
    s->named = calloc(1, sizeof(*s->named));
    if (!s->named) { hr = E_OUTOFMEMORY; goto out; }
    s->named->IDispatch_iface.lpVtbl = (IDispatchVtbl *)&nm_vtbl;
    s->named->s = s;
    hr = items_build(s, cont, (IUnknown *)ldr);
    IDirectMusicContainer_Release(cont);
    if (FAILED(hr)) goto out;
    hr = E_BADFILE;
    if (!chunk_find(f, &off, 2, mmioFOURCC('s', 'c', 'l', 'a'), &c)) goto out;
    if (!(s->lang = chunk_string(&c))) { hr = c.data.n & 1 ? E_FAIL : E_OUTOFMEMORY; goto out; }
    hr = E_BADFILE;                         /* (no source) */
    /* the source: the first plain chunk or list after the language ('scsr' its text; a DMRF reference to a
     * source file is not supported) */
    for (off = chunk_next(&c, f); chunk_at(f, off, &c); off = chunk_next(&c, f)) {
        if (c.type == 0) continue;
        if (c.type == 2 && c.id == mmioFOURCC('s', 'c', 's', 'r') && !(src = chunk_string(&c))) { hr = c.data.n & 1 ? E_FAIL : E_OUTOFMEMORY; break; }
        if (c.type == 1 && c.id == mmioFOURCC('D', 'M', 'R', 'F')) {
            LOG_INFO("music: script source by reference (DMRF) not supported");
            break;
        }
        s->loaded = TRUE;
        hr = S_OK;
        int eng = language_engine(s->lang);
        if (!eng) hr = E_LANGUAGE;
        else {
            CLSID cls;
            HRESULT h = CLSIDFromProgID(s->lang, &cls);
            if (FAILED(h) && (!_wcsicmp(s->lang, L"AudioVBScript") || !_wcsicmp(s->lang, L"AudioVBScript.1"))) { cls = G_ClsidAudioVBScript; h = S_OK; }
            if (FAILED(h)) hr = h;
            else {
                if (eng == 1 && !IsEqualCLSID(&cls, &G_ClsidAudioVBScript))
                    LOG_INFO("music: script language %ls: a DirectMusic engine of its own, hosted as an Active Scripting one", s->lang);
                s->mgr = mgr_new(s, src, &cls, &hr, &s->err);
                if (FAILED(hr)) { mgr_free(s->mgr); s->mgr = NULL; }
                if (hr == E_SCRIPTERR) { s->synerr = TRUE; hr = S_OK; }
            }
        }
        break;
    }
out:
    if (b) {
        LARGE_INTEGER p;
        p.QuadPart = (LONGLONG)(at.QuadPart + n);
        IStream_Seek(st, p, STREAM_SEEK_SET, NULL);
    }
    free(b);
    free(src);
    if (ldr) IDirectMusicLoader8_Release(ldr);
    LeaveCriticalSection(&s->cs);
    return hr;
}
static const IPersistStreamVtbl sp_vtbl = { sp_qi, sp_addref, sp_release, sp_classid, sp_isdirty, sp_load, sp_save, sp_sizemax };

/* ---------------------------------------------------------------- the script's IDispatch: its routines and
 * variables for other scripts (00408aec / 00408b43 through 00403c9d / 00404bbd), and "Load" */
static HRESULT WINAPI sd_qi(IDispatch *i, REFIID r, void **o) { return sc_qi(&sdi(i)->IDirectMusicScript_iface, r, o); }
static ULONG WINAPI sd_addref(IDispatch *i) { return sc_addref(&sdi(i)->IDirectMusicScript_iface); }
static ULONG WINAPI sd_release(IDispatch *i) { return sc_release(&sdi(i)->IDirectMusicScript_iface); }
static HRESULT WINAPI sd_typecount(IDispatch *i, UINT *n) { if (!n) return E_POINTER; *n = 0; return sdi(i)->zombie ? S_GC : S_OK; }
static HRESULT WINAPI sd_typeinfo(IDispatch *i, UINT n, LCID l, ITypeInfo **o) { if (o) *o = NULL; return E_NOTIMPL; }
static HRESULT WINAPI sd_ids(IDispatch *i, REFIID r, LPOLESTR *names, UINT n, LCID l, DISPID *ids)
{
    rm_script *s = sdi(i);
    if (s->zombie) { if (ids) for (UINT k = 0; k < n; k++) ids[k] = -1; return S_GC; }
    if (!s->mgr || !s->perf) return E_NOTINIT;
    return mgr_ids(s->mgr, r, names, n, ids);
}
static HRESULT WINAPI sd_invoke(IDispatch *i, DISPID id, REFIID r, LCID l, WORD f, DISPPARAMS *p, VARIANT *res,
                                EXCEPINFO *ei, UINT *argerr)
{
    rm_script *s = sdi(i);
    if (s->zombie) { if (res) VariantInit(res); return S_GC; }
    if (!s->mgr || !s->perf) return E_NOTINIT;
    return mgr_invoke(s->mgr, id, r, f, p, res, ei, argerr);
}
static const IDispatchVtbl sd_vtbl = { sd_qi, sd_addref, sd_release, sd_typecount, sd_typeinfo, sd_ids, sd_invoke };

HRESULT rm_script_create(REFIID riid, void **out)
{
    rm_script *s = calloc(1, sizeof(*s));
    if (!s) return E_OUTOFMEMORY;
    s->IDirectMusicScript_iface.lpVtbl = (IDirectMusicScriptVtbl *)&sc_vtbl;
    s->IPersistStream_iface.lpVtbl = (IPersistStreamVtbl *)&sp_vtbl;
    s->IDirectMusicObject_iface.lpVtbl = (IDirectMusicObjectVtbl *)&so_vtbl;
    s->IDirectMusicObjectP_iface.lpVtbl = (IUnknownVtbl *)&sz_vtbl;
    s->IDispatch_iface.lpVtbl = (IDispatchVtbl *)&sd_vtbl;
    s->ref = 1;
    errinfo_init(&s->err);
    InitializeCriticalSection(&s->cs);
    HRESULT hr = sc_qi(&s->IDirectMusicScript_iface, riid, out);
    sc_release(&s->IDirectMusicScript_iface);
    return hr;
}

/* ===================================================================== the script track (CDirectMusicScriptTrack)
 * The track's engine side (dmtrack.c) finds the events; these are its COM pieces: the scripts' Init as a state
 * starts playing the track (0040fb32), an event's message to the track's tool (0040fbe9: a DMUS_PMSGT_USER message
 * carrying a copy of the event), the tool's ProcessPMsg calling the routine (0040f8c2 with the script's private
 * call, 00408a16 / 0040416a) and the script error message (004109b6) the track sends when asked to
 * (its SetParam of 1cc7e0bf-981c-4b9f-be17-d572fc5fa933). */
static const GUID G_IidScriptEvent = { 0x8f42c9da, 0xd37a, 0x499c, { 0x85, 0x82, 0x1a, 0x80, 0xeb, 0xf9, 0xb2, 0x3c } };
#define PT_SCRIPTERROR 15000

static rm_script *script_of(IUnknown *u)
{
    if (!u || IsBadReadPtr(u, sizeof(void *))) return NULL;
    IDirectMusicScript *i = (IDirectMusicScript *)u;
    return i->lpVtbl == &sc_vtbl ? scr(i) : NULL;
}

/* 004109b6: the error as a message of type 15000 (the error info after the header), now, stamped by the state's
 * graph */
static HRESULT script_error_pmsg(IDirectMusicPerformance *perf, IUnknown *state, DWORD vtrack, const DMUS_SCRIPT_ERRORINFO *ei)
{
    IDirectMusicGraph *g = NULL;
    if (!state || !perf) return E_POINTER;
    HRESULT hr = IUnknown_QueryInterface(state, &G_IidGraph, (void **)&g);
    if (FAILED(hr)) return hr;
    DMUS_PMSG *m = NULL;
    ULONG size = sizeof(DMUS_PMSG) + sizeof(DMUS_SCRIPT_ERRORINFO);
    hr = IDirectMusicPerformance_AllocPMsg(perf, size, &m);
    if (SUCCEEDED(hr)) {
        memset(m, 0, size);
        m->dwSize = size;
        REFERENCE_TIME rt = 0;
        hr = IDirectMusicPerformance_GetTime(perf, &rt, NULL);
        if (SUCCEEDED(hr)) {
            m->rtTime = rt;
            m->dwFlags = 0xc1;
            m->dwVirtualTrackID = vtrack;
            m->dwType = PT_SCRIPTERROR;
            m->dwGroupID = 0xffffffff;
            memcpy(m + 1, ei, sizeof(*ei));
            hr = IDirectMusicGraph_StampPMsg(g, m);
            if (SUCCEEDED(hr)) hr = IDirectMusicPerformance_SendPMsg(perf, m);
            if (SUCCEEDED(hr)) { m = NULL; hr = S_OK; }
        }
        if (m) IDirectMusicPerformance_FreePMsg(perf, m);
    }
    IDirectMusicGraph_Release(g);
    return hr;
}

/* whether a script's Init would run its engine on the scripts' thread (an OLE engine not started yet): the track's
 * caller lets the performance go meanwhile, as the script's code may call it */
BOOL rm_script_init_blocks(IUnknown *script)
{
    rm_script *s = script_of(script);
    return s && !s->zombie && s->mgr && s->mgr->ole && !s->perf;
}

/* 0040fb32: a script's Init for a state starting the track; a script error as a message when the track was asked */
void rm_script_track_init(IUnknown *script, IDirectMusicPerformance *perf, IUnknown *state, DWORD vtrack, BOOL errnotify)
{
    DMUS_SCRIPT_ERRORINFO ei;
    memset(&ei, 0, sizeof(ei));
    ei.dwSize = sizeof(ei);             /* (00404121) */
    if (!script) return;
    HRESULT hr = ((IDirectMusicScript *)script)->lpVtbl->Init((IDirectMusicScript *)script, perf, &ei);
    if (errnotify && hr == E_SCRIPTERR) script_error_pmsg(perf, state, vtrack, &ei);
}

/* the event's copy a message carries (0040f805): its state, script and routine, and the time it was due */
typedef struct {
    IUnknown IUnknown_iface;
    LONG ref;
    IUnknown *state;                    /* +0x04 */
    IUnknown *script;                   /* +0x08 the event */
    WCHAR *routine;
    REFERENCE_TIME t;                   /* +0x10 */
    DWORD tflags;                       /* +0x18 */
} scriptev;
static scriptev *sev(IUnknown *i) { return CONTAINING_RECORD(i, scriptev, IUnknown_iface); }
static HRESULT WINAPI sev_qi(IUnknown *i, REFIID r, void **o)     /* 0040f84e */
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &G_IidScriptEvent)) { *o = i; InterlockedIncrement(&sev(i)->ref); return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI sev_addref(IUnknown *i) { return InterlockedIncrement(&sev(i)->ref); }
static ULONG WINAPI sev_release(IUnknown *i)
{
    scriptev *x = sev(i);
    LONG n = InterlockedDecrement(&x->ref);
    if (!n) {
        if (x->state) IUnknown_Release(x->state);
        if (x->script) IUnknown_Release(x->script);
        free(x->routine);
        free(x);
    }
    return n;
}
static IUnknownVtbl sev_vtbl = { sev_qi, sev_addref, sev_release };

/* 0040fbe9: an event's message: due at its time (music time) with the tool flags its own ask for, to the track's
 * tool, the copy as its user object */
HRESULT rm_script_track_event(IDirectMusicPerformance *perf, IUnknown *state, IDirectMusicTool *tool, IUnknown *script,
                              const WCHAR *routine, DWORD evflags, MUSIC_TIME mt, DWORD vtrack)
{
    DMUS_PMSG *m = NULL;
    HRESULT hr = IDirectMusicPerformance_AllocPMsg(perf, sizeof(DMUS_PMSG), &m);
    if (FAILED(hr)) return hr;
    memset(m, 0, sizeof(*m));
    m->dwSize = sizeof(*m);
    scriptev *x = calloc(1, sizeof(*x));
    size_t n = wcslen(routine) + 1;
    if (x) x->routine = malloc(n * sizeof(WCHAR));
    if (!x || !x->routine) { free(x); IDirectMusicPerformance_FreePMsg(perf, m); return E_OUTOFMEMORY; }
    memcpy(x->routine, routine, n * sizeof(WCHAR));
    x->IUnknown_iface.lpVtbl = &sev_vtbl;
    x->ref = 1;
    x->state = state; if (state) IUnknown_AddRef(state);
    x->script = script; IUnknown_AddRef(script);
    DWORD f = (evflags & 1) ? DMUS_PMSGF_TOOL_IMMEDIATE : (evflags & 2) ? DMUS_PMSGF_TOOL_QUEUE :
              (evflags & 4) ? DMUS_PMSGF_TOOL_ATTIME : DMUS_PMSGF_MUSICTIME;
    m->mtTime = mt;
    m->dwFlags = f | DMUS_PMSGF_MUSICTIME;
    if (!(evflags & 4)) { x->t = mt; x->tflags = 0; }
    m->dwVirtualTrackID = vtrack;
    m->punkUser = &x->IUnknown_iface;
    m->pTool = tool; IDirectMusicTool_AddRef(tool);
    m->dwType = DMUS_PMSGT_USER;
    hr = IDirectMusicPerformance_SendPMsg(perf, m);
    if (SUCCEEDED(hr)) return hr;
    IDirectMusicPerformance_FreePMsg(perf, m);     /* (its tool and user object with it) */
    return hr;
}

/* 0040f8c2 with 00408a16 / 0040416a: the tool's ProcessPMsg of an event's message: the routine called with the
 * event's time as what the script plays starts from; a script error as a message when the track was asked */
HRESULT rm_script_track_process(IUnknown *user, DWORD vtrack, BOOL errnotify)
{
    if (!user) return E_POINTER;
    IUnknown *u = NULL;
    if (FAILED(IUnknown_QueryInterface(user, &G_IidScriptEvent, (void **)&u))) return DMUS_S_FREE;
    scriptev *x = sev(u);
    rm_script *s = script_of(x->script);
    if (s && !s->zombie) {
        EnterCriticalSection(&s->cs);
        if (s->mgr && s->perf) {
            manager *m = s->mgr;
            DMUS_SCRIPT_ERRORINFO ei;
            memset(&ei, 0, sizeof(ei));
            ei.dwSize = sizeof(ei);
            REFERENCE_TIME t0 = m->tstart; DWORD f0 = m->tflags;
            m->tstart = x->t; m->tflags = x->tflags;
            HRESULT hr = mgr_call(m, x->routine, &ei);
            m->tstart = t0; m->tflags = f0;
            /* (dmscript takes the performance from the host's reference count and crashes: the script's here) */
            if (errnotify && hr == E_SCRIPTERR)
                script_error_pmsg((IDirectMusicPerformance *)s->perf, x->state, vtrack, &ei);
        }
        LeaveCriticalSection(&s->cs);
    }
    IUnknown_Release(u);
    return DMUS_S_FREE;
}
