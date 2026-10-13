/* fxprobe.dll - a DMO that records what DirectSound feeds it: register it (tests/fxprobe.reg) and put it in a buffer's
 * effect chain with SetFX, after the effects under test.  It passes the audio through unchanged and logs the media
 * types it is given, every call, and the samples it processes:
 *   FXPROBE_LOG   text log of the calls (appended)
 *   FXPROBE_DATA  the sample data, as processed (appended)
 * Its class is {5e1d2c3b-7a4f-4c61-9d2e-0f8a1b6c4d55}. */
#define COBJMACROS
#include <windows.h>
#include <mediaobj.h>
#include <dmoreg.h>
#include <dmort.h>
#include <mediaerr.h>
#include <stdio.h>
#include <olectl.h>

static const GUID CLSID_FxProbe = { 0x5e1d2c3b, 0x7a4f, 0x4c61, { 0x9d, 0x2e, 0x0f, 0x8a, 0x1b, 0x6c, 0x4d, 0x55 } };
static const GUID IID_IMediaObject_ = { 0xd8ad0f58, 0x5494, 0x4102, { 0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4 } };
static const GUID IID_IMediaObjectInPlace_ = { 0x651b9ad0, 0x0fc7, 0x4aa9, { 0x95, 0x38, 0xd8, 0x99, 0x31, 0x01, 0x07, 0x41 } };
static LONG g_locks, g_objs;

