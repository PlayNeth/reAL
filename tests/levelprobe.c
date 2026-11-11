/* levelprobe - how loud a tone comes out, per output channel, for 2D and 3D buffers in the usual placements: the
 * default output's loopback (WASAPI) recorded while each plays.  Compares the gain and panning laws of the
 * dsound.dll next to it (tools/alchemy-kit run5.bat to run7.bat: ALchemy, reAL, Windows).
 *   levelprobe.exe          a 441 Hz tone: 2D at five pans, 2D stereo, 3D in nine places
 *   levelprobe.exe noise    pink noise: 2D, and 3D every 15 degrees of azimuth on the right, two on the left, five
 *                           elevations ahead and one up on the right; each channel's level in all, below 500 Hz,
 *                           500 Hz to 4 kHz and above 4 kHz
 *   levelprobe.exe noise headphones   the same after SetSpeakerConfig(DSSPEAKER_HEADPHONE) (the kit's run8.bat)
 *   levelprobe.exe clicks [file]   one sample from each direction, the response written to the file (run9.bat)
 *   levelprobe.exe near     pink noise: 2D mono and stereo, then a mono and a stereo 3D buffer at the listener's own
 *                           position, head-relative, inside the minimum distance and with 3D disabled */
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

DEFINE_GUID(LP_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(LP_IID_IMMDeviceEnumerator,  0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(LP_IID_IAudioClient,         0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(LP_IID_IAudioCaptureClient,  0xC8ADBD64, 0xE71E, 0x48A0, 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17);
DEFINE_GUID(LP_SUBTYPE_FLOAT,            0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

#define P(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)
#define MAXCH 8
#define CAPMAX (48000 * 4)          /* frames kept for the bands (a measurement is 0.5 s; up to 192 kHz) */

static float s_capbuf[2][CAPMAX];   /* the first two channels of the measurement */
static UINT32 s_capn;
static int s_bands;                 /* noise mode: the band levels too */
static UINT32 s_rate = 48000;

static IAudioClient *s_ac;
static IAudioCaptureClient *s_cap;
static int s_float, s_bits, s_ch, s_nocap;   /* nocap: no loopback (Wine): each case only plays, between silences */

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
    s_ch = wf->nChannels; s_bits = wf->wBitsPerSample; s_rate = wf->nSamplesPerSec;
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
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)    /* (zeros in the band capture too) */
                for (UINT32 f = 0; f < frames && s_capn + f < CAPMAX; f++) s_capbuf[0][s_capn + f] = s_capbuf[1][s_capn + f] = 0.0f;
            for (UINT32 f = 0; f < frames && !(flags & AUDCLNT_BUFFERFLAGS_SILENT); f++)
                for (int c = 0; c < s_ch && c < MAXCH; c++) {
                    UINT32 i = f * s_ch + c;
                    double v = s_float ? ((float *)d)[i] : s_bits == 16 ? ((short *)d)[i] / 32768.0 :
                               s_bits == 32 ? ((int *)d)[i] / 2147483648.0 : 0.0;
                    sum[c] += v * v;
                    if (c < 2 && s_capn + f < CAPMAX) s_capbuf[c][s_capn + f] = (float)v;
                }
            *frames_out += frames;
        }
        if (sum) s_capn = s_capn + frames < CAPMAX ? s_capn + frames : CAPMAX;
        IAudioCaptureClient_ReleaseBuffer(s_cap, frames);
    }
}

static double db(double ms) { return ms > 0 ? 10.0 * log10(ms) : -200.0; }

