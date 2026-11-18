/* test_limiter - reAL's port of ALchemy's output limiter (src/limiter.c).
 *
 * The reference is the DLL itself: tools/limref.py runs its CLimiterChan function in an emulator, and the hashes
 * below are of what it put out for these blocks (tools/limvec.py: noise, a square wave, scattered clicks, rare ones
 * and blocks of one kind each, at six levels up to a million times full scale and nine rates, 200 blocks of twelve sizes each).  The port
 * has to put out the same bits.  (tools/limcmp.py compares the two on random blocks, with the DLL at hand.)
 * Then what a limiter is for: nothing above full scale when the mix is loud, nothing changed when it is not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include "../src/limiter.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static uint32_t lcg(uint32_t s) { return s * 1664525u + 1013904223u; }

static const struct { uint32_t rate; int pattern, level; uint32_t hash; } ref[] = {
    { 8000, 0, 3, 0x5269a6a4u },
    { 8000, 0, 4, 0x11545638u },
    { 8000, 0, 6, 0x00a4ab95u },
    { 8000, 0, 16, 0x134a2b39u },
    { 8000, 0, 4000, 0x4b566da2u },
    { 8000, 0, 4000000, 0x9c5be443u },
    { 8000, 1, 3, 0x6594082du },
    { 8000, 1, 4, 0x95b11b7du },
    { 8000, 1, 6, 0xae71d282u },
    { 8000, 1, 16, 0xe6b032fcu },
    { 8000, 1, 4000, 0xad529461u },
    { 8000, 1, 4000000, 0xbe7f28cfu },
    { 8000, 2, 3, 0x2bc94578u },
    { 8000, 2, 4, 0xf546413fu },
    { 8000, 2, 6, 0x528837a5u },
    { 8000, 2, 16, 0x9b04e5a4u },
    { 8000, 2, 4000, 0x86043a05u },
    { 8000, 2, 4000000, 0x71b53cd9u },
    { 8000, 3, 3, 0x5b01d515u },
    { 8000, 3, 4, 0x324ec1c5u },
    { 8000, 3, 6, 0x2cbb93e8u },
    { 8000, 3, 16, 0x236a0bc8u },
    { 8000, 3, 4000, 0x783529eau },
    { 8000, 3, 4000000, 0xd54022d3u },
    { 8000, 4, 3, 0xc100e346u },
    { 8000, 4, 4, 0x0201a8a6u },
    { 8000, 4, 6, 0xefd55cfeu },
    { 8000, 4, 16, 0x5d38bd17u },
    { 8000, 4, 4000, 0x91c6003eu },
    { 8000, 4, 4000000, 0x7da9b9abu },
    { 11025, 0, 3, 0x7ad73912u },
    { 11025, 0, 4, 0x42b03185u },
    { 11025, 0, 6, 0x685c1565u },
    { 11025, 0, 16, 0x44552e80u },
    { 11025, 0, 4000, 0x248c9e54u },
    { 11025, 0, 4000000, 0xbe11fe02u },
    { 11025, 1, 3, 0xd2e68025u },
    { 11025, 1, 4, 0xe0b6a935u },
    { 11025, 1, 6, 0x6ad08325u },
    { 11025, 1, 16, 0x392f4551u },
    { 11025, 1, 4000, 0xe6d869b7u },
    { 11025, 1, 4000000, 0x733e26d9u },
    { 11025, 2, 3, 0xbd82e750u },
    { 11025, 2, 4, 0xeb4e5bcdu },
    { 11025, 2, 6, 0x0e7ee17fu },
    { 11025, 2, 16, 0xd30f8ba8u },
    { 11025, 2, 4000, 0x2e562da3u },
    { 11025, 2, 4000000, 0x28978faeu },
    { 11025, 3, 3, 0xe8c557dcu },
    { 11025, 3, 4, 0xa6d75720u },
    { 11025, 3, 6, 0x6d3cb86eu },
    { 11025, 3, 16, 0xe896e742u },
    { 11025, 3, 4000, 0x3dca3513u },
    { 11025, 3, 4000000, 0xe4b3fa5eu },
    { 11025, 4, 3, 0x4060cdecu },
    { 11025, 4, 4, 0xd851f578u },
    { 11025, 4, 6, 0xf5bdbcdfu },
    { 11025, 4, 16, 0xf3638e18u },
    { 11025, 4, 4000, 0x7068aea8u },
    { 11025, 4, 4000000, 0x7e8d6d68u },
    { 22050, 0, 3, 0xcd526588u },
    { 22050, 0, 4, 0x8e0f058cu },
    { 22050, 0, 6, 0x60b7db0eu },
    { 22050, 0, 16, 0xbb4adbf5u },
    { 22050, 0, 4000, 0x8b2ac4fau },
    { 22050, 0, 4000000, 0xea034642u },
    { 22050, 1, 3, 0x748ddbadu },
    { 22050, 1, 4, 0xe5252fddu },
    { 22050, 1, 6, 0x7784c830u },
    { 22050, 1, 16, 0x0eff8ad9u },
    { 22050, 1, 4000, 0x950bbc38u },
    { 22050, 1, 4000000, 0x373e3e20u },
    { 22050, 2, 3, 0x6322d446u },
    { 22050, 2, 4, 0xbf00a3feu },
    { 22050, 2, 6, 0xd70b97c5u },
    { 22050, 2, 16, 0x200fddbcu },
    { 22050, 2, 4000, 0x92b552afu },
    { 22050, 2, 4000000, 0x2ca43b47u },
    { 22050, 3, 3, 0x2aa2ef43u },
    { 22050, 3, 4, 0x930d9d0cu },
    { 22050, 3, 6, 0xe511cf24u },
    { 22050, 3, 16, 0x61448c5bu },
    { 22050, 3, 4000, 0x338044fbu },
    { 22050, 3, 4000000, 0x4ed658b8u },
    { 22050, 4, 3, 0x489562a7u },
    { 22050, 4, 4, 0x77c95c77u },
    { 22050, 4, 6, 0xd0ff66bcu },
    { 22050, 4, 16, 0xf1d8d17fu },
    { 22050, 4, 4000, 0xa01ba5b2u },
    { 22050, 4, 4000000, 0xf2eba5eeu },
    { 32000, 0, 3, 0xc6a68f68u },
    { 32000, 0, 4, 0x4843e260u },
    { 32000, 0, 6, 0xe207bc9au },
    { 32000, 0, 16, 0x10653ec2u },
    { 32000, 0, 4000, 0x0593134eu },
    { 32000, 0, 4000000, 0xf57da06bu },
    { 32000, 1, 3, 0xb6f9e2bdu },
    { 32000, 1, 4, 0x4bae0a7du },
    { 32000, 1, 6, 0x44e7079au },
    { 32000, 1, 16, 0xc0d14205u },
    { 32000, 1, 4000, 0xe50d82bcu },
    { 32000, 1, 4000000, 0x2d77bf87u },
    { 32000, 2, 3, 0xe09f1dd1u },
    { 32000, 2, 4, 0x6569bfc6u },
    { 32000, 2, 6, 0x82e25f2fu },
    { 32000, 2, 16, 0xf75d377fu },
    { 32000, 2, 4000, 0x28c5fa36u },
    { 32000, 2, 4000000, 0x976cd8ffu },
    { 32000, 3, 3, 0x6c90ec75u },
    { 32000, 3, 4, 0x4c51512cu },
    { 32000, 3, 6, 0x84466301u },
    { 32000, 3, 16, 0x232df907u },
    { 32000, 3, 4000, 0x36a4e2a6u },
    { 32000, 3, 4000000, 0xd7ce24e2u },
    { 32000, 4, 3, 0x7d835c2du },
    { 32000, 4, 4, 0x9e2a08eau },
    { 32000, 4, 6, 0x051615bbu },
    { 32000, 4, 16, 0xbc283eb0u },
    { 32000, 4, 4000, 0xbfe814b9u },
    { 32000, 4, 4000000, 0x0c3088c1u },
    { 44100, 0, 3, 0x53c5cd85u },
    { 44100, 0, 4, 0x19fe3601u },
    { 44100, 0, 6, 0x25b005b9u },
    { 44100, 0, 16, 0xa90a9a1eu },
    { 44100, 0, 4000, 0xaee492fau },
    { 44100, 0, 4000000, 0x40eadbeau },
    { 44100, 1, 3, 0x5f3c89adu },
    { 44100, 1, 4, 0x90af1b4du },
    { 44100, 1, 6, 0x16b5623bu },
    { 44100, 1, 16, 0x577237e8u },
    { 44100, 1, 4000, 0x8c1fb977u },
    { 44100, 1, 4000000, 0xa073d1f0u },
    { 44100, 2, 3, 0x78fc6934u },
    { 44100, 2, 4, 0xec224a85u },
    { 44100, 2, 6, 0xd95860c1u },
    { 44100, 2, 16, 0xbf6008f9u },
    { 44100, 2, 4000, 0xb16229f5u },
    { 44100, 2, 4000000, 0x8c2dd3deu },
    { 44100, 3, 3, 0xa5fd3bb9u },
    { 44100, 3, 4, 0xdcba3c25u },
    { 44100, 3, 6, 0x1d788f32u },
    { 44100, 3, 16, 0x1cdac04bu },
    { 44100, 3, 4000, 0xf0e52820u },
    { 44100, 3, 4000000, 0x369e06a6u },
    { 44100, 4, 3, 0x8549d49bu },
    { 44100, 4, 4, 0x26e5d11eu },
    { 44100, 4, 6, 0x1b1a08d4u },
    { 44100, 4, 16, 0x8147532bu },
    { 44100, 4, 4000, 0x7bb30e15u },
    { 44100, 4, 4000000, 0x7a258ca7u },
    { 48000, 0, 3, 0xe6eddb68u },
    { 48000, 0, 4, 0xd179f8d0u },
    { 48000, 0, 6, 0x9c8ccbbbu },
    { 48000, 0, 16, 0x9333116eu },
    { 48000, 0, 4000, 0x3f045376u },
    { 48000, 0, 4000000, 0x6018226au },
    { 48000, 1, 3, 0x390a333du },
    { 48000, 1, 4, 0x48f65c6du },
    { 48000, 1, 6, 0xa1c76d1du },
    { 48000, 1, 16, 0x5b1a3130u },
    { 48000, 1, 4000, 0x2660e410u },
    { 48000, 1, 4000000, 0x6d4ba232u },
    { 48000, 2, 3, 0x59ab436au },
    { 48000, 2, 4, 0x4d171ad2u },
    { 48000, 2, 6, 0x111eed8bu },
    { 48000, 2, 16, 0x921f00bfu },
    { 48000, 2, 4000, 0x5846be5eu },
    { 48000, 2, 4000000, 0x6586c0c3u },
    { 48000, 3, 3, 0x707d5298u },
    { 48000, 3, 4, 0xa8bde154u },
    { 48000, 3, 6, 0x4ff4db71u },
    { 48000, 3, 16, 0x638d4d38u },
    { 48000, 3, 4000, 0x400258cbu },
    { 48000, 3, 4000000, 0x60bbc5dfu },
    { 48000, 4, 3, 0xe891e468u },
    { 48000, 4, 4, 0x2c12d794u },
    { 48000, 4, 6, 0x997dcc94u },
    { 48000, 4, 16, 0x0ce4e4a5u },
    { 48000, 4, 4000, 0xde9963b7u },
    { 48000, 4, 4000000, 0xc8a2bff5u },
    { 88200, 0, 3, 0xa4f7247eu },
    { 88200, 0, 4, 0xe4874944u },
    { 88200, 0, 6, 0xf329d35du },
    { 88200, 0, 16, 0xa6e996c4u },
    { 88200, 0, 4000, 0x8b45ea66u },
    { 88200, 0, 4000000, 0x0ba7b4a2u },
    { 88200, 1, 3, 0x55569d4du },
    { 88200, 1, 4, 0x0f3b783du },
    { 88200, 1, 6, 0x31e58087u },
    { 88200, 1, 16, 0x9d591b22u },
    { 88200, 1, 4000, 0xd892e5dau },
    { 88200, 1, 4000000, 0x2f8f9c41u },
    { 88200, 2, 3, 0x40e05265u },
    { 88200, 2, 4, 0xe1e22ff7u },
    { 88200, 2, 6, 0xf62d2d61u },
    { 88200, 2, 16, 0x6b6b54f3u },
    { 88200, 2, 4000, 0x1fb115a4u },
    { 88200, 2, 4000000, 0xead63f6du },
    { 88200, 3, 3, 0xd120725du },
    { 88200, 3, 4, 0xb52bcedfu },
    { 88200, 3, 6, 0x6dac0a0cu },
    { 88200, 3, 16, 0x94ec7934u },
    { 88200, 3, 4000, 0x8ff62fa9u },
    { 88200, 3, 4000000, 0x09eb38b8u },
    { 88200, 4, 3, 0x0210798cu },
    { 88200, 4, 4, 0xc05abfa0u },
    { 88200, 4, 6, 0x15058269u },
    { 88200, 4, 16, 0x24d73e2eu },
    { 88200, 4, 4000, 0x913ab521u },
    { 88200, 4, 4000000, 0xba136d64u },
    { 96000, 0, 3, 0xb7f3e625u },
    { 96000, 0, 4, 0x319281d5u },
    { 96000, 0, 6, 0x6b3ec48bu },
    { 96000, 0, 16, 0xd4975f64u },
    { 96000, 0, 4000, 0xc523fc97u },
    { 96000, 0, 4000000, 0x12c5927cu },
    { 96000, 1, 3, 0x11678fcdu },
    { 96000, 1, 4, 0x845fe72du },
    { 96000, 1, 6, 0xcfb211c8u },
    { 96000, 1, 16, 0xba2fa534u },
    { 96000, 1, 4000, 0x8536d718u },
    { 96000, 1, 4000000, 0xb48407c9u },
    { 96000, 2, 3, 0x4b80792bu },
    { 96000, 2, 4, 0xb0dcc1b6u },
    { 96000, 2, 6, 0x7dfacb76u },
    { 96000, 2, 16, 0x5aa26534u },
    { 96000, 2, 4000, 0x6a574f1fu },
    { 96000, 2, 4000000, 0x327228dfu },
    { 96000, 3, 3, 0xd28daab0u },
    { 96000, 3, 4, 0xdeaf8ebfu },
    { 96000, 3, 6, 0x3b2d2c63u },
    { 96000, 3, 16, 0x6c001c8eu },
    { 96000, 3, 4000, 0x736fd0e2u },
    { 96000, 3, 4000000, 0xdd41400du },
    { 96000, 4, 3, 0x2884600bu },
    { 96000, 4, 4, 0x670d6232u },
    { 96000, 4, 6, 0x09aa6880u },
    { 96000, 4, 16, 0x7aa9b59bu },
    { 96000, 4, 4000, 0xb1437a45u },
    { 96000, 4, 4000000, 0xc4df68f1u },
    { 192000, 0, 3, 0x4093da2du },
    { 192000, 0, 4, 0x95a20608u },
    { 192000, 0, 6, 0x08855dd3u },
    { 192000, 0, 16, 0x7ad5efecu },
    { 192000, 0, 4000, 0x644e0e99u },
    { 192000, 0, 4000000, 0xc953e7ddu },
    { 192000, 1, 3, 0x39f8d948u },
    { 192000, 1, 4, 0x82e21e10u },
    { 192000, 1, 6, 0x663b9005u },
    { 192000, 1, 16, 0x32d1d543u },
    { 192000, 1, 4000, 0xecccd94du },
    { 192000, 1, 4000000, 0x30cce592u },
    { 192000, 2, 3, 0x4c9191edu },
    { 192000, 2, 4, 0x09cbeeecu },
    { 192000, 2, 6, 0xa56d3afeu },
    { 192000, 2, 16, 0x703834e3u },
    { 192000, 2, 4000, 0xf75902abu },
    { 192000, 2, 4000000, 0x72b7b323u },
    { 192000, 3, 3, 0x952b69efu },
    { 192000, 3, 4, 0x66cf3c59u },
    { 192000, 3, 6, 0xbea98daeu },
    { 192000, 3, 16, 0xba8713ecu },
    { 192000, 3, 4000, 0xa95a5811u },
    { 192000, 3, 4000000, 0x80c315ceu },
    { 192000, 4, 3, 0xfd2d208cu },
    { 192000, 4, 4, 0xdaaef3d7u },
    { 192000, 4, 6, 0xae8c4f52u },
    { 192000, 4, 16, 0xaff1b714u },
    { 192000, 4, 4000, 0xa7cdb4fau },
    { 192000, 4, 4000000, 0x2ff1022eu },
};

static uint32_t run(uint32_t rate, int pattern, int level, uint32_t seed)
{
    static const float mul[8] = { 1, 1, 0, 2, 1, 0.25f, 4, 1 };
    const uint32_t sizes[12] = { 128, 128, 128, 96, 1, 480, rate * 3 / 1000, rate * 3 / 1000 + 1, 64, 1024, 2000, 7 };
    rl_limiter l;
    rl_limiter_init(&l, rate, 1, 0);
    uint32_t s = seed, h = 0x811c9dc5u, t = 0;
    static float x[2000];
    for (int b = 0; b < 200; b++) {
        const uint32_t n = sizes[b % 12];
        const float lv = (float)(level * mul[(b / 3) % 8] / 4.0);
        uint32_t kind = 0, pos = 0;
        if (pattern == 4) { s = lcg(s); kind = (s >> 12) % 6; pos = (s >> 16) % n; }
        for (uint32_t i = 0; i < n; i++) {
            s = lcg(s);
            float v;
            if (pattern == 4)       /* a block of one kind: a click in silence, a steady level, silence, noise, both ends, a ramp */
                v = kind == 0 ? (i == pos ? 1.0f : 0.0f) : kind == 1 ? 1.0f : kind == 2 ? 0.0f
                    : kind == 3 ? (float)(((int)((s >> 8) & 0xffff) - 32768) / 32768.0) : kind == 4 ? (i == 0 || i == n - 1 ? 1.0f : 0.0f)
                    : (float)((double)i / (double)n);
            else if (pattern == 0) v = (float)(((int)((s >> 8) & 0xffff) - 32768) / 32768.0);
            else if (pattern == 1) v = ((t + i) / 37) % 2 ? 1.0f : -1.0f;
            else if (pattern == 2) v = (s >> 28) == 0 ? 1.0f : 0.0f;
            else v = (s >> 22) == 0 ? ((s >> 8) & 1 ? 1.0f : -0.75f) : 0.0f;
            x[i] = (float)((double)v * (double)lv);
        }
        t += n;
        rl_limiter_chan_process(&l, &l.ch[0], x, n, 1);     /* (a block as the DLL was given it) */
        for (uint32_t i = 0; i < n; i++) { uint32_t w; memcpy(&w, &x[i], 4); h = (h ^ w) * 0x01000193u; }
        uint32_t w; memcpy(&w, &l.ch[0].gain, 4); h = (h ^ w) * 0x01000193u;
    }
    return h;
}

