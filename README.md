# reAL

A from-scratch reimplementation of **Creative ALchemy**'s `dsound.dll`, aimed at Linux + Wine.
Drop-in DirectSound replacement, configured from one `.ini`, no GUI.

ALchemy has two engines. On Creative X-Fi/Audigy hardware it translates DirectSound to OpenAL.
On everything else (and under Wine) it runs Creative's own software renderer straight to WASAPI:
HRTF, CMSS-3D, an "HRvb" reverb and all 12 EAX 4/5 effects. That second engine is the sound reAL
is chasing, so reAL is **its own mixer outputting to WASAPI**, not another OpenAL wrapper.
See [`docs/alchemy-re-notes.md`](docs/alchemy-re-notes.md).

## Status

reAL has two halves in one DLL:
- **The DirectSound side** is the ALchemy reimplementation. It is done except for CMSS / surround and EAX 5.
- **reDM** is its own DirectMusic, switched on with `music=true` (see "reDM" below). It is a faithful port of
  Windows XP's DirectMusic.

### DirectSound

| | |
|---|---|
| IDirectSound / IDirectSound8, class factory (CoCreateInstance) | ✅ |
| Secondary buffers: PCM 8/16/24/32-bit, float, WAVE_FORMAT_EXTENSIBLE, 1-8 ch | ✅ |
| Lock/Unlock (wrap), play/write cursors, looping, one-shots, SetCurrentPosition | ✅ |
| Volume, pan, frequency | ✅ 32-tap windowed-sinc resampler (`resampler=linear` for ALchemy's) |
| Position notifications incl. DSBPN_OFFSETSTOP | ✅ |
| DuplicateSoundBuffer (shared data) | ✅ |
| DS3D buffer + listener, deferred settings/commit, cones, distance model | ✅ |
| HRTF 3D (headphones) | ✅ from `reAL_hrtf.bin` (measured with hrtfprobe); stereo panning if absent |
| Reports hardware-accelerated 3D voices (LOCHARDWARE), like ALchemy | ✅ |
| Output | ✅ WASAPI |
| Capture / full duplex | forwarded to the real dsound.dll if one is reachable |
| EAX 1.0 (listener environment/volume/decay/damping, per-buffer reverb mix) | ✅ ALchemy's own reverb (CHRvbReverb) ported and verified sample-for-sample against ALchemy, incl. its stereo virtualizer |
| Volume/pan/send smoothing, declicked position jumps | ✅ |
| EAX 2.0 and 3.0 (listener reverb incl. environment size scaling, EAX 3 reflections/reverb panning, per-buffer direct/room/HF, obstruction, occlusion, exclusion, roll-off, air absorption) | ✅ ALchemy's property handling and per-source model ported; verified bit-for-bit against ALchemy in the emulator |
| EAX 4.0 (context, 4 FX slots, a reverb engine per slot, per-source sends / occlusion / exclusion per slot, properties of all 12 effects) | ✅ ALchemy's handling and source model ported; bit-for-bit against ALchemy in the emulator. Audio: all 12 effects (reverb, AGC compressor, autowah, chorus, distortion, echo, equalizer, flanger, frequency shifter, pitch shifter, ring modulator, vocal morpher), the 11 non-reverb ones ported from ALchemy's `CPlug*` DSP and matching it to >310 dB SNR |
| Doppler (DS3D factor, EAX 3 per-buffer factor) | ✅ ALchemy's formula, bit-identical in the emulator |
| DirectSound effects (SetFX): Gargle, Echo, ParamEq, Chorus, Flanger, Distortion, Compressor, Waves reverb, I3DL2 reverb | ✅ bit-exact ports of Microsoft's dsdmo (Windows 11's; I3DL2 from XP SP3); SetFX's validation and placement match Windows 7's dsound |
| CMSS-3D upmix, speaker configs > 2.0 | ⏳ left for last |
| EAX 5.0 | ⏳ left for last; reported unsupported until then (games fall back to EAX 4) |
| Primary buffer write access (DSSCL_WRITEPRIMARY) | ✗ |

### reDM (DirectMusic)

Checked against Microsoft's own DLLs running under Wine: COM test programs run against both and their output is
diffed line by line, and offline captures are compared event by event.

