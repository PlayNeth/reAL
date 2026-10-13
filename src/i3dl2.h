/* reAL: DirectSound's I3DL2 environmental reverb (GUID_DSFX_STANDARD_I3DL2REVERB), Windows XP SP3's dsdmo.dll
 * (5.3.2600.5512) translated instruction for instruction (src/i3dl2.inc, tools/i3dl2/asm2c.py) so that it
 * computes bit for bit what the original does.  Platform independent; x86's x87 is required (the translation
 * keeps the original's x87 arithmetic in long double). */
#ifndef REAL_I3DL2_H
#define REAL_I3DL2_H
#include <stdint.h>

typedef struct i3dl2 i3dl2;
#define I3DL2_FIX_QUALITY 1     /* the default quality (2, as GetQuality reports it) is the one that runs; in the
                                 * original the object's own copy stays 0 until SetQuality, so it runs at half rate */
i3dl2 *i3dl2_new(uint32_t fixes);
void i3dl2_free(i3dl2 *r);
/* the format (tag 1: PCM 8/16, tag 3: float 32; mono or stereo): the effect starts over with its parameters */
int i3dl2_set_format(i3dl2 *r, unsigned tag, unsigned channels, unsigned rate, unsigned bits);
/* IMediaParams::SetParam / GetParam (13 parameters: the DSFXI3DL2Reverb fields, then the quality) */
int i3dl2_set_param(i3dl2 *r, int index, float v);
float i3dl2_get_param(const i3dl2 *r, int index);
/* IDirectSoundFXI3DL2Reverb */
int i3dl2_set_all(i3dl2 *r, const void *params);       /* DSFXI3DL2Reverb, 48 bytes */
int i3dl2_get_all(i3dl2 *r, void *params);
int i3dl2_set_preset(i3dl2 *r, uint32_t preset);
int i3dl2_get_preset(i3dl2 *r, uint32_t *preset);
int i3dl2_set_quality(i3dl2 *r, int32_t quality);
int i3dl2_get_quality(i3dl2 *r, int32_t *quality);
void i3dl2_discontinuity(i3dl2 *r);
/* in place, frames of the format */
void i3dl2_process(i3dl2 *r, void *data, uint32_t frames);
/* debugging: the original's object (0x4c8 bytes) as it stands */
void i3dl2_object(const i3dl2 *r, void *out);
void i3dl2_frame(i3dl2 *r, uint32_t fn, void *out);

#endif
