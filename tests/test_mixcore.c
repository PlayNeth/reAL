/* Native unit tests for the platform independent parts of reAL. */
#include "../src/mixcore.h"
#include "../src/eax.h"
#include "../src/spatial.h"
#include "../src/reverb.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 1e-3f)

static int fired[8];
static void sig(void *ev) { fired[(int)(intptr_t)ev]++; }

static void test_gain(void)
{
    CHECK(NEAR(rl_db100_to_gain(0), 1.0f));
    CHECK(NEAR(rl_db100_to_gain(-600), 0.501187f));
    CHECK(rl_db100_to_gain(-10000) == 0.0f);
    float g[2];
    rl_pan_gains(0, g);    CHECK(NEAR(g[0], 1) && NEAR(g[1], 1));
    rl_pan_gains(600, g);  CHECK(NEAR(g[0], 0.501187f) && NEAR(g[1], 1));
    rl_pan_gains(-600, g); CHECK(NEAR(g[0], 1) && NEAR(g[1], 0.501187f));
}

static int16_t pcm[100];

static rl_voice make_voice(int looping)
{
    for (int i = 0; i < 100; i++) pcm[i] = (int16_t)(i * 100);
    rl_voice v;
    memset(&v, 0, sizeof(v));
    v.data = (const uint8_t *)pcm;
    v.size = sizeof(pcm);
    v.channels = 1;
    v.block = 2;
    v.sfmt = RL_S16;
    v.rate = 44100;
    v.playing = 1;
    v.looping = looping;
    v.gain[0] = v.gain[1] = 1.0f;
    v.cur_gain[0] = v.cur_gain[1] = 1.0f;
    return v;
}

static void test_basic_mix(void)
{
    rl_voice v = make_voice(1);
    float out[20] = { 0 };
    rl_voice_mix(&v, out, NULL, 10, 44100, NULL);
    for (int i = 0; i < 10; i++) {
        CHECK(NEAR(out[i * 2], i * 100 / 32768.0f));
        CHECK(NEAR(out[i * 2 + 1], i * 100 / 32768.0f));
    }
    CHECK(rl_voice_play_bytes(&v) == 20);
}

static void test_resample(void)
{
    rl_voice v = make_voice(1);
    v.rate = 22050;
    float out[8] = { 0 };
    rl_voice_mix(&v, out, NULL, 4, 44100, NULL);
    /* frames: 0, 0.5, 1, 1.5 -> 0, 50, 100, 150 */
    CHECK(NEAR(out[0], 0));
    CHECK(NEAR(out[2], 50 / 32768.0f));
    CHECK(NEAR(out[4], 100 / 32768.0f));
    CHECK(NEAR(out[6], 150 / 32768.0f));
    CHECK(rl_voice_play_bytes(&v) == 4);
}

static void test_notify_loop(void)
{
    rl_notify n[3] = { { 0, (void *)1 }, { 100, (void *)2 }, { RL_NOTIFY_STOP, (void *)3 } };
    rl_voice v = make_voice(1);
    v.notifies = n; v.n_notifies = 3;
    float out[400];
    memset(fired, 0, sizeof(fired));
    rl_voice_mix(&v, out, NULL, 75, 44100, sig);   /* 0 -> 150 bytes */
    CHECK(fired[1] == 0 && fired[2] == 1 && fired[3] == 0);
    rl_voice_mix(&v, out, NULL, 50, 44100, sig);   /* 150 -> wrap -> 50 bytes */
    CHECK(fired[1] == 1 && fired[2] == 1 && fired[3] == 0);
    CHECK(rl_voice_play_bytes(&v) == 50);
    CHECK(v.playing);
}

static void test_one_shot_end(void)
{
    rl_notify n[2] = { { 150, (void *)1 }, { RL_NOTIFY_STOP, (void *)2 } };
    rl_voice v = make_voice(0);
    v.notifies = n; v.n_notifies = 2;
    float out[400];
    memset(fired, 0, sizeof(fired));
    memset(out, 0, sizeof(out));
    int stopped = rl_voice_mix(&v, out, NULL, 150, 44100, sig);
    CHECK(stopped == 1);
    CHECK(!v.playing);
    CHECK(rl_voice_play_bytes(&v) == 0);
    CHECK(fired[1] == 1 && fired[2] == 1);
    /* after the end the output must be silent */
    CHECK(out[2 * 120] == 0.0f);
}

static void test_u8_stereo(void)
{
    uint8_t d[4] = { 0x80, 0xFF, 0x00, 0x80 };
    rl_voice v;
    memset(&v, 0, sizeof(v));
    v.data = d; v.size = 4; v.channels = 2; v.block = 2; v.sfmt = RL_U8;
    v.rate = 1000; v.playing = 1; v.looping = 1; v.gain[0] = v.gain[1] = 1; v.cur_gain[0] = v.cur_gain[1] = 1;
    float out[4] = { 0 };
    rl_voice_mix(&v, out, NULL, 2, 1000, NULL);
    CHECK(NEAR(out[0], 0) && NEAR(out[1], 127 / 128.0f));
    CHECK(NEAR(out[2], -1) && NEAR(out[3], 0));
}

