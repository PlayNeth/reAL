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

DirectMusic private QI (`IDirectSound::QueryInterface`, 0x1001b4ed): for {2a8af120-e9de-4132-aaa5-4bdda5f325b8}
and {fa82931b-de98-4a68-a7fe-d683e38a6502} (asked by Microsoft's DirectMusic synth sink) ALchemy loads the
system dsound.dll, calls `DirectSoundCreate8`, `SetCooperativeLevel(GetForegroundWindow(), level)` and
returns that object's interface, so DirectMusic plays through the real DirectSound.  reAL does the same.
Wine's builtin dsound does not implement these interfaces either (E_NOINTERFACE).
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

## EAX 2.0 / 3.0 (ported in src/eax.c, verified in the emulator)

Property sets are dispatched by GUID in `0x1001eaa0`: EAX2 listener `0x10025b30`, EAX2 buffer
`0x100255a0`, EAX3 listener `0x10026980`, EAX3 buffer `0x10026120`.  Every Get/Set first records the
context's EAX version (ctx+0x28) and, on the first use of a version, loads the effect slots
(`0x10042220`); a version switch resets that version's listener block (`0x1004bb40` EAX2 defaults at
`.rdata 0x100b46b4`, `0x100456b0` EAX3 = preset 0).

**Listener.** The reverb effect keeps one block per version (effect+0x2284 EAX2, +0x22f0 EAX3): the
EAX properties, a dirty mask, and for environment-size scaling the base value of each scalable property
plus the size it was set at.  Setters validate (EAX ranges; failures are errors), record and set dirty
bits; EAX3 single-property setters also set the environment to 26 (custom).  Quirks kept:
* Environment size scales decay, reflections delay (and reflections only together with it), reverb,
  reverb delay, echo/modulation time (EAX3) with `-2000·log10` / `-3000·log10` for the levels; when the
  scaled decay crosses its 20 s limit the reverb is rebased at the crossing size.
* EAX3 air absorption: a new value is stored but flagged dirty only if nonzero (`0x100453f0`), so
  going back to 0 keeps the old DECAYHFLIMIT cap on the HF decay.  **Not kept:** reAL flags a change to 0.
* Environment 23 cubes the diffusion and scales decay by 0.75; environment 24 has no echo (EAX3 path
  uses the environment of the last preset/ALL, EAX2 the current one).
* Decay HF with DECAYHFLIMIT: `min(ratio·decay, -17.524/airAbsorptionHF)`.
Conversion into the reverb property block: EAX2 `0x1004bba0`, EAX3 `0x10045740` (room LF, echo,
modulation and HF/LF reference come from the preset table for EAX2).  No +300 mB bias (that is EAX1 only).

**Sources** (voice update `0x10040ee0`, switch on the EAX version): EAX2 `0x1004a8a0`, EAX3
`0x1004c890` turn the buffer block into four gains, millibel sums truncated toward zero:
* direct = direct + obstruction·obstructionLF + occlusion·max(ODR·occlLF, ODR+occlLF−1)
  − (EAX3 only) 2000·log10(1 + (DS3D rolloff + EAX rolloff)·X), X = (min(d,max) − min)/min.
  EAX2 multiplies DS3D's distance gain instead.  Both multiply the cone gain.
* direct HF (relative): EAX2 obstruction·(1−obstructionLF) + occlusion·(1−occlusionLF); EAX3
  obstruction + occlusion·ODR − occlusion·max(..); plus air absorption
  (listener airHF · buffer air · distance factor · rolloff · X) and, with DIRECTHFAUTO, outside volume HF ·
  cone factor.
* room = room + occlusion·(room ratio mix) [+ exclusion·exclusionLF], limited by the cone's power average
  (`0x100434c0`) with ROOMAUTO, − 2000·log10(1 + (listener + buffer room rolloff)·X), with ROOMAUTO
  − 6000·distanceFactor·rolloff·X / (342.38·decay).  Not distance attenuated otherwise.