| | |
|---|---|
| Loader, segments (sequence, band, tempo, time signature, SysEx, wave, marker tracks), MIDI files | ✅ |
| Performance: playback, transitions, loops, control and secondary segments, notifications, GetParam/SetParam, tool graphs and application tools | ✅ |
| DLS synth (dmsynth port, incl. wave voices and streaming), the Waves reverb, SoundFont banks converted to DLS | ✅ |
| Audiopaths: the standard ones and audiopath configurations (buffers, effects, sends, PChannel maps) | ✅ |
| Port objects (the synth port, its sink and master clock) | ✅ |
| MIDI through winmm (midiOut, midiStream, MCI) for games that play General MIDI | ✅ |
| Styles: style and motif tracks, the pattern chooser, variations, chord mapping, mute and pattern tracks | ✅ |
| Chord maps and the composer: chord map, chord map and signpost tracks, ComposeSegmentFromTemplate, ComposeSegmentFromShape, ComposeTemplateFromShape, ChangeChordMap, Segment::Compose | ✅ |
| Composer: ComposeTransition / AutoTransition | 🔧 in progress |
| Composer: Segment::Compose with from/to segments, recomposing while a segment plays | ⏳ |
| Scripts (dmscript, AudioVBScript) | ⏳ |

As on the DirectSound side, Microsoft's bugs are fixed behind flags. `REAL_MUSIC_NOFIXES` in the environment
turns the fixes off for comparison runs. `docs/STATUS.md` has the details and the testing setup.

Deliberate departures from ALchemy (things it gets wrong): no ~11 kHz cut on the output; a
windowed-sinc resampler instead of linear interpolation (a 9 kHz tone in a 22 kHz sound: −5 dB and a
mirror image only 6.5 dB below it with linear, −0.1 dB and −85 dB with sinc); EAX 3
reverb panning routes the late reverb's right-hand 30°/90° slots to front-right/side-right like the
reflections (ALchemy swaps those two, so a reverb panned hard right lands in the front speaker);
setting EAX 3 air absorption back to 0 takes effect (ALchemy keeps the old HF-decay limit); an EAX 4
reverb loaded into a slot through ALLPARAMETERS starts from the default room (ALchemy leaves it all zeros,
which breaks F.E.A.R.'s extra reverb slot); the flanger's default delay is the SDK's 0.002 s (ALchemy 0.0002).
EAX 4 effect bugs fixed: the ring modulator processes both channels (ALchemy filters and modulates the left
one twice and leaves the right dry); the frequency shifter shifts by the frequency asked for (ALchemy: twice
it); the pitch shifter plays its grains forwards (ALchemy's taps run backwards, so everything but a pure tone
comes out reversed in ~20 ms chunks); the AGC compressor gets its look-ahead back after off → on; the vocal
morpher's reset no longer zeroes phoneme A's first formant; the chorus / flanger delay lines are long enough
for 192 kHz; a slot's volume is applied after the effect (it matters for distortion / AGC).

Tested under Wine (9.0 with winepulse → PulseAudio; 9.22 WoW64 for the reDM comparisons).

## Build

```sh
pip install ziglang     # or have i686-w64-mingw32-gcc in PATH
./build.sh              # -> build/dsound.dll (DirectSound + reDM), the tracers and the test programs
```

## Install (Wine)

```sh
WINEPREFIX=~/Games/foo ./install.sh                     # every app in the prefix
WINEPREFIX=~/Games/foo ./install.sh --app game.exe      # only game.exe (others keep Wine's dsound)
WINEPREFIX=~/Games/foo ./install.sh --uninstall
```

That copies the DLL into `syswow64` (64-bit prefix) or `system32` (32-bit prefix), sets the
`dsound=native,builtin` override and drops a default `C:\windows\reAL.ini`. Then forget about it.
Re-run after a Wine upgrade, because `wineboot -u` restores Wine's dsound.dll.

Or do it by hand: put `dsound.dll` next to the game's exe and set `WINEDLLOVERRIDES=dsound=n,b`.

On Windows, put it next to the game's exe. Don't overwrite the system copy.

## Configure

`C:\windows\reAL.ini`, with a `[default]` section and per-exe sections. See `reAL.ini.example`.
ALchemy's own `dsound.ini` keys (`buffers`, `duration`, `maxvoicecount`, `disabledirectmusic`,
`logdirectsound*`) are also accepted, from a `dsound.ini` next to the DLL. `REAL_LOG=2` in the
environment turns on API logging to `reAL.log` next to the game's exe.

## reDM

`music=true` in `reAL.ini` makes reAL answer for DirectMusic itself. It needs nothing from Microsoft except
the GM instrument collection (`gm.dls`), and `gmdls=` can point at another DLS file or a SoundFont instead.
reAL takes over DirectMusic when the game loads `dsound.dll`, which nearly every game does before its music
starts. For a game that creates DirectMusic first, copy `dsound.dll` to `reAL-music.dll` and register it with
`regsvr32` (other programs are handed on to the DirectMusic registered before). `midi=true` adds a GM synth
MIDI output for games that play General MIDI through winmm. `reAL.ini.example` has the details.

## Roadmap

