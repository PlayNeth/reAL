/* reAL: DirectSound buffer effects (IDirectSoundBuffer8::SetFX): the effect chain and its objects.  The standard
 * effects are ours (dsfx.c, as COM objects with the IDirectSoundFX* interfaces); any other class is a DMO hosted
 * through CoCreateInstance, as DirectSound does. */
#ifndef REAL_FX_H
#define REAL_FX_H
#include "real.h"

typedef struct rl_fxchain rl_fxchain;

/* SetFX's work once its arguments are checked: the chain for a buffer of format `wfx`, or NULL and the first
 * failure (results: DSFXR_LOCSOFTWARE / DSFXR_UNALLOCATED for a LOCDEFER buffer; on failure DSFXR_PRESENT for the
 * effects that could be made and DSFXR_FAILED / DSFXR_UNKNOWN for those that could not) */
rl_fxchain *rl_fxchain_create(const WAVEFORMATEX *wfx, DWORD count, const DSEFFECTDESC *desc, DWORD *results,
                              BOOL defer, HRESULT *hr);
void    rl_fxchain_free(rl_fxchain *c);
DWORD   rl_fxchain_count(const rl_fxchain *c);
/* a deferred chain placed (in software: Play, AcquireResources) or given up again (Stop) */
void    rl_fxchain_place(rl_fxchain *c, BOOL placed);
void    rl_fxchain_status(const rl_fxchain *c, DWORD *results, DWORD count);
/* GetObjectInPath for the effect stages: the index'th effect of class `cls` (GUID_All_Objects: any) */
HRESULT rl_fxchain_get_object(rl_fxchain *c, REFGUID cls, DWORD index, REFIID iid, void **out);
/* the mixer's side: a fresh start (Play, SetCurrentPosition), and frames processed in place */
void    rl_fxchain_discontinuity(rl_fxchain *c);
void    rl_fxchain_process(rl_fxchain *c, BYTE *data, DWORD bytes);
/* a DirectMusic sink's buffer (6efd7afc): with the reference time of its first frame, for parameter envelopes */
void    rl_fxchain_process_at(rl_fxchain *c, BYTE *data, DWORD bytes, LONGLONG t);

/* DirectMusic's buffers made from a buffer configuration (DSBC): the effects made at load (IMediaObject; ours for
 * the standard classes), and the chain of a buffer made from it, which clones them.  Sends (GUID_DSFX_SEND) go to
 * another buffer of the sink, found by its GUID; the chain's Process hands their data to `mix`. */
typedef struct rl_sendops {
    int  (*find)(void *ctx, REFGUID buffer);            /* the target, or -1 */
    void (*mix)(void *ctx, int target, const BYTE *data, DWORD bytes, uint16_t gain, int srcch);
} rl_sendops;
typedef struct { GUID cls; IUnknown *obj; GUID send; LONG level; } rl_fxsrc;
HRESULT rl_fx_create_config(REFCLSID cls, IUnknown **out);
rl_fxchain *rl_fxchain_from_config(const WAVEFORMATEX *wfx, DWORD count, const rl_fxsrc *src, const rl_sendops *ops,
                                   void *ctx, HRESULT *hr);
/* the send's mixing (dst gets n samples, or 2n when `dup`: a mono send into a stereo buffer) */
void rl_send_mix(int16_t *dst, const int16_t *src, DWORD n, uint16_t gain, BOOL first, BOOL dup);

/* one of our effects as a COM object (CoCreateInstance of a standard effect's class, for DirectMusic) */
HRESULT rl_fx_create(REFCLSID cls, REFIID iid, void **out);

#endif
