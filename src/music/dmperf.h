/* reAL-Music performance engine: a port of DirectMusic's performance (dmime.dll + dmband.dll, DirectX 9 5.3.x).
 *
 * The engine is single-threaded and driven from outside: the host runs dm_perf_transport() and dm_perf_realtime()
 * the way dmime's transport and realtime threads run (each returns how long its thread would wait), wakes them
 * early when the signal callback asks for it, and serialises every call with one lock.  Output goes to "ports" as
 * time-stamped MIDI (what IDirectMusicPort::PlayBuffer receives from the performance). */
#ifndef REAL_DMPERF_H
#define REAL_DMPERF_H
#include "dmi.h"

typedef struct dm_perf dm_perf;
typedef struct dm_segstate dm_segstate;
typedef struct dm_audiopath dm_audiopath;

enum { DM_THREAD_TRANSPORT = 0, DM_THREAD_REALTIME = 1 };

typedef struct {
    rtime_t (*clock)(void *ctx);                   /* master clock (IReferenceClock::GetTime), 0 = failed */
    uint32_t (*msec)(void *ctx);                   /* timeGetTime */
    void (*signal)(void *ctx, int thread);         /* SetEvent on a thread's wake event */
} dm_hostops;

typedef struct { rtime_t rt; uint32_t group; uint32_t len; const uint8_t *data; } dm_event;

typedef struct {
    int (*latency)(void *ctx, rtime_t *t);          /* the port's latency clock; 0 if unavailable */
    void (*play)(void *ctx, const dm_event *ev, int n);       /* PlayBuffer: events in buffer order */
    void (*priority)(void *ctx, uint32_t group, uint32_t chan, uint32_t prio);   /* SetChannelPriority */
    /* instrument download (band tracks): returns a handle or NULL */
    void *(*download)(void *ctx, dm_collection *coll, uint32_t patch, int setpatch, uint32_t newpatch,
                      const uint32_t *ranges, int nranges, int *hr);
    void (*unload)(void *ctx, void *handle);
    /* AssignChannelToBuses (audiopaths): the synth channel's output buses */
    void (*assign)(void *ctx, uint32_t group, uint32_t chan, const uint32_t *buses, uint32_t n);
    /* waves (the port's private interface: DownloadWave, UnloadWave, AllocVoice; the voice's Play and Stop).  A
     * download or voice handle stays valid until wave_unload / voice_free. */
    void *(*wave_download)(void *ctx, dm_wave *w, rtime_t hint, int *hr);
    void (*wave_unload)(void *ctx, void *dl);
    void *(*voice_alloc)(void *ctx, void *dl, uint32_t chan, uint32_t group, rtime_t start, uint32_t loopstart,
                         uint32_t loopend, int *hr);
    int (*voice_play)(void *ctx, void *voice, rtime_t rt, int32_t pitch, int32_t vol);
    int (*voice_stop)(void *ctx, void *voice, rtime_t rt);
    void (*voice_free)(void *ctx, void *voice);
} dm_portops;

dm_perf *dm_perf_new(const dm_hostops *ops, void *ctx);
void dm_perf_free(dm_perf *pf);
/* IDirectMusicPerformance::AddPort / AssignPChannelBlock / AssignPChannel */
int dm_perf_add_port(dm_perf *pf, const dm_portops *ops, void *ctx);
int dm_perf_assign_block(dm_perf *pf, uint32_t block, int port, uint32_t group);
int dm_perf_assign_pchannel(dm_perf *pf, uint32_t pch, int port, uint32_t group, uint32_t chan);

/* the threads: run one pass, return the wait in milliseconds */
uint32_t dm_perf_transport(dm_perf *pf);
uint32_t dm_perf_realtime(dm_perf *pf);

/* DMUS_SEGF_* flags */
#define SEGF_REFTIME          0x40
#define SEGF_SECONDARY        0x80
#define SEGF_QUEUE            0x100
#define SEGF_CONTROL          0x200
#define SEGF_AFTERPREPARETIME 0x400
#define SEGF_GRID             0x800
#define SEGF_BEAT             0x1000
#define SEGF_MEASURE          0x2000
#define SEGF_DEFAULT          0x4000
#define SEGF_NOINVALIDATE     0x8000
#define SEGF_ALIGN            0x10000
#define SEGF_AFTERQUEUETIME   0x200000
#define SEGF_AFTERLATENCYTIME 0x400000
#define SEGF_SEGMENTEND       0x800000
#define SEGF_MARKER           0x1000000
#define SEGF_TIMESIG_ALWAYS   0x2000000

int dm_perf_play(dm_perf *pf, dm_segment *seg, uint32_t flags, int64_t start, dm_audiopath *ap, dm_segstate **out);

