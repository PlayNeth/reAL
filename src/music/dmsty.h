/* reAL-Music: the style engine's data (dmstyle.dll, XP 5.3.2600.5512): the chord, command and mute tracks, kept the
 * way dmstyle keeps them (lists in its order, its merge sort).  Notes in docs/dmstyle_notes.md. */
#ifndef REAL_DMSTY_H
#define REAL_DMSTY_H
#include "dmi.h"
#include "dmtheory.h"

/* ------------------------------------------------------------------ chord track (CChordTrack 77402b98, 0x3c) */
typedef struct dms_chord {          /* DMChord (0x20) */
    struct dms_chord *next;         /* +0x1c */
    uint16_t name[21];              /* +0x00 the name string (up to the first 0 in the 40 bytes read) */
    mtime_t t;                      /* +0x08 */
    uint16_t measure;               /* +0x0c */
    uint8_t beat;                   /* +0x0e */
    uint8_t key;                    /* +0x0f the track's root */
    uint32_t scale;                 /* +0x10 the track's scale */
    uint8_t silent;                 /* +0x14 bFlags & DMUS_CHORDKEYF_SILENT */
    dm_subchord *sub; uint32_t nsub;    /* +0x18 in file order */
} dms_chord;
typedef struct {
    dms_chord *chords;              /* +0x0c, sorted by measure and beat */
    uint8_t root;                   /* +0x10 */
    uint32_t scale;                 /* +0x14 */
    int notify;                     /* +0x38 GUID_NOTIFICATION_CHORD */
} dms_chordtrack;

/* ------------------------------------------------------------------ command track (CCommandTrack 77403b95, 0x38) */
typedef struct dms_command {        /* 0x10 */
    struct dms_command *next;       /* +0x0c */
    mtime_t t;                      /* +0x00 */
    uint16_t measure;               /* +0x04 */
    uint8_t beat, command, groove, range, repeat;   /* +0x06 .. +0x0a */
} dms_command;
typedef struct {
    dms_command *cmds;              /* +0x0c, sorted by measure */
    int notify;                     /* +0x34 GUID_NOTIFICATION_COMMAND */
    int hasnext; uint8_t next[8];   /* +0x2c GUID_CommandParamNext (a DMUS_COMMAND_PARAM_2) */
} dms_cmdtrack;

/* ------------------------------------------------------------------ mute track (CMuteTrack 774159e1, 0x30) */
typedef struct dms_muteitem { struct dms_muteitem *next; mtime_t t; uint32_t map; uint32_t mute; } dms_muteitem;
typedef struct dms_mutepch { struct dms_mutepch *next; uint32_t pch; dms_muteitem *items; } dms_mutepch;
typedef struct { dms_mutepch *pchs; } dms_mutetrack;

/* DMUS_COMMAND_PARAM / DMUS_COMMAND_PARAM_2 / DMUS_RHYTHM_PARAM / DMUS_MUTE_PARAM */
typedef struct { uint8_t command, groove, range, repeat; } dms_cmdparam;
typedef struct { mtime_t t; uint8_t command, groove, range, repeat; } dms_cmdparam2;
typedef struct { mtime_t t; uint8_t bpm, beat; uint16_t grids; uint32_t rhythm; } dms_rhythmparam;
typedef struct { uint32_t pch, map; int32_t mute; } dms_muteparam;

/* loading: the data of the track's chunk or list (what IPersistStream::Load reads); NULL when Load fails */
dms_chordtrack *dms_chordtrack_load(const uint8_t *p, size_t n, int32_t *hr);
dms_cmdtrack *dms_cmdtrack_load(const uint8_t *p, size_t n, int32_t *hr);
dms_mutetrack *dms_mutetrack_load(const uint8_t *p, size_t n, int32_t *hr);
void dms_chordtrack_free(dms_chordtrack *T);
void dms_cmdtrack_free(dms_cmdtrack *T);
void dms_mutetrack_free(dms_mutetrack *T);
/* Clone [start, end) */
dms_chordtrack *dms_chordtrack_clone(const dms_chordtrack *T, mtime_t start, mtime_t end);
dms_cmdtrack *dms_cmdtrack_clone(const dms_cmdtrack *T, mtime_t start, mtime_t end);
dms_mutetrack *dms_mutetrack_clone(const dms_mutetrack *T, mtime_t start, mtime_t end);
/* 774045b5: a chord as DMUS_CHORD_KEY */
void dms_chord_key(const dms_chord *c, dm_chord_key *k);
/* the track parameters (GetParam / SetParam) */
int dms_chord_getparam(const dms_chordtrack *T, int rhythm, mtime_t t, mtime_t *next, void *data);
int dms_chord_setparam(dms_chordtrack *T, mtime_t t, const dm_chord_key *k);
int dms_cmd_getparam(dms_cmdtrack *T, int which, mtime_t t, mtime_t *next, void *data);   /* 0 param, 2 param2, 1 next */
int dms_cmd_setparam(dms_cmdtrack *T, int which, mtime_t t, const void *data);
int dms_mute_getparam(const dms_mutetrack *T, mtime_t t, mtime_t *next, dms_muteparam *m);
int dms_mute_setparam(dms_mutetrack *T, mtime_t t, const dms_muteparam *m);

/* dmstyle's list merge sort (77402021 split, 77402056 merge, the same for every list): cmp(a, b) != 0 when a
 * goes before b; next at nextoff in every element */
void *dms_msort(void *head, size_t nextoff, int (*cmp)(const void *a, const void *b));

#endif
