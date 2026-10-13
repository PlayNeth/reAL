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

/* DMUS_DOWNLOADINFO buffer; returns 0, 1 (instrument accepted with default articulation where dmsynth refuses it)
 * or a negative code. *handle identifies it for unload */
int msynth_download(msynth *s, const void *buf, uint32_t size, void **handle);
int msynth_unload(msynth *s, void *handle);

/* a MIDI message (short message, or sysex starting with 0xf0) for channel group 'group' (1-based, 0 = all) at
 * sample time t (the time IDirectMusicSynthSink::RefTimeToSample gives for the event's reference time) */
void msynth_midi(msynth *s, int64_t t, const uint8_t *msg, uint32_t len, uint32_t group);

/* IDirectMusicSynth::SetChannelPriority (group 1-based); immediate */
int msynth_set_priority(msynth *s, uint32_t group, uint32_t channel, uint32_t prio);
int msynth_get_priority(msynth *s, uint32_t group, uint32_t channel, uint32_t *prio);

/* the running statistics of the last completed second (CSynth +50): the milliseconds spent and the samples voices
 * played (raw), lost notes (note-ons no voice started for), the average voice count, CPU milliseconds per second
 * of output and the output's peak sample */
typedef struct { uint32_t ms, vsamples, lost, voices, cpu, peak; } msynth_stats;
void msynth_get_stats(msynth *s, msynth_stats *st);
/* the millisecond clock the CPU statistic is measured with */
void msynth_set_clock(msynth *s, uint32_t (*clock_ms)(void));

/* IDirectMusicSynth::Render: interleaved 16-bit stereo (or mono) frames starting at sample position pos */
void msynth_render(msynth *s, int16_t *buf, uint32_t frames, int64_t pos);

/* DirectX 8 sink interface (IDirectSoundSource::Read): nbuf mono 16-bit buffers, one per bus; busids are the ids
 * channels are assigned to (msynth_assign_buses; by default 0 left, 1 right, 0x40 reverb, 0x41 chorus), funcs their
 * functions (DSBUSID_*), pitch a per-bus pitch offset in cents.  Reads continue from msynth_seek's position. */
void msynth_read(msynth *s, int16_t **bufs, const uint32_t *busids, const uint32_t *funcs, const int32_t *pitch,
                 uint32_t nbuf, uint32_t frames);
void msynth_seek(msynth *s, int64_t pos);
int msynth_assign_buses(msynth *s, uint32_t group, uint32_t channel, const uint32_t *buses, uint32_t n);

/* IDirectMusicSynth8::PlayVoice / StopVoice (t: sample time, 0 = now; dlid: a wave articulation, download type 4,
 * whose buffers are downloads of type 5 or 6; start and loop points in samples) */
int msynth_play_voice(msynth *s, int64_t t, uint32_t voiceid, uint32_t group, uint32_t chan, uint32_t dlid,
                      int32_t pitch, int32_t vol, uint64_t start, uint64_t ls, uint64_t le);
int msynth_stop_voice(msynth *s, int64_t t, uint32_t voiceid);
/* IDirectMusicSynth8::Refresh: a streaming wave buffer (download type 5) has been refilled */
int msynth_refresh(msynth *s, uint32_t dlid);

/* fixes of the original's flaws (off by default, for comparison against it) */
#define MS_FIX_WAVE_END 1       /* a one-shot wave voice plays its last sample (the original stops two short of the
                                 * buffer's end, guard sample included) */
#define MS_FIX_WAVE_PASTEND 2   /* a wave voice started past the end of a wave that does not loop plays nothing (the
                                 * original starts it over from the beginning) */
void msynth_set_fixes(msynth *s, uint32_t fixes);

/* options matching the synth's environment: MMX mixing paths (default on, as on any CPU with MMX) */
void msynth_set_mmx(msynth *s, int on);
/* overall gain in millibels (GUID_DMUS_PROP_Volume; the synth's own default is +600, DirectMusic's port sets 0) */
void msynth_set_gain(msynth *s, int32_t mb);

#endif
