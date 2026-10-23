/* reDM host (Windows): one performance with its synth, the transport and realtime threads, and a sink that
 * renders the synth into reAL's own buffers, as DirectMusic's default audiopath (shared stereo + Waves reverb)
 * plays it through DirectSound. */
#ifndef REAL_DMHOST_H
#define REAL_DMHOST_H
#include <windows.h>
#include "dmperf.h"
#include "dmdls.h"
#include "msynth.h"
#include "../fx.h"

typedef struct dm_host dm_host;

/* rate / voices: the default synth port's parameters (DirectMusic: 22050 Hz, 64 voices); groups: channel groups */
dm_host *dm_host_new(uint32_t rate, uint32_t voices, uint32_t groups);
dm_host *dm_host_new_ex(uint32_t rate, uint32_t voices, uint32_t groups, int channels);   /* 1 or 2 */
/* not with the engine lock held, nor on one of the host's own threads (it waits for them, and they take the lock) */
void dm_host_free(dm_host *h);
int dm_host_own_thread(dm_host *h);       /* whether this thread is one of the host's */
/* the engine lock: one for every performance (see dmhost.c); dm_host_lock(h) is the same lock */
void dm_engine_lock(void);
void dm_engine_unlock(void);
void dm_host_lock(dm_host *h);
void dm_host_unlock(dm_host *h);
dm_perf *dm_host_perf(dm_host *h);        /* call only with the lock held */
void dm_host_set_volume(dm_host *h, int32_t mb);
rtime_t dm_host_clock(dm_host *h);
/* whether this thread holds a performance's lock; a COM object released once it no longer does (now if it does not) */
int  dm_host_held(void);
void dm_host_release_later(void *unk);
uint32_t dm_host_rate(dm_host *h);
/* an application's own download buffers (IDirectMusicPortDownload), with the engine lock held: ids from the port's
 * counter, a buffer handed to the synth (< 0 refused), and its unload */
uint32_t dm_host_reserve_dlids(dm_host *h, uint32_t n);
int dm_host_raw_download(dm_host *h, const void *buf, uint32_t size, void **handle);
void dm_host_raw_unload(dm_host *h, void *handle);
/* a collection's instrument (IDirectMusicPort::DownloadInstrument), with the engine lock held: patch is its patch in
 * the collection, newpatch the one to download it as; ranges: pairs (low, high) of keys, or none.  NULL with *hr */
void *dm_host_ins_download(dm_host *h, dm_collection *c, uint32_t patch, uint32_t newpatch, const uint32_t *ranges,
                           int nranges, int *hr);
void dm_host_ins_unload(dm_host *h, void *handle);
/* with the engine lock held: another host's synth as a port of h's performance (CreatePort + AddPort; its index, < 0
 * when there is no room), and that port taken out (RemovePort: nothing goes to it) or put back */
int dm_host_port_add(dm_host *h, dm_host *port);
void dm_host_port_enable(dm_host *h, int idx, dm_host *port, int on);

/* plain MIDI input (the winmm MIDI device): a message (short, or sysex starting with 0xf0) played now */
void dm_host_midi(dm_host *h, const uint8_t *msg, uint32_t len);
/* a message at a reference time on a channel group (0: all of them), as IDirectMusicPort::PlayBuffer plays it */
void dm_host_midi_at(dm_host *h, rtime_t rt, uint32_t group, const uint8_t *msg, uint32_t len);
int dm_host_download_all(dm_host *h, dm_collection *c);     /* returns how many instruments went in */
void dm_host_set_reverb(dm_host *h, int on);
/* audiopath buffers (see dmhost.c) */
int dm_host_path_open(dm_host *h, uint32_t type, int idx[2], int *nidx, uint32_t buses[4], uint32_t *nbuses);
void dm_host_path_close(dm_host *h, const int *idx, int nidx);
HRESULT dm_host_buffer_query(dm_host *h, int idx, REFIID iid, void **out);
/* a buffer an audiopath configuration describes (see dmhost.c); released with dm_host_path_close */
typedef struct {
    int nch;                            /* 1 or 2 (16-bit, the synth's rate) */
    DWORD flags;                        /* the DSBCAPS_* asked for */
    DWORD caps;                         /* what GetCaps reports */
    LONG vol, pan;
    DWORD nfunc; const DWORD *func;     /* the buses' functions (DSBUSID_*); none: a mix-in */
    const GUID *alg; const DS3DBUFFER *ds3d;
    GUID guid;                          /* sends find it by this */
    DWORD nfx; const rl_fxsrc *fx;      /* the effects (the configuration's, cloned) */
} dm_hostbuf;
HRESULT dm_host_buffer_new(dm_host *h, const dm_hostbuf *d, int *idx);
DWORD dm_host_buffer_buses(dm_host *h, int idx, uint32_t *out, DWORD max);
void dm_host_buffer_addref(dm_host *h, int idx);
int dm_host_buffer_inuse(dm_host *h, int idx);     /* held by a path (the shared buffers: beyond the host's own) */
HRESULT dm_host_buffer_dmo(dm_host *h, int idx, REFGUID g, DWORD n, REFIID iid, void **out);
HRESULT dm_host_primary_query(dm_host *h, REFIID iid, void **out);
int dm_host_priority(dm_host *h, uint32_t group, uint32_t chan, uint32_t *prio, int set);     /* get / set */
/* the synth's running statistics (IDirectMusicPort::GetRunningStats) */
void dm_host_stats(dm_host *h, msynth_stats *st);
rtime_t dm_host_latency(dm_host *h);       /* the latency clock: the time of the next sample to be written */
int dm_host_reverb_set(dm_host *h, float ingain, float mix, float time, float hfratio);

/* the process-wide collection cache (the loader's) */
/* a collection file through the process-wide cache; the caller gets a reference (dls_release) */
dm_collection *dm_collection_from_file(const char *path, int gm);
const char *dm_gm_dls_path(void);          /* gm.dls: reAL.ini [Music] GMDLS, else the system's */

#endif
