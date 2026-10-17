/* reAL - ALchemy's stereo virtualizer (channel converter mode 3).  See virt.h. */
#include "virt.h"

#include <stdlib.h>
#include <string.h>

#include "alrv_virt_tables.h"

#define VIRT_MAXLEN 80
#define C_TO_LR    0.7071067690849304f       /* converter: centre into both sides (0x100b5158) */

void rl_virt_init(rl_virt *v, uint32_t rate)
{
    memset(v, 0, sizeof(*v));
    for (int i = 0; i < 3; i++) if (k_virt[i].rate == rate) v->tab = &k_virt[i];
    v->hist_len = VIRT_MAXLEN;
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
        float *nh = calloc((size_t)4 * stride, sizeof(float));
        if (!nh) return 0;
        if (v->hist) for (int s = 0; s < 4; s++) memcpy(nh + s * stride, v->hist + s * v->hist_cap, VIRT_MAXLEN * sizeof(float));
        free(v->hist);
        v->hist = nh; v->hist_cap = stride;
    }
    float *u[4];
    for (int s = 0; s < 4; s++) u[s] = v->hist + s * v->hist_cap + VIRT_MAXLEN;
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

void rl_virt_free(rl_virt *v)
{
    free(v->hist);
    v->hist = NULL;
    v->hist_cap = 0;
}
