/* Native unit tests for the platform independent parts of reAL. */
#include "../src/mixcore.h"
#include "../src/spatial.h"
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
    return v;
}

static void test_basic_mix(void)
{
    rl_voice v = make_voice(1);
    float out[20] = { 0 };
    rl_voice_mix(&v, out, 10, 44100, NULL);
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
    rl_voice_mix(&v, out, 4, 44100, NULL);
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
    rl_voice_mix(&v, out, 75, 44100, sig);   /* 0 -> 150 bytes */
    CHECK(fired[1] == 0 && fired[2] == 1 && fired[3] == 0);
    rl_voice_mix(&v, out, 50, 44100, sig);   /* 150 -> wrap -> 50 bytes */
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
    int stopped = rl_voice_mix(&v, out, 150, 44100, sig);
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
    v.rate = 1000; v.playing = 1; v.looping = 1; v.gain[0] = v.gain[1] = 1;
    float out[4] = { 0 };
    rl_voice_mix(&v, out, 2, 1000, NULL);
    CHECK(NEAR(out[0], 0) && NEAR(out[1], 127 / 128.0f));
    CHECK(NEAR(out[2], -1) && NEAR(out[3], 0));
}

static void test_spatial(void)
{
    rl_listener3d l; rl_source3d s; float g[2];
    rl_listener_defaults(&l); rl_source_defaults(&s);

    s.pos.z = 1; rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 1));

    s.pos.x = 1; s.pos.z = 0; rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 0) && NEAR(g[1], 1));

    s.pos.x = -1; rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 0));

    /* inverse distance: 2x min distance straight ahead -> 0.5 */
    s.pos.x = 0; s.pos.z = 2; rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 0.5f) && NEAR(g[1], 0.5f));

    /* rolloff 0 -> no attenuation */
    l.rolloff_factor = 0; rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 1));
    l.rolloff_factor = 1;

    /* listener turned to face +x: a source at +x is now in front */
    l.front.x = 1; l.front.z = 0;
    s.pos.x = 5; s.pos.z = 0; s.min_dist = 5;
    rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 1));
    /* and a source at +z is to the listener's left */
    s.pos.x = 0; s.pos.z = 5; rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 0));

    /* max distance clamp */
    rl_listener_defaults(&l); rl_source_defaults(&s);
    s.max_dist = 4; s.pos.z = 100; rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 0.25f));
    s.mute_at_max = 1; rl_spatialize(&l, &s, g);
    CHECK(g[0] == 0.0f);

    /* cone: source faces away from listener, outside volume -6 dB */
    rl_source_defaults(&s);
    s.pos.z = 1; s.cone_inside = 90; s.cone_outside = 180; s.cone_outside_volume = -600;
    s.cone_orient.z = 1; /* pointing away from listener at origin */
    rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 0.501187f));
    s.cone_orient.z = -1; /* pointing at listener */
    rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 1));

    /* disabled mode */
    s.mode = RL_DS3DMODE_DISABLE; s.pos.x = 100;
    rl_spatialize(&l, &s, g);
    CHECK(NEAR(g[0], 1) && NEAR(g[1], 1));
}

int main(void)
{
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
