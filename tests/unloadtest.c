/* unloadtest - a game that loads and frees dsound.dll for each sound setting (Need for Speed III switching between
 * mono, stereo and EAX in its menu), releasing DirectSound each time but keeping a buffer (and its primary buffer),
 * with an exception it handles itself after each unload.  Each cycle then uses the buffers the last one kept and
 * lets them go.  Anything reAL left registered or running after the FreeLibrary (an exception handler, a thread) is
 * called into unmapped memory; a kept buffer must not keep the device (each cycle opens and shuts down its own).
 * reAL stays loaded once it has run a device, which is what makes the kept buffers usable after the FreeLibrary: a
 * dsound.dll that really unloads (Wine's) crashes on them, as Windows' would.
 * unloadtest.exe [cycles]: prints ok at the end, crashes otherwise. */
#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *create_fn)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

static LONG CALLBACK handled(EXCEPTION_POINTERS *ep)
{
    return ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

/* what a game may still do with a buffer it kept after its DirectSound object went (it plays no more) */
static void use_leftover(IDirectSoundBuffer *b, IDirectSoundBuffer *prim)
{
    DWORD st = 0, pc = 0, wc = 0, n1 = 0, n2 = 0;
    void *p1 = NULL, *p2 = NULL;
    IDirectSoundBuffer_SetVolume(b, -600);
    IDirectSoundBuffer_SetFrequency(b, 11025);
    IDirectSoundBuffer_GetStatus(b, &st);
    IDirectSoundBuffer_GetCurrentPosition(b, &pc, &wc);
    if (SUCCEEDED(IDirectSoundBuffer_Lock(b, 0, 1024, &p1, &n1, &p2, &n2, 0))) {
        if (p1) memset(p1, 0, n1);
        IDirectSoundBuffer_Unlock(b, p1, n1, p2, n2);
    }
    IDirectSoundBuffer_Play(b, 0, 0, 0);
    IDirectSound3DBuffer *b3 = NULL;
    if (SUCCEEDED(IDirectSoundBuffer_QueryInterface(b, &IID_IDirectSound3DBuffer, (void **)&b3)) && b3) {
        IDirectSound3DBuffer_SetPosition(b3, 1.0f, 0.0f, 2.0f, DS3D_IMMEDIATE);
        IDirectSound3DBuffer_Release(b3);
    }
    if (prim) {
        IDirectSound3DListener *l = NULL;
        if (SUCCEEDED(IDirectSoundBuffer_QueryInterface(prim, &IID_IDirectSound3DListener, (void **)&l)) && l) {
            IDirectSound3DListener_SetPosition(l, 0.0f, 1.0f, 0.0f, DS3D_DEFERRED);
            IDirectSound3DListener_CommitDeferredSettings(l);
            IDirectSound3DListener_Release(l);
        }
        IDirectSoundBuffer_GetStatus(prim, &st);
    }
}

int main(int argc, char **argv)
{
    int cycles = argc > 1 ? atoi(argv[1]) : 13;
    IDirectSoundBuffer *left = NULL, *leftprim = NULL;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 0; i < cycles; i++) {
        HMODULE h = LoadLibraryA("dsound.dll");
        if (left) {                     /* the last cycle's buffers, its DirectSound gone, then let go */
            use_leftover(left, leftprim);
            IDirectSoundBuffer_Release(left);
            if (leftprim) IDirectSoundBuffer_Release(leftprim);
            left = leftprim = NULL;
        }
        create_fn create = h ? (create_fn)GetProcAddress(h, "DirectSoundCreate") : NULL;
        if (!create) { printf("no dsound\n"); return 1; }
        IDirectSound *ds = NULL;
        if (SUCCEEDED(create(NULL, &ds, NULL))) {
            IDirectSound_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
            DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D, 0, 0, NULL };
            IDirectSoundBuffer *prim = NULL;
            IDirectSound_CreateSoundBuffer(ds, &pd, &prim, NULL);
            WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)(i % 3 == 0 ? 1 : 2), 22050, 0, 0, 16, 0 };
            wf.nBlockAlign = wf.nChannels * 2; wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            DSBUFFERDESC d = { sizeof(d), DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY |
                               (wf.nChannels == 1 ? DSBCAPS_CTRL3D : 0), 35264, 0, &wf };
            IDirectSoundBuffer *b = NULL;
            IDirectSound_CreateSoundBuffer(ds, &d, &b, NULL);
            if (b) IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
            Sleep(50);
            IDirectSound_Release(ds);       /* (the buffer left referenced, as the game leaves one; the primary too) */
            left = b; leftprim = prim;
        }
        FreeLibrary(h);
        /* an exception the game handles itself (its own vectored handler, registered after reAL's): every handler
         * registered before it is called first */
        PVOID mine = AddVectoredExceptionHandler(0, handled);
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, NULL);
        RemoveVectoredExceptionHandler(mine);
        printf("cycle %d survived\n", i + 1);
    }
    printf("ok\n");
    return 0;
}
