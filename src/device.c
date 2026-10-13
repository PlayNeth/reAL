/* reAL - output device: software mixer + shared-mode WASAPI render thread.
 * Same output strategy ALchemy's software path uses (CWASAPIRender), which under Wine goes
 * through mmdevapi -> winepulse/winealsa -> PipeWire/PulseAudio/ALSA. */
#include "real.h"
#include "eax4dsp.h"
#include <math.h>
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

static void update_all_gains(rl_device *dev)
{
    for (rl_buffer *b = dev->all; b; b = b->next_all)
        if (!b->is_primary) rl_buffer_update_gains(b);
}

/* The first EAX call of a version loads the reverb (ALchemy fn 0x10042220) and, when the game switches
 * versions, that version's listener properties start from their defaults (fns 0x1004bb40 / 0x100456b0). */
void rl_device_eax_use(rl_device *dev, int version)
{
    BOOL first = !dev->eax_used;
    dev->eax_used = TRUE;
    if (dev->eax_version == version) return;
    LOG_INFO("EAX %d.0 in use%s", version, first ? ": reverb enabled" : "");
    dev->eax_version = version;
    if (version == 1) {
        dev->eax1_env = 0;
        dev->eax1_volume = rl_eax1_presets[0].volume;
        dev->eax1_decay = rl_eax1_presets[0].decay;
        dev->eax1_damping = rl_eax1_presets[0].damping;
    } else if (version == 2) {
        eax2l_reset(&dev->eax2l);
    } else if (version == 3) {
        eax3l_reset(&dev->eax3l);
    } else if (!dev->eax4_init) {
        /* EAX 4: context defaults; slot 0 reverb and slot 1 chorus, both locked (fn 0x10042220(4)) */
        dev->eax4_init = TRUE;
        eax4_context_reset(&dev->eax4c);
        eax4_slots_init(dev->eax4s);
    }
    dev->reverb_dirty = TRUE;
    update_all_gains(dev);
}

void rl_device_eax1_changed(rl_device *dev)
{
    dev->reverb_dirty = TRUE;
}

void rl_device_eax_listener_changed(rl_device *dev)
{
    dev->reverb_dirty = TRUE;
}

static float slot_gain(int32_t mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }

/* EAX 4: every slot holding a reverb gets its own engine (slot 0 uses dev->reverb) */
static void apply_eax4(rl_device *dev)
{
    for (int k = 0; k < 4; k++) {
        eax4_slot *s = &dev->eax4s[k];
        dev->fx_gain[k] = slot_gain(s->volume);
        if (s->type != EAX4FX_REVERB) {
            /* any other effect: (re)create its unit when the type or the output rate changed */
            eax4fx_unit *u = dev->fxunit[k];
            uint32_t rate = dev->out_rate ? dev->out_rate : 48000;
            if (u && (eax4fx_unit_type(u) != s->type || eax4fx_unit_rate(u) != rate)) {
                eax4fx_unit_free(u);
                dev->fxunit[k] = u = NULL;
            }
            if (!u && s->type != EAX4FX_NONE) {
                dev->fxunit[k] = u = eax4fx_unit_new(s->type, rate);
                if (u) LOG_EAX("fx: EAX4 slot %d loads effect type %d", k, s->type);
            }
            if (u) eax4fx_unit_props(u, s->fx);
            continue;
        }
        if (dev->fxunit[k]) { eax4fx_unit_free(dev->fxunit[k]); dev->fxunit[k] = NULL; }
        rl_reverb *rv = &dev->reverb;
        alrv_props *props = &dev->eax_props;
        if (k) {
            if (!dev->xreverb[k - 1]) {
                rl_reverb *n = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*n));
                if (!n || !rl_reverb_init(n, dev->out_rate ? dev->out_rate : 48000)) { HeapFree(GetProcessHeap(), 0, n); continue; }
                dev->xreverb[k - 1] = n;
                alrv_props_default(&dev->xprops[k - 1]);
                s->reverb.dirty = 0xffffffff;        /* full conversion into the new engine */
            }
            rv = dev->xreverb[k - 1];
            props = &dev->xprops[k - 1];
        }
        if (!eax3l_apply(&s->reverb, props)) continue;
        rl_reverb_set_props(rv, props);
        LOG_EAX("reverb: EAX4 slot %d env %lu room %ld decay %.2f hf ratio %.2f volume %ld%s", k,
                (unsigned long)s->reverb.p.environment, (long)s->reverb.p.room, s->reverb.p.decay_time,
                s->reverb.p.decay_hf_ratio, (long)s->volume, (s->flags & EAX4SLOT_ENVIRONMENT) ? " (environment)" : "");
    }
    update_all_gains(dev);      /* the sources' room / HF gains depend on the slots' reverb */
}

