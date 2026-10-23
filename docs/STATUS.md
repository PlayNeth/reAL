# reAL status and handoff notes

reAL is a personal-use reimplementation of Creative ALchemy's `dsound.dll`, reverse-engineered faithfully,
with the original's flaws fixed. reDM is its own DirectMusic, built into the same DLL and selected
with `music=1` in `reAL.ini`. It is a faithful port of Windows XP's dmime, dmusic, dmsynth, dmband and dswave.

Ground rules carried through the project:
- Match Microsoft's behaviour exactly, and check it against Microsoft's own DLLs running under Wine.
- When the original has a bug, fix it behind a flag. Fixes are on in games and off in comparison tests.
  The flags are `DM_FIX_*` in `src/music/dmperf.h`, `MS_FIX_*` in `msynth.h`, plus `dm_wave_set_fix_length`
  and `dm_set_clone_fix`. Setting `REDM_NOFIXES` in the environment turns all of them off.
- For gm.dls, use the system's copy by default. `gmdls=` in `reAL.ini` points to a custom DLS or SoundFont,
  and Linux paths work under Wine.

## Done

**DirectSound side:** 2D/3D output, HRTF, EAX 1–4 (including ALchemy's reverb and the 11 EAX4 effects),
Doppler, and WASAPI output.

**Inaudible voices** (`src/mixcore.c`):
- A voice is idle when its gains and every effect send are exactly 0, current and target, and no crossfade is
  running. That covers DSBVOLUME_MIN, a 3D source muted at maximum distance, and an EAX path at -10000 mB.
- An idle voice only moves its position. Loop wraps, the end of a one-shot and notifications are the same as a
  full mix; no samples are read.
- HRTF: once its history is all silence it is skipped; its output is exactly zero then.
- CMSS-3D and its virtualizer stop after a second of silence. The upmixer keeps its block timing
  (`rl_cmss3_skip`).
- Sources whose LFE goes through the bass limiter are never idle: the limiter follows the source at any level.
- On resume, the EAX direct-path HF filter, which a full mix runs at any gain, is replayed over the frames it
  missed.
- `tests/test_idle.c` mixes random voices both ways. The output is bit-identical except:
  - the HF filter replay (within 1e-6), unless the position jumped while the voice was idle;
  - CMSS after a second of silence (its analysis state is held).
- Cost: 64 sinc voices with 32 muted went from about 9% to 5.5% of one core; a muted voice now costs almost
  nothing.

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
- Fixes (off with `REAL_NOFIXES` / `REDM_NOFIXES`): Echo keeps a PanDelay set before Play
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
  clock) and IKsControl. Microsoft's also answers its private IDirectSoundSynthSink, which reDM does not offer.
- IDirectMusicPortDownload for an application's own downloads (Pro Evolution Soccer 6 builds its instruments that
  way): AllocateBuffer and the buffer object, GetDLId from the port's counter (the one its collections use),
  Download straight to the synth, GetBuffer by id, Unload, with dmusic's results (E_POINTER for a buffer too small
  for its header, DMUS_E_UNKNOWNDOWNLOAD for a type there is none of, an instrument without articulation refused,
  Unload of a buffer not downloaded S_OK). It used to be stubbed with E_NOTIMPL, and the game crashed on the buffer
  it never got.
- `tests/comcmp.sh porttest.exe` matches but for that interface (`COMCMP_DIR=/tmp/real/sp/port`); its `download`
  and `inst` sections match in full.
- Instruments one at a time, the DirectX 6/7 way: Collection::GetInstrument hands out an IDirectMusicInstrument
  (GetPatch / SetPatch, any value; the same object while it lives; DMUS_E_INVALIDPATCH for a patch the collection
  lacks), Port::DownloadInstrument downloads it with the engine's DLS code (key ranges included) as an
  IDirectMusicDownloadedInstrument (the same object, counted, for the same instrument and patch without ranges;
  UnloadInstrument S_FALSE until the last, then DMUS_E_NOT_DOWNLOADED_TO_PORT), and Performance::DownloadInstrument
  through the PChannel's port, with its group and channel (porttest `inst`).
