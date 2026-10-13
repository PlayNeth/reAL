/* reAL: DirectSound buffer effects - the chain SetFX builds, and our standard effects as COM objects.
 *
 * What DirectSound (Windows 7) does, as tests/fxtest.c finds it: SetFX makes each effect (CoCreateInstance of its
 * class; the standard ones are dsdmo.dll's DMOs) and gives it the buffer's own format with SetInputType and
 * SetOutputType; Play and SetCurrentPosition call Discontinuity; the mixer then runs IMediaObjectInPlace::Process
 * (time 0, flags 0) on a copy of the buffer ahead of the play cursor.  The application's memory is never touched.
 * GetObjectInPath hands out the effect objects themselves, which outlive the chain if the application keeps them. */
#include "fx.h"
#include "dsfx.h"
#include <mediaobj.h>
#include <mediaerr.h>
#include <objidl.h>

#ifndef DMO_INPUT_STREAMF_WHOLE_SAMPLES
#define DMO_INPUT_STREAMF_WHOLE_SAMPLES 0x1
#define DMO_INPUT_STREAMF_FIXED_SAMPLE_SIZE 0x4
#define DMO_OUTPUT_STREAMF_WHOLE_SAMPLES 0x1
#define DMO_OUTPUT_STREAMF_FIXED_SAMPLE_SIZE 0x4
#endif

