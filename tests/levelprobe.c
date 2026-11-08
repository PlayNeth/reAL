/* levelprobe - how loud a tone comes out, per output channel, for 2D and 3D buffers in the usual placements: the
 * default output's loopback (WASAPI) recorded while each plays.  Compares the gain and panning laws of the
 * dsound.dll next to it (tools/alchemy-kit run5.bat: ALchemy, reAL, Windows).
 *   levelprobe.exe */
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

DEFINE_GUID(LP_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(LP_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(LP_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(LP_IID_IAudioCaptureClient,  0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(LP_SUBTYPE_FLOAT,            0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define MAXCH 8

static IAudioClient *s_ac;
static IAudioCaptureClient *s_cap;
static int s_float, s_bits, s_ch;

static int loopback_open(void)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    WAVEFORMATEX *wf = NULL;
    HRESULT hr = CoCreateInstance(&LP_CLSID_MMDeviceEnumerator, NULL, CLSCTX_INPROC_SERVER, &LP_IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { P("loopback: no device enumerator %08lx\n", hr); return 0; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    IMMDeviceEnumerator_Release(en);
    if (FAILED(hr)) { P("loopback: no default output %08lx\n", hr); return 0; }
    hr = IMMDevice_Activate(dev, &LP_IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&s_ac);
    IMMDevice_Release(dev);
    if (FAILED(hr)) { P("loopback: Activate %08lx\n", hr); return 0; }
    hr = IAudioClient_GetMixFormat(s_ac, &wf);
    if (FAILED(hr)) { P("loopback: GetMixFormat %08lx\n", hr); return 0; }
    s_ch = wf->nChannels; s_bits = wf->wBitsPerSample;
    s_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
              (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)wf)->SubFormat, &LP_SUBTYPE_FLOAT));
    P("loopback: %d ch %lu Hz %d bit%s\n", s_ch, wf->nSamplesPerSec, s_bits, s_float ? " float" : "");
    hr = IAudioClient_Initialize(s_ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 20000000, 0, wf, NULL);
    CoTaskMemFree(wf);
    if (FAILED(hr)) { P("loopback: Initialize %08lx\n", hr); return 0; }
    hr = IAudioClient_GetService(s_ac, &LP_IID_IAudioCaptureClient, (void **)&s_cap);
    if (FAILED(hr)) { P("loopback: GetService %08lx\n", hr); return 0; }
    hr = IAudioClient_Start(s_ac);
    if (FAILED(hr)) { P("loopback: Start %08lx\n", hr); return 0; }
    return 1;
}

/* captured since the last call: each channel's sum of squares and the frame count (dropped when sum is NULL) */
static void loopback_read(double *sum, double *frames_out)
{
    UINT32 n = 0;
    while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(s_cap, &n)) && n) {
        BYTE *d; UINT32 frames; DWORD flags;
        if (FAILED(IAudioCaptureClient_GetBuffer(s_cap, &d, &frames, &flags, NULL, NULL))) break;
        if (sum) {
            for (UINT32 f = 0; f < frames && !(flags & AUDCLNT_BUFFERFLAGS_SILENT); f++)
                for (int c = 0; c < s_ch && c < MAXCH; c++) {
                    UINT32 i = f * s_ch + c;
                    double v = s_float ? ((float *)d)[i] : s_bits == 16 ? ((short *)d)[i] / 32768.0 :
                               s_bits == 32 ? ((int *)d)[i] / 2147483648.0 : 0.0;
                    sum[c] += v * v;
                }
            *frames_out += frames;
        }
        IAudioCaptureClient_ReleaseBuffer(s_cap, frames);
    }
}

static double db(double ms) { return ms > 0 ? 10.0 * log10(ms) : -200.0; }

/* each channel's level over ms milliseconds from now (dB full scale; -200 silence), printed */
static void levels(const char *what, int ms)
{
    double sum[MAXCH] = { 0 }, frames = 0;
    loopback_read(NULL, NULL);
    DWORD end = GetTickCount() + (DWORD)ms;
    while ((LONG)(end - GetTickCount()) > 0) { Sleep(10); loopback_read(sum, &frames); }
    loopback_read(sum, &frames);
    P("%-34s", what);
    double all = 0;
    for (int c = 0; c < s_ch && c < MAXCH; c++) {
        double m = frames ? sum[c] / frames : 0;
        all += m;
        P(" ch%d %6.1f", c, db(m));
    }
    P("  (sum of the channels %6.1f dB)\n", db(all));
}

static IDirectSound8 *s_ds;

/* a looping second of a 441 Hz tone at -6 dB peak, mono or the same on both channels */
static IDirectSoundBuffer *tone(int channels, DWORD flags)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)channels, 22050, 22050 * 2 * channels, (WORD)(2 * channels), 16, 0 };
    DSBUFFERDESC bd = { sizeof(bd), flags | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS, 22050 * 2 * channels, 0, &wf, { 0 } };
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(s_ds, &bd, &b, NULL);
    if (!b) { P("CreateSoundBuffer(%d ch, flags %lx) %08lx\n", channels, flags, hr); return NULL; }
    void *p1; DWORD n1;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &n1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) {
        short *s = p1;
        for (DWORD i = 0; i < n1 / 2; i++)
            s[i] = (short)(16000.0 * sin(2.0 * 3.14159265358979 * 441.0 * (double)(i / channels) / 22050.0));
        IDirectSoundBuffer_Unlock(b, p1, n1, NULL, 0);
    }
    return b;
}