* room HF the same way with the HF terms, decay HF and air absorption.
The voice gets a one-pole low-pass for direct HF and room HF at the HF reference (EAX2: 5 kHz):
`0x10031000`, |H(f_ref)|² = gain, coefficient quantised to Q15.  DS3DMODE_DISABLE voices have X = 0,
cone factor 1 and still feed the reverb.

Verification: `re/gen_eax_gt.py` runs ALchemy's setters, listener conversions, source conversions and the
coefficient function on random inputs in the emulator; `tests/test_eax.c` matches all of it bit for bit
(400+400 listener sequences, 1500+1500 source cases, 3000 coefficients).  `tests/eaxtest.c` +
`tools/eaxtest_analyze.py` check the result end to end under Wine (room −1000 mB → tail −10.0 dB, etc.).

## Doppler (fn 0x10043420, inputs from the source geometry 0x10042af0)

Only the radial components count: u = unit vector listener → source, vs = v_source·u, vl = v_listener·u
(head-relative: listener at the origin, not moving; DISABLE: 1).  c = (float)(342.38 / distance factor),
ratio = (float)((c + f·vl) / (f·vs + c)), clamped to 0..10; 1 when f = 0 or vs = vl.  f is the listener's
Doppler factor; with EAX 3 the buffer's EAXBUFFER_DOPPLERFACTOR is added (sum capped at 10).  The ratio
multiplies the voice's playback rate (sink method +4).  Verified against the emulator (3000/3000).

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

## Speakers beyond stereo (ported in src/speakers.c; verified in the emulator)

Layout (CSensDirectSound ctor 0x1001acd0 → 0x1001be70 → 0x1001c1a0): the engine starts at 44.1 kHz stereo, takes
the endpoint's mix format through Mmdevapi, then asks the system dsound.dll for GetSpeakerConfig and maps its low
byte: 1 headphones → 2 ch 0x3, 3 quad → 4 ch 0x33, 4 stereo → 2 ch 0x3, 6/9 (5.1 back / 5.1 surround) → 6 ch 0x3f,
7/8 (7.1 wide / 7.1 surround) → 8 ch 0x63f.  Any other value leaves the mix format.  The value stored at
CSensDirectSound+0x84c (default 6) is what GetSpeakerConfig returns (1, 3, 4, 6 or 7: no geometry);
SetSpeakerConfig only overwrites it.  The renderer (CSensDirectSoundRender 0x10021010) copies the engine's
per-bit buffers into the device channels by speaker bit.

Every buffer is mixed into one channel buffer per speaker bit (descriptor +8 + 4·bit).

2D streams (C2DStream 0x10032440): a stream whose mask fits the output's is copied channel by channel (a mono
stream without a mask becomes dual mono, mask 3); otherwise it goes through the stream's channel converter
(0x10036740, object at stream+0x11d4: +0x10 mode, +0x14 out mask, +0x18 in mask, +0x20 in count; mode chosen by
0x10035fd0 from the out mask: 3/0xb → 4 (3 = the reverb's stereo virtualizer when +8 is set), 7/0xf → 5,
0x33/0x3b → 6, otherwise 7):
- mode 4 (0x10036c10): L = (FC + BC + TC)·0.7071, R = L, then FL/BL/SL into L and FR/BR/SR into R at 1.0.
- mode 5 (0x10036d90): as 4, but FC to FC.
- mode 6 (0x10036f00): L = TC·0.5, BL = L, L += FC·0.7071, R = L, BL += BC·0.7071, BR = BL, the four corners
  at 1.0, SL into FL and BL at 0.7071 each (SR likewise).
- mode 7 (0x100370e0): shared speakers copied; FC missing → FL/FR ·0.7071 (when the input has FL/FR); TC → FL/FR
  ·0.5 and BL/BR (or SL/SR) ·0.5; BC missing → BL/BR, else SL/SR, else FL/FR ·0.7071; SL missing → FL ·0.366 +
  BL ·0.634 (SR likewise); BL missing → SL (BR → SR).
- after every mode (0x10036690): FLC/FRC missing → FL (FR) at 1.0, or FL and FC at 0.7071 each when there is a FC.
- LFE: only mode 7 copies it.  With no LFE in the output, CBassBoostMixerSmallSpeakersAndLFE (0x10034700) sends
  the input's LFE ·0.5623 through a limiter (kernel 0x1006bc70: e = max(|x| - e, 0)·a + e·b, x·thr/e above thr;
  0x1006bc30: a = 0.03 / (10·rate/48000), b = 1 - 0.01 / (50·rate/48000), thr 0.1778) and ·3.35 into FL and FR.

