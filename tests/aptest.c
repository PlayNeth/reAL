/* aptest - DirectMusic standard audiopaths through the COM interfaces: creates each standard type, asks
 * GetObjectInPath for its buffers, 3D interfaces, the listener and the reverb DMO, prints what comes back (HRESULTs,
 * buffer formats and caps), then plays a file on a 3D path moving around the listener.
 * aptest <file> [ms] */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <dsound.h>
#include <stdio.h>
#include <math.h>

static void show_buffer(IDirectSoundBuffer8 *b)
{
    WAVEFORMATEX wf; DWORD got = 0; DSBCAPS caps = { sizeof(caps) };
    IDirectSoundBuffer8_GetFormat(b, &wf, sizeof(wf), &got);
    IDirectSoundBuffer8_GetCaps(b, &caps);
    LONG vol = 0; DWORD freq = 0;
    IDirectSoundBuffer8_GetVolume(b, &vol);
    IDirectSoundBuffer8_GetFrequency(b, &freq);
    printf("      format %u ch %lu Hz %u bit, caps flags %08lx bytes %lu, volume %ld, frequency %lu\n", wf.nChannels,
           wf.nSamplesPerSec, wf.wBitsPerSample, caps.dwFlags, caps.dwBufferBytes, vol, freq);
}

static void probe(IDirectMusicAudioPath *ap, const char *name)
{
    printf("%s:\n", name);
    for (DWORD k = 0; k < 3; k++) {
        IDirectSoundBuffer8 *b = NULL;
        HRESULT hr = IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, k, &GUID_All_Objects, 0,
                                                           &IID_IDirectSoundBuffer8, (void **)&b);
        printf("  buffer %lu: %08lx\n", k, hr);
        if (SUCCEEDED(hr)) {
            show_buffer(b);
            IDirectSound3DBuffer8 *b3 = NULL;
            hr = IDirectSoundBuffer8_QueryInterface(b, &IID_IDirectSound3DBuffer8, (void **)&b3);
            printf("      IDirectSound3DBuffer8 %08lx\n", hr);
            if (b3) IDirectSound3DBuffer_Release(b3);
            IDirectSoundBuffer8_Release(b);
        }
    }
    IDirectSound3DBuffer8 *b3 = NULL;
    printf("  3D buffer directly: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, 0,
           &GUID_NULL, 0, &IID_IDirectSound3DBuffer8, (void **)&b3));
    if (b3) IDirectSound3DBuffer_Release(b3);
    printf("  buffer for pchannel 0: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_BUFFER, 0,
           &GUID_All_Objects, 0, &IID_IDirectSoundBuffer8, (void **)&b3));
    if (b3) IUnknown_Release((IUnknown *)b3);
    IDirectSound3DListener8 *l = NULL;
    printf("  listener: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_PRIMARY_BUFFER, 0, &GUID_All_Objects, 0,
           &IID_IDirectSound3DListener8, (void **)&l));
    if (l) IDirectSound3DListener_Release(l);
    for (DWORD k = 0; k < 2; k++) {
        IDirectSoundFXWavesReverb8 *r = NULL;
        HRESULT hr = IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER_DMO, k, &GUID_All_Objects, 0,
                                                           &IID_IDirectSoundFXWavesReverb8, (void **)&r);
        printf("  reverb DMO on buffer %lu: %08lx", k, hr);
        if (r) {
            DSFXWavesReverb p;
            IDirectSoundFXWavesReverb_GetAllParameters(r, &p);
            printf(" (in %g mix %g time %g hf %g)", p.fInGain, p.fReverbMix, p.fReverbTime, p.fHighFreqRTRatio);
            IDirectSoundFXWavesReverb_Release(r);
        }
        printf("\n");
    }
    IDirectMusicPerformance8 *pf = NULL;
    printf("  performance: %08lx\n", IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_PERFORMANCE, 0, &GUID_All_Objects, 0,
           &IID_IDirectMusicPerformance8, (void **)&pf));
    if (pf) IDirectMusicPerformance8_Release(pf);
    DWORD out = 0;
    printf("  ConvertPChannel(3): %08lx -> %lu\n", IDirectMusicAudioPath_ConvertPChannel(ap, 3, &out), out);
}

int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;
    const char *file = argc > 1 ? argv[1] : "test.mid";
    DWORD ms = argc > 2 ? atoi(argv[2]) : 4000;
    WCHAR wfile[MAX_PATH], wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, file, -1, wfile, MAX_PATH);
    GetCurrentDirectoryW(MAX_PATH, wdir);
    CoInitialize(NULL);
    IDirectMusicLoader8 *loader = NULL; IDirectMusicPerformance8 *perf = NULL; IDirectMusicSegment8 *seg = NULL;
    CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    if (FAILED(hr)) { printf("performance %08lx\n", hr); return 1; }
    hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    printf("InitAudio %08lx\n", hr);
    if (FAILED(hr)) return 1;
    IDirectMusicAudioPath *def = NULL;
    IDirectMusicPerformance8_GetDefaultAudioPath(perf, &def);
    if (def) { probe(def, "default (shared stereo + reverb)"); }
    static const DWORD types[3] = { DMUS_APATH_DYNAMIC_3D, DMUS_APATH_DYNAMIC_MONO, DMUS_APATH_DYNAMIC_STEREO };
    static const char *names[3] = { "dynamic 3D", "dynamic mono", "dynamic stereo" };
    IDirectMusicAudioPath *paths[3] = { 0 };
    for (int t = 0; t < 3; t++) {
        hr = IDirectMusicPerformance8_CreateStandardAudioPath(perf, types[t], 16, TRUE, &paths[t]);
        printf("CreateStandardAudioPath(%s) %08lx\n", names[t], hr);
        if (SUCCEEDED(hr)) probe(paths[t], names[t]);
    }
    IDirectMusicAudioPath *bad = NULL;
    printf("CreateStandardAudioPath(5) %08lx\n", IDirectMusicPerformance8_CreateStandardAudioPath(perf, 5, 16, TRUE, &bad));
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, wdir, FALSE);
    hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, wfile, (void **)&seg);
    printf("load %08lx\n", hr);
    if (FAILED(hr) || !paths[0]) return 0;
    IDirectMusicSegment8_Download(seg, (IUnknown *)paths[0]);
    IDirectSound3DBuffer8 *b3 = NULL;
    IDirectMusicAudioPath_GetObjectInPath(paths[0], DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, 0, &GUID_NULL, 0, &IID_IDirectSound3DBuffer8, (void **)&b3);
    hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)seg, NULL, NULL, 0, 0, NULL, NULL, (IUnknown *)paths[0]);
    printf("play on the 3D path %08lx\n", hr);
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        float a = (GetTickCount() - t0) / 1000.0f * 1.5f;
        if (b3) IDirectSound3DBuffer_SetPosition(b3, 5.0f * sinf(a), 0.0f, 5.0f * cosf(a), DS3D_IMMEDIATE);
        Sleep(20);
    }
    if (b3) IDirectSound3DBuffer_Release(b3);
    IDirectMusicPerformance8_StopEx(perf, (IUnknown *)seg, 0, 0);
    for (int t = 0; t < 3; t++) if (paths[t]) IDirectMusicAudioPath_Release(paths[t]);
    if (def) IDirectMusicAudioPath_Release(def);
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    printf("done\n");
    return 0;
}