/* each channel's level over ms milliseconds from now (dB full scale; -200 silence), printed */
static void levels(const char *what, int ms)
{
    if (s_nocap) { Sleep((DWORD)ms); P("%s\n", what); return; }  /* (recorded from outside: tools/levels.py) */
    double sum[MAXCH] = { 0 }, frames = 0;
    loopback_read(NULL, NULL);
    s_capn = 0;
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
    if (!s_bands || !s_capn) return;
    /* the bands: two one-pole low-passes at 500 Hz, the rest above 500 Hz split again at 4 kHz the same way */
    double a5 = 1.0 - exp(-2.0 * 3.14159265358979 * 500.0 / s_rate), a4 = 1.0 - exp(-2.0 * 3.14159265358979 * 4000.0 / s_rate);
    P("%-34s", "");
    for (int c = 0; c < 2 && c < s_ch; c++) {
        double l1 = 0, l2 = 0, h1 = 0, h2 = 0, el = 0, em = 0, eh = 0;
        for (UINT32 i = 0; i < s_capn; i++) {
            double x = s_capbuf[c][i];
            l1 += a5 * (x - l1); l2 += a5 * (l1 - l2);
            double r = x - l2;              /* above 500 Hz */
            h1 += a4 * (r - h1); h2 += a4 * (h1 - h2);
            double hi = r - h2, mid = h2;
            el += l2 * l2; em += mid * mid; eh += hi * hi;
        }
        P(" ch%d low %6.1f mid %6.1f high %6.1f", c, db(el / s_capn), db(em / s_capn), db(eh / s_capn));
    }
    P("\n");
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

/* two seconds of pink noise (Paul Kellet's filter on a fixed white noise), about -15 dB RMS, mono, looping */
static IDirectSoundBuffer *noise_ch(DWORD flags, int ch)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)ch, 44100, 88200 * ch, (WORD)(2 * ch), 16, 0 };
    DSBUFFERDESC bd = { sizeof(bd), flags | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS, 176400 * ch, 0, &wf, { 0 } };
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(s_ds, &bd, &b, NULL);
    if (!b) { P("CreateSoundBuffer(noise, flags %lx) %08lx\n", flags, hr); return NULL; }
    void *p1; DWORD n1;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &n1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) {
        short *s = p1;
        unsigned seed = 12345;
        double b0 = 0, b1 = 0, b2 = 0;
        for (DWORD i = 0; i < n1 / 2; i++) {
            if (ch == 2 && (i & 1)) { s[i] = i >= 22051 ? s[i - 22051] : 0; continue; }   /* (the right: the left a quarter second late) */
            seed = seed * 1103515245u + 12345u;
            double w = ((seed >> 8) & 0xffff) / 32768.0 - 1.0;
            b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913;
            double v = (b0 + b1 + b2 + w * 0.1848) * 0.11;
            if (v > 0.99) v = 0.99; else if (v < -0.99) v = -0.99;
            s[i] = (short)(v * 32767.0);
        }
        IDirectSoundBuffer_Unlock(b, p1, n1, NULL, 0);
    }
    return b;
}

static IDirectSoundBuffer *noise(DWORD flags) { return noise_ch(flags, 1); }

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

static void noise_run(void)
{
    s_bands = 1;
    IDirectSoundBuffer *b = noise(0);
    if (b) { play_measure(b, "2D noise"); IDirectSoundBuffer_Release(b); }
    if (!(b = noise(DSBCAPS_CTRL3D))) return;
    IDirectSound3DBuffer *d = NULL;
    IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&d);
    if (!d) { IDirectSoundBuffer_Release(b); return; }
    static const struct { int az, el; } at[] = {
        { 0, 0 }, { 15, 0 }, { 30, 0 }, { 45, 0 }, { 60, 0 }, { 75, 0 }, { 90, 0 }, { 105, 0 }, { 120, 0 }, { 135, 0 },
        { 150, 0 }, { 165, 0 }, { 180, 0 }, { -30, 0 }, { -90, 0 },
        { 0, 30 }, { 0, 60 }, { 0, 90 }, { 0, -30 }, { 0, -60 }, { 90, 45 },
    };
    for (unsigned k = 0; k < sizeof(at) / sizeof(at[0]); k++) {
        double a = at[k].az * 3.14159265358979 / 180.0, e = at[k].el * 3.14159265358979 / 180.0;
        IDirectSound3DBuffer_SetPosition(d, (float)(sin(a) * cos(e)), (float)sin(e), (float)(cos(a) * cos(e)), DS3D_IMMEDIATE);
        char w[64];
        snprintf(w, sizeof(w), "3D noise, azimuth %d elevation %d", at[k].az, at[k].el);
        play_measure(b, w);
    }
    IDirectSound3DBuffer_Release(d);
    IDirectSoundBuffer_Release(b);
}

