/* test_doppler [rate] - reAL's Doppler for DirectMusic's sink buffers on the cases in doppler_cases.h, printed as
 * dopptest prints Microsoft's (the frequency dsound computes and the synth pitch offset). */
#include <stdio.h>
#include <stdlib.h>
#include "../src/spatial.h"
#include "doppler_cases.h"

int main(int argc, char **argv)
{
    unsigned rate = argc > 1 ? (unsigned)atoi(argv[1]) : 22050;
    for (unsigned i = 0; i < DOP_NCASES; i++) {
        const dop_case *c = &dop_cases[i];
        rl_listener3d l; rl_source3d s;
        rl_listener_defaults(&l); rl_source_defaults(&s);
        l.pos = (rl_vec3){ c->lpos[0], c->lpos[1], c->lpos[2] };
        l.vel = (rl_vec3){ c->lvel[0], c->lvel[1], c->lvel[2] };
        l.doppler_factor = c->ldoppler; l.distance_factor = c->ldist;
        s.pos = (rl_vec3){ c->bpos[0], c->bpos[1], c->bpos[2] };
        s.vel = (rl_vec3){ c->bvel[0], c->bvel[1], c->bvel[2] };
        s.mode = c->mode;
        unsigned f = c->freq ? c->freq : rate;
        if (s.mode != RL_DS3DMODE_DISABLE) f = rl_spatial_doppler_ds(&l, &s, f);
        printf("case %u freq %u cents %d\n", i, f, rl_sink_cents(f, rate));
    }
    return 0;
}
