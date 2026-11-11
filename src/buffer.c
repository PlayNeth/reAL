/* reAL - IDirectSoundBuffer8 + IDirectSound3DBuffer + IDirectSoundNotify + IKsPropertySet
 *        + IDirectSound3DListener (primary buffer)
 * One translation unit: this file holds the helpers, a released buffer's interfaces and creation, and includes
 * buffer8.c (IDirectSoundBuffer8, IDirectSoundNotify), buffer3d.c (3D buffer, listener) and bufferprop.c (IKsPropertySet,
 * EAX) in order. */
#include "real.h"
#include "fx.h"
#include "cmss3.h"
#include "virt.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <float.h>
#include <string.h>

#ifndef E_PROP_ID_UNSUPPORTED
#define E_PROP_ID_UNSUPPORTED ((HRESULT)0x80070490L)
#endif

static const GUID RL_KSDATAFORMAT_SUBTYPE_PCM =
    { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID RL_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static LONG s_next_id;

#define BUF(iface)   impl_from(iface, rl_buffer, IDirectSoundBuffer8_iface)
#define BUF3D(iface) impl_from(iface, rl_buffer, IDirectSound3DBuffer_iface)
#define NOTIFY(iface) impl_from(iface, rl_buffer, IDirectSoundNotify_iface)
#define PROPS(iface) impl_from(iface, rl_buffer, IKsPropertySet_iface)
#define LISTENER(iface) impl_from(iface, rl_buffer, IDirectSound3DListener_iface)

void rl_signal_event(void *ev) { if (ev) SetEvent((HANDLE)ev); }

/* ======================================================================= helpers */

/* Runs on the game's threads: some games unmask FPU exceptions or drop the x87 to 24-bit precision
 * (Direct3D does). Compute with a known FPU state and give the game its own back. */
static void update_gains(rl_buffer *b);

unsigned int rl_fpu_enter(void)
{
    unsigned int cw = _controlfp(0, 0);
    _controlfp(_MCW_EM | _PC_53 | _RC_NEAR, _MCW_EM | _MCW_PC | _MCW_RC);
    return cw;
}

void rl_fpu_leave(unsigned int cw)
{
    _clearfp();
    _controlfp(cw, _MCW_EM | _MCW_PC | _MCW_RC);
}

static DWORD orig_rate(rl_buffer *b);

/* DSSCL_WRITEPRIMARY: the application writes the final mix into the primary buffer itself, as Windows' DirectSound
 * lets it (ALchemy does not: its primary buffer plays nothing).  The primary buffer then holds PRIMARY_BYTES in the
 * primary format, played round and round as a 2D voice, and its owner's secondary buffers are lost.  What Windows does
 * is in tests/primtest.c. */
#define PRIMARY_BYTES 32768
/* (lock held) whether the buffer's owner is at DSSCL_WRITEPRIMARY */
static BOOL writeprimary(const rl_buffer *b) { return b->owner && b->owner->coop_level == DSSCL_WRITEPRIMARY; }
/* (lock held) a DirectSound object of the device writes the primary buffer: every secondary buffer is lost, any
 * object's (Windows 7: tests/primtest.c lost) */
static BOOL device_writeprimary(const rl_device *dev) { return dev->wp_owners > 0; }

/* a float that is neither infinite nor NaN, by its exponent bits: the build's fast math takes every float as finite
 * and folds isfinite() away (the empty asm hides where u came from) */
static BOOL is_finite(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    __asm__("" : "+r"(u));
    return (u & 0x7f800000u) != 0x7f800000u;
}
static BOOL finite3(float x, float y, float z) { return is_finite(x) && is_finite(y) && is_finite(z); }
/* NaN, by its bits (x != x folds away under fast math) */
static BOOL is_nan(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    __asm__("" : "+r"(u));
    return (u & 0x7fffffffu) > 0x7f800000u;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* a minimum or maximum distance: finite and not negative (ALchemy 0x10042a90); not zero either, as Windows (a fix) */
static BOOL distance_ok(float d) { return is_finite(d) && d >= 0.0f && (!g_cfg.fixes || d > 0.0f); }
/* a direction: finite, and not all zero (ALchemy 0x10042960, 0x10043740) */
static BOOL direction_ok(float x, float y, float z) { return finite3(x, y, z) && !(x == 0.0f && y == 0.0f && z == 0.0f); }
static HRESULT parse_format(const WAVEFORMATEX *wfx, WAVEFORMATEXTENSIBLE *out, rl_sfmt *sfmt);
/* (lock held) the primary buffer's voice in the device's primary format; nothing plays of a format it cannot read */
static void prim_format(rl_buffer *b)
{
    rl_sfmt sf;
    WAVEFORMATEXTENSIBLE f;
    if (!b->shared || FAILED(parse_format(&b->dev->primary_fmt, &f, &sf)) || f.Format.nChannels > 2) {
        b->voice.size = 0;
        b->voice.block = 0;
        b->voice.playing = 0;
        if (b->in_play_list) rl_device_stop_voice(b->dev, b);
        return;
    }
    b->fmt = f;
    b->voice.data = b->shared->data;
    b->voice.interp = g_cfg.resampler;
    b->voice.channels = f.Format.nChannels;
    b->voice.block = f.Format.nBlockAlign;
    b->voice.sfmt = sf;
    b->voice.rate = f.Format.nSamplesPerSec;
    b->voice.size = PRIMARY_BYTES - PRIMARY_BYTES % b->voice.block;
    if ((b->voice.pos >> 32) >= rl_voice_frames(&b->voice)) b->voice.pos = 0;
    b->voice.hist = 0;
    rl_buffer_update_gains(b);
}
void rl_buffer_update_gains(rl_buffer *b)
{
    unsigned int cw = rl_fpu_enter();
    update_gains(b);
    rl_fpu_leave(cw);
}

static void route_speakers(rl_buffer *b, BOOL spatial, BOOL use_hrtf, const float pan[2], float level);

static void update_gains(rl_buffer *b)
{
    rl_device *dev = b->dev;
    b->eax4_pending = FALSE;            /* (its EAX 4 values, set deferred or not, are read now) */
    float base = rl_db100_to_gain(b->volume);
    BOOL is3d = !b->is_primary && (b->desc.dwFlags & DSBCAPS_CTRL3D) != 0;   /* (the primary's: plain 2D, unpanned) */
    BOOL spatial = is3d && b->src3d.mode != RL_DS3DMODE_DISABLE;
    BOOL use_hrtf = spatial && b->hrtf && dev->hrtf.dirs;
    rl_geom geo;
    float pan[2];
    if (is3d) rl_spatial_geometry(&dev->listener, &b->src3d, &geo);    /* (read only for a 3D buffer) */
    else memset(&geo, 0, sizeof(geo));
    if (spatial) { pan[0] = geo.pan[0]; pan[1] = geo.pan[1]; }
    else rl_pan_gains(b->is_primary ? 0 : b->pan, pan);
    /* stereo speakers, no HRTF: ALchemy's levels for the direction (its HRTF renderer's, measured: spatial.c).  The
     * equal-power pan above stays for headphones and four or more speakers, not measured yet */
    if (spatial && !use_hrtf && dev->layout.n <= 2 && !dev->headphones) {
        float az, el;
        rl_spatial_direction(&dev->listener, &b->src3d, &az, &el);
        rl_spatial_stereo_pan(az, el, b->sink, pan);  /* (a DirectMusic sink buffer: Microsoft's, Windows' levels) */
    }

    /* direct path level, EAX high-frequency damping and reverb send; a 2D buffer has no distance or cone (its 3D
     * state is the default, at the origin: the listener's distance from there used to attenuate it) */
    float direct = is3d ? geo.dist_gain * geo.cone_gain : 1.0f, send = 0.0f;
    float lp_a = 1.0f, send_lp_a = 1.0f;
    BOOL eax4 = FALSE;
    float eax4_dop = 1.0f, eax4_df = 1.0f;
    if (is3d && dev->eax_used && (dev->eax_version == 2 || dev->eax_version == 3)) {
        /* ALchemy's EAX 2 / 3 source model (fns 0x1004a8a0 / 0x1004c890 and the voice update 0x10040ee0) */
        eax_geom eg = { geo.x, geo.cone_factor, b->src3d.cone_inside, b->src3d.cone_outside,
                        b->src3d.cone_outside_volume, dev->listener.distance_factor, dev->listener.rolloff_factor };
        eax_gains o;
        float ref;
        if (dev->eax_version == 2) {
            eax2_source_gains(&b->eax2, &dev->eax2l, &eg, &o);
            direct = geo.dist_gain * geo.cone_gain * o.direct;       /* EAX 2 keeps DS3D's distance gain */
            ref = 5000.0f;
        } else {
            eax3_source_gains(&b->eax3, &dev->eax3l, &eg, &o);
            direct = o.direct * geo.cone_gain;                       /* EAX 3 computes the roll-off itself */
            ref = dev->eax3l.p.hf_reference;
        }
        if (direct > 1.0f) direct = 1.0f;
        if (geo.muted) direct = 0.0f;
        float rate = dev->out_rate ? (float)dev->out_rate : 48000.0f;
        float f = ref / rate;
        if (f > 0.5f) f = 0.5f;
        lp_a = eax_lp_coef(o.direct_hf, f);
        send_lp_a = eax_lp_coef(o.room_hf, f);
        send = geo.muted ? 0.0f : base * o.room;
    } else if (is3d && dev->eax_used && dev->eax_version == 4) {
        /* ALchemy's EAX 4 source model (fn 0x1004e7e0, voice update 0x10040ee0 case 4): a direct path and
         * one send per FX slot, every HF filter at the context's (or primary reverb's) HF reference */
        eax_geom eg = { geo.x, geo.cone_factor, b->src3d.cone_inside, b->src3d.cone_outside,
                        b->src3d.cone_outside_volume, dev->listener.distance_factor, dev->listener.rolloff_factor };
        eax4_gains o;
        eax4_source_gains(&b->eax4, &dev->eax4c, dev->eax4s, &eg, dev->listener.doppler_factor, &o);
        direct = o.direct * geo.cone_gain;
        if (direct > 1.0f) direct = 1.0f;
        if (geo.muted) direct = 0.0f;
        float rate = dev->out_rate ? (float)dev->out_rate : 48000.0f;
        float f = o.hf_reference / rate;
        if (f > 0.5f) f = 0.5f;
        lp_a = eax_lp_coef(o.direct_hf, f);
        for (int k = 0; k < RL_SENDS; k++) {
            b->voice.send_gain[k] = geo.muted ? 0.0f : base * o.send[k];
            b->voice.send_lp_a[k] = eax_lp_coef(o.send_hf[k], f);
        }
        eax4 = TRUE;
        eax4_dop = o.doppler_factor;
        eax4_df = o.distance_factor;
    } else if (is3d && dev->eax_used) {
        /* EAX 1.0: EAX_REVERBMIX_USEDISTANCE (-1) currently means full send */
        float mix = b->eax1_mix < 0.0f ? 1.0f : b->eax1_mix;
        send = base * direct * mix;
    }
    /* Doppler: EAX 3 adds the buffer's own factor to the listener's (ALchemy caps the sum at 10) */
    float dop = dev->listener.doppler_factor;
    if (is3d && dev->eax_used && dev->eax_version == 3) {
        dop += b->eax3.doppler;
        if (dop > 10.0f) dop = 10.0f;
    }
    if (eax4) {
        /* EAX 4: listener x source factor; the speed of sound scales with the context's distance factor too */
        rl_listener3d l = dev->listener;
        l.distance_factor = eax4_df;
        b->voice.pitch = spatial ? rl_spatial_doppler(&l, &b->src3d, eax4_dop) : 1.0f;
    } else {
        b->voice.pitch = spatial ? rl_spatial_doppler(&dev->listener, &b->src3d, dop) : 1.0f;
    }
    /* (a fix: an overflowing Doppler shift, from speeds near the float range, never reaches the resampler) */
    if (g_cfg.fixes && (!is_finite(b->voice.pitch) || b->voice.pitch <= 0.0f)) b->voice.pitch = 1.0f;

    if (b->sink) {
        /* a sink buffer keeps its rate: the shift goes to the synth as a per-bus pitch offset (DirectX 8 sink).  These
         * are Microsoft DirectSound's buffers even under ALchemy, so the Doppler is DirectSound's (whole Hz), and the
         * offset is dsound's (6efd6bbe): cents = trunc(1200 * log2(frequency / sink rate)) */
        DWORD rate = orig_rate(b), f = b->freq ? b->freq : rate;
        if (spatial) f = rl_spatial_doppler_ds(&dev->listener, &b->src3d, f);
        b->sink_cents = rl_sink_cents(f, rate);
        b->voice.pitch = 1.0f;
    }
    b->voice.lp_a = lp_a;
    if (!eax4) {
        b->voice.send_lp_a[0] = send_lp_a;
        b->voice.send_gain[0] = send;
        for (int k = 1; k < RL_SENDS; k++) b->voice.send_gain[k] = 0.0f;
    }

    if (use_hrtf) {
        /* HRTF carries the direction; only distance/cone attenuation and volume are gains */
        float az, el;
        rl_hrtf_filter f;
        rl_spatial_direction(&dev->listener, &b->src3d, &az, &el);
        rl_hrtf_lookup(&dev->hrtf, az, el, &f);
        rl_hrtf_voice_set(b->hrtf, &f);
        pan[0] = pan[1] = 1.0f;
        b->voice.hrtf = b->hrtf;
        b->voice.hrtf_set = &dev->hrtf;
    } else {
        b->voice.hrtf = NULL;
    }
    /* positioned: the mono sum of every channel.  ALchemy plays a stereo 3D buffer that way with 3D disabled too
     * (tests/levelprobe.c near: both speakers the same, 3.5 dB below the 2D buffer); with the fixes it stays stereo,
     * as a DirectMusic sink buffer, Microsoft's, does */
    b->voice.mono3d = spatial ? b->voice.channels >= 2 : is3d && !g_cfg.fixes && !b->sink && b->voice.channels == 2;
    b->voice.gain[0] = pan[0] * direct * base;
    b->voice.gain[1] = pan[1] * direct * base;
    /* (a fix: a NaN gain would carry into the mix, the reverb and every later block) */
    if (g_cfg.fixes)
        for (int k = 0; k < 2; k++) if (!is_finite(b->voice.gain[k])) b->voice.gain[k] = 0.0f;
    route_speakers(b, spatial, use_hrtf, pan, direct * base);
}

static void cmss_free(rl_voice *v)
{
    rl_cmss3_destroy(v->cmss);
    v->cmss = NULL;
    if (v->cmss_virt) { rl_virt_free(v->cmss_virt); free(v->cmss_virt); v->cmss_virt = NULL; }
}

/* CMSS-3D: ALchemy gives each 2D stereo buffer its own upmixer (fn 0x10032790) when it is on for the speaker
 * mode; on a stereo device the upmixer's quad goes through the converter's stereo virtualizer: mode 3 on
 * speakers (with the upmixer's rear allpasses), mode 0 on headphones. */
static void route_cmss(rl_buffer *b)
{
    rl_device *dev = b->dev;
    rl_voice *v = &b->voice;
    const rl_layout *lay = &dev->layout;
    BOOL want = g_cfg.cmss && v->channels == 2 && !b->is_primary && !(b->desc.dwFlags & DSBCAPS_CTRL3D) &&
                lay->n >= 2 && dev->out_rate;
    uint32_t layout = lay->mask | (lay->n == 2 && dev->headphones ? 0x80000000u : 0);
    if (v->cmss && (!want || b->cmss_rate != dev->out_rate || b->cmss_layout != layout)) cmss_free(v);
    if (!want || v->cmss) return;
    BOOL hp = lay->n == 2 && dev->headphones;
    v->cmss = rl_cmss3_create(dev->out_rate, lay->mask, lay->n == 2 && !hp);
    if (!v->cmss) return;
    rl_cmss3_set_intensity(v->cmss, g_cfg.cmss_intensity);
    if (lay->n == 2) {
        v->cmss_virt = calloc(1, sizeof(rl_virt));
        if (!v->cmss_virt) { cmss_free(v); return; }
        if (hp) {
            if (!rl_virt_init_hp(v->cmss_virt, dev->out_rate)) { cmss_free(v); return; }
        } else {
            rl_virt_init(v->cmss_virt, dev->out_rate);
        }
    }
    for (int bit = 0; bit < 12; bit++) v->cmss_idx[bit] = lay->idx[bit];
    b->cmss_rate = dev->out_rate;
    b->cmss_layout = layout;
}

/* more than two channels on either side: the voice's way onto the speakers (speakers.c) */
static void route_speakers(rl_buffer *b, BOOL spatial, BOOL use_hrtf, const float pan[2], float level)
{
    rl_device *dev = b->dev;
    rl_voice *v = &b->voice;
    const rl_layout *lay = &dev->layout;
    const int was_mc = v->mc_on;
    v->route_on = v->mc_on = 0;
    route_cmss(b);
    if (!lay->n) return;
    if (v->channels > 2 && !spatial) {
        /* ALchemy's 2D stream: every channel through the converter, the level applied after it */
        DWORD mask = 0;
        if (b->fmt.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) mask = b->fmt.dwChannelMask & 0xfff;
        if (!mask || (uint32_t)__builtin_popcount(mask) != v->channels) mask = rl_default_mask(v->channels);
        rl_layout in;
        rl_layout_init(&in, mask);
        int redirect;
        rl_convert_matrix(&in, lay, v->mc, &redirect);
        v->mc_lfe = redirect ? in.idx[3] : -1;
        /* coefficients for the current rate; the envelope carries on (a gain update must not reset it) */
        float env = v->lfe_lim.env;
        rl_bass_limiter_init(&v->lfe_lim, dev->out_rate ? dev->out_rate : 48000);
        if (was_mc) v->lfe_lim.env = env;
        v->mc_on = 1;
        v->gain[0] = v->gain[1] = level;
        return;
    }
    if (!spatial || lay->n <= 2) return;
    /* a 3D source on four or more speakers (fn 0x10034900): its pair (HRTF or panned) on the front and rear
     * speakers, its mono sum on the centre, LFE and side ones */
    float az, el, g[12];
    rl_spatial_direction(&dev->listener, &b->src3d, &az, &el);
    if (az > 180.0f) az -= 360.0f;
    double dist = (double)rl_spatial_distance(&dev->listener, &b->src3d) * dev->listener.distance_factor;
    rl_speaker_gains3d(lay->mask, lay->n, az, el, dist, g);
    float pl = use_hrtf ? 1.0f : pan[0], pr = use_hrtf ? 1.0f : pan[1];
    memset(v->rt, 0, sizeof(v->rt));
    for (int bit = 0; bit < 12; bit++) {
        int c = lay->idx[bit];
        if (c < 0 || !g[bit]) continue;
        switch (bit) {
        case 0: case 4: v->rt[0][c] = g[bit] * pl; break;    /* front / rear left */
        case 1: case 5: v->rt[1][c] = g[bit] * pr; break;    /* front / rear right */
        default: v->rt[2][c] = g[bit]; break;                /* centre, LFE, sides */
        }
    }
    v->route_on = 1;
    v->gain[0] = v->gain[1] = level;
}

static DWORD orig_rate(rl_buffer *b) { return b->fmt.Format.nSamplesPerSec; }

static HRESULT parse_format(const WAVEFORMATEX *wfx, WAVEFORMATEXTENSIBLE *out, rl_sfmt *sfmt)
{
    if (!wfx) return DSERR_INVALIDPARAM;
    ZeroMemory(out, sizeof(*out));
    WORD tag = wfx->wFormatTag;
    WORD bits = wfx->wBitsPerSample;
    int is_float = 0;

    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (wfx->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return DSERR_INVALIDPARAM;
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wfx;
        if (IsEqualGUID(&ext->SubFormat, &RL_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) is_float = 1;
        else if (!IsEqualGUID(&ext->SubFormat, &RL_KSDATAFORMAT_SUBTYPE_PCM)) return DSERR_BADFORMAT;
        *out = *ext;
    } else if (tag == WAVE_FORMAT_PCM || tag == 3 /* WAVE_FORMAT_IEEE_FLOAT */) {
        is_float = tag == 3;
        out->Format = *wfx;
        if (tag == WAVE_FORMAT_PCM) out->Format.cbSize = 0;    /* (kept for float, as Windows: rl_buffer_create) */
    } else {
        return DSERR_BADFORMAT;
    }

    /* ALchemy (tests/alprobe.c create): WAVE_FORMAT_PCM and IEEE_FLOAT at most stereo, more channels only as
     * WAVE_FORMAT_EXTENSIBLE (Windows refuses them too); any rate.  No channel or a rate of 0, which ALchemy also
     * takes (and later divides by), stay refused */
    if (!wfx->nChannels || wfx->nChannels > 8) return DSERR_BADFORMAT;
    if (tag != WAVE_FORMAT_EXTENSIBLE && wfx->nChannels > 2) return DSERR_BADFORMAT;
    if (!wfx->nSamplesPerSec) return DSERR_BADFORMAT;
    if (is_float) {
        if (bits != 32) return DSERR_BADFORMAT;
        *sfmt = RL_F32;
    } else switch (bits) {
        case 8:  *sfmt = RL_U8; break;
        case 16: *sfmt = RL_S16; break;
        case 24: *sfmt = RL_S24; break;
        case 32: *sfmt = RL_S32; break;
        default: return DSERR_BADFORMAT;
    }
    /* the block size as it must be (some games pass garbage, which Windows refuses); the byte rate as given, as
     * Windows and ALchemy answer it, unless it is 0 */
    out->Format.nBlockAlign = (WORD)(wfx->nChannels * bits / 8);
    if (!out->Format.nAvgBytesPerSec) out->Format.nAvgBytesPerSec = out->Format.nBlockAlign * wfx->nSamplesPerSec;
    /* WAVE_FORMAT_EXTENSIBLE without valid bits: all of them (Windows) */
    if (tag == WAVE_FORMAT_EXTENSIBLE && !out->Samples.wValidBitsPerSample) out->Samples.wValidBitsPerSample = bits;
    return DS_OK;
}

static void list_add_all(rl_device *dev, rl_buffer *b)
{
    b->prev_all = NULL;
    b->next_all = dev->all;
    if (dev->all) dev->all->prev_all = b;
    dev->all = b;
}

static void list_remove_all(rl_device *dev, rl_buffer *b)
{
    if (b->prev_all) b->prev_all->next_all = b->next_all; else if (dev->all == b) dev->all = b->next_all;
    if (b->next_all) b->next_all->prev_all = b->prev_all;
    b->next_all = b->prev_all = NULL;
}

#include "buffer8.c"

#include "buffer3d.c"

#include "bufferprop.c"

/* =========================================================== a released buffer
 * Windows' DirectSound keeps a released buffer's interfaces answering: every call on one is E_INVALIDARG and
 * AddRef / Release answer 0xffffffff (tests/primtest.c overrelease).  Games that release an interface more often than
 * they referenced it, and so let the buffer go under another interface they still use (Fallout 3, Fallout: New Vegas),
 * live with that; a freed buffer took their later calls into freed memory (the heap corrupted, or a crash).  So a
 * buffer that goes keeps its interfaces, shrunk to them, with these tables. */
#define ZDECL(n, ...) static HRESULT WINAPI z##n(void *t, ##__VA_ARGS__) \
    { if (g_cfg.log_level >= 1) rl_refused_zombie(t, __builtin_return_address(0)); return DSERR_INVALIDPARAM; }
ZDECL(0)
ZDECL(1, DWORD a) ZDECL(2, DWORD a, DWORD b) ZDECL(3, DWORD a, DWORD b, DWORD c)
ZDECL(4, DWORD a, DWORD b, DWORD c, DWORD d) ZDECL(6, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f)
ZDECL(7, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f, DWORD g)
static ULONG WINAPI zref(void *t) { (void)t; return 0xffffffffu; }
static HRESULT WINAPI zqi(void *t, REFIID riid, void **out)
{
    (void)riid;
    if (g_cfg.log_level >= 1) rl_refused_zombie(t, __builtin_return_address(0));
    if (out) *out = NULL;
    return DSERR_INVALIDPARAM;
}
#define ZU (void *)zqi, (void *)zref, (void *)zref
static void *const z_dsb[] = { ZU, z1, z2, z3, z1, z1, z1, z1, z2, z7, z3, z1, z1, z1, z1, z1, z0, z4, z0, z3, z3, z4 };
static void *const z_b3d[] = { ZU, z1, z2, z1, z1, z1, z1, z1, z1, z1, z2, z3, z4, z2, z2, z2, z2, z4, z4 };
static void *const z_ntf[] = { ZU, z2 };
static void *const z_ks[] = { ZU, z7, z6, z3 };
static void *const z_lst[] = { ZU, z1, z1, z1, z2, z1, z1, z1, z2, z2, z2, z7, z4, z2, z4, z0 };
_Static_assert(sizeof(z_dsb) == sizeof(IDirectSoundBuffer8Vtbl), "dsb");
_Static_assert(sizeof(z_b3d) == sizeof(IDirectSound3DBufferVtbl), "b3d");
_Static_assert(sizeof(z_ntf) == sizeof(IDirectSoundNotifyVtbl), "ntf");
_Static_assert(sizeof(z_ks) == sizeof(IKsPropertySetVtbl), "ks");
_Static_assert(sizeof(z_lst) == sizeof(IDirectSound3DListenerVtbl), "lst");
/* one of reAL's live buffers (not one released, nor another implementation's), or NULL */
rl_buffer *rl_buffer_from_iface(IDirectSoundBuffer *i)
{
    if (!i || i->lpVtbl != (void *)&dsb_vtbl) return NULL;
    return impl_from(i, rl_buffer, IDirectSoundBuffer8_iface);
}

static void zombify(rl_buffer *b)
{
    b->IDirectSoundBuffer8_iface.lpVtbl = (void *)z_dsb;
    b->IDirectSound3DBuffer_iface.lpVtbl = (void *)z_b3d;
    b->IDirectSoundNotify_iface.lpVtbl = (void *)z_ntf;
    b->IKsPropertySet_iface.lpVtbl = (void *)z_ks;
    b->IDirectSound3DListener_iface.lpVtbl = (void *)z_lst;
    HeapReAlloc(GetProcessHeap(), HEAP_REALLOC_IN_PLACE_ONLY, b, offsetof(rl_buffer, ref));    /* (kept as it is if not) */
}

/* =========================================================== creation */

static rl_buffer *alloc_buffer(rl_device *dev)
{
    rl_buffer *b = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b));
    if (!b) return NULL;
    b->IDirectSoundBuffer8_iface.lpVtbl = &dsb_vtbl;
    b->IDirectSound3DBuffer_iface.lpVtbl = &b3d_vtbl;
    b->IDirectSoundNotify_iface.lpVtbl = &ntf_vtbl;
    b->IKsPropertySet_iface.lpVtbl = &ks_vtbl;
    b->IDirectSound3DListener_iface.lpVtbl = &lst_vtbl;
    b->ref = 1;
    b->iref[RL_IREF_BUF] = 1;
    b->id = InterlockedIncrement(&s_next_id);
    b->dev = dev;
    rl_source_defaults(&b->src3d);
    b->src3d_pending = b->src3d;
    b->eax1_mix = -1.0f;   /* EAX_REVERBMIX_USEDISTANCE */
    eax2b_defaults(&b->eax2);
    eax3b_defaults(&b->eax3);
    eax4_source_defaults(&b->eax4);
    return b;
}

static void flags_str(DWORD f, char *out, size_t n)
{
    static const struct { DWORD f; const char *s; } t[] = {
        { DSBCAPS_PRIMARYBUFFER, "PRIMARY" }, { DSBCAPS_STATIC, "STATIC" }, { DSBCAPS_LOCHARDWARE, "LOCHARDWARE" },
        { DSBCAPS_LOCSOFTWARE, "LOCSOFTWARE" }, { DSBCAPS_CTRL3D, "CTRL3D" }, { DSBCAPS_CTRLFREQUENCY, "CTRLFREQUENCY" },
        { DSBCAPS_CTRLPAN, "CTRLPAN" }, { DSBCAPS_CTRLVOLUME, "CTRLVOLUME" }, { DSBCAPS_CTRLPOSITIONNOTIFY, "CTRLPOSITIONNOTIFY" },
        { DSBCAPS_CTRLFX, "CTRLFX" }, { DSBCAPS_STICKYFOCUS, "STICKYFOCUS" }, { DSBCAPS_GLOBALFOCUS, "GLOBALFOCUS" },
        { DSBCAPS_GETCURRENTPOSITION2, "GETCURRENTPOSITION2" }, { DSBCAPS_MUTE3DATMAXDISTANCE, "MUTE3DATMAXDISTANCE" },
        { DSBCAPS_LOCDEFER, "LOCDEFER" },
    };
    size_t len = 0;
    out[0] = 0;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if ((f & t[i].f) && len + strlen(t[i].s) + 2 < n) {
            lstrcatA(out, t[i].s); lstrcatA(out, " ");
            len += strlen(t[i].s) + 1;
        }
}

void rl_buffer_set_sink(rl_buffer *b, DWORD caps)
{
    rl_buffer_lock(b);
    b->sink = TRUE; b->sink_cents = 0; b->sink_caps = caps;
    rl_buffer_update_gains(b);
    LeaveCriticalSection(&b->dev->lock);
}
int32_t rl_buffer_sink_cents(rl_buffer *b) { return b->sink ? b->sink_cents : 0; }
DWORD rl_buffer_position_bytes(rl_buffer *b)
{
    rl_buffer_lock(b);
    DWORD p = rl_voice_play_bytes(&b->voice);
    LeaveCriticalSection(&b->dev->lock);
    return p;
}

HRESULT rl_buffer_create(rl_device *dev, rl_dsound *owner, LPCDSBUFFERDESC desc, rl_buffer **out)
{
    char fl[400];
    flags_str(desc->dwFlags, fl, sizeof(fl));
    *out = NULL;

    rl_buffer *b = alloc_buffer(dev);
    if (!b) return DSERR_OUTOFMEMORY;
    b->owner = owner;
    memset(&b->desc, 0, sizeof(b->desc));
    memcpy(&b->desc, desc, desc->dwSize >= sizeof(DSBUFFERDESC) ? sizeof(DSBUFFERDESC) : sizeof(DSBUFFERDESC1));
    b->desc.dwSize = sizeof(b->desc);
    b->desc.lpwfxFormat = NULL;

    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
        b->is_primary = TRUE;
        /* its listener only if it was made with CTRL3D: ALchemy refuses it to a primary asked without (the kit's fuzz
         * runs), and Windows to the object made without, even when a later request with CTRL3D gets the same object
         * (tests/primtest.c more, object) */
        b->listener_ok = (desc->dwFlags & DSBCAPS_CTRL3D) != 0;
        /* what an owner at DSSCL_WRITEPRIMARY writes: silence in the primary format to start with (Windows' 0x80 for
         * its 8-bit default) */
        b->shared = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b->shared));
        BYTE *data = b->shared ? HeapAlloc(GetProcessHeap(), 0, PRIMARY_BYTES) : NULL;
        if (!data) {
            HeapFree(GetProcessHeap(), 0, b->shared);
            HeapFree(GetProcessHeap(), 0, b);
            return DSERR_OUTOFMEMORY;
        }
        b->shared->ref = 1;
        b->shared->data = data;
        b->shared->size = PRIMARY_BYTES;
        rl_device_addref(dev);
        EnterCriticalSection(&dev->lock);
        memset(data, dev->primary_fmt.wFormatTag == WAVE_FORMAT_PCM && dev->primary_fmt.wBitsPerSample == 8 ? 0x80 : 0,
               PRIMARY_BYTES);
        list_add_all(dev, b);
        prim_format(b);
        LeaveCriticalSection(&dev->lock);
        LOG_CALL("CreateSoundBuffer[%d] primary: %s", b->id, fl);
        *out = b;
        return DS_OK;
    }

    rl_sfmt sfmt;
    HRESULT hr = parse_format(desc->lpwfxFormat, &b->fmt, &sfmt);
    if (SUCCEEDED(hr) && (desc->dwFlags & DSBCAPS_CTRLFX)) {
        /* effects buffers: ALchemy makes any (it has no effects: its SetFX answers DSERR_CONTROLUNAVAIL); reAL's
         * effects work on the ones Windows 7 makes - IDirectSound8 only, not hardware, not static, PCM 8/16 or float,
         * at most two channels, 100 to 200000 Hz, at least DSBSIZE_FX_MIN ms - and SetFX on the others answers as
         * ALchemy */
        const WAVEFORMATEX *w = &b->fmt.Format;
        HRESULT why = DS_OK;
        if (owner && !owner->is_ds8) why = DSERR_DS8_REQUIRED;
        else if (desc->dwFlags & DSBCAPS_LOCHARDWARE) why = E_NOTIMPL;
        else if ((desc->dwFlags & DSBCAPS_STATIC) || w->nChannels > 2 ||
                 w->nSamplesPerSec < DSBFREQUENCY_MIN || w->nSamplesPerSec > DSBFREQUENCY_MAX ||
                 (sfmt != RL_U8 && sfmt != RL_S16 && sfmt != RL_F32)) why = DSERR_INVALIDPARAM;
        else if ((unsigned long long)desc->dwBufferBytes * 1000 < (unsigned long long)DSBSIZE_FX_MIN * w->nAvgBytesPerSec)
            why = DSERR_BUFFERTOOSMALL;
        b->fx_ok = SUCCEEDED(why);
        if (!b->fx_ok) LOG_CALL("CreateSoundBuffer: CTRLFX outside Windows' rules (%08lx): no effects", (unsigned long)why);
    }
    if (FAILED(hr)) {
        LOG_CALL("CreateSoundBuffer: bad format (tag %u)", desc->lpwfxFormat ? desc->lpwfxFormat->wFormatTag : 0);
        HeapFree(GetProcessHeap(), 0, b);
        return hr;
    }
    /* a 3D buffer of any channel count: ALchemy makes them from any object (tests/alprobe.c create; Windows 7 makes a
     * stereo one only through an older IDirectSound or with DSBUFFERDESC1, and nothing wider).  It plays as its mono
     * sum while it is positioned.  The size: ALchemy takes any but 0 (kept: it divides by it), rounded up to a block */
    DWORD block = b->fmt.Format.nBlockAlign;
    DWORD bytes = desc->dwBufferBytes;
    if (!bytes || bytes > 0xffffffffu - block) {
        HeapFree(GetProcessHeap(), 0, b);
        return DSERR_INVALIDPARAM;
    }
    /* a size that isn't a whole number of blocks is rounded up, as Windows (GetCaps answers the rounded size, and a
     * Lock of the size asked for succeeds); ALchemy keeps the size as asked.  Fallout 3 asks for its sample's byte
     * count, which can be odd for 16-bit mono; rounded down, its Lock of that count failed, the game took the sound
     * as not loaded, and its copy of it then wrote past the end of a buffer */
    if (bytes % block) bytes += block - bytes % block;
    b->desc.dwBufferBytes = bytes;

    b->shared = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*b->shared));
    BYTE *data = b->shared ? HeapAlloc(GetProcessHeap(), 0, bytes) : NULL;
    if (!data) {
        HeapFree(GetProcessHeap(), 0, b->shared);
        HeapFree(GetProcessHeap(), 0, b);
        return DSERR_OUTOFMEMORY;
    }
    memset(data, sfmt == RL_U8 ? 0x80 : 0, bytes);
    b->shared->ref = 1;
    b->shared->data = data;
    b->shared->size = bytes;
    if (b->fmt.Format.wFormatTag != WAVE_FORMAT_PCM) {
        /* Windows keeps a format other than PCM whole, the cbSize bytes after it too (GetFormat answers them); the
         * fields reAL settled (the block size, the byte rate, the valid bits) as reAL has them */
        const WAVEFORMATEX *in = desc->lpwfxFormat;
        DWORD n = sizeof(WAVEFORMATEX) + in->cbSize;
        BOOL readable = !IsBadReadPtr(in, n);
        if (!readable) n = sizeof(b->fmt);                      /* (the caller's cbSize runs past its memory) */
        WAVEFORMATEX *w = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n > sizeof(b->fmt) ? n : sizeof(b->fmt));
        if (w) {
            if (readable) memcpy(w, in, n);
            /* the settled fields: the header, and for WAVE_FORMAT_EXTENSIBLE the rest of its 40 bytes */
            memcpy(w, &b->fmt, b->fmt.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? sizeof(b->fmt) : sizeof(WAVEFORMATEX));
            w->cbSize = (WORD)(n - sizeof(WAVEFORMATEX));
            b->shared->wfx = w;
        }
        /* b->fmt holds 40 bytes: its own cbSize no more than that (GetFormat and SetFX fall back to it without w) */
        if (b->fmt.Format.cbSize > sizeof(b->fmt) - sizeof(WAVEFORMATEX))
            b->fmt.Format.cbSize = (WORD)(sizeof(b->fmt) - sizeof(WAVEFORMATEX));
    }

    b->voice.data = data;
    b->voice.interp = g_cfg.resampler;
    b->voice.size = bytes;
    b->voice.channels = b->fmt.Format.nChannels;
    b->voice.block = (uint16_t)block;
    b->voice.sfmt = sfmt;
    b->voice.rate = b->fmt.Format.nSamplesPerSec;
    b->src3d.mute_at_max = b->src3d_pending.mute_at_max = (desc->dwFlags & DSBCAPS_MUTE3DATMAXDISTANCE) != 0;

    if (desc->dwFlags & DSBCAPS_CTRL3D)
        b->hrtf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(rl_hrtf_voice));

    rl_device_addref(dev);
    EnterCriticalSection(&dev->lock);
    list_add_all(dev, b);
    dev->n_buffers++;
    if (!(b->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) dev->n_hw_buffers++;
    rl_buffer_update_gains(b);
    b->lost = device_writeprimary(dev);     /* (made while an object writes the primary buffer) */
    LeaveCriticalSection(&dev->lock);

    LOG_CALL("CreateSoundBuffer[%d] %lu bytes, %lu Hz %u ch %u bit: %s", b->id, (unsigned long)bytes,
             (unsigned long)b->fmt.Format.nSamplesPerSec, b->fmt.Format.nChannels, b->fmt.Format.wBitsPerSample, fl);
    *out = b;
    return DS_OK;
}

