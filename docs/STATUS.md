# reAL status and handoff notes

reAL is a personal-use reimplementation of Creative ALchemy's `dsound.dll`, reverse-engineered faithfully,
with the original's flaws fixed. reAL-Music is its own DirectMusic, built into the same DLL and selected
with `music=1` in `reAL.ini`. It is a faithful port of Windows XP's dmime, dmusic, dmsynth, dmband and dswave.

Ground rules carried through the project:
- Match Microsoft's behaviour exactly, and check it against Microsoft's own DLLs running under Wine.
- When the original has a bug, fix it behind a flag. Fixes are on in games and off in comparison tests.
  The flags are `DM_FIX_*` in `src/music/dmperf.h`, `MS_FIX_*` in `msynth.h`, plus `dm_wave_set_fix_length`
  and `dm_set_clone_fix`. Setting `REAL_MUSIC_NOFIXES` in the environment turns all of them off.
- For gm.dls, use the system's copy by default. `gmdls=` in `reAL.ini` points to a custom DLS or SoundFont,
  and Linux paths work under Wine.

## Done

**DirectSound side:** 2D/3D output, HRTF, EAX 1–4 (including ALchemy's reverb and the 11 EAX4 effects),
Doppler, and WASAPI output.

**DirectSound effects (SetFX)**
- `src/fx.c`: the chain SetFX builds, with Windows 7's validation, placement (LOCDEFER, AcquireResources),
  GetObjectInPath, the 32-frame pre-roll and Discontinuity at Play / SetCurrentPosition. `tests/fxtest.c` (sections
  api, params, data, seq, none) matches Windows 7's dsound line for line.
- `src/dsfx.c`: Gargle, Echo, ParamEq, Chorus, Flanger, Distortion, Compressor and Waves reverb, ported from
  Windows 11's dsdmo (10.0.26100.5074), bit-exact. XP's versions of these differ only in rounding, except XP's Waves
  (one frame earlier) and Distortion (about 2 dB quieter); Windows 11's are kept.
- `src/i3dl2.c`: the I3DL2 reverb, which Windows 11's dsdmo no longer has, from XP SP3's dsdmo (5.3.2600.5512).
  Its code is translated instruction for instruction by `tools/i3dl2/asm2c.py` (`tools/i3dl2/gen.sh` makes
  `src/i3dl2.inc`) and runs on a small emulated 32-bit machine; bit-exact for every format, quality and preset.
  Notes in `docs/i3dl2_notes.md`.
- Fixes (off with `REAL_NOFIXES` / `REAL_MUSIC_NOFIXES`): Echo keeps a PanDelay set before Play
  (`DSFX_FIX_ECHO_PANDELAY`); I3DL2 runs at the quality GetQuality reports (2) until SetQuality, where XP runs at
  half rate (`DSFX_FIX_I3DL2_QUALITY`).

**DirectMusic: playback**
- DLS synth (dmsynth port), including wave voices and streaming.
- Segments with sequence, band, tempo, time signature, SysEx and wave tracks.
- MIDI files, and SoundFont banks converted to DLS.
- MIDI routed through winmm (midiOut, midiStream and MCI).
- The Waves reverb.

**DirectMusic: notifications and audiopaths**
- Notifications.
- Standard audiopaths: shared stereo + reverb, dynamic 3D, mono and stereo.
- The 3D path's Doppler and pitch, matching Windows dsound.

**DirectMusic: audiopath configurations** (`src/music/dmapcfg.c`, `dmcom.c`'s cfg_path, `dmhost.c`)
- The configuration object (CLSID_DirectMusicAudioPathConfig, the 'DMAP' form: descriptor, top-level buffers, port
  configurations with their buffers and PChannel maps, the tool graph) loaded by the loader or IPersistStream, with
  the DSBC buffer descriptions as dsound loads them (effects made at load, 'data' through IPersistStream).
- CreateAudioPath as dmime builds it (77320f10): top-level buffers first, then each port's, shared buffers found
  again by GUID (the standard path's stereo and reverb too), the maps' buffers looked up in the port, the path, the
  other ports and the performance, PChannels from blocks of 16 (drums on 10 honoured, freed with the path) on the
  map's buffers' buses when the port has the audiopath feature.
- The defined buffers (Reverb with Waves, EnvReverb mix-in with I3DL2, 3D dry, mono, stereo) and DSBC ones, playing
  in the sink: effects cloned from the configuration and run as each block is written (no JIT, no Discontinuity, as
  dsound's sink), sends (GUID_DSFX_SEND, its IDirectSoundFXSend object, dsound's fixed-point mixing and millibel
  table) into mix-in buffers, which are written after the buffers that send to them.
- GetObjectInPath for configured paths: buffers (dwBuffer across the maps), their effects, mix-ins (PChannel 0 only),
  the primary buffer; ConvertPChannel as dmime (broadcast values pass, others DMUS_E_NOT_FOUND).
- `tests/cfgcmp.sh` runs `tests/cfgtest.c`'s configurations one by one against Microsoft's: all match, the port and
  sink stages included.

**DirectMusic: port objects** (`src/music/dmport.c`)
- The synth port, as dmusic's DirectX 8 software synth port: IDirectMusicPort, IDirectMusicPortDownload and
  IKsControl. It is what PChannelInfo, the audiopath port stage (DMUS_PATH_PORT, by PChannel as dmime converts it)
  and the configuration's ports return.
- GetCaps (Microsoft Synthesizer, flags 0xe85), GetNumChannelGroups (the groups the performance uses), channel
  priorities (dmsynth's defaults: channel 9 0x8000000f, then 0x8000000e down the others), GetFormat, the KS properties
  (GM/GS/XG, DLS1/DLS2, sample memory, playback rate, volume and effects sets), GetRunningStats, PlayBuffer, Read,
  and the methods Microsoft leaves E_NOTIMPL.
- GetRunningStats is dmsynth's (7748fcd4, 77485a00): average voices, lost notes (preallocation steals and note-ons no
  voice started for), CPU, and the output's peak in dB, published once per second of samples, with dmsynth's MMX peak
  quirk (the first sample of each four is not checked for the minimum). `DMGT_STATS` (dmgt) and `MS_STATS`
  (test_msynth) print them for an offline comparison; they match on both render paths.
- The sink stage (0x5000): an object with IReferenceClock (the master clock; GetLatencyClock's gives the latency
  clock) and IKsControl. Microsoft's also answers its private IDirectSoundSynthSink, which reAL-Music does not offer.
- `tests/comcmp.sh porttest.exe` matches but for that interface (`COMCMP_DIR=/tmp/real/sp/port`).

**DirectMusic: COM methods**
- GetParam, SetParam and GetParamEx.
- SetTrackConfig, SetPChannelsUsed and SendPMsg.
- GetSegmentState, TimeToRhythm, AdjustTime, ClonePMsg and PChannelInfo.
- Track objects: GetTrack, InsertTrack, RemoveTrack, and track and segment Clone.
- SegmentState::GetObjectInPath.

**DirectMusic: tool graphs and application tools**
- The graph object (CLSID_DirectMusicGraph, `src/music/dmgraph.c`, from dmime's CGraph): InsertTool, GetTool,
  RemoveTool (a removed tool stays referenced until the graph goes), StampPMsg, the descriptor, IPersistStream::Load
  of a 'DMTG' form, and the private Clone (IDirectMusicTool8 tools are cloned, others shared).
- Graphs on the performance (Get/SetGraph), segments (Get/SetGraph, shared by Clone, loaded from a segment file's
  'DMTG' form), segment states (a clone of the segment's, made at play) and audiopaths.
- The performance, segment states and audiopaths answer QueryInterface for IDirectMusicGraph, and their StampPMsg
  chains the graphs: state graph → audiopath graph (virtual PChannels) → performance graph → output tool. The
  performance also answers IDirectMusicTool (the output tool).
- GetObjectInPath stages 0x1200/0x1300 (segment graph and tools), 0x2200/0x2300 (audiopath) and 0x3200/0x3300
  (performance), with the graphs made on demand where Microsoft makes them.
- Application tools in the message pipeline: ProcessPMsg with DMUS_S_REQUEUE / DMUS_S_FREE / S_OK, the delivery
  types (immediate, queue, at time), Flush on invalidation, DMUS_PMSGT_DIRTY at a segment's start, end and abort, and
  DMUS_PCHANNEL_BROADCAST_SEGMENT.
- How it fits together: an engine message the application sees gets an application view (`dm_pmsg.ext`, a
  DMUS_PMSG with a private header). The view and the engine message are synced at every hand-over (pm_to_com /
  pm_from_com in dmcom.c). The engine calls back into the COM layer through `dm_toolops`. Messages for application
  tools wait in a queue that the host threads drain after each pass, with the engine lock released around each
  ProcessPMsg call, as Microsoft calls tools without its locks.

**DirectMusic: styles** (`src/music/dmstyload.c`, `dmstyplay.c`, `dmcomsty.c`, from dmstyle)
- Style files ('DMST'): parts, patterns, motifs, bands, markers, DirectX 7 parts. IDirectMusicStyle8 (bands,
  motifs, patterns, time signature, tempo, embellishment lengths) and IDirectMusicBand (CreateSegment, Download,
  Unload) through the loader.
- The style track: Microsoft's pattern chooser (groove and embellishment commands, rhythm, destination groove),
  variation choice in every mode, markers and chord alignment, the chord mapping (play modes, inversions, subchords),
  ranges, curves, chase, and measure-and-beat notifications. It supports several styles, time signature changes,
  loops, transitions, stops, and control and secondary segments.
- The motif track: IDirectMusicStyle::GetMotif segments, with beat and marker alignment and the motif's band.
- The pattern track ('DMPT'): load, play (no inversions), and Clone by measures.
- The mute track on style, sequence and wave tracks. Band objects as track parameters (GUID_IDirectMusicBand,
  GUID_BandParam).
- rand() is the one Microsoft's code uses. The style and motif tracks reseed it from time() when they are made or
  cloned; the pattern track never does. It plays the same as Microsoft's when time() is fixed (`DMGT_TIME`).
- `tools/sty/cases.py` writes the cases. `tools/sty/cap.sh <case>` compares dmgt with test_perf; every case matches
  event for event. `tests/comcmp.sh styletest.exe` gives 927 lines and `stylecom.exe` gives 186, all identical.
- Open: IDirectMusicPatternTrack (audition, used by Producer); GUID_Variations with play state and dmstyle's private
  GetParams; chord regeneration (DMUS_SEGF... regenerate flags); the style's chordmap methods (stubs until dmcompos).

## Remaining, in the agreed order

1. Scripts (dmscript; needs an AudioVBScript interpreter)
2. Chordmaps and the composer (dmcompos); styles are done but for the open items above
3. Surround / CMSS
4. EAX 5

Smaller items still open:
- The sink object does not answer IDirectSoundSynthSink (Microsoft's private interface between dmusic and dsound).
- Standard audiopaths still use the host's fixed buffers rather than the configuration machinery (they match
  Microsoft's in every test so far).
- Tool graphs: a segment file's 'DMTG' form is only tested on the failure path (an unregistered tool fails the
  segment's load, as on Microsoft's). A working case needs a tool class registered in both Wine prefixes.
- reAL treats every application as DX8 (`dxver` 8): a DX7 application calling Get/SetGraph before Init gets
  DMUS_E_NOT_INIT, which Microsoft returns only to DX8 applications.
- IPersistStream::Load on standalone track objects.
- Track InitPlay/Play called directly by an application (returns E_NOTIMPL).

## Testing

- `tests/comcmp.sh comtest.exe` runs `comtest` under Microsoft's DirectMusic and under reAL-Music, then diffs
  the output. It currently gives 776 identical lines.
- `tests/comcmp.sh tooltest.exe` does the same for graphs and tools (the graph object, every owner's graph,
  GetObjectInPath stages, the messages tools receive while bells1 and fe_hard_04 play, Flush, loading).
  It gives 255 identical lines.
- `comcmp.sh` uses `/tmp/real/sp/com` (`COMCMP_DIR`), the Microsoft prefix `/tmp/real/sp/msprefix` and
  the reAL prefix `/tmp/real/sp/realprefix` (`REAL_PREFIX`).
- `tools/dmgt` drives Microsoft's dmime/dmsynth offline and logs what reaches the port.
  `tests/test_perf.c` does the same for reAL's engine.
  - Switches: `DMGT_SMF`/`TP_SMF`, `DMGT_CLONE`/`TP_CLONE`, `DMGT_WAVE`, `DMGT_APATH`, `DMGT_BUSES` and others.
  - Run both and compare the logs event by event.
- `tools/dsfx/cmp.sh <effect> <rate> <ch> <bits> <chunk> [params]` runs a dsdmo effect (dmgt dmo) and reAL's
  port (build/test_dsfx) on the same signal and compares samples and parameters. `DMDIR='Z:\home\user\re\dmxp'`
  picks XP's dsdmo (needed for i3dl2); `DMO_I3DL2_QUALITY` / `DMO_I3DL2_PRESET` set those first. dmgt's
  `DMO_DUMP`, `DMO_FRAME` and `DMO_PROBE` (a hook that saves the stack at an address) with test_dsfx's `DSFX_DUMP`
  are how the I3DL2 translation was checked against the original.
- `tests/comcmp.sh fxtest.exe <section>` with `FXPROBE_LOG=fxlog.txt FXPROBE_DATA=fxdata.raw FXPROBE_VERBOSE=1`
  compares SetFX against Windows 7 (`FXTEST_ONLY` / `FXTEST_SIZES` narrow the data section to one run).
- The Microsoft DLLs (XP dm*, dswave, dsdmo; Windows 7 dsound), the Ghidra project and the reference
  captures lived outside this repository and are not part of the zip. Keep your own copies of the Microsoft
  DLLs you supplied.
- Setup used on 2026-10-12: Zig 0.17, Wine 9.22 in WoW64 mode (the cloud container has no 32-bit libraries), a
  null ALSA device (`pcm.!default { type null }` in `~/.asoundrc`, `Audio=alsa` in each prefix). The Microsoft
  prefix has the XP dm* DLLs and Windows 7 dsound in `syswow64`, and Windows 11's dsdmo (`re/dm/dsdmo.dll`,
  10.0.26100.5074, not XP's), patched by `tools/dsdmo/patch_dsdmo.py` to import `msvcrx.dll` (a proxy supplying
  msvcrt's `_libm_sse2_pow_precise` and relatives, which Wine lacks). The I3DL2 class is registered (32-bit view)
  to XP's dsdmo copied as `syswow64/dsdmoxp.dll` (`re/dmxp/dsdmo.dll`, 5.3.2600.5512).
- With the null ALSA device, reAL's clock (which follows its output buffer) runs about 2.5 times too fast, so
  timing tests such as notiftest don't compare in that setup. The COM tests and dmgt are unaffected.
- `dm_perf_free` used to destroy the playing segment states before freeing queued messages that still named them
  (a notification's state), which crashed notiftest at exit. It now frees the messages first.
- Known reference races (Microsoft varies from run to run):
  - ALMOSTEND notification times.
  - First-chunk tempo note-offs in a few segments, including t_temple.
  - One wave-voice allocation interleave in a cloned wave segment.
