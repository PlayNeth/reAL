/* eaxtest - EAX 2.0 / 3.0 smoke test for a DirectSound implementation (reAL or ALchemy).
 *
 * Plays one click per slot through 3D buffers with different EAX settings, the way Painkiller drives
 * EAX (listener environment + read-backs, per-sound room / room HF, obstruction, a DS3DMODE_DISABLE
 * voice), while recording the output with WASAPI loopback.  tools/eaxtest_analyze.py measures the
 * direct click and the reverb tail of each slot.
 *
 * Usage: eaxtest.exe [2|3] [dsound.dll] [fpe]   writes eaxtest.txt and eaxtest.wav
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <float.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#define RATE 48000
#define SLOT_MS 1500
#define LEAD 4800                 /* click 100 ms into the sound */
#define BUF_SAMPLES (RATE * 3 / 10)

static FILE *g_out;
static LARGE_INTEGER g_freq, g_t0;

static double now_ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void P(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    va_start(ap, fmt); vfprintf(g_out, fmt, ap); va_end(ap);
    fflush(g_out); fflush(stdout);
}

static const GUID EAX1_L = { 0x4a4e6fc1, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID EAX1_B = { 0x4a4e6fc0, 0xc341, 0x11d1, { 0xb7, 0x3a, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID EAX2_L = { 0x0306a6a8, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } };
static const GUID EAX2_B = { 0x0306a6a7, 0xb224, 0x11d2, { 0x99, 0xe5, 0x00, 0x00, 0xe8, 0xd8, 0xc7, 0x22 } };
static const GUID EAX3_L = { 0xa8fa6882, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } };
static const GUID EAX3_B = { 0xa8fa6881, 0xb476, 0x11d3, { 0xbd, 0xb9, 0x00, 0xc0, 0xf0, 0x2d, 0xdf, 0x87 } };
static const GUID EAX4_C = { 0x1d4870ad, 0x0def, 0x43c0, { 0xa4, 0x0c, 0x52, 0x36, 0x32, 0x29, 0x63, 0x42 } };
static const GUID EAX4_S = { 0x1b86b823, 0x22df, 0x4eae, { 0x8b, 0x3c, 0x12, 0x78, 0xce, 0x54, 0x42, 0x27 } };
static const GUID EAX4_SLOT[4] = {
    { 0xc4d79f1e, 0xf1ac, 0x436b, { 0xa8, 0x1d, 0xa7, 0x38, 0xe7, 0x04, 0x54, 0x69 } },
    { 0x08c00e96, 0x74be, 0x4491, { 0x93, 0xaa, 0xe8, 0xad, 0x35, 0xa4, 0x91, 0x17 } },
    { 0x1d433b88, 0xf0f6, 0x4637, { 0x91, 0x9f, 0x60, 0xe7, 0xe0, 0x6b, 0x5e, 0xdd } },
    { 0xefff08ea, 0xc7d8, 0x44ab, { 0x93, 0xad, 0x6d, 0xbd, 0x5f, 0x91, 0x00, 0x64 } } };
static const GUID EAX4_PRIMARY = { 0xf317866d, 0x924c, 0x450c, { 0x86, 0x1b, 0xe6, 0xda, 0xa2, 0x5e, 0x7c, 0x20 } };
static const GUID EAX_REVERB_EFFECT = { 0x0cf95c8f, 0xa3cc, 0x4849, { 0xb0, 0xb6, 0x83, 0x2e, 0xcc, 0x18, 0x22, 0xdf } };
static const GUID EAX_ECHO_EFFECT = { 0x0e9f1bc0, 0xac82, 0x11d2, { 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1 } };
static const GUID EAX_DISTORTION_EFFECT = { 0x975a4ce0, 0xac7e, 0x11d2, { 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1 } };
static const GUID EAX_PITCH_EFFECT = { 0xe7905100, 0xafb2, 0x11d2, { 0x88, 0xdd, 0x00, 0xa0, 0x24, 0xd1, 0x3c, 0xe1 } };
static const GUID EAX5_C = { 0x57e13437, 0xb932, 0x4ab2, { 0xb8, 0xbd, 0x52, 0x66, 0xc1, 0xa8, 0x87, 0xee } };
static const GUID EAX5_S = { 0x5edf82f0, 0x24a7, 0x4f38, { 0x8e, 0x64, 0x2f, 0x09, 0xca, 0x05, 0xde, 0xe1 } };

/* ------------------------------------------------------------- loopback recorder */
static const GUID R_CLSID_MMDeviceEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID R_IID_IMMDeviceEnumerator  = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID R_IID_IAudioClient         = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const GUID R_IID_IAudioCaptureClient  = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };

