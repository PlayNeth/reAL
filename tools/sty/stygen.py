#!/usr/bin/env python3
"""stygen.py: write DirectMusic styles (.sty, the 'DMST' form) and segments (.sgt) that play them, for comparing
reAL-Music's style engine with Microsoft's dmstyle.  The structures follow dmusicf.h (pack 2 where it says so).

Used as a module by the scenario scripts (tools/sty/cases.py); everything is plain data:

    st = Style(guid, name, timesig=(4, 4, 4), tempo=120.0)
    p = st.part(guid, measures=1, variations=[0xffffffff] * 32, playmode=DMUS_PLAYMODE_PEDALPOINT)
    p.note(grid=0, var=0xffffffff, dur=768, music=..., vel=100, ...)
    pt = st.pattern(name, measures=1, groove=(1, 100), embellish=0, rhythm=[1])
    pt.partref(p, pchannel=0, lock=0, subchord=0, priority=0, random=0)
    seg = Segment(length, ...); seg.style(0, 'name.sty', st.guid, st.name); seg.chords(...); seg.commands(...)
"""
import struct, uuid

def fcc(s): return s.encode('latin1')

def chunk(cid, body):
    b = struct.pack('<4sI', fcc(cid), len(body)) + body
    return b + (b'\0' if len(body) & 1 else b'')

def lst(kind, typ, items):
    body = fcc(typ) + b''.join(items)
    return chunk(kind, body)

def LIST(typ, *items): return lst('LIST', typ, items)
def RIFF(typ, *items): return lst('RIFF', typ, items)

def wstr(s): return (s + '\0').encode('utf-16-le')

def guid_bytes(g): return uuid.UUID(g).bytes_le

def unfo(name): return LIST('UNFO', chunk('UNAM', wstr(name)))

# play modes (dmusici.h)
DMUS_PLAYMODE_KEY_ROOT, DMUS_PLAYMODE_CHORD_ROOT, DMUS_PLAYMODE_SCALE_INTERVALS = 1, 2, 4
DMUS_PLAYMODE_CHORD_INTERVALS, DMUS_PLAYMODE_NONE = 8, 16
DMUS_PLAYMODE_FIXED, DMUS_PLAYMODE_FIXEDTOKEY, DMUS_PLAYMODE_FIXEDTOCHORD = 0, 1, 2
DMUS_PLAYMODE_PEDALPOINT = 1 | 4
DMUS_PLAYMODE_MELODIC = 2 | 4
DMUS_PLAYMODE_NORMALCHORD = 2 | 8
DMUS_PLAYMODE_ALWAYSPLAY = 1 | 2 | 4 | 8
DMUS_PLAYMODE_PURPLEIZED = DMUS_PLAYMODE_ALWAYSPLAY
DMUS_PLAYMODE_PEDALPOINTCHORD = 1 | 8
DMUS_PLAYMODE_PEDALPOINTALWAYS = DMUS_PLAYMODE_PEDALPOINT | DMUS_PLAYMODE_CHORD_INTERVALS

CLSID = {
    'style': 'd2ac288a-b39b-11d1-8704-00600893b1bd',
    'styletrack': 'd2ac288d-b39b-11d1-8704-00600893b1bd',
    'chordtrack': 'd2ac288b-b39b-11d1-8704-00600893b1bd',
    'commandtrack': 'd2ac288c-b39b-11d1-8704-00600893b1bd',
    'mutetrack': 'd2ac2898-b39b-11d1-8704-00600893b1bd',
    'tempotrack': 'd2ac2885-b39b-11d1-8704-00600893b1bd',
    'timesigtrack': 'd2ac2888-b39b-11d1-8704-00600893b1bd',
    'bandtrack': 'd2ac2894-b39b-11d1-8704-00600893b1bd',
    'collection': '480ff4b0-28b2-11d1-bef7-00c04fbf8fef',
}

def timesig(ts):
    beats, beat, grids = ts
    return struct.pack('<BBH', beats, beat, grids)

def music_value(octave, chord_pos, scale_off, accidental=0):
    """wMusicValue: octave (4 bits), chord position (4), scale offset (4), accidental (4, signed)."""
    return ((octave & 15) << 12) | ((chord_pos & 15) << 8) | ((scale_off & 15) << 4) | (accidental & 15)

