/* reAL - inaudible voices: every voice mixed twice from the same start, once as the mixer does it (an inaudible
 * voice only moves its position) and once with rl_mix_no_idle (mixed in full, as before).  Random sources, formats,
 * rates, loops, jumps and gains that go to 0 and back; stereo, multichannel, 3D routed on 5.1, HRTF on headphones
 * and on 5.1, CMSS-3D on stereo and 5.1.
 * Must hold:
 *   - positions, loop wraps, ends, crossfades, gain ramps and every notification (and their order): always equal;
 *   - the output: bit for bit, except
 *       - EAX HF damping: the replayed filter within 1e-6, unless the position jumped while the voice was idle
 *         (nothing to replay then; the filter follows the signal again before the voice is heard if it stays
 *         muted ~5 ms);
 *       - CMSS-3D after a second of silence (its analysis state held): only reported.
 * test_idle [cases] */
#include "../src/mixcore.h"
#include "../src/cmss3.h"
#include "../src/virt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int evlog[2][4096], nev[2], who;
static void sig(void *e) { if (nev[who] < 4096) evlog[who][nev[who]++] = (int)(intptr_t)e; }
static unsigned seed = 777;
static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }
static float frand(void) { return (rnd() % 100000) / 100000.0f; }

static int mix(rl_voice *v, int full, float *out, uint32_t nout, float *const *sends, uint32_t n)
{
    rl_mix_no_idle = full;
    who = full;
    int r = rl_voice_mix_n(v, out, nout, sends, n, 48000, sig);
    rl_mix_no_idle = 0;
    return r;
}

