#!/bin/sh
# gen.sh <XP dsdmo.dll>: regenerate src/i3dl2.inc, the I3DL2 reverb of Windows XP SP3's dsdmo.dll (5.3.2600.5512)
# translated to C by asm2c.py.
#   kernels: 5c020 (line positions), 59e90 (float -> int16), the six process kernels (8/16-bit, float; mono, stereo)
#   control: SetParam 65e90 and its coefficient helpers, Discontinuity 66860 with the line allocators, kernel
#            choice 66680, IDirectSoundFXI3DL2Reverb's methods, IMediaParams::SetParam 66630
#   data:    the IMediaParams and IDirectSoundFXI3DL2Reverb vtables, the presets, the parameter table, and the
#            constants 65880 indexes
cd "$(dirname "$0")/../.." || exit 1
exec python3 -I tools/i3dl2/asm2c.py "$1" src/i3dl2.inc \
    6ca5c020 6ca59e90 6ca5c280 6ca5e030 6ca60d10 6ca61c10 6ca63aa0 6ca5fdd0 \
    6ca65e90 6ca5a1a0 6ca5a2a0 6ca5a4c0 6ca5a520 6ca5a5d0 6ca65880 \
    6ca66860 6ca5acc0 6ca5ad70 6ca5ae20 6ca5aed0 6ca5af80 6ca5b030 6ca5b0e0 6ca5b190 6ca5b240 6ca5b2f0 6ca5b3a0 \
    6ca5b450 6ca5b500 6ca5b5a0 6ca5b650 6ca5b700 6ca5b790 6ca66680 \
    6ca5a780 6ca5a890 6ca5aa10 6ca5aa50 6ca5aa90 6ca5aae0 6ca66630 \
    @6ca43204:20 @6ca431ac:28 @6ca42b88:5a0 @6ca6a1e0:7ec @6ca42b48:20