/* audiopaths: standard ones only (virtual PChannels 0..n-1 on new performance PChannels of port 0) */
dm_audiopath *dm_perf_create_audiopath(dm_perf *pf, uint32_t npch, int activate, const uint32_t *buses, uint32_t nbuses);
void dm_audiopath_addref(dm_audiopath *ap);
void dm_audiopath_release(dm_audiopath *ap);
int dm_audiopath_activate(dm_audiopath *ap, int on);
int dm_audiopath_set_volume(dm_audiopath *ap, int32_t vol, uint32_t dur);     /* AudioPath::SetVolume */
int dm_audiopath_convert(dm_audiopath *ap, uint32_t pch, uint32_t *out);
void dm_perf_set_default_audiopath(dm_perf *pf, dm_audiopath *ap);
dm_audiopath *dm_perf_get_default_audiopath(dm_perf *pf);
uint32_t dm_perf_pchannels(dm_perf *pf);
int dm_perf_stop(dm_perf *pf, dm_segment *seg, dm_segstate *ss, mtime_t mt, uint32_t flags);
int dm_perf_is_playing(dm_perf *pf, dm_segment *seg, dm_segstate *ss);
void dm_perf_get_time(dm_perf *pf, rtime_t *rt, mtime_t *mt);
void dm_segstate_release(dm_segstate *ss);
/* references for other holders (the COM layer's wrapper object, kept in *dm_segstate_wrapper()) */
void dm_segstate_addref(dm_segstate *ss);
void dm_segstate_unref(dm_segstate *ss);
void **dm_segstate_wrapper(dm_segstate *ss);
dm_segment *dm_segstate_segment(dm_segstate *ss);   /* NULL once the state has left the performance */
int dm_segstate_start_time(dm_segstate *ss, mtime_t *mt);
mtime_t dm_segstate_seek(dm_segstate *ss);
dm_segstate *dm_perf_get_segstate(dm_perf *pf, mtime_t t);
int dm_perf_pchannel_info(dm_perf *pf, uint32_t pch, int *port, uint32_t *group, uint32_t *chan);
int dm_perf_adjust_time(dm_perf *pf, rtime_t rt);
/* instrument download for a segment's band tracks (IDirectMusicSegment::Download / Unload) */
/* Get/SetParam.  The parameter GUIDs the engine's tracks know (the COM layer maps GUIDs to these); PRM_ANY is
 * GUID_NULL (every track), PRM_OTHER any other GUID.  Data layouts are DirectMusic's: DMUS_TEMPO_PARAM for
 * PRM_TEMPO, DMUS_TIMESIGNATURE for PRM_TIMESIG; download parameters take a dm_dltarget, PRM_CONNECTDLS a
 * dm_collection. */
enum { PRM_TEMPO, PRM_PRIVTEMPO, PRM_TIMESIG, PRM_MUTE, PRM_BAND,
       PRM_ENABLETEMPO, PRM_DISABLETEMPO, PRM_ENABLETIMESIG, PRM_DISABLETIMESIG,
       PRM_DOWNLOAD, PRM_UNLOAD, PRM_DOWNLOAD_AP, PRM_UNLOAD_AP, PRM_ENABLE_AUTODL, PRM_DISABLE_AUTODL,
       PRM_CLEARBANDS, PRM_IBAND, PRM_BANDPARAM, PRM_SMF, PRM_CONNECTDLS, PRM_WAVEPRIV, PRM_ANY, PRM_OTHER };
typedef struct { dm_perf *pf; dm_audiopath *ap; } dm_dltarget;
#define DM_SEG_ANYTRACK 0x80000000u
#define DM_SEG_ALLTRACKS 0x80000000u
/* SetTrackConfig: segment 7731245b (tracks by class; GUID_NULL refused), segment state 77315d52 (GUID_All_Objects
 * matches every class); config = (config & ~off) | on.  SetPChannelsUsed 7731281d: PChannels added to the list. */