The reverb plug uses the same converter on four or more speakers (its 7 outputs, the centre copy already folded
into BL/BR), the stereo virtualizer (mode 3) on two.

3D streams on four or more speakers (C3DStream update 0x10033f90 → 0x10034900, Q15 step 0x10035780, render
0x10035190): the HRTF output is split between the front pair and the rear pair (kernels 0x1d8/0x1e8 write the
larger with the filter scaled by it, the other at the Q15 ratio), and the mono signal goes straight to the other
speakers (kernel 0x108).  Gains for a source of level g (az: 0 ahead, + right, ±180 behind; el up; both from
0x10042af0; d = distance × distance factor):
- p = ((1 - |el|/90)(|az|/90 - 1) + 1)/2 clamped to 0.25..0.75; when d < 0.5: p = (p - 0.5)·2d + 0.5.
  f = (0.75 - p)·2: front pair g·f, rear pair g·(1 - f).  (Ghidra shows the |az|/90 as 90/|az|: read the x87.)
- 6+ channels: LFE = max(front)·0.5 (halved again in 0x10035780); with whole degrees a = |rint(az ∓ 0.5 ± 1.5e-8)|
  (likewise e for el), when a, e ≤ 44: FC = lobe[a]·lobe[e]·max(front pair), front pair ·(1 - duck[e]·duck[a]),
  lobe[i] = 2^-1/4 cos(2i°) (0x100ae7f8), duck[i] = 2^-1/2 cos²(2i°) (0x100ae8b0).
- 8 channels, 0x63f: when 46 ≤ a ≤ 134 and e < 45, the side speaker of az's sign gets lobe[|a - 90|]·lobe[e]·
  max(g) and all four corners ·(1 - duck[|a-90|]·duck[e]).  (0xff, 7.1 wide, gets FLC/FRC from lobe[|2a - 45|]
  instead; ALchemy never picks that layout.)
tools/gen_speakers_gt.py runs 0x10036740 and 0x10034900 in the emulator; tests/test_speakers.c compares (128
converter cases exact, 1924 3D cases within 6e-8).

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
- EAX 3 panning: fn 0x10060250 fills the 9 output gains (slot 0 unused) for reflections (caller
  0x10060130, scale 0.4565) and late reverb (0x100601c0, scale 0.913) from (angle, spread), where the
  property conversion 0x100442f0 gives angle = atan2(x, z) in degrees and spread = 1 - |v| (1 = all
  around).  Slot angles 1..8: 0, 30, 90, 150, 180, 210, 270, 330.  Power mix of an even part, 12 virtual
  sources 30 degrees apart (full within 30 degrees of the direction, fading to 0 at 60, folded onto the
  slots) and a pairwise pan between neighbouring slots; out = sqrt(sum) * scale.  Port: calc_pan in
  src/alrv_params.c, 400/400 random property sets identical (gt/params_pan.bin).