/* levelprobe clicks [file]: the dsound.dll's response to a single sample, for each direction - what it does to a
 * positioned sound, as a filter (the levels of `noise` are that filter's in three bands).  A mono 3D buffer at the
 * output's rate holding one sample at -6 dB is played once from each direction at the minimum distance, and what the
 * first two channels get is written to the file (default clicks.f32): for each case, in the order printed, IR_LEN
 * frames of 32-bit float left and right, from IR_PRE frames before the first sample that is not silence.  First a 2D
 * buffer (the path's own delay and level), then every 15 degrees around at elevations 0 and +-30, every 30 degrees at
 * +-60, straight up; then checks that it is a fixed filter: one direction three times, at half the level, and from
 * twice the distance.  A silent buffer loops throughout, so the output never stops between cases. */
#define IR_LEN 4096
#define IR_PRE 64
static FILE *s_irf;
static IDirectSoundBuffer *click_buffer(DWORD flags, int amp)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, s_rate, s_rate * 2, 2, 16, 0 };
    DWORD bytes = (s_rate * 3 / 10) * 2;
    DSBUFFERDESC bd = { sizeof(bd), flags | DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS, bytes, 0, &wf, { 0 } };
    IDirectSoundBuffer *b = NULL;
    HRESULT hr = IDirectSound8_CreateSoundBuffer(s_ds, &bd, &b, NULL);
    if (!b) { P("CreateSoundBuffer(click, flags %lx) %08lx\n", flags, hr); return NULL; }
    void *p1; DWORD n1;
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 0, &p1, &n1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) {
        memset(p1, 0, n1);
        if (n1 >= 2 * 257) ((short *)p1)[256] = (short)amp;
        IDirectSoundBuffer_Unlock(b, p1, n1, NULL, 0);
    }
    return b;
}
/* one click: played once, half a second recorded, the response cut out and written */
static void click_measure(IDirectSoundBuffer *b, const char *what)
{
    static float out[IR_LEN * 2];
    double sum[MAXCH] = { 0 }, frames = 0;
    Sleep(120);                                     /* (the place set reaches the output's next block) */
    if (s_nocap) { IDirectSoundBuffer_Play(b, 0, 0, 0); Sleep(500); IDirectSoundBuffer_Stop(b); P("%s\n", what); return; }
    loopback_read(NULL, NULL);
    s_capn = 0;
    IDirectSoundBuffer_SetCurrentPosition(b, 0);
    IDirectSoundBuffer_Play(b, 0, 0, 0);
    DWORD end = GetTickCount() + 500;
    while ((LONG)(end - GetTickCount()) > 0) { Sleep(10); loopback_read(sum, &frames); }
    loopback_read(sum, &frames);
    IDirectSoundBuffer_Stop(b);
    UINT32 on = s_capn;
    float peak[2] = { 0, 0 };
    for (UINT32 i = 0; i < s_capn; i++)
        for (int c = 0; c < 2; c++) {
            float v = fabsf(s_capbuf[c][i]);
            if (v > peak[c]) peak[c] = v;
            if (v > 1e-4f && i < on) on = i;
        }
    memset(out, 0, sizeof(out));
    UINT32 from = on > IR_PRE ? on - IR_PRE : 0;
    for (UINT32 i = 0; i < IR_LEN && from + i < s_capn; i++) { out[2 * i] = s_capbuf[0][from + i]; out[2 * i + 1] = s_capbuf[1][from + i]; }
    if (s_irf) fwrite(out, sizeof(float), IR_LEN * 2, s_irf);
    P("%-40s captured %lu frames, first sound at %lu, peak ch0 %.5f ch1 %.5f%s\n", what, (unsigned long)s_capn, (unsigned long)on,
      peak[0], peak[1], on == s_capn ? "  (NOTHING HEARD)" : on + IR_LEN - IR_PRE > s_capn ? "  (cut short)" : "");
}
static void click_at(IDirectSound3DBuffer *d, IDirectSoundBuffer *b, int az, int el, float dist, const char *tag)
{
    double a = az * 3.14159265358979 / 180.0, e = el * 3.14159265358979 / 180.0;
    IDirectSound3DBuffer_SetPosition(d, (float)(sin(a) * cos(e)) * dist, (float)sin(e) * dist, (float)(cos(a) * cos(e)) * dist,
                                     DS3D_IMMEDIATE);
    char w[64];
    snprintf(w, sizeof(w), "click az %d el %d%s", az, el, tag);
    click_measure(b, w);
}
static void click_run(const char *file)
{
    if (!s_nocap && !(s_irf = fopen(file, "wb"))) { P("cannot write %s\n", file); return; }
    P("clicks: %d frames a case before the first sound, %d in all, 32-bit float left and right, at %lu Hz\n", IR_PRE, IR_LEN,
      (unsigned long)s_rate);
    /* the output kept running: a loop of silence */
    WAVEFORMATEX zf = { WAVE_FORMAT_PCM, 2, s_rate, s_rate * 4, 4, 16, 0 };
    DSBUFFERDESC zd = { sizeof(zd), DSBCAPS_GLOBALFOCUS, s_rate * 4, 0, &zf, { 0 } };
    IDirectSoundBuffer *z = NULL;
    IDirectSound8_CreateSoundBuffer(s_ds, &zd, &z, NULL);
    if (z) {
        void *p1; DWORD n1;
        if (SUCCEEDED(IDirectSoundBuffer_Lock(z, 0, 0, &p1, &n1, NULL, NULL, DSBLOCK_ENTIREBUFFER))) { memset(p1, 0, n1); IDirectSoundBuffer_Unlock(z, p1, n1, NULL, 0); }
        IDirectSoundBuffer_Play(z, 0, 0, DSBPLAY_LOOPING);
    }
    Sleep(500);
    IDirectSoundBuffer *b = click_buffer(0, 16384);
    if (b) { click_measure(b, "click 2D"); click_measure(b, "click 2D again"); IDirectSoundBuffer_Release(b); }
    IDirectSoundBuffer *h = click_buffer(DSBCAPS_CTRL3D, 8192);
    if ((b = click_buffer(DSBCAPS_CTRL3D, 16384))) {
        IDirectSound3DBuffer *d = NULL;
        IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&d);
        if (d) {
            static const int els[] = { 0, 30, -30, 60, -60 };
            for (unsigned k = 0; k < sizeof(els) / sizeof(els[0]); k++)
                for (int az = 0; az < 360; az += (els[k] == 60 || els[k] == -60) ? 30 : 15) click_at(d, b, az, els[k], 1.0f, "");
            click_at(d, b, 0, 90, 1.0f, "");
            click_at(d, b, 45, 0, 1.0f, " (again)");
            click_at(d, b, 45, 0, 1.0f, " (a third time)");
            click_at(d, b, 45, 0, 2.0f, " (twice the distance)");
            click_at(d, b, 45, 0, 0.5f, " (half the minimum distance)");
            IDirectSound3DBuffer_Release(d);
        }
        IDirectSoundBuffer_Release(b);
    }
    if (h) {
        IDirectSound3DBuffer *d = NULL;
        IDirectSoundBuffer_QueryInterface(h, &IID_IDirectSound3DBuffer, (void **)&d);
        if (d) { click_at(d, h, 45, 0, 1.0f, " (half the level)"); click_at(d, h, 0, 0, 1.0f, " (half the level)"); IDirectSound3DBuffer_Release(d); }
        IDirectSoundBuffer_Release(h);
    }
    if (z) { IDirectSoundBuffer_Stop(z); IDirectSoundBuffer_Release(z); }
    if (s_irf) { fclose(s_irf); s_irf = NULL; }
}