HRESULT rl_buffer_duplicate(rl_device *dev, rl_dsound *owner, rl_buffer *src, rl_buffer **out)
{
    *out = NULL;
    /* (an effects buffer too, as ALchemy duplicates it, tests/dsfuzz.c against it; Windows refuses: the copy plays
     * the samples without the effects, and takes none of its own, fx_ok FALSE) */
    rl_buffer *b = alloc_buffer(dev);
    if (!b) return DSERR_OUTOFMEMORY;
    b->owner = owner;

    EnterCriticalSection(&dev->lock);
    b->desc = src->desc;
    b->fmt = src->fmt;
    b->shared = src->shared;
    InterlockedIncrement(&b->shared->ref);
    b->voice = src->voice;
    b->voice.data = b->shared->data;           /* (the source's may be its effects' output) */
    b->voice.pos = 0;
    b->voice.hist = 0;
    b->voice.playing = 0;
    b->voice.looping = 0;
    b->voice.notifies = NULL;
    b->voice.n_notifies = 0;
    b->volume = src->volume;
    b->pan = src->pan;
    b->freq = src->freq;
    b->src3d = src->src3d;
    b->src3d_pending = src->src3d_pending;
    b->eax1_mix = src->eax1_mix;
    b->eax2 = src->eax2;
    b->eax3 = src->eax3;
    b->eax4 = src->eax4;
    b->voice.hrtf = NULL;
    b->voice.cmss = NULL;
    b->voice.cmss_virt = NULL;
    if (b->desc.dwFlags & DSBCAPS_CTRL3D)
        b->hrtf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(rl_hrtf_voice));
    rl_buffer_update_gains(b);
    b->lost = device_writeprimary(dev);
    list_add_all(dev, b);
    dev->n_buffers++;
    if (!(b->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) dev->n_hw_buffers++;
    LeaveCriticalSection(&dev->lock);
    rl_device_addref(dev);

    LOG_CALL("DuplicateSoundBuffer[%d] from [%d]", b->id, src->id);
    *out = b;
    return DS_OK;
}

