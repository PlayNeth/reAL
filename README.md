# reAL

A from-scratch reimplementation of **Creative ALchemy**'s `dsound.dll`, aimed at Linux + Wine.
Drop-in DirectSound replacement, configured from one `.ini`, no GUI.

ALchemy has two engines. On Creative X-Fi/Audigy hardware it translates DirectSound to OpenAL.
On everything else (and under Wine) it runs Creative's own software renderer straight to WASAPI:
HRTF, CMSS-3D, an "HRvb" reverb and all 12 EAX 4/5 effects. That second engine is the sound reAL
is chasing, so reAL is **its own mixer outputting to WASAPI**, not another OpenAL wrapper.
See [`docs/alchemy-re-notes.md`](docs/alchemy-re-notes.md).

## Status — milestone 2 (in progress)

| | |
|---|---|
| IDirectSound / IDirectSound8, class factory (CoCreateInstance) | ✅ |
| Secondary buffers: PCM 8/16/24/32-bit, float, WAVE_FORMAT_EXTENSIBLE, 1-8 ch | ✅ |
| Lock/Unlock (wrap), play/write cursors, looping, one-shots, SetCurrentPosition | ✅ |
| Volume, pan, frequency (linear resampler) | ✅ |
| Position notifications incl. DSBPN_OFFSETSTOP | ✅ |
| DuplicateSoundBuffer (shared data) | ✅ |
| DS3D buffer + listener, deferred settings/commit, cones, distance model | ✅ |
| HRTF 3D (headphones) | ✅ from `reAL_hrtf.bin` (measured with hrtfprobe); stereo panning if absent |
| Reports hardware-accelerated 3D voices (LOCHARDWARE), like ALchemy | ✅ |
| Capture / full duplex | forwarded to the real dsound.dll if one is reachable |
| EAX 1.0 (listener environment/volume/decay/damping, per-buffer reverb mix) | ✅ ALchemy's own reverb (CHRvbReverb) ported and verified sample-for-sample against ALchemy, incl. its stereo virtualizer |
| Volume/pan/send smoothing, declicked position jumps | ✅ |
| EAX 2.0 and 3.0 (listener reverb incl. environment size scaling, per-buffer direct/room/HF, obstruction, occlusion, exclusion, roll-off, air absorption) | ✅ ALchemy's property handling and per-source model ported; verified bit-for-bit against ALchemy in the emulator |
| EAX 4–5 | ⏳ reported unsupported (games fall back to EAX 3) |
| Doppler, CMSS, EAX 4/5 effects | ⏳ |
| Primary buffer write access (DSSCL_WRITEPRIMARY) | ✗ |

Tested under Wine 9.0 (32-bit prefix → winepulse → PulseAudio). `tests/dstest.c` passes and the
recorded output matches the expected 3D gains.

## Build

```sh
pip install ziglang     # or have i686-w64-mingw32-gcc in PATH
./build.sh              # -> build/dsound.dll (+ runs the native mixer unit tests)
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

## Roadmap

1. ~~DirectSound core + WASAPI mixer~~
2. ~~EAX 1/2/3 via IKsPropertySet: listener reverb props + per-source direct/room/occlusion/obstruction~~
   done (src/eax.c)
3. HRTF 3D path (`C3DStream`, `CHspHrtfFilterState`), doppler
4. ~~HRvb reverb (`CHRvbReverb`, `CHRvbDspEngine`, early reflections + late reverb)~~ done (src/alrv*.c)
5. EAX 4/5: FX slots, sends, the 11 remaining effects (`CPlug*`); reflections/reverb panning
6. CMSS-3D upmix, speaker configs > 2.0, better SRC

## Repo layout

```
src/        the DLL (C11, Win32 COM by hand)
  mixcore.* platform independent voice mixer  (unit tested natively)
  spatial.* placeholder DS3D model            (unit tested natively)
  device.c  WASAPI render thread
  buffer.c  DSB8 / 3DBuffer / Notify / KsPropertySet / 3DListener
tests/      test_mixcore.c (native), dstest.c (runs under Wine)
tools/      pe.py, rtti.py — RE helpers (pefile + capstone)
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
