/* reAL - ALchemy's output limiter.  See limiter.h.
 *
 * The arithmetic is the DLL's: x87 at 53 bits, so in double here, each store to a float where the DLL stores
 * one.  Built without -ffast-math and without fused multiply-adds (build.sh), with SSE arithmetic: an x87 build
 * rounds otherwise and is not the DLL's bit for bit. */
#include <string.h>
#include <math.h>
#include <float.h>
#include "limiter.h"
#ifdef __SSE__
#include <xmmintrin.h>
#endif

void rl_limiter_init(rl_limiter *l, uint32_t rate, uint32_t nch, int fixed)
{
    memset(l, 0, sizeof(*l));
    l->fixed = fixed; l->gain = 1.0f;
    l->delay = (uint32_t)((uint64_t)rate * 3 / 1000);
    if (l->delay < 1) l->delay = 1;
    if (l->delay > RL_LIM_MAXDELAY) l->delay = RL_LIM_MAXDELAY;
    /* (CWASAPIRender: periods of 128 * floor(30000 * rate / 1.28e9) frames; none below 42667 Hz, where ALchemy's
     * engine does not run: 128 there) */
    l->block = 128 * (uint32_t)(30000.0 * rate / 1.28e9);
    if (l->block < 128) l->block = 128;
    l->down = (float)(1.0 / ((double)0.005f * rate));
    l->up = (float)(1.0 / ((double)0.05f * rate));
    l->nch = nch > RL_LIM_MAXCH ? RL_LIM_MAXCH : nch;
    l->lowest = 1.0f;
    for (uint32_t c = 0; c < RL_LIM_MAXCH; c++) l->ch[c].gain = 1.0f;
    for (uint32_t i = 0; i < l->delay; i++) l->weight[i] = 1.0f - (float)i / (float)l->delay;
}

void rl_limiter_chan_process(rl_limiter *l, rl_limiter_chan *c, float *buf, uint32_t n, uint32_t stride)
{
    const uint32_t D = l->delay;
    if (!n) return;
    /* the peak of what is in sight (the ring, then the block coming in) and where it is, counted from the next
     * sample out; and the same for the n samples this call puts out */
    double peak = 0.0, peak_out = 0.0;
    uint32_t at = 0, at_out = 0, r = c->at;
    for (uint32_t i = 0; i < D; i++) {
        const double a = fabs((double)c->ring[r]);
        if (a > peak) { peak = a; at = i; }
        if (i == n - 1) { peak_out = peak; at_out = at; }
        if (++r >= D) r = 0;
    }
    for (uint32_t k = 0; k < n; k++) {
        const double a = fabs((double)buf[(size_t)k * stride]);
        if (a > peak) { peak = a; at = D + k; }
        if (D + k == n - 1) { peak_out = peak; at_out = at; }
    }

    const double g = c->gain;
    const double k98 = (double)0.98f, k995 = (double)0.995f;
    float step = 0.0f;                  /* what the gain moves by, a sample */
    uint32_t until = 0;                 /* going down: as far as this sample */
    if (g == 1.0 && peak < 1.0) {
        /* nothing to do: the delay alone */
        for (uint32_t k = 0; k < n; k++) {
            float *x = &buf[(size_t)k * stride];
            const float in = *x;
            *x = c->ring[c->at]; c->ring[c->at] = in;
            if (++c->at >= D) c->at = 0;
        }
        return;
    }
    if (peak > 1.0) {
        if (peak_out < (double)1e-8f) c->gain = (float)(k98 / peak);    /* (silence going out: at once) */
        else if (g * peak > 1.0) {
            /* down to where the peak comes out at 0.98, in a line that ends at it; if the peak of what goes out
             * now needs a steeper line, that one */
            const double slope = at ? peak / at : HUGE_VAL, slope_out = at_out ? peak_out / at_out : HUGE_VAL;
            double s;
            if (slope < slope_out && peak_out * g > k98) { until = at_out; s = at_out ? (k98 / peak_out - g) / at_out : -HUGE_VAL; }
            else { until = at; s = at ? (k98 / peak - g) / at : -HUGE_VAL; }
            step = (float)s;
            if ((double)l->down < fabs((double)step)) step = -l->down;
        } else if (peak * (g + at * (double)l->up) < k98) step = (float)((double)l->up * 0.5);
    } else if (g < 1.0) {
        step = (float)((1.0 - g) / n);
        if ((double)l->up < (double)step) step = l->up;
    }

    if (step > 0.0f) {
        /* back up; at 0.995 it is 1 again */
        for (uint32_t k = 0; k < n; k++) {
            float *x = &buf[(size_t)k * stride];
            const float in = *x;
            *x = (float)((double)c->ring[c->at] * (double)c->gain); c->ring[c->at] = in;
            c->gain = (float)((double)step + (double)c->gain);
            if (!((double)c->gain < k995)) { c->gain = 1.0f; step = 0.0f; }
            if (++c->at >= D) c->at = 0;
        }
    } else if (step < 0.0f) {
        for (uint32_t k = 0; k < n; k++) {
            float *x = &buf[(size_t)k * stride];
            const float in = *x;
            *x = (float)((double)c->ring[c->at] * (double)c->gain); c->ring[c->at] = in;
            if (k > until) step = 0.0f;
            c->gain = (float)((double)step + (double)c->gain);
            if (++c->at >= D) c->at = 0;
        }
        if (c->gain < 0.0f) c->gain = FLT_MIN;
    } else {
        const double gc = c->gain;
        for (uint32_t k = 0; k < n; k++) {
            float *x = &buf[(size_t)k * stride];
            const float in = *x;
            *x = (float)((double)c->ring[c->at] * gc); c->ring[c->at] = in;
            if (++c->at >= D) c->at = 0;
        }
    }
    if (c->gain < 1.0f || g < 1.0) {
        l->limited += n;
        const float low = c->gain < (float)g ? c->gain : (float)g;
        if (low < l->lowest) l->lowest = low;
    }
}

