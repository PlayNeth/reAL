/* reAL-Music host (Windows): one performance with its synth, the transport and realtime threads, and a sink that
 * renders the synth into reAL's own buffers, as DirectMusic's default audiopath (shared stereo + Waves reverb)
 * plays it through DirectSound. */
#ifndef REAL_DMHOST_H
#define REAL_DMHOST_H
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

/* the process-wide collection cache (the loader's) */
dm_collection *dm_collection_from_file(const char *path, int gm);
const char *dm_gm_dls_path(void);          /* gm.dls: reAL.ini [Music] GMDLS, else the system's */

#endif
