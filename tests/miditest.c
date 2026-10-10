/* miditest [file.mid]: plays notes through midiOut and a MIDI file through the MCI sequencer (for the winmm hooks) */
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
int main(int argc, char **argv)
{
    void *volatile keep = (void *)DirectSoundCreate; (void)keep;    /* load dsound.dll (reAL) like a game */
    printf("devs %u\n", midiOutGetNumDevs());
    HMIDIOUT h;
    MMRESULT r = midiOutOpen(&h, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
    printf("open %u\n", r);
    if (!r) {
        midiOutShortMsg(h, 0x00001ac0); midiOutShortMsg(h, 0x00643c90); Sleep(300); midiOutShortMsg(h, 0x00003c80);
        midiOutReset(h); midiOutClose(h);
    }
    char cmd[512], ret[128];
    snprintf(cmd, sizeof(cmd), "open \"%s\" type sequencer alias mus", argc > 1 ? argv[1] : "test.mid");
    printf("mci open %lu\n", mciSendStringA(cmd, ret, sizeof(ret), NULL));
    printf("mci play %lu\n", mciSendStringA("play mus", ret, sizeof(ret), NULL));
    Sleep(1500);
    mciSendStringA("close mus", ret, sizeof(ret), NULL);
    return 0;
}
