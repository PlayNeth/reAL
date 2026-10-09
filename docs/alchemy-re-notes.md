# Creative ALchemy `dsound.dll` — RE notes

Binary: `dsound.dll`, PE32 i386, 1,253,376 bytes, image base `0x10000000`,
link timestamp `0x535E0D66` (2014-04-28). PDB path: `f:\codes\ALchemy\DSoundDLL\trunk\release\dsound.pdb`.
MSVC, full RTTI present (class names below are real, recovered from RTTI).

Companion files: `ALchemy.exe` (MFC GUI, installs DLL + ini into game dirs), `ALchemy.ini`
(title DB: RegPath / SubDir / Buffers / Duration / ...), `ALchemy.crl` (pt-BR resource DLL),
`HookWnd.dll` (GUI skinning), `CTSU*` (Creative Software AutoUpdate). None of these are needed at
runtime: **the game only ever loads `dsound.dll` + `dsound.ini`.**

## Exports

| Export | Ord | Behaviour |
|---|---|---|
| DirectSoundCreate / DirectSoundCreate8 | 1 / 11 | ALchemy implementation, falls back to real dsound on init failure |
| DirectSoundEnumerateA/W | 2 / 3 | ALchemy (falls back) |
| DllGetClassObject / DllCanUnloadNow | 5 / 4 | ALchemy class factory (`CFactory`), CLSID_DirectSound / CLSID_DirectSound8 |
| DirectSoundCapture* , DirectSoundFullDuplexCreate, GetDeviceID | 6-10,12 | **passed straight to the real dsound.dll** |

Real dsound is loaded from `GetSystemDirectoryA() + "\DSound.dll"` (fn `0x10019190`).
⚠ Under Wine, if this DLL itself lives in system32/syswow64, that path resolves to *itself* →
capture and fallback break. reAL must not copy this design.

Imports are tiny (no OpenAL, no registry, no WASAPI in IAT): OpenAL (`openal32.dll` / `ct_oal.dll`)
and `Mmdevapi.dll` are `LoadLibrary`'d at runtime. `iphlpapi!GetAdaptersInfo`,
`GetVolumeInformationA`, `CMD5`, `\Creative\SoftwareLock\*.kga` = Creative licensing. Not relevant to
a reimplementation; not analysed further.

## Config: `dsound.ini`  (DllMain → `0x10012c40`)

Path = this DLL's full path with the last 3 chars replaced by `ini` (i.e. `dsound.ini` beside the DLL),
fallback `dsound.ini` in CWD. Hand-rolled parser: `fgets`, skip lines starting `;`, split on `=`,
key lower-cased, exact compare. **No sections.** Booleans must be the literal `true`.

| key | global | default | clamp | notes |
|---|---|---|---|---|
| buffers | `0x100cc690` | 4 | ≥2 | AL path: # of queued AL buffers per streaming source |
| duration | `0x100cc694` | 25 | ≥5 | ms per queued buffer (so default latency 4×25 = 100 ms) |
| maxvoicecount | `0x100cc69c` | 128 | ≥16 | |
| logdirectsound | `0x100d350d` | false | | |
| logdirectsound2d | `0x100d350e` | false | | |
| logdirectsound2dstreaming | `0x100d350f` | false | | |
| logdirectsound3d | `0x100d3514` | false | | |
| logdirectsoundlistener | `0x100d3515` | false | | |
| logdirectsoundeax | `0x100d3516` | false | | |
| logdirectsoundtiminginfo | `0x100d3517` | false | | profiling table |
| logstarvation | `0x100d3518` | false | | |
| disabledirectmusic | `0x100d3519` | false | | refuses the DMusic private QI |
| disablenativeal | `0x100d351a` | false | | **forces software engine** even on Creative HW |

Log file: `dsoundlog.txt` beside the DLL (only created if any `log*` flag is set).
Note the ALchemy.ini `Preset=` key is GUI-only and never reaches the DLL.

