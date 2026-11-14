/* The timeline of a waveOut stream played through reAL (waveout.c), apart from Windows so tests can run it.
 *
 * A game writing waveOut expects a device with no latency: a buffer starts playing the moment it is written to an
 * idle device, and after waveOutReset the next buffer starts the moment it is written.  Some games mix their sound
 * themselves and depend on it: Pajama Sam 2 resets about ten times a second and writes its mix again from the time
 * its own clock says, which an output that drops what it has buffered at a reset (Wine's) turns into a gap each time.
 *
 * So the stream keeps that ideal device: its clock (I, in frames) runs with real time while the stream plays, and
 * buffers are laid along it (frames indexed from the open, gaps silent).  What is played is that timeline, `delta`
 * frames later: the mixer reads the stream's ring buffer at R, kept `delta` (and up to a mixing period) behind I.  A
 * buffer is done, and the position counts, as the ideal device plays it (at I); a reset keeps what is between R and
 * I, which the ideal device has played already, and the next buffer goes in at I.  I runs with real time, which the
 * game's clock is too; when the output device's clock has drifted a mixing period away from it, I goes a little
 * faster or slower (0.05% at most) until it is back. */
#ifndef REAL_WAVECORE_H
#define REAL_WAVECORE_H
#include <stdint.h>

typedef struct wc_seg {
    void *hdr;                  /* the buffer (WAVEHDR) this is a piece of */
    const uint8_t *data;
    uint32_t frames;
    int64_t start;              /* on the timeline */
    uint8_t last;               /* the buffer's last piece: it is done once played */
    uint32_t pass;              /* a loop's: which time through */
} wc_seg;

#define WC_MAXLOOP 64
typedef struct wc_stream {
    uint32_t rate, block;
    uint8_t silence;            /* a frame's bytes of silence (0x80 for 8-bit) */
    uint32_t ring;              /* the ring buffer's frames */
    uint32_t period;            /* the mixer's period (frames): its position moves that much at a time */
    uint32_t delta;             /* how far the mixer plays behind the ideal device, at least (a period more at most) */
    uint32_t lead;              /* how far ahead of the mixer the ring is filled */
    uint32_t guard;             /* frames past R the mixer may already have read (the resampler's taps) */
    /* the ideal device's clock: I = ia + (t - ta) * rate * slope while running */
    double ia, ta, slope, err;
    int running;
    int64_t R;                  /* the mixer's position */
    int64_t W;                  /* the ring holds the timeline up to here */
    int64_t tail;               /* the end of what is queued */
    int64_t pos_done;           /* frames played (at I) since the open or the last reset, of the pieces gone */
    wc_seg *seg;                /* the queue: [head, n) */
    int head, n, cap;
    /* a loop (WHDR_BEGINLOOP .. WHDR_ENDLOOP): its buffers, played `passes` times.  The passes are queued as the
     * ideal device nears them (a game may loop for ever and end it with waveOutBreakLoop); buffers written meanwhile
     * wait in `later` */
    struct { void *hdr; const uint8_t *data; uint32_t frames; } loop[WC_MAXLOOP];
    int nloop, loop_open, broken;
    uint32_t passes, next_pass;     /* the passes in all, the next one to queue (passes: all queued) */
    struct wc_later { void *hdr; const uint8_t *data; uint32_t frames, flags, loops; } *later;
    int nlater, caplater;
    /* the ring buffer: frames to it at a ring position (src NULL: silence); a buffer that is done */
    void (*put)(void *ctx, uint32_t at, const uint8_t *src, uint32_t frames);
    void (*done)(void *ctx, void *hdr);
    void *ctx;
} wc_stream;

#define WC_BEGINLOOP 4          /* WHDR_BEGINLOOP / WHDR_ENDLOOP */
#define WC_ENDLOOP 8

int wc_init(wc_stream *s, uint32_t rate, uint32_t block, uint8_t silence, uint32_t ring, uint32_t period,
            uint32_t delta, uint32_t guard, double t);
void wc_free(wc_stream *s);
double wc_ideal(const wc_stream *s, double t);
/* the mixer's position, as a ring position (it moved less than the ring's length since the last call) */
void wc_mixer_at(wc_stream *s, uint32_t ringpos);
void wc_tick(wc_stream *s, double t);
int wc_write(wc_stream *s, double t, void *hdr, const uint8_t *data, uint32_t frames, uint32_t flags, uint32_t loops);
void wc_reset(wc_stream *s, double t);
void wc_pause(wc_stream *s, double t);
void wc_restart(wc_stream *s, double t);
void wc_breakloop(wc_stream *s, double t);
int64_t wc_position(const wc_stream *s, double t);
#endif
