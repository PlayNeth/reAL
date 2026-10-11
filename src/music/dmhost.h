/* reAL-Music host (Windows): one performance with its synth, the transport and realtime threads, and a sink that
 * renders the synth into reAL's own buffers, as DirectMusic's default audiopath (shared stereo + Waves reverb)
 * plays it through DirectSound. */
#ifndef REAL_DMHOST_H
#define REAL_DMHOST_H
#include <windows.h>
#include "dmperf.h"
#include "dmdls.h"

typedef struct dm_host dm_host;

/* rate / voices: the default synth port's parameters (DirectMusic: 22050 Hz, 64 voices); groups: channel groups */
dm_host *dm_host_new(uint32_t rate, uint32_t voices, uint32_t groups);
void dm_host_free(dm_host *h);
void dm_host_lock(dm_host *h);
void dm_host_unlock(dm_host *h);
dm_perf *dm_host_perf(dm_host *h);        /* call only with the lock held */
void dm_host_set_volume(dm_host *h, int32_t mb);
rtime_t dm_host_clock(dm_host *h);
uint32_t dm_host_rate(dm_host *h);

/* plain MIDI input (the winmm MIDI device): a message (short, or sysex starting with 0xf0) played now */
void dm_host_midi(dm_host *h, const uint8_t *msg, uint32_t len);
int dm_host_download_all(dm_host *h, dm_collection *c);     /* returns how many instruments went in */
void dm_host_set_reverb(dm_host *h, int on);
/* audiopath buffers (see dmhost.c) */
int dm_host_path_open(dm_host *h, uint32_t type, int idx[2], int *nidx, uint32_t buses[4], uint32_t *nbuses);
void dm_host_path_close(dm_host *h, const int *idx, int nidx);
HRESULT dm_host_buffer_query(dm_host *h, int idx, REFIID iid, void **out);
HRESULT dm_host_primary_query(dm_host *h, REFIID iid, void **out);
int dm_host_reverb_set(dm_host *h, float ingain, float mix, float time, float hfratio);

/* the process-wide collection cache (the loader's) */
dm_collection *dm_collection_from_file(const char *path, int gm);
const char *dm_gm_dls_path(void);          /* gm.dls: reAL.ini [Music] GMDLS, else the system's */

#endif