/* As Windows' DirectSound (tests/primtest.c): at DSSCL_WRITEPRIMARY every secondary buffer of the device is lost,
 * any object's (they stop; Lock, Play, Stop, Restore and the positions answer DSERR_BUFFERLOST, GetStatus
 * DSBSTATUS_BUFFERLOST), and stays lost after it leaves that level until Restore; leaving it stops the primary
 * buffer's own playing.  The level is set here, under the lock
 * the buffers read it with. */
void rl_buffers_coop_level(rl_device *dev, rl_dsound *owner, DWORD level)
{
    EnterCriticalSection(&dev->lock);
    if ((owner->coop_level == DSSCL_WRITEPRIMARY) != (level == DSSCL_WRITEPRIMARY))
        dev->wp_owners += level == DSSCL_WRITEPRIMARY ? 1 : -1;
    owner->coop_level = level;
    int lost = 0;
    for (rl_buffer *b = dev->all; b; b = b->next_all) {
        if (!b->is_primary) {               /* (any object's: Windows loses them all) */
            if (level == DSSCL_WRITEPRIMARY && !b->lost) {
                b->lost = TRUE;
                stop_locked(b);
                lost++;
            }
            continue;
        }
        if (b->owner != owner) continue;
        if (level != DSSCL_WRITEPRIMARY) {
            b->voice.playing = 0;
            if (b->in_play_list) rl_device_stop_voice(dev, b);
        }
        prim_format(b);                     /* (its gains) */
    }
    LeaveCriticalSection(&dev->lock);
    if (lost) LOG_CALL("SetCooperativeLevel(DSSCL_WRITEPRIMARY): %d secondary buffer(s) lost", lost);
}