int dm_segment_set_trackconfig(dm_segment *s, const dm_guid *clsid, uint32_t group, uint32_t index, uint32_t on, uint32_t off);
dm_track *dm_segstate_track(dm_segstate *ss, const dm_guid *clsid, uint32_t index);
int dm_segstate_set_trackconfig(dm_segstate *ss, const dm_guid *clsid, uint32_t group, uint32_t index, uint32_t on, uint32_t off);
int dm_segment_add_pchannels(dm_segment *s, const uint32_t *pch, uint32_t n);
/* a track object's own IsParamSupported / GetParam / SetParam / Add/RemoveNotificationType */
int dm_track_isparam(const dm_track *t, int prm);
int dm_track_getparam(const dm_track *t, int prm, mtime_t mt, mtime_t *next, void *data);
int dm_track_setparam(dm_track *t, int prm, mtime_t mt, void *data);
int dm_segment_getparam(dm_segment *s, int prm, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data);
int dm_segment_setparam(dm_segment *s, int prm, uint32_t group, uint32_t index, mtime_t mt, void *data);
int dm_perf_getparam(dm_perf *pf, int prm, uint32_t trackid, uint32_t group, uint32_t index, mtime_t mt, mtime_t *next, void *data);
int dm_perf_setparam(dm_perf *pf, int prm, uint32_t group, uint32_t index, mtime_t mt, void *data);
int dm_segment_download(dm_perf *pf, dm_segment *seg);
int dm_segment_unload(dm_perf *pf, dm_segment *seg);
/* the same through an audiopath (GUID_DownloadToAudioPath / GUID_UnloadFromAudioPath: its PChannels) */
int dm_segment_download_ap(dm_perf *pf, dm_segment *seg, dm_audiopath *ap);
int dm_segment_unload_ap(dm_perf *pf, dm_segment *seg, dm_audiopath *ap);

int dm_perf_m2r(dm_perf *pf, mtime_t mt, rtime_t *rt);
int dm_perf_r2m(dm_perf *pf, rtime_t rt, mtime_t *mt);
rtime_t dm_perf_latency_time(dm_perf *pf);
rtime_t dm_perf_queue_time(dm_perf *pf);
void dm_perf_set_auto_download(dm_perf *pf, int on);
int dm_perf_get_auto_download(dm_perf *pf);
void dm_perf_set_prepare_time(dm_perf *pf, uint32_t ms);
uint32_t dm_perf_get_prepare_time(dm_perf *pf);
void dm_perf_set_bumper_length(dm_perf *pf, uint32_t ms);
uint32_t dm_perf_get_bumper_length(dm_perf *pf);
void dm_perf_invalidate(dm_perf *pf, mtime_t mt, uint32_t flags);
int dm_perf_stop_audiopath(dm_perf *pf, dm_audiopath *ap, mtime_t mt, uint32_t flags);
/* x87 precision of application threads when not built with DM_FPU_NATIVE (1 = 64-bit mantissa, as under Wine) */
void dm_perf_set_app_precision(dm_perf *pf, int extended);
/* fixes of the original's flaws (off by default, for comparison against it) */
#define DM_FIX_FIRST_TEMPO 1    /* a primary segment started from silence plays its first piece at its own tempo */
#define DM_FIX_MEASURE_NUMBER 2 /* measure/beat notifications count measures across time signature changes (the
                                 * original divides the time by the current measure length, so the count jumps) */
#define DM_FIX_WAVE_SEEK 4      /* a wave item entered after it began (a seek or start inside it) picks up at the right
                                 * point and keeps its end (the original mixes up the times: off + 2 x start, ...) */
#define DM_FIX_WAVE_SYNCVAR 8   /* a wave track that follows another track's variations, with none to follow, chooses
                                 * them once per start or loop (the original chooses again on every play pass) */
#define DM_FIX_STATE_TRACKCONFIG 16  /* SegmentState::SetTrackConfig with DMUS_SEG_ALLTRACKS changes every matching track
                                 * (the original looks up the track with the unchanged index: none for ALLTRACKS) */
void dm_perf_set_fixes(dm_perf *pf, uint32_t fixes);
/* the transport thread's srand(time(NULL)) (wave track variations and random volume/pitch) */
void dm_perf_set_seed(dm_perf *pf, uint32_t seed);

/* notifications (AddNotificationType, RemoveNotificationType, SetNotificationHandle, GetNotificationPMsg, FreePMsg).
 * A notification's user is its segment state (or NULL); dm_perf_free_pmsg releases it. */
int dm_perf_add_notification_type(dm_perf *pf, const dm_guid *g);
int dm_perf_remove_notification_type(dm_perf *pf, const dm_guid *g);
void dm_perf_set_notification_handle(dm_perf *pf, void (*cb)(void *ctx), void *ctx, rtime_t rtmin);
dm_pmsg *dm_perf_get_notification(dm_perf *pf);
/* SendPMsg (77304e7e): an application's message into the queues; a message without a tool goes to the output tool.
 * dm_perf_alloc_pmsg gives a cleared message of the type's size; on failure dm_perf_send_pmsg frees it. */
dm_pmsg *dm_perf_alloc_pmsg(dm_perf *pf, uint32_t size);
int dm_perf_send_pmsg(dm_perf *pf, dm_pmsg *p);
int dm_perf_free_pmsg(dm_perf *pf, dm_pmsg *p);

#endif