static volatile LONG g_rec_stop, g_rec_ok;
static HANDLE g_rec_ready;

static DWORD WINAPI rec_thread(LPVOID arg)
{
    const char *path = arg;
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL; IAudioCaptureClient *cc = NULL;
    WAVEFORMATEX *wf = NULL; FILE *f = NULL;
    HRESULT hr;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&R_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &R_IID_IMMDeviceEnumerator, (void **)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &R_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetMixFormat(ac, &wf);
    if (SUCCEEDED(hr)) hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, wf, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(ac, &R_IID_IAudioCaptureClient, (void **)&cc);
    if (SUCCEEDED(hr)) f = fopen(path, "wb");
    if (FAILED(hr) || !f) {
        P("loopback recording unavailable (%08lx)%s\n", hr, SUCCEEDED(hr) && !f ? " - cannot create the wav file" : "");
        SetEvent(g_rec_ready);
        return 1;
    }
    DWORD fmtlen = sizeof(WAVEFORMATEX) + wf->cbSize, zero = 0;
    fwrite("RIFF", 1, 4, f); fwrite(&zero, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtlen, 4, 1, f); fwrite(wf, 1, fmtlen, f);
    fwrite("data", 1, 4, f); long data_at = ftell(f); fwrite(&zero, 4, 1, f);
    P("recording %s: %lu Hz, %u ch, %u bit, tag %x\n", path, wf->nSamplesPerSec, wf->nChannels, wf->wBitsPerSample, wf->wFormatTag);
    IAudioClient_Start(ac);
    InterlockedExchange(&g_rec_ok, 1);
    SetEvent(g_rec_ready);

    DWORD total = 0;
    BYTE *silence = calloc(1, wf->nBlockAlign * 48000);
    while (!g_rec_stop) {
        Sleep(10);
        UINT32 n = 0;
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(cc, &n)) && n) {
            BYTE *data; UINT32 frames; DWORD flags;
            if (FAILED(IAudioCaptureClient_GetBuffer(cc, &data, &frames, &flags, NULL, NULL))) break;
            DWORD bytes = frames * wf->nBlockAlign;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) && frames <= 48000) fwrite(silence, 1, bytes, f);
            else fwrite(data, 1, bytes, f);
            total += bytes;
            IAudioCaptureClient_ReleaseBuffer(cc, frames);
        }
    }
    IAudioClient_Stop(ac);
    DWORD riff = 4 + 8 + fmtlen + 8 + total;
    fseek(f, data_at, SEEK_SET); fwrite(&total, 4, 1, f);
    fseek(f, 4, SEEK_SET); fwrite(&riff, 4, 1, f);
    fclose(f);
    P("recorded %lu bytes\n", total);
    free(silence);
    CoTaskMemFree(wf);
    IAudioCaptureClient_Release(cc); IAudioClient_Release(ac); IMMDevice_Release(dev); IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return 0;
}

static IDirectSoundBuffer *make_click_buffer(IDirectSound *ds, DWORD flags)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, RATE, RATE * 2, 2, 16, 0 };
    DSBUFFERDESC bd = { sizeof(bd), flags, BUF_SAMPLES * 2, 0, &wf };
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound_CreateSoundBuffer(ds, &bd, &b, NULL);
    if (FAILED(hr)) { P("CreateSoundBuffer(flags %lx) failed %08lx\n", flags, hr); return NULL; }
    void *p1, *p2; DWORD b1, b2;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER))) {
        memset(p1, 0, b1);
        ((INT16 *)p1)[LEAD] = 32000;
        IDirectSoundBuffer_Unlock(b, p1, b1, p2, b2);
    }
    return b;
}