static const GUID RL_IID_IMediaObject = { 0xd8ad0f58, 0x5494, 0x4102, { 0x97, 0xc5, 0xec, 0x79, 0x8e, 0x59, 0xbc, 0xf4 } };
static const GUID RL_IID_IMediaObjectInPlace = { 0x651b9ad0, 0x0fc7, 0x4aa9, { 0x95, 0x38, 0xd8, 0x99, 0x31, 0x01, 0x07, 0x41 } };
static const GUID RL_IID_IPersistStream = { 0x00000109, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID RL_IID_IPersist = { 0x0000010c, 0, 0, { 0xc0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID RL_GUID_All_Objects = { 0xaa114de5, 0xc262, 0x4169, { 0xa1, 0xc8, 0x23, 0xd6, 0x98, 0xcc, 0x73, 0xb5 } };
static const GUID RL_MEDIATYPE_Audio = { 0x73647561, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID RL_FORMAT_WaveFormatEx = { 0x05589f81, 0xc356, 0x11ce, { 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a } };

/* the standard effects: class, interface (IDirectSoundFX*8), dsfx kind */
static const struct { const GUID *cls; const GUID *iid; int kind; } s_std[] = {
    { &GUID_DSFX_STANDARD_GARGLE, &IID_IDirectSoundFXGargle8, DSFX_GARGLE },
    { &GUID_DSFX_STANDARD_CHORUS, &IID_IDirectSoundFXChorus8, DSFX_CHORUS },
    { &GUID_DSFX_STANDARD_FLANGER, &IID_IDirectSoundFXFlanger8, DSFX_FLANGER },
    { &GUID_DSFX_STANDARD_ECHO, &IID_IDirectSoundFXEcho8, DSFX_ECHO },
    { &GUID_DSFX_STANDARD_DISTORTION, &IID_IDirectSoundFXDistortion8, DSFX_DISTORTION },
    { &GUID_DSFX_STANDARD_COMPRESSOR, &IID_IDirectSoundFXCompressor8, DSFX_COMPRESSOR },
    { &GUID_DSFX_STANDARD_PARAMEQ, &IID_IDirectSoundFXParamEq8, DSFX_PARAMEQ },
    { &GUID_DSFX_WAVES_REVERB, &IID_IDirectSoundFXWavesReverb8, DSFX_WAVES },
    { &GUID_DSFX_STANDARD_I3DL2REVERB, &IID_IDirectSoundFXI3DL2Reverb8, DSFX_I3DL2 },  /* Windows XP's */
};
#define NSTD (sizeof(s_std) / sizeof(s_std[0]))

/* fixes of dsdmo's flaws: on, except for comparison runs */
static uint32_t fx_fixes(void)
{
    if (GetEnvironmentVariableA("REAL_NOFIXES", NULL, 0) || GetEnvironmentVariableA("REAL_MUSIC_NOFIXES", NULL, 0)) return 0;
    return DSFX_FIX_ECHO_PANDELAY | DSFX_FIX_I3DL2_QUALITY;
}

/* ------------------------------------------------------------------ an effect object */
/* every IDirectSoundFX* interface starts the same way; IDirectSoundFXI3DL2Reverb has four methods more */
typedef struct fxvtbl {
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *SetAllParameters)(void *, const void *);
    HRESULT (WINAPI *GetAllParameters)(void *, void *);
    HRESULT (WINAPI *SetPreset)(void *, DWORD);
    HRESULT (WINAPI *GetPreset)(void *, DWORD *);
    HRESULT (WINAPI *SetQuality)(void *, LONG);
    HRESULT (WINAPI *GetQuality)(void *, LONG *);
} fxvtbl;

typedef struct rl_fx {
    const fxvtbl *fx_vtbl;                  /* the IDirectSoundFX* interface */
    IMediaObject IMediaObject_iface;
    IMediaObjectInPlace IMediaObjectInPlace_iface;
    IPersistStream IPersistStream_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    int kind, std;
    dsfx *fx;
    WAVEFORMATEX wf;
    BOOL has_in, has_out;
} rl_fx;

static rl_fx *from_fx(void *i) { return (rl_fx *)i; }
static rl_fx *from_mo(IMediaObject *i) { return impl_from(i, rl_fx, IMediaObject_iface); }
static rl_fx *from_ip(IMediaObjectInPlace *i) { return impl_from(i, rl_fx, IMediaObjectInPlace_iface); }
static rl_fx *from_ps(IPersistStream *i) { return impl_from(i, rl_fx, IPersistStream_iface); }

static HRESULT fx_qi(rl_fx *This, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, s_std[This->std].iid)) *out = &This->fx_vtbl;
    else if (IsEqualIID(riid, &RL_IID_IMediaObject)) *out = &This->IMediaObject_iface;
    else if (IsEqualIID(riid, &RL_IID_IMediaObjectInPlace)) *out = &This->IMediaObjectInPlace_iface;
    else if (IsEqualIID(riid, &RL_IID_IPersistStream) || IsEqualIID(riid, &RL_IID_IPersist)) *out = &This->IPersistStream_iface;
    else return E_NOINTERFACE;
    InterlockedIncrement(&This->ref);
    return S_OK;
}
static ULONG fx_addref(rl_fx *This) { return InterlockedIncrement(&This->ref); }
static ULONG fx_release(rl_fx *This)
{
    LONG r = InterlockedDecrement(&This->ref);
    if (!r) {
        dsfx_free(This->fx);
        DeleteCriticalSection(&This->cs);
        HeapFree(GetProcessHeap(), 0, This);
    }
    return r;
}

static HRESULT WINAPI fxi_QueryInterface(void *i, REFIID riid, void **out) { return fx_qi(from_fx(i), riid, out); }
static ULONG WINAPI fxi_AddRef(void *i) { return fx_addref(from_fx(i)); }
static ULONG WINAPI fxi_Release(void *i) { return fx_release(from_fx(i)); }
static HRESULT WINAPI fxi_SetAllParameters(void *i, const void *p)
{
    rl_fx *This = from_fx(i);
    if (!p) return E_POINTER;
    unsigned int cw = rl_fpu_enter();
    EnterCriticalSection(&This->cs);
    HRESULT hr = dsfx_set_all(This->fx, p);
    LeaveCriticalSection(&This->cs);
    rl_fpu_leave(cw);
    LOG_CALL("FX[%d]::SetAllParameters -> %08lx", This->kind, (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI fxi_GetAllParameters(void *i, void *p)
{
    rl_fx *This = from_fx(i);
    if (!p) return E_POINTER;
    EnterCriticalSection(&This->cs);
    HRESULT hr = dsfx_get_all(This->fx, p);
    LeaveCriticalSection(&This->cs);
    return hr;
}
static const fxvtbl fxi_vtbl = { fxi_QueryInterface, fxi_AddRef, fxi_Release, fxi_SetAllParameters, fxi_GetAllParameters };
/* IDirectSoundFXI3DL2Reverb: XP's methods check their pointers themselves (E_POINTER) */
static HRESULT WINAPI fxi_SetPreset(void *i, DWORD p)
{
    rl_fx *This = from_fx(i);
    EnterCriticalSection(&This->cs);
    unsigned int cw = rl_fpu_enter();
    HRESULT hr = dsfx_i3dl2_set_preset(This->fx, p);
    rl_fpu_leave(cw);
    LeaveCriticalSection(&This->cs);
    LOG_CALL("FX[I3DL2]::SetPreset(%lu) -> %08lx", (unsigned long)p, (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI fxi_GetPreset(void *i, DWORD *p)
{
    rl_fx *This = from_fx(i);
    if (!p) return E_POINTER;
    uint32_t v = 0;
    EnterCriticalSection(&This->cs);
    HRESULT hr = dsfx_i3dl2_get_preset(This->fx, &v);
    LeaveCriticalSection(&This->cs);
    if (SUCCEEDED(hr)) *p = v;
    return hr;
}
static HRESULT WINAPI fxi_SetQuality(void *i, LONG q)
{
    rl_fx *This = from_fx(i);
    EnterCriticalSection(&This->cs);
    unsigned int cw = rl_fpu_enter();
    HRESULT hr = dsfx_i3dl2_set_quality(This->fx, q);
    rl_fpu_leave(cw);
    LeaveCriticalSection(&This->cs);
    LOG_CALL("FX[I3DL2]::SetQuality(%ld) -> %08lx", (long)q, (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI fxi_GetQuality(void *i, LONG *q)
{
    rl_fx *This = from_fx(i);
    if (!q) return E_POINTER;
    int32_t v = 0;
    EnterCriticalSection(&This->cs);
    unsigned int cw = rl_fpu_enter();
    HRESULT hr = dsfx_i3dl2_get_quality(This->fx, &v);
    rl_fpu_leave(cw);
    LeaveCriticalSection(&This->cs);
    *q = v;
    return hr;
}
static const fxvtbl fxi_i3dl2_vtbl = { fxi_QueryInterface, fxi_AddRef, fxi_Release, fxi_SetAllParameters,
                                       fxi_GetAllParameters, fxi_SetPreset, fxi_GetPreset, fxi_SetQuality, fxi_GetQuality };

/* IMediaObject */
static HRESULT WINAPI mo_QueryInterface(IMediaObject *i, REFIID riid, void **out) { return fx_qi(from_mo(i), riid, out); }
static ULONG WINAPI mo_AddRef(IMediaObject *i) { return fx_addref(from_mo(i)); }
static ULONG WINAPI mo_Release(IMediaObject *i) { return fx_release(from_mo(i)); }
static HRESULT WINAPI mo_GetStreamCount(IMediaObject *i, DWORD *in, DWORD *out)
{
    if (!in || !out) return E_POINTER;
    *in = *out = 1;
    return S_OK;
}
static HRESULT WINAPI mo_GetInputStreamInfo(IMediaObject *i, DWORD s, DWORD *f)
{
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    if (!f) return E_POINTER;
    *f = DMO_INPUT_STREAMF_WHOLE_SAMPLES | DMO_INPUT_STREAMF_FIXED_SAMPLE_SIZE;
    return S_OK;
}
static HRESULT WINAPI mo_GetOutputStreamInfo(IMediaObject *i, DWORD s, DWORD *f)
{
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    if (!f) return E_POINTER;
    *f = DMO_OUTPUT_STREAMF_WHOLE_SAMPLES | DMO_OUTPUT_STREAMF_FIXED_SAMPLE_SIZE;
    return S_OK;
}
static HRESULT WINAPI mo_GetType(IMediaObject *i, DWORD s, DWORD t, DMO_MEDIA_TYPE *mt)
{
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    return DMO_E_NO_MORE_ITEMS;
}
/* the formats the effects take: PCM 8/16 or float 32 (Waves: no 8-bit), mono or stereo, as a WAVEFORMATEX */
static HRESULT check_type(rl_fx *This, const DMO_MEDIA_TYPE *mt, WAVEFORMATEX *wf)
{
    if (!mt) return E_POINTER;
    if (!IsEqualGUID(&mt->majortype, &RL_MEDIATYPE_Audio) || !IsEqualGUID(&mt->formattype, &RL_FORMAT_WaveFormatEx) ||
        mt->cbFormat < sizeof(PCMWAVEFORMAT) || !mt->pbFormat) return DMO_E_TYPE_NOT_ACCEPTED;
    const WAVEFORMATEX *w = (const WAVEFORMATEX *)mt->pbFormat;
    if (w->wFormatTag == WAVE_FORMAT_PCM) { if (w->wBitsPerSample != 8 && w->wBitsPerSample != 16) return DMO_E_TYPE_NOT_ACCEPTED; }
    else if (w->wFormatTag != WAVE_FORMAT_IEEE_FLOAT || w->wBitsPerSample != 32) return DMO_E_TYPE_NOT_ACCEPTED;
    if (w->nChannels != 1 && w->nChannels != 2) return DMO_E_TYPE_NOT_ACCEPTED;
    if (w->nBlockAlign != w->nChannels * w->wBitsPerSample / 8 || !w->nSamplesPerSec) return DMO_E_TYPE_NOT_ACCEPTED;
    if (This->kind == DSFX_WAVES && w->wBitsPerSample == 8) return DMO_E_TYPE_NOT_ACCEPTED;
    memcpy(wf, w, sizeof(PCMWAVEFORMAT));
    wf->cbSize = 0;
    return S_OK;
}
static HRESULT set_type(rl_fx *This, DWORD s, const DMO_MEDIA_TYPE *mt, DWORD flags, BOOL out)
{
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    if (flags & DMO_SET_TYPEF_CLEAR) {
        EnterCriticalSection(&This->cs);
        if (out) This->has_out = FALSE; else This->has_in = FALSE;
        LeaveCriticalSection(&This->cs);
        return S_OK;
    }
    WAVEFORMATEX wf;
    HRESULT hr = check_type(This, mt, &wf);
    if (FAILED(hr)) return hr;
    EnterCriticalSection(&This->cs);
    /* in place: the other side, once set, has to be the same format */
    if ((out ? This->has_in : This->has_out) && memcmp(&wf, &This->wf, sizeof(PCMWAVEFORMAT))) hr = DMO_E_TYPE_NOT_ACCEPTED;
    else if (!(flags & DMO_SET_TYPEF_TEST_ONLY)) {
        unsigned int cw = rl_fpu_enter();
        hr = dsfx_set_format(This->fx, wf.wFormatTag, wf.nChannels, wf.nSamplesPerSec, wf.wBitsPerSample);
        rl_fpu_leave(cw);
        if (SUCCEEDED(hr)) {
            This->wf = wf;
            if (out) This->has_out = TRUE; else This->has_in = TRUE;
        }
    }
    LeaveCriticalSection(&This->cs);
    return hr;
}
static HRESULT WINAPI mo_SetInputType(IMediaObject *i, DWORD s, const DMO_MEDIA_TYPE *mt, DWORD f) { return set_type(from_mo(i), s, mt, f, FALSE); }
static HRESULT WINAPI mo_SetOutputType(IMediaObject *i, DWORD s, const DMO_MEDIA_TYPE *mt, DWORD f) { return set_type(from_mo(i), s, mt, f, TRUE); }
static HRESULT cur_type(rl_fx *This, DWORD s, DMO_MEDIA_TYPE *mt, BOOL out)
{
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    if (!mt) return E_POINTER;
    if (!(out ? This->has_out : This->has_in)) return DMO_E_TYPE_NOT_SET;
    memset(mt, 0, sizeof(*mt));
    mt->majortype = RL_MEDIATYPE_Audio;
    mt->subtype = RL_MEDIATYPE_Audio;
    mt->subtype.Data1 = This->wf.wFormatTag;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = This->wf.nBlockAlign;
    mt->formattype = RL_FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(WAVEFORMATEX);
    mt->pbFormat = CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!mt->pbFormat) return E_OUTOFMEMORY;
    memcpy(mt->pbFormat, &This->wf, sizeof(WAVEFORMATEX));
    return S_OK;
}
static HRESULT WINAPI mo_GetInputCurrentType(IMediaObject *i, DWORD s, DMO_MEDIA_TYPE *mt) { return cur_type(from_mo(i), s, mt, FALSE); }
static HRESULT WINAPI mo_GetOutputCurrentType(IMediaObject *i, DWORD s, DMO_MEDIA_TYPE *mt) { return cur_type(from_mo(i), s, mt, TRUE); }
static HRESULT WINAPI mo_GetInputSizeInfo(IMediaObject *i, DWORD s, DWORD *size, DWORD *look, DWORD *align)
{
    rl_fx *This = from_mo(i);
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    if (!size || !look || !align) return E_POINTER;
    if (!This->has_in) return DMO_E_TYPE_NOT_SET;
    *size = This->wf.nBlockAlign; *look = 0; *align = 1;
    return S_OK;
}
static HRESULT WINAPI mo_GetOutputSizeInfo(IMediaObject *i, DWORD s, DWORD *size, DWORD *align)
{
    rl_fx *This = from_mo(i);
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    if (!size || !align) return E_POINTER;
    if (!This->has_out) return DMO_E_TYPE_NOT_SET;
    *size = This->wf.nBlockAlign; *align = 1;
    return S_OK;
}
static HRESULT WINAPI mo_GetInputMaxLatency(IMediaObject *i, DWORD s, REFERENCE_TIME *t) { return E_NOTIMPL; }
static HRESULT WINAPI mo_SetInputMaxLatency(IMediaObject *i, DWORD s, REFERENCE_TIME t) { return E_NOTIMPL; }
static HRESULT WINAPI mo_Flush(IMediaObject *i)
{
    rl_fx *This = from_mo(i);
    EnterCriticalSection(&This->cs);
    dsfx_discontinuity(This->fx);
    LeaveCriticalSection(&This->cs);
    return S_OK;
}
static HRESULT WINAPI mo_Discontinuity(IMediaObject *i, DWORD s)
{
    rl_fx *This = from_mo(i);
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    EnterCriticalSection(&This->cs);
    dsfx_discontinuity(This->fx);
    LeaveCriticalSection(&This->cs);
    return S_OK;
}
static HRESULT WINAPI mo_AllocateStreamingResources(IMediaObject *i) { return S_OK; }
static HRESULT WINAPI mo_FreeStreamingResources(IMediaObject *i) { return S_OK; }
static HRESULT WINAPI mo_GetInputStatus(IMediaObject *i, DWORD s, DWORD *f)
{
    if (s) return DMO_E_INVALIDSTREAMINDEX;
    if (!f) return E_POINTER;
    *f = DMO_INPUT_STATUSF_ACCEPT_DATA;
    return S_OK;
}
/* the buffered (not in place) interface: not offered, DirectSound only uses the in-place one */
static HRESULT WINAPI mo_ProcessInput(IMediaObject *i, DWORD s, IMediaBuffer *b, DWORD f, REFERENCE_TIME t, REFERENCE_TIME l) { return E_NOTIMPL; }
static HRESULT WINAPI mo_ProcessOutput(IMediaObject *i, DWORD f, DWORD n, DMO_OUTPUT_DATA_BUFFER *b, DWORD *st) { return E_NOTIMPL; }
static HRESULT WINAPI mo_Lock(IMediaObject *i, LONG l)
{
    rl_fx *This = from_mo(i);
    if (l) EnterCriticalSection(&This->cs); else LeaveCriticalSection(&This->cs);
    return S_OK;
}
static IMediaObjectVtbl mo_vtbl = {
    mo_QueryInterface, mo_AddRef, mo_Release, mo_GetStreamCount, mo_GetInputStreamInfo, mo_GetOutputStreamInfo,
    mo_GetType, mo_GetType, mo_SetInputType, mo_SetOutputType, mo_GetInputCurrentType, mo_GetOutputCurrentType,
    mo_GetInputSizeInfo, mo_GetOutputSizeInfo, mo_GetInputMaxLatency, mo_SetInputMaxLatency, mo_Flush,
    mo_Discontinuity, mo_AllocateStreamingResources, mo_FreeStreamingResources, mo_GetInputStatus, mo_ProcessInput,
    mo_ProcessOutput, mo_Lock,
};

/* IMediaObjectInPlace */
static HRESULT WINAPI ip_QueryInterface(IMediaObjectInPlace *i, REFIID riid, void **out) { return fx_qi(from_ip(i), riid, out); }
static ULONG WINAPI ip_AddRef(IMediaObjectInPlace *i) { return fx_addref(from_ip(i)); }
static ULONG WINAPI ip_Release(IMediaObjectInPlace *i) { return fx_release(from_ip(i)); }
static HRESULT WINAPI ip_Process(IMediaObjectInPlace *i, ULONG size, BYTE *data, REFERENCE_TIME t, DWORD flags)
{
    rl_fx *This = from_ip(i);
    if (!data) return E_POINTER;
    EnterCriticalSection(&This->cs);
    if (!This->has_in || !This->has_out) { LeaveCriticalSection(&This->cs); return DMO_E_TYPE_NOT_SET; }
    if (flags & DMO_INPLACE_ZERO) memset(data, This->wf.wBitsPerSample == 8 ? 0x80 : 0, size);
    unsigned int cw = rl_fpu_enter();
    dsfx_process(This->fx, data, size / This->wf.nBlockAlign);
    rl_fpu_leave(cw);
    LeaveCriticalSection(&This->cs);
    return S_OK;
}
static rl_fx *fx_new(int std);
/* Clone (DirectSound clones a buffer configuration's effects for each buffer made from it): the same effect with
 * the same parameters and, if set, the same format */
static HRESULT WINAPI ip_Clone(IMediaObjectInPlace *i, IMediaObjectInPlace **out)
{
    rl_fx *This = from_ip(i);
    if (!out) return E_POINTER;
    *out = NULL;
    rl_fx *n = fx_new(This->std);
    if (!n) return E_OUTOFMEMORY;
    BYTE p[64];
    unsigned int cw = rl_fpu_enter();
    EnterCriticalSection(&This->cs);
    dsfx_get_all(This->fx, p);
    dsfx_set_all(n->fx, p);
    if (This->kind == DSFX_I3DL2) {
        int32_t q = 0;
        if (SUCCEEDED(dsfx_i3dl2_get_quality(This->fx, &q))) dsfx_i3dl2_set_quality(n->fx, q);
    }
    if (This->has_in || This->has_out) {
        dsfx_set_format(n->fx, This->wf.wFormatTag, This->wf.nChannels, This->wf.nSamplesPerSec, This->wf.wBitsPerSample);
        n->wf = This->wf; n->has_in = This->has_in; n->has_out = This->has_out;
    }
    LeaveCriticalSection(&This->cs);
    rl_fpu_leave(cw);
    *out = &n->IMediaObjectInPlace_iface;
    return S_OK;
}
static HRESULT WINAPI ip_GetLatency(IMediaObjectInPlace *i, REFERENCE_TIME *t) { if (!t) return E_POINTER; *t = 0; return S_OK; }
static IMediaObjectInPlaceVtbl ip_vtbl = { ip_QueryInterface, ip_AddRef, ip_Release, ip_Process, ip_Clone, ip_GetLatency };

/* IPersistStream: the parameters as their DSFX structure (as an audio path's DMO data carries them) */
static HRESULT WINAPI ps_QueryInterface(IPersistStream *i, REFIID riid, void **out) { return fx_qi(from_ps(i), riid, out); }
static ULONG WINAPI ps_AddRef(IPersistStream *i) { return fx_addref(from_ps(i)); }
static ULONG WINAPI ps_Release(IPersistStream *i) { return fx_release(from_ps(i)); }
static HRESULT WINAPI ps_GetClassID(IPersistStream *i, CLSID *cls)
{
    if (!cls) return E_POINTER;
    *cls = *s_std[from_ps(i)->std].cls;
    return S_OK;
}
static HRESULT WINAPI ps_IsDirty(IPersistStream *i) { return S_FALSE; }
static HRESULT WINAPI ps_Load(IPersistStream *i, IStream *s)
{
    rl_fx *This = from_ps(i);
    BYTE p[64];
    ULONG n = (ULONG)dsfx_params_size(This->kind), got = 0;
    if (!s) return E_POINTER;
    HRESULT hr = IStream_Read(s, p, n, &got);
    if (FAILED(hr)) return hr;
    if (got != n) return E_FAIL;
    return fxi_SetAllParameters(&This->fx_vtbl, p);
}
static HRESULT WINAPI ps_Save(IPersistStream *i, IStream *s, BOOL clear)
{
    rl_fx *This = from_ps(i);
    BYTE p[64];
    if (!s) return E_POINTER;
    fxi_GetAllParameters(&This->fx_vtbl, p);
    return IStream_Write(s, p, (ULONG)dsfx_params_size(This->kind), NULL);
}
static HRESULT WINAPI ps_GetSizeMax(IPersistStream *i, ULARGE_INTEGER *size)
{
    if (!size) return E_POINTER;
    size->QuadPart = (ULONGLONG)dsfx_params_size(from_ps(i)->kind);
    return S_OK;
}
static IPersistStreamVtbl ps_vtbl = { ps_QueryInterface, ps_AddRef, ps_Release, ps_GetClassID, ps_IsDirty, ps_Load,
                                      ps_Save, ps_GetSizeMax };

static int std_index(REFCLSID cls)
{
    for (unsigned i = 0; i < NSTD; i++) if (IsEqualGUID(cls, s_std[i].cls)) return (int)i;
    return -1;
}

static rl_fx *fx_new(int std)
{
    rl_fx *This = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*This));
    if (!This) return NULL;
    unsigned int cw = rl_fpu_enter();
    This->fx = dsfx_new(s_std[std].kind);
    rl_fpu_leave(cw);
    if (!This->fx) { HeapFree(GetProcessHeap(), 0, This); return NULL; }
    dsfx_set_fixes(This->fx, fx_fixes());
    This->fx_vtbl = s_std[std].kind == DSFX_I3DL2 ? &fxi_i3dl2_vtbl : &fxi_vtbl;
    This->IMediaObject_iface.lpVtbl = &mo_vtbl;
    This->IMediaObjectInPlace_iface.lpVtbl = &ip_vtbl;
    This->IPersistStream_iface.lpVtbl = &ps_vtbl;
    This->ref = 1;
    This->std = std;
    This->kind = s_std[std].kind;
    InitializeCriticalSection(&This->cs);
    return This;
}

HRESULT rl_fx_create(REFCLSID cls, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    int s = std_index(cls);
    if (s < 0) return CLASS_E_CLASSNOTAVAILABLE;
    rl_fx *This = fx_new(s);
    if (!This) return E_OUTOFMEMORY;
    HRESULT hr = fx_qi(This, iid, out);
    fx_release(This);
    return hr;
}

/* ------------------------------------------------------------------ the chain */
typedef struct {
    GUID cls;
    IUnknown *unk;                      /* the object (ours or a hosted DMO) */
    IMediaObject *mo;
    IMediaObjectInPlace *ip;
    rl_fx *ours;                        /* our object: processed directly */
    struct rl_send *send;               /* a send (GUID_DSFX_SEND): no DMO */
    DWORD status;
} fxentry;

struct rl_fxchain {
    DWORD n;
    DWORD block;
    const rl_sendops *ops; void *ctx;   /* where sends go */
    int nch;
    fxentry e[1];
};

DWORD rl_fxchain_count(const rl_fxchain *c) { return c ? c->n : 0; }

/* ------------------------------------------------------------------ the send effect (DirectSound's 6efdaa1b): adds the
 * buffer's data, as it is at that point of the chain, to a mix-in buffer's, at the send level */
static const GUID RL_GUID_DSFX_SEND = { 0xef602176, 0xbcbb, 0x49e0, { 0x8c, 0xca, 0xe0, 0x9a, 0x5a, 0x15, 0x2b, 0x33 } };
static const GUID RL_IID_IDirectSoundFXSend = { 0xb30f3564, 0x1698, 0x45ba, { 0x9f, 0x75, 0xfc, 0x3c, 0x6c, 0x3b, 0x28, 0x10 } };
#include "sendgain.inc"
typedef struct rl_send {
    const void *vtbl;
    LONG ref;
    LONG level;                         /* +0x4c */
    uint16_t gain;                      /* +0x50 */
    int target;
} rl_send;
typedef struct { LONG lSendLevel; } RL_DSFXSend;
/* 6efb3079: millibels to the 16-bit gain */
static uint16_t send_gain(LONG mb)
{
    if (mb >= 0) return 0xffff;
    LONG i = (mb + 5) / 10;
    if (i > -1) i = -1;
    if (i < -964) i = -964;
    return s_sendgain[i + 964];
}
static HRESULT WINAPI sd_QueryInterface(void *i, REFIID riid, void **out)
{
    if (!out) return E_INVALIDARG;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &RL_IID_IDirectSoundFXSend)) {
        *out = i;
        InterlockedIncrement(&((rl_send *)i)->ref);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI sd_AddRef(void *i) { return InterlockedIncrement(&((rl_send *)i)->ref); }
static ULONG WINAPI sd_Release(void *i)
{
    LONG r = InterlockedDecrement(&((rl_send *)i)->ref);
    if (!r) HeapFree(GetProcessHeap(), 0, i);
    return r;
}
/* 6efda3c5 / 6efda400 */
static HRESULT WINAPI sd_SetAllParameters(void *i, const RL_DSFXSend *p)
{
    rl_send *This = i;
    if (!p || p->lSendLevel < -10000 || p->lSendLevel > 0) return E_INVALIDARG;
    This->level = p->lSendLevel;
    This->gain = send_gain(p->lSendLevel);
    return S_OK;
}
static HRESULT WINAPI sd_GetAllParameters(void *i, RL_DSFXSend *p)
{
    if (!p) return E_INVALIDARG;
    p->lSendLevel = ((rl_send *)i)->level;
    return S_OK;
}
static const struct {
    HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **);
    ULONG (WINAPI *AddRef)(void *);
    ULONG (WINAPI *Release)(void *);
    HRESULT (WINAPI *SetAllParameters)(void *, const RL_DSFXSend *);
    HRESULT (WINAPI *GetAllParameters)(void *, RL_DSFXSend *);
} sd_vtbl = { sd_QueryInterface, sd_AddRef, sd_Release, sd_SetAllParameters, sd_GetAllParameters };

/* 6efda423 / 6efda510: dst = (first send ? 0 : dst) + src * gain / 65535 (rounded toward zero), clamped; a mono
 * send into a stereo mix-in goes to both channels */
void rl_send_mix(int16_t *dst, const int16_t *src, DWORD n, uint16_t gain, BOOL first, BOOL dup)
{
    for (DWORD k = 0; k < n; k++) {
        int32_t t = (int32_t)src[k] * (int32_t)gain;
        int64_t h = (int64_t)t * (int32_t)0x80008001;
        int32_t q = ((int32_t)(h >> 32) + t) >> 15;
        q += (int32_t)((uint32_t)q >> 31);
        for (int c = 0; c < (dup ? 2 : 1); c++) {
            int32_t v = q + (first ? 0 : dst[0]);
            *dst++ = (int16_t)(v > 0x7fff ? 0x7fff : v < -0x8000 ? -0x8000 : v);
        }
    }
}

static void entry_free(fxentry *e)
{
    if (e->send) sd_Release(e->send);
    if (e->ip) IMediaObjectInPlace_Release(e->ip);
    if (e->mo) IMediaObject_Release(e->mo);
    if (e->unk) IUnknown_Release(e->unk);
    memset(e, 0, sizeof(*e));
}

void rl_fxchain_free(rl_fxchain *c)
{
    if (!c) return;
    for (DWORD i = 0; i < c->n; i++) entry_free(&c->e[i]);
    HeapFree(GetProcessHeap(), 0, c);
}

/* the media type DirectSound gives its effects: the buffer's format, one sample of one channel as the size */
static void media_type(const WAVEFORMATEX *wfx, DMO_MEDIA_TYPE *mt)
{
    memset(mt, 0, sizeof(*mt));
    mt->majortype = RL_MEDIATYPE_Audio;
    mt->subtype = RL_MEDIATYPE_Audio;
    mt->subtype.Data1 = wfx->wFormatTag;
    mt->bFixedSizeSamples = TRUE;
    mt->lSampleSize = wfx->wBitsPerSample / 8;
    mt->formattype = RL_FORMAT_WaveFormatEx;
    mt->cbFormat = sizeof(WAVEFORMATEX) + (wfx->wFormatTag == WAVE_FORMAT_PCM ? 0 : wfx->cbSize);
    mt->pbFormat = (BYTE *)wfx;
}

rl_fxchain *rl_fxchain_create(const WAVEFORMATEX *wfx, DWORD count, const DSEFFECTDESC *desc, DWORD *results,
                              BOOL defer, HRESULT *phr)
{
    char g[64];
    HRESULT first = S_OK;
    rl_fxchain *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, offsetof(rl_fxchain, e) + count * sizeof(fxentry));
    if (!c) { *phr = DSERR_OUTOFMEMORY; return NULL; }
    c->n = count;
    c->block = wfx->nBlockAlign;
    WAVEFORMATEX wf = *wfx;
    if (wf.wFormatTag == WAVE_FORMAT_PCM) wf.cbSize = 0;
    DMO_MEDIA_TYPE mt;
    media_type(wfx->wFormatTag == WAVE_FORMAT_PCM ? &wf : wfx, &mt);
    for (DWORD i = 0; i < count; i++) {
        fxentry *e = &c->e[i];
        HRESULT hr;
        e->cls = desc[i].guidDSFXClass;
        /* hardware alone: none (as on Windows 7 and later) */
        if ((desc[i].dwFlags & (DSFX_LOCHARDWARE | DSFX_LOCSOFTWARE)) == DSFX_LOCHARDWARE) {
            hr = E_NOTIMPL;
            e->status = DSFXR_FAILED;
        } else {
            int s = std_index(&e->cls);
            if (s >= 0) {
                rl_fx *f = fx_new(s);
                hr = f ? S_OK : E_OUTOFMEMORY;
                if (f) { e->ours = f; e->unk = (IUnknown *)&f->fx_vtbl; }
            } else hr = CoCreateInstance(&e->cls, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&e->unk);
            if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(e->unk, &RL_IID_IMediaObject, (void **)&e->mo);
            if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(e->unk, &RL_IID_IMediaObjectInPlace, (void **)&e->ip);
            if (SUCCEEDED(hr)) hr = IMediaObject_SetInputType(e->mo, 0, &mt, 0);
            if (SUCCEEDED(hr)) hr = IMediaObject_SetOutputType(e->mo, 0, &mt, 0);
            e->status = SUCCEEDED(hr) ? (defer ? DSFXR_UNALLOCATED : DSFXR_LOCSOFTWARE) : DSFXR_UNKNOWN;
        }
        LOG_CALL("  SetFX %lu: %s%s -> %08lx", (unsigned long)i, rl_guid_str(&e->cls, g), e->ours ? " (ours)" : "", (unsigned long)hr);
        if (FAILED(hr)) {
            if (SUCCEEDED(first)) first = hr;
            entry_free(e);
            e->cls = desc[i].guidDSFXClass;
            e->status = hr == E_NOTIMPL ? DSFXR_FAILED : DSFXR_UNKNOWN;
        }
    }
    if (FAILED(first)) {
        /* the ones that could be made are there but not used */
        for (DWORD i = 0; i < count; i++) if (c->e[i].unk) c->e[i].status = DSFXR_PRESENT;
        if (results) for (DWORD i = 0; i < count; i++) results[i] = c->e[i].status;
        rl_fxchain_free(c);
        *phr = first;
        return NULL;
    }
    if (results) for (DWORD i = 0; i < count; i++) results[i] = c->e[i].status;
    *phr = DS_OK;
    return c;
}

void rl_fxchain_place(rl_fxchain *c, BOOL placed)
{
    if (!c) return;
    for (DWORD i = 0; i < c->n; i++) c->e[i].status = placed ? DSFXR_LOCSOFTWARE : DSFXR_UNALLOCATED;
}

void rl_fxchain_status(const rl_fxchain *c, DWORD *results, DWORD count)
{
    if (!c || !results) return;
    for (DWORD i = 0; i < count && i < c->n; i++) results[i] = c->e[i].status;
}

HRESULT rl_fxchain_get_object(rl_fxchain *c, REFGUID cls, DWORD index, REFIID iid, void **out)
{
    BOOL all = IsEqualGUID(cls, &RL_GUID_All_Objects);
    for (DWORD i = 0; c && i < c->n; i++) {
        if (!all && !IsEqualGUID(cls, &c->e[i].cls)) continue;
        if (index--) continue;
        return IUnknown_QueryInterface(c->e[i].unk, iid, out);
    }
    return DSERR_OBJECTNOTFOUND;
}

void rl_fxchain_discontinuity(rl_fxchain *c)
{
    for (DWORD i = 0; c && i < c->n; i++) if (c->e[i].mo) IMediaObject_Discontinuity(c->e[i].mo, 0);
}

void rl_fxchain_process(rl_fxchain *c, BYTE *data, DWORD bytes)
{
    for (DWORD i = 0; c && i < c->n; i++) {
        if (c->e[i].send) { if (c->ops) c->ops->mix(c->ctx, c->e[i].send->target, data, bytes, c->e[i].send->gain, c->nch); }
        else IMediaObjectInPlace_Process(c->e[i].ip, bytes, data, 0, 0);
    }
}

/* DirectSound's buffer configuration effects (DSBC 'DSFX' forms, 6efdb574): the object made at load (as CoCreateInstance
 * with IID_IMediaObject: ours for the standard classes) */
HRESULT rl_fx_create_config(REFCLSID cls, IUnknown **out)
{
    *out = NULL;
    int s = std_index(cls);
    if (s >= 0) {
        rl_fx *f = fx_new(s);
        if (!f) return E_OUTOFMEMORY;
        *out = (IUnknown *)&f->IMediaObject_iface;
        return S_OK;
    }
    return CoCreateInstance(cls, NULL, CLSCTX_INPROC_SERVER, &RL_IID_IMediaObject, (void **)out);
}

/* a buffer made from a configuration: its effects cloned (IMediaObjectInPlace::Clone) and given the buffer's format;
 * sends found by the target buffer's GUID (DSERR_BADSENDBUFFERGUID if there is none) */
rl_fxchain *rl_fxchain_from_config(const WAVEFORMATEX *wfx, DWORD count, const rl_fxsrc *src, const rl_sendops *ops,
                                   void *ctx, HRESULT *phr)
{
    rl_fxchain *c = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, offsetof(rl_fxchain, e) + (count ? count : 1) * sizeof(fxentry));
    if (!c) { *phr = DSERR_OUTOFMEMORY; return NULL; }
    c->n = count; c->block = wfx->nBlockAlign; c->ops = ops; c->ctx = ctx; c->nch = wfx->nChannels;
    WAVEFORMATEX wf = *wfx;
    wf.cbSize = 0;
    DMO_MEDIA_TYPE mt;
    media_type(&wf, &mt);
    HRESULT hr = S_OK;
    for (DWORD i = 0; i < count && SUCCEEDED(hr); i++) {
        fxentry *e = &c->e[i];
        e->cls = src[i].cls;
        e->status = DSFXR_LOCSOFTWARE;
        if (IsEqualGUID(&src[i].cls, &RL_GUID_DSFX_SEND)) {
            int t = ops ? ops->find(ctx, &src[i].send) : -1;
            if (t < 0) { hr = DSERR_BADSENDBUFFERGUID; break; }
            rl_send *sd = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*sd));
            if (!sd) { hr = E_OUTOFMEMORY; break; }
            sd->vtbl = &sd_vtbl; sd->ref = 1; sd->target = t;
            RL_DSFXSend p = { src[i].level };
            sd_SetAllParameters(sd, &p);
            e->send = sd;
            e->unk = (IUnknown *)sd; IUnknown_AddRef(e->unk);
            continue;
        }
        IMediaObjectInPlace *ip = NULL, *n = NULL;
        hr = IUnknown_QueryInterface(src[i].obj, &RL_IID_IMediaObjectInPlace, (void **)&ip);
        if (SUCCEEDED(hr)) { hr = IMediaObjectInPlace_Clone(ip, &n); IMediaObjectInPlace_Release(ip); }
        if (SUCCEEDED(hr)) {
            e->ip = n;
            void **vt = *(void ***)n;
            if (vt == (void **)&ip_vtbl) e->ours = from_ip(n);
            hr = IMediaObjectInPlace_QueryInterface(n, &IID_IUnknown, (void **)&e->unk);
        }
        if (SUCCEEDED(hr)) hr = IUnknown_QueryInterface(e->unk, &RL_IID_IMediaObject, (void **)&e->mo);
        if (SUCCEEDED(hr)) hr = IMediaObject_SetInputType(e->mo, 0, &mt, 0);
        if (SUCCEEDED(hr)) hr = IMediaObject_SetOutputType(e->mo, 0, &mt, 0);
    }
    if (FAILED(hr)) { rl_fxchain_free(c); *phr = hr; return NULL; }
    *phr = DS_OK;           /* (no Discontinuity: dsound's sink buffers play from the start) */
    return c;
}
