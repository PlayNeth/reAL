/* wavecore.c: a waveOut stream's timeline, with a simulated mixer reading its ring buffer.
 *   cc -O1 -std=gnu11 -Wall -o build/test_wavecore tests/test_wavecore.c src/wavecore.c -lm && ./build/test_wavecore */
#include "../src/wavecore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define RATE 11025
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                     printf("\n"); } } while (0)

/* the ring, a mixer reading it a period at a time on a clock `ppm` off real time, and what it played */
typedef struct {
    wc_stream s;
    uint8_t ring[RATE];
    uint8_t *out; long nout;
    double ppm, next_mix, next_tick, t;
    uint32_t rp, pf;
    int paused;
    void *done[4096]; double done_t[4096]; int ndone;
} sim;
static void put(void *c, uint32_t at, const uint8_t *src, uint32_t n)
{
    sim *m = c;
    if (src) memcpy(m->ring + at, src, n); else memset(m->ring + at, 0x80, n);
}
static void on_done(void *c, void *h) { sim *m = c; m->done_t[m->ndone] = m->t; m->done[m->ndone++] = h; }
static void sim_init(sim *m, double ppm, double period_ms)
{
    memset(m, 0, sizeof(*m));
    m->out = malloc(RATE * 400);
    m->ppm = ppm;
    m->pf = (uint32_t)(period_ms * RATE / 1000) + 1;
    m->s.put = put; m->s.done = on_done; m->s.ctx = m;
    memset(m->ring, 0x80, sizeof(m->ring));
    wc_init(&m->s, RATE, 1, 0x80, RATE, m->pf, m->pf + RATE / 100 + 32, 32, 0.0);
    m->next_mix = 0.001;
    m->next_tick = 0.0;
}
/* run the mixer and the 5 ms tick up to t */
static void sim_run(sim *m, double t)
{
    for (;;) {
        double tn = m->next_mix < m->next_tick ? m->next_mix : m->next_tick;
        if (tn > t) break;
        m->t = tn;
        if (tn == m->next_mix) {
            uint32_t n = m->pf;
            for (uint32_t k = 0; k < n; k++) m->out[m->nout++] = m->paused ? 0x80 : m->ring[(m->rp + k) % RATE];
            if (!m->paused) m->rp = (m->rp + n) % RATE;
            m->next_mix += (double)m->pf / RATE / (1 + m->ppm * 1e-6);
        } else {
            wc_mixer_at(&m->s, m->rp);
            wc_tick(&m->s, tn);
            m->next_tick += 0.005;
        }
    }
    m->t = t;
    wc_mixer_at(&m->s, m->rp);
}
static long first_sound(const sim *m, long from)
{
    for (long k = from; k < m->nout; k++) if (m->out[k] != 0x80) return k;
    return -1;
}

