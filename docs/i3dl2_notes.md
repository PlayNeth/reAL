# XP dsdmo.dll I3DL2 reverb: porting notes

Source: Windows XP SP3 dsdmo.dll 5.3.2600.5512, image base 0x6ca40000. x87 code (53-bit precision control under
Windows/Wine: compute in double, store to float where the code does `fst/fstp DWORD`). `_ftol` (msvcrt) truncates.
Disassembly: `/tmp/real/sp/dis/dsdmoxp.asm`; the reference copy is in `$HOME/re/dmxp` (`DMDIR` for
tools/dsfx/cmp.sh; dmgt dmo i3dl2, DMO_I3DL2_QUALITY / DMO_I3DL2_PRESET).

Windows 11's dsdmo has no I3DL2. Its other seven effects differ from XP's: rounding only for Echo, Chorus, Flanger,
Compressor; Waves is one frame earlier in XP; Distortion about 2 dB quieter in XP. reAL keeps the Windows 11 ports
of those (the user's call: no audible gain from XP's).

## Object (0x4c8 bytes, constructor 6ca5b840, factory 6ca56e20 -> 6ca66730)
- +0x130 rate (constructor 22050); quality at +0x228 (default 2); `rate_eff = rate / (quality & 2 ? 1 : 2)`.
- +0x1d4 IDirectSoundFXI3DL2Reverb (vtable 6ca431ac): SetAll 6ca5a780, GetAll 6ca5a890, SetPreset 6ca5aa10,
  GetPreset 6ca5aa50, SetQuality 6ca5aa90, GetQuality 6ca5aae0.
- +0x3d4 preset (-1 = none; any parameter set resets it).
- Delay lines (12 bytes each: buf, pos, len) at +0x3d8, +0x3e4 ... (14), allocated by Init 6ca66860
  through 6ca5acc0, 6ca5ad70, ... (one allocator per line).
- Parameter table 6ca6a1e0 (13 entries, stride as Win11's), IMediaParams at +0x180.

## SetParam 6ca65e90 (index, value, internal); 0x80040205 if rate_eff == 0
| idx | field | range | stored | then |
|---|---|---|---|---|
| 0 | lRoom | -10000..0 | +0x1f8 | 5a520, 5a4c0 |
| 1 | lRoomHF | -10000..0 | +0x1fc | 5a1a0 |
| 2 | flRoomRolloffFactor | 0..10 | +0x200 | 5a1a0 |
| 3 | flDecayTime | 0.1..20 | +0x204 = v*1000 | 65880 |
| 4 | flDecayHFRatio | 0.1..2 | +0x208 = v*10 | 65880 |
| 5 | lReflections | -10000..1000 | +0x20c | 5a4c0 |
| 6 | flReflectionsDelay | 0..0.3 | +0x210 = v*1000 | 5a2a0 |
| 7 | lReverb | -10000..2000 | +0x214 | 5a520 |
| 8 | flReverbDelay | 0..0.1 | +0x218 = v*1000, at least 5 | 5a2a0 |
| 9 | flDiffusion | 0..100 | +0x21c = +0x238 = v*0.0061803399 | 65880 |
| 10 | flDensity | 0..100 | +0x220 = min((v*0.01+0.1)*0.9091, 1) | tap lengths, 65880 |
| 11 | flHFReference | 20..20000 | +0x224 | 5a1a0, 65880 |
| 12 | quality | 0..3 | +0x228 = ftol(v) | |

Density's tap lengths: d*67 times 0.93, 0.8649, 0.804357, 0.748052, 0.695688 cumulatively (each product kept
in the register, its float stored); taps = ftol(rate_eff * product * 0.001) shortest first at +0x338..+0x34c;
the same from d*75 at +0x354..+0x368; copied to +0x244..+0x270; +0x350 = rate_eff*10/1000.

## The port (src/i3dl2.c, src/i3dl2.inc)
Everything that computes is translated: SetParam and its coefficient helpers, Discontinuity 66860 with the twenty
line allocators, the kernel choice 66680, the six kernels with 5c020 and 59e90, IDirectSoundFXI3DL2Reverb's methods
and IMediaParams::SetParam 66630. `asm2c.py` refuses a function that calls one it was not given, and code that
indexes the DLL's data that is not copied (`@va:len`); give whole tables (an index can run one past the first
loop's range, as 65880's does into 0x6ca42b58). By hand: the constructor's stores, _ftol, _CIpow, operator
new/delete (an arena heap), CParams' value store / GetParam, Init and the base class's Process.

Things found on the way, all reproduced:
- The quality kept in the object (+0x228) is only written by SetParam(12); the constructor leaves it 0 (operator
  new on a fresh heap), so until SetQuality the reverb runs at half rate while GetQuality says 2
  (DSFX_FIX_I3DL2_QUALITY sets it to 2).
- Process (6ca44e70) runs FBRProcess on 3 ms blocks (rate*3/1000, at least 16 frames) while more than a block is
  left, then the rest. At half rate a block of odd length leaves a sample owed to the next block; in 8-bit that
  sample is written from the converted value's low byte (a click every few blocks). With DirectSound the blocks
  follow the mixer's Process calls, so XP's 8-bit half-rate output depends on timing.
- 65880 reads stack slots it did not write in some quality paths and stores them in the object (0x2bc..0x310);
  the kernels of those paths do not use them.