/* reAL's own (see limiter.h): a line for every frame above full scale, one gain for all the channels */

/* the most that any frame in sight asks the gain to come down by now: need[] from the oldest (the next out, in
 * full) to the one that came in last (hardly), in the ring's two stretches */
static float most_needed(const rl_limiter *l, uint32_t at)
{
    const uint32_t D = l->delay;
    float most = 0.0f;
    for (int half = 0; half < 2; half++) {
        const float *nd = half ? l->need : l->need + at, *w = half ? l->weight + (D - at) : l->weight;
        const uint32_t n = half ? at : D - at;
        uint32_t j = 0;
#ifdef __SSE__
        __m128 m = _mm_setzero_ps();
        for (; j + 4 <= n; j += 4) m = _mm_max_ps(m, _mm_mul_ps(_mm_loadu_ps(nd + j), _mm_loadu_ps(w + j)));
        float q[4];
        _mm_storeu_ps(q, m);
        for (int k = 0; k < 4; k++) if (q[k] > most) most = q[k];
#endif
        for (; j < n; j++) { const float x = nd[j] * w[j]; if (x > most) most = x; }
    }
    return most;
}

void rl_limiter_fold(rl_limiter *l, const float *m, uint32_t stride, uint32_t nout)
{
    l->fold_out = m ? (nout > RL_LIM_MAXCH ? RL_LIM_MAXCH : nout) : 0;
    for (uint32_t c = 0; c < l->nch && m; c++)
        for (uint32_t o = 0; o < l->fold_out; o++) l->fold[c][o] = m[(size_t)c * stride + o];
}

static void process_fixed(rl_limiter *l, float *buf, uint32_t frames)
{
    const uint32_t D = l->delay, nch = l->nch;
    uint32_t at = l->ch[0].at;
    float g = l->gain;
    for (uint32_t k = 0; k < frames; k++, buf += nch) {
        const float want = l->needed ? 1.0f - most_needed(l, at) : 1.0f;
        g += l->up;
        if (g > want) g = want;
        float peak = 0.0f, sum[RL_LIM_MAXCH] = { 0 };
        for (uint32_t c = 0; c < nch; c++) {
            float in = buf[c], a = fabsf(in);
            if (!(a <= FLT_MAX)) in = a = 0.0f;         /* (an infinity or a NaN: no gain brings it down) */
            if (a > peak) peak = a;
            if (l->fold_out) for (uint32_t o = 0; o < l->fold_out; o++) sum[o] += in * l->fold[c][o];
            buf[c] = l->ch[c].ring[at] * g;
            l->ch[c].ring[at] = in;
        }
        if (l->fold_out) {          /* (what counts is what the device's speakers get) */
            peak = 0.0f;
            for (uint32_t o = 0; o < l->fold_out; o++) {
                const float a = fabsf(sum[o]);
                if (!(a <= peak)) peak = a <= FLT_MAX ? a : INFINITY;       /* (a sum that overflowed: silence) */
            }
        }
        float need = 0.0f;
        if (peak > 1.0f) {
            const volatile float as_kept = 1.0f - 0.98f / peak;     /* (a float, whatever the FPU works in) */
            need = as_kept;
            /* (a peak of hundreds of thousands of times full scale: 1 - need is not fine enough to bring it to
             * 0.98; silence then, as for a folded sum that is no number) */
            if (!(peak * (1.0f - need) <= 0.99f)) need = 1.0f;
        }
        l->needed += (need != 0.0f) - (l->need[at] != 0.0f);
        l->need[at] = need;
        if (++at >= D) at = 0;
        if (g < 1.0f) { l->limited++; if (g < l->lowest) l->lowest = g; }
    }
    l->ch[0].at = at;
    l->gain = g;
}

void rl_limiter_process(rl_limiter *l, float *buf, uint32_t frames)
{
    if (l->fixed) { process_fixed(l, buf, frames); return; }
    while (frames) {
        const uint32_t n = frames < l->block ? frames : l->block;
        const uint32_t before = l->limited;
        for (uint32_t c = 0; c < l->nch; c++) rl_limiter_chan_process(l, &l->ch[c], buf + c, n, l->nch);
        if (l->limited != before) l->limited = before + n;      /* (frames, whichever channels) */
        buf += (size_t)n * l->nch;
        frames -= n;
    }
}
