#!/usr/bin/env python3
"""fuzzcmp - run dsfuzz.exe under Windows' DirectSound and under reAL (tests/comcmp.sh) and report, for each seed,
the first call whose result differs.  After a first difference the two runs may go apart (a buffer made on one side
only), so only the first one per seed counts; many seeds give many first differences, grouped by call.

    tests/fuzzcmp.py <first seed> <seeds> [ops per seed] [--raw] [--show N]

Known differences, where reAL keeps ALchemy's answers on purpose (docs/STATUS.md), are taken out first:
the LOCHARDWARE / LOCSOFTWARE bits of GetCaps and GetStatus.  --raw keeps them.
Environment: COMCMP (path of comcmp.sh), COMCMP_DIR (its work directory)."""
import os, re, subprocess, sys
from collections import defaultdict

def norm(line, raw):
    if raw:
        return line
    # GetCaps flags: LOCHARDWARE 0x4 / LOCSOFTWARE 0x8 (reAL answers hardware, as ALchemy)
    line = re.sub(r'(caps [0-9a-f]{8} flags |GetCaps\(size \d+\) [0-9a-f]{8} flags )([0-9a-f]+)',
                  lambda m: m.group(1) + '%x' % (int(m.group(2), 16) & ~0xc), line)
    # GetStatus: LOCHARDWARE 0x8 / LOCSOFTWARE 0x10
    line = re.sub(r'(status (?:[0-9a-f]{8} )?)([0-9a-f]+)\b',
                  lambda m: m.group(1) + '%x' % (int(m.group(2), 16) & ~0x18), line)
    line = re.sub(r'(GetStatus [0-9a-f]{8} )([0-9a-f]+)', lambda m: m.group(1) + '%x' % (int(m.group(2), 16) & ~0x18), line)
    # a 3D algorithm asked for: Windows answers DS_NO_VIRTUALIZATION, reAL DS_OK (ALchemy's)
    line = line.replace(') 0878000a set', ') 00000000 set')
    # DirectSound GetCaps: reAL reports ALchemy's device (192000 Hz at most)
    line = re.sub(r'(ds\d GetCaps\(size \d+\) [0-9a-f]{8} min \d+ max )\d+', r'\1*', line)
    return line

def hr_of(line):
    """the call's HRESULT: the first 8-digit hex number after its arguments"""
    m = re.search(r'\) ([0-9a-f]{8})\b', line) or re.search(r' ([0-9a-f]{8})\b', line)
    return int(m.group(1), 16) if m else None

def severity(l1, l2):
    """1: reAL refuses what Windows takes, 2: both take it but answer differently, 3: Windows refuses what reAL takes,
    4: both refuse, with other codes"""
    a, b = hr_of(l1), hr_of(l2)
    if a is None or b is None:
        return 2
    fa, fb = a >= 0x80000000, b >= 0x80000000
    if fb and not fa: return 1
    if fa and not fb: return 3
    if fa and fb: return 4
    return 2

LABEL = { 1: 'reAL refuses, Windows accepts', 2: 'different answers', 3: 'Windows refuses, reAL accepts (lenient)',
          4: 'both refuse, other codes (not counted as a first difference)' }

def is_primary(line):
    m = re.match(r'\d+ (?:p\d+ |ds\d CreateSoundBuffer\(size \d+ flags ([0-9a-f]+))', line)
    return bool(m) and (m.group(1) is None or int(m.group(1), 16) & 1)

def seeds(text):
    out, cur = {}, None
    for l in text.splitlines():
        if l.startswith('SEED '):
            cur = int(l.split()[1]); out[cur] = []
        elif cur is not None and l != 'END':
            out[cur].append(l)
    return out

