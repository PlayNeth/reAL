/* reDM: the style engine's data (dmstyle.dll, XP 5.3.2600.5512): the chord, command and mute tracks, kept the
 * way dmstyle keeps them (lists in its order, its merge sort).  Notes in docs/dmstyle_notes.md. */
#ifndef REAL_DMSTY_H
#define REAL_DMSTY_H
#include "dmi.h"
#include "dmtheory.h"
typedef struct dms_style dms_style;

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
struct dms_style {
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
};

/* a style file's 'DMST' form (the data after the form type) */
dms_style *dms_style_load(const uint8_t *p, size_t n, dm_getcoll_fn getcoll, void *ctx, int32_t *hr);
/* the host's lock around a shared style's events sorted again by a Load (enter 1, then 0); NULL: none */
extern void (*dms_resort_hook)(int enter);
void dms_style_addref(dms_style *s);
void dms_style_release(dms_style *s);
/* the style track's Load sorts every part's events again (77412645: the sort is not stable) */
void dms_style_resort(dms_style *s);
void dms_part_resort(dms_part *p);
/* a time signature (beats, beat, grids) of the pattern or part, falling back to the style's (77407c91 / 77407cb1) */
const uint8_t *dms_pattern_ts(const dms_pattern *p, const dms_style *s);
const uint8_t *dms_part_ts(const dms_part *pt, const dms_style *s, const dms_pattern *p);
/* 77401b7f: clocks per beat; 7740625b: a grid position in clocks */
int32_t dms_beat_clocks(const uint8_t *ts);
int32_t dms_grid_clocks(const uint8_t *ts, uint16_t grid);

/* ------------------------------------------------------------------ style track (CStyleTrack 774128f4; StyleTrackInfo) */
typedef struct dms_stynode {        /* +0 time, +4 IDirectMusicStyle, +8 next */
    struct dms_stynode *next;
    mtime_t t;
    dms_style *style;
} dms_stynode;
typedef struct {
    dms_stynode *styles;            /* +0x08 sorted by time after Load */
    uint32_t *pchs; int npchs;      /* +0x0c +0x10 every style's PChannels (each once) */
    int notify;                     /* +0x14 GUID_NOTIFICATION_MEASUREANDBEAT */
    int tsenabled, tsset;           /* +0x18 +0x1c GUID_EnableTimeSig / DisableTimeSig */
    int pchschanged;                /* +0x20 */
    int32_t seed;                   /* +0x24 GUID_SeedVariations */
    uint32_t gen;                   /* +0x2c load generation */
    uint8_t *pvar; uint32_t *pnorep;    /* +0x30 +0x34 the parts' variations kept across plays (a pattern with flag 1) */
    dms_pattern *motif;             /* +0x38 the motif track's pattern (MotifTrackInfo) */
    int kind;                       /* TRK_PATTERN: a pattern track (its pattern its own, cloned by measures) */
    /* the pattern track's audition (PatternTrackInfo, IDirectMusicPatternTrack::SetVariation and the private
     * interfaces), given to every state it starts */
    uint32_t *varbuf;               /* +0x3c GUID_Variations' variations, one per part */
    uint32_t au_flags, au_part;     /* +0x40 +0x44 the variations to play, on the part with this PChannel */
    dm_guid au_guid;                /* +0x48 or the part with this guid */
    int au_byguid, au_all;          /* +0x58 +0x5c (1, 1: no audition) */
    dms_pattern **retired; int nretired;    /* patterns replaced (SetPatternByName), kept for the states */
} dms_styletrack;
/* GUID_Variations (DMUS_VARIATIONS_PARAM): the parts' PChannels and variations */
typedef struct { uint32_t n; uint32_t *pchs; uint32_t *vars; } dms_varparam;
/* a style reference (DMRF): the loader's style, with a reference for the caller */
typedef dm_getstyle_fn dms_getstyle_fn;
/* the 'sttr' list's data (after its type) */
dms_styletrack *dms_styletrack_load(const uint8_t *p, size_t n, dms_getstyle_fn get, void *ctx, int32_t *hr);
void dms_styletrack_free(dms_styletrack *T);
dms_styletrack *dms_styletrack_clone(const dms_styletrack *T, mtime_t start, mtime_t end);
/* the motif track (CMotifTrack 7740ec38) of a style's motif: 7740ecba SetParams; the motif's parts sorted again
 * (77409532) */
dms_styletrack *dms_motiftrack_new(dms_style *s, dms_pattern *p);
/* the pattern track (CPatternTrack 7741484d): a 'DMPT' form's data; the pattern in ->motif, its style the first */
dms_styletrack *dms_patterntrack_load(const uint8_t *p, size_t n, dm_getcoll_fn getcoll, void *ctx, int32_t *hr);
/* IDirectMusicStyle::GetMotif (77409532): a segment with the motif track (and the motif's band) */
dm_segment *dms_motif_segment(dms_style *s, dms_pattern *p);
void dms_pattern_addref(dms_pattern *p);
dms_pattern *dms_pattern_copy(const dms_pattern *p);
dms_pattern *dms_patterntrack_swap(dms_styletrack *T, dms_pattern *p);   /* (the old one kept by the track) */
/* IDirectMusicPatternTrack (the engine's lock held) */
struct dm_segstate; struct dm_segment;
int dms_patterntrack_set_variation(dm_track *t, struct dm_segstate *ss, uint32_t flags, uint32_t part);
int dms_patterntrack_set_pattern(dm_track *t, struct dm_segstate *ss, const dms_pattern *src, mtime_t *length);
int dms_patterntrack_create_segment(dm_track *t, const dms_style *style, struct dm_segment **out);
void dms_pattern_release(dms_pattern *p);
/* 77410c9b: the PChannels of all the styles */
int dms_styletrack_pchs(dms_styletrack *T);

/* dmstyle's list merge sort (77402021 split, 77402056 merge, the same for every list): cmp(a, b) != 0 when a
 * goes before b; next at nextoff in every element */
void *dms_msort(void *head, size_t nextoff, int (*cmp)(const void *a, const void *b));

#endif