static IKsPropertySet *g_ks;
static const GUID *g_lst, *g_buf;
static int g_ver;

static HRESULT kset(IKsPropertySet *ks, const GUID *set, ULONG id, void *v, ULONG len)
{
    HRESULT hr = IKsPropertySet_Set(ks, set, id, NULL, 0, v, len);
    if (FAILED(hr)) P("  Set id %lu FAILED %08lx\n", id, hr);
    return hr;
}
static void seti(IKsPropertySet *ks, const GUID *set, ULONG id, LONG v) { kset(ks, set, id, &v, 4); }

static void slot(IDirectSoundBuffer *b, const char *label)
{
    IDirectSoundBuffer_Stop(b);
    IDirectSoundBuffer_SetCurrentPosition(b, 0);
    double t = now_ms();
    HRESULT hr = IDirectSoundBuffer_Play(b, 0, 0, 0);
    P("SLOT t=%.1f %s%s\n", t + 100.0, label, FAILED(hr) ? " PLAYFAILED" : "");
    Sleep(SLOT_MS);
}

int main(int argc, char **argv)
{
    g_ver = argc > 1 ? atoi(argv[1]) : 3;
    if (argc > 3 && !strcmp(argv[3], "fpe")) {
        /* behave like a game that unmasks FPU exceptions and runs the x87 at 24 bits (Direct3D) */
        _controlfp(_PC_24, _MCW_PC);
        _controlfp(~(_EM_INVALID | _EM_ZERODIVIDE | _EM_OVERFLOW) & _MCW_EM, _MCW_EM);
    }
    const char *dll = argc > 2 ? argv[2] : "dsound.dll";
    g_out = fopen("eaxtest.txt", "w");
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
    HMODULE h = LoadLibraryA(dll);
    char path[MAX_PATH] = "?"; if (h) GetModuleFileNameA(h, path, MAX_PATH);
    P("eaxtest EAX %d  loaded=%s\n", g_ver, path);
    if (!h) return 1;
    HRESULT (WINAPI *create)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN) = (void *)GetProcAddress(h, "DirectSoundCreate");
    IDirectSound *ds = NULL;
    HRESULT hr = create(NULL, &ds, NULL);
    P("DirectSoundCreate -> %08lx\n", hr); if (FAILED(hr)) return 1;
    IDirectSound_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D };
    IDirectSoundBuffer *prim = NULL;
    IDirectSound_CreateSoundBuffer(ds, &pd, &prim, NULL);
    WAVEFORMATEX pf = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    if (prim) IDirectSoundBuffer_SetFormat(prim, &pf);
    IDirectSound3DListener *lst = NULL;
    if (prim) IDirectSoundBuffer_QueryInterface(prim, &IID_IDirectSound3DListener, (void **)&lst);

    const DWORD fl = DSBCAPS_CTRL3D | DSBCAPS_LOCHARDWARE | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME |
                     DSBCAPS_CTRLFREQUENCY | DSBCAPS_MUTE3DATMAXDISTANCE;
    IDirectSoundBuffer *b = make_click_buffer(ds, fl), *b2 = make_click_buffer(ds, fl);
    if (!b) b = make_click_buffer(ds, fl & ~DSBCAPS_LOCHARDWARE);
    if (!b2) b2 = make_click_buffer(ds, fl & ~DSBCAPS_LOCHARDWARE);
    if (!b || !b2) return 1;
    IDirectSound3DBuffer *b3d = NULL, *b23d = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3d);
    IDirectSoundBuffer_QueryInterface(b2, &IID_IDirectSound3DBuffer, (void **)&b23d);
    IKsPropertySet *ks = NULL, *ks2 = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IKsPropertySet, (void **)&ks);
    IDirectSoundBuffer_QueryInterface(b2, &IID_IKsPropertySet, (void **)&ks2);
    if (!ks || !ks2) { P("no IKsPropertySet\n"); return 1; }
    /* EAX 4: the "listener" is FX slot 0's reverb (same property ids as the EAX 3 listener), the buffer
     * properties are the EAX 4 source's (same ids as EAX 3 for these) */
    g_lst = g_ver == 2 ? &EAX2_L : g_ver == 4 ? &EAX4_SLOT[0] : &EAX3_L;
    g_buf = g_ver == 2 ? &EAX2_B : g_ver == 4 ? &EAX4_S : &EAX3_B;
    /* property ids differ between the versions */
    const ULONG L_ENV = g_ver == 2 ? 11 : 2, L_ROOM = g_ver == 2 ? 2 : 5, L_DECAY = g_ver == 2 ? 5 : 8,
                L_HFR = g_ver == 2 ? 6 : 9, L_RDEL = g_ver == 2 ? 10 : 15;
    const ULONG B_ROOM = g_ver == 2 ? 4 : 7, B_ROOMHF = g_ver == 2 ? 5 : 8, B_OBST = g_ver == 2 ? 7 : 9,
                B_FLAGS = g_ver == 2 ? 14 : 22;

    ULONG sup = 0;
    hr = IKsPropertySet_QuerySupport(ks, g_lst, 1, &sup);
    P("QuerySupport listener -> %08lx support %lu\n", hr, sup);
    hr = IKsPropertySet_QuerySupport(ks, g_buf, 1, &sup);
    P("QuerySupport buffer -> %08lx support %lu\n", hr, sup);

    /* listener: hangar, then the read-backs Painkiller does */
    seti(ks, g_lst, L_ENV, 10);
    LONG room = 0; float decay = 0, hfr = 0, rdel = 0; ULONG got;
    IKsPropertySet_Get(ks, g_lst, L_ROOM, NULL, 0, &room, 4, &got);
    IKsPropertySet_Get(ks, g_lst, L_DECAY, NULL, 0, &decay, 4, &got);
    IKsPropertySet_Get(ks, g_lst, L_HFR, NULL, 0, &hfr, 4, &got);
    IKsPropertySet_Get(ks, g_lst, L_RDEL, NULL, 0, &rdel, 4, &got);
    P("listener env 10: room %ld decay %.3f hfratio %.3f reverb delay %.4f\n", room, decay, hfr, rdel);
    LONG flags = 0;
    IKsPropertySet_Get(ks, g_buf, B_FLAGS, NULL, 0, &flags, 4, &got);
    P("buffer flags %ld\n", flags);

    if (lst) {
        IDirectSound3DListener_SetPosition(lst, 0, 0, 0, DS3D_IMMEDIATE);
        IDirectSound3DListener_SetOrientation(lst, 0, 0, 1, 0, 1, 0, DS3D_IMMEDIATE);
    }
    IDirectSound3DBuffer_SetPosition(b3d, 0, 0, 2, DS3D_IMMEDIATE);
    IDirectSound3DBuffer_SetPosition(b23d, 0, 0, 2, DS3D_IMMEDIATE);

    g_rec_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE rec = CreateThread(NULL, 0, rec_thread, "eaxtest.wav", 0, NULL);
    WaitForSingleObject(g_rec_ready, 10000);
    Sleep(1000);
    seti(ks, g_lst, L_ENV, 2);          /* short decay (0.4 s) so the slots do not overlap */
    P("BEGIN\n");
    slot(b, "warm-up");
    slot(b, "room 0");
    seti(ks, g_buf, B_ROOM, -1000);
    slot(b, "room -1000");
    seti(ks, g_buf, B_ROOM, -10000);
    slot(b, "room -10000");
    seti(ks, g_buf, B_ROOM, 0); seti(ks, g_buf, B_ROOMHF, -3000);
    slot(b, "roomhf -3000");
    seti(ks, g_buf, B_ROOMHF, 0); seti(ks, g_buf, B_OBST, -3000);
    slot(b, "obstruction -3000");
    seti(ks, g_buf, B_OBST, 0);
    IDirectSound3DBuffer_SetMode(b23d, DS3DMODE_DISABLE, DS3D_IMMEDIATE);
    slot(b2, "disabled room 0");
    seti(ks2, g_buf, B_ROOM, -10000);
    slot(b2, "disabled room -10000");
    seti(ks, g_lst, L_ENV, 0);
    slot(b, "env generic room 0");
    seti(ks, g_lst, L_ENV, 2);
    seti(ks, g_lst, L_ROOM, -10000);
    slot(b, "listener room -10000");
    if (g_ver == 4) {
        seti(ks, g_lst, L_ENV, 2);                       /* back to the short room */
        seti(ks, &EAX4_SLOT[0], 0x10003, -1000);         /* slot volume */
        slot(b, "slot volume -1000");
        seti(ks, &EAX4_SLOT[0], 0x10003, 0);
        /* a second reverb in slot 2, loaded through ALLPARAMETERS like F.E.A.R. does */
        struct { GUID g; LONG volume, lock; DWORD flags; } fs = { EAX_REVERB_EFFECT, 0, 1, 1 };
        kset(ks, &EAX4_SLOT[2], 0x10001, &fs, sizeof(fs));
        BYTE rv[112]; ULONG got = 0;
        hr = IKsPropertySet_Get(ks, &EAX4_SLOT[2], 1, NULL, 0, rv, sizeof(rv), &got);
        LONG env2; float hfref2; memcpy(&env2, rv, 4); memcpy(&hfref2, rv + 0x60, 4);
        P("slot 2 reverb after ALL load: get %08lx env %ld hf reference %.0f\n", hr, env2, hfref2);
        hr = IKsPropertySet_Set(ks, &EAX4_SLOT[2], 1, NULL, 0, rv, sizeof(rv));
        P("slot 2 reverb ALL set back -> %08lx\n", hr);
        seti(ks, &EAX4_SLOT[2], L_ENV, 2);
        GUID act[2] = { EAX4_SLOT[2], { 0 } };
        kset(ks, &EAX4_S, 27, act, sizeof(act));
        slot(b, "slot 2 only");
        act[0] = EAX4_PRIMARY; act[1] = EAX4_SLOT[2];
        kset(ks, &EAX4_S, 27, act, sizeof(act));
        slot(b, "primary + slot 2");
        struct { GUID g; LONG room, roomhf; } snd = { EAX4_SLOT[2], -10000, 0 };
        kset(ks, &EAX4_S, 23, &snd, sizeof(snd));
        slot(b, "primary + slot 2 send -10000");
        /* deferred source change, committed by a slot NONE set (F.E.A.R.'s pattern) */
        seti(ks, &EAX4_S, 7 | 0x80000000u, -10000);
        kset(ks, &EAX4_SLOT[1], 0, NULL, 0);
        slot(b, "deferred room -10000 committed");
        seti(ks, &EAX4_S, 7, 0);
        /* the other effects: echo, distortion and pitch shifter in slot 3, the source sends only there */
        act[0] = EAX4_SLOT[3]; act[1] = (GUID){ 0 };
        kset(ks, &EAX4_S, 27, act, sizeof(act));
        kset(ks, &EAX4_SLOT[3], 0x10002, (void *)&EAX_ECHO_EFFECT, sizeof(GUID));
        float dl = 0.15f, lr = 0.1f, fb = 0.5f;
        kset(ks, &EAX4_SLOT[3], 2, &dl, 4); kset(ks, &EAX4_SLOT[3], 3, &lr, 4); kset(ks, &EAX4_SLOT[3], 5, &fb, 4);
        slot(b, "slot 3 echo 150/250 ms feedback 0.5");
        kset(ks, &EAX4_SLOT[3], 0x10002, (void *)&EAX_DISTORTION_EFFECT, sizeof(GUID));
        slot(b, "slot 3 distortion");
        kset(ks, &EAX4_SLOT[3], 0x10002, (void *)&EAX_PITCH_EFFECT, sizeof(GUID));
        seti(ks, &EAX4_SLOT[3], 2, -12);    /* down an octave (a one-sample click is skipped when pitched up) */
        slot(b, "slot 3 pitch shifter -12");
        LONG err = 0; IKsPropertySet_Get(ks, &EAX4_C, 6, NULL, 0, &err, 4, &got);
        P("last error %ld\n", err);
    }
    P("END\n");
    Sleep(300);
    InterlockedExchange(&g_rec_stop, 1);
    WaitForSingleObject(rec, 10000);
    fclose(g_out);
    return 0;
}
