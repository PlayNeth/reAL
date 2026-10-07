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
} rl_device;

HRESULT     rl_device_acquire(rl_device **out);
void        rl_device_release(rl_device *dev);
void        rl_device_start_voice(rl_device *dev, rl_buffer *b);   /* lock held */
void        rl_device_stop_voice(rl_device *dev, rl_buffer *b);    /* lock held */
void        rl_device_commit_3d(rl_device *dev);                    /* lock held: apply deferred 3D */
void        rl_device_addref(rl_device *dev);

/* ---------------- IDirectSound8 ---------------- */
typedef struct rl_dsound {
    IDirectSound8 IDirectSound8_iface;
    LONG          ref;
    rl_device    *dev;
    DWORD         coop_level;
    rl_buffer    *primary;
    BOOL          is_ds8;
} rl_dsound;

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
};

HRESULT rl_buffer_create(rl_device *dev, LPCDSBUFFERDESC desc, rl_buffer **out);
HRESULT rl_buffer_duplicate(rl_device *dev, rl_buffer *src, rl_buffer **out);
void    rl_buffer_update_gains(rl_buffer *b);     /* lock held */
void    rl_signal_event(void *ev);

/* GUIDs not always present in import libs */
extern const GUID RL_DEVICE_GUID;

#endif
