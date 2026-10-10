/* msynth: a port of Microsoft's DirectMusic software synthesizer (dmsynth.dll, DirectX 9 / 5.3.x), reproducing its
 * output sample for sample.  Instruments come in as the download buffers DirectMusic hands a synth
 * (DMUS_DOWNLOADINFO + offset table + DMUS_INSTRUMENT/REGION/WAVE/...), events as MIDI messages stamped with sample
 * times, audio comes out through the same mixing paths Microsoft's synth uses. */
#ifndef REAL_MSYNTH_H
#define REAL_MSYNTH_H
#include <stdint.h>

typedef struct msynth msynth;

/* rate: sample rate; voices: dwVoices of the port parameters; groups: channel groups; stereo: 1 for 2 channels */
msynth *msynth_new(uint32_t rate, int voices, int groups, int stereo);
void msynth_free(msynth *s);

/* DMUS_DOWNLOADINFO buffer; returns 0 or an HRESULT-style negative code. *handle identifies it for unload */
int msynth_download(msynth *s, const void *buf, uint32_t size, void **handle);
int msynth_unload(msynth *s, void *handle);

/* a MIDI message (short message, or sysex starting with 0xf0) for channel group 'group' (1-based, 0 = all) at
 * sample time t (the time IDirectMusicSynthSink::RefTimeToSample gives for the event's reference time) */
void msynth_midi(msynth *s, int64_t t, const uint8_t *msg, uint32_t len, uint32_t group);

/* IDirectMusicSynth::Render: interleaved 16-bit stereo (or mono) frames starting at sample position pos */
void msynth_render(msynth *s, int16_t *buf, uint32_t frames, int64_t pos);

/* options matching the synth's environment: MMX mixing paths (default on, as on any CPU with MMX) */
void msynth_set_mmx(msynth *s, int on);
/* overall gain in millibels (GUID_DMUS_PROP_Volume; the synth's own default is +600, DirectMusic's port sets 0) */
void msynth_set_gain(msynth *s, int32_t mb);

#endif
