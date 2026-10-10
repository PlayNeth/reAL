/* reAL - EAX 2.0 / 3.0 property handling, ported from ALchemy.
 *
 * Listener: the EAX 2 and EAX 3 listener property blocks ALchemy keeps per reverb effect, their
 * setters (validation, environment presets, environment-size scaling) and the conversion into the
 * reverb's property set (alrv_props).  ALchemy fns: EAX 2 setters 0x1004ae70.. 0x1004ba90, conversion
 * 0x1004bba0; EAX 3 setters 0x100444f0.. 0x100455b0, conversion 0x10045740.
 *
 * Source: the per-buffer EAX 2 / EAX 3 blocks and the conversion into the four gains ALchemy hands to a
 * voice: direct, direct HF, room (reverb send) and room HF (fns 0x1004a8a0 and 0x1004c890).
 *
 * Platform independent (no Windows headers) so it can be tested natively. */
#ifndef REAL_EAX_H
#define REAL_EAX_H

#include <stdint.h>
#include "alrv.h"

/* ------------------------------------------------------------------ property layouts (as in eax.h) */

typedef struct eax2_reverb {          /* EAX 2.0 EAXLISTENERPROPERTIES, 0x38 bytes */
    int32_t  room, room_hf;
    float    room_rolloff, decay_time, decay_hf_ratio;
    int32_t  reflections;
    float    reflections_delay;
    int32_t  reverb;
    float    reverb_delay;
    uint32_t environment;
    float    env_size, env_diffusion, air_absorption_hf;
    uint32_t flags;
} eax2_reverb;

typedef struct eax3_reverb {          /* EAX 3.0 EAXLISTENERPROPERTIES, 0x70 bytes */
    uint32_t environment;
    float    env_size, env_diffusion;
    int32_t  room, room_hf, room_lf;
    float    decay_time, decay_hf_ratio, decay_lf_ratio;
    int32_t  reflections;
    float    reflections_delay, reflections_pan[3];
    int32_t  reverb;
    float    reverb_delay, reverb_pan[3];
    float    echo_time, echo_depth, mod_time, mod_depth;
    float    air_absorption_hf, hf_reference, lf_reference, room_rolloff;
    uint32_t flags;
} eax3_reverb;

typedef struct eax2_buffer {          /* EAX 2.0 EAXBUFFERPROPERTIES, 0x34 bytes */
    int32_t  direct, direct_hf, room, room_hf;
    float    room_rolloff;
    int32_t  obstruction;
    float    obstruction_lf;
    int32_t  occlusion;
    float    occlusion_lf, occlusion_room;
    int32_t  outside_volume_hf;
    float    air_absorption;
    uint32_t flags;
} eax2_buffer;

typedef struct eax3_buffer {          /* EAX 3.0 EAXBUFFERPROPERTIES, 0x48 bytes */
    int32_t  direct, direct_hf, room, room_hf;
    int32_t  obstruction;
    float    obstruction_lf;
    int32_t  occlusion;
    float    occlusion_lf, occlusion_room, occlusion_direct;
    int32_t  exclusion;
    float    exclusion_lf;
    int32_t  outside_volume_hf;
    float    doppler, rolloff, room_rolloff, air_absorption;
    uint32_t flags;
} eax3_buffer;

/* listener flags */
#define EAXL_DECAYTIMESCALE        0x01
#define EAXL_REFLECTIONSSCALE      0x02
#define EAXL_REFLECTIONSDELAYSCALE 0x04
#define EAXL_REVERBSCALE           0x08
#define EAXL_REVERBDELAYSCALE      0x10
#define EAXL_DECAYHFLIMIT          0x20
#define EAXL_ECHOTIMESCALE         0x40
#define EAXL_MODULATIONTIMESCALE   0x80
/* buffer flags */
#define EAXB_DIRECTHFAUTO 0x1
#define EAXB_ROOMAUTO     0x2
#define EAXB_ROOMHFAUTO   0x4