static void apply_reverb(rl_device *dev)
{
    if (dev->eax_version == 4) { apply_eax4(dev); return; }
    if (dev->eax_version == 2 || dev->eax_version == 3) {
        int changed = dev->eax_version == 2 ? eax2l_apply(&dev->eax2l, &dev->eax_props)
                                            : eax3l_apply(&dev->eax3l, &dev->eax_props);
        if (!changed) return;
        rl_reverb_set_props(&dev->reverb, &dev->eax_props);
        update_all_gains(dev);      /* the sources' room / HF gains depend on decay, air absorption, ... */
        if (dev->eax_version == 2)
            LOG_EAX("reverb: EAX2 env %lu room %ld decay %.2f hf ratio %.2f -> fdn %d/%d/%d/%d refl %d late %d",
                    (unsigned long)dev->eax2l.p.environment, (long)dev->eax2l.p.room, dev->eax2l.p.decay_time,
                    dev->eax2l.p.decay_hf_ratio, dev->reverb.target.fdn_delay[0], dev->reverb.target.fdn_delay[1],
                    dev->reverb.target.fdn_delay[2], dev->reverb.target.fdn_delay[3], dev->reverb.target.refl_delay,
                    dev->reverb.target.late_tap_delay[0]);
        else
            LOG_EAX("reverb: EAX3 env %lu room %ld decay %.2f hf ratio %.2f -> fdn %d/%d/%d/%d refl %d late %d",
                    (unsigned long)dev->eax3l.p.environment, (long)dev->eax3l.p.room, dev->eax3l.p.decay_time,
                    dev->eax3l.p.decay_hf_ratio, dev->reverb.target.fdn_delay[0], dev->reverb.target.fdn_delay[1],
                    dev->reverb.target.fdn_delay[2], dev->reverb.target.fdn_delay[3], dev->reverb.target.refl_delay,
                    dev->reverb.target.late_tap_delay[0]);
        return;
    }
    rl_reverb_set_eax1(&dev->reverb, dev->eax1_env, dev->eax1_volume, dev->eax1_decay, dev->eax1_damping);
    LOG_EAX("reverb: env %lu volume %.3f decay %.2f damping %.3f -> fdn %d/%d/%d/%d refl %d late %d",
            (unsigned long)dev->eax1_env, dev->eax1_volume, dev->eax1_decay, dev->eax1_damping,
            dev->reverb.target.fdn_delay[0], dev->reverb.target.fdn_delay[1], dev->reverb.target.fdn_delay[2],
            dev->reverb.target.fdn_delay[3], dev->reverb.target.refl_delay, dev->reverb.target.late_tap_delay[0]);
}

/* ---------------------------------------------------------------- mixing */

