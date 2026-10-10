/* miditest [file.mid] [ms] [mode]: plays notes through midiOut (twice, reopening) and a MIDI file through the MCI
 * sequencer (for the winmm hooks).  mode: 1 = midiOut only, 2 = MCI only */
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
static void notes(int prog)
{
    HMIDIOUT h;
    MMRESULT r = midiOutOpen(&h, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
    printf("open %u\n", r);
    if (r) return;
    midiOutShortMsg(h, 0x000000c0 | (prog << 8));
    for (int i = 0; i < 3; i++) {
        midiOutShortMsg(h, 0x00643c90 + (i << 10)); Sleep(250); midiOutShortMsg(h, 0x00003c80 + (i << 10));
    }
    midiOutReset(h); midiOutClose(h);
}
int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;    /* load dsound.dll (reAL) like a game */
    int mode = argc > 3 ? atoi(argv[3]) : 0;
    printf("devs %u\n", midiOutGetNumDevs());
    if (mode != 2) { notes(0x1a); Sleep(200); notes(0); Sleep(200); }
    if (mode == 1) return 0;
    char cmd[512], ret[128];
    snprintf(cmd, sizeof(cmd), "open \"%s\" type sequencer alias mus", argc > 1 ? argv[1] : "test.mid");
    printf("mci open %lu\n", mciSendStringA(cmd, ret, sizeof(ret), NULL));
    printf("mci play %lu\n", mciSendStringA("play mus", ret, sizeof(ret), NULL));
    Sleep(argc > 2 ? atoi(argv[2]) : 1500);
    mciSendStringA("close mus", ret, sizeof(ret), NULL);
    return 0;
}
