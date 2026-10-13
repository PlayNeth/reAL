#!/usr/bin/env python3
"""cases.py <outdir> [case...]: write the style test cases (<case>.sgt, plus the .sty files they reference)."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stygen import *

MAJOR, MINOR, MAJ7, DOM7 = 0x91, 0x89, 0x891, 0x491
CMAJ_SCALE = 0xab5ab5
BAR = 4 * 768

def chord(t, m, b, name, root, pattern=MAJOR, scale=CMAJ_SCALE, scaleroot=0, levels=0xffffffff, inv=0xffffff,
          flags=0):
    return (t, m, b, name, flags, [(pattern, scale, inv, levels, root, scaleroot)])

def basic_style(name='basic', playmode=DMUS_PLAYMODE_NORMALCHORD, notes=None, variations=(0xffffffff,)):
    st = Style('8f6d1a10-0000-4000-8000-0000000000%02x' % (len(name) & 0xff), name)
    p = st.part('8f6d1a10-0001-4000-8000-000000000001', measures=1, variations=variations, playmode=playmode)
    for i, mv in enumerate(notes or [music_value(4, 0, 0), music_value(4, 1, 0), music_value(4, 2, 0),
                                     music_value(5, 0, 0)]):
        p.note(grid=i * 4, var=0xffffffff, dur=600, music=mv, vel=100, playmode=DMUS_PLAYMODE_NONE)
    pt = st.pattern('main', measures=1, groove=(1, 100))
    pt.partref(p, pchannel=0)
    return st

def case_basic(out):
    st = basic_style()
    seg = Segment('8f6d1a10-1000-4000-8000-000000000001', 'basic', 2 * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'basic.sty', st.guid, st.name)])
    seg.chords(0x00ab5ab5, [chord(0, 0, 0, 'C', 0), chord(BAR, 1, 0, 'F', 5)])
    return {'basic.sty': st.build(), 'basic.sgt': seg.build()}

def case_params(out):
    """chord, command and mute tracks for GetParam / SetParam / Clone (tests/styletest.c)"""
    seg = Segment('8f6d1a10-1000-4000-8000-000000000002', 'params', 8 * BAR)
    seg.timesig([(0, 4, 4, 4)])
    subs2 = [(MAJOR, CMAJ_SCALE, 0xffffff, 1, 0, 0), (MAJ7, 0xad5ad5, 0x0f0f0f, 6, 12, 2), (0x491, 0, 0, 0, 7, 0)]
    seg.chords(0x07ab5ab5, [
        chord(2 * BAR, 2, 0, 'G7', 7, DOM7),
        chord(0, 0, 0, 'C', 0),
        (BAR + 768, 1, 1, 'Dm', 0, [(MINOR, CMAJ_SCALE, 0xffffff, 0xffffffff, 2, 0)]),
        (BAR, 1, 0, 'Am', 1, [(MINOR, CMAJ_SCALE, 0xffffff, 0xffffffff, 9, 0)]),
        (3 * BAR + 1536, 3, 2, 'multi', 0, subs2),
        (5 * BAR, 5, 0, 'FFFFFFFFFFFFFFFF', 0, [(MAJOR, CMAJ_SCALE, 0xffffff, 0xffffffff, 5, 0)]),
        (5 * BAR + 768, 5, 1, 'silent', 1, [(MINOR, 0, 0, 0, 4, 0)]),
    ])
    seg.commands([(0, 0, 0, 0, 50, 10, 0), (BAR, 1, 0, 1, 0, 0, 2), (2 * BAR, 2, 0, 0, 80, 120, 7),
                  (3 * BAR + 20, 3, 0, 3, 0, 5, 1), (BAR + 1536, 1, 2, 2, 70, 20, 3), (6 * BAR, 6, 0, 4, 99, 0, 4)])
    seg.mutes([(BAR, 0, 3), (0, 1, 0xffffffff), (2 * BAR, 0, 0xffffffff), (BAR // 2, 0, 5), (3 * BAR, 2, 2)])
    return {'params.sgt': seg.build()}

def mix_style(name, gseed=0):
    """several patterns by groove and embellishment, parts with several variations in each variation mode"""
    import random
    R = random.Random(gseed)
    st = Style('8f6d1a10-0000-4000-8000-0000000001%02x' % gseed, name)
    parts = []
    modes = [0, 1, 2, 3, 4, 1]
    for k in range(6):
        nv = 1 + k % 4 + (k // 3)
        vars_ = []
        for v in range(nv):
            vars_.append(R.choice([0xffffffff, 0x7fffffff, 0x3fffffff, 0x20000000 | R.getrandbits(29),
                                   R.getrandbits(14), 0x101 | R.getrandbits(15), 0x60000000 | R.getrandbits(29)]))
        p = st.part('8f6d1a10-0002-4000-8000-0000000000%02x' % k, measures=1 + (k == 2), variations=vars_,
                    playmode=[DMUS_PLAYMODE_NORMALCHORD, DMUS_PLAYMODE_PEDALPOINT, DMUS_PLAYMODE_MELODIC,
                              DMUS_PLAYMODE_FIXED, DMUS_PLAYMODE_ALWAYSPLAY, DMUS_PLAYMODE_PEDALPOINTCHORD][k],
                    inv_lower=[0, 40, 0, 0, 30, 0][k], inv_upper=[127, 70, 127, 100, 60, 127][k])
        for n in range(6 + 2 * k):
            grid = R.randrange(0, 16 * p.measures)
            mv = music_value(R.randrange(3, 6), R.randrange(0, 4), R.randrange(0, 3), R.choice([0, 0, 1, 15]))
            if k == 3: mv = R.randrange(36, 90)
            pm = R.choice([DMUS_PLAYMODE_NONE] * 3 + [DMUS_PLAYMODE_NORMALCHORD, DMUS_PLAYMODE_SCALE_INTERVALS,
                                                      DMUS_PLAYMODE_CHORD_ROOT | 8, DMUS_PLAYMODE_KEY_ROOT | 4])
            p.note(grid=grid, var=R.choice([0xffffffff, (1 << nv) - 1, R.getrandbits(nv) or 1]), dur=R.randrange(50, 900),
                   music=mv, vel=R.randrange(40, 127), offset=R.randrange(-20, 20),
                   timerange=R.choice([0, 0, 0, 5, 200]), durrange=R.choice([0, 0, 10]), velrange=R.choice([0, 0, 20]),
                   inversion=R.choice([0, 0, 0, 1, 2]) if k in (1, 4) else 0, playmode=pm)
        if k == 5:
            p.curve(grid=2, var=0xffffffff, dur=700, reset_dur=0, start=0, end=127, etype=4, cc=7)
            p.curve(grid=9, var=0xffffffff, dur=300, reset_dur=0, start=8192, end=12000, etype=3)
        parts.append((p, modes[k]))
    defs = [('slow', 1, (1, 40), 0), ('mid', 1, (30, 70), 0), ('fast', 2, (60, 100), 0), ('fill', 1, (1, 100), 1),
            ('brk', 1, (1, 100), 2), ('intro', 1, (1, 100), 4), ('end', 1, (1, 100), 8), ('mid2', 1, (35, 65), 0)]
    for i, (pn, meas, gr, emb) in enumerate(defs):
        pt = st.pattern(pn, measures=meas, groove=gr, embellish=emb, rhythm=[R.choice([1, 5, 0x11, 0xf]) for _ in range(meas)])
        for j, (p, mode) in enumerate(parts):
            if (i + j) % 3 == 2: continue
            pt.partref(p, pchannel=j, lock=1 if j in (1, 4) and i % 2 else 0, subchord=R.choice([0, 1, 2]), random=mode)
    return st

def mix_chords(R, nbars):
    roots = [0, 5, 7, 9, 2, 4, 11, 3]
    items = []
    for b in range(nbars):
        for beat in ([0] if b % 2 else [0, 2]):
            r = R.choice(roots)
            pat = R.choice([MAJOR, MINOR, MAJ7, DOM7, 0x49])
            subs = [(pat, CMAJ_SCALE, R.choice([0xffffff, 0x0f0f0f, 0x1000000]), 0xffffffff, r, 0)]
            if R.random() < 0.3:
                subs.append((R.choice([MAJOR, MINOR]), CMAJ_SCALE, 0xffffff, 6, (r + 7) % 24, 0))
            items.append((b * BAR + beat * 768, b, beat, 'c%d' % b, 0, subs))
    return items

def case_mix(out, seed=1):
    import random
    R = random.Random(seed)
    st = mix_style('mix%d' % seed, seed)
    n = 12
    seg = Segment('8f6d1a10-1000-4000-8000-0000000000%02x' % (16 + seed), 'mix', n * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'mix%d.sty' % seed, st.guid, st.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, n))
    cmds = []
    for b in range(n):
        if b == 0 or R.random() < 0.5:
            cmds.append((b * BAR, b, 0, R.choice([0, 0, 0, 1, 2, 3, 4]), R.randrange(1, 100), R.choice([0, 10, 40]),
                         R.choice([0, 1, 2, 3, 4, 5])))
    seg.commands(cmds)
    return {'mix%d.sty' % seed: st.build(), 'mix%d.sgt' % seed: seg.build()}

def case_markers(out, seed=4):
    """parts that change variation at markers (DMUS_PARTF_USE_MARKERS) and with the chords (ALIGN_CHORDS)"""
    import random
    R = random.Random(seed)
    st = mix_style('mark', seed)
    for i, p in enumerate(st.parts):
        p.flags = [1, 3, 2, 1, 0, 3][i]
        nv = sum(1 for v in p.variations if v)
        for g in range(0, 16 * p.measures, R.choice([2, 4, 6])):
            p.marker(grid=g, var=R.getrandbits(nv) or 1, flags=R.choice([1, 2, 3, 6, 7]))
    n = 10
    seg = Segment('8f6d1a10-1000-4000-8000-000000000031', 'markers', n * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'mark.sty', st.guid, st.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, n))
    seg.commands([(b * BAR, b, 0, R.choice([0, 0, 1]), R.randrange(1, 100), 20, R.choice([0, 1, 2])) for b in range(0, n, 2)])
    return {'mark.sty': st.build(), 'markers.sgt': seg.build()}

def case_dx7(out, seed=5):
    """a DirectX 7 style (patterns at random, the DX7 variation flags)"""
    import random
    R = random.Random(seed)
    st = mix_style('dx7', seed)
    for p in st.parts: p.dx7 = True
    n = 10
    seg = Segment('8f6d1a10-1000-4000-8000-000000000032', 'dx7', n * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'dx7.sty', st.guid, st.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, n))
    seg.commands([(b * BAR, b, 0, R.choice([0, 0, 1, 2]), R.randrange(1, 100), 20, 0) for b in range(0, n, 2)])
    return {'dx7.sty': st.build(), 'dx7.sgt': seg.build()}

def case_twostyles(out, seed=6):
    """two styles (the second in 3/4) on the style track, no time signature track"""
    import random
    R = random.Random(seed)
    a = mix_style('twoa', seed)
    b = mix_style('twob', seed + 1)
    b.ts = (3, 4, 4)
    for p in b.parts: p.ts = (3, 4, 4)
    for p in b.patterns: p.ts = (3, 4, 4)
    seg = Segment('8f6d1a10-1000-4000-8000-000000000033', 'twostyles', 4 * BAR + 6 * 3 * 768)
    seg.tempo([(0, 100.0)])
    seg.style([(0, 'twoa.sty', a.guid, a.name), (4 * BAR, 'twob.sty', b.guid, b.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, 8))
    seg.commands([(0, 0, 0, 0, 50, 10, 0), (4 * BAR, 4, 0, 0, 80, 10, 2)])
    return {'twoa.sty': a.build(), 'twob.sty': b.build(), 'twostyles.sgt': seg.build()}

def case_twots(out, seed=6):
    d = case_twostyles(out, seed)
    import random
    R = random.Random(seed)
    a = mix_style('twoa', seed)
    b = mix_style('twob', seed + 1)
    seg = Segment('8f6d1a10-1000-4000-8000-000000000036', 'twots', 4 * BAR + 6 * 3 * 768)
    seg.tempo([(0, 100.0)])
    seg.timesig([(0, 4, 4, 4), (4 * BAR, 3, 4, 4)])
    seg.style([(0, 'twoa.sty', a.guid, a.name), (4 * BAR, 'twob.sty', b.guid, b.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, 8))
    seg.commands([(0, 0, 0, 0, 50, 10, 0), (4 * BAR, 4, 0, 0, 80, 10, 2)])
    d['twots.sgt'] = seg.build()
    return d

def case_mute(out, seed=7):
    """the mute track: parts muted and moved to other PChannels"""
    import random
    R = random.Random(seed)
    st = mix_style('mute', seed)
    n = 8
    seg = Segment('8f6d1a10-1000-4000-8000-000000000034', 'mute', n * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'mute.sty', st.guid, st.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, n))
    seg.mutes([(BAR, 0, 0xffffffff), (2 * BAR, 0, 0), (BAR + 768, 2, 7), (3 * BAR, 3, 0xffffffff), (5 * BAR, 3, 3),
               (2 * BAR + 100, 4, 9), (4 * BAR, 1, 0xffffffff), (6 * BAR, 1, 1)])
    return {'mute.sty': st.build(), 'mute.sgt': seg.build()}

def case_long(out, seed=8):
    """patterns of several measures, parts shorter (repeating) and longer than their pattern, repeats"""
    import random
    R = random.Random(seed)
    st = mix_style('long', seed)
    for i, p in enumerate(st.parts): p.measures = [1, 3, 2, 1, 4, 2][i]
    for i, pt in enumerate(st.patterns):
        pt.measures = [2, 3, 1, 4, 2, 1, 2, 3][i]
        pt.rhythm = [R.choice([1, 5, 0xf]) for _ in range(pt.measures)]
    n = 14
    seg = Segment('8f6d1a10-1000-4000-8000-000000000035', 'long', n * BAR, repeats=1)
    seg.tempo([(0, 140.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'long.sty', st.guid, st.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, n))
    seg.commands([(b * BAR, b, 0, R.choice([0, 0, 0, 1, 3]), R.randrange(1, 100), 30, R.choice([0, 1, 2, 3, 4, 5]))
                  for b in range(0, n, 3)])
    return {'long.sty': st.build(), 'long.sgt': seg.build()}

def case_loop(out, seed=9):
    """a short segment repeated (the patterns start again at each loop)"""
    import random
    R = random.Random(seed)
    st = mix_style('loop', seed)
    n = 3
    seg = Segment('8f6d1a10-1000-4000-8000-000000000037', 'loop', n * BAR, repeats=2)
    seg.tempo([(0, 150.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'loop.sty', st.guid, st.name)])
    seg.chords(0x00ab5ab5, mix_chords(R, n))
    seg.commands([(0, 0, 0, 0, 40, 20, 2), (BAR, 1, 0, 1, 60, 20, 1), (2 * BAR, 2, 0, 0, 70, 20, 0)])
    return {'loop.sty': st.build(), 'loop.sgt': seg.build()}

def case_comstyle(out, seed=10):
    """a style with bands and motifs for the COM interfaces (tests/stylecom.c)"""
    st = mix_style('comstyle', seed)
    st.bands.append(band('8f6d1a10-0005-4000-8000-000000000001', 'Default Band',
                         [(0, 0x000000, 100, 64, None), (1, 0x000018, 90, 30, None), (2, 0x000021, 80, 100, None)]))
    st.bands.append(band('8f6d1a10-0005-4000-8000-000000000002', 'Second Band',
                         [(0, 0x000005, 127, 64, None), (3, 0x000038, 70, 64, None)]))
    st.parts[0].marker(grid=4, var=0xffffffff, flags=1)
    st.parts[0].marker(grid=12, var=0xffffffff, flags=3)
    m = st.pattern('Motif A', measures=2, groove=(1, 100), embellish=0x10)
    m.motif = (1, 0, 0, 768 * 4, 0)
    m.band = band('8f6d1a10-0005-4000-8000-000000000003', 'Motif Band', [(9, 0x000030, 110, 20, None)])
    m.partref(st.parts[0], pchannel=9)
    m2 = st.pattern('Motif B', measures=1, groove=(1, 100), embellish=0x10)
    m2.motif = (0, 0, 0, 0, 0)
    m2.partref(st.parts[1], pchannel=10)
    st.pattern('Custom', measures=3, groove=(1, 100), embellish=0x0700).partref(st.parts[2], pchannel=2)
    seg = Segment('8f6d1a10-1000-4000-8000-000000000038', 'comstyle', 8 * BAR)
    seg.tempo([(0, 120.0)])
    seg.style([(0, 'comstyle.sty', st.guid, st.name), (4 * BAR, 'mix1.sty', None, None)])
    return {'comstyle.sty': st.build(), 'comstyle.sgt': seg.build()}

def case_seqmute(out, seed=11):
    """the mute track on a sequence track's parts (muted and remapped)"""
    seg = Segment('8f6d1a10-1000-4000-8000-000000000039', 'seqmute', 6 * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    ev = []
    for b in range(6 * 4):
        for p in range(4):
            ev.append((b * 768 + p * 10, 300, p, 0x90 | p, 48 + p * 5 + b % 7, 90))
        ev.append((b * 768 + 384, 0, 2, 0xb2, 10, b * 5 % 128))
    seg.seq(ev, curves=[(BAR, 1000, 3, 4, 7, 0, 127), (3 * BAR, 500, 0, 3, 0, 0, 8000)])
    seg.mutes([(BAR, 0, 0xffffffff), (2 * BAR, 0, 0), (BAR + 768, 2, 7), (3 * BAR, 3, 0xffffffff), (4 * BAR, 3, 1),
               (2 * BAR + 100, 1, 9), (5 * BAR, 1, 1)])
    return {'seqmute.sgt': seg.build()}

def case_wavemute(out, seed=12):
    """the mute track on a wave track's parts"""
    import math
    pcm = b''.join(struct.pack('<h', int(8000 * math.sin(i * 0.05))) for i in range(22050 // 4))
    wav = b'RIFF' + struct.pack('<I', 36 + len(pcm)) + b'WAVEfmt ' + struct.pack('<IHHIIHH', 16, 1, 1, 22050, 44100, 2, 16) + \
        b'data' + struct.pack('<I', len(pcm)) + pcm
    seg = Segment('8f6d1a10-1000-4000-8000-00000000003a', 'wavemute', 6 * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.waves([(p, [(b * 768 + p * 7, 600, 'tone.wav') for b in range(24)]) for p in range(3)])
    seg.mutes([(BAR, 0, 0xffffffff), (2 * BAR, 0, 0), (BAR + 768, 2, 5), (3 * BAR, 1, 0xffffffff), (4 * BAR, 1, 1)])
    return {'tone.wav': wav, 'wavemute.sgt': seg.build()}

def case_pattern(out, seed=13, flags=None):
    """a pattern track: one pattern with its parts embedded, played over chords (no inversions)"""
    import random
    R = random.Random(seed)
    st = mix_style('pat', seed)
    pt = Pattern('ptrack', (4, 4, 4), 2, (1, 100), 0, [0x11, 5])
    for j, p in enumerate(st.parts):
        if flags: p.flags = flags[j]
        pt.parts.append(p)
        pt.partref(p, pchannel=j, subchord=R.choice([0, 1]), random=[0, 1, 2, 3, 4, 1][j])
    if flags:
        st.parts[0].marker(grid=6, var=0xffffffff, flags=1)
        st.parts[1].marker(grid=20, var=0xffffffff, flags=5)
    n = 8
    seg = Segment('8f6d1a10-1000-4000-8000-00000000003b', 'pattern', n * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.pattern(pt)
    seg.chords(0x00ab5ab5, mix_chords(R, n))
    return {'pattern.sgt' if not flags else 'patmark.sgt': seg.build()}

MIN, DIM = 0x89, 0x49
def cmap1(name='cmap1', guid='8f6d1a10-0006-4000-8000-000000000001', loadname=None):
    sc = CMAJ_SCALE
    subs = [(MAJOR, sc, 0xffffff, 0, 0, 0, 0xffffffff), (MIN, sc, 0xffffff, 2, 0, 0, 0xffffffff),
            (MIN, sc, 0xffffff, 4, 0, 0, 0xffffffff), (MAJOR, sc, 0xffffff, 5, 0, 0, 0xffffffff),
            (MAJOR, sc, 0xffffff, 7, 0, 0, 0xffffffff), (MIN, sc, 0xffffff, 9, 0, 0, 0xffffffff),
            (DOM7, sc, 0x0f0f0f, 7, 0, 0, 0xffffffff), (DIM, sc, 0xffffff, 11, 0, 0, 0xffffffff),
            (MAJ7, sc, 0xffffff, 0, 0, 0, 0xffffffff), (MAJOR, 0xad5ad5, 0xffffff, 2, 2, 0, 0xffffffff)]
    names = ['C', 'Dm', 'Em', 'F', 'G', 'Am', 'G7', 'Bdim', 'Cmaj7', 'D']
    C = lambda i: (names[i], [i, i, i, i])
    pal = [C([0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 7][r % 12]) for r in range(24)]
    pal[13] = ('LongChordName1', [8, 8, 8, 8])
    E = lambda i, cid, fl, nx: (fl, cid, C(i), nx)
    ent = [E(0, 1, 2 | 4, [(0, 20, 1, 4, 2), (0, 30, 2, 4, 4), (0, 10, 1, 2, 6), (0, 20, 2, 8, 5)]),
           E(1, 2, 0, [(0, 50, 1, 4, 5), (0, 30, 1, 2, 7)]),
           E(2, 3, 2, [(0, 40, 2, 4, 6), (0, 20, 1, 4, 4)]),
           E(3, 4, 2, [(0, 40, 1, 4, 5), (0, 20, 2, 4, 1), (0, 20, 1, 2, 2)]),
           E(4, 5, 2 | 4, [(0, 60, 2, 4, 1), (0, 20, 1, 4, 6), (0, 10, 1, 1, 3)]),
           E(5, 6, 0, [(0, 30, 1, 4, 2), (0, 30, 2, 2, 4), (0, 10, 1, 4, 3)]),
           E(6, 7, 4, [(0, 80, 1, 4, 1), (0, 5, 1, 1, 9)]),
           E(7, 8, 0, [(0, 50, 1, 2, 1), (0, 10, 1, 4, 3)]),
           E(8, 9, 2, [(0, 50, 1, 4, 4), (0, 50, 1, 4, 2), (0, 0, 1, 1, 0)]),
           E(9, 10, 4, [(0, 40, 2, 4, 5)])]
    sps = [(0x0001 | 0x0100, 0, C(0), []), (0x0001, 2, C(0), [C(6)]), (0x0002 | 0x0400, 0, C(3), []),
           (0x0004 | 0x1000, 4, C(4), [C(1)]), (0x0100, 6, C(0), [C(3), C(4)]), (0x0002 | 0x8000, 2, C(5), [C(4)]),
           (0x0008, 0, C(8), []), (0x0002, 0, C(3), [])]
    return chordmap(guid, name, 0x00ab5ab5, subs, pal, ent, sps, loadname=loadname)

def case_compose(out, seed=15):
    """a chord map, a style that refers to it, and a template (signposts, commands, the chord map track)"""
    st = basic_style('compsty')
    st.chordmaps = [('cmap1.cdm', '8f6d1a10-0006-4000-8000-000000000001', 'cmap1'),
                    ('cmap2.cdm', '8f6d1a10-0006-4000-8000-000000000002', 'cmap2'),
                    ('cmap1.cdm', None, 'cmap1')]
    seg = Segment('8f6d1a10-1000-4000-8000-00000000003c', 'template', 12 * BAR)
    seg.tempo([(0, 120.0)])
    seg.timesig([(0, 4, 4, 4)])
    seg.style([(0, 'compsty.sty', st.guid, st.name)])
    seg.commands([(0, 0, 0, 0, 40, 0, 0), (4 * BAR, 4, 0, 1, 70, 0, 0), (8 * BAR, 8, 0, 0, 90, 0, 0)])
    seg.signposts([(0, 0x0101, 0), (2 * BAR, 0x0002, 2), (4 * BAR, 0x0004, 4), (6 * BAR, 0x0400, 6),
                   (8 * BAR, 0x8002, 8), (10 * BAR, 0x0100, 10)])
    seg.chordmaps([(0, 'cmap1.cdm', '8f6d1a10-0006-4000-8000-000000000001', 'cmap1'),
                   (6 * BAR, 'cmap2.cdm', '8f6d1a10-0006-4000-8000-000000000002', 'cmap2')])
    return {'cmap1.cdm': cmap1(),
            'cmap2.cdm': cmap1('cmap2', '8f6d1a10-0006-4000-8000-000000000002', loadname='Second Map'),
            'compsty.sty': st.build(), 'template.sgt': seg.build()}

CASES = {'compose': case_compose, 'pattern': case_pattern, 'patmark': lambda o: case_pattern(o, 14, [1, 3, 2, 1, 0, 3]),
         'wavemute': case_wavemute, 'seqmute': case_seqmute, 'comstyle': case_comstyle, 'loop': case_loop, 'markers': case_markers, 'dx7': case_dx7, 'twostyles': case_twostyles, 'twots': case_twots, 'mute': case_mute, 'long': case_long,
'basic': case_basic, 'params': case_params, 'mix1': lambda o: case_mix(o, 1), 'mix2': lambda o: case_mix(o, 2),
         'mix3': lambda o: case_mix(o, 3)}

if __name__ == '__main__':
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for name in sys.argv[2:] or CASES:
        for f, data in CASES[name](outdir).items():
            open(os.path.join(outdir, f), 'wb').write(data)
        print(name)
