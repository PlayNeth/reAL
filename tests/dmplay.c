/* dmplay - plays a MIDI file or segment through DirectMusic, with reAL loaded (for reAL-Music); built with -DNO_DSOUND
 * (dmplay_nods.exe) it never loads dsound.dll itself, as a game that reaches reAL only through a regsvr32 registration */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <stdio.h>
#include <string.h>
#include <dsound.h>

int main(int argc, char **argv)
{
#ifndef NO_DSOUND
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;     /* load reAL like a DirectSound game */
#endif
    const char *file = argc > 1 ? argv[1] : "test.mid";
    WCHAR wfile[MAX_PATH], wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, file, -1, wfile, MAX_PATH);
    GetCurrentDirectoryW(MAX_PATH, wdir);
    CoInitialize(NULL);
    char srv[MAX_PATH] = "(not registered)"; LONG sl = sizeof(srv);
    RegQueryValueA(HKEY_CLASSES_ROOT, "CLSID\\{D2AC2881-B39B-11D1-8704-00600893B1BD}\\InprocServer32", srv, &sl);
    printf("DirectMusicPerformance server: %s%s\n", srv, strstr(srv, "dmtrace") ? "  (dmtrace is active)" : "  (dmtrace is NOT active)");
    IDirectMusicLoader8 *loader = NULL; IDirectMusicPerformance8 *perf = NULL; IDirectMusicSegment8 *seg = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    printf("loader %08lx\n", hr); if (FAILED(hr)) return 1;
    hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    printf("performance %08lx\n", hr); if (FAILED(hr)) return 1;
    hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    printf("InitAudio %08lx\n", hr); if (FAILED(hr)) return 1;
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, wdir, FALSE);
    hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, wfile, (void **)&seg);
    printf("load %s %08lx\n", file, hr); if (FAILED(hr)) return 1;
    IDirectMusicSegment8_SetParam(seg, &GUID_StandardMIDIFile, 0xffffffff, 0, 0, NULL);
    MUSIC_TIME len = 0; IDirectMusicSegment8_GetLength(seg, &len);
    printf("length %ld\n", (long)len);
    hr = IDirectMusicSegment8_Download(seg, (IUnknown *)perf);
    printf("download %08lx\n", hr);
    IDirectMusicSegmentState *st = NULL;
    hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)seg, NULL, NULL, 0, 0, &st, NULL, NULL);
    printf("play %08lx\n", hr);
    Sleep(argc > 2 ? atoi(argv[2]) : 3000);
    IDirectMusicPerformance8_StopEx(perf, (IUnknown *)seg, 0, 0);
    if (st) IDirectMusicSegmentState_Release(st);
    IDirectMusicSegment8_Unload(seg, (IUnknown *)perf);
    IDirectMusicSegment8_Release(seg);
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    IDirectMusicLoader8_Release(loader);
    CoUninitialize();
    printf("done\n");
    return 0;
}