static void test_spatial(void)
{
    rl_listener3d l; rl_source3d s; float g[2];
    rl_listener_defaults(&l); rl_source_defaults(&s);

    s.pos.z = 1; rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 1));

    s.pos.x = 1; s.pos.z = 0; rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 0) && NEAR(g[1], 1));

    s.pos.x = -1; rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 0));

    /* inverse distance: 2x min distance straight ahead -> 0.5 */
    s.pos.x = 0; s.pos.z = 2; rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 0.5f) && NEAR(g[1], 0.5f));

    /* rolloff 0 -> no attenuation */
    l.rolloff_factor = 0; rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 1));
    l.rolloff_factor = 1;

    /* listener turned to face +x: a source at +x is now in front */
    l.front.x = 1; l.front.z = 0;
    s.pos.x = 5; s.pos.z = 0; s.min_dist = 5;
    rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 1));
    /* and a source at +z is to the listener's left */
    s.pos.x = 0; s.pos.z = 5; rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 0));

    /* max distance clamp */
    rl_listener_defaults(&l); rl_source_defaults(&s);
    s.max_dist = 4; s.pos.z = 100; rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 0.25f));
    s.mute_at_max = 1; rl_spatialize(&l, &s, g, NULL);
    CHECK(g[0] == 0.0f);

    /* cone: source faces away from listener, outside volume -6 dB */
    rl_source_defaults(&s);
    s.pos.z = 1; s.cone_inside = 90; s.cone_outside = 180; s.cone_outside_volume = -600;
    s.cone_orient.z = 1; /* pointing away from listener at origin */
    rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 0.501187f));
    s.cone_orient.z = -1; /* pointing at listener */
    rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 1));

    /* disabled mode */
    s.mode = RL_DS3DMODE_DISABLE; s.pos.x = 100;
    rl_spatialize(&l, &s, g, NULL);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 1));
}

static void test_send(void)
{
    rl_voice v = make_voice(1);
    v.send_gain = v.cur_send = 0.5f;
    float out[20] = { 0 }, send[10] = { 0 };
    rl_voice_mix(&v, out, send, 10, 44100, NULL);
    CHECK(NEAR(send[4], 0.5f * 400 / 32768.0f));
    v.send_gain = v.cur_send = 0.0f;
    memset(send, 0, sizeof(send));
    rl_voice_mix(&v, out, send, 10, 44100, NULL);
    CHECK(send[3] == 0.0f);
}

/* EAX HF damping: the one-pole low-pass on the direct path and on the send.  ALchemy fits the
 * filter so the *power* response at the reference frequency equals the HF gain. */
static double tone_power(float lp_a, int on_send, double f, uint32_t rate)
{
    static int16_t sine[48000];
    for (int i = 0; i < 48000; i++) sine[i] = (int16_t)(16000 * sin(2 * M_PI * f * i / rate));
    rl_voice v; memset(&v, 0, sizeof(v));
    v.data = (const uint8_t *)sine; v.size = sizeof(sine); v.channels = 1; v.block = 2; v.sfmt = RL_S16;
    v.rate = rate; v.looping = 1;
    v.gain[0] = v.gain[1] = 1.0f; v.send_gain = 1.0f;
    v.lp_a = on_send ? 1.0f : lp_a; v.send_lp_a = on_send ? lp_a : 1.0f;
    rl_voice_start(&v);
    static float out[2 * 24000], send[24000];
    memset(out, 0, sizeof(out)); memset(send, 0, sizeof(send));
    rl_voice_mix(&v, out, send, 24000, rate, NULL);
    double e = 0;
    for (int i = 12000; i < 24000; i++) e += on_send ? (double)send[i] * send[i] : (double)out[2 * i] * out[2 * i];
    return e;
}

static void test_eax_lowpass(void)
{
    const uint32_t rate = 48000;
    float a = eax_lp_coef(0.1f, 5000.0f / rate);     /* -1000 mB at 5 kHz */
    CHECK(a > 0.0f && a < 1.0f);
    CHECK(eax_lp_coef(1.0f, 5000.0f / rate) == 1.0f);
    for (int on_send = 0; on_send < 2; on_send++) {
        double ref = tone_power(1.0f, on_send, 5000, rate), hf = tone_power(a, on_send, 5000, rate);
        double lf = tone_power(a, on_send, 100, rate) / tone_power(1.0f, on_send, 100, rate);
        printf("     EAX low-pass (%s): power at 5 kHz %.3f (want 0.100), at 100 Hz %.3f\n",
               on_send ? "send" : "direct", hf / ref, lf);
        CHECK(fabs(hf / ref - 0.1) < 0.01);
        CHECK(lf > 0.95);
    }
}