class Part:
    def __init__(self, guid, timesig, measures, variations, playmode, inv_upper=127, inv_lower=0, flags=0):
        self.guid, self.ts, self.measures = guid, timesig, measures
        self.variations = list(variations) + [0] * (32 - len(variations))
        self.playmode, self.inv_upper, self.inv_lower, self.flags = playmode, inv_upper, inv_lower, flags
        self.notes, self.curves, self.markers = [], [], []
        self.name = None
        self.dx7 = False

    def note(self, grid, var, dur, music, vel=100, offset=0, timerange=0, durrange=0, velrange=0, inversion=0,
             playmode=DMUS_PLAYMODE_NONE, noteflags=0):
        self.notes.append((grid, var, dur, offset, music, vel, timerange, durrange, velrange, inversion, playmode,
                           noteflags))

    def curve(self, grid, var, dur, reset_dur, start, end, reset=0, offset=0, etype=3, shape=0, cc=7, flags=0,
              paramtype=0, merge=0):
        self.curves.append((grid, var, dur, reset_dur, offset, start, end, reset, etype, shape, cc, flags, paramtype,
                            merge))

    def marker(self, grid, var, flags=1):
        """DMUS_IO_STYLEMARKER: mtGridStart, dwVariation, wMarkerFlags (1 start, 2 stop, 4 chord aligned)"""
        self.markers.append((grid, var, flags))

    def build(self):
        prth = timesig(self.ts) + struct.pack('<32I', *self.variations) + guid_bytes(self.guid) + \
            struct.pack('<HBBB3xI', self.measures, self.playmode, self.inv_upper, self.inv_lower, self.flags)
        if self.dx7: prth = prth[:0x9c]      # DMUS_IO_STYLEPART without dwFlags: a DirectX 7 style
        items = [chunk('prth', prth)]
        if self.notes:
            fmt = '<iIihHBBBBBBB'
            sz = struct.calcsize(fmt) + 1      # pack(2): 25 -> 26
            body = struct.pack('<I', sz) + b''.join(struct.pack(fmt, *n) + b'\0' for n in self.notes)
            items.append(chunk('note', body))
        if self.curves:
            fmt = '<iIiihhhhBBBBHH'
            sz = struct.calcsize(fmt)
            body = struct.pack('<I', sz) + b''.join(struct.pack(fmt, *c) for c in self.curves)
            items.append(chunk('crve', body))
        if self.markers:
            fmt = '<iIH'
            body = struct.pack('<I', struct.calcsize(fmt)) + b''.join(struct.pack(fmt, *m) for m in self.markers)
            items.append(chunk('mrkr', body))
        if self.name: items.append(unfo(self.name))
        return LIST('part', *items)

class Pattern:
    def __init__(self, name, timesig, measures, groove, embellish, rhythm, dest=(0, 0), flags=0):
        self.name, self.ts, self.measures, self.groove, self.embellish = name, timesig, measures, groove, embellish
        self.rhythm = rhythm if rhythm is not None else [1] * measures
        self.dest, self.flags = dest, flags
        self.refs = []
        self.motif = None
        self.band = None

    def partref(self, part, pchannel, lock=0, subchord=0, priority=0, random=0, logical=None):
        self.refs.append((part.guid, pchannel if logical is None else logical, lock, subchord, priority, random,
                          pchannel))

    def build(self):
        ptnh = timesig(self.ts) + struct.pack('<BBHHBBI', self.groove[0], self.groove[1], self.embellish,
                                              self.measures, self.dest[0], self.dest[1], self.flags)
        items = [chunk('ptnh', ptnh), chunk('rhtm', struct.pack('<%dI' % len(self.rhythm), *self.rhythm))]
        if self.motif:
            items.append(chunk('mtfs', struct.pack('<IiiiI', *self.motif)))
        if self.band:
            items.append(self.band)
        if self.name: items.append(unfo(self.name))
        for g, logical, lock, sub, prio, rnd, pch in self.refs:
            prfc = guid_bytes(g) + struct.pack('<HBBBBHI', logical & 0xffff, lock, sub, prio, rnd, 0, pch)
            items.append(LIST('pref', chunk('prfc', prfc)))
        return LIST('pttn', *items)

class Style:
    def __init__(self, guid, name, timesig=(4, 4, 4), tempo=120.0):
        self.guid, self.name, self.ts, self.tempo = guid, name, timesig, tempo
        self.parts, self.patterns, self.bands = [], [], []

    def part(self, guid, measures=1, variations=(0xffffffff,), playmode=DMUS_PLAYMODE_PEDALPOINT, **kw):
        p = Part(guid, self.ts, measures, variations, playmode, **kw)
        self.parts.append(p)
        return p

    def pattern(self, name, measures=1, groove=(1, 100), embellish=0, rhythm=None, **kw):
        p = Pattern(name, self.ts, measures, groove, embellish, rhythm, **kw)
        self.patterns.append(p)
        return p

    def build(self):
        items = [chunk('guid', guid_bytes(self.guid)), chunk('vers', struct.pack('<II', 0x10000, 0)),
                 unfo(self.name), chunk('styh', timesig(self.ts) + struct.pack('<d', self.tempo))]
        items += [p.build() for p in self.parts]
        items += [p.build() for p in self.patterns]
        items += self.bands
        return RIFF('DMST', *items)

def reference(clsid, guid=None, name=None, file=None):
    valid = 0
    items = []
    if guid: valid |= 0x10; items.append(chunk('guid', guid_bytes(guid)))   # DMUS_OBJ_OBJECT
    if name: valid |= 0x40; items.append(chunk('name', wstr(name)))          # DMUS_OBJ_NAME
    if file: valid |= 0x100; items.append(chunk('file', wstr(file)))         # DMUS_OBJ_FILENAME
    valid |= 0x02                                                             # DMUS_OBJ_CLASS
    return LIST('DMRF', chunk('refh', guid_bytes(clsid) + struct.pack('<I', valid)), *items)

