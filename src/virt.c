/* reAL - ALchemy's stereo virtualizer (channel converter mode 3).  See virt.h. */
#include "virt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "alrv_virt_tables.h"
#include "virt_hp_tables.h"

#define VIRT_MAXLEN 80
#define C_TO_LR    0.7071067690849304f       /* converter: centre into both sides (0x100b5158) */

#define NHIST 6                 /* FIR inputs: side sum/diff, back sum/diff, front sum/diff */
#define ER_RING 1024            /* > the longest early-reflection delay (703 at 48 kHz) */
#define ER_AP_C 0.600000024f    /* early-reflection allpass coefficient (0x100b4e90) */

/* headphones: early reflections (fn 0x100376d0) */
struct rl_virt_hp {
    const virt_hp_tables *tab;
    float ring[8][ER_RING];     /* one per input slot */
    uint32_t pos[8], len[8];    /* fixed: write position; ALchemy mode: its block ring length */
    float lp[2];                /* one-pole smoothers (fn 0x10037990) */
    float ap[2][ER_RING];       /* allpasses (fn 0x1003ad10) */
    uint32_t ap_pos, ap_delay;
    int alchemy;                /* ALchemy's block-wise taps (see rl_virt_hp_alchemy_er) */
    float e[2][ER_RING];        /* its per-block tap sums */
};

void rl_virt_init(rl_virt *v, uint32_t rate)
{
    memset(v, 0, sizeof(*v));
    for (int i = 0; i < 3; i++) if (k_virt[i].rate == rate) v->tab = &k_virt[i];
    v->hist_len = VIRT_MAXLEN;
}

int rl_virt_init_hp(rl_virt *v, uint32_t rate)
{
    rl_virt_init(v, rate);
    if (!v->tab) return 1;                      /* other rates: ALchemy's plain fold-down (mode 4) */
    v->hp = calloc(1, sizeof(*v->hp));
    if (!v->hp) return 0;
    for (int i = 0; i < 3; i++) if (k_vhp[i].rate == rate) v->hp->tab = &k_vhp[i];
    for (int i = 0; i < 8; i++) v->hp->len[i] = (v->hp->tab->er_size[i] + 8) & ~7u;
    /* fn 0x10037470: rate * 256 / 48000, rounded */
    v->hp->ap_delay = (uint32_t)lrint((double)(float)(rate << 8) / 48000.0 + 1.4999999464748726e-08);
    return 1;
}

static inline float biquad(float x, const float *c, float *s)
{
    float w = x + c[0] * s[0] + c[1] * s[1];
    float y = c[2] * w + c[3] * s[0] + c[4] * s[1];
    s[1] = s[0]; s[0] = w;
    return y;
}

/* y[n] += sum h[k] u[n-1-k];  u points at sample n=0, history before it */
static void fir_add(float *y, const float *u, const float *h, uint32_t len, uint32_t frames)
{
    for (uint32_t n = 0; n < frames; n++) {
        const float *x = u + n - 1;
        float acc = 0.0f;
        for (uint32_t k = 0; k < len; k++) acc += h[k] * x[-(int32_t)k];
        y[n] += acc;
    }
}

/* the input slots of the early reflections (0x100ae310: FL FR SL SR BL BR FC BC; no BC here) */
static const int k_er_chan[8] = { ALRV_FL, ALRV_FR, ALRV_SL, ALRV_SR, ALRV_BL, ALRV_BR, ALRV_FC, -1 };

/* headphones (converter mode 0, fn 0x10036860's headphone branch): the side, rear and front pairs through
 * their sum/difference FIRs straight to the ears, early reflections per input channel (four taps to each ear,
 * one-pole smoothing y = (y + x) / 2, an allpass), the centre at 0.7071 */