/* property ids */
enum { EAX2L_NONE, EAX2L_ALL, EAX2L_ROOM, EAX2L_ROOMHF, EAX2L_ROOMROLLOFF, EAX2L_DECAYTIME, EAX2L_DECAYHFRATIO,
       EAX2L_REFLECTIONS, EAX2L_REFLECTIONSDELAY, EAX2L_REVERB, EAX2L_REVERBDELAY, EAX2L_ENVIRONMENT,
       EAX2L_ENVIRONMENTSIZE, EAX2L_ENVIRONMENTDIFFUSION, EAX2L_AIRABSORPTIONHF, EAX2L_FLAGS, EAX2L_COUNT };
enum { EAX3L_NONE, EAX3L_ALL, EAX3L_ENVIRONMENT, EAX3L_ENVIRONMENTSIZE, EAX3L_ENVIRONMENTDIFFUSION, EAX3L_ROOM,
       EAX3L_ROOMHF, EAX3L_ROOMLF, EAX3L_DECAYTIME, EAX3L_DECAYHFRATIO, EAX3L_DECAYLFRATIO, EAX3L_REFLECTIONS,
       EAX3L_REFLECTIONSDELAY, EAX3L_REFLECTIONSPAN, EAX3L_REVERB, EAX3L_REVERBDELAY, EAX3L_REVERBPAN,
       EAX3L_ECHOTIME, EAX3L_ECHODEPTH, EAX3L_MODULATIONTIME, EAX3L_MODULATIONDEPTH, EAX3L_AIRABSORPTIONHF,
       EAX3L_HFREFERENCE, EAX3L_LFREFERENCE, EAX3L_ROOMROLLOFFFACTOR, EAX3L_FLAGS, EAX3L_COUNT };
enum { EAX2B_NONE, EAX2B_ALL, EAX2B_DIRECT, EAX2B_DIRECTHF, EAX2B_ROOM, EAX2B_ROOMHF, EAX2B_ROOMROLLOFF,
       EAX2B_OBSTRUCTION, EAX2B_OBSTRUCTIONLF, EAX2B_OCCLUSION, EAX2B_OCCLUSIONLF, EAX2B_OCCLUSIONROOM,
       EAX2B_OUTSIDEVOLUMEHF, EAX2B_AIRABSORPTION, EAX2B_FLAGS, EAX2B_COUNT };
enum { EAX3B_NONE, EAX3B_ALL, EAX3B_OBSTRUCTIONPARAMS, EAX3B_OCCLUSIONPARAMS, EAX3B_EXCLUSIONPARAMS,
       EAX3B_DIRECT, EAX3B_DIRECTHF, EAX3B_ROOM, EAX3B_ROOMHF, EAX3B_OBSTRUCTION, EAX3B_OBSTRUCTIONLF,
       EAX3B_OCCLUSION, EAX3B_OCCLUSIONLF, EAX3B_OCCLUSIONROOM, EAX3B_OCCLUSIONDIRECT, EAX3B_EXCLUSION,
       EAX3B_EXCLUSIONLF, EAX3B_OUTSIDEVOLUMEHF, EAX3B_DOPPLER, EAX3B_ROLLOFF, EAX3B_ROOMROLLOFF,
       EAX3B_AIRABSORPTION, EAX3B_FLAGS, EAX3B_COUNT };
#define EAX_DEFERRED 0x80000000u

/* results of the property calls */
enum { EAX_OK = 0, EAX_BADID = 1, EAX_BADVALUE = 2, EAX_BADSIZE = 3 };

/* ------------------------------------------------------------------ listener state */