/* the owner's last release: its buffers stop for good, like real DirectSound's, and the ones the game still holds
 * move to the graveyard device (rl_device_grave), letting go of this one: a buffer kept by the game must not keep the
 * device, its thread and its state alive (Need for Speed III releases DirectSound for each sound setting in its menu
 * and keeps one buffer each time; with the device kept, the next DirectSound got the old one's EAX and 3D state).
 * The primary buffer goes too while the game holds it (the owner holds no reference to it). */
void rl_buffers_orphan(rl_device *dev, rl_dsound *owner)
{
    int n = 0, playing = 0, moved = 0;
    rl_device *grave = rl_device_grave();
    EnterCriticalSection(&dev->lock);
    if (owner->coop_level == DSSCL_WRITEPRIMARY) dev->wp_owners--;  /* (the others' buffers may be restored now) */
    for (rl_buffer *b = dev->all, *next; b; b = next) {
        next = b->next_all;
        if (b->owner != owner) continue;
        b->owner = NULL;
        if (b->is_primary) {
            owner->primary = NULL;      /* (the game holds it: it lives on without its owner) */
            b->voice.playing = 0;
        } else {
            b->dead = TRUE;
            if (b->voice.playing) playing++;
            b->voice.playing = 0;
            n++;
        }
        if (b->in_play_list) rl_device_stop_voice(dev, b);
        if (!grave) continue;
        /* moved with this device's lock held (and the graveyard's, after it): a method that took this lock, or
         * the final Release, sees the buffer either here or there (rl_buffer_lock), never between */
        list_remove_all(dev, b);
        if (!b->is_primary) {
            dev->n_buffers--;
            if (!(b->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) dev->n_hw_buffers--;
        }
        EnterCriticalSection(&grave->lock);
        list_add_all(grave, b);
        if (!b->is_primary) {
            grave->n_buffers++;
            if (!(b->desc.dwFlags & DSBCAPS_LOCSOFTWARE)) grave->n_hw_buffers++;
        }
        rl_device_addref(grave);
        b->dev = grave;
        LeaveCriticalSection(&grave->lock);
        moved++;
    }
    LeaveCriticalSection(&dev->lock);
    for (int k = 0; k < moved; k++) rl_device_release(dev);   /* (the owner still holds the device: not the last) */
    if (n) LOG_CALL("DirectSound released with %d buffer(s) still referenced by the game (%d playing): stopped", n, playing);
    if (moved) LOG_CALL("  %d buffer(s) moved off the device", moved);
}