## Backend selection (`DirectSoundCreate` core, `~0x10013550`)

```
if (!disablenativeal && IsNativeCreativeAL())       // 0x10018150
        obj = new CALDirectSound(...)                // 0x1000f9d0  → OpenAL / ct_oal.dll
else    obj = CreateSensDirectSound(...)             // 0x1001a2d0  → software engine, WASAPI
```
`IsNativeCreativeAL()`: `%SYSTEM%\ct_oal.dll` must load, and the ALC device name *and* the
DirectSound device description must both contain `X-Fi`, `Audigy` or `SE-300PCIE`.

**On any non-Creative card (and on Wine) ALchemy runs the Sens software engine.** That's the sound
you're comparing DSOAL against — Creative's own DSP, not OpenAL. `CreateSensDirectSound` constructs
`CSensDirectSound` with feature flags `0x81` (licensed reverb feature `CTLA20RVB5`) or `0x41`.

## Class map (from RTTI)

### COM front end — software path
| class | vtbl | entries | role |
|---|---|---|---|
| CSensDirectSound | 100b3008 (+12) / 100b2ff4 | 12 / 4 | IDirectSound8 + non-delegating IUnknown |
| CSensDirectSoundBuffer | 100b3a74 | 24 | IDirectSoundBuffer8 (secondary + primary) |
| CSensDirectSound3DBuffer | 100b39ec | 21 | IDirectSound3DBuffer |
| CSensDirectSoundListener | 100b3c14 | 18 | IDirectSound3DListener |
| CSensDirectSoundNotify | 100b3a60 | 4 | IDirectSoundNotify |
| CSensPropertySet | 100b3a44 | 6 | IKsPropertySet → EAX 1-5 |
| CSensBufferEAX1..5 | (non-virtual) | | per-buffer EAX version state |
| CSensDirectSoundRender | 100b3c70/80 | | render loop |
| CWASAPIRender | 100b2ed0 | 3 | shared-mode WASAPI output via Mmdevapi.dll |

### COM front end — OpenAL path (Creative HW only)
`CALDirectSound` (100b1be4), `CALDirectSoundBuffer` (100b15fc, 24), `CALDirectSound3DBuffer`,
`CALDirectSoundListener`, `CALDirectSoundNotify`, `CALPropertySet`. Streams DS buffers through
`buffers × duration ms` AL buffer queues (`ServiceStream`, timer via `timeSetEvent`).

### Engine
* Mixing: `CMixer`, `CMixerStream`, `C2DStream`, `C3DStream`, `CMixBin*`, `CMixBusBase/Direct/Effect/Sink`,
  `CMixBusSRC<80>`, `CMixBusSRCSHQFixed<80>` (80-tap SRC), `CSRCState*`, `CSRCHistoryBuffer`.
* Output: `CClipper`, `CLimiter`, `CLimiterChan`, `CBassBoostMixer*`, `CControlSinkStereoUpmix/Duplication`.
* 3D: `CHspHrtfFilterState` (HRTF), `CControlSink3D`, `CListener`, `CListenerManager`.
* CMSS: `CCMSSSurround`, `CCMSSUpmix`, `CCMSS1Upmix`, `CCMSS3Upmix`, `CCMSSRealUpmix`.
* EAX 4/5 slots: `CFxSlot`, `CFxSlotManager`, `CEffectBus(Manager)`, `CFxSlotEffect`.
* Effects (all 12 EAX effects): `CFx{Reverb,Chorus,Distortion,Echo,Flanger,FrequencyShifter,
  VocalMorpher,PitchShifter,RingModulator,Autowah,AGCCompressor,Equaliser}` each wrapping a
  `CPlug*` DSP object.
* Reverb: `CHRvbReverb` (100b4dc0), `CHRvbDspEngine` (100b4dec), `CHRvbEarlyReflections`,
  `CHRvbLateReverb`, `CHRvbParamUpdate`, `CEarlyRefBuffer`, `CEarlyRefCircularBuffer`,
  `CHspDelayBuffer<N>` (N = 0x30A, 0x804, 0xC19, 0x1000, 0x1400, 0x2400, 0x2580).