- ALchemy bug, fixed in reAL: the late-reverb outputs multiply the SR tap by slot 2 (30 degrees) and the FR
  tap by slot 3 (90 degrees), mirrored against the early reflections and against the left side (verified
  in the emulator with gt/pan_r30, pan_l60).  reAL swaps them; with no panning the two gains are equal,
  so unpanned output is unchanged.  tests/test_alrv swaps them back to compare with ALchemy.
- FDN lengths: `1 - total / d0` is an extended-precision division (not rounded to float before the
  subtraction); the Cardano step after it cancels heavily, so a float division shifts delays by 8 samples
  in rare cases.
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

## EAX 4.0 (ported in src/eax.c, src/eax4fx.c; verified in the emulator)

**Dispatch** (`0x1001eaa0`, GUID table at `.rdata 0x100b3aec`): EAX4 context `0x10028260`, FX slots 0-3
`0x10028540` (one handler, slot from the GUID via `0x10027270`; `0x10027340` maps an index back to the
GUID), source `0x10027400`.  EAX5: context `0x10022500`, slots `0x10022950`, source `0x100215d0`.
Request struct: [0] buffer, [5] flags (1 get, 2 set, 0x200 basic support), [6] id (bit 31 deferred),
[7] size, [8] data.  Failures set the context's last error (+0xa8) and return 0xc0000001
(-> DSERR_INVALIDPARAM); a non-deferred Set commits (`0x1001d680`).  First use of version 4 calls
`0x10042220(4)`: slot 0 <- reverb (type 10), slot 1 <- chorus (type 3), both locked with the system key
(-1,-1) and lock = 1 (games cannot unlock them); slots 2/3 empty; `0x10050850` resets each slot.

**Context** (ctx + 0x8c): primary slot GUID, distance factor (+0x10), air absorption HF (+0x14), HF
reference (+0x18), dirty (+0x20), last error (+0xa8 = ctx+0x8c+0x1c, read-and-clear by id 6).
ALL `0x1004f700` (primary must be NULL or a slot GUID, air -100..0, HF ref 1000..20000; distance factor
not checked), primary `0x1004f840`, distance factor `0x1004f8d0` (FLT_MIN..FLT_MAX), air `0x1004f930`,
HF ref `0x1004f990`.  Last error codes: -2 invalid parameter, -4 load effect failed (slot 0x10002),
-1 lock failed, -3 no effect in the slot.

**FX slot** (slot object: +0x14 effect, +0x18/+0x1c lock key, +0x28 props: GUID +0x2c, volume +0x3c,
lock +0x40, flags +0x44, dirty +0x48): ids 0x10000 none, 0x10001 ALL (`0x1004d3a0`: volume
-10000..0, flags & ~1 == 0, lock check, load effect), 0x10002 load effect (`0x1004d470`), 0x10003 volume
(`0x1004d510`), 0x10004 lock (`0x100507e0`), 0x10005 flags (only ENVIRONMENT = 1).  Ids < 0x10000 go
to the loaded effect by type (+0xc): 1 AGC compressor, 2 autowah, 3 chorus, ..., 10 reverb (props block
effect+0x23a8, EAX3 listener layout, same setters 0x100444f0..0x100455b0 as EAX3, conversion 0x10045740
from the reverb update 0x10050e90), 11 ...  Effect GUID -> type: `0x1004cfb0` (NULL = 0).

**Source** (buffer+0x218): +8 EAX3-layout props (0x48 bytes), +0x50 + 0x20*slot per-slot sends {room,
roomHF, occlusion, occlusionLF, occlusionRoomRatio, occlusionDirectRatio, exclusion, exclusionLF},
+0xd0 two active slot GUIDs (NULL, EAX_PrimaryFXSlotID or a slot; no dirty bit), +0xf0 dirty, +0xf4 per-slot
send dirty.  Ids 0..0x1b (0x17 send, 0x18 all send, 0x19 occlusion send, 0x1a exclusion send, 0x1b active
slots; send arrays are {GUID, values} records, partial application on a bad record).
Conversion: `0x1004e7e0` (called from the voice update 0x10040ee0 case 4); direct HF reference = context HF
ref, or the primary slot's reverb HF ref when that slot has the ENVIRONMENT flag.