static int hp_process(rl_virt *v, float *const ch[ALRV_NOUT], float *out, uint32_t frames, float *tmp, float **u)
{
    const alrv_virt_tables *t = v->tab;
    struct rl_virt_hp *h = v->hp;
    const virt_hp_tables *ht = h->tab;
    for (uint32_t n = 0; n < frames; n++) {
        u[0][n] = ch[ALRV_SL][n] + ch[ALRV_SR][n]; u[1][n] = ch[ALRV_SL][n] - ch[ALRV_SR][n];
        u[2][n] = ch[ALRV_BL][n] + ch[ALRV_BR][n]; u[3][n] = ch[ALRV_BL][n] - ch[ALRV_BR][n];
        u[4][n] = ch[ALRV_FL][n] + ch[ALRV_FR][n]; u[5][n] = ch[ALRV_FL][n] - ch[ALRV_FR][n];
    }
    float *S = tmp, *D = tmp + frames;
    memset(tmp, 0, 2 * frames * sizeof(float));
    fir_add(S, u[2], t->back_s, t->back_len, frames);
    fir_add(D, u[3], t->back_d, t->back_len, frames);
    fir_add(S, u[0], t->side_s, t->side_len, frames);
    fir_add(D, u[1], t->side_d, t->side_len, frames);
    fir_add(S, u[4], ht->front_s, ht->front_len, frames);
    fir_add(D, u[5], ht->front_d, ht->front_len, frames);
    for (int s = 0; s < NHIST; s++)
        memmove(u[s] - VIRT_MAXLEN, u[s] + frames - VIRT_MAXLEN, VIRT_MAXLEN * sizeof(float));

    if (h->alchemy) {
        /* ALchemy reads a block's taps from the ring before writing the block into it, so taps shorter than
         * the block would reach a lap (the ring length) too far back for part of it (its blocks are 128
         * frames, shorter than every tap) */
        memset(h->e, 0, sizeof h->e);
        for (int i = 0; i < 8; i++) {
            if (k_er_chan[i] < 0) continue;
            const uint32_t len = h->len[i];
            float *r = h->ring[i];
            for (int k = 0; k < 8; k++) {
                uint32_t q = (h->pos[i] + len - ht->er_delay[i][k]) % len;
                for (uint32_t n = 0; n < frames && n < ER_RING; n++) {
                    h->e[k >> 2][n] += r[q] * k_vhp_er_gain[i][k];
                    if (++q == len) q = 0;
                }
            }
            for (uint32_t n = 0; n < frames; n++) {
                r[h->pos[i]] = ch[k_er_chan[i]][n];
                if (++h->pos[i] == len) h->pos[i] = 0;
            }
        }
    }
    for (uint32_t n = 0; n < frames; n++) {
        float e[2] = { 0.0f, 0.0f };
        if (h->alchemy) { e[0] = h->e[0][n < ER_RING ? n : 0]; e[1] = h->e[1][n < ER_RING ? n : 0]; }
        else for (int i = 0; i < 8; i++) {
            if (k_er_chan[i] < 0) continue;
            float *r = h->ring[i];
            uint32_t p = h->pos[i];
            r[p] = ch[k_er_chan[i]][n];
            for (int k = 0; k < 8; k++)
                e[k >> 2] += r[(p - ht->er_delay[i][k]) & (ER_RING - 1)] * k_vhp_er_gain[i][k];
            h->pos[i] = (p + 1) & (ER_RING - 1);
        }
        uint32_t ap = h->ap_pos, apr = (ap - h->ap_delay) & (ER_RING - 1);
        float y2[2];
        for (int k = 0; k < 2; k++) {
            float x = h->lp[k] = (h->lp[k] + e[k]) * 0.5f;
            float y = x * ER_AP_C + h->ap[k][apr];
            h->ap[k][ap] = x - y * ER_AP_C;
            y2[k] = y;
        }
        h->ap_pos = (ap + 1) & (ER_RING - 1);
        float c = ch[ALRV_FC][n] * C_TO_LR;
        out[2 * n]     += (S[n] + D[n]) + y2[0] + c;
        out[2 * n + 1] += (S[n] - D[n]) + y2[1] + c;
    }
    return 1;
}