static void plog(const char *fmt, ...)
{
    char b[512], *p = getenv("FXPROBE_LOG");
    if (!p) return;
    va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    FILE *f = fopen(p, "a");
    if (f) { fputs(b, f); fputc('\n', f); fclose(f); }
}
static void pdata(const BYTE *d, DWORD n)
{
    char *p = getenv("FXPROBE_DATA");
    if (!p) return;
    FILE *f = fopen(p, "ab");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

typedef struct {
    IMediaObject IMediaObject_iface;
    IMediaObjectInPlace IMediaObjectInPlace_iface;
    LONG ref;
    DMO_MEDIA_TYPE in, out;
    WAVEFORMATEX wf;
    BOOL hasin, hasout;
    DWORD calls;
} probe;

static probe *mo(IMediaObject *i) { return CONTAINING_RECORD(i, probe, IMediaObject_iface); }
static probe *ip(IMediaObjectInPlace *i) { return CONTAINING_RECORD(i, probe, IMediaObjectInPlace_iface); }

static HRESULT WINAPI mo_qi(IMediaObject *i, REFIID r, void **o)
{
    probe *p = mo(i);
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IMediaObject_)) *o = &p->IMediaObject_iface;
    else if (IsEqualIID(r, &IID_IMediaObjectInPlace_)) *o = &p->IMediaObjectInPlace_iface;
    else {
        plog("QueryInterface {%08lx-%04x-%04x-...}: no", r->Data1, r->Data2, r->Data3);
        *o = NULL; return E_NOINTERFACE;
    }
    InterlockedIncrement(&p->ref);
    return S_OK;
}
static ULONG WINAPI mo_addref(IMediaObject *i) { return InterlockedIncrement(&mo(i)->ref); }
static ULONG WINAPI mo_release(IMediaObject *i)
{
    probe *p = mo(i);
    LONG r = InterlockedDecrement(&p->ref);
    if (!r) { plog("released after %lu calls", p->calls); free(p); InterlockedDecrement(&g_objs); }
    return r;
}
static HRESULT WINAPI mo_streamcount(IMediaObject *i, DWORD *a, DWORD *b) { *a = *b = 1; return S_OK; }
static HRESULT WINAPI mo_ininfo(IMediaObject *i, DWORD s, DWORD *f) { *f = 0; return s ? DMO_E_INVALIDSTREAMINDEX : S_OK; }
static HRESULT WINAPI mo_outinfo(IMediaObject *i, DWORD s, DWORD *f) { *f = 0; return s ? DMO_E_INVALIDSTREAMINDEX : S_OK; }
static HRESULT WINAPI mo_intype(IMediaObject *i, DWORD s, DWORD t, DMO_MEDIA_TYPE *m) { return DMO_E_NO_MORE_ITEMS; }
static HRESULT WINAPI mo_outtype(IMediaObject *i, DWORD s, DWORD t, DMO_MEDIA_TYPE *m) { return DMO_E_NO_MORE_ITEMS; }
static void logtype(const char *what, const DMO_MEDIA_TYPE *m, DWORD f)
{
    if (!m) { plog("%s NULL flags %lx", what, f); return; }
    const WAVEFORMATEX *w = (const WAVEFORMATEX *)m->pbFormat;
    if (w && m->cbFormat >= sizeof(PCMWAVEFORMAT))
        plog("%s flags %lx major %08lx sub %08lx fixed %d size %lu fmt tag %u ch %u rate %lu bits %u align %u cb %lu", what, f,
             m->majortype.Data1, m->subtype.Data1, m->bFixedSizeSamples, m->lSampleSize, w->wFormatTag, w->nChannels,
             w->nSamplesPerSec, w->wBitsPerSample, w->nBlockAlign, m->cbFormat);
    else plog("%s flags %lx no format", what, f);
}
static HRESULT WINAPI mo_setin(IMediaObject *i, DWORD s, const DMO_MEDIA_TYPE *m, DWORD f)
{
    probe *p = mo(i);
    logtype("SetInputType", m, f);
    if (m && m->pbFormat && !(f & DMO_SET_TYPEF_TEST_ONLY)) { memcpy(&p->wf, m->pbFormat, sizeof(PCMWAVEFORMAT)); p->hasin = TRUE; }
    return S_OK;
}
static HRESULT WINAPI mo_setout(IMediaObject *i, DWORD s, const DMO_MEDIA_TYPE *m, DWORD f)
{
    logtype("SetOutputType", m, f);
    if (m && !(f & DMO_SET_TYPEF_TEST_ONLY)) mo(i)->hasout = TRUE;
    return S_OK;
}
static HRESULT WINAPI mo_curin(IMediaObject *i, DWORD s, DMO_MEDIA_TYPE *m) { plog("GetInputCurrentType"); return DMO_E_TYPE_NOT_SET; }
static HRESULT WINAPI mo_curout(IMediaObject *i, DWORD s, DMO_MEDIA_TYPE *m) { plog("GetOutputCurrentType"); return DMO_E_TYPE_NOT_SET; }
static HRESULT WINAPI mo_insize(IMediaObject *i, DWORD s, DWORD *a, DWORD *b, DWORD *c) { plog("GetInputSizeInfo"); *a = *b = *c = 0; return E_NOTIMPL; }
static HRESULT WINAPI mo_outsize(IMediaObject *i, DWORD s, DWORD *a, DWORD *b) { plog("GetOutputSizeInfo"); *a = *b = 0; return E_NOTIMPL; }
static HRESULT WINAPI mo_getlat(IMediaObject *i, DWORD s, REFERENCE_TIME *t) { plog("GetInputMaxLatency"); return E_NOTIMPL; }
static HRESULT WINAPI mo_setlat(IMediaObject *i, DWORD s, REFERENCE_TIME t) { plog("SetInputMaxLatency"); return E_NOTIMPL; }
static HRESULT WINAPI mo_flush(IMediaObject *i) { plog("Flush"); return S_OK; }
static HRESULT WINAPI mo_discont(IMediaObject *i, DWORD s) { plog("Discontinuity"); return S_OK; }
static HRESULT WINAPI mo_alloc(IMediaObject *i) { plog("AllocateStreamingResources"); return S_OK; }
static HRESULT WINAPI mo_free(IMediaObject *i) { plog("FreeStreamingResources"); return S_OK; }
static HRESULT WINAPI mo_instatus(IMediaObject *i, DWORD s, DWORD *f) { plog("GetInputStatus"); *f = DMO_INPUT_STATUSF_ACCEPT_DATA; return S_OK; }
static HRESULT WINAPI mo_procin(IMediaObject *i, DWORD s, IMediaBuffer *b, DWORD f, REFERENCE_TIME t, REFERENCE_TIME l)
{ plog("ProcessInput"); return E_NOTIMPL; }
static HRESULT WINAPI mo_procout(IMediaObject *i, DWORD f, DWORD n, DMO_OUTPUT_DATA_BUFFER *b, DWORD *st)
{ plog("ProcessOutput"); return E_NOTIMPL; }
static HRESULT WINAPI mo_lock(IMediaObject *i, LONG l) { return S_OK; }
static IMediaObjectVtbl mo_vtbl = { mo_qi, mo_addref, mo_release, mo_streamcount, mo_ininfo, mo_outinfo, mo_intype,
    mo_outtype, mo_setin, mo_setout, mo_curin, mo_curout, mo_insize, mo_outsize, mo_getlat, mo_setlat, mo_flush, mo_discont,
    mo_alloc, mo_free, mo_instatus, mo_procin, mo_procout, mo_lock };

