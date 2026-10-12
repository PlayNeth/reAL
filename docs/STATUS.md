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

## Remaining, in the agreed order

1. Scripts (dmscript; needs an AudioVBScript interpreter)
2. Styles and chordmaps (dmstyle, dmcompos)
3. Audiopath configurations (.aud / CreateAudioPath) and environmental reverb
4. Tools and graphs: IDirectMusicGraph and IDirectMusicTool; application tools in the message pipeline.
   Research started:
   - Graph InsertTool 77302861, StampPMsg 77302c55, GetTool 77302b98, RemoveTool 77303408 (graph vtable 0x77301198).
   - The segment state's StampPMsg 77315fdd and the audiopath's 7731ef40 chain segment graph → audiopath graph →
     performance graph → output tool.
   - On the segment state, `GetObjectInPath` stages 0x1200/0x1300/0x3200/0x3300 still return NOT_FOUND
     (Microsoft creates the graph on demand).
5. DirectSound effects (SetFX)
6. Surround / CMSS
7. EAX 5

Smaller items still open:
- Audiopath stages for ports and mix-in buffers.
- IPersistStream::Load on standalone track objects.
- Track InitPlay/Play called directly by an application (returns E_NOTIMPL).

## Testing

- `tests/comcmp.sh comtest.exe` runs `comtest` under Microsoft's DirectMusic and under reAL-Music, then diffs
  the output. It currently gives 776 identical lines.
- `tools/dmgt` drives Microsoft's dmime/dmsynth offline and logs what reaches the port.
  `tests/test_perf.c` does the same for reAL's engine.
  - Switches: `DMGT_SMF`/`TP_SMF`, `DMGT_CLONE`/`TP_CLONE`, `DMGT_WAVE`, `DMGT_APATH`, `DMGT_BUSES` and others.
  - Run both and compare the logs event by event.
- The Microsoft DLLs (XP dm*, dswave, dsdmo; Windows 7 dsound), the Ghidra project and the reference
  captures lived outside this repository and are not part of the zip. Keep your own copies of the Microsoft
  DLLs you supplied.
- Known reference races (Microsoft varies from run to run):
  - ALMOSTEND notification times.
  - First-chunk tempo note-offs in a few segments, including t_temple.
  - One wave-voice allocation interleave in a cloned wave segment.