typedef struct eax2_listener {        /* ALchemy layout (effect + 0x2284) */
    eax2_reverb p;                    /* 0x00 */
    uint32_t dirty;                   /* 0x38 */
    uint32_t taken;                   /* 0x3c dirty bits consumed by the last conversion */
    float    decay_hf;                /* 0x40 */
    int32_t  b_reverb;                /* 0x44 values the environment-size scaling starts from */
    float    b_reverb_delay, b_decay; /* 0x48 0x4c */
    int32_t  b_reflections;           /* 0x50 */
    float    b_refl_delay;            /* 0x54 */
    float    s_reverb, s_reverb_delay, s_decay, s_reflections, s_refl_delay;  /* 0x58.. sizes they were set at */
} eax2_listener;

typedef struct eax3_listener {        /* ALchemy layout (effect + 0x22f0) */
    eax3_reverb p;                    /* 0x00 */
    uint32_t env_copy;                /* 0x70 environment of the last preset / ALL */
    uint32_t dirty;                   /* 0x74 */
    uint32_t taken;                   /* 0x78 */
    float    decay_hf;                /* 0x7c */
    int32_t  b_reverb;                /* 0x80 */
    float    b_reverb_delay, b_decay; /* 0x84 0x88 */
    int32_t  b_reflections;           /* 0x8c */
    float    b_refl_delay, b_echo, b_mod;                                   /* 0x90.. */
    float    s_reverb, s_reverb_delay, s_decay, s_reflections, s_refl_delay, s_echo, s_mod;  /* 0x9c.. */
} eax3_listener;

void eax2l_reset(eax2_listener *l);                      /* fn 0x1004bb40 */
void eax3l_reset(eax3_listener *l);                      /* fn 0x100456b0 */
/* Set: returns EAX_OK / EAX_BADVALUE / EAX_BADID / EAX_BADSIZE; Get copies the stored value */
int  eax2l_set(eax2_listener *l, uint32_t id, const void *data, uint32_t len);
int  eax2l_get(const eax2_listener *l, uint32_t id, void *data, uint32_t len, uint32_t *ret);
int  eax3l_set(eax3_listener *l, uint32_t id, const void *data, uint32_t len);
int  eax3l_get(const eax3_listener *l, uint32_t id, void *data, uint32_t len, uint32_t *ret);
/* fold the changed properties into the reverb property set; returns nonzero if anything changed */
int  eax2l_apply(eax2_listener *l, alrv_props *e);       /* fn 0x1004bba0 */
int  eax3l_apply(eax3_listener *l, alrv_props *e);       /* fn 0x10045740 */

/* ------------------------------------------------------------------ source */

void eax2b_defaults(eax2_buffer *b);
void eax3b_defaults(eax3_buffer *b);
int  eax2b_set(eax2_buffer *b, uint32_t id, const void *data, uint32_t len);
int  eax2b_get(const eax2_buffer *b, uint32_t id, void *data, uint32_t len, uint32_t *ret);
int  eax3b_set(eax3_buffer *b, uint32_t id, const void *data, uint32_t len);
int  eax3b_get(const eax3_buffer *b, uint32_t id, void *data, uint32_t len, uint32_t *ret);

typedef struct eax_geom {             /* what the conversions need from the DirectSound3D side */
    float x;                          /* (min(dist, max) - min) / min, 0 inside min distance */
    float cone_factor;                /* 0 inside the inner cone .. 1 outside the outer cone */
    uint32_t cone_inside, cone_outside;
    int32_t  cone_outside_volume;     /* mB */
    float distance_factor, rolloff_factor;   /* DS3D listener */
} eax_geom;

typedef struct eax_gains {
    float direct;                     /* EAX 2: multiplies the DS3D distance gain; EAX 3: includes distance */
    float direct_hf;                  /* gain at the HF reference (fed to the one-pole low-pass as is) */
    float room, room_hf;
} eax_gains;

void eax2_source_gains(const eax2_buffer *b, const eax2_listener *l, const eax_geom *g, eax_gains *out);
void eax3_source_gains(const eax3_buffer *b, const eax3_listener *l, const eax_geom *g, eax_gains *out);