int main(int argc, char **argv)
{
    int cases = argc > 1 ? atoi(argv[1]) : 2000, bad = 0, exact_cases = 0, idle_blocks = 0, hrtf_skips = 0;
    int hf_checked = 0, cmss_blocks = 0;
    double hf_worst = 0, cmss_worst = 0;
    static rl_notify nt[6];
    static rl_hrtf_filter flt;
    rl_hrtf_set set;
    memset(&set, 0, sizeof set);
    set.rate = 48000; set.dirs = &flt;
    rl_mix_prepare();
    for (int cs = 0; cs < cases; cs++) {
        /* kind: 0 stereo, 1 multichannel, 2 routed 3D on 5.1, 3 HRTF (headphones), 4 HRTF routed on 5.1, 5 CMSS */
        int kind = rnd() % 6;
        int sf = rnd() % 5, ch = kind == 1 ? (rnd() % 2 ? 6 : 4) : kind == 5 ? 2 : 1 + rnd() % 2;
        int bps = sf == RL_U8 ? 1 : sf == RL_S16 ? 2 : sf == RL_S24 ? 3 : 4;
        int frames = 5 + rnd() % 3000;
        if (rnd() % 4 == 0) frames = 1 + rnd() % 40;
        int sz = frames * ch * bps;
        uint8_t *d = malloc(sz);
        for (int i = 0; i < sz; i++) d[i] = rnd();
        if (sf == RL_F32) for (int i = 0; i < frames * ch; i++) { float f = frand() * 2 - 1; memcpy(d + 4 * i, &f, 4); }
        rl_voice a;
        memset(&a, 0, sizeof a);
        a.data = d; a.size = sz; a.channels = ch; a.block = ch * bps; a.sfmt = sf;
        a.rate = 4000 + rnd() % 92000;
        if (rnd() % 3 == 0) a.rate = 48000;
        a.pitch = rnd() % 4 == 0 ? 0.5f + frand() : 0; a.interp = rnd() % 2; a.looping = rnd() % 2;
        int lp = rnd() % 4 == 0;
        a.lp_a = lp ? 0.05f + frand() * 0.9f : 1.0f;
        for (int k = 0; k < RL_SENDS; k++) a.send_lp_a[k] = lp && rnd() % 2 ? 0.05f + frand() * 0.9f : 1.0f;
        int nout = (kind == 1 || kind == 2 || kind == 4) ? 6 : (kind == 5 && rnd() % 2) ? 6 : 2;
        if (kind == 1) {
            a.mc_on = 1; a.mc_lfe = rnd() % 2 ? 3 : -1;
            for (int s = 0; s < ch; s++) for (int c = 0; c < nout; c++) a.mc[s][c] = rnd() % 3 == 0 ? frand() : 0;
            rl_bass_limiter_init(&a.lfe_lim, 48000);
        }
        if (kind == 2 || kind == 4) {
            a.route_on = 1;
            for (int k = 0; k < 3; k++) for (int c = 0; c < nout; c++) a.rt[k][c] = frand();
        }
        rl_hrtf_voice *ha = NULL, *hb = NULL;
        if (kind == 3 || kind == 4) {
            set.taps = 1 + rnd() % RL_HRTF_MAX_TAPS;
            for (int c = 0; c < 2; c++) {
                flt.delay[c] = rnd() % 40;
                for (int k = 0; k < RL_HRTF_MAX_TAPS; k++) flt.coef[c][k] = frand() - 0.5f;
            }
            ha = calloc(1, sizeof *ha); hb = calloc(1, sizeof *hb);
            rl_hrtf_voice_set(ha, &flt); rl_hrtf_voice_set(hb, &flt);
            a.hrtf_set = &set;
        }
        rl_cmss3 *ca = NULL, *cb = NULL;
        rl_virt *va = NULL, *vb = NULL;
        if (kind == 5) {
            uint32_t mask = nout == 6 ? 0x3f : 0x3;
            ca = rl_cmss3_create(48000, mask, nout == 2); cb = rl_cmss3_create(48000, mask, nout == 2);
            for (int b = 0; b < 12; b++) a.cmss_idx[b] = nout == 6 && b < 6 ? b : -1;
            if (nout == 2) {
                va = calloc(1, sizeof *va); vb = calloc(1, sizeof *vb);
                rl_virt_init(va, 48000); rl_virt_init(vb, 48000);
            }
        }
        for (int k = 0; k < 4; k++) { nt[k].offset = (rnd() % frames) * a.block; nt[k].event = (void *)(intptr_t)(k + 1); }
        nt[4].offset = RL_NOTIFY_STOP; nt[4].event = (void *)(intptr_t)99;
        a.notifies = nt; a.n_notifies = 5;
        a.pos = ((uint64_t)(rnd() % frames) << 32) | (rnd() % 3 == 0 ? 0 : rnd());
        if (rnd() % 2) { a.gain[0] = a.gain[1] = 0; for (int k = 0; k < RL_SENDS; k++) a.send_gain[k] = 0; }
        else {
            a.gain[0] = frand(); a.gain[1] = frand();
            for (int k = 0; k < RL_SENDS; k++) a.send_gain[k] = rnd() % 2 ? frand() : 0;
        }
        rl_voice b = a;                 /* a: mixed in full; b: as the mixer does it */
        a.hrtf = ha; b.hrtf = hb; a.cmss = ca; b.cmss = cb; a.cmss_virt = va; b.cmss_virt = vb;
        rl_voice_start(&a); rl_voice_start(&b);
        int longmute = kind == 5 && rnd() % 2;      /* past CMSS's second of silence, then heard again */
        int blocks = longmute ? 140 : 1 + rnd() % 16;
        int exact = !lp && kind != 5;
        exact_cases += exact;
        nev[0] = nev[1] = 0;
        uint32_t heard_since_jump = 1u << 30;
        for (int bl = 0; bl < blocks; bl++) {
            int n = 1 + rnd() % 700;
            if (rnd() % 5 == 0) n = 1 + rnd() % 8;
            if (longmute) n = bl < 120 ? 480 : 300;
            float *oa = calloc((size_t)n * nout, 4), *ob = calloc((size_t)n * nout, 4), *sa[RL_SENDS], *sb[RL_SENDS];
            for (int k = 0; k < RL_SENDS; k++) { sa[k] = calloc(n, 4); sb[k] = calloc(n, 4); }
            int usesends = rnd() % 2, jumped = 0;
            if (rnd() % 4 == 0) {
                uint64_t np = ((uint64_t)(rnd() % frames) << 32) | rnd();
                rl_voice_jump(&a, np); rl_voice_jump(&b, np); jumped = 1;
            }
            if (jumped && b.idle) heard_since_jump = 0;
            int r = rnd() % 3;
            if (longmute) r = bl == 0 ? 1 : bl == 120 ? 2 : 0;
            if (r == 1) { a.gain[0] = a.gain[1] = 0; for (int k = 0; k < RL_SENDS; k++) a.send_gain[k] = 0; }
            else if (r == 2) {
                a.gain[0] = frand(); a.gain[1] = frand();
                for (int k = 0; k < RL_SENDS; k++) a.send_gain[k] = rnd() % 2 ? frand() : 0;
            }
            memcpy(b.gain, a.gain, sizeof a.gain); memcpy(b.send_gain, a.send_gain, sizeof a.send_gain);
            uint32_t hw = hb ? hb->w : 0;
            int ra = mix(&a, 1, oa, nout, usesends ? sa : NULL, n);
            int rb = mix(&b, 0, ob, nout, usesends ? sb : NULL, n);
            idle_blocks += b.idle;
            if (hb && b.idle && hb->quiet >= RL_HRTF_HIST && hb->w == hw + (uint32_t)n) hrtf_skips++;
            if (jumped && b.idle) heard_since_jump = 0;
            int jump_idle = heard_since_jump < 8192;
            if (!b.idle) heard_since_jump += n;
            double err = 0;
            int bitdiff = 0;
            for (int i = 0; i < n * nout; i++) { err = fmax(err, fabs(oa[i] - ob[i])); bitdiff |= memcmp(&oa[i], &ob[i], 4) != 0; }
            for (int k = 0; k < RL_SENDS; k++)
                for (int i = 0; i < n; i++) { err = fmax(err, fabs(sa[k][i] - sb[k][i])); bitdiff |= memcmp(&sa[k][i], &sb[k][i], 4) != 0; }
            int st = ra != rb || a.pos != b.pos || a.playing != b.playing || nev[0] != nev[1] ||
                     memcmp(evlog[0], evlog[1], nev[0] * sizeof(int)) || a.xfade_left != b.xfade_left ||
                     (a.xfade_left && a.xfade_pos != b.xfade_pos) || a.cur_gain[0] != b.cur_gain[0] ||
                     memcmp(a.cur_send, b.cur_send, sizeof a.cur_send);
            int hf = !exact && kind != 5 && !jump_idle;
            if (hf) { hf_checked++; if (err > hf_worst) hf_worst = err; }
            if (kind == 5 && err > 0) { cmss_blocks++; if (err > cmss_worst) cmss_worst = err; }
            if (st || (exact && bitdiff) || (hf && err > 1e-6)) {
                if (bad < 20)
                    printf("case %d kind %d block %d (format %d, %d ch, loop %d, damping %d, %d frames): %s, difference %.3g, "
                           "pos %llx/%llx, playing %d/%d, events %d/%d\n", cs, kind, bl, sf, ch, a.looping, lp, n,
                           st ? "state differs" : "output differs", err, (unsigned long long)a.pos,
                           (unsigned long long)b.pos, a.playing, b.playing, nev[0], nev[1]);
                bad++;
            }
            free(oa); free(ob);
            for (int k = 0; k < RL_SENDS; k++) { free(sa[k]); free(sb[k]); }
            if (!a.playing) break;
        }
        free(d); free(ha); free(hb);
        rl_cmss3_destroy(ca); rl_cmss3_destroy(cb);
        if (va) { rl_virt_free(va); rl_virt_free(vb); free(va); free(vb); }
    }
    printf("idle: %d cases (%d bit-exact by design), %d idle blocks, %d HRTF blocks skipped; HF filter replay %d blocks, "
           "worst %.3g; CMSS after a second of silence %d blocks, worst %.3g; %d mismatches\n",
           cases, exact_cases, idle_blocks, hrtf_skips, hf_checked, hf_worst, cmss_blocks, cmss_worst, bad);
    if (!idle_blocks || !hrtf_skips) { printf("idle: the skip never happened\n"); return 1; }
    return bad != 0;
}
