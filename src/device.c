/* reAL - output device: software mixer + shared-mode WASAPI render thread.
 * Same output strategy ALchemy's software path uses (CWASAPIRender), which under Wine goes
 * through mmdevapi -> winepulse/winealsa -> PipeWire/PulseAudio/ALSA. */
#include "real.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <string.h>

DEFINE_GUID(RL_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(RL_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(RL_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(RL_IID_IAudioRenderClient,   0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(RL_SUBTYPE_FLOAT,            0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

static rl_device *s_dev;
static CRITICAL_SECTION s_dev_cs;
static INIT_ONCE s_dev_once = INIT_ONCE_STATIC_INIT;

typedef enum { OUT_F32, OUT_S16, OUT_S32 } out_fmt;

typedef struct wasapi_out {
    IMMDeviceEnumerator *enumr;
    IMMDevice           *mmdev;
    IAudioClient        *client;
    IAudioRenderClient  *render;
    HANDLE               event;
    UINT32               buffer_frames;
    UINT32               channels;
    out_fmt              fmt;
} wasapi_out;

static BOOL CALLBACK init_cs(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_dev_cs);
    return TRUE;
}

/* ---------------------------------------------------------------- play list (lock held) */

void rl_device_start_voice(rl_device *dev, rl_buffer *b)
{
    if (b->in_play_list) return;
    b->prev_playing = NULL;
    b->next_playing = dev->playing;
    if (dev->playing) dev->playing->prev_playing = b;
    dev->playing = b;
    b->in_play_list = TRUE;
}

void rl_device_stop_voice(rl_device *dev, rl_buffer *b)
{
    if (!b->in_play_list) return;
    if (b->prev_playing) b->prev_playing->next_playing = b->next_playing; else dev->playing = b->next_playing;
    if (b->next_playing) b->next_playing->prev_playing = b->prev_playing;
    b->next_playing = b->prev_playing = NULL;
    b->in_play_list = FALSE;
}

void rl_device_commit_3d(rl_device *dev)
{
    BOOL all = dev->listener_dirty;
    if (all) {
        dev->listener = dev->listener_pending;
        dev->listener_dirty = FALSE;
    }
    for (rl_buffer *b = dev->all; b; b = b->next_all) {
        if (b->is_primary || !(b->desc.dwFlags & DSBCAPS_CTRL3D)) continue;
        if (b->src3d_dirty) {
            b->src3d = b->src3d_pending;
            b->src3d_dirty = FALSE;
            rl_buffer_update_gains(b);
        } else if (all) {
            rl_buffer_update_gains(b);
        }
    }
}

/* ---------------------------------------------------------------- mixing */

static void mix(rl_device *dev, UINT32 frames)
{
    if (frames > dev->mixbuf_frames) {
        HeapFree(GetProcessHeap(), 0, dev->mixbuf);
        dev->mixbuf_frames = frames * 2;
        dev->mixbuf = HeapAlloc(GetProcessHeap(), 0, dev->mixbuf_frames * 2 * sizeof(float));
        if (!dev->mixbuf) { dev->mixbuf_frames = 0; return; }
    }
    memset(dev->mixbuf, 0, frames * 2 * sizeof(float));

    EnterCriticalSection(&dev->lock);
    rl_buffer *b = dev->playing;
    while (b) {
        rl_buffer *next = b->next_playing;
        if (rl_voice_mix(&b->voice, dev->mixbuf, frames, dev->out_rate, rl_signal_event))
            rl_device_stop_voice(dev, b);
        else if (!b->voice.playing)
            rl_device_stop_voice(dev, b);
        b = next;
    }
    float master = rl_db100_to_gain(dev->primary_volume);
    LeaveCriticalSection(&dev->lock);

    if (master != 1.0f)
        for (UINT32 i = 0; i < frames * 2; i++) dev->mixbuf[i] *= master;
}

static inline float clampf(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }

static void write_out(const rl_device *dev, const wasapi_out *w, BYTE *dst, UINT32 frames)
{
    const float *src = dev->mixbuf;
    const UINT32 ch = w->channels;
    for (UINT32 i = 0; i < frames; i++) {
        float l = clampf(src[i * 2]), r = clampf(src[i * 2 + 1]);
        for (UINT32 c = 0; c < ch; c++) {
            float v = ch == 1 ? (l + r) * 0.5f : c == 0 ? l : c == 1 ? r : 0.0f;
            switch (w->fmt) {
            case OUT_F32: ((float *)dst)[i * ch + c] = v; break;
            case OUT_S16: ((INT16 *)dst)[i * ch + c] = (INT16)(v * 32767.0f); break;
            case OUT_S32: ((INT32 *)dst)[i * ch + c] = (INT32)(v * 2147483647.0f); break;
            }
        }
    }
}

/* ---------------------------------------------------------------- WASAPI */

static void wasapi_close(wasapi_out *w)
{
    if (w->client) IAudioClient_Stop(w->client);
    if (w->render) IAudioRenderClient_Release(w->render);
    if (w->client) IAudioClient_Release(w->client);
    if (w->mmdev) IMMDevice_Release(w->mmdev);
    if (w->enumr) IMMDeviceEnumerator_Release(w->enumr);
    if (w->event) CloseHandle(w->event);
    memset(w, 0, sizeof(*w));
}

static HRESULT wasapi_open(rl_device *dev, wasapi_out *w)
{
    WAVEFORMATEX *mixfmt = NULL;
    HRESULT hr;
    memset(w, 0, sizeof(*w));

    hr = CoCreateInstance(&RL_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER,
                          &RL_IID_IMMDeviceEnumerator, (void **)&w->enumr);
    if (FAILED(hr)) { LOG_INFO("WASAPI: no MMDeviceEnumerator (%08lx)", (unsigned long)hr); goto fail; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(w->enumr, eRender, eConsole, &w->mmdev);
    if (FAILED(hr)) { LOG_INFO("WASAPI: no default render endpoint (%08lx)", (unsigned long)hr); goto fail; }
    hr = IMMDevice_Activate(w->mmdev, &RL_IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&w->client);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetMixFormat(w->client, &mixfmt);
    if (FAILED(hr)) goto fail;

    w->channels = mixfmt->nChannels;
    BOOL is_float = mixfmt->wFormatTag == 3 ||
        (mixfmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)mixfmt)->SubFormat, &RL_SUBTYPE_FLOAT));
    if (is_float && mixfmt->wBitsPerSample == 32) w->fmt = OUT_F32;
    else if (mixfmt->wBitsPerSample == 16) w->fmt = OUT_S16;
    else if (mixfmt->wBitsPerSample == 32) w->fmt = OUT_S32;
    else { hr = AUDCLNT_E_UNSUPPORTED_FORMAT; goto fail; }

    REFERENCE_TIME dur = (REFERENCE_TIME)g_cfg.latency_ms * 10000;
    hr = IAudioClient_Initialize(w->client, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                 dur, 0, mixfmt, NULL);
    if (FAILED(hr)) { LOG_INFO("WASAPI: Initialize failed (%08lx)", (unsigned long)hr); goto fail; }

    w->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IAudioClient_SetEventHandle(w->client, w->event);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetBufferSize(w->client, &w->buffer_frames);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetService(w->client, &RL_IID_IAudioRenderClient, (void **)&w->render);
    if (FAILED(hr)) goto fail;

    REFERENCE_TIME period = 0;
    IAudioClient_GetDevicePeriod(w->client, &period, NULL);

    dev->out_rate = mixfmt->nSamplesPerSec;
    dev->out_channels = w->channels;
    dev->period_frames = period ? (UINT32)(period * dev->out_rate / 10000000) : dev->out_rate / 100;

    /* pre-fill with silence so the first period doesn't underrun */
    BYTE *p;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(w->render, w->buffer_frames, &p)))
        IAudioRenderClient_ReleaseBuffer(w->render, w->buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);

    hr = IAudioClient_Start(w->client);
    if (FAILED(hr)) goto fail;

    LOG_INFO("WASAPI: %lu Hz, %lu ch, %s, buffer %lu frames, period %lu frames",
             (unsigned long)dev->out_rate, (unsigned long)w->channels,
             w->fmt == OUT_F32 ? "float32" : w->fmt == OUT_S16 ? "s16" : "s32",
             (unsigned long)w->buffer_frames, (unsigned long)dev->period_frames);
    CoTaskMemFree(mixfmt);
    return S_OK;

fail:
    if (mixfmt) CoTaskMemFree(mixfmt);
    wasapi_close(w);
    return FAILED(hr) ? hr : E_FAIL;
}

