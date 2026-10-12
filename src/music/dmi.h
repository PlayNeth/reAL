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
#define DM_E_NOTIMPL        ((int32_t)0x80004001)
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
#ifndef DMUS_E_TYPE_UNSUPPORTED
#define DMUS_E_TYPE_UNSUPPORTED ((int32_t)0x88781164)
#endif
#ifndef DMUS_E_SET_UNSUPPORTED
#define DMUS_E_SET_UNSUPPORTED ((int32_t)0x88781123)
#endif
#ifndef DMUS_E_GET_UNSUPPORTED
#define DMUS_E_GET_UNSUPPORTED ((int32_t)0x88781124)
#endif
#ifndef DMUS_E_NO_MASTER_CLOCK
#define DMUS_E_NO_MASTER_CLOCK ((int32_t)0x88781170)
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
#ifndef DMUS_S_LAST_TOOL
#define DMUS_S_LAST_TOOL    0x08781211
#endif
#ifndef DMUS_E_AUDIOPATH_INACTIVE
#define DMUS_E_AUDIOPATH_INACTIVE ((int32_t)0x88781229)
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
    int tool;               /* +0x20 pTool: 0 none, 1 the performance's output tool, 2 an application's tool (in ext) */
    uint32_t type;          /* +0x28 */
    uint32_t voice;         /* +0x2c */
    uint32_t group;         /* +0x30 */
    void *user;             /* +0x34 punkUser (a segment state for notifications) */
    int userref;            /* user holds a reference (released when the message is freed) */
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
    /* WAVE (DMUS_WAVE_PMSG) */
    rtime_t w_startoff;     /* +0x38 */
    rtime_t w_dur;          /* +0x40 */
    int32_t w_offset;       /* +0x48 */
    int32_t w_vol;          /* +0x4c */
    int32_t w_pitch;        /* +0x50 */
    uint8_t w_flags;        /* +0x54 DMUS_WAVEF_* */
    struct dm_wvoice *w_voice;      /* punkUser: the port's voice */
    /* the application's view of the message (the host's DMUS_PMSG, with pTool, pGraph and data the engine does not
     * keep): made when the message first reaches the application, freed with it (dm_toolops) */
    void *ext;
};
#define WAVEF_OFF 1
#define WAVEF_STREAMING 2
#define WAVEF_NOINVALIDATE 4
#define WAVEF_NOPREROLL 8
#define WAVEF_IGNORELOOPS 0x20

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
typedef struct { dm_tempoitem *items; int n; int active, setbyparam; uint32_t version; } dm_tempotrack;

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
    int notinfile;                      /* MIDI file import: a default instrument, neither sent nor downloaded */
    int smf; uint32_t sendpatch;        /* GUID_StandardMIDIFile: downloads take the GM program, the band still sends sendpatch */
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

/* ------------------------------------------------------------------ waves (dmwave.c: dswave CDirectSoundWave) */
typedef struct dm_wave {
    int refs;
    uint16_t channels, bits;
    uint32_t rate, frames;          /* PCM as played: interleaved 8-bit unsigned or 16-bit */
    uint8_t *data;
    uint32_t filerate, fileframes, skip;   /* the file's own format (the length in ms comes from these) */
    int streaming, nopreroll;       /* GetStreamingParms: DMUS_WAVEF_STREAMING, DMUS_WAVEF_NOPREROLL */
    rtime_t readahead;
    dm_guid guid; int hasguid;
    uint32_t vers[2]; int hasvers;
    char name[64];
    void *wrapper;                  /* the COM object (weak) */
} dm_wave;
/* a compressed format's decoder (the host's ACM): fills ch, rate and bits unless set (the 'wavu' format) */
typedef int (*dm_wave_decoder)(const uint8_t *wfx, uint32_t wfxlen, const uint8_t *in, uint32_t inlen,
                               uint8_t **out, uint32_t *outlen, uint16_t *ch, uint32_t *rate, uint16_t *bits);