/* the cone power average ROOMAUTO uses (fn 0x100434c0), mB */
float eax_cone_auto(const eax_geom *g, int32_t extra_mb);

/* ALchemy's one-pole low-pass for a gain at f/fs (fn 0x10031000): returns the input weight a of
 * y += a (x - y), quantised to 1/32768 as ALchemy does; 1 = no filtering */
float eax_lp_coef(float gain, float f_over_fs);

/* ------------------------------------------------------------------ EAX 4.0 (ALchemy fns in docs) */

typedef struct eax_guid { uint32_t d[4]; } eax_guid;          /* compared as 4 dwords, like ALchemy */
extern const eax_guid EAX_NULL_GUID, EAX_PRIMARY_SLOT_GUID;
extern const eax_guid EAX4_SLOT_GUID[4];                        /* EAXPROPERTYID_EAX40_FXSlot0..3 */
extern const eax_guid EAX4_EFFECT_GUID[13];                     /* [type]; [0] = NULL (no effect) */
int eax4_slot_index(const eax_guid *g);                          /* 0..3, or -1 */

enum { EAX4FX_NONE, EAX4FX_AGCCOMPRESSOR, EAX4FX_AUTOWAH, EAX4FX_CHORUS, EAX4FX_DISTORTION, EAX4FX_ECHO,
       EAX4FX_EQUALIZER, EAX4FX_FLANGER, EAX4FX_FREQSHIFTER, EAX4FX_PITCHSHIFTER, EAX4FX_REVERB,
       EAX4FX_RINGMODULATOR, EAX4FX_VOCALMORPHER, EAX4FX_COUNT };

/* EAX 4 last-error codes (EAXCONTEXT_LASTERROR) */
enum { EAX4ERR_OK = 0, EAX4ERR_INVALID_OPERATION = -1, EAX4ERR_INVALID_VALUE = -2, EAX4ERR_NO_EFFECT = -3,
       EAX4ERR_UNKNOWN_EFFECT = -4 };

/* property ids */
enum { EAX4C_NONE, EAX4C_ALL, EAX4C_PRIMARYFXSLOTID, EAX4C_DISTANCEFACTOR, EAX4C_AIRABSORPTIONHF,
       EAX4C_HFREFERENCE, EAX4C_LASTERROR, EAX4C_COUNT };
enum { EAX4FXS_NONE = 0x10000, EAX4FXS_ALL, EAX4FXS_LOADEFFECT, EAX4FXS_VOLUME, EAX4FXS_LOCK, EAX4FXS_FLAGS };
enum { EAX4S_NONE, EAX4S_ALL, EAX4S_OBSTRUCTIONPARAMS, EAX4S_OCCLUSIONPARAMS, EAX4S_EXCLUSIONPARAMS,
       EAX4S_DIRECT, EAX4S_DIRECTHF, EAX4S_ROOM, EAX4S_ROOMHF, EAX4S_OBSTRUCTION, EAX4S_OBSTRUCTIONLF,
       EAX4S_OCCLUSION, EAX4S_OCCLUSIONLF, EAX4S_OCCLUSIONROOMRATIO, EAX4S_OCCLUSIONDIRECTRATIO, EAX4S_EXCLUSION,
       EAX4S_EXCLUSIONLF, EAX4S_OUTSIDEVOLUMEHF, EAX4S_DOPPLERFACTOR, EAX4S_ROLLOFFFACTOR, EAX4S_ROOMROLLOFFFACTOR,
       EAX4S_AIRABSORPTIONFACTOR, EAX4S_FLAGS, EAX4S_SENDPARAMS, EAX4S_ALLSENDPARAMS, EAX4S_OCCLUSIONSENDPARAMS,
       EAX4S_EXCLUSIONSENDPARAMS, EAX4S_ACTIVEFXSLOTID, EAX4S_COUNT };
#define EAX4SLOT_ENVIRONMENT 0x1