- IDirectMusic::CreatePort: a synth port of its own each time, as dmusic's (a host of its own: synth and sink, freed
  with the port; it outlives the performance). The parameters as dmusic and its synth settle them: voices 1-1000
  (32), channel groups (the port keeps what was asked, the synth at most 1000; 1 when not given, 2 reported back),
  audio channels 1-2, rate 11025-96000 (22050), reverb only, no sharing; each one brought into range makes it
  S_FALSE, except that dwFeatures given makes it S_OK; dwValidParams comes back with them all; DX7's smaller
  structure works; E_NOINTERFACE for a port GUID that is not the synth's (dx6test `port`).
- AddPort / RemovePort / AssignPChannelBlock / AssignPChannel / PChannelInfo with several ports: Init's performance
  has no port added (AddPort(NULL) adds its own and assigns PChannels 0-15 when there are none); AddPort of a port
  does not assign PChannels; a port not added is E_INVALIDARG; a group beyond the port's channel groups is assigned
  and answered S_FALSE, group 0 accepted; after RemovePort the PChannels stay, to no port, and play nothing. A
  CreatePort'ed port joins the engine's ports (at most 8 per performance, a removed one keeping its place).
- CreateMusicBuffer and IDirectMusicBuffer as dmusic's (dx6test `buffer`): the size rounded up to 8, events in time
  order (after those at the same time; an earlier one moves the start time), DMUS_EVENTHEADER layout, status bytes
  dmusic packs (F0, F4, F5, F7 and running status refused), E_OUTOFMEMORY when full, DMUS_E_BUFFER_EMPTY,
  SetUsedBytes beyond the size DMUS_E_BUFFER_FULL. PlayBuffer plays each event at its time (the start time plus
  its offset, the master clock's time; one already past at once) on its channel group (0: all), and answers
  DMUS_E_SYNTHINACTIVE until Activate(TRUE). Microsoft's sink under Wine never lets its voices go, so dx6test
  `play` compares only what sounds; `DX6TEST_TIMES=1` prints when each note starts.
- The master clock's and the latency clock's AdviseTime / AdvisePeriodic / Unadvise are E_NOTIMPL in Microsoft's
  DLLs too (the master clock: E_POINTER for no cookie); reDM answers the same (dx6test `clock`).

**DirectMusic: COM methods**
- GetParam, SetParam and GetParamEx.
- SetTrackConfig, SetPChannelsUsed and SendPMsg.
- GetSegmentState, TimeToRhythm, AdjustTime, ClonePMsg and PChannelInfo.
- Start times as dmime resolves them (7730959f / 7730ae6a): grid, beat and measure, DMUS_SEGF_MARKER (the next play
  marker) and SEGMENTEND, and DMUS_SEGF_ALIGN with the VALID_START flags: the segment comes in partway at its first
  valid start (its marker track), else at the next measure, beat or grid, its start point moved on (GetStartPoint).
  GetResolvedTime takes the same flags. `comptest.exe align` matches.
- Track objects: GetTrack, InsertTrack, RemoveTrack, and track and segment Clone.
- SegmentState::GetObjectInPath.

**DirectMusic: lyrics and segment trigger tracks** (`src/music/dmseg.c`, `dmtrack.c`, from dmime)
- The lyrics track ('lyrt'): DMUS_LYRIC_PMSGs at each lyric's physical time, chosen by logical time, through the
  state's graph; Clone; a header asking for reference time or a tool's queue fails the load.
- The segment trigger track ('segt'): each event plays its segment (or a style's motif, by name) with its flags on
  the state's audiopath; SetParam passes parameters to every event's segment (ten deep at most); IsParamSupported
  answers S_OK for every type; Clone. A reference that does not load fails the track.
- A track whose Load fails is left out of the segment, which loads with DMUS_S_PARTIALLOAD.
- `tests/comcmp.sh trktest.exe lyrics` and `trigger` (`cases.py tracks`) match.

**DirectMusic: parameter control track** (`src/music/dmseg.c`, `dmtrack.c`, from dmime 77310eae)
- Load ('prmt': 'prol' objects with 'proh', 'prpl' parameters with 'prph' and 'prcc' curves; a time format other
  than music or reference time fails it, a parameter without curves is left out); both lists kept newest first,
  as dmime keeps them. Clone keeps the curves from the first one not over before the start.
- InitPlay asks the state's path for each object's IMediaParams (GetObjectInPath) and sets its time format;
  an object that refuses it (dsdmo's effects take reference time only) gets nothing.
- Play sends each curve starting in the range as one AddEnvelope segment (music time converted to reference
  time for that format). On a seek, loop, start or flush it starts again from the first curve at or after the
  position; what is left of the curve before is sent linear from its current value, whatever its shape, or as a
  jump to its end once over (not on a loop). DMUS_S_END once no curves are left.
- EndPlay flushes, for each parameter that was sent curves, what ended before the last one's start; dmime counts
  the parameters from 0 again for each object, so only the first object's states are looked at (kept).
  GetParam/SetParam/IsParamSupported: unsupported.
- `FXPROBE_LOG=probe.txt tests/comcmp.sh trktest.exe param` (`cases.py paramctl`) matches: the envelopes dmime
  gives the probe effect (tests/fxprobe.c now has IMediaParams) for plays from the start, from a start point in a
  curve, with a loop, and of a clone. dmime spins forever on a clone shorter than its loop points; the test
  avoids that.
- reAL's standard effects (`src/dsfx.c`, `src/fx.c`) have IMediaParams and IMediaParamInfo as dsdmo's CParams:
  AddEnvelope, FlushEnvelope, SetParam (which turns the newest segment into a jump), GetParam (the newest
  segment's end value), reference time only; Process works in 3 ms quanta and follows the envelopes to each
  quantum's time stamp; Clone copies the envelopes. The sink gives its buffers' effects the reference time of
  each piece it mixes. `tools/dsfx/cmp.sh` with `DMO_ENV`, `DMO_SETPARAM`, `DMO_FLUSH`, `DMO_T0` and `DMO_INFO`
  compares envelopes, curves and parameter descriptions with dsdmo.dll: all match.
- Found on the way: CloseDown now shuts down every segment state and frees every queued message, and a state
  lets its audiopath go when it leaves the performance, as dmime does (an application keeping a state no longer
  keeps the audiopath's buffers and effects); a race in PlaySegment could leave a second state object holding
  the audiopath.

**DirectMusic: the loader and containers** (`src/music/dmldr.c`, from dmloader's CLoader and CContainer)
- The loader keeps a list of classes, each with its entries (a descriptor, the object once loaded, the garbage
  collection links). GetObject finds an entry by object GUID, memory, stream, full path, name and category, name,
  file name, then the object itself (a full path is never matched against a relative file name, as in dmloader),
  creates the object by class, loads it with IPersistStream::Load from a stream that hands out the loader
  (IDirectMusicGetLoader), and merges what the object says of itself into the entry, then tells the object.
  SetObject, SetSearchDirectory, ScanDirectory (no cache file), CacheObject, ReleaseObject, ReleaseObjectByUnknown,
  ClearCache, EnableCache, EnumObject, CollectGarbage, LoadObjectFromFile, and the private IDirectMusicLoader8P
  (dynamic references, the streams' references) as dmloader has them; the default GM collection is set up at
  creation. File streams read on demand (no Stat, a short read fails), so a large gm.dls or SoundFont is opened
  through the process-wide collection cache.
- Every object loads from the stream it is given, and its references go through the stream's loader: bands'
  collections, wave, style, chord map and segment trigger tracks, styles' chord maps. Collections are now objects
  of their own (CLSID_DirectMusicCollection with IPersistStream). Segments, collections, bands and styles have
  Get/Set/ParseDescriptor as their DLLs do (segments parse 'DMSG'/'AASE' forms, waves and MIDI files).
- The container (CLSID_DirectMusicContainer): Load of a 'DMCN' form gives the loader each item (embedded or a
  reference) with SetObject, then loads them unless DMUS_CONTAINER_NOLOADS; EnumObject with aliases;
  Get/Set/ParseDescriptor; when it goes it releases its items from the loader (dmloader never keeps the KEEP
  flag, so every item is released). A segment's embedded container is loaded through the loader while the
  segment loads and let go of afterwards (dmime 77313c61).
- A let-go-of entry stays in its class while it has a place to load from or a name (dmloader's 0xc14 mask).
- Locks (reAL's own rules, as reDM has one performance lock where dmime has several):
  - The loader calls nothing with its lock held except the Load in GetObject, as dmloader does. Releases and
    Zombies of objects it lets go of wait until its lock is let go. GetDescriptor in ReleaseObject and the
    dynamic-reference methods is asked before the lock is taken.
  - Nothing calls the loader, or waits for a thread that may, while holding a performance's lock:
    - track and segment Loads parse without it and take it only to put the new data in place, or to sort a
      shared style again;
    - a script's first Init runs with it let go;
    - COM objects the engine releases under it (script track scripts, messages' tools, graphs and user objects,
      effects' IMediaParams, audiopaths) are released once this thread's outermost hold ends
      (`dm_host_release_later`).
  - A script's Zombie takes the script's lock, so it waits for a routine running on another thread.
- A track loaded again while it plays: the states playing it end their state on the old data and start a new
  one on the new data, which seeks to where they are (dmime's tracks re-seek when their validate count changes).
  Before, the old data was freed under them.
- Collections are reference counted (dls_addref / dls_release). Holders are the COM object, each band
  instrument and each port download (a port's per-collection state is keyed on a serial, not the address).
  Collections loaded from a stream are freed with their last holder; files stay in the process-wide cache.
- `tests/comcmp.sh trktest.exe container` and `loader` (`cases.py container` and `tracks`) match: partial loads,
  descriptors and flags of every item, lookups by GUID, name, alias and file, NOLOADS, embedded containers,
  ReleaseObject of a container from the cache, ScanDirectory, SetObject, a disabled cache, EnumObject of all types.

**DirectMusic: scripts, stages 1 and 2** (the whole AudioVBScript interpreter came with stage 1) (`src/music/dmscript.c`, `src/music/dmavs.c`, from dmscript.dll)
- The script object (CLSID_DirectMusicScript): IPersistStream::Load of a 'DMSC' form ('schd' flags, descriptor
  chunks up to 'scve', the embedded container loaded through the stream's loader, 'scla', 'scsr'); Init, CallRoutine,
  Set/GetVariableVariant/Number/Object, EnumRoutine/EnumVariable, Get/Set/ParseDescriptor (ParseDescriptor writes
  the script's own descriptor first, as dmscript's does), IDirectMusicObjectP's Zombie, and IDispatch for other
  scripts (routines, variables and "Load"). Error information (DMUS_SCRIPT_ERRORINFO) is filled as dmscript does
  it: the "file»line»description" split, 1-based line and column, the script's file name when the error names none,
  only as much as the caller's dwSize holds. A syntax error lets Load succeed and makes Init fail with it.
- The language comes from the registry as in dmscript (HKCR\<language> with a DMScript key; OLEScript engines
  are stage 4); AudioVBScript is reDM's own engine and works without the registry entry. An unknown language
  fails Load with DMUS_E_SCRIPT_LANGUAGE_INCOMPATIBLE, a file without source with DMUS_E_SCRIPT_INVALID_FILE.
- AudioVBScript (dmavs.c, CAudioVBScriptEngine): the lexer, the parser with its 26 messages, the shunting-yard
  expression compiler and the executor over a stack of VARIANTs: Dim, Sub, Set, Call, If/ElseIf/Else, integer
  arithmetic (wrapping), comparisons as -1/0, bitwise And/Or/Not, Is on objects, implicit routine variables,
  dotted property chains and method calls, the runtime messages, and dmscript's own variant conversions and copies
  (no arrays or references; Let/Set checking). Quirks kept: "sub_x" lexes as Sub; an If condition's own error is
  replaced by the If's type mismatch; a VT_I2 or VT_R8 set from the application is kept as is (and is then not a
  number to the script).
- The "DirectMusic" item: the constants (IsSecondary ... NoCutoff), the performance's methods, the content by
  alias. The content (the container's objects) are proxies loaded with the script (schd 1), downloaded at Init
  (schd 1+2), or on the script's "x.Load" through the loader's dynamic references; a nested script is Init'ed.
- The performance's IDispatch (CAutDirectMusicPerformance, aggregated, made on first QueryInterface):
  SetMasterTempo/GetMasterTempo, SetMasterVolume (a CC7 curve message to every channel, the volume table of
  dmscript) / GetMasterVolume, SetMasterGrooveLevel/Get, SetMasterTranspose (a transpose message)/Get, Trace (a
  script lyric message) and Rand, with the argument checks and "An error occurred in a call to X". The segment's
  IDispatch has only Load so far (Play and the rest are stage 3).
- Found on the way: the performance's global parameters are now dmime's list (any GUID, its size fixed by the
  first set; E_INVALIDARG when not set), InitAudio sets automatic download off as dmime does, and
  GUID_PerfMasterTempo now changes the tempo map (it was ignored).
- `tests/comcmp.sh scrtest.exe <dir> lang` (884 lines: 55 syntax cases, a 70-routine script, the variable calls,
  a script driving another, the messages the performance methods send, global parameters) and `files`
  (`cases.py script`: content loaded in each way, a missing item, errors with the file name) match, except a
  VBScript file (stage 4).
- dmscript loses a syntax error inside a call's arguments in an expression (its error code is overwritten) and
  then crashes running the routine; reDM reports the error. GetVariableObject of a variable holding Nothing
  crashes in dmscript as in reDM. Script source by reference (a DMRF list) is not supported.
  (`files` matched in full once stage 4 brought VBScript.)

**DirectMusic: scripts, stage 3** (`src/music/dmscript.c`: dmscript's CAutDirectMusic* classes)
- IDispatch on the segment (Load, Play, Stop, DownloadSoundData, UnloadSoundData, Recompose), the segment state
  (IsPlaying, Stop), the audiopath (SetVolume, GetVolume) and the audiopath configuration (Load, Create), each an
  aggregated object made on the first QueryInterface, with dmscript's method tables (argument types, optional
  arguments and their interfaces) and its translation of the script flags into segment flags (AtMeasure ->
  DMUS_SEGF_MEASURE, AlignToBar -> ALIGN|VALID_START_BEAT|MEASURE, NoCutoff -> NOINVALIDATE, AtImmediate clears
  DEFAULT ...), composition commands and composition flags (PlayFill/Intro/Break/End/EndAndIntro go through the
  composer's AutoTransition, as does Stop PlayEnd). Play on an audiopath of another performance remembers it so
  Stop stops it there; a segment state's Stop goes to the performance it plays on. The methods act on the running
  script's performance and composer.
- Found on the way: PlaySegmentEx with DMUS_SEGF_AUTOTRANSITION and a template composes the transition (the
  template's Compose from the segment playing then) and plays it, then the segment from its end, as dmime 7730d51c
  does (the transition template was ignored). A segment state the application meets without having played it (a
  composed transition's, through GetSegmentState) now has a segment object for GetSegment.
- dmscript's parser keeps going after an error inside a call's arguments within an expression (its error code is
  overwritten when the call is pushed) and reports the last error it met, or none; reDM does the same now, and runs
  such a script's call as nothing instead of crashing.
- `tests/comcmp.sh scrtest.exe <dir> auto` (`cases.py scriptauto`: a script with two segments and an audiopath
  configuration in its container) matches: every method, the flag combinations, auto-transitions, an audiopath's
  volume curves, the errors. Its playing checks are made only after immediate plays and stops, as reAL's clock
  runs fast with the null ALSA device.

**DirectMusic: scripts, stage 4** (`src/music/dmscript.c`, `dmseg.c`, `dmtrack.c`, `dmcom.c`)
- VBScript, JScript and any other Active Scripting language a script names: dmscript's host (the site,
  IObjectSafety, the "DirectMusic" item added with its members global, the script's errors through
  OnScriptError, the latest kept), run on one thread of its own that has COM initialized, as dmscript does,
  with each call made from there (BSTRs and IID_NULL for those engines).
- The script track (CLSID_DirectMusicScriptTrack): 'scrt' / 'scrl' / 'scre' events ('scrh' header, the script
  by reference through the stream's loader, 'scrn' the routine; any of them missing fails with
  DMUS_E_INVALID_SCRIPTTRACK, a missing script with the loader's error), sorted by logical time. InitPlay Inits
  each script on the performance; Play sends each event in [start, end) as a DMUS_PMSGT_USER message to the
  track's tool (the event's flags giving TOOL_IMMEDIATE / QUEUE / ATTIME, a copy of the event as its user object;
  none on a flush), and the tool's ProcessPMsg calls the routine with the event's time as what the script plays
  starts from. SetParam of 1cc7e0bf-981c-4b9f-be17-d572fc5fa933 (the only parameter it knows) makes script errors
  messages of type 15000 (the DMUS_SCRIPT_ERRORINFO after the header, stamped by the state's graph). Clone takes
  any range and leaves that setting out. A script whose engine is not started yet is Init'ed with the performance
  let go, as its code may call it.
- dmscript crashes sending a routine's runtime error (0040416a takes the performance from the host's reference
  count); reDM sends it with the script's performance.
- `tests/comcmp.sh scrtest.exe <dir> ole` (VBScript and JScript, 152 lines) and `track` (`cases.py scripttrack`:
  AudioVBScript and VBScript scripts on a track, every event flag, a clone, an Init error as a message, the load
  errors) match; `files` now matches with its VBScript file.

**DirectMusic: track objects on their own** (`src/music/dmseg.c`, `dmtrack.c`, `dmcom.c`)
- CoCreateInstance of every track class reDM has (sequence, tempo, time signature, SysEx, band, wave, chord,
  command, style, motif, pattern, mute, chord map, signpost, marker, lyrics, segment trigger, parameter control)
  gives the track with no items, as each class's constructor leaves it (the style and motif tracks seed the
  application thread's rand(), as dmstyle's constructors do).
- IPersistStream::Load reads the track's own chunk ('seqt', LIST 'lyrt', RIFF 'DMPT', ...) and resolves its
  references through the stream's loader (IDirectMusicGetLoader). Without one, the segment trigger track fails
  before reading (E_NOINTERFACE), the style and chord map tracks after reading (E_NOINTERFACE), the band track with
  0x88781150, and the wave track at its first item's reference, as their DLLs do.
- InitPlay / Play / PlayEx / EndPlay on a state and performance of the application's choosing: the engine keeps the
  track's state as a segment state would, with every group bit and on the application thread's rand(). The chord
  map, signpost, mute and marker tracks hand out no state; a style or pattern track without a style fails with
  E_POINTER, a motif track without a motif with E_FAIL; the wave track fails with DMUS_E_NOT_INIT when an item's
  wave is not downloaded to its port (and, as dmime's, keeps the track: its state is never ended).
- A segment made with CoCreateInstance has a length of 1 and no tracks, and plays (it used to refuse).
- Found on the way: the performance's time signature when no track gives one is the newest one in its time
  signature list (dmime 77304559), not 4/4 from time 0, so a segment without a time signature track aligns
  secondary segments to the beat of the one before it.
- `tests/comcmp.sh trktest.exe standalone lyr.sgt basic.sgt bells1.sgt pctl.sgt c1.sgt params.sgt aprim.sgt
  wavemute.sgt` matches: every class made empty (interfaces, class ID, supported parameters, GetParam, Clone,
  direct play), and every track of those files loaded with and without a loader, played in a new segment and
  directly. Two differences are left: a pattern track playing alone in a segment (no chord track) and a motif
  triggered by a segment trigger track the application plays directly call rand() for some notes in a different
  order than dmstyle does, so a note or two lands a few ticks off.

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
- The pattern track ('DMPT'): load, play (no inversions), and Clone by measures. IDirectMusicPatternTrack:
  CreateSegment, SetVariation (the audition, also through a part's variation lock) and SetPatternByName;
  GUID_Variations while playing and dmstyle's private 689821f4 (the track's style) and 09f25101.
- The mute track on style, sequence and wave tracks. Band objects as track parameters (GUID_IDirectMusicBand,
  GUID_BandParam).
- rand() is the one Microsoft's code uses. The style and motif tracks reseed it from time() when they are made or
  cloned; the pattern track never does. It plays the same as Microsoft's when time() is fixed (`DMGT_TIME`).
- `tools/sty/cases.py` writes the cases. `tools/sty/cap.sh <case>` compares dmgt with test_perf; every case matches
  event for event. `tests/comcmp.sh styletest.exe` gives 927 lines and `stylecom.exe` gives 186, all identical.
- Chord changes reach notes already queued (77309f00): DMUS_NOTEF_REGENERATE notes are played on in the new chord
  when it makes another note, NOINVALIDATE_INSCALE / _INCHORD notes are cut when they no longer fit; a note waiting
  to start is shortened to the chord, a sounding one ended through its tool (`cap.sh regen 20000` matches).
- Open: dmstyle's 09f25101 writes a part's variations to the address its PChannel makes (reDM doesn't).

**DirectMusic: chord maps and the composer** (`src/music/dmcompose.c`, `dmcomcmp.c`, `dmtheory.c`, from dmcompos)
- Chord map files ('DMPR'): the chord palette, the chord graph (entries and their connections) and the
  signposts with their cadences. IDirectMusicChordMap (GetScale) and the chord map, signpost and chord map
  tracks, with their parameters, Clone and Save.
- IDirectMusicComposer8: ComposeSegmentFromTemplate(Ex) (with and without activity), ComposeSegmentFromShape,
  ComposeTemplateFromShape(Ex), ChangeChordMap (DMUS_CHORDMAPF_VERSION8 or the old transposition),
  ComposeTransition and AutoTransition (embellishments, the transition's band, tempo, marker track and valid
  starts). The path search, signpost choice, cadences, beat spreading and measure alignment follow Microsoft's,
  rand() call for rand() call.
- IDirectMusicSegment::Compose (the signpost track's Compose), with from and to segments (each track's Join;
  time signature and marker tracks are not IDirectMusicTrack8, so joins with them fail as on Microsoft's).
- Recomposing while playing: the signpost track recomposes at the start (DMUS_TRACKCONFIG_PLAY_COMPOSE) or on a
  loop (DMUS_TRACKCONFIG_LOOP_COMPOSE) and sends GUID_NOTIFICATION_RECOMPOSE. The transport thread's rand() is
  seeded from time() when it starts, as Microsoft's is, so these compose the same chords.
- Microsoft's flaws fixed (off with REDM_NOFIXES):
  - On a chord map with cycles, the path the search found is read back along its marks, which can lead round a
    cycle for ever: Microsoft hangs. reDM follows the path as found when that happens (`c10.sgt`).
  - ChangeChordMap drops the chord track's chord names, failing or not; reDM keeps them.
- Not emulated: a missing "from" chord in ComposeTransition leaves stack contents in Microsoft's buffer; reDM
  uses the default chord there (which is what the stack held in every test).
- `python3 tools/sty/cases.py <dir> compose2` writes the chord maps, styles and templates. `tests/comcmp.sh
  comptest.exe` with the templates (c1..c9, t1..t6) and the modes shape, change, trans, join, recomp, align and pattrack (`cases.py` align, pattrack), and
  `cmapcom.exe` and `theorytest.exe`, all match.

## Remaining, in the agreed order

1. ~~Surround / CMSS~~: speakers beyond stereo, CMSS-3D on quad/5.1/7.1, stereo speakers and headphones.
(EAX 5 dropped: games used it through OpenAL, which OpenAL Soft covers; the EAX 5 property set stays
reported unsupported so games fall back to EAX 4.)

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
- Segment::InitPlay and the DirectSound effects' DMO ProcessInput / ProcessOutput (E_NOTIMPL; games use the effects
  through SetFX).
- A CreatePort'ed port asking for one audio channel reports a mono format but renders stereo, and a port's synth
  plays channel groups beyond the ones it was made with (Microsoft's would not).

## Testing

- `tests/comcmp.sh comtest.exe` runs `comtest` under Microsoft's DirectMusic and under reDM, then diffs
  the output. It currently gives 776 identical lines.
- `tests/comcmp.sh tooltest.exe` does the same for graphs and tools (the graph object, every owner's graph,
  GetObjectInPath stages, the messages tools receive while bells1 and fe_hard_04 play, Flush, loading).
  It gives 255 identical lines.
- `tests/comcmp.sh dx6test.exe [port|buffer|play|clock]` compares the DirectX 6/7 calls (CreatePort and its
  parameters, AddPort / AssignPChannelBlock / RemovePort with several ports, CreateMusicBuffer and the buffer's
  methods, PlayBuffer, the clocks' Advise calls): all match. `multiperf`'s `dx6` thread (MP_THREADS bit 0x40) runs
  that way of playing against the other threads.
- `comcmp.sh` uses `/tmp/real/sp/com` (`COMCMP_DIR`), the Microsoft prefix `/tmp/real/sp/msprefix` and
  the reAL prefix `/tmp/real/sp/realprefix` (`REAL_PREFIX`).
- `build/lockstress.exe <seconds> <segments...> [script.spt]` (Wine) runs four threads on one loader and one
  performance: loads, plays, garbage collection and cache clears, and script calls. A watchdog reports a
  deadlock. `build/lockorder.exe` forces two of those meetings: a Load paused inside the loader's lock against
  a play, or against a segment's last release (`release`); and a VBScript routine against a script's teardown
  (`vbs`).
- `TP_RELOAD=<ms>` makes `test_perf` load every track of the playing segment again from its own chunk.
  Build it with `-fsanitize=address` to check the reload path.
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
  - GetSegmentState right after a segment is played: whether the transport thread has started the state yet
    (comtest sleeps before stopping to keep clear of it).
  - Whether a segment's first Play happens in PlaySegmentEx or on the transport thread (which rand() a
    recompose at the start uses); comptest's recomp mode starts its segments a second ahead to keep clear of it.