void dm_wave_set_decoder(dm_wave_decoder fn);
/* the length of a compressed wave without a 'fact' chunk from its decoded samples (dswave counts compressed blocks,
 * which decides streaming and a .wav segment's length wrongly) */
void dm_wave_set_fix_length(int on);
dm_wave *dm_wave_load(const void *data, size_t size);
void dm_wave_addref(dm_wave *w);
void dm_wave_release(dm_wave *w);
uint32_t dm_wave_length_ms(const dm_wave *w);

/* wave track (dmime CWavTrack 77328647): parts sorted by PChannel, items in play order */
typedef struct dm_dlnode { struct dm_dlnode *next; void *dl; } dm_dlnode;
typedef struct dm_waveitem {
    struct dm_waveitem *next;   /* +0x50 */
    int64_t rt;                 /* +0x00 rtTime (a music time unless the track plays in clock time) */
    int32_t vol, pitch;         /* +0x08 +0x0c */
    uint32_t variations;        /* +0x10 */
    int64_t startoff;           /* +0x18 rtStartOffset (reference time) */
    int64_t dur;                /* +0x20 rtDuration (a music time unless clock time) */
    mtime_t logical;            /* +0x28 */
    uint32_t flags;             /* +0x2c */
    dm_wave *wave;              /* +0x30 */
    dm_dlnode *dls;             /* +0x34 the streaming downloads made for it */
    uint16_t volrange, pitchrange;  /* +0x38 */
    int streaming, nopreroll;   /* +0x3c +0x40 */
    uint32_t loopstart, loopend;    /* +0x44 +0x48 */
    int32_t index;              /* +0x4c voice slot */
} dm_waveitem;
typedef struct dm_wavepart {
    struct dm_wavepart *next;
    uint32_t pch, index;
    int32_t vol;
    uint32_t variations, lock, flags;
    dm_waveitem *items;
} dm_wavepart;
typedef struct dm_wavetrack {
    int32_t vol; uint32_t flags;            /* +0x30 +0x34 DMUS_WAVETRACKF_SYNC_VAR 1, PERSIST_CONTROL 2 */
    uint32_t nparts; uint32_t *pchs;        /* +0x38 +0x3c */
    dm_wavepart *parts;                     /* +0x40 */
    uint32_t version;                       /* +0x48 */
    uint32_t aud[6];                        /* +0x50 audition: variation, pchannel, index, lock, by part, by lock */
    int autodl, autodl_set;                 /* +0x68 +0x6c */
    uint32_t *persistvar, *persistused;     /* +0x70 +0x74 */
    uint32_t nitems;                        /* +0x78 */
    struct wsdl *sdl;                       /* +0x80 streaming downloads in use, by state */
} dm_wavetrack;

enum { TRK_OTHER = 0, TRK_SEQ, TRK_TEMPO, TRK_TIMESIG, TRK_BAND, TRK_SYSEX, TRK_WAVE };
typedef struct { mtime_t mt; uint32_t len; uint8_t *data; } dm_sysexitem;
typedef struct { dm_sysexitem *items; int n; uint32_t validate; } dm_sysextrack;
/* a track object (IDirectMusicTrack): shared by the segments it was inserted in and the states playing it */
typedef struct dm_track {
    int refs;
    int kind;
    dm_guid clsid;
    void *data;
    int notify;             /* AddNotificationType accepted (time signature track: GUID_NOTIFICATION_MEASUREANDBEAT) */
    void *wrapper;          /* the COM object (weak) */
} dm_track;
/* a segment's entry for a track (dmime's track list item): the segment's own group, config, position, priority */
typedef struct dm_segtrack {
    dm_track *t;
    uint32_t position, group, config, priority;
} dm_segtrack;

/* a segment's notification types (dmime 7731322f): fromperf = added because the performance has it */
typedef struct { dm_guid g; int fromperf; } dm_ntype;