* DSP primitives: `CStaticHspSystem{,MMX,SSE,SSE2}` — 141-entry vtables of kernels (filters, mix, SRC).
  Read the plain C one (`100b30fc`) first; the SIMD ones compute the same thing.
* Filters: `CHspIIR2State`, `CHspIIR4State`, `CHspShelvingF`, `CHspAllPassF`, `CHspAPFStateF`,
  `CHspHPFStateF`, `CParamEqFilter{Peak,LowFreq,HighFreq}`.

`docs/vtables.json` (generated by `tools/rtti.py`) has every vtable's full method list.

## Suggested RE order
1. `CSensDirectSoundBuffer` — cursor/lock semantics, what "play position" means w.r.t. render latency.
2. `CSensPropertySet::Set` → `CSensBufferEAX*` — how EAX 1-5 props map to internal params
   (logging strings make this easy to follow).
3. `C3DStream` + `CHspHrtfFilterState` — distance model, cone, HRTF.
4. `CHRvbReverb` / `CHRvbDspEngine` — the reverb you actually like. Biggest job.
5. Remaining `CPlug*` effects.

## Observed behaviour (dstrace of NFS: High Stakes on ALchemy, Wine)

* `GetCaps`: flags 0xf5f, hw 3D/mixing voices `maxvoicecount + 1` (129); free count drops by one per
  live non-LOCSOFTWARE secondary buffer (the LOCSOFTWARE music stream does not count); hw memory 0;
  sample rate range 100–192000.
* `IKsPropertySet::QuerySupport(DSPROPSETID_VoiceManager {62a69bae-…})` → `S_OK`, support 0.
* `QuerySupport(EAX1 listener, 0)` → `S_OK`, support 3 (GET|SET). Asked on a 128-byte LOCHARDWARE|CTRL3D
  probe buffer.
* EAX 1.0 preset table (environment, volume, decay, damping) at `.rdata 0x100a7ef0`, 26 entries,
  identical to the public eax.h values; second copy in `.data 0x100cf164`.
* NFS4 only ever sets listener ENVIRONMENT + VOLUME (1.0 in race, 0.0 in menus; envs 0, 7, 15, 16, 19)
  and per-buffer REVERBMIX (0–0.504). Positions are unit vectors; distance is done with SetVolume.

## EAX 1.0 → reverb conversion (fn 0x1004a050, called from CFxReverb update 0x10050e90)

Setters (0x10049dc0 env, 0x10049e70 volume, 0x10049f10 decay, 0x10049fb0 damping) only store and
set dirty bits; environment ≥ 26 is clamped to 25 and volume to 0..1 (no error); NaNs are rejected.
Setting the same environment again keeps tweaked values.

On conversion, the environment indexes an EAX3-style preset table at `.rdata 0x100a8280`
(0x70 bytes/entry: env, size, diffusion, room, roomHF, roomLF, decay, decayHFRatio, decayLFRatio,
reflections, reflectionsDelay, reflectionsPan, reverb, reverbDelay, reverbPan, echoTime, echoDepth,
modulationTime, modulationDepth, airAbsorptionHF, hfRef, lfRef, roomRolloff, flags). Copied: size,
diffusion, roomLF, reflections(+300 bias in the setter call), reflections delay, reverb(+300),
reverb delay, echo, modulation, HF/LF reference (echo depth forced to 0 for env 24).
Then:
* room gain = EAX1 volume (linear); decay = EAX1 decay; LF decay = decay × preset decayLFRatio
* damping d → decay HF ratio t: `t = d` if d ≥ 1, else `t = 1 / (1 − k1[env]·(1 − 1/d))`, clamped
  0.1..2.0 (k1 = `.rdata 0x100a7e20`)
