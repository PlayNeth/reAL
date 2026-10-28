# reAL

A drop-in `dsound.dll` that brings back **EAX, 3D audio, surround and DirectMusic** in classic PC games.

reAL is an open reimplementation of **Creative ALchemy**'s `dsound.dll` and of Microsoft's **DirectMusic**. Put it in place of DirectSound and a game gets ALchemy's EAX 1–4 effects and reverb, HRTF 3D audio on headphones, surround up to 7.1 with CMSS-3D upmixing, and, if you want it, a DirectMusic of its own. No Creative hardware or Windows DLLs are needed. It's made for Linux with Wine or Proton, and works on Windows too.

Both halves are checked against the originals: the EAX processing bit for bit against ALchemy, and DirectMusic call for call against Windows XP's DLLs. One `.ini` file, no GUI.

*reAL is an independent project, not affiliated with or endorsed by Creative Technology or Microsoft, and contains none of their files. Creative, ALchemy, EAX, Microsoft, DirectSound and DirectMusic are trademarks of their owners.*

---

## Contents

- [Requirements](#requirements)
- [Installing](#installing)
- [Uninstalling](#uninstalling)
- [What it does](#what-it-does)
- [Settings](#settings)
- [Files reAL uses](#files-real-uses)
- [Troubleshooting](#troubleshooting)
- [HRTF data](#hrtf-data)
- [Building from source](#building-from-source)
- [For developers](#for-developers)
- [License](#license)

---

## Requirements

- A **32-bit game** that plays its sound through DirectSound, and optionally DirectMusic or Windows' MIDI.
- **Linux** with Wine or Proton, or **Windows**. reAL plays through WASAPI and uses Microsoft's Universal C Runtime, which Windows 10 and 11 include; Windows 7 and 8.1 get it from Windows Update.
- **Optional, for DirectMusic music (reDM):** the General MIDI collection `gm.dls`. Windows has it; under Wine, copy it from a Windows install or point `gmdls=` at another DLS file or a SoundFont.
- **Optional, for HRTF on headphones:** a `reAL_hrtf.bin` you build yourself (see [HRTF data](#hrtf-data)). Without it, 3D sounds use stereo panning.

## Installing

### Linux (Wine / Proton)

Unzip the release, then run `install.sh` for the prefix the game uses:

```sh
WINEPREFIX=~/Games/foo ./install.sh                  # every program in the prefix
WINEPREFIX=~/Games/foo ./install.sh --app game.exe   # only game.exe; the rest keep Wine's dsound
```

It copies `dsound.dll` into the prefix's `syswow64` (64-bit prefix) or `system32` (32-bit prefix), keeping Wine's own copy, sets the `dsound=native,builtin` override, and writes a default `C:\windows\reAL.ini`.

- **Proton:** use the game's prefix, `.../steamapps/compatdata/<appid>/pfx`.
- **After a Wine or Proton update,** run it again: updating the prefix puts Wine's `dsound.dll` back.
- **By hand, for one game:** put `dsound.dll` next to the game's exe and run the game with `WINEDLLOVERRIDES=dsound=n,b` (on Steam: `WINEDLLOVERRIDES=dsound=n,b %command%` in the launch options).

### Windows

Put `dsound.dll` next to the game's exe. Don't replace the one in `System32` or `SysWOW64`.

> **Games that create DirectSound through COM** (`CoCreateInstance`; Hot Wheels: Beat That!, for one) load Windows' own `dsound.dll` and never see the one next to them. For those, set the two paths in `tools/dsound-com-redirect.reg` to your `dsound.dll` and double-click it. It points DirectSound's COM classes at reAL for your Windows user, so it also applies to any other 32-bit program of yours that does the same (few do). It doesn't apply to programs run as administrator. `tools/dsound-com-redirect-undo.reg` removes it.

### Checking it works

Set `log=1` in `reAL.ini` and start the game. `reAL.log` appears next to the game's exe, and its first line names reAL's version and build.

## Uninstalling

- **Wine / Proton:** `WINEPREFIX=~/Games/foo ./install.sh --uninstall` puts Wine's `dsound.dll` back and removes the global override. Per-game overrides made with `--app` stay, but fall back to Wine's DLL.
- **Windows:** delete `dsound.dll` from the game's folder. If you used the COM redirect, run `dsound-com-redirect-undo.reg`. If you registered `reDM.dll` (see [DirectMusic](#directmusic-redm)), run `regsvr32 /u` on it.
- You can also delete `reAL.ini`, `reAL.log` and `reAL_hrtf.bin`.

---

## What it does

reAL has two halves in one DLL. The DirectSound side is the ALchemy reimplementation, and **reDM** is its own DirectMusic, a port of Windows XP's, switched on with `music=true`. Both are complete.

ALchemy has two engines. On Creative X-Fi and Audigy cards it translates DirectSound to OpenAL. Everywhere else, Wine included, it runs Creative's own software renderer straight to WASAPI: HRTF, CMSS-3D, the "HRvb" reverb and all the EAX 4 effects. That second engine is the sound reAL recreates, so reAL is **its own mixer playing through WASAPI**, not another OpenAL wrapper. [`docs/alchemy-re-notes.md`](docs/alchemy-re-notes.md) has the details.

### DirectSound

- **All of DirectSound 8:**
  - buffers in PCM (8, 16, 24 and 32-bit), float and WAVE_FORMAT_EXTENSIBLE, 1 to 8 channels;
  - looping and one-shot sounds, Lock / Unlock, position notifications, duplicate buffers;
  - volume, pan and frequency, with a 32-tap windowed-sinc resampler;
  - smoothed volume changes and declicked position jumps;
  - writing to the primary buffer (DSSCL_WRITEPRIMARY), as Windows' DirectSound does.
- **Hardware 3D voices:** reAL reports hardware-accelerated buffers, as ALchemy does, so games turn on their best sound options.
- **Answers as Windows does:** reAL has been compared with Windows 7's DirectSound over thousands of random call sequences. Where Windows accepts a call, so does reAL; where ALchemy is more lenient than Windows, reAL keeps ALchemy's behaviour. The differences kept on purpose are listed in [`docs/STATUS.md`](docs/STATUS.md).
- **Capture and full duplex** are passed to the system's DirectSound.

### EAX

- **EAX 1.0, 2.0 and 3.0:** ALchemy's own reverb, its property handling and its per-sound model (direct, room, obstruction, occlusion, exclusion, roll-off, air absorption), checked bit for bit against ALchemy.
- **EAX 4.0:** the context, four effect slots with a reverb engine each, per-sound sends, and all 12 effects: reverb, AGC compressor, autowah, chorus, distortion, echo, equalizer, flanger, frequency shifter, pitch shifter, ring modulator and vocal morpher. The 11 non-reverb effects are ported from ALchemy's and match it to better than 310 dB.
- **EAX 5.0** isn't supported, on purpose: games used it through OpenAL, which OpenAL Soft covers. reAL reports it unsupported, so games fall back to EAX 4.

### 3D audio and speakers

- **DirectSound 3D:** listener and sounds, deferred settings, cones, distances, and Doppler with ALchemy's formula.
- **HRTF on headphones,** from a `reAL_hrtf.bin` (see [HRTF data](#hrtf-data)).
- **Stereo 3D sounds from older games** are accepted, as Windows accepts them, and play as their mono mix while positioned.
- **Speakers:** stereo, headphones, quad, 5.1 and 7.1, from Windows' speaker setup or `speakers=`. ALchemy's layouts, its channel converter for multichannel sounds (the LFE goes to the fronts through its bass limiter), and its 3D panning across the speakers.
- **CMSS-3D** (`cmss=on`, off by default as in ALchemy): ALchemy's stereo-to-surround upmixer for music, ambience and videos. On quad, 5.1 and 7.1 it moves centred sound to the centre speaker and ambience to the rears and sides; on stereo speakers and headphones it virtualizes the rear pair. It matches ALchemy to better than −80 dB.

### DirectSound effects

- **SetFX:** Gargle, Echo, ParamEq, Chorus, Flanger, Distortion, Compressor, Waves reverb and I3DL2 reverb, as bit-exact ports of Microsoft's own effects. Their checks and placement match Windows 7's DirectSound.

### DirectMusic (reDM)

`music=true` makes reAL answer for DirectMusic itself. It needs nothing from Microsoft except the General MIDI collection, and `gmdls=` can point at another DLS file or a SoundFont instead.

- **What's covered:**
  - the loader with its cache, containers and MIDI files;
  - segments and all their tracks: sequence, band, tempo, time signature, SysEx, wave, marker, lyrics, triggers and parameter control;
  - the performance, with transitions, loops, notifications and tool graphs;
  - the DLS synth, with the Waves reverb;
  - audiopaths and their configurations;
  - ports, buffers and clocks, including the older DirectX 6 and 7 ways of driving them;
  - styles, chord maps and the composer;
  - scripts: AudioVBScript, VBScript, JScript and the script track.
- **Checked against Microsoft's DLLs:** test programs run against both and their output is compared line by line, and offline captures are compared event by event.
- **When reAL takes over:** when the game loads `dsound.dll`, which nearly every game does before its music starts. For a game that creates DirectMusic first, copy `dsound.dll` to `reDM.dll` and register it with `regsvr32` (under Wine, `wine regsvr32`). Games with `music=true` get reDM, and every other program is handed to the DirectMusic registered before.

### MIDI music

`midi=true` adds a "reDM GM Synth" output and makes it the MIDI mapper, for games that play General MIDI through Windows' MIDI functions (Claw, for one). The music plays through reAL with the same GM collection.

### Where reAL differs from ALchemy

reAL fixes what ALchemy gets wrong:

- **Cleaner sound:**
  - no ~11 kHz cut on the output;
  - a windowed-sinc resampler instead of linear interpolation. A 9 kHz tone in a 22 kHz sound loses 5 dB and gets a mirror image only 6.5 dB below it with linear; with sinc it loses 0.1 dB and the image is 85 dB down. `resampler=linear` gives ALchemy's.
- **EAX fixes:**
  - EAX 3 reverb panned hard right goes to the right speakers (ALchemy swaps two of its slots, so it lands in the front speaker);
  - setting EAX 3 air absorption back to 0 takes effect;
  - an EAX 4 reverb loaded through ALLPARAMETERS starts from the default room (ALchemy leaves it all zeros, which breaks F.E.A.R.'s extra reverb slot);
  - the flanger's default delay is the SDK's 0.002 s (ALchemy: 0.0002).
- **EAX 4 effect bugs fixed:**
  - the ring modulator processes both channels;
  - the frequency shifter shifts by the frequency asked for, not twice it;
  - the pitch shifter plays forwards (ALchemy's comes out reversed in ~20 ms chunks);
  - the AGC compressor keeps its look-ahead after being switched off and on;
  - the vocal morpher's reset no longer zeroes a formant;
  - the chorus and flanger work at 192 kHz;
  - a slot's volume is applied after the effect.
- **`fixes`** (on by default): where ALchemy lets through something harmful, reAL refuses a zero 3D distance, as Windows does, and keeps a runaway Doppler shift or volume out of the mix. It also fixes flaws in Microsoft's DirectSound effects. `fixes=false` behaves exactly as ALchemy and Microsoft's effects do.

reDM fixes Microsoft's DirectMusic bugs the same way; `REDM_NOFIXES` in the environment turns those off. [`docs/STATUS.md`](docs/STATUS.md) has the details.

---

## Settings

reAL reads `reAL.ini` from these places, later ones winning:

1. `C:\windows\reAL.ini` (in a Wine prefix, `drive_c/windows/reAL.ini`);
2. `dsound.ini` next to `dsound.dll`, in ALchemy's format;
3. `reAL.ini` in the game's folder;
4. the file named by the `REAL_INI` environment variable.

Keys in `[default]` apply to every game, and a `[game.exe]` section overrides them for that game. [`reAL.ini.example`](reAL.ini.example) explains every key. The common ones:

| Setting | What it does |
|---|---|
| `log` | `0` off, `1` info, `2` API calls, `3` everything (very verbose). With `1` or more, refused calls are logged too. |
| `music` | `true` uses reDM, reAL's own DirectMusic. |
| `gmdls` | Another DLS file or SoundFont (`.sf2`) for reDM's instruments. A Linux path works under Wine. |
| `midi` | `true` plays the game's MIDI music through reDM's GM synth. `midirate=` and `midireverb=` adjust it. |
| `speakers` | `auto` (Windows' setup), `stereo`, `headphones`, `quad`, `5.1` or `7.1`. |
| `cmss`, `cmssintensity` | CMSS-3D upmixing of stereo sounds, and its rear level (`-100` to `99`, `0` is ALchemy's default). |
| `hrtf` | A path to a `reAL_hrtf.bin`, or `off`. |
| `resampler` | `sinc` (default) or `linear` (ALchemy's). |
| `fixes` | `false` behaves exactly as ALchemy. |
| `latency` | WASAPI buffer length in ms; `0` uses ALchemy's `buffers` × `duration` / 2. |
| `buffers`, `duration`, `maxvoicecount`, `disabledirectmusic` | ALchemy's own keys, with the same meaning and limits. |
| `enabled`, `realdsound` | `enabled=false` hands everything to the DirectSound named by `realdsound=`. |

**Environment variables:** `REAL_LOG=0`–`3` overrides `log`, `REAL_INI` names an extra ini file, `REAL_NOFIXES` is `fixes=false`, and `REDM_NOFIXES` turns off reDM's fixes.

## Files reAL uses

| File | |
|---|---|
| `dsound.dll` | reAL itself. |
| `reAL.ini` | Settings (see [Settings](#settings)). |
| `dsound.ini` | ALchemy's settings file, if you have one next to the DLL. |
| `reAL.log` | Next to the game's exe, with `log=1` or more; `logfile=` puts it elsewhere. Send it with any bug report. |
| `reAL_hrtf.bin` | HRTF filters, next to `dsound.dll` or in the Windows folder. |
| `gm.dls` | DirectMusic's instruments, for reDM: the system's, or the one named by `gmdls=`. |
| `reDM.dll` | Only if you registered reAL as DirectMusic's server (a copy of `dsound.dll`). |

## Troubleshooting

- **Nothing changes, and `log=1` writes no `reAL.log`:** reAL isn't being loaded.
  - Under Wine: run `install.sh` again (a Wine or Proton update puts Wine's DLL back), or check the `dsound=n,b` override.
  - On Windows: the game may create DirectSound through COM; use the COM redirect (see [Installing](#windows)). The redirect doesn't apply to games run as administrator.
- **A game misbehaves or crashes:** set `log=1` and look for `refused:` lines in `reAL.log`. Each one is a DirectSound call reAL turned down, with its arguments and where in the game it came from. A game seldom expects a call that works on Windows to fail, so these are the first place to look. Please report problems with the log.
- **No DirectMusic music:** set `music=true` for that game. If the game creates DirectMusic before loading `dsound.dll`, register `reDM.dll` (see [DirectMusic](#directmusic-redm)). The log says which GM collection was loaded; if none was found, set `gmdls=`.
- **Microsoft's DirectMusic under Wine** (`winetricks directmusic`) needs two private DirectSound interfaces that Wine's dsound lacks. reAL passes them to the real dsound, so point `realdsound=` at Microsoft's `dsound.dll` for those games, or use reDM instead.
- **No MIDI music** in an older game: try `midi=true`.
- **Crackling or stutter:** raise `latency=`, or `buffers` / `duration` for that game.
- **To compare with ALchemy:** `fixes=false` and `resampler=linear`.

## HRTF data

reAL renders 3D sounds on headphones through per-direction filters loaded from `reAL_hrtf.bin`, next to `dsound.dll` or in the Windows folder (`hrtf=<path>` or `hrtf=off` in `reAL.ini`). The file isn't included: build it from a measurement of the implementation you want to match, such as your own ALchemy install. The tools are in the release's `tools` folder:

```
hrtfprobe.exe                                   # on Windows: plays and records itself -> hrtfprobe.wav / .txt
python3 tools/hrtf_extract.py hrtfprobe.wav hrtfprobe.txt measured.npz
python3 tools/hrtf_build.py measured.npz reAL_hrtf.bin
```

Measured against ALchemy: a median difference of 1.1 % (−39 dB) over 133 directions, with ear levels within 0.1 dB.

## Building from source

Releases are built with Zig 0.17 (its C compiler, `zig cc`):

```sh
pip install ziglang     # or have zig in PATH
./build.sh              # -> build/dsound.dll, the tracers, the test programs, and runs the native unit tests
```

`build.sh` also takes a MinGW-w64 toolchain (`CC=i686-w64-mingw32-gcc ./build.sh`).

---

## For developers

### Repo layout

```
src/        the DLL (C11, Win32 COM by hand)
  dsound.c, buffer.c   IDirectSound8, buffers, 3D buffers and listener, notifications, property sets
  device.c             the WASAPI output thread
  mixcore.*            the platform-independent voice mixer (unit tested natively)
  spatial.*            the DirectSound 3D model (distance, cones, Doppler)
  eax*.c, alrv*.c      EAX 1-4 and ALchemy's reverb; eax4dsp.* the 11 non-reverb EAX 4 effects
  speakers.c, cmss3.c, virt.c, hrtf.c   speaker layouts, CMSS-3D, virtualizers, HRTF
  fx.c, dsfx.c, i3dl2.c                 DirectSound effects (SetFX)
  refused.c            the log of refused calls
  music/               reDM: engine (dm*.c), synth (msynth.c), DLS / SoundFont, COM layer (dmcom*.c)
tests/      native unit tests, and test programs run under Wine (comcmp.sh and fuzzcmp.py compare them with
            Microsoft's DLLs)
tools/      RE helpers (pe.py, rtti.py), dmgt (drives Microsoft's DirectMusic offline), sty/ (style, chord map
            and segment test cases), dsfx/ and i3dl2/ (effect comparisons), hrtf_*.py, dsound-com-redirect*.reg
tracer/     dstrace, the DirectSound API tracer
dmtrace/    dmtrace, the DirectMusic API tracer (gen.py generates its wrappers from the MinGW headers)
docs/       STATUS.md (what's done, how it's tested), RE notes, vtables.json (every ALchemy class and vtable)
```

[`docs/STATUS.md`](docs/STATUS.md) describes how each part is tested against the originals.

### dstrace: DirectSound API tracer

`build/tracer/dsound.dll` sits in front of another `dsound.dll` and logs every call the game makes and what came back, including the raw values of every EAX property. This is how reference traces are taken from ALchemy, whose software engine has no call logging of its own.

```
game dir/
  dsound.dll            <- build/tracer/dsound.dll
  alchemy_dsound.dll    <- ALchemy's dsound.dll, renamed
  dstrace.ini           (optional) target=alchemy_dsound.dll  level=1|2
```

Run with `WINEDLLOVERRIDES=dsound=n,b`. The output is `dstrace.log` next to the game's exe. `level=2` adds the per-frame calls (Lock, Unlock, GetCurrentPosition, GetStatus, AddRef, Release).

### dmtrace: DirectMusic API tracer

Logs what a game asks DirectMusic to do (it's how reDM started). DirectMusic is a set of COM classes, so dmtrace is installed as their server for the current user instead of going into the game's folder:

1. Put `dmtrace.dll` somewhere permanent, such as `C:\dmtrace\`.
2. Run `regsvr32 C:\dmtrace\dmtrace.dll`. No administrator rights are needed: it only writes to `HKCU\Software\Classes\CLSID`, so Windows' own registration is untouched. (Under Wine, whose COM ignores per-user classes, it also redirects the machine-wide entries and keeps the originals to restore.)
3. Run the game, not as administrator. The log is `dmtrace.log` next to the game's exe.
4. `regsvr32 /u C:\dmtrace\dmtrace.dll` uninstalls it.

`dmtrace.ini` next to the DLL: `level=1` (the game's calls), `2` (also AddRef / Release, GetTime, GetParam and similar), `3` (also DirectMusic's internal calls); `target_dir=` loads the DirectMusic DLLs from another folder. `dmtest.exe` plays `test.mid` through DirectMusic as a smoke test.

---

## License

MIT, see [`LICENSE`](LICENSE). A personal project; ALchemy is © Creative Technology, and this repository contains no Creative or Microsoft code or binaries.
