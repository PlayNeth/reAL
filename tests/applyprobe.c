/* applyprobe - what a 3D Set does with each dwApply value, measured by ear: a tone plays through a 3D buffer, the
 * buffer (or the listener) is moved 100 times further away with apply 0, 1, 2, 3 and 0xffffffff, and the level
 * the speakers get is recorded (WASAPI loopback of the default output) straight after the Set and again after
 * CommitDeferredSettings.  A drop after the Set: applied at once; a drop only after the commit: deferred.
 * The values read back can't tell (ALchemy's getters and reAL's give the new value either way: alprobe d3).
 * Uses the dsound.dll next to it (tools/alchemy-kit run4.bat runs it under ALchemy, reAL and Windows).
 *   applyprobe.exe [wait ms]      (default 1000: how long a change is given to reach the output, whatever the
 *                                 output latency set)
 *   applyprobe.exe cross [wait ms]   what else applies a deferred change: an immediate set of the other object (the
 *                                 listener's for the buffer's, the buffer's for the listener's), an immediate set of
 *                                 another value of the same buffer, SetVolume, Stop and Play (run7.bat) */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmreg.h>
#include <dsound.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_GUID(AP_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(AP_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(AP_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(AP_IID_IAudioCaptureClient,  0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(AP_SUBTYPE_FLOAT,            0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

static IAudioClient *s_ac;
static IAudioCaptureClient *s_cap;
static int s_float, s_bits, s_ch;

static int loopback_open(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    WAVEFORMATEX *wf = NULL;
    HRESULT hr = CoCreateInstance(&AP_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &AP_IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { P("loopback: no device enumerator %08lx\n", hr); return 0; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) { P("loopback: no default output %08lx\n", hr); return 0; }
    hr = IMMDevice_Activate(dev, &AP_IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&s_ac);
    IMMDevice_Release(dev);
    if (FAILED(hr)) { P("loopback: Activate %08lx\n", hr); return 0; }
    hr = IAudioClient_GetMixFormat(s_ac, &wf);
    if (FAILED(hr)) { P("loopback: GetMixFormat %08lx\n", hr); return 0; }
    s_ch = wf->nChannels; s_bits = wf->wBitsPerSample;
    s_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
              (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)wf)->SubFormat, &AP_SUBTYPE_FLOAT));
    P("loopback: %d ch %lu Hz %d bit%s\n", s_ch, wf->nSamplesPerSec, s_bits, s_float ? " float" : "");
    hr = IAudioClient_Initialize(s_ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 20000000, 0, wf, NULL);
    CoTaskMemFree(wf);
    if (FAILED(hr)) { P("loopback: Initialize %08lx\n", hr); return 0; }
    hr = IAudioClient_GetService(s_ac, &AP_IID_IAudioCaptureClient, (void **)&s_cap);
    if (FAILED(hr)) { P("loopback: GetService %08lx\n", hr); return 0; }
    hr = IAudioClient_Start(s_ac);
    if (FAILED(hr)) { P("loopback: Start %08lx\n", hr); return 0; }
    return 1;
}

/* what was captured since the last call: its mean square (sum, count); the samples dropped when sum is NULL */
static void loopback_read(double *sum, double *count)
{
    UINT32 n = 0;
    while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(s_cap, &n)) && n) {
        BYTE *d; UINT32 frames; DWORD flags;
        if (FAILED(IAudioCaptureClient_GetBuffer(s_cap, &d, &frames, &flags, NULL, NULL))) break;
        if (sum) {
            UINT32 m = frames * s_ch;
            for (UINT32 i = 0; i < m && !(flags & AUDCLNT_BUFFERFLAGS_SILENT); i++) {
                double v = s_float ? ((float *)d)[i] : s_bits == 16 ? ((short *)d)[i] / 32768.0 :
                           s_bits == 32 ? ((int *)d)[i] / 2147483648.0 : 0.0;
                *sum += v * v;
            }
            *count += m;
        }
        IAudioCaptureClient_ReleaseBuffer(s_cap, frames);
    }
}

