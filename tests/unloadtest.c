/* unloadtest - a game that loads and frees dsound.dll for each sound setting (Need for Speed III switching between
 * mono, stereo and EAX in its menu), with an exception it handles itself after each unload: anything reAL leaves
 * registered in the process after FreeLibrary (an exception handler, a hook, a thread) is called into unmapped
 * memory then.  unloadtest.exe [cycles]: prints ok at the end, crashes otherwise. */
#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>

typedef HRESULT (WINAPI *create_fn)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

static LONG CALLBACK handled(EXCEPTION_POINTERS *ep)
{
    return ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char **argv)
{
    int cycles = argc > 1 ? atoi(argv[1]) : 13;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 0; i < cycles; i++) {
        HMODULE h = LoadLibraryA("dsound.dll");
        create_fn create = h ? (create_fn)GetProcAddress(h, "DirectSoundCreate") : NULL;
        if (!create) { printf("no dsound\n"); return 1; }
        IDirectSound *ds = NULL;
        if (SUCCEEDED(create(NULL, &ds, NULL))) {
            IDirectSound_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
            WAVEFORMATEX wf = { WAVE_FORMAT_PCM, (WORD)(i % 3 == 0 ? 1 : 2), 22050, 0, 0, 16, 0 };
            wf.nBlockAlign = wf.nChannels * 2; wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
            DSBUFFERDESC d = { sizeof(d), DSBCAPS_GETCURRENTPOSITION2, 35264, 0, &wf };
            IDirectSoundBuffer *b = NULL;
            IDirectSound_CreateSoundBuffer(ds, &d, &b, NULL);
            if (b) IDirectSoundBuffer_Play(b, 0, 0, DSBPLAY_LOOPING);
            Sleep(50);
            IDirectSound_Release(ds);       /* (the buffer left referenced, as the game leaves one) */
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
