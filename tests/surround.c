/* Surround check for reAL under Wine/Windows: plays a sequence of test sounds and prints what was played
 * when, so a recording of the device (tools/surround_check.py) can be checked speaker by speaker.
 *   surround.exe [dsound.dll]
 * Sequence (each step 0.6 s, 0.2 s of silence between):
 *   1. a 3D mono tone at 3 m, azimuth 0, 45, 90, 135, 180, -135, -90, -45
 *   2. a 2D stereo buffer with the tone on its left channel only, then on its right only
 *   3. a 6-channel buffer (5.1) with the tone in one channel at a time
 *   4. an 8-channel buffer (7.1) with the tone in its side left then its side right channel
 *   5. a 2D stereo buffer of partly correlated noise (for CMSS-3D: cmss=on puts some on the centre and rear) */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <dsound.h>
#include <math.h>
#include <stdio.h>

typedef HRESULT (WINAPI *create8_t)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);
#define RATE 48000
#define STEP_MS 600
#define GAP_MS 200

static IDirectSoundBuffer *make(IDirectSound8 *ds, int ch, DWORD mask, DWORD flags, int tone_ch)
{
    WAVEFORMATEXTENSIBLE f = { { WAVE_FORMAT_EXTENSIBLE, (WORD)ch, RATE, RATE * 2 * ch, (WORD)(2 * ch), 16, 22 } };
    f.Samples.wValidBitsPerSample = 16;
    f.dwChannelMask = mask;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    DWORD frames = RATE / 2, bytes = frames * 2 * ch;
    DSBUFFERDESC bd = { sizeof(bd), flags | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS, bytes, 0, (WAVEFORMATEX *)&f };
    if (ch <= 2) { f.Format.wFormatTag = WAVE_FORMAT_PCM; f.Format.cbSize = 0; }
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(ds, &bd, &b, NULL);
    if (FAILED(hr)) { printf("CreateSoundBuffer(%d ch) failed %08lx\n", ch, hr); return NULL; }
    void *p; DWORD n;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, bytes, &p, &n, NULL, NULL, 0))) {
        INT16 *s = p;
        for (DWORD i = 0; i < frames; i++)
            for (int c = 0; c < ch; c++)
                s[i * ch + c] = c == tone_ch ? (INT16)(sinf(2.0f * 3.14159265f * 1000.0f * i / RATE) * 8000.0f) : 0;
        IDirectSoundBuffer_Unlock(b, p, n, NULL, 0);
    }
    return b;
}

/* stereo noise: a common part (centre) plus an independent part per side (ambience) */
static IDirectSoundBuffer *make_mix(IDirectSound8 *ds)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, RATE, RATE * 4, 4, 16, 0 };
    DWORD frames = RATE / 2, bytes = frames * 4;
    DSBUFFERDESC bd = { sizeof(bd), DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS, bytes, 0, &f };
    IDirectSoundBuffer *b = NULL;
    if (FAILED(IDirectSound8_CreateSoundBuffer(ds, &bd, &b, NULL))) return NULL;
    void *p; DWORD n;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, bytes, &p, &n, NULL, NULL, 0))) {
        INT16 *s = p;
        unsigned seed = 1;
#define RND() ((seed = seed * 1103515245u + 12345u), ((int)(seed >> 9 & 0xffff) - 32768) / 32768.0f)
        for (DWORD i = 0; i < frames; i++) {
            float c = RND(), l = RND(), r = RND();
            s[2 * i] = (INT16)((0.7f * c + 0.5f * l) * 6000.0f);
            s[2 * i + 1] = (INT16)((0.7f * c + 0.5f * r) * 6000.0f);
        }
        IDirectSoundBuffer_Unlock(b, p, n, NULL, 0);
    }
    return b;
}

static DWORD t0;
static void play(IDirectSoundBuffer *b, const char *what)
{
    if (!b) return;
    printf("%6lu %s\n", GetTickCount() - t0, what);
    fflush(stdout);
    IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
    Sleep(STEP_MS);
    IDirectSoundBuffer_Stop(b);
    Sleep(GAP_MS);
}

int main(int argc, char **argv)
{
    HMODULE h = LoadLibraryA(argc > 1 ? argv[1] : ".\\dsound.dll");
    if (!h) { printf("no dsound.dll\n"); return 1; }
    IDirectSound8 *ds = NULL;
    if (FAILED(((create8_t)GetProcAddress(h, "DirectSoundCreate8"))(NULL, &ds, NULL))) return 1;
    IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    DWORD cfg = 0;
    IDirectSound8_GetSpeakerConfig(ds, &cfg);
    printf("speaker config %08lx\n", cfg);
    DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D };
    IDirectSoundBuffer *prim = NULL;
    IDirectSound8_CreateSoundBuffer(ds, &pd, &prim, NULL);
    IDirectSound3DListener *lst = NULL;
    if (prim) IDirectSoundBuffer_QueryInterface(prim, &IID_IDirectSound3DListener, (void **)&lst);
    Sleep(500);
    t0 = GetTickCount();
    printf("start\n");

    IDirectSoundBuffer *b3 = make(ds, 1, 0, DSBCAPS_CTRL3D, 0);
    IDirectSound3DBuffer *d3 = NULL;
    if (b3) IDirectSoundBuffer_QueryInterface(b3, &IID_IDirectSound3DBuffer, (void **)&d3);
    static const int az[] = { 0, 45, 90, 135, 180, -135, -90, -45 };
    for (int i = 0; i < 8 && d3; i++) {
        float a = az[i] * 3.14159265f / 180.0f;
        IDirectSound3DBuffer_SetPosition(d3, 3.0f * sinf(a), 0.0f, 3.0f * cosf(a), DS3D_IMMEDIATE);
        char what[64];
        snprintf(what, sizeof(what), "3d az %d", az[i]);
        play(b3, what);
    }
    play(make(ds, 2, 0, 0, 0), "2d stereo left");
    play(make(ds, 2, 0, 0, 1), "2d stereo right");
    static const char *n51[] = { "FL", "FR", "FC", "LFE", "BL", "BR" };
    for (int c = 0; c < 6; c++) {
        char what[64];
        snprintf(what, sizeof(what), "5.1 buffer %s", n51[c]);
        play(make(ds, 6, KSAUDIO_SPEAKER_5POINT1, 0, c), what);
    }
    play(make(ds, 8, KSAUDIO_SPEAKER_7POINT1_SURROUND, 0, 6), "7.1 buffer SL");
    play(make(ds, 8, KSAUDIO_SPEAKER_7POINT1_SURROUND, 0, 7), "7.1 buffer SR");
    play(make_mix(ds), "2d stereo noise");
    printf("%6lu end\n", GetTickCount() - t0);
    return 0;
}