static void mix(rl_device *dev, UINT32 frames)
{
    if (frames > dev->mixbuf_frames) {
        HeapFree(GetProcessHeap(), 0, dev->mixbuf);
        HeapFree(GetProcessHeap(), 0, dev->sendbuf);
        dev->mixbuf_frames = frames * 2;
        dev->mixbuf = HeapAlloc(GetProcessHeap(), 0, dev->mixbuf_frames * 2 * sizeof(float));
        dev->sendbuf = HeapAlloc(GetProcessHeap(), 0, RL_SENDS * dev->mixbuf_frames * sizeof(float));
        if (!dev->mixbuf || !dev->sendbuf) {
            HeapFree(GetProcessHeap(), 0, dev->mixbuf);
            HeapFree(GetProcessHeap(), 0, dev->sendbuf);
            dev->mixbuf = dev->sendbuf = NULL;
            dev->mixbuf_frames = 0;
            return;
        }
    }
    memset(dev->mixbuf, 0, frames * 2 * sizeof(float));

    EnterCriticalSection(&dev->lock);
    if (dev->reverb_dirty) {
        dev->reverb_dirty = FALSE;
        apply_reverb(dev);
    }
    if (dev->eax_commit) {
        dev->eax_commit = FALSE;
        update_all_gains(dev);
    }
    /* the effect buses this period: slot 0 for EAX 1-3, every slot running an effect for EAX 4 */
    float *bus[RL_SENDS] = { NULL };
    rl_reverb *fxr[RL_SENDS] = { NULL };
    eax4fx_unit *fxu[RL_SENDS] = { NULL };
    float fxg[RL_SENDS] = { 1.0f, 1.0f, 1.0f, 1.0f };
    int nbus = 0;
    if (dev->eax_used && dev->reverb.eng[0].mem) {
        if (dev->eax_version == 4) {
            for (int k = 0; k < 4; k++) {
                if (dev->eax4s[k].type != EAX4FX_REVERB) {
                    if (!dev->fxunit[k]) continue;
                    bus[k] = dev->sendbuf + (size_t)k * dev->mixbuf_frames;
                    fxu[k] = dev->fxunit[k]; fxg[k] = dev->fx_gain[k]; nbus++;
                    continue;
                }
                rl_reverb *rv = k ? dev->xreverb[k - 1] : &dev->reverb;
                if (!rv || !rv->eng[0].mem) continue;
                bus[k] = dev->sendbuf + (size_t)k * dev->mixbuf_frames;
                fxr[k] = rv; fxg[k] = dev->fx_gain[k]; nbus++;
            }
        } else {
            bus[0] = dev->sendbuf; fxr[0] = &dev->reverb; nbus = 1;
        }
    }
    for (int k = 0; k < RL_SENDS; k++) if (bus[k]) memset(bus[k], 0, frames * sizeof(float));
    rl_buffer *b = dev->playing;
    while (b) {
        rl_buffer *next = b->next_playing;
        if (b->fx) rl_buffer_fx_run(b, frames, dev->out_rate);
        if (rl_voice_mix(&b->voice, dev->mixbuf, nbus ? bus : NULL, frames, dev->out_rate, rl_signal_event))
            rl_device_stop_voice(dev, b);
        else if (!b->voice.playing)
            rl_device_stop_voice(dev, b);
        b = next;
    }
    float master = rl_db100_to_gain(dev->primary_volume);
    LeaveCriticalSection(&dev->lock);

    for (int k = 0; k < RL_SENDS; k++) {
        if (!bus[k]) continue;
        if (fxu[k]) { eax4fx_unit_process(fxu[k], bus[k], dev->mixbuf, fxg[k], frames); continue; }  /* volume on the output */
        if (fxg[k] != 1.0f) for (UINT32 i = 0; i < frames; i++) bus[k][i] *= fxg[k];   /* slot volume */
        rl_reverb_process(fxr[k], bus[k], dev->mixbuf, frames);
    }

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

/* ---------------------------------------------------------------- HRTF data */

static BOOL read_file(const char *path, BYTE **data, DWORD *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD sz = GetFileSize(h, NULL), got = 0;
    BYTE *p = (sz && sz < 64 * 1024 * 1024) ? HeapAlloc(GetProcessHeap(), 0, sz) : NULL;
    BOOL ok = p && ReadFile(h, p, sz, &got, NULL) && got == sz;
    CloseHandle(h);
    if (!ok) { HeapFree(GetProcessHeap(), 0, p); return FALSE; }
    *data = p; *size = sz;
    return TRUE;
}

static void load_hrtf(rl_device *dev, UINT32 rate)
{
    char cand[3][MAX_PATH + 32];
    int n = 0;
    if (g_cfg.hrtf_path[0]) lstrcpynA(cand[n++], g_cfg.hrtf_path, MAX_PATH);
    else {
        GetModuleFileNameA(g_self, cand[n], MAX_PATH);
        char *s = strrchr(cand[n], '\\'); if (s) s[1] = 0; else cand[n][0] = 0;
        lstrcatA(cand[n++], "reAL_hrtf.bin");
        if (GetWindowsDirectoryA(cand[n], MAX_PATH)) { lstrcatA(cand[n++], "\\reAL_hrtf.bin"); }
    }
    for (int i = 0; i < n; i++) {
        BYTE *data; DWORD size;
        if (!read_file(cand[i], &data, &size)) continue;
        rl_hrtf_set set;
        int ok = rl_hrtf_load(&set, data, size, rate);
        HeapFree(GetProcessHeap(), 0, data);
        if (!ok) { LOG_INFO("HRTF: %s is not a valid reAL_hrtf.bin", cand[i]); continue; }
        EnterCriticalSection(&dev->lock);
        rl_hrtf_free(&dev->hrtf);
        dev->hrtf = set;
        for (rl_buffer *b = dev->all; b; b = b->next_all)
            if (!b->is_primary) rl_buffer_update_gains(b);
        LeaveCriticalSection(&dev->lock);
        LOG_INFO("HRTF: %s (%lu directions, %lu taps, %lu Hz)", cand[i], (unsigned long)set.n_dirs,
                 (unsigned long)set.taps, (unsigned long)set.rate);
        return;
    }
    LOG_INFO("HRTF: no reAL_hrtf.bin found, using stereo panning for 3D sounds");
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

    if (dev->out_rate != mixfmt->nSamplesPerSec || !dev->reverb.eng[0].mem) {
        EnterCriticalSection(&dev->lock);
        rl_reverb_free(&dev->reverb);
        rl_reverb_init(&dev->reverb, mixfmt->nSamplesPerSec);
        for (int k = 0; k < 3; k++)          /* EAX 4 slot reverbs are recreated at the new rate */
            if (dev->xreverb[k]) { rl_reverb_free(dev->xreverb[k]); HeapFree(GetProcessHeap(), 0, dev->xreverb[k]); dev->xreverb[k] = NULL; }
        dev->eax4s[0].reverb.dirty = 0xffffffff;
        dev->reverb_dirty = TRUE;
        LeaveCriticalSection(&dev->lock);
    }
    if (g_cfg.hrtf_mode && (dev->hrtf.rate != mixfmt->nSamplesPerSec || !dev->hrtf.dirs))
        load_hrtf(dev, mixfmt->nSamplesPerSec);
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
    dev->eax1_env = 0;  /* EAX_ENVIRONMENT_GENERIC */
    dev->eax1_volume = rl_eax1_presets[0].volume;
    dev->eax1_decay = rl_eax1_presets[0].decay;
    dev->eax1_damping = rl_eax1_presets[0].damping;
    eax2l_reset(&dev->eax2l);
    eax3l_reset(&dev->eax3l);
    alrv_props_default(&dev->eax_props);
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
        rl_reverb_free(&dev->reverb);
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
    HeapFree(GetProcessHeap(), 0, dev->sendbuf);
    rl_reverb_free(&dev->reverb);
    for (int k = 0; k < 3; k++) if (dev->xreverb[k]) { rl_reverb_free(dev->xreverb[k]); HeapFree(GetProcessHeap(), 0, dev->xreverb[k]); }
    for (int k = 0; k < 4; k++) eax4fx_unit_free(dev->fxunit[k]);
    rl_hrtf_free(&dev->hrtf);
    HeapFree(GetProcessHeap(), 0, dev);
}
