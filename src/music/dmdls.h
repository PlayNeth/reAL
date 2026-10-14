/* reDM: DLS collections (Level 1 and 2) and the instrument downloads dmusic.dll builds from them for a
 * software synth (DMUS_DOWNLOADINFO buffers: one per wave, one INSTRUMENT2 per instrument). */
#ifndef REAL_DMDLS_H
#define REAL_DMDLS_H
#include "dmi.h"

dm_collection *dls_load(const void *data, size_t size, int gm);   /* gm: the default GM set (instrument flag) */
dm_collection *dls_load_owned(void *data, size_t size, int gm);   /* takes over a malloc'd buffer */
void dls_free(dm_collection *c);
int dls_count(const dm_collection *c);
int dls_enum(const dm_collection *c, int i, uint32_t *patch, const char **name);
int dls_find(const dm_collection *c, uint32_t patch);              /* instrument index or -1 */

/* where the buffers go: the synth's Download / Unload */
typedef struct {
    int (*download)(void *ctx, const void *buf, uint32_t size, void **handle);
    void (*unload)(void *ctx, void *handle);
} dls_sink;

typedef struct dls_port dls_port;
dls_port *dls_port_new(const dls_sink *sink, void *ctx);
void dls_port_free(dls_port *p);
/* IDirectMusicCollection::GetInstrument + (SetPatch) + IDirectMusicPort::DownloadInstrument.  ranges: pairs
 * (low, high) of notes to keep, or NULL.  Returns a handle, or NULL with *hr set (DMUS_E_NOT_FOUND: no such patch) */
void *dls_port_download(dls_port *p, dm_collection *c, uint32_t patch, int setpatch, uint32_t newpatch,
                        const uint32_t *ranges, int nranges, int *hr);
void dls_port_unload(dls_port *p, void *handle);

#endif