/* levelprobe near: what the directions of `noise` leave out - a source at the listener's own position (the usual
 * way to play a sound "nowhere" through a 3D buffer: head-relative at 0,0,0), inside the minimum distance, and a
 * stereo 3D buffer (which ALchemy makes; Windows 7 and later refuse it) */
static void near_run(void)
{
    s_bands = 1;
    IDirectSoundBuffer *b = noise(0);
    if (b) { play_measure(b, "2D noise"); IDirectSoundBuffer_Release(b); }
    if ((b = noise_ch(0, 2))) { play_measure(b, "2D stereo noise"); IDirectSoundBuffer_Release(b); }
    for (int ch = 1; ch <= 2; ch++) {
        if (!(b = noise_ch(DSBCAPS_CTRL3D, ch))) continue;
        IDirectSound3DBuffer *d = NULL;
        IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&d);
        if (!d) { IDirectSoundBuffer_Release(b); continue; }
        static const struct { float x, y, z; DWORD mode; const char *n; } at[] = {
            { 0, 0, 1, DS3DMODE_NORMAL, "ahead at the minimum distance" },
            { 0, 0, 0, DS3DMODE_NORMAL, "at the listener" },
            { 0, 0, 0, DS3DMODE_HEADRELATIVE, "head-relative at the listener" },
            { 0, 0, 1, DS3DMODE_HEADRELATIVE, "head-relative ahead" },
            { 1, 0, 0, DS3DMODE_HEADRELATIVE, "head-relative right" },
            { 0, 0, 0.25f, DS3DMODE_NORMAL, "ahead at a quarter of the minimum distance" },
            { 0.25f, 0, 0, DS3DMODE_NORMAL, "right at a quarter of the minimum distance" },
            { 0.001f, 0, 0, DS3DMODE_NORMAL, "right at a thousandth of the minimum distance" },
            { 1, 0, 0, DS3DMODE_NORMAL, "right at the minimum distance" },
            { 0, 0, 1, DS3DMODE_DISABLE, "3D disabled" },
        };
        for (unsigned k = 0; k < sizeof(at) / sizeof(at[0]); k++) {
            IDirectSound3DBuffer_SetMode(d, at[k].mode, DS3D_IMMEDIATE);
            IDirectSound3DBuffer_SetPosition(d, at[k].x, at[k].y, at[k].z, DS3D_IMMEDIATE);
            char w[96];
            snprintf(w, sizeof(w), "3D %s noise, %s", ch == 2 ? "stereo" : "mono", at[k].n);
            play_measure(b, w);
        }
        IDirectSound3DBuffer_Release(d);
        IDirectSoundBuffer_Release(b);
    }
}