typedef struct { rl_device *dev; HANDLE ready; HRESULT hr; } thread_args;

static DWORD WINAPI render_thread(LPVOID param)
{
    thread_args *a = param;
    rl_device *dev = a->dev;
    wasapi_out w;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    HRESULT open_hr = wasapi_open(dev, &w);
    a->hr = open_hr;
    SetEvent(a->ready);          /* `a` is dead after this */
    if (FAILED(open_hr)) {
        CoUninitialize();
        return 1;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    while (dev->running) {
        HANDLE hs[2] = { dev->quit_event, w.event };
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, 200);
        if (r == WAIT_OBJECT_0) break;

        UINT32 padding = 0;
        HRESULT hr = IAudioClient_GetCurrentPadding(w.client, &padding);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
            LOG_INFO("WASAPI: device invalidated, reopening");
            wasapi_close(&w);
            while (dev->running && FAILED(wasapi_open(dev, &w)))
                if (WaitForSingleObject(dev->quit_event, 500) == WAIT_OBJECT_0) break;
            continue;
        }
        if (FAILED(hr)) continue;

        UINT32 avail = w.buffer_frames - padding;
        if (!avail) continue;
        BYTE *dst;
        if (FAILED(IAudioRenderClient_GetBuffer(w.render, avail, &dst))) continue;
        mix(dev, avail);
        if (dev->mixbuf) {
            write_out(dev, &w, dst, avail);
            IAudioRenderClient_ReleaseBuffer(w.render, avail, 0);
        } else {
            IAudioRenderClient_ReleaseBuffer(w.render, avail, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    wasapi_close(&w);
    CoUninitialize();
    return 0;
}

/* ---------------------------------------------------------------- lifetime */

void rl_device_addref(rl_device *dev)
{
    InterlockedIncrement(&dev->ref);
}

HRESULT rl_device_acquire(rl_device **out)
{
    InitOnceExecuteOnce(&s_dev_once, init_cs, NULL, NULL);
    EnterCriticalSection(&s_dev_cs);
    if (s_dev) {
        rl_device_addref(s_dev);
        *out = s_dev;
        LeaveCriticalSection(&s_dev_cs);
        return DS_OK;
    }

    rl_device *dev = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*dev));
    if (!dev) { LeaveCriticalSection(&s_dev_cs); return DSERR_OUTOFMEMORY; }
    InitializeCriticalSection(&dev->lock);
    dev->ref = 1;
    rl_listener_defaults(&dev->listener);
    dev->listener_pending = dev->listener;
    dev->primary_fmt.wFormatTag = WAVE_FORMAT_PCM;
    dev->primary_fmt.nChannels = 2;
    dev->primary_fmt.nSamplesPerSec = 22050;
    dev->primary_fmt.wBitsPerSample = 8;
    dev->primary_fmt.nBlockAlign = 2;
    dev->primary_fmt.nAvgBytesPerSec = 44100;
    dev->speaker_config = DSSPEAKER_COMBINED(DSSPEAKER_STEREO, DSSPEAKER_GEOMETRY_WIDE);
    dev->quit_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    dev->running = 1;

    thread_args a = { dev, CreateEventW(NULL, TRUE, FALSE, NULL), E_FAIL };
    dev->thread = CreateThread(NULL, 0, render_thread, &a, 0, NULL);
    if (dev->thread) WaitForSingleObject(a.ready, INFINITE);
    CloseHandle(a.ready);

    if (!dev->thread || FAILED(a.hr)) {
        LOG_INFO("device init failed (%08lx)", (unsigned long)a.hr);
        dev->running = 0;
        SetEvent(dev->quit_event);
        if (dev->thread) { WaitForSingleObject(dev->thread, INFINITE); CloseHandle(dev->thread); }
        CloseHandle(dev->quit_event);
        DeleteCriticalSection(&dev->lock);
        HeapFree(GetProcessHeap(), 0, dev);
        LeaveCriticalSection(&s_dev_cs);
        return DSERR_NODRIVER;
    }

    s_dev = dev;
    *out = dev;
    LeaveCriticalSection(&s_dev_cs);
    return DS_OK;
}

void rl_device_release(rl_device *dev)
{
    EnterCriticalSection(&s_dev_cs);
    if (InterlockedDecrement(&dev->ref)) { LeaveCriticalSection(&s_dev_cs); return; }
    s_dev = NULL;
    LeaveCriticalSection(&s_dev_cs);

    LOG_INFO("device shutting down");
    dev->running = 0;
    SetEvent(dev->quit_event);
    WaitForSingleObject(dev->thread, INFINITE);
    CloseHandle(dev->thread);
    CloseHandle(dev->quit_event);
    DeleteCriticalSection(&dev->lock);
    HeapFree(GetProcessHeap(), 0, dev->mixbuf);
    HeapFree(GetProcessHeap(), 0, dev);
}
