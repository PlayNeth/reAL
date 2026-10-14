/* mc2sim <dir> <segment.sgt> [ms] [segment2.sgt]: drives DirectMusic the way Midnight Club II does (shared
 * stereo+reverb InitAudio, four standard audiopaths, SetSearchDirectory + LoadObjectFromFile, Download,
 * PlaySegmentEx 0x280 on an audiopath, volume changes, IsPlaying polling, Stop + StopEx x2, CloseDown).
 * Run next to reAL's dsound.dll with music=true to exercise reDM. */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>

static IDirectMusicSegment8 *load(IDirectMusicLoader8 *ld, const char *name)
{
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH);
    IDirectMusicSegment8 *s = NULL;
    HRESULT hr = IDirectMusicLoader8_LoadObjectFromFile(ld, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, w, (void **)&s);
    printf("LoadObjectFromFile(%s) %08lx\n", name, hr);
    return SUCCEEDED(hr) ? s : NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: mc2sim dir seg.sgt [ms] [seg2.sgt]\n"); return 2; }
    int ms = argc > 3 ? atoi(argv[3]) : 8000;
    void *volatile keep = (void *)DirectSoundCreate8;    /* import dsound.dll statically, as a DirectSound game does */
    (void)keep;
    CoInitialize(NULL);
    IDirectMusicLoader8 *ld = NULL; IDirectMusicPerformance8 *pf = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf);
    printf("performance %08lx\n", hr); if (FAILED(hr)) return 1;
    hr = IDirectMusicPerformance8_InitAudio(pf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 128, 32, NULL);
    printf("InitAudio %08lx\n", hr); if (FAILED(hr)) return 1;
    IDirectMusicAudioPath *ap[4];
    for (int i = 0; i < 4; i++) {
        hr = IDirectMusicPerformance8_CreateStandardAudioPath(pf, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, TRUE, &ap[i]);
        printf("CreateStandardAudioPath %08lx\n", hr); if (FAILED(hr)) return 1;
    }
    hr = CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&ld);
    printf("loader %08lx\n", hr); if (FAILED(hr)) return 1;
    WCHAR wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, wdir, MAX_PATH);
    hr = IDirectMusicLoader8_SetSearchDirectory(ld, &GUID_DirectMusicAllTypes, wdir, FALSE);
    printf("SetSearchDirectory %08lx\n", hr);
    for (int k = 2; k < argc; k += (k == 2 ? 2 : 1)) {
        IDirectMusicSegment8 *seg = load(ld, argv[k]);
        if (!seg) return 1;
        hr = IDirectMusicSegment8_Download(seg, (IUnknown *)pf);
        printf("Download %08lx\n", hr);
        IDirectMusicSegmentState *ss = NULL;
        IDirectMusicAudioPath *p = ap[k == 2 ? 1 : 2];
        IDirectMusicAudioPath_SetVolume(p, -600, 0);
        hr = IDirectMusicPerformance8_PlaySegmentEx(pf, (IUnknown *)seg, NULL, NULL, 0x280, 0, &ss, NULL, (IUnknown *)p);
        printf("PlaySegmentEx %08lx\n", hr);
        DWORD t0 = GetTickCount();
        int fade = 0;
        while (GetTickCount() - t0 < (DWORD)ms) {
            Sleep(50);
            HRESULT pl = IDirectMusicPerformance8_IsPlaying(pf, (IDirectMusicSegment *)seg, NULL);
            DWORD el = GetTickCount() - t0;
            if (!fade && el > (DWORD)ms / 2) { IDirectMusicAudioPath_SetVolume(p, -2000, 1000); fade = 1; printf("fade at %lu, playing %08lx\n", el, pl); }
            if (fade == 1 && el > (DWORD)ms * 3 / 4) { IDirectMusicAudioPath_SetVolume(p, 0, 0); fade = 2; }
        }
        MUSIC_TIME mt; REFERENCE_TIME rt;
        IDirectMusicPerformance8_GetTime(pf, &rt, &mt);
        printf("time %ld, playing %08lx\n", (long)mt, IDirectMusicPerformance8_IsPlaying(pf, (IDirectMusicSegment *)seg, NULL));
        IDirectMusicPerformance8_Stop(pf, (IDirectMusicSegment *)seg, ss, 0, 0);
        IDirectMusicPerformance8_StopEx(pf, (IUnknown *)seg, 0, 0);
        IDirectMusicPerformance8_StopEx(pf, (IUnknown *)ss, 0, 0);
        Sleep(300);
        printf("after stop: playing %08lx\n", IDirectMusicPerformance8_IsPlaying(pf, (IDirectMusicSegment *)seg, NULL));
        IDirectMusicSegmentState_Release(ss);
        IDirectMusicLoader8_ReleaseObjectByUnknown(ld, (IUnknown *)seg);
        IDirectMusicSegment8_Unload(seg, (IUnknown *)pf);
        IDirectMusicSegment8_Release(seg);
    }
    for (int i = 0; i < 4; i++) IDirectMusicAudioPath_Release(ap[i]);
    IDirectMusicPerformance8_CloseDown(pf);
    IDirectMusicPerformance8_Release(pf);
    IDirectMusicLoader8_Release(ld);
    CoUninitialize();
    printf("done\n");
    return 0;
}
