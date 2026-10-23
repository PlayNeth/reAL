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

#define REAL_VERSION "1.2.1"

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
    int  resampler;             /* RL_INTERP_* (default sinc) */
    uint32_t speakers;          /* output layout (RL_LAYOUT_*); 0 = the device's (ALchemy: Windows' speaker setup) */
    int  headphones;            /* -1 = the device's form factor, 0 = speakers, 1 = headphones */
    int  cmss;                  /* CMSS-3D: upmix 2D stereo buffers (ALchemy: off) */
    int  cmss_intensity;        /* its rear/side level, percent -100..99 (0 = ALchemy's default) */
    int  music;                 /* reDM: serve DirectMusic (loader/performance) ourselves */
    char gm_dls[MAX_PATH];      /* custom GM collection; "" = the system's gm.dls */
    int  midi_trace;            /* log the game's winmm MIDI / MCI calls */
    int  midi;                  /* reDM GM Synth as the MIDI mapper / an extra MIDI output */
    int  midi_rate;             /* its sample rate */
    int  midi_reverb;           /* reverb send (CC91) through the Waves reverb */
} rl_config;

extern rl_config g_cfg;
extern HMODULE   g_self;

void rl_config_load(void);

/* ---------------- logging ---------------- */
void rl_log_init(void);
void rl_log_flush(void);
void rl_crash_logger(void);
void rl_log_shutdown(int process_exit);    /* DLL_PROCESS_DETACH: the crash handler removed, the log closed */
void *rl_hotpatch(void *fn, void *repl, const char *name);   /* returns a pointer that runs the original */
void rl_midi_hook(int can_load);
void rl_log(int level, const char *fmt, ...);
#define LOG_INFO(...)  rl_log(1, __VA_ARGS__)
#define LOG_CALL(...)  do { if (g_cfg.log_level >= 2) rl_log(2, __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (g_cfg.log_level >= 3) rl_log(3, __VA_ARGS__); } while (0)
#define LOG_3D(...)    do { if (g_cfg.log_3d) rl_log(3, __VA_ARGS__); } while (0)
#define LOG_EAX(...)   do { if (g_cfg.log_eax) rl_log(3, __VA_ARGS__); } while (0)
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
    UINT32      out_channels;     /* channels in the mix: layout.n */
    rl_layout   layout;           /* the speaker layout mixed (ALchemy's: stereo, quad, 5.1 or 7.1) */
    rl_layout   dev_layout;       /* the device's own, when different (folded with out_mtx at the output) */
    BOOL        out_direct;       /* the device takes the layout as it is */
    BOOL        headphones;       /* stereo layout on headphones (GetSpeakerConfig: DSSPEAKER_HEADPHONE) */
    float       out_mtx[RL_MAXOUT][RL_MAXOUT];
    float      *fxtmp;            /* stereo scratch for the EAX 4 effects on more than two channels */
    UINT32      period_frames;    /* approx frames mixed per wakeup */
    rl_buffer  *playing;          /* intrusive list of playing buffers */
    rl_buffer  *all;              /* intrusive list of all buffers (for 3D commits) */
    rl_listener3d listener;       /* committed listener state */
    rl_listener3d listener_pending; /* includes DS3D_DEFERRED changes */
    BOOL        listener_dirty;
    BOOL        primary_playing;
    LONG        primary_volume;
    float       *mixbuf;          /* the mix: out_channels interleaved */
    UINT32      mixbuf_frames;
    WAVEFORMATEX primary_fmt;
    DWORD       speaker_config;   /* what GetSpeakerConfig reports */
    BOOL        speaker_config_set;   /* the application set it (SetSpeakerConfig: kept across device reopens) */
    LONG        n_buffers;        /* live secondary buffers */
    LONG        n_hw_buffers;     /* live non-LOCSOFTWARE secondaries (ALchemy counts these in GetCaps) */

    /* EAX */
    BOOL        eax_used;         /* game made an EAX call: reverb on (ALchemy loads no effect before) */
    int         eax_version;      /* 1..4: the EAX version the game last used (selects the source model) */
    DWORD       eax1_env;
    float       eax1_volume, eax1_decay, eax1_damping;
    eax2_listener eax2l;          /* EAX 2 / EAX 3 listener property blocks */
    eax3_listener eax3l;
    alrv_props  eax_props;        /* reverb properties they are folded into */
    BOOL        reverb_dirty;
    rl_reverb   reverb;           /* FX slot 0's reverb (the only one before EAX 4) */
    float      *sendbuf;          /* RL_SENDS effect buses of mixbuf_frames each; bus 0 feeds `reverb` */
    BOOL        eax_commit;       /* an immediate EAX set: recompute every source (applied by the mixer) */
    /* EAX 4: context and FX slots; slots 1..3 get their own reverb when one is loaded into them */
    BOOL        eax4_init;
    eax4_context eax4c;
    eax4_slot   eax4s[4];
    rl_reverb  *xreverb[3];
    alrv_props  xprops[3];
    float       fx_gain[4];       /* slot volume, linear */
    struct eax4fx_unit *fxunit[4];/* the other effects (eax4dsp.c), created by the mixer thread */

    /* HRTF (loaded from reAL_hrtf.bin once the output rate is known) */
    rl_hrtf_set hrtf;
} rl_device;