typedef struct dm_segment {
    int refs;
    uint32_t repeats, resolution, ioflags;
    rtime_t rtlength, rtloopstart, rtloopend, rtplaystart;
    mtime_t length, playstart, loopstart, loopend;
    dm_segtrack *tracks; int ntracks;  /* segment track order */
    uint32_t *pchs; int npchs;
    uint32_t version;
    char name[64];
    dm_ntype *ntypes; int nntypes;
    void *wrapper;          /* the COM object (weak) */
} dm_segment;

/* notification types (dmusici.h) */
static const dm_guid GUID_NOTIFICATION_SEGMENT_ = { 0xd2ac2899, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid GUID_NOTIFICATION_MEASUREANDBEAT_ = { 0xd2ac289a, 0xb39b, 0x11d1, { 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd } };
static const dm_guid GUID_NOTIFICATION_PERFORMANCE_ = { 0x81f75bc5, 0x4e5d, 0x11d2, { 0xbc, 0xc7, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb } };
/* DMUS_NOTIFICATION_* options */
enum { NOTIF_SEGSTART = 0, NOTIF_SEGEND, NOTIF_SEGALMOSTEND, NOTIF_SEGLOOP, NOTIF_SEGABORT };
enum { NOTIF_MUSICSTARTED = 0, NOTIF_MUSICSTOPPED, NOTIF_MUSICALMOSTEND };
/* IDirectMusicSegment::AddNotificationType / RemoveNotificationType (fromperf: on behalf of the performance) */
int dm_segment_add_notification(dm_segment *s, const dm_guid *g, int fromperf);
int dm_segment_remove_notification(dm_segment *s, const dm_guid *g, int fromperf);
int dm_segment_has_notification(const dm_segment *s, const dm_guid *g);
int dm_track_add_notification(dm_track *t, const dm_guid *g);      /* a track's own (S_FALSE: not one it sends) */
int dm_track_remove_notification(dm_track *t, const dm_guid *g);

/* segment loading: resolve a DLS reference (DMRF) of a band instrument; NULL guid/file means the default GM set */
typedef dm_collection *(*dm_getcoll_fn)(void *ctx, const dm_guid *guid, const char *file);
dm_segment *dm_segment_load(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx);
/* with wave references too (a wave track's DMRF: guid, file, name); a RIFF WAVE file loads as a one-wave segment */
typedef dm_wave *(*dm_getwave_fn)(void *ctx, const dm_guid *guid, const char *file, const char *name);
dm_segment *dm_segment_load_ex(const void *data, size_t size, dm_getcoll_fn getcoll, dm_getwave_fn getwave, void *ctx);
dm_segment *dm_segment_from_wave(dm_wave *w);
/* a Standard MIDI File (or RMID) as dmime imports it (dmsmf.c) */
dm_segment *dm_segment_from_midi(const void *data, size_t size, dm_getcoll_fn getcoll, void *ctx);
int dm_is_midi(const void *data, size_t size);
/* dmseg.c internals the MIDI importer builds on */
/* a new track object of the kind with its data (one reference); add / drop references */
dm_track *dm_track_new(int kind, const dm_guid *clsid, void *data);
void dm_track_addref(dm_track *t);
void dm_track_release(dm_track *t);
/* Clone: a track's data in [start, end) moved to 0 (77..: per kind); a segment's tracks cloned (77313a27).
 * dm_set_clone_fix: keep what the original's clones lose (wave item ranges, a band carried over without a later band) */
dm_track *dm_track_clone(const dm_track *t, mtime_t start, mtime_t end, int *hr);
dm_segment *dm_segment_clone(const dm_segment *s, mtime_t start, mtime_t end, int *hr);
void dm_set_clone_fix(int on);
/* insert in segment track order (position, then sysex before band before the rest); takes over one reference */
void dm_segment_insert_track(dm_segment *s, dm_track *t, uint32_t position, uint32_t group, uint32_t config, uint32_t priority);
void dm_segment_release(dm_segment *s);

#endif
