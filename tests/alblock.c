/* alblock: the block size ALchemy's software engine would mix in on this PC.
 * ALchemy (CWASAPIRender, fns 0x1001a560 / 0x1001a3f0) opens the default output in shared mode asking for
 * 24 periods of 128 * floor(30000 * rate / 1.28e9) frames, then mixes GetBufferSize() / 24 frames per block.
 * This asks Windows for the same buffer and prints the result.  No ALchemy needed.
 *   alblock.exe   -> prints, and writes alblock.txt next to it */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>

DEFINE_GUID(CLSID_MMDeviceEnumerator_, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(IID_IMMDeviceEnumerator_, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(IID_IAudioClient_, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);

static FILE *out;
static void say(const char *fmt, ...)
{
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (out) { va_start(a, fmt); vfprintf(out, fmt, a); va_end(a); }
}

int main(void)
{
    out = fopen("alblock.txt", "w");
    CoInitialize(NULL);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *ac = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator_, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator_, (void **)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &IID_IAudioClient_, CLSCTX_ALL, NULL, (void **)&ac);
    WAVEFORMATEX *wf = NULL;
    if (SUCCEEDED(hr)) hr = IAudioClient_GetMixFormat(ac, &wf);
    if (FAILED(hr)) { say("could not open the default output (%08lx)\n", hr); return 1; }
    UINT32 rate = wf->nSamplesPerSec;
    UINT32 period = (UINT32)((unsigned long long)30000 * rate / (128ull * 10000000)) * 128;
    REFERENCE_TIME dur = (REFERENCE_TIME)((unsigned long long)24 * period * 10000000 / rate);
    REFERENCE_TIME defp = 0, minp = 0;
    IAudioClient_GetDevicePeriod(ac, &defp, &minp);
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, wf, NULL);
    UINT32 frames = 0;
    if (SUCCEEDED(hr)) hr = IAudioClient_GetBufferSize(ac, &frames);
    say("output: %lu Hz, %u channels; device period %.2f ms (minimum %.2f ms)\n", (unsigned long)rate, wf->nChannels,
        defp / 10000.0, minp / 10000.0);
    if (FAILED(hr)) { say("Initialize/GetBufferSize failed (%08lx)\n", hr); return 1; }
    say("ALchemy asks for %lu frames (%.2f ms), Windows gives %lu frames\n", (unsigned long)(24 * period),
        dur / 10000.0, (unsigned long)frames);
    say("=> ALchemy's mix block here: %lu frames\n", (unsigned long)(frames / 24));
    if (out) fclose(out);
    return 0;
}
