/* Runtime smoke test for reAL under Wine/Windows. Build as a 32-bit exe and run next to dsound.dll.
 * Exercises the API the way games do and prints PASS/FAIL lines. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

typedef HRESULT (WINAPI *create8_t)(LPCGUID, LPDIRECTSOUND8 *, LPUNKNOWN);

static void fill_sine(BYTE *p, DWORD bytes, DWORD rate, float hz, DWORD *phase)
{
    INT16 *s = (INT16 *)p;
    for (DWORD i = 0; i < bytes / 2; i++, (*phase)++)
        s[i] = (INT16)(sinf(2.0f * 3.14159265f * hz * (float)*phase / (float)rate) * 12000.0f);
}

int main(int argc, char **argv)
{
    const char *dll = argc > 1 ? argv[1] : ".\\dsound.dll";
    HMODULE h = LoadLibraryA(dll);
    CHECK(h != NULL, "LoadLibrary(%s)", dll);
    if (!h) return 1;
    char path[MAX_PATH];
    GetModuleFileNameA(h, path, MAX_PATH);
    printf("     loaded: %s\n", path);

    create8_t create8 = (create8_t)GetProcAddress(h, "DirectSoundCreate8");
    IDirectSound8 *ds = NULL;
    HRESULT hr = create8(NULL, &ds, NULL);
    CHECK(SUCCEEDED(hr) && ds, "DirectSoundCreate8 hr=%08lx", hr);
    if (!ds) return 1;

    hr = IDirectSound8_SetCooperativeLevel(ds, GetDesktopWindow(), DSSCL_PRIORITY);
    CHECK(hr == DS_OK, "SetCooperativeLevel");

    DSCAPS caps = { sizeof(caps) };
    hr = IDirectSound8_GetCaps(ds, &caps);
    CHECK(hr == DS_OK && caps.dwMaxHw3DAllBuffers > 0, "GetCaps hw3d=%lu", caps.dwMaxHw3DAllBuffers);

    /* primary + listener */
    DSBUFFERDESC pd = { sizeof(pd), DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME };
    IDirectSoundBuffer *prim = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &pd, &prim, NULL);
    CHECK(hr == DS_OK, "create primary");
    WAVEFORMATEX pf = { WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0 };
    CHECK(IDirectSoundBuffer_SetFormat(prim, &pf) == DS_OK, "primary SetFormat");
    IDirectSound3DListener *lst = NULL;
    hr = IDirectSoundBuffer_QueryInterface(prim, &IID_IDirectSound3DListener, (void **)&lst);
    CHECK(hr == DS_OK && lst, "QI listener");

    /* streaming 2D buffer with 4 notifications, 16-bit stereo... use mono 3D buffer for 3D test */
    const DWORD rate = 22050, size = rate * 2 / 2; /* 0.5 s mono 16-bit */
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 1, rate, rate * 2, 2, 16, 0 };
    DSBUFFERDESC bd = { sizeof(bd), DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY |
                        DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_LOCHARDWARE, size, 0, &wf };
    IDirectSoundBuffer *buf = NULL;
    hr = IDirectSound8_CreateSoundBuffer(ds, &bd, &buf, NULL);
    CHECK(hr == DS_OK, "create 3D streaming buffer");

    IDirectSoundBuffer8 *buf8 = NULL;
    CHECK(IDirectSoundBuffer_QueryInterface(buf, &IID_IDirectSoundBuffer8, (void **)&buf8) == S_OK, "QI DSB8");
    IDirectSound3DBuffer *b3d = NULL;
    CHECK(IDirectSoundBuffer_QueryInterface(buf, &IID_IDirectSound3DBuffer, (void **)&b3d) == S_OK, "QI 3D buffer");
    IDirectSoundNotify *ntf = NULL;
    CHECK(IDirectSoundBuffer_QueryInterface(buf, &IID_IDirectSoundNotify, (void **)&ntf) == S_OK, "QI notify");
    IKsPropertySet *ks = NULL;
    CHECK(IDirectSoundBuffer_QueryInterface(buf, &IID_IKsPropertySet, (void **)&ks) == S_OK, "QI IKsPropertySet");

    DSBCAPS bc = { sizeof(bc) };
    IDirectSoundBuffer_GetCaps(buf, &bc);
    CHECK(bc.dwFlags & DSBCAPS_LOCHARDWARE, "buffer reports LOCHARDWARE");

    /* stereo 3D buffer must be rejected */
    WAVEFORMATEX wf2 = { WAVE_FORMAT_PCM, 2, rate, rate * 4, 4, 16, 0 };
    DSBUFFERDESC bd2 = { sizeof(bd2), DSBCAPS_CTRL3D, 4096, 0, &wf2 };
    IDirectSoundBuffer *bad = NULL;
    CHECK(IDirectSound8_CreateSoundBuffer(ds, &bd2, &bad, NULL) == DSERR_INVALIDPARAM, "stereo 3D buffer rejected");

    HANDLE ev[4];
    DSBPOSITIONNOTIFY pn[4];
    for (int i = 0; i < 4; i++) {
        ev[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        pn[i].dwOffset = (size / 4) * i + (size / 4) - 2;
        pn[i].hEventNotify = ev[i];
    }
    CHECK(IDirectSoundNotify_SetNotificationPositions(ntf, 4, pn) == DS_OK, "SetNotificationPositions");

    /* fill whole buffer */
    DWORD phase = 0;
    void *p1, *p2; DWORD b1, b2;
    hr = IDirectSoundBuffer_Lock(buf, 0, 0, &p1, &b1, &p2, &b2, DSBLOCK_ENTIREBUFFER);
    CHECK(hr == DS_OK && b1 == size && !p2, "Lock entire buffer");
    fill_sine(p1, b1, rate, 440.0f, &phase);
    IDirectSoundBuffer_Unlock(buf, p1, b1, p2, b2);

    /* wrapping lock */
    hr = IDirectSoundBuffer_Lock(buf, size - 100, 300, &p1, &b1, &p2, &b2, 0);
    CHECK(hr == DS_OK && b1 == 100 && b2 == 200 && p2, "wrapping Lock split 100+200");
    IDirectSoundBuffer_Unlock(buf, p1, b1, p2, b2);

    IDirectSound3DListener_SetPosition(lst, 0, 0, 0, DS3D_DEFERRED);
    IDirectSound3DBuffer_SetPosition(b3d, 2.0f, 0, 1.0f, DS3D_DEFERRED);
    IDirectSound3DListener_CommitDeferredSettings(lst);
    D3DVECTOR v;
    IDirectSound3DBuffer_GetPosition(b3d, &v);
    CHECK(v.x == 2.0f && v.z == 1.0f, "3D GetPosition after commit");

    /* play and stream for ~2 seconds, refilling on notifications */
    DWORD t0 = GetTickCount();
    hr = IDirectSoundBuffer_Play(buf, 0, 0, DSBPLAY_LOOPING);
    CHECK(hr == DS_OK, "Play looping");
    DWORD st = 0;
    IDirectSoundBuffer_GetStatus(buf, &st);
    CHECK((st & DSBSTATUS_PLAYING) && (st & DSBSTATUS_LOOPING), "status PLAYING|LOOPING (%lx)", st);

    int got = 0, timeouts = 0;
    DWORD last_play = 0; int monotonic_fail = 0;
    while (GetTickCount() - t0 < 2000) {
        DWORD r = WaitForMultipleObjects(4, ev, FALSE, 500);
        if (r == WAIT_TIMEOUT) { timeouts++; continue; }
        int idx = (int)(r - WAIT_OBJECT_0);
        got++;
        /* refill the quarter before the one that just finished playing */
        DWORD off = (size / 4) * ((idx + 2) % 4);
        if (SUCCEEDED(IDirectSoundBuffer_Lock(buf, off, size / 4, &p1, &b1, &p2, &b2, 0))) {
            fill_sine(p1, b1, rate, 440.0f, &phase);
            IDirectSoundBuffer_Unlock(buf, p1, b1, p2, b2);
        }
        DWORD pl, wr;
        IDirectSoundBuffer_GetCurrentPosition(buf, &pl, &wr);
        (void)last_play; (void)monotonic_fail;
        if (got == 3) IDirectSoundBuffer_SetFrequency(buf, rate * 2);
    }
    DWORD elapsed = GetTickCount() - t0;
    /* 0.5s buffer, 4 notifies: ~8/s at 1x; we doubled speed after 3 events */
    printf("     notifications in %lu ms: %d (timeouts %d)\n", elapsed, got, timeouts);
    CHECK(got >= 14 && got <= 34 && timeouts == 0, "notification rate plausible");

    DWORD pl, wr;
    IDirectSoundBuffer_GetCurrentPosition(buf, &pl, &wr);
    CHECK(pl < size && wr < size && pl % 2 == 0, "cursors in range (play %lu write %lu)", pl, wr);

    IDirectSoundBuffer_Stop(buf);
    IDirectSoundBuffer_GetStatus(buf, &st);
    CHECK(!(st & DSBSTATUS_PLAYING), "stopped");

    /* one-shot: should stop by itself */
    WAVEFORMATEX wf8 = { WAVE_FORMAT_PCM, 1, 11025, 11025, 1, 8, 0 };
    DSBUFFERDESC sd = { sizeof(sd), DSBCAPS_CTRLPAN | DSBCAPS_CTRLVOLUME | DSBCAPS_STATIC, 2205, 0, &wf8 };
    IDirectSoundBuffer *one = NULL;
    CHECK(IDirectSound8_CreateSoundBuffer(ds, &sd, &one, NULL) == DS_OK, "create 8-bit static buffer");
    IDirectSoundBuffer_SetPan(one, -2000);
    IDirectSoundBuffer *dup = NULL;
    CHECK(IDirectSound8_DuplicateSoundBuffer(ds, one, &dup) == DS_OK, "DuplicateSoundBuffer");
    IDirectSoundBuffer_Play(one, 0, 0, 0);
    IDirectSoundBuffer_Play(dup, 0, 0, 0);
    Sleep(400);
    IDirectSoundBuffer_GetStatus(one, &st);
    CHECK(!(st & DSBSTATUS_PLAYING), "one-shot finished on its own");

    ULONG sup = 0;
    hr = IKsPropertySet_QuerySupport(ks, &IID_IDirectSound3DBuffer, 0, &sup);
    CHECK(FAILED(hr), "IKsPropertySet reports unsupported (EAX not yet implemented)");

    IDirectSoundBuffer_Release(dup);
    IDirectSoundBuffer_Release(one);
    IKsPropertySet_Release(ks);
    IDirectSoundNotify_Release(ntf);
    IDirectSound3DBuffer_Release(b3d);
    IDirectSoundBuffer8_Release(buf8);
    CHECK(IDirectSoundBuffer_Release(buf) == 0, "buffer refcount reaches 0");
    IDirectSound3DListener_Release(lst);
    IDirectSoundBuffer_Release(prim);
    CHECK(IDirectSound8_Release(ds) == 0, "device refcount reaches 0");

    /* COM path */
    CoInitialize(NULL);
    typedef HRESULT (WINAPI *gco_t)(REFCLSID, REFIID, LPVOID *);
    gco_t gco = (gco_t)GetProcAddress(h, "DllGetClassObject");
    IClassFactory *cf = NULL;
    hr = gco(&CLSID_DirectSound8, &IID_IClassFactory, (void **)&cf);
    CHECK(hr == S_OK, "DllGetClassObject(CLSID_DirectSound8)");
    if (cf) {
        IDirectSound8 *d2 = NULL;
        hr = IClassFactory_CreateInstance(cf, NULL, &IID_IDirectSound8, (void **)&d2);
        CHECK(hr == S_OK, "ClassFactory CreateInstance");
        CHECK(IDirectSound8_SetCooperativeLevel(d2, NULL, DSSCL_NORMAL) == DSERR_UNINITIALIZED, "uninitialized until Initialize");
        CHECK(IDirectSound8_Initialize(d2, NULL) == DS_OK, "Initialize");
        IDirectSound8_Release(d2);
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASSED\n", fails);
    return fails ? 1 : 0;
}