* room HF (mB) = `round(k2[env] · (1/t − 1))`, clamped −10000..0 (k2 = `.rdata 0x100a7e88`)
For generic and concert hall the default EAX1 damping reproduces the EAX3 preset exactly
(0.829/−101 vs 0.83/−100; 0.697/−507 vs 0.70/−500).

## When the reverb exists at all (FX slots, fn 0x10042220)

The Sens engine has 4 FX slots (CFxSlot, ctor 0x1004fe80); each starts with **no effect loaded**
(slot+0x14 = NULL). `0x10042220(mode)` loads effects by EAX version: 0 → all slots emptied,
1–3 → slot 0 = reverb (type 10), 4–5 → slot 0 = reverb + slot 1 = chorus (type 3). It is called
only from the EAX 1–5 property handlers (0x10021687 … 0x1002861f), the first time a context uses
that EAX version (bit in ctx+0x2c), and with mode 0 from 0x10041ce0 when the last context that
used EAX goes away. So **a game that never makes an EAX call gets no reverb** — no default
"generic room" like hardware EAX. Once EAX is used, every buffer sends to it by default
(per-buffer defaults at `.rdata 0x100b4590`: room 0 mB, active FX slots {NULL, Primary}; slot
reverb defaults = generic, room −1000 mB, fn 0x10043f40).

## HRTF (measured with hrtfprobe on Windows, speaker config DSSPEAKER_HEADPHONE)

* Exactly left/right mirror-symmetric; compact responses (99.9 % energy within ≤ 80 samples of
  onset at 48 kHz); interaural onset difference up to 11 samples (0.23 ms).
* Level at 0 dB elevation: front −3.7 dB/ear, back −11.7 dB/ear; front/back difference mostly
  above 2 kHz (+7.5 dB @ 4 kHz, +15–17 dB @ 12–16 kHz).
* Distance: pure inverse distance (−6.0 dB per doubling from min distance), no extra filtering.
* Each 3D response leaves a tiny constant DC offset (≈ −60 dB).
* SetPosition on a stopped buffer is sometimes not applied before Play (8 of 146 positions came
  out identical to the previous one even with a 100 ms lead-in).
* With the EAX listener volume at 0 there is no reverb tail at all.

## CHRvbReverb (ALchemy's software reverb) — DSP

Tooling: Ghidra 11.0.3 headless project in `~/re/ghidra` (`dec.sh addr…` decompiles), Unicorn harness
`~/re/tools/emu.py`. Engine needs **≤128-frame blocks** (internal buffers are 128 frames; 512 blows up),
and the process call's `edx` is the sample rate. The first block after start is faded in (0→1 ramp).

Call chain: CPlugReverb process 0x100537b0 → 0x1005c1b0(W=plug+0xbc) → CHRvbReverb v3 0x10060f10 →
v5 0x1005de10(in, out, hasInput, tailLen, rate). Output channel slots (desc+0xc+4k, k = speaker bit):
FL FR FC — BL BR … SL SR, plus an internal centre copy at slot +0x2c that the plug folds into BL/BR.
Everything is scaled by 40.0 ([0x100b5670]) at the end.

DSP kernels (HSP vtable, stdcall): #42 dst=src*g, #43 buf*=g, #62 dst+=src*g, #63 dst+=src,
#94 one-pole HPF  s=x+(2a-1)s1, y=a(s-s1);  #99 two cascaded one-pole LPs v=v(1-a)+xa, w=va+w(1-a);
#92 1st-order allpass t=x+a·t1, y=t1-a·t;  #91 shelf y=c1·AP(x)+c0·x;  #106 one-pole LP y=y(1-a)+xa.
CRT math: 0x1009a770 pow(st1,st0), 0x10088af0 cos, 0x10088c20 sin, 0x100887f0 sqrt(float),
0x100888f0 ln, 0x1009ac68 sinh.

Coefficients (verified against the emulator):
- input HPF (roomLF, P+0x0c gain, P+0x10 f/fs): G=g², c=cos2πf, A=2G+c-1, B=c-2cG-1,
  a=½(1+(-B-√(B²-A²))/A); g==1 → a=1 (bypass).