int main(void)
{
    int same = 0;
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0
    printf("     (an x87 build: it rounds otherwise than the DLL's limiter does, the hashes are not compared)\n");
    if (0)
#endif
    for (unsigned k = 0; k < sizeof(ref) / sizeof(*ref); k++) {
        const uint32_t h = run(ref[k].rate, ref[k].pattern, ref[k].level, ref[k].rate + (uint32_t)ref[k].pattern);
        same += h == ref[k].hash;
        CHECK(h == ref[k].hash, "%u Hz, pattern %d, level %d/4: %08x, the DLL's %08x", ref[k].rate, ref[k].pattern,
              ref[k].level, h, ref[k].hash);
    }
    printf("     the DLL's output, bit for bit: %d of %d runs\n", same, (int)(sizeof(ref) / sizeof(*ref)));

    for (int fixed = 0; fixed < 2; fixed++) {
        /* a quiet mix: the same samples, 3 ms later, whatever the size of the calls */
        {
            rl_limiter l;
            rl_limiter_init(&l, 48000, 2, fixed);
            static float in[2 * 20000], out[2 * 20000];
            uint32_t s = 7;
            for (int i = 0; i < 2 * 20000; i++) { s = lcg(s); in[i] = (float)(((int)((s >> 8) & 0xffff) - 32768) / 32768.0) * 0.99f; }
            memcpy(out, in, sizeof(in));
            uint32_t done = 0, k = 0;
            static const uint32_t calls[5] = { 480, 441, 1, 1000, 77 };
            while (done < 20000) { uint32_t n = calls[k++ % 5]; if (n > 20000 - done) n = 20000 - done; rl_limiter_process(&l, out + 2 * done, n); done += n; }
            int ok = 1;
            for (int i = 0; i < 20000 && ok; i++)
                for (int c = 0; c < 2; c++) ok = out[2 * i + c] == (i < 144 ? 0.0f : in[2 * (i - 144) + c]);
            CHECK(ok && l.delay == 144 && l.block == 128 && !l.limited, "%s, a mix below full scale: delayed by %u frames, unchanged %d, limited %u",
                  fixed ? "fixed" : "ALchemy's", l.delay, ok, l.limited);
        }
        /* a loud mix (a 300 Hz tone and noise in bursts of up to 4 times full scale, the right channel at a third
         * of the left): how far over full scale it comes out, and what becomes of left against right.  ALchemy's
         * goes down by 1/(5 ms) a sample at most with 3 ms of warning and holds each channel down on its own; fixed,
         * nothing is above full scale and the two keep their proportion */
        {
            rl_limiter l;
            rl_limiter_init(&l, 48000, 2, fixed);
            static float x[2 * 96000], in[2 * 96000];
            uint32_t s = 3;
            double worst = 0, over = 0, ratio_lo = 1e9, ratio_hi = 0;
            for (int i = 0; i < 96000; i++) {
                s = lcg(s);
                const double env = (i / 6000) % 4 == 3 ? 0.2 : 1.0 + (i / 6000) % 4;
                in[2 * i] = (float)(env * (0.7 * sin(2 * M_PI * 300 * i / 48000.0) + 0.3 * (((int)((s >> 8) & 0xffff) - 32768) / 32768.0)));
                in[2 * i + 1] = in[2 * i] / 3;
            }
            memcpy(x, in, sizeof(in));
            for (int i = 0; i < 96000; i += 480) rl_limiter_process(&l, x + 2 * i, 480);
            for (int i = 144; i < 96000; i++) {
                if (fabs(x[2 * i]) > worst) worst = fabs(x[2 * i]);
                over += fabs(x[2 * i]) > 1.0f;
                if (fabs(in[2 * (i - 144)]) > 0.05) {       /* (the gain on the left over the gain on the right) */
                    const double q = (x[2 * i] / in[2 * (i - 144)]) / (x[2 * i + 1] / in[2 * (i - 144) + 1]);
                    if (q < ratio_lo) ratio_lo = q;
                    if (q > ratio_hi) ratio_hi = q;
                }
            }
            printf("     %-9s a mix up to 4 times full scale: peak out %.3f, %.2f%% of its samples above full scale;\n"
                   "               left against right at its lowest %.2f (limited %u frames, lowest gain %.3f)\n",
                   fixed ? "fixed:" : "ALchemy:", worst, over / 960.0, ratio_lo, l.limited, l.lowest);
            if (fixed) CHECK(worst <= 1.0 && over == 0 && ratio_lo > 0.999 && ratio_hi < 1.001 && l.lowest < 0.35f,
                             "fixed, loud mix: peak %.4f, left against right %.3f to %.3f", worst, ratio_lo, ratio_hi);
            else CHECK(worst > 1.2 && over / 96000 < 0.01 && ratio_lo < 0.5, "ALchemy's, loud mix: peak %.3f, %.2f%% over, ratio %.2f",
                       worst, over / 960.0, ratio_lo);
        }
        /* fixed: the gain never moves by more than a 3 ms line down or a 50 ms line up, a sample */
        if (fixed) {
            rl_limiter l;
            rl_limiter_init(&l, 48000, 1, 1);
            float prev = 1.0f, down = 0, up = 0;
            uint32_t s = 11;
            for (int i = 0; i < 48000; i++) {
                s = lcg(s);
                float x = (i / 3000) % 3 == 1 ? (float)(((int)((s >> 8) & 0xffff) - 32768) / 32768.0) * 6.0f : 0.1f;
                rl_limiter_process(&l, &x, 1);
                if (prev - l.gain > down) down = prev - l.gain;
                if (l.gain - prev > up) up = l.gain - prev;
                prev = l.gain;
            }
            CHECK(down <= 1.0f / 144 + 1e-6f && up <= l.up + 1e-7f && down > 0.004f, "fixed: the gain moved by %.5f down and %.6f up a sample at most",
                  down, up);
        }
    }
    /* fixed: an infinity or a NaN in the mix is left out, and what is around it comes through as it was */
    {
        rl_limiter l;
        rl_limiter_init(&l, 48000, 2, 1);
        static float x[2 * 1000];
        for (int i = 0; i < 2000; i++) x[i] = 0.5f;
        x[2 * 300] = INFINITY; x[2 * 301 + 1] = NAN; x[2 * 302] = -INFINITY;
        rl_limiter_process(&l, x, 1000);
        int ok = 1;
        for (int i = 0; i < 2000 && ok; i++) {
            const int f = i / 2 - 144, bad = i == 2 * (300 + 144) || i == 2 * (301 + 144) + 1 || i == 2 * (302 + 144);
            ok = x[i] == (f < 0 || bad ? 0.0f : 0.5f);
        }
        CHECK(ok && l.gain == 1.0f && !l.limited, "fixed: an infinity and a NaN left out (gain %g, limited %u)", l.gain, l.limited);
    }
    /* fixed: the loudest channel counts, whichever it is; nothing above full scale on any; and when the mix is quiet
     * again nothing is left in sight */
    {
        rl_limiter l;
        rl_limiter_init(&l, 44100, 6, 1);
        static float x[6 * 44100];
        uint32_t s = 5;
        for (int i = 0; i < 44100; i++)
            for (int c = 0; c < 6; c++) {
                s = lcg(s);
                const float v = (float)(((int)((s >> 8) & 0xffff) - 32768) / 32768.0);
                x[6 * i + c] = i >= 30000 ? 0.2f * v : c == (i / 3000) % 6 ? 5.0f * v : 0.3f * v;
            }
        for (int i = 0; i < 44100; i += 441) rl_limiter_process(&l, x + 6 * i, 441);
        float worst = 0;
        for (int i = 0; i < 6 * 44100; i++) if (fabsf(x[i]) > worst) worst = fabsf(x[i]);
        CHECK(worst > 0.9f && worst <= 1.0f && l.needed == 0 && l.gain == 1.0f && l.lowest < 0.25f,
              "fixed, 5.1 with the loud channel moving: peak %.4f, %u frames still in sight, gain %g", worst, l.needed, l.gain);
    }
    /* fixed: peaks far beyond anything a mix holds, up to where a float gives out: still nothing above full scale */
    {
        float worst = 0, plain = 0;
        static const float big[] = { 100.0f, 2e5f, 937776.0f, 2.96e6f, 1.7e7f, 2.9e7f, 3.2e7f, 1e9f, 3e38f };
        for (unsigned b = 0; b < sizeof(big) / sizeof(*big); b++) {
            rl_limiter l;
            rl_limiter_init(&l, 48000, 1, 1);
            for (int i = 0; i < 4000; i++) {
                float x = i < 2000 ? (i & 1 ? big[b] : -big[b]) : i == 3000 ? big[b] : 0.1f;
                rl_limiter_process(&l, &x, 1);
                if (!(fabsf(x) <= worst)) worst = fabsf(x);
                if (b == 0 && fabsf(x) > plain) plain = fabsf(x);
            }
        }
        CHECK(worst <= 1.0f && plain > 0.979f && plain < 0.981f, "fixed: peaks of up to 3e38 come out at %g, one of 100 at %g", worst, plain);
    }
    /* fixed, the channels folded to others after it: it is those that stay within full scale (two channels at 0.8,
     * both going to one output), and channels that are loud but fold to less are left alone */
    {
        static const float m[2][3] = { { 1.0f, 0.0f, 0.25f }, { 1.0f, 0.0f, 0.25f } }, quiet[2][3] = { { 0.3f, 0, 0 }, { 0, 0.3f, 0 } };
        for (int pass = 0; pass < 2; pass++) {
            rl_limiter l;
            rl_limiter_init(&l, 48000, 2, 1);
            rl_limiter_fold(&l, pass ? &quiet[0][0] : &m[0][0], 3, 3);
            float worst = 0;
            for (int i = 0; i < 9600; i++) {
                float x[2] = { (pass ? 3.0f : 0.8f) * (float)sin(i * 0.05), (pass ? 3.0f : 0.8f) * (float)sin(i * 0.05) };
                rl_limiter_process(&l, x, 1);
                const float out = pass ? fabsf(x[0]) * 0.3f : fabsf(x[0] + x[1]);
                if (out > worst) worst = out;
            }
            if (pass) CHECK(worst > 0.89f && !l.limited, "fixed, folded to less: %.3f out, limited %u frames", worst, l.limited);
            else CHECK(worst > 0.9f && worst <= 1.0f && l.limited, "fixed, two channels folded to one output: %.4f out", worst);
        }
    }
    /* (the matrix as the device has it: rows of 8, two outputs, each channel to its own) */
    {
        float m[2][8] = { { 0.5f, 0.0f, 9, 9, 9, 9, 9, 9 }, { 0.0f, 2.0f / 3, 9, 9, 9, 9, 9, 9 } }, worst[2] = { 0, 0 };
        rl_limiter l;
        rl_limiter_init(&l, 48000, 2, 1);
        rl_limiter_fold(&l, &m[0][0], 8, 2);
        for (int i = 0; i < 19200; i++) {
            float x[2] = { i < 9600 ? 1.9f * (float)sin(i * 0.05) : 0.2f, i < 9600 ? 0.2f : 2.5f * (float)sin(i * 0.05) };
            rl_limiter_process(&l, x, 1);
            const int h = i >= 9600 + 144;
            if (i < 9600 || h) for (int c = 0; c < 2; c++) {
                const float out = fabsf(x[c]) * m[c][c];
                if (c == h && out > worst[h]) worst[h] = out;
            }
        }
        /* (1.9 on the left is 0.95 out: left alone; 2.5 on the right is 1.67 out: held at 0.98) */
        CHECK(worst[0] > 0.949f && worst[0] < 0.951f && worst[1] > 0.97f && worst[1] <= 0.981f,
              "fixed, a fold in rows of 8: left %.4f out, right %.4f", worst[0], worst[1]);
    }
    /* how long it was at work is counted in frames, however many channels */
    for (int fixed = 0; fixed < 2; fixed++) {
        rl_limiter l;
        rl_limiter_init(&l, 48000, 6, fixed);
        static float x[6 * 4800];
        for (int i = 0; i < 6 * 4800; i++) x[i] = i / 6 < 2400 ? 1.5f : 0.1f;
        for (int i = 0; i < 4800; i += 480) rl_limiter_process(&l, x + 6 * i, 480);
        CHECK(l.limited > 2400 && l.limited <= 4800, "%s: limited %u of 4800 frames", fixed ? "fixed" : "ALchemy's", l.limited);
    }
    if (fails) { printf("test_limiter: %d FAILED\n", fails); return 1; }
    printf("test_limiter: all passed\n");
    return 0;
}
