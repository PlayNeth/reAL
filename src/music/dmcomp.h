/* reAL-Music: chord maps, the chord map and signpost tracks, and the composer (dmcompos).
 * The structures keep dmcompos's own shapes (the composer walks them as it does). */
#ifndef DMCOMP_H
#define DMCOMP_H
#include "dmi.h"

/* DMUS_IO_CHORDMAP_SUBCHORD, shared by the chords that name it (0x18) */
typedef struct dmc_sub {
    uint32_t chordpat, scalepat, invpat;    /* +0x00 +0x04 +0x08 */
    uint8_t chordroot, scaleroot;           /* +0x0c +0x0d */
    uint16_t cflags;                        /* +0x0e */
    uint32_t levels;                        /* +0x10 */
    int refs;                               /* +0x14 */
} dmc_sub;
typedef struct dmc_subnode { dmc_sub *sub; struct dmc_subnode *next; } dmc_subnode;
/* a chord: its name (dmcompos's string: no buffer until set) and subchords (0xc) */
#define DMC_NAME 24
typedef struct dmc_chord {
    int hasname;
    uint16_t name[DMC_NAME];
    dmc_subnode *subs;
} dmc_chord;
typedef struct dmc_palchord { dmc_chord c; struct dmc_palchord *next; } dmc_palchord;     /* (0x10) */
struct dmc_entry;
/* DMUS_IO_NEXTCHORD (0x14): a connection to another entry; flags bits 1/2 are the composer's marks */
typedef struct dmc_next {
    struct dmc_entry *to;                   /* +0x00 */
    uint32_t flags;                         /* +0x04 */
    int16_t connid;                         /* +0x08 */
    uint16_t weight, minbeats, maxbeats;    /* +0x0a +0x0c +0x0e */
    struct dmc_next *next;                  /* +0x10 */
} dmc_next;
/* a chord map entry ('choe', 0x1c): DMUS_IO_CHORDENTRY's flags (2 may begin, 4 may end a run; 0x10 the
 * composer's mark), its chord and connections */
typedef struct dmc_entry {
    uint32_t flags;                         /* +0x00 */
    int16_t connid;                         /* +0x04 */
    dmc_chord chord;                        /* +0x08 */
    dmc_next *nexts;                        /* +0x14 */
    struct dmc_entry *next;                 /* +0x18 */
} dmc_entry;
/* a chord map signpost ('spst', 0x34): DMUS_IO_CHORDMAP_SIGNPOST, its chord and cadence chords */
typedef struct dmc_signpost {
    uint32_t chords, flags;                 /* +0x00 +0x04 */
    uint32_t used;                          /* +0x08 the composer's: the signpost groups it was chosen for */
    dmc_chord chord;                        /* +0x0c */
    dmc_chord cadence[2];                   /* +0x18 +0x24 */
    struct dmc_signpost *next;              /* +0x30 */
} dmc_signpost;
/* CDMChordMap (its data at +0x34) */
typedef struct dmc_chordmap {
    int refs;
    int loaded;                             /* +0x34 */
    dm_guid guid;                           /* +0x38 */
    int hasname;
    uint16_t name[24];                      /* +0x48 ('perh' wszLoadName) */
    uint32_t scale, flags;                  /* +0x50 +0x54 */
    dmc_palchord *palette[24];              /* +0x58 (each newest first) */
    dmc_entry *entries;                     /* +0xb8 newest first */
    dmc_signpost *signposts;                /* +0xbc in file order */
    void *wrapper;                          /* the COM object (weak) */
} dmc_chordmap;

dmc_chordmap *dmc_chordmap_new(void);
/* IPersistStream::Load (77288eae): the data after the 'DMPR' form type; the old contents go first */
int dmc_chordmap_load(dmc_chordmap *m, const uint8_t *p, size_t n);
void dmc_chordmap_addref(dmc_chordmap *m);
void dmc_chordmap_release(dmc_chordmap *m);
/* ParseDescriptor (77287eea): guid and the 'perh' name; returns the DMUS_OBJ_* flags found */
uint32_t dmc_chordmap_parse(const uint8_t *p, size_t n, dm_guid *guid, uint16_t name[17]);

/* the chord map track (CPersonalityTrack, 'pftr'): chord maps by time, sorted */
typedef struct dmc_cmnode { mtime_t t; dmc_chordmap *map; struct dmc_cmnode *next; } dmc_cmnode;
typedef struct { dmc_cmnode *items; } dmc_cmtrack;
typedef dmc_chordmap *(*dm_getchordmap_fn)(void *ctx, const dm_guid *guid, const char *file, const char *name);
dmc_cmtrack *dmc_cmtrack_load(const uint8_t *p, size_t n, dm_getchordmap_fn get, void *ctx, int32_t *hr);
void dmc_cmtrack_free(dmc_cmtrack *T);
dmc_cmtrack *dmc_cmtrack_clone(const dmc_cmtrack *T, mtime_t start, mtime_t end);
/* GUID_IDirectMusicChordMap (7728c1d1 / 7728c327); Get adds a reference */
int dmc_cmtrack_getparam(const dmc_cmtrack *T, mtime_t mt, mtime_t *next, dmc_chordmap **out);
int dmc_cmtrack_setparam(dmc_cmtrack *T, mtime_t mt, dmc_chordmap *m);

/* the signpost track (CSPstTrack, 'sgnp'): DMUS_IO_SIGNPOST by time */
typedef struct dmc_sp { mtime_t t; uint32_t chords; uint16_t measure; struct dmc_sp *next; } dmc_sp;   /* (0x10) */
typedef struct { dmc_sp *items; int notify; void *composer; } dmc_sptrack;
dmc_sptrack *dmc_sptrack_load(const uint8_t *p, size_t n, int32_t *hr);
void dmc_sptrack_free(dmc_sptrack *T);
dmc_sptrack *dmc_sptrack_clone(const dmc_sptrack *T, mtime_t start, mtime_t end);
/* IPersistStream::Save (7728af59): the 'sgnp' chunk; returns the bytes (malloc), *n their count */
uint8_t *dmc_sptrack_save(const dmc_sptrack *T, size_t *n);

/* the composer (dmcompose.c).  ComposeSegmentFromTemplate (77287974 with flags 3): the template's chords composed
 * with the style (NULL: the template's) and chord map (NULL: the template's chord map track); a new segment */
struct dms_style;
dm_segment *dmc_compose_template(dm_segment *tmpl, struct dms_style *style, dmc_chordmap *cm, int activity, int *hr);
/* Segment::Compose (dmime 77314b64): the composing tracks composed and their results put in the segment */
int dmc_segment_compose(dm_segment *s);
/* ComposeTemplateFromShape (77286629; intro, fill and break lengths 1 from the COM method): a template with a command
 * and a signpost track */
dm_segment *dmc_template_from_shape(unsigned measures, unsigned shape, int intro, int end, int introlen, int fill, int brk,
                                    int endlen, int *hr);
/* ComposeSegmentFromShape (77286edd / 77283810): the measures, shape and lengths as 77287754 adjusts them.  A segment
 * may come back with an error (the style's default band missing) */
dm_segment *dmc_segment_from_shape(struct dms_style *style, dmc_chordmap *cm, unsigned measures, unsigned shape,
                                   int activity, int intro, int end, int introlen, int fill, int brk, int endlen, int *hr);

#endif
