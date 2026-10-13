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

/* ------------------------------------------------------------------ styles (CDMStyle 7740bd9d; DMStyleStruct at +0x38) */
enum { EV_NOTE = 1, EV_CURVE = 2, EV_MARKER = 3, EV_ANTICIPATION = 4 };
typedef struct dms_event {          /* CDirectMusicEventItem: note 0x24, curve 0x28, marker / anticipation 0x14 */
    struct dms_event *next;         /* +0x00 */
    int16_t grid;                   /* +0x04 mtGridStart (kept as a short) */
    int16_t offset;                 /* +0x06 nTimeOffset */
    uint32_t var;                   /* +0x08 dwVariation */
    int type;                       /* +0x0c */
    mtime_t dur;                    /* +0x10 note / curve duration */
    uint16_t mv;                    /* +0x14 note music value */
    uint8_t vel, timerange, durrange, velrange, inversion, playmode, noteflags;   /* +0x16 .. +0x1b, +0x20 */
    mtime_t resetdur;               /* +0x14 curve */
    int16_t cstart, cend, creset;   /* +0x18 +0x1a +0x1c */
    uint8_t ctype, shape, cc, cflags;   /* +0x1e .. +0x21 */
    uint16_t param, merge;          /* +0x22 +0x24 */
    uint16_t mflags;                /* +0x10 marker flags */
    uint8_t arange;                 /* +0x10 anticipation time range */
} dms_event;
typedef struct dms_marker { struct dms_marker *next; mtime_t t; uint16_t flags; } dms_marker;
typedef struct dms_mtime { struct dms_mtime *next; mtime_t t; } dms_mtime;
typedef struct dms_res { struct dms_res *next; uint32_t var; uint16_t mv; uint8_t inversion, playmode; } dms_res;
typedef struct dms_part {           /* DirectMusicPart (300) */
    int refs;
    dm_guid guid;                   /* +0x04 */
    uint8_t ts[4];                  /* +0x14 beats, beat, grids (word) */
    uint16_t measures;              /* +0x18 */
    uint32_t varchoices[32];        /* +0x1c */
    uint8_t playmode, invupper, invlower;   /* +0x9c +0x9d +0x9e */
    uint32_t flags;                 /* +0xa0 */
    dms_event *events;              /* +0xa4 */
    dms_marker *markers[32];        /* +0xa8 */
    dms_res *res;                   /* +0x128 */
} dms_part;
typedef struct dms_partref {        /* DirectMusicPartRef (0x10) */
    struct dms_partref *next;       /* +0x0c */
    dms_part *part;                 /* +0x00 */
    uint32_t pch;                   /* +0x04 */
    uint8_t lock, subchord, priority, random;   /* +0x08 .. +0x0b */
} dms_partref;
typedef struct dms_pattern {        /* CDirectMusicPattern (0x48) */
    int refs;
    uint16_t name[64];              /* +0x04 */
    uint8_t ts[4];                  /* +0x0c */
    uint8_t groovelo, groovehi, destlo, desthi;     /* +0x12 +0x13 +0x14 +0x15 */
    uint16_t embellish;             /* +0x16 */
    uint16_t measures;              /* +0x18 */
    uint32_t *rhythm;               /* +0x1c */
    dms_partref *prefs;             /* +0x20 */
    int hasmotif;                   /* +0x24 */
    uint32_t repeats;               /* +0x28 */
    mtime_t playstart, loopstart, loopend;  /* +0x2c +0x30 +0x34 */
    uint32_t resolution;            /* +0x38 */
    dm_band *band;                  /* +0x3c the motif's band */
    dms_mtime *markers;             /* +0x40 */
    uint32_t flags;                 /* +0x44 */
} dms_pattern;
typedef struct dms_style {
    int refs;
    dm_guid guid; int hasguid;      /* +0x04 */
    uint16_t category[64];          /* +0x14 */
    uint16_t name[64];              /* +0x1c */
    uint32_t vers[2];               /* +0x24 */
    uint8_t ts[4];                  /* +0x2c */
    double tempo;                   /* +0x30 */
    dms_part **parts; int nparts;   /* +0x38 */
    dms_pattern **patterns; int npatterns;  /* +0x3c */
    dms_pattern **motifs; int nmotifs;      /* +0x40 */
    dm_band **bands; int nbands;    /* +0x44 (newest first) */
    dm_band *defband;               /* +0x48 */
    uint32_t *pchs; int npchs;      /* +0x54 newest first */
    uint32_t flags;                 /* +0x58 1: DX8 */
    void *wrapper;                  /* the COM object (weak) */
} dms_style;

/* a style file's 'DMST' form (the data after the form type) */
dms_style *dms_style_load(const uint8_t *p, size_t n, dm_getcoll_fn getcoll, void *ctx, int32_t *hr);
void dms_style_addref(dms_style *s);
void dms_style_release(dms_style *s);
/* the style track's Load sorts every part's events again (77412645: the sort is not stable) */
void dms_style_resort(dms_style *s);
/* a time signature (beats, beat, grids) of the pattern or part, falling back to the style's (77407c91 / 77407cb1) */
const uint8_t *dms_pattern_ts(const dms_pattern *p, const dms_style *s);
const uint8_t *dms_part_ts(const dms_part *pt, const dms_style *s, const dms_pattern *p);
/* 77401b7f: clocks per beat; 7740625b: a grid position in clocks */
int32_t dms_beat_clocks(const uint8_t *ts);
int32_t dms_grid_clocks(const uint8_t *ts, uint16_t grid);

/* dmstyle's list merge sort (77402021 split, 77402056 merge, the same for every list): cmp(a, b) != 0 when a
 * goes before b; next at nextoff in every element */
void *dms_msort(void *head, size_t nextoff, int (*cmp)(const void *a, const void *b));

#endif
