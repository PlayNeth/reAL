#!/usr/bin/env python3
"""cases.py <outdir> [case...]: write the style test cases (<case>.sgt, plus the .sty files they reference)."""
import os, sys
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

CASES = {'basic': case_basic}

if __name__ == '__main__':
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for name in sys.argv[2:] or CASES:
        for f, data in CASES[name](outdir).items():
            open(os.path.join(outdir, f), 'wb').write(data)
        print(name)
