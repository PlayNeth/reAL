/* reDM: DirectMusic's music values, as dmime converts them (IDirectMusicPerformance8::MusicToMIDI 773085af,
 * MIDIToMusic 773082a6) and the chord analysis dmstyle builds on. */
#ifndef REAL_DMTHEORY_H
#define REAL_DMTHEORY_H
#include <stdint.h>

/* DMUS_SUBCHORD / DMUS_CHORD_KEY, laid out as dmusici.h's (0x14 / 0xcc bytes) */
typedef struct {
    uint32_t chord, scale, inversion, levels;   /* dwChordPattern, dwScalePattern, dwInversionPoints, dwLevels */
    uint8_t chordroot, scaleroot;               /* bChordRoot, bScaleRoot */
    uint8_t pad[2];
} dm_subchord;
typedef struct {
    uint16_t name[16];          /* +0x00 wszName */
    uint16_t measure;           /* +0x20 */
    uint8_t beat;               /* +0x22 */
    uint8_t nsub;               /* +0x23 bSubChordCount */
    dm_subchord sub[8];         /* +0x24 */
    uint32_t scale;             /* +0xc4 dwScale */
    uint8_t key;                /* +0xc8 bKey */
    uint8_t flags;              /* +0xc9 bFlags */
    uint8_t pad[2];
} dm_chord_key;

/* DMUS_S_OVER_CHORD / DMUS_S_UP_OCTAVE / DMUS_S_DOWN_OCTAVE / DMUS_E_CANNOT_CONVERT */
#define DMT_S_OVER_CHORD    0x08781212
#define DMT_S_UP_OCTAVE     0x08781213
#define DMT_S_DOWN_OCTAVE   0x08781214
#define DMT_E_CANNOT_CONVERT ((int32_t)0x88781207)

/* the subchord for a chord level (the first whose dwLevels has the level, else the first) */
const dm_subchord *dmt_subchord(const dm_chord_key *c, uint8_t level);
/* IDirectMusicPerformance::MusicToMIDI / MIDIToMusic, results and HRESULTs as dmime's (the chord is not changed:
 * MIDIToMusic's temporary edit of a subchord is made on a copy) */
int32_t dmt_music_to_midi(uint16_t mv, const dm_chord_key *c, uint8_t playmode, uint8_t level, uint8_t *midi);
int32_t dmt_midi_to_music(uint8_t midi, const dm_chord_key *c, uint8_t playmode, uint8_t level, uint16_t *mv);
/* 7730822e: a music value against one subchord (0x1000 when it falls between chord tones) */
uint32_t dmt_note_scale(uint8_t root, uint32_t scale);
int dmt_in_pattern(uint8_t midi, uint8_t root, uint32_t pattern);
int dmt_music_value(uint32_t mv, uint8_t playmode, const dm_subchord *s, uint8_t key);

#endif