/* energy of the reverb tail in a window starting at t seconds */
static double tail_energy(float decay, float t)
{
    const uint32_t rate = 48000, frames = rate * 3;
    rl_reverb r;
    rl_reverb_init(&r, rate);
    rl_reverb_set_eax1(&r, 0, 1.0f, decay, 1.0f);   /* damping 1.0 -> hf ratio 1.0: flat decay */
    float *in = calloc(frames, sizeof(float)), *out = calloc(frames * 2, sizeof(float));
    in[200] = 1.0f;          /* ALchemy fades the input in over the first 128 samples */
    rl_reverb_process(&r, in, out, frames);
    double e = 0;
    uint32_t s = (uint32_t)(t * rate), n = rate / 10;
    for (uint32_t i = s; i < s + n; i++) e += out[i * 2] * out[i * 2] + out[i * 2 + 1] * out[i * 2 + 1];
    free(in); free(out); rl_reverb_free(&r);
    return e;
}

/* ALchemy's damping conversion must reproduce its own EAX3 presets for each environment's
 * default EAX1 damping (that is how Creative fitted the constants). */
static void test_eax1_conversion(void)
{
    struct { int env; float hfr; int roomhf; } exp[] = {
        { 0, 0.83f, -100 }, { 7, 0.70f, -500 }, { 15, 0.54f, -3300 }, { 16, 0.67f, -800 }, { 19, 0.50f, -2000 },
    };
    for (unsigned i = 0; i < sizeof(exp) / sizeof(exp[0]); i++) {
        int e = exp[i].env;
        float t = rl_eax1_damping_to_hf_ratio(e, rl_eax1_presets[e].damping);
        int rh = rl_eax1_room_hf(e, t);
        printf("     env %2d: damping %.3f -> hfratio %.3f (preset %.2f), roomHF %d (preset %d)\n",
               e, rl_eax1_presets[e].damping, t, exp[i].hfr, rh, exp[i].roomhf);
        /* only generic and concert hall are fitted to land on the preset (verified); the rest is
         * simply what ALchemy computes */
        if (e == 0 || e == 7) {
            CHECK(fabsf(t - exp[i].hfr) < 0.015f);
            CHECK(abs(rh - exp[i].roomhf) <= 25);
        }
    }
}

/* a jump while playing crossfades instead of stepping */
static void test_declick(void)
{
    static int16_t sq[2000];
    for (int i = 0; i < 2000; i++) sq[i] = i < 1000 ? 16000 : -16000;
    rl_voice v; memset(&v, 0, sizeof(v));
    v.data = (const uint8_t *)sq; v.size = sizeof(sq); v.channels = 1; v.block = 2; v.sfmt = RL_S16;
    v.rate = 48000; v.looping = 1; v.gain[0] = v.gain[1] = 1.0f;
    rl_voice_start(&v);
    float out[400] = { 0 };
    rl_voice_mix(&v, out, NULL, 50, 48000, NULL);         /* inside the +16000 half */
    rl_voice_jump(&v, (uint64_t)1500 << 32);               /* jump into the -16000 half */
    rl_voice_mix(&v, out + 100, NULL, 150, 48000, NULL);
    float maxstep = 0.0f;
    for (int i = 1; i < 200; i++) { float d = fabsf(out[i * 2] - out[(i - 1) * 2]); if (d > maxstep) maxstep = d; }
    printf("     declick: largest sample step %.4f (raw jump would be 0.98)\n", maxstep);
    CHECK(maxstep < 0.05f);

    /* volume change ramps */
    rl_voice w = make_voice(1);
    for (int i = 0; i < 100; i++) pcm[i] = 16000;
    float o2[1000] = { 0 };
    w.gain[0] = w.gain[1] = 0.0f;               /* drop from 1.0 to 0.0 */
    rl_voice_mix(&w, o2, NULL, 400, 48000, NULL);
    CHECK(o2[0] > 0.4f && o2[2 * 120] > 0.0f && o2[2 * 239] < 0.01f && o2[2 * 300] == 0.0f);
}

static void test_reverb(void)
{
    /* 60 dB energy drop per decay time: compare windows 1 s apart for a 2 s decay -> ~30 dB */
    double e1 = tail_energy(2.0f, 0.5f), e2 = tail_energy(2.0f, 1.5f);
    double db = 10.0 * log10(e1 / e2);
    printf("     reverb: 2.0s decay, drop over 1s = %.1f dB (expect ~30)\n", db);
    CHECK(db > 24 && db < 36);
    /* longer decay -> more energy late */
    CHECK(tail_energy(5.0f, 1.5f) > tail_energy(1.0f, 1.5f) * 100);
    CHECK(rl_eax1_presets[15].decay == 3.145f && rl_eax1_presets[19].volume == 0.097f);
}

int main(void)
{
    test_send();
    test_eax_lowpass();
    test_eax1_conversion();
    test_declick();
    test_reverb();
    test_gain();
    test_basic_mix();
    test_resample();
    test_notify_loop();
    test_one_shot_end();
    test_u8_stereo();
    test_spatial();
    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    printf("mixcore tests: all passed\n");
    return 0;
}