typedef struct eax4_context {          /* ALchemy: ctx + 0x8c */
    eax_guid primary;                  /* EAX40CONTEXTPROPERTIES */
    float    distance_factor, air_absorption_hf, hf_reference;
    int32_t  last_error;               /* +0x1c */
    uint32_t dirty;                    /* +0x20 */
} eax4_context;

typedef struct eax4_slot {
    eax_guid effect;                   /* EAX40FXSLOTPROPERTIES */
    int32_t  volume, lock;
    uint32_t flags;
    int      type;                     /* EAX4FX_* of the loaded effect */
    int      system_lock;              /* slots 0/1: loaded and locked by the system, games cannot change them */
    uint32_t dirty;
    eax3_listener reverb;              /* EAX4FX_REVERB properties (same layout and setters as EAX 3) */
    uint32_t fx[16];                   /* the other effects' properties */
} eax4_slot;

typedef struct eax4_send {             /* per slot, ALchemy source block + 0x50 + 0x20 * slot */
    int32_t  room, room_hf, occlusion;
    float    occlusion_lf, occlusion_room_ratio, occlusion_direct_ratio;
    int32_t  exclusion;
    float    exclusion_lf;
} eax4_send;

typedef struct eax4_source {           /* ALchemy: buffer + 0x218 (+8) */
    eax3_buffer p;                     /* EAX40SOURCEPROPERTIES: same layout as EAX 3 */
    eax4_send   send[4];
    eax_guid    active[2];             /* EAXSOURCE_ACTIVEFXSLOTID */
    uint32_t    dirty, send_dirty[4];
} eax4_source;

typedef struct eax4_gains {
    float direct, direct_hf;           /* direct path (includes distance roll-off) */
    float send[4], send_hf[4];         /* per FX slot; 0 when the slot is not active for this source */
    float hf_reference;                /* Hz, for every HF filter of the voice */
    float doppler_factor;              /* listener x source factor, capped at 10 */
    float distance_factor;             /* DS3D x context distance factor (Doppler speed of sound) */
} eax4_gains;

void eax4_context_reset(eax4_context *c);                         /* fn 0x1004f670 */
int  eax4c_set(eax4_context *c, uint32_t id, const void *data, uint32_t len);
int  eax4c_get(eax4_context *c, uint32_t id, void *data, uint32_t len, uint32_t *ret);
/* version switch to EAX 4 (fn 0x10042220(4) + 0x10050850): slot 0 reverb, slot 1 chorus, both locked */
void eax4_slots_init(eax4_slot slots[4]);
/* slot properties; errors are also recorded in the context's last error */
int  eax4fx_set(eax4_slot *s, eax4_context *c, uint32_t id, const void *data, uint32_t len);
int  eax4fx_get(const eax4_slot *s, eax4_context *c, uint32_t id, void *data, uint32_t len, uint32_t *ret);
void eax4_source_defaults(eax4_source *s);                         /* fn 0x1004d5e0 */
int  eax4s_set(eax4_source *s, eax4_context *c, uint32_t id, const void *data, uint32_t len);
int  eax4s_get(const eax4_source *s, eax4_context *c, uint32_t id, void *data, uint32_t len, uint32_t *ret);
/* the other effects' properties (src/eax4fx.c): count, defaults, set/get, a field as float */
int   eax4fx_effect_props(int type);
void  eax4fx_effect_defaults(int type, uint32_t fx[16]);
int   eax4fx_effect_set(int type, uint32_t fx[16], uint32_t id, const void *data, uint32_t len);
int   eax4fx_effect_get(int type, const uint32_t fx[16], uint32_t id, void *data, uint32_t len, uint32_t *ret);
float eax4fx_value(int type, const uint32_t fx[16], int i);
/* the voice gains (fn 0x1004e7e0); geom->distance_factor is the DS3D listener's */
void eax4_source_gains(const eax4_source *s, const eax4_context *c, const eax4_slot slots[4], const eax_geom *g,
                       float ds3d_doppler, eax4_gains *out);

#endif