static HRESULT WINAPI ip_qi(IMediaObjectInPlace *i, REFIID r, void **o) { return mo_qi(&ip(i)->IMediaObject_iface, r, o); }
static ULONG WINAPI ip_addref(IMediaObjectInPlace *i) { return mo_addref(&ip(i)->IMediaObject_iface); }
static ULONG WINAPI ip_release(IMediaObjectInPlace *i) { return mo_release(&ip(i)->IMediaObject_iface); }
static HRESULT WINAPI ip_process(IMediaObjectInPlace *i, ULONG n, BYTE *d, REFERENCE_TIME t, DWORD f)
{
    probe *p = ip(i);
    p->calls++;
    if (getenv("FXPROBE_VERBOSE")) plog("Process %lu bytes time %lld flags %lx", n, (long long)t, f);
    pdata(d, n);
    return S_OK;
}
static HRESULT WINAPI cf_create(IClassFactory *i, IUnknown *outer, REFIID r, void **o);
/* a new probe (DirectSound clones a buffer configuration's effects for each buffer made from it) */
static HRESULT WINAPI ip_clone(IMediaObjectInPlace *i, IMediaObjectInPlace **o)
{
    plog("Clone");
    if (!o) return E_POINTER;
    return cf_create(NULL, NULL, &IID_IMediaObjectInPlace_, (void **)o);
}
static HRESULT WINAPI ip_latency(IMediaObjectInPlace *i, REFERENCE_TIME *t) { plog("GetLatency"); *t = 0; return S_OK; }
static IMediaObjectInPlaceVtbl ip_vtbl = { ip_qi, ip_addref, ip_release, ip_process, ip_clone, ip_latency };

/* ------------------------------------------------------------------ class factory */
static HRESULT WINAPI cf_qi(IClassFactory *i, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IClassFactory)) { *o = i; return S_OK; }
    *o = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI cf_addref(IClassFactory *i) { return 2; }
static ULONG WINAPI cf_release(IClassFactory *i) { return 1; }
static HRESULT WINAPI cf_create(IClassFactory *i, IUnknown *outer, REFIID r, void **o)
{
    *o = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    probe *p = calloc(1, sizeof(*p));
    if (!p) return E_OUTOFMEMORY;
    p->IMediaObject_iface.lpVtbl = &mo_vtbl;
    p->IMediaObjectInPlace_iface.lpVtbl = &ip_vtbl;
    p->ref = 1;
    InterlockedIncrement(&g_objs);
    plog("created");
    HRESULT hr = mo_qi(&p->IMediaObject_iface, r, o);
    mo_release(&p->IMediaObject_iface);
    return hr;
}
static HRESULT WINAPI cf_lock(IClassFactory *i, BOOL l) { if (l) InterlockedIncrement(&g_locks); else InterlockedDecrement(&g_locks); return S_OK; }
static IClassFactoryVtbl cf_vtbl = { cf_qi, cf_addref, cf_release, cf_create, cf_lock };
static IClassFactory s_cf = { &cf_vtbl };

HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, void **o)
{
    if (!IsEqualCLSID(c, &CLSID_FxProbe)) { *o = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
    return cf_qi(&s_cf, r, o);
}
HRESULT WINAPI DllCanUnloadNow(void) { return g_locks || g_objs ? S_FALSE : S_OK; }

static HMODULE g_self;
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, void *r) { if (why == DLL_PROCESS_ATTACH) g_self = h; return TRUE; }
HRESULT WINAPI DllRegisterServer(void)
{
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    HKEY k;
    if (RegCreateKeyExA(HKEY_CLASSES_ROOT, "CLSID\\{5E1D2C3B-7A4F-4C61-9D2E-0F8A1B6C4D55}\\InprocServer32", 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL)) return SELFREG_E_CLASS;
    RegSetValueExA(k, NULL, 0, REG_SZ, (BYTE *)path, strlen(path) + 1);
    RegSetValueExA(k, "ThreadingModel", 0, REG_SZ, (BYTE *)"Both", 5);
    RegCloseKey(k);
    return S_OK;
}
HRESULT WINAPI DllUnregisterServer(void)
{
    RegDeleteKeyA(HKEY_CLASSES_ROOT, "CLSID\\{5E1D2C3B-7A4F-4C61-9D2E-0F8A1B6C4D55}\\InprocServer32");
    RegDeleteKeyA(HKEY_CLASSES_ROOT, "CLSID\\{5E1D2C3B-7A4F-4C61-9D2E-0F8A1B6C4D55}");
    return S_OK;
}
