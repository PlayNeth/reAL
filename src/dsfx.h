/* reAL: DirectSound's standard effects (dsdmo.dll's DMOs, used through IDirectSoundBuffer8::SetFX), ported from
 * Microsoft's dsdmo.dll.  Platform independent: an effect has its parameters (stored as floats, as dsdmo's
 * IMediaParams keeps them) and processes PCM in place in its format (8/16-bit integer or 32-bit float, mono or
 * stereo).  The COM objects around them are in fx.c. */
#ifndef REAL_DSFX_H
#define REAL_DSFX_H
#include <stdint.h>

enum { DSFX_GARGLE, DSFX_CHORUS, DSFX_FLANGER, DSFX_ECHO, DSFX_DISTORTION, DSFX_COMPRESSOR, DSFX_PARAMEQ, DSFX_WAVES,
       DSFX_I3DL2, DSFX_NKINDS };

#define DSFX_OK            0
#define DSFX_FALSE         1
#define DSFX_E_INVALIDARG  ((int32_t)0x80070057)
#define DSFX_E_POINTER     ((int32_t)0x80004003)
#define DSFX_E_OUTOFMEMORY ((int32_t)0x8007000e)
#define DSFX_E_TYPE_NOT_ACCEPTED ((int32_t)0x80040205)

typedef struct dsfx dsfx;
dsfx *dsfx_new(int kind);
void dsfx_free(dsfx *f);
int dsfx_kind(const dsfx *f);
/* SetInputType/SetOutputType with one format: tag 1 (PCM, 8 or 16 bits) or 3 (float, 32 bits), 1 or 2 channels */
int dsfx_set_format(dsfx *f, unsigned tag, unsigned channels, unsigned rate, unsigned bits);
/* the effect's parameters as dsdmo's IMediaParams has them (index in the order of its DSFX structure) */
int dsfx_nparams(int kind);
int dsfx_set_param(dsfx *f, int index, float v);
float dsfx_get_param(const dsfx *f, int index);
/* SetAllParameters / GetAllParameters with the effect's DSFX structure (DSFXGargle, DSFXChorus, ...) */
int dsfx_set_all(dsfx *f, const void *params);
int dsfx_get_all(const dsfx *f, void *params);
int dsfx_params_size(int kind);
/* Discontinuity, and IMediaObjectInPlace::Process on frames of the format */
void dsfx_discontinuity(dsfx *f);
/* fixes of the original's flaws (off by default, for comparison against it) */
#define DSFX_FIX_ECHO_PANDELAY 1    /* Echo keeps PanDelay through Discontinuity (DirectSound calls it at every Play,
                                     * so in the original a PanDelay set before playing never takes effect) */
#define DSFX_FIX_I3DL2_QUALITY 2    /* I3DL2 runs at its default quality (2) until SetQuality (the original: half rate) */
void dsfx_set_fixes(dsfx *f, uint32_t fixes);
/* IDirectSoundFXI3DL2Reverb's own methods (I3DL2 only: Windows XP's reverb, src/i3dl2.c) */
int dsfx_i3dl2_set_preset(dsfx *f, uint32_t preset);
int dsfx_i3dl2_get_preset(dsfx *f, uint32_t *preset);
int dsfx_i3dl2_set_quality(dsfx *f, int32_t quality);
int dsfx_i3dl2_get_quality(dsfx *f, int32_t *quality);
void dsfx_i3dl2_object(dsfx *f, void *out);
void dsfx_i3dl2_frame(dsfx *f, uint32_t fn, void *out);  /* debugging: a function's frame (i3dl2_frame) */       /* debugging: XP's object, 0x4c8 bytes */
void dsfx_process(dsfx *f, void *data, uint32_t frames);

#endif
