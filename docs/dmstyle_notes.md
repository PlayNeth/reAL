# dmstyle notes (XP dmstyle.dll 5.3.2600.5512, base 0x77400000)

Decompiled with Ghidra 11.4.2 headless (tools in /tmp/real/gscripts: VtFuncs creates functions at vtable
targets, ThisCall marks functions that read ECX on entry as __thiscall, DecompAll dumps every function).

## Classes (DllGetClassObject 77405edb)

| CLSID | factory CreateInstance | object | size | vtables |
|---|---|---|---|---|
| Style d2ac288a | 77405839 | 7740bd9d | 0xf8 | 774016e8 private, 774016ac IDirectMusicStyle8, 77401694 IDirectMusicObject, 77401674 IPersistStream |
| Section 3f037241 | 774058e0 | | | |
| ChordTrack d2ac288b | 77405a9c | 77402b98 | 0x3c | 77401120 track, 77401100 IPersistStream |
| CommandTrack d2ac288c | 77405a1e | 77403b95 | 0x38 | 77401188, 77401168 |
| StyleTrack d2ac288d | 77405930 | 774128f4 | 0x34 | 77401898 track, 77401884 private, 77401864 IPersistStream |
| MotifTrack d2ac288e | 77405b2b | | | |
| MuteTrack d2ac2898 | 77405ba9 | | | |
| PatternTrack d2ac2897 | 77405c30 | | | |

Style track ctor: srand(time(NULL)) (application thread).  rand() is msvcrt's (per thread).

### Style track (CStyleTrack, 0x34)
- +0x24 track info (StyleTrackInfo, vtable 77401858; base TrackInfo 7740184c, ctor 77410b28): +0x08 list of
  style refs (node: +0 time, +4 IDirectMusicStyle*, +8 next), +0x0c/+0x10 PChannel count/array (all parts of all
  styles, sorted unique), +0x28 type (3 style), +0x2c load generation.
- vtable 77401898: Init 7741152f, InitPlay 774115be, EndPlay 77411675, Play 774116c7, GetParam 7741285f,
  SetParam 774129be, IsParamSupported 77411916, Clone 774126f2, PlayEx 77411ad2, GetParamEx 77412825,
  Compose 7741553f, Join 7741305c.  Load 7741313c ('sttr' 77412fbc, each 'strf' 77412b3c).
- InitPlay -> info vtable[2] 77412e52: StyleTrackState (0x38c, ctor 77412de3, vtable 774018e0) on PatternTrackState
  (ctor 7740f355, vtable 77401838).

### Style data (DMStyleStruct at style object +0x38; offsets below are from it)
- +0x04 guid, +0x14 category, +0x1c name, +0x24/+0x28 version, +0x2c timesig (beats, beat, grids),
  +0x30 tempo (double), +0x38 parts (list node: +0 item, +4 next), +0x3c patterns, +0x40 motifs,
  +0x48 default band (+0x80 of the object), +0x54 PChannels, +0x58 flags (1: DX8 file, a 'prth' > 0x9f bytes).
- Load 7740cf73: guid, styh, vers, UNFO (UNAM/INAM name 0x7e max, catg), 'part' 77408e32, 'pttn' 7740c5f3 (motif
  when pattern +0x16 & 0x10 -> motif list, else pattern list), RIFF DMBD/AABN bands (default: first DMBD, or the
  AABN with flag 2), 'prrf' 7740af13 (chordmap references).  Then 7740bd29 on every pattern and motif.
- 'AASY' (IMA styles) loaded by 7740ca94.

### Part (DirectMusicPart, 300 bytes, ctor 7740b215, created by 7740b531)
+0x04 guid, +0x14 timesig, +0x18 measures, +0x1c variation choices[32] (-1 -> 0x3fffffff; DX8 file: 0x3fffffff ->
0x7fffffff), +0x9c playmode (default 10), +0x9d invert upper (0x7f), +0x9e invert lower, +0xa0 flags,
+0xa4 events (sorted 774087cf), +0xa8 4 x 0x20 ?, +0x128 resolutions.  A 'prth' whose guid is already a part's
discards the new part.

Events (+0 next, +4 grid start (short), +6 time offset, +8 variations, +0xc type):
- note (1, 0x24): +0x10 duration, +0x14 music value, +0x16 velocity, +0x17 time range, +0x18 duration range,
  +0x19 velocity range, +0x1a inversion id, +0x1b playmode, +0x20 note flags.  'note' chunk records are 0x18
  bytes (larger records: the rest skipped).
- curve (2, 0x28): +0x10 duration, +0x14 reset duration, +0x18 start, +0x1a end, +0x1c reset, +0x1e type,
  +0x1f shape, +0x20 cc, +0x21 flags, +0x22 param type, +0x24 merge index.  Records 0x20.
- marker (3, 0x14): +0x10 marker flags.  Records 0x0a.
- anticipation (4, 0x14): +0x6 time offset, +0x10 time range.  Records 0x0c.
- Events with a negative grid start are dropped.

### Pattern (0x48, ctor 7740c9ea)
+0x0c timesig, +0x10 ?, +0x12/+0x13 groove bottom/top (1, 100), +0x14/+0x15 dest groove (1, 100),
+0x16 embellishment (0x10: motif), +0x18 measures (1), +0x34 = -1.

## PatternTrackState (ctor 7740f355)
+0x04 track info, +0x08 segment state, +0x0c style data, +0x10 segment state (IUnknown), +0x14 virtual track id,
+0x24 chord name ("M7"), +0x44.., +0x48 chord: subchords at +0x48 (8 x 0x14: chord pattern 0x891, scale 0xab5ab5,
inversion points 0xffffff, levels -1, chord root 0x0c, scale root 0), +0xe8 scale 0xab5ab5, +0xec root 0x0c,
+0x1bc current pattern, +0x1c4 per-part variation (byte, 0xff none), +0x244 per-PChannel mute state (12 bytes:
+0 ?, +4 PChannel, +8 mute), +0x34c group id, +0x350/+0x354 per-measure random tables, +0x364 ...

Note output (7740f63c): DMUS_NOTE_PMSG: mtTime = grid time + offset + part offset (+ time range random), measure/
beat/grid/offset, duration (+ duration range random), velocity (+ velocity range rand, clamped 1..127), music
value, MIDI value and transpose from the conversion 77408907, playmode, subchord level (part ref +9), flags =
note flags | NOTEON.  rand() order: time, duration, velocity.  Range (7740f5e3): r < 0xbf: r; < 0xd5: (r-0x98)*5;
< 0xe9: (r-0xb6)*10; else (r-0xde)*50; result rand() % r - r/2.
