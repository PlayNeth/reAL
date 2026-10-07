/* hrtfprobe - measure a DirectSound3D implementation's spatialisation (e.g. ALchemy's HRTF).
 *
 * Plays one click per position on a sphere around the listener through 3D buffers, after three
 * plain 2D sync clicks. Record the system output while it runs; tools/hrtf_extract.py turns the
 * recording into per-position left/right impulse responses.
 *
 * Usage: hrtfprobe.exe [path\to\dsound-implementation.dll] [--norecord]
 *   default: alchemy_dsound.dll next to the exe if present, else dsound.dll
 * Writes hrtfprobe.txt (schedule + what the implementation reports) and, unless --norecord,
 * hrtfprobe.wav: a WASAPI loopback recording of the default output device made while it runs.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#define RATE 48000
#define SLOT_MS 350
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
        P("loopback recording unavailable (%08lx)%s\n", hr, SUCCEEDED(hr) && !f ? " - cannot create hrtfprobe.wav" : "");
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

static void play_slot(IDirectSoundBuffer *b, const char *label)
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
    char dll[MAX_PATH];
    int record = 1;
    if (argc > 1 && !strcmp(argv[argc - 1], "--norecord")) { record = 0; argc--; }
    if (argc > 1) lstrcpynA(dll, argv[1], MAX_PATH);
    else {
        GetModuleFileNameA(NULL, dll, MAX_PATH);
        char *s = strrchr(dll, '\\'); if (s) s[1] = 0;
        lstrcatA(dll, "alchemy_dsound.dll");
        if (GetFileAttributesA(dll) == INVALID_FILE_ATTRIBUTES) lstrcpyA(dll, "dsound.dll");
    }
    g_out = fopen("hrtfprobe.txt", "w");
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    HMODULE h = LoadLibraryA(dll);
    char path[MAX_PATH] = "?";
    if (h) GetModuleFileNameA(h, path, MAX_PATH);
    P("hrtfprobe 1  dll=%s  loaded=%s\n", dll, path);
    if (!h) return 1;
    HRESULT (WINAPI *create)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN) = (void *)GetProcAddress(h, "DirectSoundCreate");
    IDirectSound *ds = NULL;
    HRESULT hr = create ? create(NULL, &ds, NULL) : E_FAIL;
    P("DirectSoundCreate -> %08lx\n", hr);
    if (FAILED(hr)) return 1;
    IDirectSound_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);

    DWORD sc = 0;
    IDirectSound_GetSpeakerConfig(ds, &sc);
    P("speaker config before: %lx\n", sc);
    hr = IDirectSound_SetSpeakerConfig(ds, DSSPEAKER_HEADPHONE);
    IDirectSound_GetSpeakerConfig(ds, &sc);
    P("SetSpeakerConfig(HEADPHONE) -> %08lx, now %lx\n", hr, sc);

    DSCAPS caps = { sizeof(caps) };
    IDirectSound_GetCaps(ds, &caps);
    P("caps flags %lx hw3d %lu/%lu\n", caps.dwFlags, caps.dwFreeHw3DAllBuffers, caps.dwMaxHw3DAllBuffers);

    DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D };
    IDirectSoundBuffer *prim = NULL;
    IDirectSound_CreateSoundBuffer(ds, &pd, &prim, NULL);
    WAVEFORMATEX pf = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    if (prim) IDirectSoundBuffer_SetFormat(prim, &pf);
    IDirectSound3DListener *lst = NULL;
    if (prim) IDirectSoundBuffer_QueryInterface(prim, &IID_IDirectSound3DListener, (void **)&lst);

    IDirectSoundBuffer *b2d = make_click_buffer(ds, DSBCAPS_GLOBALFOCUS);
    IDirectSoundBuffer *b3 = make_click_buffer(ds, DSBCAPS_CTRL3D | DSBCAPS_LOCHARDWARE | DSBCAPS_GLOBALFOCUS);
    if (!b3) b3 = make_click_buffer(ds, DSBCAPS_CTRL3D | DSBCAPS_GLOBALFOCUS);
    if (!b2d || !b3) return 1;
    IDirectSound3DBuffer *b3d = NULL;
    IDirectSoundBuffer_QueryInterface(b3, &IID_IDirectSound3DBuffer, (void **)&b3d);
    IKsPropertySet *ks = NULL;
    IDirectSoundBuffer_QueryInterface(b3, &IID_IKsPropertySet, (void **)&ks);

    if (ks) {
        struct { const GUID *g; const char *n; } sets[] = {
            { &EAX1_L, "EAX1 listener" }, { &EAX1_B, "EAX1 buffer" }, { &EAX2_L, "EAX2 listener" },
            { &EAX2_B, "EAX2 buffer" }, { &EAX3_L, "EAX3 listener" }, { &EAX3_B, "EAX3 buffer" },
            { &EAX4_C, "EAX4 context" }, { &EAX4_S, "EAX4 source" }, { &EAX5_C, "EAX5 context" },
            { &EAX5_S, "EAX5 source" },
        };
        for (int i = 0; i < 10; i++) {
            ULONG sup = 0;
            hr = IKsPropertySet_QuerySupport(ks, sets[i].g, 0, &sup);
            P("QuerySupport %-14s -> %08lx support %lu\n", sets[i].n, hr, sup);
        }
        /* reverb off: EAX1 padded cell at volume 0 and no send from this buffer */
        DWORD env = 1; float zero = 0.0f;
        P("EAX1 env 1 -> %08lx\n", IKsPropertySet_Set(ks, &EAX1_L, 1, NULL, 0, &env, 4));
        P("EAX1 volume 0 -> %08lx\n", IKsPropertySet_Set(ks, &EAX1_L, 2, NULL, 0, &zero, 4));
        P("EAX1 buffer mix 0 -> %08lx\n", IKsPropertySet_Set(ks, &EAX1_B, 1, NULL, 0, &zero, 4));
    }
    if (lst) {
        IDirectSound3DListener_SetPosition(lst, 0, 0, 0, DS3D_IMMEDIATE);
        IDirectSound3DListener_SetOrientation(lst, 0, 0, 1, 0, 1, 0, DS3D_IMMEDIATE);
    }
    IDirectSound3DBuffer_SetMinDistance(b3d, 1.0f, DS3D_IMMEDIATE);

    HANDLE rec = NULL;
    if (record) {
        g_rec_ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        rec = CreateThread(NULL, 0, rec_thread, "hrtfprobe.wav", 0, NULL);
        WaitForSingleObject(g_rec_ready, 10000);
    }

    /* warm-up so device start-up latency isn't in the first slot */
    Sleep(1000);
    P("BEGIN\n");
    for (int i = 0; i < 3; i++) play_slot(b2d, "sync 2d");
    Sleep(500);

    static const int elevs[] = { -40, -20, 0, 20, 40, 60 };
    char label[128];
    for (int e = 0; e < 6; e++) {
        for (int az = 0; az < 360; az += 15) {
            double ra = az * 3.14159265358979 / 180.0, re = elevs[e] * 3.14159265358979 / 180.0;
            float x = (float)(sin(ra) * cos(re)), y = (float)sin(re), z = (float)(cos(ra) * cos(re));
            IDirectSound3DBuffer_SetPosition(b3d, x, y, z, DS3D_IMMEDIATE);
            sprintf(label, "pos az %d el %d dist 1", az, elevs[e]);
            play_slot(b3, label);
        }
    }
    IDirectSound3DBuffer_SetPosition(b3d, 0, 1, 0, DS3D_IMMEDIATE);
    play_slot(b3, "pos az 0 el 90 dist 1");
    static const float dists[] = { 1, 2, 4, 8, 16 };
    for (int i = 0; i < 5; i++) {
        IDirectSound3DBuffer_SetPosition(b3d, 0, 0, dists[i], DS3D_IMMEDIATE);
        sprintf(label, "pos az 0 el 0 dist %g", dists[i]);
        play_slot(b3, label);
    }
    P("END\n");
    if (rec) {
        Sleep(500);
        InterlockedExchange(&g_rec_stop, 1);
        WaitForSingleObject(rec, 10000);
    }

    if (ks) IKsPropertySet_Release(ks);
    IDirectSound3DBuffer_Release(b3d);
    IDirectSoundBuffer_Release(b3);
    IDirectSoundBuffer_Release(b2d);
    if (lst) IDirectSound3DListener_Release(lst);
    if (prim) IDirectSoundBuffer_Release(prim);
    IDirectSound_Release(ds);
    fclose(g_out);
    if (record && !g_rec_ok) {
        printf("\nNo recording was made - record the output yourself while running with --norecord.\n");
        return 2;
    }
    printf("\nDone. Send hrtfprobe.wav and hrtfprobe.txt.\n");
    return 0;
}
