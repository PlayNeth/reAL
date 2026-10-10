/* Waves reverb (DirectSound effect GUID_DSFX_WAVES_REVERB), a port of dsdmo.dll's 16-bit paths: the parameters are
 * turned into Q15 coefficients and the network runs in integer arithmetic, so the output matches Microsoft's. */
#ifndef REAL_WAVESREV_H
#define REAL_WAVESREV_H
#include <stdint.h>

typedef struct wavesrev wavesrev;

wavesrev *wavesrev_new(uint32_t rate);
void wavesrev_free(wavesrev *w);
void wavesrev_reset(wavesrev *w);
/* DSFXWavesReverb: in gain [-96, 0] dB, reverb mix [-96, 0] dB, reverb time [0.001, 3000] ms,
 * high frequency RT ratio [0.001, 0.999].  Defaults 0, 0, 1000, 0.001.  Returns 0 or -1 (out of range). */
int wavesrev_set(wavesrev *w, float ingain, float mix, float time, float hfratio);
/* in place on interleaved 16-bit frames */
void wavesrev_stereo16(wavesrev *w, int16_t *buf, uint32_t frames);
void wavesrev_mono16(wavesrev *w, int16_t *buf, uint32_t frames);

#endif
