/* theorytest [detail]: IDirectMusicPerformance8::MusicToMIDI and MIDIToMusic over many chords, play modes and values,
 * printed as one checksum per chord and play mode (detail=<chord index>: every result of that chord). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <dmusici.h>

static unsigned rnd = 12345;
static unsigned rnum(void) { rnd = rnd * 1103515245 + 12345; return (rnd >> 8) & 0xffffff; }

int main(int argc, char **argv)
{
    int detail = argc > 1 ? atoi(argv[1]) : -1;
    LoadLibraryA("dsound.dll");
    CoInitialize(NULL);
    IDirectMusicPerformance8 *pf;
    if (FAILED(CoCreateInstance(&CLSID_DirectMusicPerformance, NULL, CLSCTX_INPROC, &IID_IDirectMusicPerformance8, (void **)&pf))) return 1;
    static const DWORD chords[] = { 0x91, 0x89, 0x891, 0x491, 0x49, 0x111, 0x2491, 0x4891, 0x1, 0x0, 0x95, 0x291, 0x8091,
                                    0x91091, 0xa1, 0x881, 0x24489, 0x891891, 0x1001 };
    static const DWORD scales[] = { 0xab5ab5, 0x5ab5ab, 0xad5ad5, 0x0, 0x891891, 0x555555, 0xfff, 0x2a5, 0xab5ad5 };
    static const BYTE modes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0x10, 0x15 };
    for (int ci = 0; ci < 120; ci++) {
        DMUS_CHORD_KEY k;
        memset(&k, 0, sizeof(k));
        if (ci < 40) {
            k.bSubChordCount = 1;
            k.SubChordList[0].dwChordPattern = chords[ci % 19];
            k.SubChordList[0].dwScalePattern = scales[ci % 9];
            k.SubChordList[0].dwInversionPoints = 0xffffff;
            k.SubChordList[0].dwLevels = 0xffffffff;
            k.SubChordList[0].bChordRoot = (BYTE)(ci * 5 % 24);
            k.SubChordList[0].bScaleRoot = (BYTE)(ci * 7 % 12);
            k.dwScale = scales[(ci + 3) % 9];
            k.bKey = (BYTE)(ci * 3 % 24);
        } else {
            k.bSubChordCount = (BYTE)(1 + rnum() % 4);
            for (int s = 0; s < 4; s++) {
                k.SubChordList[s].dwChordPattern = rnum() % 3 ? chords[rnum() % 19] : rnum();
                k.SubChordList[s].dwScalePattern = rnum() % 3 ? scales[rnum() % 9] : rnum();
                k.SubChordList[s].dwInversionPoints = rnum();
                k.SubChordList[s].dwLevels = rnum() % 2 ? 1u << (rnum() % 4) : rnum();
                k.SubChordList[s].bChordRoot = (BYTE)(rnum() % 30);
                k.SubChordList[s].bScaleRoot = (BYTE)(rnum() % 30);
            }
            k.dwScale = rnum();
            k.bKey = (BYTE)(rnum() % 30);
        }
        for (int mi = 0; mi < (int)sizeof(modes); mi++) {
            unsigned h1 = 0, h2 = 0;
            for (int lv = 0; lv < 4; lv++) {
                for (unsigned v = 0; v < 0x10000; v += (ci < 40 ? 1 : 7)) {
                    BYTE o = 0xee;
                    HRESULT hr = IDirectMusicPerformance8_MusicToMIDI(pf, (WORD)v, &k, modes[mi], (BYTE)lv, &o);
                    h1 = h1 * 31 + (unsigned)hr * 7 + o;
                    if (detail == ci) printf("m2m c%d m%x l%d v%04x: %08lx %02x\n", ci, modes[mi], lv, v, hr, o);
                }
                for (unsigned n = 0; n < 0x100; n++) {
                    WORD o = 0xeeee;
                    HRESULT hr = IDirectMusicPerformance8_MIDIToMusic(pf, (BYTE)n, &k, modes[mi], (BYTE)lv, &o);
                    h2 = h2 * 31 + (unsigned)hr * 7 + o;
                    if (detail == ci) printf("m2u c%d m%x l%d n%02x: %08lx %04x\n", ci, modes[mi], lv, n, hr, o);
                }
            }
            if (detail < 0) printf("chord %d mode %x: %08x %08x\n", ci, modes[mi], h1, h2);
        }
    }
    return 0;
}