HRESULT     rl_device_acquire(rl_device **out);
void        rl_device_release(rl_device *dev);
void        rl_device_start_voice(rl_device *dev, rl_buffer *b);   /* lock held */
void        rl_device_stop_voice(rl_device *dev, rl_buffer *b);    /* lock held */
void        rl_device_commit_3d(rl_device *dev);                    /* lock held: apply deferred 3D */
void        rl_device_addref(rl_device *dev);
rl_device  *rl_device_grave(void);   /* where orphaned buffers live on (rl_buffers_orphan): no thread, never freed */

/* ---------------- IDirectSound8 ---------------- */
typedef struct rl_dsound rl_dsound;
struct rl_dsound {
    IDirectSound8 IDirectSound8_iface;
    LONG          ref;
    rl_device    *dev;
    DWORD         coop_level;
    rl_buffer    *primary;
    BOOL          is_ds8;
    IDirectSound8 *real_ds;     /* the system's DirectSound, for DirectMusic's private interfaces */
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
    BOOL          sink;         /* fed by the DirectMusic sink: always played at its own rate; its frequency and
                                 * Doppler shift go to the synth as pitch (rl_buffer_sink_cents) */
    int32_t       sink_cents;
    DWORD         sink_caps;    /* GetCaps flags of a sink buffer (DirectSound reports the ones DirectMusic asked for) */
    rl_notify    *notifies;
    DWORD         n_notifies;
    rl_buffer    *next_playing, *prev_playing;
    BOOL          in_play_list;
    rl_buffer    *next_all, *prev_all;

    /* 3D: committed + deferred copies */
    rl_source3d   src3d, src3d_pending;
    BOOL          src3d_dirty;

    rl_hrtf_voice *hrtf;        /* 3D buffers only */
    UINT32        cmss_rate, cmss_layout;   /* what voice.cmss was made for */
    struct rl_dsound *owner;    /* IDirectSound that created it (weak); NULL once that is released */
    BOOL          dead;         /* owner released: stopped for good, like real DirectSound */

    /* effects (SetFX): processed into a copy of the buffer ahead of the play cursor, which the voice plays */
    struct rl_fxchain *fx;
    BYTE         *fxdata;
    uint32_t      fx_head;      /* next frame to process (== frames: the end of a non-looping buffer reached) */
    uint32_t      fx_lead;      /* frames processed ahead of the play cursor */
    uint32_t      fx_last;      /* the play cursor (frames) when that was worked out */
    LONG          locks;        /* Lock calls not yet unlocked (SetFX is refused meanwhile) */
    BOOL          fx_placed;    /* a LOCDEFER buffer's effects have their resources (Play / AcquireResources to Stop) */

    /* EAX per-buffer */
    float         eax1_mix;     /* EAX_REVERBMIX_USEDISTANCE (-1) or 0..1 */
    eax2_buffer   eax2;
    eax3_buffer   eax3;
    eax4_source   eax4;
};

HRESULT rl_buffer_create(rl_device *dev, rl_dsound *owner, LPCDSBUFFERDESC desc, rl_buffer **out);
void rl_buffer_set_sink(rl_buffer *b, DWORD caps);
int32_t rl_buffer_sink_cents(rl_buffer *b);
HRESULT rl_buffer_duplicate(rl_device *dev, rl_dsound *owner, rl_buffer *src, rl_buffer **out);
void    rl_buffers_orphan(rl_device *dev, rl_dsound *owner);   /* owner released */
void    rl_buffer_update_gains(rl_buffer *b);     /* lock held */
void    rl_buffer_fx_run(rl_buffer *b, UINT32 out_frames, UINT32 out_rate);   /* lock held: effects ahead of the mix */
/* a known FPU state for float work on the game's threads (and the game's own state back after) */
unsigned int rl_fpu_enter(void);
void    rl_fpu_leave(unsigned int cw);
void    rl_signal_event(void *ev);
void    rl_device_eax1_changed(rl_device *dev);  /* lock held */
void    rl_device_eax_use(rl_device *dev, int version);   /* lock held: an EAX <version> call was made */
void    rl_device_eax_listener_changed(rl_device *dev);   /* lock held: EAX 2/3 listener properties set */

/* GUIDs not always present in import libs */
extern const GUID RL_DEVICE_GUID;

#endif