Effect types (GUID table `.rdata 0x100b4464`): 1 AGC compressor, 2 autowah, 3 chorus, 4 distortion, 5 echo,
6 equalizer, 7 flanger, 8 frequency shifter, 9 pitch shifter, 10 reverb, 11 ring modulator, 12 vocal morpher.
Their setters accept exactly the SDK ranges (float compares, NaN rejected; a float is only written when it
compares unequal, so -0 does not replace 0); defaults at `.rdata 0x100b4444..` equal the SDK's except the
flanger delay (0.0002 s instead of 0.002 s).

Slot loading: `0x1004ffc0` keeps an already loaded effect of the same type (with its properties); LOADEFFECT
(`0x1004d470`) then resets the effect's properties for EAX 4 (vtable +4, arg 4), ALLPARAMETERS (`0x1004d3a0`)
does not, so a reverb created through ALLPARAMETERS keeps zeroed properties (F.E.A.R. reads slot 3 back, gets
zeros, and its write is rejected for HF reference 0).  The slot lock only matters for the system key (-1,-1)
of slots 0/1; any value is stored for the others.  Slot volume (mB) scales the effect output (`0x1004d540` ->
slot + 0x50, then the 12 output gains of the slot's bus).

Source conversion details (`0x1004e7e0`, EBX = source block, [0] context props, [1] DS3D geometry):
* direct as EAX 3, plus, when the primary slot has the ENVIRONMENT flag, that slot's send occlusion
  (max(ratio * LF, ratio + LF - 1) * occlusion); direct HF likewise; air absorption HF from the primary
  reverb when the primary slot is ENVIRONMENT and holds a reverb, else from the context; all distance terms
  use DS3D distance factor x context distance factor.
* per slot: not active -> 0; slot without ENVIRONMENT -> send = room + the direct path's computed mB (no
  clamp at 0 mB), send HF = room HF + direct HF's; ENVIRONMENT slot -> the EAX 3 room / room HF model with the
  slot's send occlusion / exclusion (and the source's own for the primary slot), the slot reverb's room
  roll-off, decay / decay HF (DECAYHFLIMIT with that reverb's air absorption) for ROOMAUTO, and its air
  absorption; nothing reverb specific when the slot holds another effect.
* every HF filter of the voice (direct and sends) uses one reference: the primary ENVIRONMENT reverb's, else
  the context's.  Doppler factor = listener x source (max 10), speed of sound / (DS3D x context distance factor).
Verification: re/gen_eax4_gt.py runs the context and source handlers (whole request path) on random
sequences and the conversion on random states; re/gen_eax4fx_gt.py the slot handler's effect properties.
tests/test_eax4.c: 1800 + 6000 + 3000 + 7920 cases identical.

## EAX 4 effect DSP (`CPlug*`, ported in src/eax4dsp.c; verified with re/gen_fx_gt.py + tests/test_fx.c)

Every `CFx*` object holds its `CPlug*` at +0x10 (distortion / ring modulator: +8); the CFx update (vtable at
+8) takes `this = obj + 8`, swaps the dirty word with `InterlockedExchange` and calls the plug's setters.
Plugs process a descriptor {+4 frames, +8.. channel buffers}; channels are picked with the mask at plug
+0x14 / +0x30 and the bit table 0x100aeab4.  Ramps / glides are per sample (or per 8 samples for kernels).
Effects that take one input (echo, chorus, pitch shifter) read channel 0 only.

| effect | update | process | notes |
|---|---|---|---|
| distortion | 0x10051100 | 0x100543e0 | LP, `sign*log10(1+shape|x|)`, lattice EQ, ramped |
| ring modulator | 0x10051480 | 0x10054ea0 | HPF kernel #97 gliding ×1.01/8 samples, sine/saw/square; **bug**: every channel loop re-tests the channel it just found → left processed twice, right dry |
| frequency shifter | 0x10052460 | 0x10059010 | mono sum ×0.25, two 3-stage all-pass chains (90° pair), sin/cos; **bug**: increment 4πf/fs (shift ×2) |
| AGC compressor | 0x10052620 | 0x10059690 | DC-blocked detector power, log10, threshold −6.2, ratio 0.4, 20 ms look-ahead with two cross-faded taps; **bug**: off→on fades to the wrong tap (look-ahead stays 30 samples) |
| equalizer | 0x10052d60 | 0x1005b710 | `CParamEqFilter` low shelf / 2 peaks / high shelf (0x1006fce0 / 0x1006f8a0 / 0x10070120), biquad kernel #88; gain ×1.02, f and Q ×1.001 glides per 8 samples |
| autowah | 0x10052800 | 0x1005a3b0 | log10 envelope (attack/release coefs 10^(−1/(t·fs))), all-pass resonator; rate change re-multiplies the stored times by 0.3 (avoided by rebuilding) |
| echo | 0x10051730 | 0x10055f80 | two taps (delay, delay+LR), feedback from tap 2 through a one-pole (coef √damping ≤ 0.99), 200 ms tap cross-fades; spread mixes the taps |
| chorus | 0x10051db0 | 0x100571c0 | mono delay line 3097, sine/triangle LFO (right channel offset by phase), centre floor(delay·fs+0.5) ≥ 25, depth ≤ centre−25, 20 ms cross-fades for delay/depth/phase |
| flanger | 0x10052120 | 0x100581e0 | as chorus but stereo, a 778-sample line per channel, minimum 10 / margin 8 |
| pitch shifter | 0x10052ba0 | 0x1005a900 | two taps sweeping ±1024 around 1026 samples, sin/cos windows; **bug**: taps move by 2^(p/12)+1 per sample → grains play backwards |
| vocal morpher | 0x10051a50 | 0x10056740 | 4 Chamberlin SVF band-passes per channel, formants (table 0x100a70f0: 4 f, 4 Q per phoneme) interpolated by a sine (rotation) / triangle / saw LFO; **bug**: reset 0x10056690 zeroes phoneme A's first formant coefficient |

Slot activation (the plug state machine, e.g. 0x10055580 for the echo) snaps parameters to their targets
and clears the delay lines; reAL does the same when an effect is loaded.

## DirectMusic content survey: Midnight Club II (AUDIO\DirectMusic\PC)

48 segments + 26 DLS collections, no styles/bands/chordmaps as separate files (tools/dmdump.py).
- 47 segments: Tempo, TimeSig, Chord, Seq, Band tracks; 1 (p_cafe) TimeSig, Seq, Band. No Style/Pattern/Motif tracks:
  the music is sequence-based (MIDI-like seqt + curves); the Chord track is there because Producer adds one by default.
  (dmstyle.dll gets loaded only because the Chord track class lives there.)
- Bands reference one game DLS each via DMRF (e.g. fe_hard_0x.sgt -> fe_hard.dls, bank LSB 1, programs 0..15).
  t_temple.sgt's band has no reference (patches 0x100..0x105) -> resolved against already-loaded collections / gm.dls.
- DLS: Level 1 and Level 2 (rgn2/art2) regions, all mono 16-bit PCM at 8..36 kHz.
- Runtime (dmtrace): InitAudio(DMUS_APATH_SHARED_STEREOPLUSREVERB, 128 pchannels, DMUS_AUDIOF_BUFFERS), 4 extra
  standard audiopaths of type 1, segments played as SECONDARY|CONTROL (0x280) on their own path, faded with
  IDirectMusicAudioPath::SetVolume, polled with IsPlaying. The reverb path uses dsdmo's Waves reverb (IMediaObject).