1. ~~DirectSound core + WASAPI mixer~~
2. ~~EAX 1/2/3 via IKsPropertySet~~ (src/eax.c)
3. ~~HRTF 3D path (`C3DStream`, `CHspHrtfFilterState`), Doppler~~
4. ~~HRvb reverb (`CHRvbReverb`, `CHRvbDspEngine`, early reflections + late reverb)~~ (src/alrv*.c)
5. ~~EAX 4: FX slots, sends, all 12 effects~~ (src/eax4dsp.c)
6. ~~DirectSound effects (SetFX)~~ (src/fx.c, src/dsfx.c, src/i3dl2.c)
7. reDM, reAL's own DirectMusic (src/music/): playback, synth, audiopaths, tools and styles done; the composer
   nearly done; scripts next
8. Last: CMSS-3D upmix and speaker configs > 2.0, then EAX 5

## Repo layout

```
src/        the DLL (C11, Win32 COM by hand)
  mixcore.* platform independent voice mixer  (unit tested natively)
  eax4dsp.* the 11 non-reverb EAX 4 effects (tests/test_fx.c against ALchemy)
  spatial.* placeholder DS3D model            (unit tested natively)
  device.c  WASAPI render thread
  buffer.c  DSB8 / 3DBuffer / Notify / KsPropertySet / 3DListener
  fx.c, dsfx.c, i3dl2.c  DirectSound effects (SetFX)
  music/    reDM: engine (dm*.c), synth (msynth.c), DLS / SoundFont, COM layer (dmcom*.c)
tests/      unit tests (native) and COM test programs (run under Wine; comcmp.sh diffs them against Microsoft's)
tools/      RE helpers (pe.py, rtti.py), dmgt (drives Microsoft's DirectMusic offline), sty/ (style, chord map and
            segment test cases), dsfx/ and i3dl2/ (effect comparisons), hrtf_*.py
dmtrace/    DirectMusic API tracer (gen.py generates the logging wrappers from the MinGW headers)
docs/       RE notes, vtables.json (every ALchemy class + vtable)
```

Personal project. ALchemy is © Creative Technology. This repo contains no Creative code or binaries.

## dstrace — DirectSound API tracer

`build/tracer/dsound.dll` sits in front of any other dsound.dll and logs every call the game makes
and what came back, including the raw values of every EAX property set/get. This is how we capture
reference traces from ALchemy: its software engine has no call logging of its own.

```
game dir/
  dsound.dll            <- build/tracer/dsound.dll
  alchemy_dsound.dll    <- ALchemy's dsound.dll, renamed
  (optional) dstrace.ini:  target=alchemy_dsound.dll   level=1|2
```
Run with `WINEDLLOVERRIDES=dsound=n,b`. Output: `dstrace.log` next to the game exe.
`level=2` adds per-frame calls (Lock/Unlock/GetCurrentPosition/GetStatus/AddRef/Release).

## dmtrace — DirectMusic API tracer

Logs what a game asks DirectMusic to do (it started reDM).
DirectMusic is a set of COM classes, so dmtrace is installed as their server for the current user instead of
being dropped into the game folder:

1. Put `dmtrace.dll` somewhere permanent, e.g. `C:\dmtrace\`.
2. `regsvr32 C:\dmtrace\dmtrace.dll` — no admin needed: it only writes `HKCU\Software\Classes\CLSID`, so
   Windows' own registration is untouched. (Under Wine, whose COM ignores HKCU classes, it also redirects
   the machine-wide entries and keeps the originals to restore.)
3. Run the game (not as administrator: elevated programs ignore per-user COM registrations). The log is
   `dmtrace.log` next to the game's exe.
4. `regsvr32 /u C:\dmtrace\dmtrace.dll` to uninstall.

`dmtrace.ini` next to the dll: `level=1` (calls the game makes), `2` (also AddRef/Release, GetTime,
GetParam, ...), `3` (also DirectMusic's internal calls); `target_dir=` to load the DirectMusic DLLs from
another folder. `dmtest.exe` plays `test.mid` through DirectMusic as a smoke test.

Under Wine, Microsoft's DirectMusic DLLs need DirectSound's private DirectMusic interfaces, which Wine's
dsound does not have (reAL forwards them to the system dsound, as ALchemy does) — another reason for an own
DirectMusic implementation rather than Wine's or Microsoft's.

## HRTF data

reAL renders 3D sounds through per-direction filters loaded from `reAL_hrtf.bin`
(next to dsound.dll, or in the Windows directory; `hrtf=<path>` or `hrtf=off` in reAL.ini).
The file is **not** in this repo: build it from a measurement of the implementation you want to
match, e.g. your own ALchemy install:

```
hrtfprobe.exe                       # Windows: plays + records itself -> hrtfprobe.wav/.txt
python3 tools/hrtf_extract.py hrtfprobe.wav hrtfprobe.txt measured.npz
python3 tools/hrtf_build.py measured.npz reAL_hrtf.bin
```
Measured against ALchemy: median rms difference 1.1 % (−39 dB) over 133 directions, ear levels
within 0.1 dB.
