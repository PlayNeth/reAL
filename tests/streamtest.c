/* streamtest: midiStream on the MIDI mapper (reDM GM Synth with midi=true): two buffers, 480 PPQN, a tempo
 * change with MEVT_F_CALLBACK, position polling, pause / restart, stop, close.  Expected notes: C4 at 0 ms (500 ms
 * long), E4 at 500 ms (250 ms, tempo doubled), G4 at 750 ms, C5 after a 300 ms pause at ~1300 ms. */
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
static volatile LONG n_open, n_done, n_pos, n_close;
static void CALLBACK cb(HMIDIOUT h, UINT m, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR p2)
{
    if (m == MOM_OPEN) InterlockedIncrement(&n_open);
    if (m == MOM_DONE) InterlockedIncrement(&n_done);
    if (m == MOM_POSITIONCB) { InterlockedIncrement(&n_pos); printf("  POSITIONCB at offset %lu\n", ((MIDIHDR *)p1)->dwOffset); }
    if (m == MOM_CLOSE) InterlockedIncrement(&n_close);
}
static DWORD *ev(DWORD *p, DWORD delta, DWORD e) { *p++ = delta; *p++ = 0; *p++ = e; return p; }
static void pos(HMIDISTRM h, const char *what)
{
    MMTIME t = { TIME_MS }, k = { TIME_TICKS };
    midiStreamPosition(h, &t, sizeof(t)); midiStreamPosition(h, &k, sizeof(k));
    printf("%-14s ms %5lu ticks %5lu  done %ld\n", what, t.u.ms, k.u.ticks, n_done);
}
int main(void)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;
    HMIDISTRM h; UINT dev = MIDI_MAPPER;
    MMRESULT r = midiStreamOpen(&h, &dev, 1, (DWORD_PTR)cb, 0, CALLBACK_FUNCTION);
    printf("open %u dev %u\n", r, dev);
    if (r) return 1;
    MIDIPROPTIMEDIV td = { sizeof(td), 480 };
    printf("timediv set %u\n", midiStreamProperty(h, (LPBYTE)&td, MIDIPROP_SET | MIDIPROP_TIMEDIV));
    static DWORD b1[64], b2[64];
    DWORD *p = b1;
    p = ev(p, 0, MEVT_SHORTMSG << 24 | 0x00c0);           /* program 0 */
    p = ev(p, 0, MEVT_SHORTMSG << 24 | 0x643c90);         /* C4 on */
    p = ev(p, 480, MEVT_SHORTMSG << 24 | 0x003c80);       /* off after a quarter (500 ms) */
    p = ev(p, 0, (MEVT_TEMPO << 24) | MEVT_F_CALLBACK | 250000);   /* tempo x2 */
    p = ev(p, 0, MEVT_SHORTMSG << 24 | 0x644090);         /* E4 */
    p = ev(p, 480, MEVT_SHORTMSG << 24 | 0x004080);       /* off after a quarter (250 ms) */
    MIDIHDR h1 = { (LPSTR)b1, sizeof(b1), (DWORD)((BYTE *)p - (BYTE *)b1) };
    p = b2;
    p = ev(p, 0, MEVT_SHORTMSG << 24 | 0x644390);         /* G4 */
    p = ev(p, 480, MEVT_SHORTMSG << 24 | 0x004380);
    p = ev(p, 0, MEVT_SHORTMSG << 24 | 0x644890);         /* C5 (after the pause) */
    p = ev(p, 480, MEVT_SHORTMSG << 24 | 0x004880);
    MIDIHDR h2 = { (LPSTR)b2, sizeof(b2), (DWORD)((BYTE *)p - (BYTE *)b2) };
    printf("prepare %u %u\n", midiOutPrepareHeader((HMIDIOUT)h, &h1, sizeof(h1)), midiOutPrepareHeader((HMIDIOUT)h, &h2, sizeof(h2)));
    printf("out %u %u\n", midiStreamOut(h, &h1, sizeof(h1)), midiStreamOut(h, &h2, sizeof(h2)));
    pos(h, "before restart");
    Sleep(100);
    pos(h, "paused 100ms");
    midiStreamRestart(h);
    Sleep(250); pos(h, "250 ms");
    Sleep(500); pos(h, "750 ms");
    Sleep(200); pos(h, "950 ms");
    midiStreamPause(h);
    Sleep(300); pos(h, "paused 300");
    midiStreamRestart(h);
    Sleep(500); pos(h, "after resume");
    printf("buffers done %ld (2), flags %lx %lx\n", n_done, h1.dwFlags, h2.dwFlags);
    midiStreamStop(h);
    pos(h, "stopped");
    midiOutUnprepareHeader((HMIDIOUT)h, &h1, sizeof(h1)); midiOutUnprepareHeader((HMIDIOUT)h, &h2, sizeof(h2));
    printf("close %u\n", midiStreamClose(h));
    printf("callbacks: open %ld done %ld positioncb %ld close %ld\n", n_open, n_done, n_pos, n_close);
    return 0;
}