/* ch: 8 channel buffers (ALRV_* order, centre copy already folded), out: interleaved stereo += */
int rl_virt_process(rl_virt *v, float *const ch[ALRV_NOUT], float *out, uint32_t frames, float *tmp)
{
    const alrv_virt_tables *t = v->tab;
    if (!t) {
        for (uint32_t n = 0; n < frames; n++) {
            float c = ch[ALRV_FC][n] * C_TO_LR;
            out[2 * n]     += ch[ALRV_FL][n] + ch[ALRV_BL][n] + ch[ALRV_SL][n] + c;
            out[2 * n + 1] += ch[ALRV_FR][n] + ch[ALRV_BR][n] + ch[ALRV_SR][n] + c;
        }
        return 1;
    }
    uint32_t stride = VIRT_MAXLEN + frames;
    if (v->hist_cap < stride) {
        float *nh = calloc((size_t)NHIST * stride, sizeof(float));
        if (!nh) return 0;
        if (v->hist) for (int s = 0; s < NHIST; s++) memcpy(nh + s * stride, v->hist + s * v->hist_cap, VIRT_MAXLEN * sizeof(float));
        free(v->hist);
        v->hist = nh; v->hist_cap = stride;
    }
    float *u[NHIST];
    for (int s = 0; s < NHIST; s++) u[s] = v->hist + s * v->hist_cap + VIRT_MAXLEN;
    if (v->hp) return hp_process(v, ch, out, frames, tmp, u);
    for (uint32_t n = 0; n < frames; n++) {
        u[0][n] = ch[ALRV_SL][n] + ch[ALRV_SR][n]; u[1][n] = ch[ALRV_SL][n] - ch[ALRV_SR][n];
        u[2][n] = ch[ALRV_BL][n] + ch[ALRV_BR][n]; u[3][n] = ch[ALRV_BL][n] - ch[ALRV_BR][n];
    }
    float *S = tmp, *D = tmp + frames;
    memset(tmp, 0, 2 * frames * sizeof(float));
    fir_add(S, u[0], t->side_s, t->side_len, frames);
    fir_add(D, u[1], t->side_d, t->side_len, frames);
    fir_add(S, u[2], t->back_s, t->back_len, frames);
    fir_add(D, u[3], t->back_d, t->back_len, frames);
    for (int s = 0; s < 4; s++)       /* keep history */
        memmove(u[s] - VIRT_MAXLEN, u[s] + frames - VIRT_MAXLEN, VIRT_MAXLEN * sizeof(float));
    const uint32_t D_ = t->delay;
    for (uint32_t n = 0; n < frames; n++) {
        float l = S[n] + D[n], r = S[n] - D[n];
        for (int k = 0; k < 3; k++) { l = biquad(l, t->bq[k], v->bq[k]); r = biquad(r, t->bq[3 + k], v->bq[3 + k]); }
        float a = biquad(biquad(l, t->bq[6], v->bq[6]), t->bq[7], v->bq[7]);
        float b = biquad(biquad(r, t->bq[8], v->bq[8]), t->bq[9], v->bq[9]);
        /* crossfeed: each side gets the other's filtered signal D samples late */
        float bd = v->dly[0][v->dpos], ad = v->dly[1][v->dpos];
        v->dly[0][v->dpos] = b; v->dly[1][v->dpos] = a;
        if (++v->dpos == D_) v->dpos = 0;
        float c = ch[ALRV_FC][n] * C_TO_LR;
        out[2 * n]     += (l + bd) + ch[ALRV_FL][n] + c;
        out[2 * n + 1] += (r + ad) + ch[ALRV_FR][n] + c;
    }
    return 1;
}

void rl_virt_hp_alchemy_er(rl_virt *v, int on)
{
    if (!v->hp) return;
    v->hp->alchemy = on;
    memset(v->hp->ring, 0, sizeof v->hp->ring);
    memset(v->hp->pos, 0, sizeof v->hp->pos);
}

void rl_virt_free(rl_virt *v)
{
    free(v->hp);
    v->hp = NULL;
    free(v->hist);
    v->hist = NULL;
    v->hist_cap = 0;
}