def sig(line):
    """the call, without its arguments' values: '<object kind> <method>'"""
    m = re.match(r'\d+ (ds\d|[bp]\d+) ([A-Za-z.]+)', line)
    if not m:
        return line.split(' ')[0]
    return ('p ' if m.group(1).startswith('p') else 'ds ' if m.group(1).startswith('ds') else 'b ') + m.group(2)

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    raw = '--raw' in sys.argv
    show = 3
    if '--show' in sys.argv:
        show = int(sys.argv[sys.argv.index('--show') + 1]); a.remove(str(show))
    first, n = int(a[0]), int(a[1])
    ops = a[2] if len(a) > 2 else '200'
    comcmp = os.environ.get('COMCMP', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'comcmp.sh'))
    wd = os.environ.get('COMCMP_DIR', '/tmp/real/sp/fz')
    env = dict(os.environ, COMCMP_DIR=wd, COMCMP_TIMEOUT=os.environ.get('COMCMP_TIMEOUT', '1200'))
    ms, rl = {}, {}
    chunk = int(os.environ.get('FUZZ_CHUNK', '25'))     # (a crash loses the rest of its chunk only)
    for c in range(first, first + n, chunk):
        k = min(chunk, first + n - c)
        subprocess.run([comcmp, 'dsfuzz.exe', str(c), str(k), ops], env=env, stdout=subprocess.DEVNULL)
        ms.update(seeds(open(os.path.join(wd, 'ms.txt'), errors='replace').read()))
        rl.update(seeds(open(os.path.join(wd, 'real.txt'), errors='replace').read()))
        if '--progress' in sys.argv:
            print('seeds %d-%d done' % (c, c + k - 1), file=sys.stderr)
    groups = defaultdict(list)
    same = 0
    for s in range(first, first + n):
        x, y = ms.get(s), rl.get(s)
        if x is None or y is None:
            groups[(0, '(seed missing: %s)' % ('both' if x is None and y is None else 'Windows' if x is None else 'reAL'))].append((s, '', ''))
            continue
        # matched by operation number (an operation may print on one side only, when the two went apart)
        def byop(lines):
            d, order = {}, []
            for l in lines:
                k = l.split(' ', 1)[0]
                if k not in d: order.append(k)
                d.setdefault(k, l)
            return d, order
        dx, ox = byop(x)
        dy, oy = byop(y)
        keys = ox + [k for k in oy if k not in dx]
        for k in keys:
            l1 = dx.get(k, '(nothing)')
            l2 = dy.get(k, '(nothing)')
            if norm(l1, raw) != norm(l2, raw):
                if is_primary(l1) and is_primary(l2) and not raw:
                    # the primary buffer: reAL keeps ALchemy's below DSSCL_WRITEPRIMARY (4096 bytes, its Play,
                    # SetFormat, position ...); listed apart (the two runs may go apart from here: the seed ends)
                    groups[(5, sig(l1))].append((s, l1, l2))
                    break
                sv = severity(l1, l2)
                if sv == 1 and re.search(r'(Set\w+|L\.Set\w+)\([^)]*<[0-9a-f]{8}>', l1):
                    # an infinite or NaN 3D value: Windows takes it (and its maths gives NaN), reAL refuses it as
                    # ALchemy does; the two runs go apart from here
                    groups[(6, sig(l1))].append((s, l1, l2))
                    break
                groups[(sv, sig(l1))].append((s, l1, l2))
                if sv == 4 and l1.split(' ', 1)[0] == l2.split(' ', 1)[0]:
                    continue        # (both refused: nothing changed on either side, so the comparison goes on)
                break
        else:
            same += 1
    print('%d of %d seeds the same' % (same, n))
    LABEL[0] = 'missing'; LABEL[5] = 'primary buffer (known: ALchemy\'s)'; LABEL[6] = 'non-finite 3D value refused (known: ALchemy\'s)'
    for g, v in sorted(groups.items(), key=lambda kv: (kv[0][0], -len(kv[1]))):
        print('\n== [%s] %s: %d seed%s' % (LABEL[g[0]], g[1], len(v), '' if len(v) == 1 else 's'))
        for s, l1, l2 in v[:show]:
            print('  seed %d\n    Windows: %s\n    reAL:    %s' % (s, l1, l2))

if __name__ == '__main__':
    main()