static void play_measure(IDirectSoundBuffer *b, const char *what)
{
    IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
    Sleep(800);
    levels(what, 500);
    IDirectSoundBuffer_Stop(b);
    Sleep(300);
}

static DWORD WINAPI watchdog(LPVOID p)
{
    Sleep((DWORD)(UINT_PTR)p);
    P("TIMEOUT\n");
    ExitProcess(3);
    return 0;
}

int main(void)
{
    CreateThread(NULL, 0, watchdog, (LPVOID)(UINT_PTR)180000, 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitialize(NULL);
    char path[MAX_PATH] = "";
    HMODULE m = LoadLibraryA("dsound.dll");
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    P("levelprobe: dsound.dll %s\n", path);
    if (!loopback_open()) return 1;
    HRESULT hr = DirectSoundCreate8(NULL, &s_ds, NULL);
    if (FAILED(hr)) { P("DirectSoundCreate8 %08lx\n", hr); return 1; }
    IDirectSound8_SetCooperativeLevel(s_ds, GetDesktopWindow(), DSSCL_PRIORITY);
    DWORD sc = 0;
    IDirectSound8_GetSpeakerConfig(s_ds, &sc);
    P("speaker config %08lx\n", sc);
    DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, 0, NULL, { 0 } };
    IDirectSoundBuffer *prim = NULL;
    IDirectSound8_CreateSoundBuffer(s_ds, &pd, &prim, NULL);
    levels("silence", 300);
    /* 2D */
    IDirectSoundBuffer *b = tone(1, DSBCAPS_CTRLPAN);
    if (b) {
        static const LONG pans[] = { 0, -2000, 2000, -10000, 10000 };
        for (unsigned k = 0; k < sizeof(pans) / sizeof(pans[0]); k++) {
            char w[64];
            IDirectSoundBuffer_SetPan(b, pans[k]);
            snprintf(w, sizeof(w), "2D mono, pan %ld", pans[k]);
            play_measure(b, w);
        }
        IDirectSoundBuffer_Release(b);
    }
    if ((b = tone(2, 0))) { play_measure(b, "2D stereo"); IDirectSoundBuffer_Release(b); }
    /* 3D, the listener at the origin facing +z, the default roll-off; at distance 1 (the minimum): no attenuation */
    if ((b = tone(1, DSBCAPS_CTRL3D))) {
        IDirectSound3DBuffer *d = NULL;
        IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&d);
        if (d) {
            static const struct { float x, y, z; const char *n; } at[] = {
                { 0, 0, 1, "3D mono, ahead" }, { 1, 0, 0, "3D mono, right" }, { -1, 0, 0, "3D mono, left" },
                { 0, 0, -1, "3D mono, behind" }, { 0, 1, 0, "3D mono, above" },
                { 0.70710678f, 0, 0.70710678f, "3D mono, 45 deg right" }, { 0, 0, 2, "3D mono, ahead at 2" },
            };
            for (unsigned k = 0; k < sizeof(at) / sizeof(at[0]); k++) {
                IDirectSound3DBuffer_SetPosition(d, at[k].x, at[k].y, at[k].z, DS3D_IMMEDIATE);
                play_measure(b, at[k].n);
            }
            IDirectSound3DBuffer_SetPosition(d, 0, 0, 1, DS3D_IMMEDIATE);
            IDirectSound3DBuffer_SetMode(d, DS3DMODE_HEADRELATIVE, DS3D_IMMEDIATE);
            play_measure(b, "3D mono, head-relative ahead");
            IDirectSound3DBuffer_SetMode(d, DS3DMODE_DISABLE, DS3D_IMMEDIATE);
            play_measure(b, "3D mono, 3D disabled");
            IDirectSound3DBuffer_Release(d);
        }
        IDirectSoundBuffer_Release(b);
    }
    if (prim) IDirectSoundBuffer_Release(prim);
    IDirectSound8_Release(s_ds);
    IAudioClient_Stop(s_ac);
    P("END\n");
    return 0;
}
