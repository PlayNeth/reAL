/* reAL-Music: internal data structures of the DirectMusic performance port (dmime.dll / dmband.dll, DirectX 9 5.3.x).
 *
 * The objects mirror Microsoft's: performance messages (pmsgs) with their queues, segments with their tracks, segment
 * states, bands.  Field comments give the original offsets where code depends on them.  All MUSIC_TIME values are int32
 * and REFERENCE_TIME values int64 (100 ns), with the original's signed comparisons. */
#ifndef REAL_DMI_H
#define REAL_DMI_H
#include <stdint.h>
#include <stddef.h>

typedef int32_t mtime_t;
typedef int64_t rtime_t;
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } dm_guid;

/* HRESULTs */
#define DM_OK               0
#define DM_FALSE            1
#ifndef DMUS_S_REQUEUE
#define DMUS_S_REQUEUE      0x08781200
#endif
#ifndef DMUS_S_FREE
#define DMUS_S_FREE         0x08781201
#endif
#ifndef DMUS_S_END
#define DMUS_S_END          0x08781202
#endif
#define DM_E_FAIL           ((int32_t)0x80004005)
#define DM_E_POINTER        ((int32_t)0x80004003)
#define DM_E_INVALIDARG     ((int32_t)0x80070057)
#define DM_E_OUTOFMEMORY    ((int32_t)0x8007000e)
#ifndef DMUS_E_NOT_FOUND
#define DMUS_E_NOT_FOUND    ((int32_t)0x88781161)
#endif
#ifndef DMUS_E_NOT_INIT
#define DMUS_E_NOT_INIT     ((int32_t)0x88781162)
#endif
#ifndef DMUS_E_TYPE_DISABLED
#define DMUS_E_TYPE_DISABLED ((int32_t)0x88781163)
#endif
#ifndef DMUS_E_TRACK_NOT_FOUND
#define DMUS_E_TRACK_NOT_FOUND ((int32_t)0x88781166)
#endif
#ifndef DMUS_E_NOT_FOUND_PCH
#define DMUS_E_NOT_FOUND_PCH ((int32_t)0x8878122b)
#endif
#ifndef DMUS_E_ALREADY_SENT
#define DMUS_E_ALREADY_SENT ((int32_t)0x88781204)
#endif
#ifndef DMUS_E_CANNOT_FREE
#define DMUS_E_CANNOT_FREE  ((int32_t)0x88781205)
#endif

/* pmsg types and flags */
enum { PT_MIDI = 0, PT_NOTE, PT_SYSEX, PT_NOTIFICATION, PT_TEMPO, PT_CURVE, PT_TIMESIG, PT_PATCH, PT_TRANSPOSE,
       PT_CHANNEL_PRIORITY, PT_STOP, PT_DIRTY, PT_WAVE };
#define PF_REFTIME        0x01
#define PF_MUSICTIME      0x02
#define PF_TOOL_IMMEDIATE 0x04
#define PF_TOOL_QUEUE     0x08
#define PF_TOOL_ATTIME    0x10
#define PF_TOOL_FLUSH     0x20
#define PF_LOCKTOREFTIME  0x40
#define PF_DX8            0x80
#define PF_RESOLVE_MASK   0x01E03C00u

#define NOTEF_NOTEON        1
#define NOTEF_NOINVALIDATE  2
#define CURVEF_RESET        1
#define CURVEF_START_FROM_CURRENT 2
enum { CURVET_PB = 3, CURVET_CC = 4, CURVET_MAT = 5, CURVET_PAT = 6, CURVET_RPN = 7, CURVET_NRPN = 8 };
enum { CURVES_LINEAR = 0, CURVES_INSTANT = 1, CURVES_EXP = 2, CURVES_LOG = 3, CURVES_SINE = 4 };

#define PCH_BROADCAST_PERFORMANCE 0xffffffffu
#define PCH_BROADCAST_AUDIOPATH   0xfffffffeu
#define PCH_BROADCAST_SEGMENT     0xfffffffdu
#define PCH_BROADCAST_GROUPS      0xfffffffcu
#define PCH_KILL_ME               0xfffffff0u

/* internal header flags (hdr+4) */
#define HF_MAGIC    0xCAFE0u
#define HF_INQUEUE  0x1u
#define HF_MARK     0x2u
#define HF_INPLAY   0x4u        /* allocated while a track was playing: track tempo pmsgs are ignored */
#define HF_ARMED    0x8u        /* curve reset armed by an invalidation */
#define HF_REQUEUE  0x100000u

