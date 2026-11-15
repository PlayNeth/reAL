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
    int burst;                  /* the mixer takes this many periods at once */
    double hold_until;          /* no tick before then (its thread held up) */
    int paused;                 /* the buffer stopped */
    int draining;               /* paused: the buffer plays on until the mixer has what the ideal device played */
    void *done[4096]; double done_t[4096]; int ndone;
} sim;
static void put(void *c, uint32_t at, const uint8_t *src, uint32_t n)
{
    sim *m = c;
    if (src) memcpy(m->ring + at, src, n); else memset(m->ring + at, 0x80, n);
}
static void on_done(void *c, void *h) { sim *m = c; m->done_t[m->ndone] = m->t; m->done[m->ndone++] = h; }
/* (m zeroed, or set up before) */
static void sim_init(sim *m, double ppm, double period_ms)
{
    free(m->out);
    wc_free(&m->s);
    memset(m, 0, sizeof(*m));
    m->out = malloc(RATE * 400);
    m->burst = 1;
    m->ppm = ppm;
    m->pf = (uint32_t)(period_ms * RATE / 1000) + 1;
    m->s.put = put; m->s.done = on_done; m->s.ctx = m;
    memset(m->ring, 0x80, sizeof(m->ring));
    wc_init(&m->s, RATE, 1, 0x80, RATE, m->pf, m->pf + RATE / 100 + 32, 32, 0.0);
    m->next_mix = 0.001;
    m->next_tick = 0.0;
}
/* waveOutPause and waveOutRestart, as waveout.c does them */
static void sim_pause(sim *m, double t) { wc_mixer_at(&m->s, m->rp); wc_pause(&m->s, t); m->draining = 1; }
static void sim_restart(sim *m, double t) { wc_restart(&m->s, t); m->paused = m->draining = 0; }
/* run the mixer and the 5 ms tick up to t */
static void sim_run(sim *m, double t)
{
    for (;;) {
        double tn = m->next_mix < m->next_tick ? m->next_mix : m->next_tick;
        if (tn > t) break;
        m->t = tn;
        if (tn == m->next_mix) {
            uint32_t n = m->pf * (uint32_t)m->burst;
            for (uint32_t k = 0; k < n; k++) m->out[m->nout++] = m->paused ? 0x80 : m->ring[(m->rp + k) % RATE];
            if (!m->paused) m->rp = (m->rp + n) % RATE;
            m->next_mix += (double)n / RATE / (1 + m->ppm * 1e-6);
        } else {
            if (tn >= m->hold_until) {
                wc_mixer_at(&m->s, m->rp);
                wc_tick(&m->s, tn);
                if (m->draining && wc_drained(&m->s)) { m->paused = 1; m->draining = 0; }   /* (as waveout.c's tick) */
            }
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
    static sim m_;
    sim *m = &m_;

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

    /* pause: the ideal device stops, and what it played is heard before the pause, to the frame; restart goes on
     * with the rest, as soon after it as a buffer after its write */
    for (int k = 0; k < 2000; k++) buf[0][k] = (uint8_t)(1 + k % 120);
    before = m->nout;
    wc_write(&m->s, 3.0, buf[0], buf[0], 2000, 0, 0);
    sim_run(m, 3.05);
    sim_pause(m, 3.05);
    int64_t p1 = wc_position(&m->s, 3.05);
    sim_run(m, 4.0);
    CHECK(wc_position(&m->s, 4.0) == p1 && m->ndone == nd + 1, "paused: position moved or a buffer done");
    CHECK(m->paused && !m->draining, "paused: the buffer was not stopped");
    long pa = first_sound(m, before), heard = 0;
    while (pa + heard < m->nout && m->out[pa + heard] != 0x80) heard++;
    CHECK(heard == (long)(p1 - 1000), "paused at position %lld: %ld frames heard before it", (long long)p1, heard);
    CHECK(first_sound(m, pa + heard) < 0, "paused: sound after the pause");
    long at_restart = m->nout;
    sim_restart(m, 4.0);
    sim_run(m, 5.0);
    long pb = first_sound(m, at_restart);
    ok = pb > 0;
    for (long k = 0; ok && k < 2000; k++) {
        long o = k < heard ? pa + k : pb + (k - heard);
        if (m->out[o] != buf[0][k]) ok = 0;
    }
    CHECK(ok && m->out[pb + 2000 - heard] == 0x80, "pause and restart: the buffer not played once, in two parts");
    lat = (double)(pb - at_restart) / RATE;
    CHECK(lat > 0.015 && lat < 0.05, "restart: the sound %.4f s after it", lat);
    CHECK(m->ndone == nd + 2 && wc_position(&m->s, 5.0) == 3000, "after restart: %d done, position %lld", m->ndone - nd,
          (long long)wc_position(&m->s, 5.0));

    /* a restart before the mixer has played up to the pause: nothing twice, nothing left out */
    for (int k = 0; k < 2000; k++) buf[1][k] = (uint8_t)(1 + k % 119);
    before = m->nout;
    wc_write(&m->s, 5.0, buf[1], buf[1], 2000, 0, 0);
    sim_run(m, 5.05);
    sim_pause(m, 5.05);
    sim_run(m, 5.058);
    sim_restart(m, 5.058);
    sim_run(m, 6.0);
    pa = first_sound(m, before);
    long got = 0;
    ok = pa > 0;
    for (long k = pa; ok && k < m->nout && got < 2000; k++) {
        if (m->out[k] == 0x80) continue;
        if (m->out[k] != buf[1][got]) ok = 0;
        got++;
    }
    CHECK(ok && got == 2000, "pause and restart at once: %ld frames in order", got);

    /* written while paused: it starts where the device stopped, when it restarts; a reset while paused keeps what
     * was played and takes back the rest */
    sim_pause(m, 6.0);
    sim_run(m, 6.5);
    nd = m->ndone;
    memset(buf[2], 77, 600);
    wc_write(&m->s, 6.5, buf[2], buf[2], 600, 0, 0);
    sim_run(m, 7.0);
    CHECK(first_sound(m, m->nout - RATE / 2) < 0 && m->ndone == nd, "written while paused: played or done");
    wc_reset(&m->s, 7.0);                       /* (the glue gives the buffer back) */
    memset(buf[3], 88, 400);
    wc_write(&m->s, 7.0, buf[3], buf[3], 400, 0, 0);
    at_restart = m->nout;
    sim_restart(m, 7.2);
    sim_run(m, 8.0);
    pb = first_sound(m, at_restart);
    long n88 = 0;
    for (long k = pb; pb > 0 && k < m->nout && m->out[k] != 0x80; k++) n88 += m->out[k] == 88;
    lat = (double)(pb - at_restart) / RATE;
    CHECK(n88 == 400 && lat > 0.015 && lat < 0.05, "reset and written while paused: %ld frames, %.4f s after the restart",
          n88, lat);
    CHECK(wc_position(&m->s, 8.0) == 400, "position after that %lld", (long long)wc_position(&m->s, 8.0));

    /* the mixer stopped for a while (the output opened again, the machine asleep): a buffer written afterwards is
     * heard as soon as ever, and buffers queued across it play once each, in order, the position never going back */
    static const double stalls[] = { 0.4, 1.7, 3600.0 };
    for (int si = 0; si < 3; si++) {
        sim_init(m, 0, 10);
        static uint8_t q[40][1024];
        for (int b = 0; b < 40; b++) for (int k = 0; k < 1024; k++) q[b][k] = (uint8_t)(1 + (b * 1024 + k) % 113);
        sim_run(m, 0.5);
        int wb = 0;
        int64_t lastpos = 0;
        int back = 0;
        double t = 0.5;
        /* (a game that keeps four buffers queued) */
        #define FEED() while (wb < 40 && wb - m->ndone < 4) { wc_write(&m->s, t, q[wb], q[wb], 1024, 0, 0); wb++; }
        for (; t < 1.5; t += 0.01) { sim_run(m, t); FEED(); }
        m->next_mix += stalls[si]; m->next_tick += stalls[si]; t += stalls[si];     /* (nothing runs meanwhile) */
        for (double e = t + 6.0; t < e; t += 0.01) {
            sim_run(m, t);
            FEED();
            int64_t pp = wc_position(&m->s, t);
            if (pp < lastpos) back = 1;
            lastpos = pp;
        }
        #undef FEED
        long n = 0;
        ok = 1;
        for (long k = 0; k < m->nout && ok; k++) {
            if (m->out[k] == 0x80) continue;
            if (n >= 40 * 1024 || m->out[k] != (uint8_t)(1 + n % 113)) ok = 0;
            n++;
        }
        CHECK(ok && n == 40 * 1024 && m->ndone == 40, "a stall of %.1f s: %ld frames in order, %d buffers done", stalls[si],
              n, m->ndone);
        CHECK(!back && lastpos == 40 * 1024, "a stall of %.1f s: the position went back, or ends at %lld", stalls[si],
              (long long)lastpos);
        before = m->nout;
        memset(q[0], 99, 500);
        wc_write(&m->s, t, q[0], q[0], 500, 0, 0);
        sim_run(m, t + 1.0);
        lat = (double)(first_sound(m, before) - before) / RATE;
        CHECK(lat > 0.015 && lat < 0.05, "after a stall of %.1f s: a buffer heard %.4f s after its write", stalls[si], lat);
    }

    /* a mixer that takes 270 ms at a time, and 400 ms (the output's buffer may be that long): the ideal device is
     * far ahead of it before each piece, and that is not a stop */
    for (int bi = 0; bi < 2; bi++) {
        sim_init(m, 0, bi ? 50 : 30);
        m->burst = bi ? 8 : 9;
        static uint8_t q[4][512];
        memset(q, 9, sizeof(q));
        int wb = 0, back = 0;
        double last = 0;
        for (double t = 0.5; t < 20.0; t += 0.01) {
            sim_run(m, t);
            while (wb - m->ndone < 4) { wc_write(&m->s, t, q[wb % 4], q[wb % 4], 512, 0, 0); wb++; }
            double I = wc_ideal(&m->s, t);
            if (I < last) back++;
            last = I;
        }
        CHECK(!back && m->ndone > 400, "a mixer taking %d ms at a time: the ideal device set back %d times, %d buffers done",
              bi ? 400 : 270, back, m->ndone);
    }

    /* a mixer that takes 60 ms at a time (20 ms periods, three at once): a pause still plays what the position has
     * counted, no more and no less, though the mixer may be past the ideal device when it comes */
    for (int round = 0; round < 12; round++) {
        sim_init(m, -150, 20);
        m->burst = 3;
        static uint8_t q[4096];
        for (int k = 0; k < 4096; k++) q[k] = (uint8_t)(1 + k % 113);
        sim_run(m, 0.5);
        wc_write(&m->s, 0.5, q, q, 4096, 0, 0);
        /* (paused at any time in the mixer's 60 ms, and right after it took a piece) */
        sim_run(m, 0.6 + round * 0.0071);
        double tp = round & 1 ? m->next_mix + 0.0004 : 0.6 + round * 0.0071;
        sim_run(m, tp);
        sim_pause(m, tp);
        int64_t pp = wc_position(&m->s, tp);
        sim_run(m, tp + 0.5);
        long n = 0;
        for (long k = 0; k < m->nout; k++) n += m->out[k] != 0x80;
        CHECK(n == (long)pp && m->paused, "a mixer in large pieces, paused at position %lld: %ld frames heard", (long long)pp, n);
        sim_restart(m, tp + 0.5);
        sim_run(m, tp + 2.0);
        n = 0;
        ok = 1;
        for (long k = 0; k < m->nout && ok; k++) {
            if (m->out[k] == 0x80) continue;
            if (m->out[k] != q[n < 4096 ? n : 0]) ok = 0;
            n++;
        }
        CHECK(ok && n == 4096, "a mixer in large pieces, pause and restart: %ld frames in order", n);
    }

    /* the tick held up for 150 ms while the mixer plays on: it runs out of what the ring holds and plays silence,
     * not what the ring held a second ago; what it missed is lost, the rest follows in order */
    {
        sim_init(m, 0, 10);
        static uint8_t q[60][1024];
        for (int b = 0; b < 60; b++) for (int k = 0; k < 1024; k++) q[b][k] = (uint8_t)(1 + (b * 1024 + k) % 113);
        int wb = 0;
        for (double t = 0.2; t < 8.0; t += 0.01) {
            if (t >= 3.0 && t < 3.01) m->hold_until = 3.15;
            sim_run(m, t);
            while (wb < 60 && wb - m->ndone < 4) { wc_write(&m->s, t, q[wb], q[wb], 1024, 0, 0); wb++; }
        }
        long n = 0, lost = 0;
        int prev = -1;
        ok = 1;
        for (long k = 0; k < m->nout; k++) {
            if (m->out[k] == 0x80) continue;
            int v = m->out[k] - 1;
            if (prev >= 0 && v != (prev + 1) % 113) {
                /* (one place where frames are missing, never one where earlier ones come again) */
                if (lost) ok = 0;
                lost = k;
            }
            prev = v; n++;
        }
        CHECK(ok && lost && n < 60 * 1024 && n > 60 * 1024 - RATE / 4 && m->ndone == 60,
              "the tick held up: %ld frames heard of %d, %d buffers done", n, 60 * 1024, m->ndone);
    }

    /* a loop of two buffers written while another loop's passes are still being queued: each in its turn */
    {
        sim_init(m, 0, 10);
        static uint8_t la[5512], lb1[1000], lb2[1000], lc[1000];
        memset(la, 1, sizeof(la)); memset(lb1, 2, sizeof(lb1)); memset(lb2, 3, sizeof(lb2)); memset(lc, 4, sizeof(lc));
        sim_run(m, 0.1);
        wc_write(&m->s, 0.1, la, la, 5512, WC_BEGINLOOP | WC_ENDLOOP, 6);
        wc_write(&m->s, 0.1, lb1, lb1, 1000, WC_BEGINLOOP, 2);
        wc_write(&m->s, 0.1, lb2, lb2, 1000, WC_ENDLOOP, 0);
        sim_run(m, 3.3);                    /* (the second loop is playing by now) */
        wc_write(&m->s, 3.3, lc, lc, 1000, 0, 0);
        sim_run(m, 9.0);
        static const struct { int v; long n; } want[] = { { 1, 6 * 5512 }, { 2, 1000 }, { 3, 1000 }, { 2, 1000 }, { 3, 1000 },
                                                          { 4, 1000 } };
        long k = first_sound(m, 0);
        ok = k > 0;
        for (int i = 0; ok && i < 6; i++) {
            long r = 0;
            while (k < m->nout && m->out[k] == want[i].v) { k++; r++; }
            if (r != want[i].n) ok = 0;
        }
        CHECK(ok && m->out[k] == 0x80 && m->ndone == 4, "a loop written while another is queued: %d buffers done of 4", m->ndone);
    }

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

    /* two loops queued, one after the other, broken during the first one's first pass: that one ends with it, the
     * other plays all its passes; each buffer done once, after its last frame */
    for (int plain = 0; plain < 2; plain++) {
        sim_init(m, 0, 10);
        memset(buf[2], 10, 1000); memset(buf[3], 20, 1000); memset(buf[4], 30, 1000);
        wc_write(&m->s, 0.1, buf[2], buf[2], 1000, WC_BEGINLOOP | WC_ENDLOOP, 3);
        if (plain) wc_write(&m->s, 0.1, buf[4], buf[4], 1000, 0, 0);
        wc_write(&m->s, 0.1, buf[3], buf[3], 1000, WC_BEGINLOOP | WC_ENDLOOP, 3);
        sim_run(m, 0.12);
        wc_breakloop(&m->s, m->t);
        sim_run(m, 3.0);
        n10 = n20 = n30 = 0;
        for (k = 0; k < m->nout; k++) n10 += m->out[k] == 10, n20 += m->out[k] == 20, n30 += m->out[k] == 30;
        CHECK(n10 == 1000 && n20 == 3000 && n30 == plain * 1000, "two loops, the first broken: %ld %ld %ld", n10, n20, n30);
        CHECK(m->ndone == 2 + plain && m->done[0] == buf[2] && m->done[1 + plain] == buf[3] &&
              m->done_t[1 + plain] > 0.1 + (4000.0 + plain * 1000) / RATE,
              "two loops, the first broken: %d done, the second loop's at %.3f", m->ndone, m->done_t[1 + plain]);
    }
    /* broken during the loop's last pass, its first buffer played already; and after the loop's end, before a tick
     * has seen it: nothing to break, no buffer done twice */
    for (int late = 0; late < 2; late++) {
        sim_init(m, 0, 10);
        memset(buf[2], 10, 1000); memset(buf[3], 20, 1000); memset(buf[4], 30, 1000);
        wc_write(&m->s, 0.1, buf[2], buf[2], 1000, WC_BEGINLOOP, 2);
        wc_write(&m->s, 0.1, buf[3], buf[3], 1000, WC_ENDLOOP, 0);
        wc_write(&m->s, 0.1, buf[4], buf[4], 1000, 0, 0);
        double tb = 0.1 + (late ? 4000.5 : 3500.0) / RATE;
        sim_run(m, tb);
        int64_t loop_end = m->s.seg[m->s.n - 1].start;
        if (late) while (wc_ideal(&m->s, tb) < (double)loop_end + 2) tb += 0.0001;
        wc_breakloop(&m->s, tb);
        sim_run(m, 3.0);
        n10 = n20 = n30 = 0;
        for (k = 0; k < m->nout; k++) n10 += m->out[k] == 10, n20 += m->out[k] == 20, n30 += m->out[k] == 30;
        CHECK(n10 == 2000 && n20 == 2000 && n30 == 1000 && m->ndone == 3 && m->done[0] == buf[2] && m->done[1] == buf[3],
              "broken %s: %ld %ld %ld, %d done", late ? "after the loop's end" : "in the last pass", n10, n20, n30, m->ndone);
    }

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
    static const double ppms[] = { 0, 150, -150, 600, 0 };
    for (int pi = 0; pi < 5; pi++) {
        sim_init(m, ppms[pi], 10);
        if (pi == 4) m->burst = 12;         /* (a mixer that takes 120 ms at a time) */
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
        CHECK(worst <= (fabs(ppms[pi]) > 500 || pi == 4 ? 2 : 1),
              "resetting game, mixer %+.0f ppm: %ld jumps, the worst %ld frames", ppms[pi], jumps, worst);
        printf("wavecore: resetting game, mixer clock %+.0f ppm%s: %ld splices off by a frame, the worst %ld\n", ppms[pi],
               pi == 4 ? ", 120 ms at a time" : "", jumps, worst);
    }

    free(m->out);
    wc_free(&m->s);
    if (fails) { printf("wavecore tests: %d FAILED\n", fails); return 1; }
    printf("wavecore tests: all passed\n");
    return 0;
}
