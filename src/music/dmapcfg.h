/* reDM: audiopath configurations (CLSID_DirectMusicAudioPathConfig, the 'DMAP' form of .aud files), as dmime
 * loads them (773210de / 773204da / 77320245) with the buffer configurations ('DSBC' forms) that dsound loads
 * (CLSID_DirectSoundBufferConfig, 6efdb7de / 6efdb574). */
#ifndef REAL_DMAPCFG_H
#define REAL_DMAPCFG_H
#include <windows.h>
#include <dmusici.h>
#include <dmusicf.h>
#include <dsound.h>
#include "../fx.h"

/* a buffer of the configuration ('dbfl' list, or a bare 'DSBC' form) */
typedef struct {
    GUID guid;                  /* +0x0c ddah's, or the DSBC form's own 'guid' */
    DWORD flags;                /* +0x1c DMUS_BUFFERF_* */
    int type;                   /* +0x20 1 Reverb, 2 EnvReverb, 4 3D dry, 6 Mono, 7 Stereo (8 the shared stereo of the
                                 * standard path); 0: described by its DSBC form */
    BOOL dsbc;                  /* a DSBC form came with it */
    BOOL hasdesc;               /* 'dsbd' */
    DWORD dsflags; WORD nch; LONG vol, pan;
    DWORD nbus; DWORD *bus;     /* 'bsid': the buses' functions (DSBUSID_*) */
    BOOL has3d; GUID alg; DS3DBUFFER ds3d;
    DWORD nfx; rl_fxsrc *fx;    /* the effects, made at load (sends: no object) */
} apc_buffer;

typedef struct { DWORD base, count, nbuf, flags; GUID *bufs; } apc_map;   /* 'pchh' */

typedef struct {
    DMUS_IO_PORTCONFIG_HEADER h;    /* 'pcfh' */
    DMUS_PORTPARAMS8 pp;            /* 'pprh' */
    apc_buffer *bufs; DWORD nbufs;
    apc_map *maps; DWORD nmaps;
} apc_port;

typedef struct {
    apc_buffer *bufs; DWORD nbufs;  /* the top-level buffers (created first, in order) */
    apc_port *ports; DWORD nports;
    IDirectMusicGraph *graph;       /* 'DMTG': the paths made from it get a clone (7731f0f2) */
} apc;

/* the configuration object (AddRef'd) behind an IUnknown, or NULL if it is not one of ours */
const apc *rm_apcfg_data(IUnknown *u, IUnknown **holder);
HRESULT rm_apcfg_create(REFIID riid, void **out);
/* loading from data (the loader): the object, or the failure */
HRESULT rm_apcfg_load(const BYTE *data, DWORD size, IUnknown **out);

#endif
