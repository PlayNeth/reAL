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

## Remaining, in the agreed order

1. Scripts (dmscript; needs an AudioVBScript interpreter)
2. Styles and chordmaps (dmstyle, dmcompos)
3. Audiopath configurations (.aud / CreateAudioPath) and environmental reverb
4. DirectSound effects (SetFX)
5. Surround / CMSS
6. EAX 5

Smaller items still open:
- Audiopath stages for ports and mix-in buffers.
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
- The Microsoft DLLs (XP dm*, dswave, dsdmo; Windows 7 dsound), the Ghidra project and the reference
  captures lived outside this repository and are not part of the zip. Keep your own copies of the Microsoft
  DLLs you supplied.
- Setup used on 2026-10-12: Zig 0.17, Wine 9.22 in WoW64 mode (the cloud container has no 32-bit libraries), a
  null ALSA device (`pcm.!default { type null }` in `~/.asoundrc`, `Audio=alsa` in each prefix). The Microsoft
  prefix has the XP dm* DLLs and Windows 7 dsound in `syswow64`, but Wine's own dsdmo: the XP dsdmo needs
  msvcrt's `_libm_sse2_pow_precise`, which Wine lacks. With Wine's dsdmo, aptest's reverb DMO line differs
  (Microsoft's side reads garbage parameters).
- With the null ALSA device, reAL's clock (which follows its output buffer) runs about 2.5 times too fast, so
  timing tests such as notiftest don't compare in that setup. The COM tests and dmgt are unaffected.
- `dm_perf_free` used to destroy the playing segment states before freeing queued messages that still named them
  (a notification's state), which crashed notiftest at exit. It now frees the messages first.
- Known reference races (Microsoft varies from run to run):
  - ALMOSTEND notification times.
  - First-chunk tempo note-offs in a few segments, including t_temple.
  - One wave-voice allocation interleave in a cloned wave segment.
