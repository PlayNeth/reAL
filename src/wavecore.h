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
 * faster or slower (0.05% at most) until it is back.  When the mixer stops for a moment (or an hour: the machine
 * asleep), I is set back to it, as a device would have stopped too.
 *
 * A pause stops the ideal device at once.  The mixer plays up to there, so that what the position has counted is
 * heard before the pause, not after it; the restart moves what is left along the timeline to keep the mixer `delta`
 * behind again. */
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
    uint32_t loop;              /* which loop (0: none) */
} wc_seg;

#define WC_MAXLOOP 64
typedef struct wc_stream {
    uint32_t rate, block;
    uint8_t silence;            /* a frame's bytes of silence (0x80 for 8-bit) */
    uint32_t ring;              /* the ring buffer's frames */
    uint32_t period;            /* the mixer's period (frames): its position moves that much at a time */
    uint32_t period0, delta0;   /* as given; period and delta go up with the pieces the mixer takes (wavecore.c) */
    uint32_t delta;             /* how far the mixer plays behind the ideal device, at least (a period more at most) */
    uint32_t lead;              /* how far ahead of the mixer the ring is filled */
    uint32_t guard;             /* frames past R the mixer may already have read (the resampler's taps) */
    /* the ideal device's clock: I = ia + (t - ta) * rate * slope while running */
    double ia, ta, slope, err;
    double lagmin, lagt;        /* the least I was ahead of its place for the mixer, since lagt (a mixer stopped) */
    int64_t lagR;               /* the mixer's position then */
    /* the pieces the mixer takes (wavecore.c, pieces()): its position and the time at the last tick */
    int64_t Rtick;
    double ttick;
    uint32_t lowmax; double lowt;   /* the longest period any step needed since lowt */
    int running;
    int64_t R;                  /* the mixer's position */
    int64_t W;                  /* the ring holds the timeline up to here */
    int64_t C;                  /* and silence from there up to here */
    int64_t tail;               /* the end of what is queued */
    int64_t pos_done;           /* frames played (at I) since the open or the last reset, of the pieces gone */
    int64_t pos_seen;           /* the highest position given out since then (it never goes back) */
    wc_seg *seg;                /* the queue: [head, n) */
    int head, n, cap;
    /* a loop (WHDR_BEGINLOOP .. WHDR_ENDLOOP): its buffers, played `passes` times.  The passes are queued as the
     * ideal device nears them (a game may loop for ever and end it with waveOutBreakLoop); buffers written meanwhile
     * wait in `later` */
    struct { void *hdr; const uint8_t *data; uint32_t frames; } loop[WC_MAXLOOP];
    int nloop, loop_open, broken;
    uint32_t loop_id;               /* that loop's number: more than one may be queued, the table is the last one's */
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

/* A compressed stream's codec takes whole blocks; what a buffer of the game's has past its last whole one waits
 * here to go before the next buffer. */
typedef struct wc_carry { uint8_t *p; uint32_t n; } wc_carry;
/* the bytes to give the codec for this buffer: what was kept, then `data`.  Returns them (`data` itself when
 * nothing was kept, NULL if that is) and their number; *made is what to free afterwards (NULL: nothing).  NULL with
 * something kept, or with *total less than len (too long): it failed */
const uint8_t *wc_carry_join(const wc_carry *c, const uint8_t *data, uint32_t len, uint32_t *total, uint8_t **made);
/* the codec used `used` of the `total` bytes at `all`: the rest is kept for the next buffer.  0: out of memory
 * (nothing is kept then) */
int wc_carry_keep(wc_carry *c, const uint8_t *all, uint32_t total, uint32_t used);
void wc_carry_drop(wc_carry *c);

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
int wc_drained(const wc_stream *s);     /* paused, and the mixer has played all the ideal device had: it can stop */
void wc_restart(wc_stream *s, double t);
void wc_breakloop(wc_stream *s, double t);
int64_t wc_position(wc_stream *s, double t);
#endif