/* a performance message: DMUS_PMSG plus every type's fields (the original overlays them; nothing reads across types) */
typedef struct dm_pmsg dm_pmsg;
struct dm_pmsg {
    dm_pmsg *next;          /* hdr+0x00 */
    uint32_t hflags;        /* hdr+0x04 */
    rtime_t rtlast;         /* hdr+0x10 */
    /* DMUS_PMSG */
    uint32_t size;
    rtime_t rt;             /* +0x08 */
    mtime_t mt;             /* +0x10 */
    uint32_t flags;         /* +0x14 */
    uint32_t pch;           /* +0x18 */
    uint32_t vpch;          /* +0x1c virtual track id */
    int tool;               /* +0x20 pTool: 0 none, 1 the performance's output tool */
    uint32_t type;          /* +0x28 */
    uint32_t voice;         /* +0x2c */
    uint32_t group;         /* +0x30 */
    void *user;             /* +0x34 punkUser (a segment state for notifications) */
    /* NOTE */
    mtime_t n_dur;          /* +0x38 */
    uint16_t n_musicval;    /* +0x3c */
    uint16_t n_measure;     /* +0x3e */
    int16_t n_offset;       /* +0x40 */
    uint8_t n_beat, n_grid, n_vel, n_flags, n_timerange, n_durrange, n_velrange, n_playmode, n_subchord, n_midival;
    int8_t n_transpose;
    /* MIDI (status, data1, data2), PATCH (instrument, msb, lsb) */
    uint8_t m_status, m_b1, m_b2;
    /* TRANSPOSE */
    int16_t t_transpose;
    uint16_t t_merge;
    /* CHANNEL_PRIORITY */
    uint32_t priority;
    /* TEMPO; internal tempo map entries also keep the master tempo (hdr+0x58) */
    double tempo;
    float master;
    /* CURVE */
    mtime_t c_dur;          /* +0x38 */
    mtime_t c_origstart;    /* +0x3c */
    mtime_t c_resetdur;     /* +0x40 */
    int16_t c_start, c_end, c_reset;    /* +0x44 +0x46 +0x48 */
    uint16_t c_measure;     /* +0x4a (the output tool's step interval) */
    int16_t c_offset;       /* +0x4c */
    uint8_t c_beat, c_grid, c_type, c_shape, c_cc, c_flags;
    uint16_t c_param, c_merge;
    /* SYSEX */
    uint32_t sx_len;
    uint8_t *sx_data;
    /* NOTIFICATION / TIMESIG */
    dm_guid nf_guid;
    uint32_t nf_option, nf_field1, nf_field2;
    uint8_t ts_bpm, ts_beat;
    uint16_t ts_grids;
};

typedef struct { dm_pmsg *head, *hint; } dm_queue;

/* ------------------------------------------------------------------ loaded objects */
typedef struct dm_collection dm_collection;     /* a DLS collection (dmdls.c) */

/* sequence track */
typedef struct { mtime_t mt, dur; uint32_t pch; int16_t off; uint8_t status, b1, b2; } dm_seqitem;
typedef struct { mtime_t start, dur, resetdur; uint32_t pch; int16_t off, sval, eval, rval;
                 uint8_t type, shape, cc, flags; uint16_t param, merge; } dm_curveitem;
typedef struct { uint32_t pch; dm_seqitem *ev; int nev; dm_curveitem *cv; int ncv; } dm_seqpart;
typedef struct {
    dm_seqpart *parts; int nparts;     /* in part-list order (reverse of first appearance) */
    uint32_t *pchs; int npchs;         /* first-appearance order (SetPChannelsUsed) */
} dm_seqtrack;

typedef struct { mtime_t mt; double tempo; } dm_tempoitem;
typedef struct { dm_tempoitem *items; int n; int active, setbyparam; } dm_tempotrack;

typedef struct { mtime_t mt; uint8_t bpm, beat; uint16_t grids; } dm_tsitem;
typedef struct { dm_tsitem *items; int n; int active, setbyparam; } dm_tstrack;

/* band instrument (dmband INST) */
typedef struct dm_dlrec { struct dm_dlrec *next; void *dl; int port; int refs; } dm_dlrec;
typedef struct {
    uint32_t patch, assign, priority;
    uint8_t pan, vol; int16_t transpose;
    uint32_t pch, flags, ranges[4];
    int16_t pbrange;
    dm_collection *coll;
    dm_dlrec *dl;
    int notinfile;                      /* MIDI file import: a default instrument, sent but not downloaded */
} dm_inst;
typedef struct dm_band {
    mtime_t logical, physical;
    dm_inst *inst; int ninst;          /* list order = reverse of the file's lbin order */
    uint32_t groupbits;
    int mode;
} dm_band;
typedef struct {
    dm_band **bands; int nbands;       /* sorted by logical time; equal times in reverse load order */
    int autodl, lockautodl;
    uint32_t validate;
} dm_bandtrack;

enum { TRK_OTHER = 0, TRK_SEQ, TRK_TEMPO, TRK_TIMESIG, TRK_BAND, TRK_SYSEX };
typedef struct { mtime_t mt; uint32_t len; uint8_t *data; } dm_sysexitem;
typedef struct { dm_sysexitem *items; int n; uint32_t validate; } dm_sysextrack;
typedef struct dm_track {
    int kind;
    dm_guid clsid;
    uint32_t position, group, config, priority;
    void *data;
} dm_track;

typedef struct dm_segment {
    int refs;
    uint32_t repeats, resolution, ioflags;
    rtime_t rtlength, rtloopstart, rtloopend, rtplaystart;
    mtime_t length, playstart, loopstart, loopend;
    dm_track *tracks; int ntracks;     /* segment track order */
    uint32_t *pchs; int npchs;
    uint32_t version;
    char name[64];
} dm_segment;

/* segment loading: resolve a DLS reference (DMRF) of a band instrument; NULL guid/file means the default GM set */
typedef dm_collection *(*dm_getcoll_fn)(void *ctx, const dm_guid *guid, const char *file);
dm_segment *dm_segment_load(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx);
/* a Standard MIDI File (or RMID) as dmime imports it (dmsmf.c) */
dm_segment *dm_segment_from_midi(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx);
int dm_is_midi(const void *data, size_t size);
/* dmseg.c internals the MIDI importer builds on */
void dm_segment_insert_track(dm_segment *s, const dm_track *t);
void dm_segment_release(dm_segment *s);

#endif