/* the output's level over ms milliseconds from now, in dB (full scale); -200 for silence */
static double level(int ms)
{
    loopback_read(NULL, NULL);
    double sum = 0, count = 0;
    DWORD end = GetTickCount() + (DWORD)ms;
    while ((LONG)(end - GetTickCount()) > 0) { Sleep(10); loopback_read(&sum, &count); }
    loopback_read(&sum, &count);
    if (count == 0 || sum <= 0) return -200.0;
    return 10.0 * log10(sum / count);
}

static const char *verdict(double base, double mid, double after)
{
    if (base < -90) return "no sound (nothing to measure)";
    if (mid - base < -10) return "applied at once";
    if (after - base < -10) return "deferred to the commit";
    return "no change, even after the commit";
}

/* a hang (under ALchemy, say) ends the run instead of the batch */
static DWORD WINAPI watchdog(LPVOID p)
{
    Sleep((DWORD)(UINT_PTR)p);
    P("TIMEOUT\n");
    ExitProcess(3);
    return 0;
}

int main(int argc, char **argv)
{
    int cross = argc > 1 && !strcmp(argv[1], "cross");
    if (cross) { argc--; argv++; }
    int wait = argc > 1 ? atoi(argv[1]) : 1000;
    if (wait < 50) wait = 1000;
    CreateThread(NULL, 0, watchdog, (LPVOID)(UINT_PTR)180000, 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitialize(NULL);
    char path[MAX_PATH] = "";
    HMODULE m = LoadLibraryA("dsound.dll");
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    P("applyprobe: dsound.dll %s\n", path);
    if (!loopback_open()) return 1;
    IDirectSound8 *ds = NULL;
    HRESULT hr = DirectSoundCreate8(NULL, &ds, NULL);
    if (FAILED(hr)) { P("DirectSoundCreate8 %08lx\n", hr); return 1; }
    IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, 0, NULL, { 0 } };
    IDirectSoundBuffer *prim = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &pd, &prim, NULL);
    IDirectSound3DListener *l = NULL;
    if (prim) IDirectSoundBuffer_QueryInterface(prim, &IID_IDirectSound3DListener, (void **)&l);
    if (!l) { P("no listener %08lx\n", hr); return 1; }
    /* a second of a 441 Hz tone, mono, looping */
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, 22050, 44100, 2, 16, 0 };
    DSBUFFERDESC bd = { sizeof(bd), DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS, 44100, 0, &wf, { 0 } };
    IDirectSoundBuffer *b = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &bd, &b, NULL);
    if (!b) { P("CreateSoundBuffer %08lx\n", hr); return 1; }
    void *p1; DWORD n1;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 44100, &p1, &n1, NULL, NULL, 0))) {
        short *s = p1;
        for (DWORD i = 0; i < n1 / 2; i++) s[i] = (short)(16000.0 * sin(2.0 * 3.14159265358979 * 441.0 * i / 22050.0));
        IDirectSoundBuffer_Unlock(b, p1, n1, NULL, 0);
    }
    IDirectSound3DBuffer *d = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&d);
    if (!d) { P("no 3D buffer\n"); return 1; }
    P("silence before playing: %.1f dB\n", level(300));
    IDirectSound3DBuffer_SetMinDistance(d, 1.0f, DS3D_IMMEDIATE);
    IDirectSound3DBuffer_SetMaxDistance(d, 1e9f, DS3D_IMMEDIATE);
    IDirectSound3DBuffer_SetPosition(d, 0, 0, 1, DS3D_IMMEDIATE);
    IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
    Sleep(500);
    static const DWORD ap[] = { 0, 1, 2, 3, 0xffffffff };
    double ref = level(300);            /* the level with nothing moved, every case's start checked against it */
    P("playing: %.1f dB\n", ref);
    /* cross: a change set deferred (the buffer or the listener 100 times further away), then one immediate call
     * that changes nothing itself; the level after it, and after the commit */
    static const char *const cases[] = {
        "buffer deferred, then the listener's SetPosition (same place) immediate",
        "listener deferred, then the buffer's SetPosition (same place) immediate",
        "buffer deferred, then the buffer's SetMinDistance (same value) immediate",
        "listener deferred, then the listener's SetRolloffFactor (same value) immediate",
        "buffer deferred, then the buffer's SetVolume (same volume)",
        "buffer deferred, then Stop and Play",
        "listener deferred, then Stop and Play",
    };
    for (unsigned k = 0; cross && k < sizeof(cases) / sizeof(cases[0]); k++) {
        IDirectSound3DListener_CommitDeferredSettings(l);
        IDirectSound3DBuffer_SetPosition(d, 0, 0, 1, DS3D_IMMEDIATE);
        IDirectSound3DListener_SetPosition(l, 0, 0, 0, DS3D_IMMEDIATE);
        Sleep(wait);
        double base = level(300);
        if (base < ref - 10) P("(the reset didn't bring the level back: %.1f dB)\n", base);
        int onlst = k == 1 || k == 3 || k == 6;
        HRESULT h = onlst ? IDirectSound3DListener_SetPosition(l, 0, 0, -99, DS3D_DEFERRED)
                          : IDirectSound3DBuffer_SetPosition(d, 0, 0, 100, DS3D_DEFERRED);
        HRESULT h2 = E_FAIL;
        switch (k) {
        case 0: h2 = IDirectSound3DListener_SetPosition(l, 0, 0, 0, DS3D_IMMEDIATE); break;
        case 1: h2 = IDirectSound3DBuffer_SetPosition(d, 0, 0, 1, DS3D_IMMEDIATE); break;
        case 2: h2 = IDirectSound3DBuffer_SetMinDistance(d, 1.0f, DS3D_IMMEDIATE); break;
        case 3: h2 = IDirectSound3DListener_SetRolloffFactor(l, 1.0f, DS3D_IMMEDIATE); break;
        case 4: h2 = IDirectSoundBuffer_SetVolume(b, 0); break;
        default: IDirectSoundBuffer_Stop(b); h2 = IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING); break;
        }
        Sleep(wait);
        double mid = level(300);
        HRESULT hc = IDirectSound3DListener_CommitDeferredSettings(l);
        Sleep(wait);
        double after = level(300);
        P("%s: %08lx %08lx; level %.1f dB, after the call %.1f dB, after the commit (%08lx) %.1f dB: %s\n", cases[k], h, h2,
          base, mid, hc, after, base < -90 ? "no sound (nothing to measure)" : mid - base < -10 ? "the call applied it" :
          after - base < -10 ? "only the commit applied it" : "no change, even after the commit");
    }
    for (int who = 0; !cross && who < 2; who++)
        for (unsigned k = 0; k < sizeof(ap) / sizeof(ap[0]); k++) {
            /* back to the start: whatever is still pending committed first, then both put back at once */
            IDirectSound3DListener_CommitDeferredSettings(l);
            IDirectSound3DBuffer_SetPosition(d, 0, 0, 1, DS3D_IMMEDIATE);
            IDirectSound3DListener_SetPosition(l, 0, 0, 0, DS3D_IMMEDIATE);
            Sleep(wait);
            double base = level(300);
            if (base < ref - 10) P("(the reset didn't bring the level back: %.1f dB)\n", base);
            /* 100 times the distance: -40 dB with the default roll-off */
            HRESULT h = who ? IDirectSound3DListener_SetPosition(l, 0, 0, -99, ap[k])
                            : IDirectSound3DBuffer_SetPosition(d, 0, 0, 100, ap[k]);
            Sleep(wait);
            double mid = level(300);
            HRESULT hc = IDirectSound3DListener_CommitDeferredSettings(l);
            Sleep(wait);
            double after = level(300);
            P("%s SetPosition apply %lx: %08lx; level %.1f dB, after the Set %.1f dB, after the commit (%08lx) %.1f dB: %s\n",
              who ? "listener" : "buffer  ", ap[k], h, base, mid, hc, after, verdict(base, mid, after));
        }
    IDirectSoundBuffer_Stop(b);
    IDirectSound3DBuffer_Release(d);
    IDirectSoundBuffer_Release(b);
    IDirectSound3DListener_Release(l);
    IDirectSoundBuffer_Release(prim);
    IDirectSound8_Release(ds);
    IAudioClient_Stop(s_ac);
    P("END\n");
    return 0;
}