int main(int argc, char **argv)
{
    int noisemode = argc > 1 && !strcmp(argv[1], "noise"), nearmode = argc > 1 && !strcmp(argv[1], "near");
    int clickmode = argc > 1 && !strcmp(argv[1], "clicks");
    CreateThread(NULL, 0, watchdog, (LPVOID)(UINT_PTR)(argc > 1 && !strcmp(argv[1], "clicks") ? 300000 : 180000), 0, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    CoInitialize(NULL);
    char path[MAX_PATH] = "";
    HMODULE m = LoadLibraryA("dsound.dll");
    if (m) GetModuleFileNameA(m, path, MAX_PATH);
    P("levelprobe: dsound.dll %s\n", path);
    if (!loopback_open()) { P("(no loopback: playing only)\n"); s_nocap = 1; }
    HRESULT hr = DirectSoundCreate8(NULL, &s_ds, NULL);
    if (FAILED(hr)) { P("DirectSoundCreate8 %08lx\n", hr); return 1; }
    IDirectSound8_SetCooperativeLevel(s_ds, GetDesktopWindow(), DSSCL_PRIORITY);
    DWORD sc = 0;
    IDirectSound8_GetSpeakerConfig(s_ds, &sc);
    P("speaker config %08lx\n", sc);
    /* "headphones" after the mode: DSSPEAKER_HEADPHONE asked of this DirectSound object before any buffer is
     * made (ALchemy keeps the setting to the object: a new one starts from the device's again), and put back at
     * the end - does the dsound.dll render for headphones then */
    int hp = argc > 2 && !clickmode && !strcmp(argv[2], "headphones");
    if (hp) {
        DWORD now = 0;
        HRESULT h1 = IDirectSound8_SetSpeakerConfig(s_ds, DSSPEAKER_HEADPHONE);
        IDirectSound8_GetSpeakerConfig(s_ds, &now);
        P("SetSpeakerConfig(DSSPEAKER_HEADPHONE) %08lx, now %08lx\n", h1, now);
    }
    DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, 0, NULL, { 0 } };
    IDirectSoundBuffer *prim = NULL;
    IDirectSound8_CreateSoundBuffer(s_ds, &pd, &prim, NULL);
    levels("silence", 300);
    if (noisemode || nearmode || clickmode) {
        if (clickmode) click_run(argc > 2 ? argv[2] : "clicks.f32"); else if (nearmode) near_run(); else noise_run();
        if (hp) P("speaker config put back: %08lx\n", IDirectSound8_SetSpeakerConfig(s_ds, sc));
        if (prim) IDirectSoundBuffer_Release(prim);
        IDirectSound8_Release(s_ds);
        if (s_ac) IAudioClient_Stop(s_ac);
        P("END\n");
        return 0;
    }
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
    if (s_ac) IAudioClient_Stop(s_ac);
    P("END\n");
    return 0;
}