def band(guid, name, insts):
    """insts: (pchannel, patch, volume, pan, collection file or None); bins as DMUS_IO_INSTRUMENT."""
    lbin = []
    for pch, patch, vol, pan, coll in insts:
        # DMUS_IO_INSTRUMENT: dwPatch, dwAssignPatch, dwNoteRanges[4], dwPChannel, dwFlags, bPan, bVolume,
        # nTranspose, dwChannelPriority, nPitchBendRange (+2 padding)
        bins = struct.pack('<II4IIIBBhIh2x', patch, 0, 0, 0, 0, 0, pch, 0x1 | 0x20 | 0x40, pan, vol, 0, 0x80000002,
                           0)
        items = [chunk('bins', bins)]
        if coll:
            items.append(reference(CLSID['collection'], file=coll))
        lbin.append(LIST('lbin', *items))
    return RIFF('DMBD', chunk('guid', guid_bytes(guid)), unfo(name), LIST('lbil', *lbin))

class Segment:
    def __init__(self, guid, name, length, repeats=0, flags=0, resolution=0):
        self.guid, self.name, self.length, self.repeats, self.flags = guid, name, length, repeats, flags
        self.resolution = resolution
        self.tracks = []

    def track(self, clsid, ckid, fcctype, data, group=1, position=0, extras=None):
        trkh = guid_bytes(clsid) + struct.pack('<II4s4s', position, group,
                                               fcc(ckid) if ckid else b'\0' * 4, fcc(fcctype) if fcctype else b'\0' * 4)
        items = [chunk('trkh', trkh)]
        if extras is not None: items.append(chunk('trkx', struct.pack('<II', *extras)))
        items.append(data)
        self.tracks.append(RIFF('DMTK', *items))

    def tempo(self, items):
        body = struct.pack('<I', 16) + b''.join(struct.pack('<i4xd', t, bpm) for t, bpm in items)
        self.track(CLSID['tempotrack'], 'tetr', None, chunk('tetr', body), group=0xffffffff)

    def timesig(self, items):
        body = struct.pack('<I', 8) + b''.join(struct.pack('<iBBH', t, b, bt, g) for t, b, bt, g in items)
        self.track(CLSID['timesigtrack'], None, 'TIMS', LIST('TIMS', chunk('tims', body)))

    def style(self, refs, group=1):
        """refs: (time, file, guid, name)"""
        items = []
        for t, f, g, n in refs:
            items.append(LIST('strf', chunk('stmp', struct.pack('<i', t)), reference(CLSID['style'], g, n, f)))
        self.track(CLSID['styletrack'], None, 'sttr', LIST('sttr', *items), group=group)

    def chords(self, scale, items, group=1):
        """scale: (root << 24) | pattern for crdh; items: (time, measure, beat, name, flags, [subchords])
        subchord: (chordpattern, scalepattern, inversionpoints, levels, chordroot, scaleroot)"""
        body = [chunk('crdh', struct.pack('<I', scale))]
        for t, m, b, name, fl, subs in items:
            nm = name.encode('utf-16-le')[:30]
            nm = nm + b'\0' * (32 - len(nm))
            c = struct.pack('<I', 40) + nm + struct.pack('<iHBB', t, m, b, fl)
            c += struct.pack('<II', len(subs), 20) + b''.join(struct.pack('<IIIIBB2x', *s) for s in subs)
            body.append(chunk('crdb', c))
        self.track(CLSID['chordtrack'], None, 'cord', LIST('cord', *body), group=group)

    def commands(self, items, group=1):
        """items: (time, measure, beat, command, groove, range, repeat)"""
        body = struct.pack('<I', 12) + b''.join(struct.pack('<iHBBBBB1x', *c) for c in items)
        self.track(CLSID['commandtrack'], 'cmnd', None, chunk('cmnd', body), group=group)

    def mutes(self, items, group=1):
        """items: (time, pchannel, map)"""
        body = struct.pack('<I', 12) + b''.join(struct.pack('<iII', *m) for m in items)
        self.track(CLSID['mutetrack'], 'mute', None, chunk('mute', body), group=group)

    def bands(self, items, group=1):
        """items: (time, band RIFF)"""
        lbnd = [LIST('lbnd', chunk('bd2h', struct.pack('<ii', 0, t)), b) for t, b in items]
        self.track(CLSID['bandtrack'], None, 'DMBT', RIFF('DMBT', LIST('lbdl', *lbnd)), group=group)

    def build(self):
        segh = struct.pack('<IiiiiI', self.repeats, self.length, 0, 0, 0, self.resolution) + \
            struct.pack('<qII', 0, self.flags, 0)
        return RIFF('DMSG', chunk('segh', segh), chunk('guid', guid_bytes(self.guid)),
                    chunk('vers', struct.pack('<II', 0x10000, 0)), unfo(self.name), LIST('trkl', *self.tracks))
