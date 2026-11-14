/* wotest: a waveOut game in miniature, for reAL's winmm.dll proxy.
 * 11025 Hz 8-bit mono (Pajama Sam 2's format), 8 buffers of 1/10 s queued, a reset every
 * ~100 ms with the mix written again, pause/restart, position queries, then close.
 * Checks: every header comes back WHDR_DONE with a WOM_DONE callback, callbacks never on the
 * game's own thread and with the right handle, no call hangs, and the clock moves forward.
 * Run it with reAL's winmm.dll and dsound.dll beside it: WINEDLLOVERRIDES="winmm,dsound=n,b" wine wotest.exe.
 * Through reAL all 12 checks pass; on Wine's own winmm (or waveout=false) the position moves while paused and a
 * callback runs on the caller's thread. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

#define NB 8
#define LEN 1102
static HWAVEOUT g_wo;
static volatile LONG g_open, g_done, g_close, g_badh, g_mainthread;
static DWORD g_main;
static WAVEHDR hdr[NB];
static BYTE buf[NB][LEN];
static int fails;

static void CALLBACK cb(HWAVEOUT h, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    (void)p1; (void)p2;
    if (inst != 0x1234) InterlockedIncrement(&g_badh);
    if (GetCurrentThreadId() == g_main) InterlockedIncrement(&g_mainthread);
    if (msg == WOM_OPEN) InterlockedIncrement(&g_open);
    else if (msg == WOM_DONE) InterlockedIncrement(&g_done);
    else if (msg == WOM_CLOSE) InterlockedIncrement(&g_close);
    if (g_wo && h != g_wo && msg != WOM_OPEN) InterlockedIncrement(&g_badh);
}

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static DWORD pos_samples(void)
{
    MMTIME t; t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(g_wo, &t, sizeof t) != MMSYSERR_NOERROR) return 0xffffffff;
    return t.u.sample;
}

static void fill(int i, int phase)
{
    for (int k = 0; k < LEN; k++) buf[i][k] = (BYTE)(128 + (((k + phase) / 25) & 1 ? 40 : -40));
}

int main(void)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 1, 11025, 11025, 1, 8, 0 };
    g_main = GetCurrentThreadId();
    HMODULE w = GetModuleHandleA("winmm.dll");
    char path[MAX_PATH] = "";
    if (w) GetModuleFileNameA(w, path, sizeof path);
    printf("winmm: %s\n", path);
    HMODULE ds = GetModuleHandleA("dsound.dll");
    printf("dsound loaded before open: %s\n", ds ? "yes" : "no");

    MMRESULT r = waveOutOpen(&g_wo, WAVE_MAPPER, &f, (DWORD_PTR)cb, 0x1234, CALLBACK_FUNCTION);
    check(r == MMSYSERR_NOERROR, "waveOutOpen 11025 Hz 8-bit mono");
    if (r) return 1;
    ds = GetModuleHandleA("dsound.dll");
    if (ds) {
        GetModuleFileNameA(ds, path, sizeof path);
        printf("dsound after open: %s (reAL_winmm_bind %s)\n", path,
               GetProcAddress(ds, "reAL_winmm_bind") ? "present" : "absent");
    }
    Sleep(50);
    check(g_open == 1, "one WOM_OPEN");

    for (int i = 0; i < NB; i++) {
        fill(i, i * LEN);
        hdr[i].lpData = (LPSTR)buf[i]; hdr[i].dwBufferLength = LEN; hdr[i].dwFlags = 0;
        waveOutPrepareHeader(g_wo, &hdr[i], sizeof hdr[i]);
    }
    /* 1: play the queue through */
    for (int i = 0; i < NB; i++) waveOutWrite(g_wo, &hdr[i], sizeof hdr[i]);
    DWORD p0 = pos_samples(); Sleep(300); DWORD p1 = pos_samples();
    check(p1 != 0xffffffff && p1 > p0, "position moves while playing");
    DWORD t0 = GetTickCount();
    while (g_done < NB && GetTickCount() - t0 < 3000) Sleep(10);
    check(g_done == NB, "all 8 buffers done");
    int alldone = 1; for (int i = 0; i < NB; i++) if (!(hdr[i].dwFlags & WHDR_DONE)) alldone = 0;
    check(alldone, "every header flagged WHDR_DONE");

    /* 2: the Pajama Sam pattern: reset every ~100 ms and write the mix again */
    LONG before = g_done; int queued = 0;
    for (int round = 0; round < 30; round++) {
        DWORD a = GetTickCount();
        r = waveOutReset(g_wo);
        if (GetTickCount() - a > 500) { check(0, "waveOutReset returns promptly"); break; }
        for (int i = 0; i < 3; i++) {
            int b = (round * 3 + i) % NB;
            fill(b, round * 999 + i * LEN);
            waveOutWrite(g_wo, &hdr[b], sizeof hdr[b]);
            queued++;
        }
        Sleep(100);
    }
    waveOutReset(g_wo);
    t0 = GetTickCount();
    while (g_done - before < queued && GetTickCount() - t0 < 2000) Sleep(10);
    check(g_done - before == queued, "every buffer of the reset rounds returned (WOM_DONE)");

    /* 3: pause / restart */
    for (int i = 0; i < 4; i++) waveOutWrite(g_wo, &hdr[i], sizeof hdr[i]);
    waveOutPause(g_wo); DWORD q0 = pos_samples(); Sleep(200); DWORD q1 = pos_samples();
    check(q1 == q0, "position holds while paused");
    waveOutRestart(g_wo); Sleep(200); DWORD q2 = pos_samples();
    check(q2 > q1, "position moves after restart");
    waveOutReset(g_wo);

    for (int i = 0; i < NB; i++) waveOutUnprepareHeader(g_wo, &hdr[i], sizeof hdr[i]);
    r = waveOutClose(g_wo);
    check(r == MMSYSERR_NOERROR, "waveOutClose");
    Sleep(50);
    check(g_close == 1, "one WOM_CLOSE");
    check(g_badh == 0, "callbacks carry the right handle and instance");
    check(g_mainthread == 0, "no callback on the game's own thread");
    printf("== %d failure(s)\n", fails);
    return fails != 0;
}
