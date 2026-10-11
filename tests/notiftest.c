/* notiftest - DirectMusic notifications through the COM interfaces: plays a segment or MIDI file with segment,
 * performance and measure/beat notifications requested, waits on the notification event and prints what arrives.
 * notiftest <file> [ms] */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <stdio.h>
#include <dsound.h>

int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;     /* load reAL like a DirectSound game */
    const char *file = argc > 1 ? argv[1] : "test.mid";
    DWORD ms = argc > 2 ? atoi(argv[2]) : 4000;
    WCHAR wfile[MAX_PATH], wdir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, file, -1, wfile, MAX_PATH);
    GetCurrentDirectoryW(MAX_PATH, wdir);
    CoInitialize(NULL);
    IDirectMusicLoader8 *loader = NULL; IDirectMusicPerformance8 *perf = NULL; IDirectMusicSegment8 *seg = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_DirectMusicLoader, NULL, CLSCTX_INPROC, &IID_IDirectMusicLoader8, (void **)&loader);
    if (FAILED(hr)) { printf("loader %08lx\n", hr); return 1; }
    hr = CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf);
    if (FAILED(hr)) { printf("performance %08lx\n", hr); return 1; }
    hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    if (FAILED(hr)) { printf("InitAudio %08lx\n", hr); return 1; }
    IDirectMusicLoader8_SetSearchDirectory(loader, &GUID_DirectMusicAllTypes, wdir, FALSE);
    hr = IDirectMusicLoader8_LoadObjectFromFile(loader, &CLSID_DirectMusicSegment, &IID_IDirectMusicSegment8, wfile, (void **)&seg);
    if (FAILED(hr)) { printf("load %08lx\n", hr); return 1; }
    IDirectMusicSegment8_Download(seg, (IUnknown *)perf);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    printf("SetNotificationHandle %08lx\n", IDirectMusicPerformance8_SetNotificationHandle(perf, ev, 0));
    printf("AddNotificationType segment %08lx\n", IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_SEGMENT));
    printf("AddNotificationType again %08lx\n", IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_SEGMENT));
    printf("AddNotificationType performance %08lx\n", IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_PERFORMANCE));
    printf("AddNotificationType measure %08lx\n", IDirectMusicPerformance8_AddNotificationType(perf, &GUID_NOTIFICATION_MEASUREANDBEAT));
    IDirectMusicSegmentState *st = NULL;
    hr = IDirectMusicPerformance8_PlaySegmentEx(perf, (IUnknown *)seg, NULL, NULL, 0, 0, &st, NULL, NULL);
    printf("play %08lx\n", hr);
    IUnknown *stu = NULL;
    if (st) IDirectMusicSegmentState_QueryInterface(st, &IID_IUnknown, (void **)&stu);
    DWORD t0 = GetTickCount(), n = 0, events = 0;
    while (GetTickCount() - t0 < ms) {
        if (WaitForSingleObject(ev, 100) == WAIT_OBJECT_0) events++;
        DMUS_NOTIFICATION_PMSG *m;
        while (IDirectMusicPerformance8_GetNotificationPMsg(perf, &m) == S_OK) {
            const char *ty = IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_SEGMENT) ? "SEGMENT" :
                             IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_MEASUREANDBEAT) ? "MEASUREANDBEAT" :
                             IsEqualGUID(&m->guidNotificationType, &GUID_NOTIFICATION_PERFORMANCE) ? "PERFORMANCE" : "OTHER";
            printf("%5lu ms: mt %6ld %-14s option %lu fields %lu %lu user %s size %lu type %lu\n", GetTickCount() - t0,
                   m->mtTime, ty, m->dwNotificationOption, m->dwField1, m->dwField2,
                   !m->punkUser ? "-" : m->punkUser == stu ? "state" : "other", m->dwSize, m->dwType);
            printf("     FreePMsg %08lx\n", IDirectMusicPerformance8_FreePMsg(perf, (DMUS_PMSG *)m));
            n++;
        }
    }
    printf("%lu notifications, event signalled %lu times\n", n, events);
    printf("RemoveNotificationType measure %08lx\n", IDirectMusicPerformance8_RemoveNotificationType(perf, &GUID_NOTIFICATION_MEASUREANDBEAT));
    printf("RemoveNotificationType again %08lx\n", IDirectMusicPerformance8_RemoveNotificationType(perf, &GUID_NOTIFICATION_MEASUREANDBEAT));
    IDirectMusicPerformance8_StopEx(perf, (IUnknown *)seg, 0, 0);
    if (stu) IUnknown_Release(stu);
    if (st) IDirectMusicSegmentState_Release(st);
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    printf("done\n");
    return 0;
}
