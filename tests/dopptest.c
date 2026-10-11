/* dopptest - Doppler on a DirectMusic 3D audiopath, as Microsoft's DirectSound gives it to the synth.  Runs the cases
 * in doppler_cases.h on a DMUS_APATH_DYNAMIC_3D buffer and its listener and prints, for each, the frequency and the
 * pitch offset dsound passed to the synth sink.  Needs Windows 7's dsound.dll (6.1.7600.16385): the offset is read
 * by patching its sink-buffer frequency function (6efd6bbe, at the store of the result). */
#define COBJMACROS
#include <windows.h>
#include <dmusici.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include "doppler_cases.h"

static volatile LONG g_freq = -1, g_cents, g_calls;
static void __cdecl on_pitch(LONG freq, LONG cents) { g_freq = freq; g_cents = cents; InterlockedIncrement(&g_calls); }

static int hook_dsound(void)
{
    static const BYTE site[9] = { 0x8b, 0x4e, 0x0c, 0x89, 0x81, 0xb8, 0x00, 0x00, 0x00 };
    BYTE *base = (BYTE *)GetModuleHandleA("dsound.dll");
    if (!base) return 0;
    BYTE *at = base + 0x56c2d;
    if (memcmp(at, site, sizeof(site))) return 0;
    BYTE *stub = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    BYTE *p = stub;
    *p++ = 0x60;                                            /* pushad */
    *p++ = 0x50;                                            /* push eax (cents) */
    *p++ = 0xff; *p++ = 0x75; *p++ = 0x0c;                  /* push [ebp+0xc] (frequency) */
    *p++ = 0xb8; *(DWORD *)p = (DWORD)(DWORD_PTR)on_pitch; p += 4;
    *p++ = 0xff; *p++ = 0xd0;                               /* call eax */
    *p++ = 0x83; *p++ = 0xc4; *p++ = 0x08;                  /* add esp, 8 */
    *p++ = 0x61;                                            /* popad */
    memcpy(p, site, sizeof(site)); p += sizeof(site);       /* the instructions replaced */
    *p++ = 0xc3;
    DWORD old;
    VirtualProtect(at, 9, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xe8; *(LONG *)(at + 1) = (LONG)(stub - (at + 5));
    memset(at + 5, 0x90, 4);
    VirtualProtect(at, 9, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 9);
    return 1;
}

int main(int argc, char **argv)
{
    /* "53" or "24": run the cases with the x87 at that precision (the offset is computed on the calling thread; MSVC
     * programs run at 53 bits, Direct3D drops to 24, MinGW's default is 64) */
    unsigned pc = argc > 1 ? (unsigned)atoi(argv[1]) : 0;
    CoInitialize(NULL);
    IDirectMusicPerformance8 *perf;
    if (FAILED(CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&perf))) {
        printf("no performance\n"); return 1;
    }
    HRESULT hr = IDirectMusicPerformance8_InitAudio(perf, NULL, NULL, NULL, DMUS_APATH_SHARED_STEREOPLUSREVERB, 64, DMUS_AUDIOF_ALL, NULL);
    if (FAILED(hr)) { printf("InitAudio %08lx\n", hr); return 1; }
    IDirectMusicAudioPath *ap;
    hr = IDirectMusicPerformance8_CreateStandardAudioPath(perf, DMUS_APATH_DYNAMIC_3D, 16, TRUE, &ap);
    if (FAILED(hr)) { printf("CreateStandardAudioPath %08lx\n", hr); return 1; }
    IDirectSoundBuffer8 *b; IDirectSound3DBuffer8 *b3; IDirectSound3DListener8 *l;
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, DMUS_PCHANNEL_ALL, DMUS_PATH_BUFFER, 0, &GUID_All_Objects, 0, &IID_IDirectSoundBuffer8, (void **)&b);
    if (FAILED(hr)) { printf("buffer %08lx\n", hr); return 1; }
    IDirectSoundBuffer8_QueryInterface(b, &IID_IDirectSound3DBuffer8, (void **)&b3);
    hr = IDirectMusicAudioPath_GetObjectInPath(ap, 0, DMUS_PATH_PRIMARY_BUFFER, 0, &GUID_All_Objects, 0, &IID_IDirectSound3DListener8, (void **)&l);
    if (FAILED(hr)) { printf("listener %08lx\n", hr); return 1; }
    int hooked = hook_dsound();
    WAVEFORMATEX wf; DWORD got;
    IDirectSoundBuffer8_GetFormat(b, &wf, sizeof(wf), &got);
    printf("# rate %lu hooked %d\n", wf.nSamplesPerSec, hooked);

    if (pc == 53) _controlfp(_PC_53, _MCW_PC);
    if (pc == 24) _controlfp(_PC_24, _MCW_PC);
    for (unsigned i = 0; i < DOP_NCASES; i++) {
        const dop_case *c = &dop_cases[i];
        IDirectSound3DListener_SetDopplerFactor(l, c->ldoppler, DS3D_DEFERRED);
        IDirectSound3DListener_SetDistanceFactor(l, c->ldist, DS3D_DEFERRED);
        IDirectSound3DListener_SetPosition(l, c->lpos[0], c->lpos[1], c->lpos[2], DS3D_DEFERRED);
        IDirectSound3DListener_SetVelocity(l, c->lvel[0], c->lvel[1], c->lvel[2], DS3D_DEFERRED);
        IDirectSound3DBuffer_SetMode(b3, c->mode, DS3D_DEFERRED);
        IDirectSound3DBuffer_SetPosition(b3, c->bpos[0], c->bpos[1], c->bpos[2], DS3D_DEFERRED);
        IDirectSound3DBuffer_SetVelocity(b3, c->bvel[0], c->bvel[1], c->bvel[2], DS3D_DEFERRED);
        IDirectSoundBuffer8_SetFrequency(b, c->freq);
        IDirectSound3DListener_CommitDeferredSettings(l);
        Sleep(30);
        DWORD f = 0;
        IDirectSoundBuffer8_GetFrequency(b, &f);
        printf("case %u freq %ld cents %ld\n", i, g_freq, g_cents);
    }
    IDirectSound3DListener_Release(l);
    IDirectSound3DBuffer_Release(b3);
    IDirectSoundBuffer8_Release(b);
    IDirectMusicAudioPath_Release(ap);
    IDirectMusicPerformance8_CloseDown(perf);
    IDirectMusicPerformance8_Release(perf);
    CoUninitialize();
    return 0;
}
