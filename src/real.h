/* reAL - shared declarations for the Windows side */
#ifndef REAL_H
#define REAL_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <dsound.h>
#include <stddef.h>
#include "mixcore.h"
#include "spatial.h"
#include "reverb.h"
#include "hrtf.h"
#include "eax.h"

#define REAL_VERSION "0.1.0"

#define impl_from(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* ---------------- config ---------------- */
typedef struct rl_config {
    int  buffers;           /* ALchemy compat: # of AL buffers (used for default latency) */
    int  duration;          /* ALchemy compat: ms per buffer */
    int  max_voices;        /* reported HW voices / hard voice limit */
    int  latency_ms;        /* WASAPI buffer length; 0 = derive from buffers*duration/2 */
    int  disable_dmusic;
    int  enabled;           /* 0 = forward everything to the real dsound.dll */
    int  log_level;         /* 0 off, 1 info, 2 calls, 3 verbose (per-lock / per-play) */
    int  log_3d;
    int  log_eax;
    char log_file[MAX_PATH];
    char real_dsound[MAX_PATH];
    int  hrtf_mode;             /* 0 off, 1 auto (use reAL_hrtf.bin if found) */
    char hrtf_path[MAX_PATH];   /* explicit file, overrides the search */
} rl_config;

extern rl_config g_cfg;
extern HMODULE   g_self;

void rl_config_load(void);

/* ---------------- logging ---------------- */
void rl_log_init(void);
void rl_log(int level, const char *fmt, ...);
#define LOG_INFO(...)  rl_log(1, __VA_ARGS__)
#define LOG_CALL(...)  do { if (g_cfg.log_level >= 2) rl_log(2, __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (g_cfg.log_level >= 3) rl_log(3, __VA_ARGS__); } while (0)
#define LOG_3D(...)    do { if (g_cfg.log_3d) rl_log(0, __VA_ARGS__); } while (0)
#define LOG_EAX(...)   do { if (g_cfg.log_eax) rl_log(0, __VA_ARGS__); } while (0)
const char *rl_guid_str(const GUID *g, char buf[64]);

/* ---------------- real dsound forwarding ---------------- */
HMODULE rl_real_dsound(void);

/* ---------------- device / mixer ---------------- */
typedef struct rl_buffer rl_buffer;

typedef struct rl_device {
    CRITICAL_SECTION lock;
    LONG        ref;              /* number of IDirectSound objects using the device */
    HANDLE      thread;
    HANDLE      quit_event;
    volatile LONG running;
    UINT32      out_rate;
    UINT32      out_channels;
    UINT32      period_frames;    /* approx frames mixed per wakeup */
    rl_buffer  *playing;          /* intrusive list of playing buffers */
    rl_buffer  *all;              /* intrusive list of all buffers (for 3D commits) */
    rl_listener3d listener;       /* committed listener state */
    rl_listener3d listener_pending; /* includes DS3D_DEFERRED changes */
    BOOL        listener_dirty;
    BOOL        primary_playing;
    LONG        primary_volume;
    float       *mixbuf;          /* stereo float scratch */
    UINT32      mixbuf_frames;
    WAVEFORMATEX primary_fmt;
    DWORD       speaker_config;
    LONG        n_buffers;        /* live secondary buffers */
    LONG        n_hw_buffers;     /* live non-LOCSOFTWARE secondaries (ALchemy counts these in GetCaps) */

    /* EAX */
    BOOL        eax_used;         /* game made an EAX call: reverb on (ALchemy loads no effect before) */
    int         eax_version;      /* 1..3: the EAX version the game last used (selects the source model) */
    DWORD       eax1_env;
    float       eax1_volume, eax1_decay, eax1_damping;
    eax2_listener eax2l;          /* EAX 2 / EAX 3 listener property blocks */
    eax3_listener eax3l;
    alrv_props  eax_props;        /* reverb properties they are folded into */
    BOOL        reverb_dirty;
    rl_reverb   reverb;
    float      *sendbuf;

    /* HRTF (loaded from reAL_hrtf.bin once the output rate is known) */
    rl_hrtf_set hrtf;
} rl_device;

HRESULT     rl_device_acquire(rl_device **out);
void        rl_device_release(rl_device *dev);
void        rl_device_start_voice(rl_device *dev, rl_buffer *b);   /* lock held */
void        rl_device_stop_voice(rl_device *dev, rl_buffer *b);    /* lock held */
void        rl_device_commit_3d(rl_device *dev);                    /* lock held: apply deferred 3D */
void        rl_device_addref(rl_device *dev);

/* ---------------- IDirectSound8 ---------------- */
typedef struct rl_dsound rl_dsound;
struct rl_dsound {
    IDirectSound8 IDirectSound8_iface;
    LONG          ref;
    rl_device    *dev;
    DWORD         coop_level;
    rl_buffer    *primary;
    BOOL          is_ds8;
};

HRESULT rl_dsound_create(REFIID riid, void **out, BOOL ds8);

/* ---------------- buffers ---------------- */
typedef struct rl_shared_data {
    LONG   ref;
    BYTE  *data;
    DWORD  size;
} rl_shared_data;

struct rl_buffer {
    IDirectSoundBuffer8    IDirectSoundBuffer8_iface;
    IDirectSound3DBuffer   IDirectSound3DBuffer_iface;
    IDirectSoundNotify     IDirectSoundNotify_iface;
    IKsPropertySet         IKsPropertySet_iface;
    IDirectSound3DListener IDirectSound3DListener_iface;   /* primary only */
    LONG          ref;
    int           id;
    rl_device    *dev;
    BOOL          is_primary;
    DSBUFFERDESC  desc;
    WAVEFORMATEXTENSIBLE fmt;
    rl_shared_data *shared;
    rl_voice      voice;
    LONG          volume, pan;
    DWORD         freq;         /* 0 means original */
    rl_notify    *notifies;
    DWORD         n_notifies;
    rl_buffer    *next_playing, *prev_playing;
    BOOL          in_play_list;
    rl_buffer    *next_all, *prev_all;

    /* 3D: committed + deferred copies */
    rl_source3d   src3d, src3d_pending;
    BOOL          src3d_dirty;

    rl_hrtf_voice *hrtf;        /* 3D buffers only */
    struct rl_dsound *owner;    /* IDirectSound that created it (weak); NULL once that is released */
    BOOL          dead;         /* owner released: stopped for good, like real DirectSound */

    /* EAX per-buffer */
    float         eax1_mix;     /* EAX_REVERBMIX_USEDISTANCE (-1) or 0..1 */
    eax2_buffer   eax2;
    eax3_buffer   eax3;
};

HRESULT rl_buffer_create(rl_device *dev, rl_dsound *owner, LPCDSBUFFERDESC desc, rl_buffer **out);
HRESULT rl_buffer_duplicate(rl_device *dev, rl_dsound *owner, rl_buffer *src, rl_buffer **out);
void    rl_buffers_orphan(rl_device *dev, rl_dsound *owner);   /* owner released */
void    rl_buffer_update_gains(rl_buffer *b);     /* lock held */
void    rl_signal_event(void *ev);
void    rl_device_eax1_changed(rl_device *dev);  /* lock held */
void    rl_device_eax_use(rl_device *dev, int version);   /* lock held: an EAX <version> call was made */
void    rl_device_eax_listener_changed(rl_device *dev);   /* lock held: EAX 2/3 listener properties set */

/* GUIDs not always present in import libs */
extern const GUID RL_DEVICE_GUID;

#endif