- input LP (roomHF, P+0x04, P+0x08): b=((1-gc)-√((1-gc)²-(1-g)²))/(1-g), a=1-b (per stage).
- late-line LP #106 (FUN_10061cf0(g,f,r)): r clamped ≥0.1 (r>1 → w=π), p=g^(2-2/r),
  B=(p-1)/(1-c), D=-2B-2, x=(S-D)/2B (or (-D-S)/2B if |x|>1), a=min(1-x,0.985); B==0 → 1.
- late-line shelf #91: c0=(1+g)/2, low shelf c1=c0-1 (DC gain g) / high shelf c1=1-c0 (Nyquist gain g);
  α=sin w·sinh(½ln2·√2·w/sin w) (×g for the high shelf), q=α/√g, a=(1-q)/(1+q).

Topology (concert hall trace, 48 kHz):
input → HPF → LP2 → ring A (early taps) and ring B (late-input taps).
- early: 4 taps of ring A at reflDelay+{0,80,112,184}·(rate/48k) gain P+0x88 → 4 Schroeder allpasses
  (delays P+0x9c.., read gain P+0xac.., coef P+0xbc: y=d-gx, w=x+gy) → outputs
  FC,Cx=y0·e, SL=y1·e, SR=y2·e, FR=y3·e and FL/BR/BL = y2/y1/y3 delayed 56/24/72 ·e (e = P+0x118..).
- late: 4 taps of ring B (P+0x78.. delays, P+0x8c.. gains) feed a 4-line FDN (delays P+0xd0..,
  read gains P+0xf0..) : line → shelf #91 → +tap → LP #106 → 4 allpasses (delays P+0xc0..,
  read gains P+0xe0.., coef P+0x100) with Hadamard butterflies (k=1) on the allpass feedback, second
  butterfly stage, modulated output delay (576-sample ring, interpolated), feedback written back
  permuted (1,3,0,2), outputs FC,Cx,BR,SL (gain P+0x13c..) and FL,FR,SR,BL via a 2-ring 32/40/48/64 delay.

### Parameters, stereo output, verification

- Property struct (per listener, linear units) -> 348-byte parameter block: fns 0x10060c50 and helpers
  (0x1005f5f0 FDN lengths via a Cardano cubic + caps, 0x1005ed10 per-line dB gains / LF shelves /
  energy normalisation, 0x1005ff30 diffusion -> rotation angle, 0x1005fcc0 delays, 0x1005f190/f330 early
  allpasses, 0x100608a0/f0 modulation).  All float->int conversions truncate; run at 53-bit precision
  (the emulator must set FPCW=0x27F, unicorn defaults to 0 = 24-bit).  EAX1 -> properties: fn 0x1004a050
  (+300 mB on reflections and reverb, size^3/16 clamped to 1).
- ALchemy quirk not reproduced: on the first, immediate parameter apply the LF-shelf allpass keeps its
  constructor coefficient 0.5; it only picks up the real value on later (crossfaded) updates.
- Kernels zero their output and state when an input chunk is all zero; not reproduced (inaudible).
- Stereo endpoint: plug folds the centre copy into BL/BR (x0.707), then converter mode 3 (fn 0x10036860,
  only at 22.05/44.1/48 kHz, else plain fold-down): side and rear pairs through sum/difference FIRs
  (80/72 taps @48k, reversed + 1 sample delay), 3 biquads per side, crossfeed of each side through 2
  more biquads and a 12-sample delay into the other, then FL/FR direct and FC x0.7071.  Tables in
  src/alrv_virt_tables.h.
- Verification: tests/test_alrv.c (8 environments incl. modulated ones: >76 dB SNR, most >100 dB),
  tests/test_alrv_params.c (266 property sets: identical blocks), virtualizer 114 dB.
  Ground truth: emulator scripts gen_gt.py / gen_params.py (re/gt).