int main(void)
{
    static uint8_t buf[8][2048];
    sim m_, *m = &m_;

    /* buffers written back to back play back to back; done in order, as the ideal device finishes each */
    sim_init(m, 0, 10);
    for (int b = 0; b < 4; b++) for (int k = 0; k < 1000; k++) buf[b][k] = (uint8_t)(1 + (b * 1000 + k) % 250);
    sim_run(m, 0.1);
    for (int b = 0; b < 4; b++) wc_write(&m->s, 0.1, buf[b], buf[b], 1000, 0, 0);
    sim_run(m, 1.0);
    long f = first_sound(m, 0);
    CHECK(f > 0, "no sound");
    int ok = 1;
    for (int k = 0; k < 4000; k++) if (m->out[f + k] != (uint8_t)(1 + k % 250)) { ok = 0; break; }
    CHECK(ok, "contiguous buffers not played as written");
    CHECK(m->out[f + 4000] == 0x80, "silence after");
    CHECK(m->ndone == 4 && m->done[0] == buf[0] && m->done[3] == buf[3], "done %d", m->ndone);
    /* (the ideal device started at 0.1 s: buffer b ends at 0.1 + (b + 1) * 1000 / RATE) */
    for (int b = 0; b < m->ndone; b++)
        CHECK(fabs(m->done_t[b] - (0.1 + (b + 1) * 1000.0 / RATE)) < 0.006, "buffer %d done at %.4f", b, m->done_t[b]);
    /* latency: from the write to the sound, the delta and up to a period or so */
    double lat = (double)f / RATE - 0.1;
    CHECK(lat > 0.015 && lat < 0.05, "latency %.4f", lat);
    CHECK(wc_position(&m->s, 1.0) == 4000, "position %lld", (long long)wc_position(&m->s, 1.0));

    /* a write to the idle device starts when it is written (a gap of silence) */
    long before = m->nout;
    wc_write(&m->s, 1.2, buf[4], buf[0], 500, 0, 0);
    sim_run(m, 2.0);
    long f2 = first_sound(m, before);
    CHECK(fabs((double)(f2 - f) / RATE - 1.1) < 0.0015, "second start %.4f s after the first", (double)(f2 - f) / RATE);

    /* a reset: what the ideal device played stays, the next buffer goes in at the reset's time */
    for (int k = 0; k < 2000; k++) buf[5][k] = (uint8_t)(1 + k % 100), buf[6][k] = 200;
    wc_write(&m->s, 2.0, buf[5], buf[5], 2000, 0, 0);
    sim_run(m, 2.05);
    int nd = m->ndone;
    wc_reset(&m->s, 2.05);
    wc_write(&m->s, 2.05, buf[6], buf[6], 1000, 0, 0);
    sim_run(m, 3.0);
    long a = first_sound(m, f2 + 500);
    long played = 0;
    while (m->out[a + played] != 200 && played < 2000) played++;
    CHECK(labs(played - (long)(0.05 * RATE)) <= 2, "%ld frames of the reset buffer played, not %ld", played,
          (long)(0.05 * RATE));
    CHECK(m->out[a + played] == 200 && m->out[a + played + 999] == 200 && m->out[a + played + 1000] == 0x80,
          "after the reset");
    /* (buf[5] went back with the reset: the glue does that) */
    CHECK(m->ndone == nd + 1, "done after reset: %d", m->ndone - nd);
    CHECK(wc_position(&m->s, 3.0) == 1000, "position after reset %lld", (long long)wc_position(&m->s, 3.0));

    /* pause: the ideal device stops; restart goes on */
    wc_write(&m->s, 3.0, buf[0], buf[0], 2000, 0, 0);
    sim_run(m, 3.05);
    m->paused = 1; wc_pause(&m->s, 3.05);
    int64_t p1 = wc_position(&m->s, 3.05);
    sim_run(m, 4.0);
    CHECK(wc_position(&m->s, 4.0) == p1 && m->ndone == nd + 1, "paused: position moved or a buffer done");
    m->paused = 0; wc_restart(&m->s, 4.0);
    sim_run(m, 5.0);
    CHECK(m->ndone == nd + 2 && wc_position(&m->s, 5.0) == 3000, "after restart: %d done, position %lld", m->ndone - nd,
          (long long)wc_position(&m->s, 5.0));

    /* a loop of one buffer, three times, done once */
    sim_init(m, 0, 10);
    for (int k = 0; k < 300; k++) buf[0][k] = (uint8_t)(1 + k % 50);
    wc_write(&m->s, 0.1, buf[0], buf[0], 300, WC_BEGINLOOP | WC_ENDLOOP, 3);
    wc_write(&m->s, 0.1, buf[1], buf[6], 100, 0, 0);
    sim_run(m, 1.0);
    f = first_sound(m, 0);
    ok = 1;
    for (int k = 0; k < 900; k++) if (m->out[f + k] != (uint8_t)(1 + (k % 300) % 50)) { ok = 0; break; }
    CHECK(ok && m->out[f + 900] == 200 && m->out[f + 1000] == 0x80, "loop of three");
    CHECK(m->ndone == 2 && m->done[0] == buf[0] && m->done[1] == buf[1], "loop: done %d", m->ndone);
    CHECK(wc_position(&m->s, 1.0) == 1000, "loop position %lld", (long long)wc_position(&m->s, 1.0));

    /* a loop over two buffers, broken during its second pass: it ends with that pass, and what follows moves up */
    sim_init(m, 0, 10);
    memset(buf[2], 10, 2000); memset(buf[3], 20, 2000); memset(buf[4], 30, 500);
    wc_write(&m->s, 0.1, buf[2], buf[2], 2000, WC_BEGINLOOP, 5);
    wc_write(&m->s, 0.1, buf[3], buf[3], 2000, WC_ENDLOOP, 0);
    wc_write(&m->s, 0.1, buf[4], buf[4], 500, 0, 0);
    sim_run(m, 0.1 + 5000.0 / RATE);       /* (in the second pass's first buffer) */
    wc_breakloop(&m->s, m->t);
    sim_run(m, 3.0);
    f = first_sound(m, 0);
    long n10 = 0, n20 = 0, n30 = 0, k = f;
    for (; k < m->nout && m->out[k] != 0x80; k++)
        n10 += m->out[k] == 10, n20 += m->out[k] == 20, n30 += m->out[k] == 30;
    CHECK(n10 == 4000 && n20 == 4000 && n30 == 500, "broken loop: %ld %ld %ld", n10, n20, n30);
    CHECK(m->ndone == 3 && m->done[2] == buf[4], "broken loop: done %d", m->ndone);

    /* a loop for ever (dwLoops 0xffffffff), broken after a few seconds: its passes are queued as they near */
    sim_init(m, 0, 10);
    memset(buf[2], 40, 1000); memset(buf[3], 50, 300);
    wc_write(&m->s, 0.1, buf[2], buf[2], 1000, WC_BEGINLOOP | WC_ENDLOOP, 0xffffffffu);
    wc_write(&m->s, 0.1, buf[3], buf[3], 300, 0, 0);
    sim_run(m, 5.0);
    CHECK(m->s.n - m->s.head < 64 && m->ndone == 0, "endless loop: %d pieces queued, %d done", m->s.n - m->s.head,
          m->ndone);
    wc_breakloop(&m->s, m->t);
    sim_run(m, 6.0);
    CHECK(m->ndone == 2 && m->done[0] == buf[2] && m->done[1] == buf[3], "endless loop broken: done %d", m->ndone);
    f = first_sound(m, 0);
    for (k = f; k < m->nout && m->out[k] == 40; k++) {}
    CHECK((k - f) % 1000 == 0 && (double)(k - f) / RATE > 4.85 && (double)(k - f) / RATE < 5.0 + 1000.0 / RATE,
          "endless loop: %ld frames of it", k - f);
    long n50 = 0;
    for (; k < m->nout && m->out[k] == 50; k++) n50++;
    CHECK(n50 == 300 && m->out[k] == 0x80, "after the endless loop: %ld", n50);

    /* a game mixing itself and resetting ten times a second (Pajama Sam 2): at each reset it writes its mix from what
     * its clock says on.  The output must go on sample by sample (a ramp here, never the silence value), whatever the
     * mixer's clock does */
    static const double ppms[] = { 0, 150, -150, 600 };
    for (int pi = 0; pi < 4; pi++) {
        sim_init(m, ppms[pi], 10);
        static uint8_t mix[16][1024];
        double t0 = 0.2;
        int bi = 0;
        for (double t = t0; t < 20.0; t += 0.104 + 0.003 * sin(t * 7)) {
            sim_run(m, t);
            wc_reset(&m->s, t);
            long g = lround((t - t0) * RATE);
            for (int b = 0; b < 7; b++, bi = (bi + 1) % 16) {
                for (int k = 0; k < 1024; k++) mix[bi][k] = (uint8_t)(1 + (g + b * 1024 + k) % 127);
                wc_write(&m->s, t, mix[bi], mix[bi], 1024, 0, 0);
            }
        }
        sim_run(m, 20.0);
        long jumps = 0, worst = 0;
        f = first_sound(m, 0);
        long k = f + 1;
        for (; k < m->nout - 1 && m->out[k + 1] != 0x80; k++) {
            int d = ((int)m->out[k] - (int)m->out[k - 1] + 127) % 127;
            if (d != 1) { jumps++; long e = d > 63 ? 127 - d + 1 : d - 1; if (e > worst) worst = e; }
        }
        CHECK((double)(k - f) / RATE > 19.5, "resetting game: the sound stopped at %.2f s", (double)(k - f) / RATE);
        /* (a splice may land a frame off: the reset's time to the nearest frame) */
        CHECK(worst <= (fabs(ppms[pi]) > 500 ? 2 : 1),
              "resetting game, mixer %+.0f ppm: %ld jumps, the worst %ld frames", ppms[pi], jumps, worst);
        printf("wavecore: resetting game, mixer clock %+.0f ppm: %ld splices off by a frame, the worst %ld\n", ppms[pi],
               jumps, worst);
    }

    if (fails) { printf("wavecore tests: %d FAILED\n", fails); return 1; }
    printf("wavecore tests: all passed\n");
    return 0;
}
