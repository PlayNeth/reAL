"""mktestdls.py <out.dls> <seed>: a random DLS collection exercising the synth: DLS2 articulations (both EGs with
delay/hold, LFO and vibrato to pitch/gain/cutoff, controller and velocity/key modulation, the low-pass with Q),
DLS1 articulations, 8 and 16 bit waves, loops (forward, to the end, short, release), one-shots, key/velocity
splits, exclusive groups, region attenuation/tuning, drum instruments."""
import math, struct, sys, random

def chunk(cid, data): return cid + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')
def lst(typ, *parts): return chunk(b'LIST', typ + b''.join(parts))
rnd = random.Random(int(sys.argv[2]))
def tc(sec): return -0x80000000 if sec <= 0 else int(1200 * math.log2(sec) * 65536)
def pc(hz): return int((1200 * math.log2(hz / 440.0) + 6900) * 65536)

# ---- waves
waves = []
for w in range(10):
    bits = rnd.choice([16, 16, 8])
    rate = rnd.choice([8000, 11025, 16000, 22050, 32000, 44100, 48000])
    n = rnd.randint(100, 3000) * 2
    kind = rnd.choice(['saw', 'sine', 'noise', 'decay', 'square'])
    per = rnd.randint(20, 200)
    smp = []
    for i in range(n):
        if kind == 'saw': v = ((i % per) / per) * 2 - 1
        elif kind == 'sine': v = math.sin(2 * math.pi * i / per)
        elif kind == 'noise': v = rnd.uniform(-1, 1)
        elif kind == 'square': v = 1 if (i % per) < per / 2 else -1
        else: v = math.sin(2 * math.pi * i / per) * math.exp(-4 * i / n)
        smp.append(v * rnd.uniform(0.3, 0.95))
    if bits == 16: data = b''.join(struct.pack('<h', int(v * 32767)) for v in smp)
    else: data = bytes((int(v * 127) + 128) & 0xff for v in smp)
    loop = rnd.choice(['none', 'fwd', 'end', 'short', 'release'])
    unity = rnd.randint(36, 84)
    if loop == 'none': loops = b''; nl = 0
    else:
        if loop == 'end': ls = rnd.randint(0, n - 50); ll = n - ls
        elif loop == 'short': ls = rnd.randint(0, n - 10); ll = rnd.randint(2, 8)
        else: ls = rnd.randint(0, n // 2); ll = rnd.randint(40, n - ls)
        loops = struct.pack('<IIII', 16, 1 if loop == 'release' else 0, ls, ll); nl = 1
    wsmp = struct.pack('<IHhiII', 20, unity, rnd.randint(-50, 50), 0, 0, nl) + loops
    fmt = struct.pack('<HHIIHH', 1, 1, rate, rate * bits // 8, bits // 8, bits)
    waves.append((lst(b'wave', chunk(b'fmt ', fmt), chunk(b'wsmp', wsmp), chunk(b'data', data)), wsmp))

import os
SKIP = set(int(x, 0) for x in os.environ.get('MKDLS_SKIP', '').split(',') if x)
def conn(src, ctl, dst, scale, tr=0):
    if dst in SKIP or (src << 16) in SKIP: return b''
    return struct.pack('<HHHHi', src, ctl, dst, tr, scale)

def art2():
    c = []
    c.append(conn(0, 0, 0x206, tc(rnd.choice([0, 0.001, 0.01, 0.05, 0.3]))))     # EG1 attack
    c.append(conn(0, 0, 0x207, tc(rnd.choice([0.05, 0.2, 1.0, 3.0]))))
    c.append(conn(0, 0, 0x20a, rnd.randint(0, 1000) << 16))
    c.append(conn(0, 0, 0x209, tc(rnd.choice([0.01, 0.1, 0.5, 2.0]))))
    if rnd.random() < 0.4: c.append(conn(0, 0, 0x20b, tc(rnd.choice([0.005, 0.05]))))     # delay
    if rnd.random() < 0.4: c.append(conn(0, 0, 0x20c, tc(rnd.choice([0.01, 0.1]))))       # hold
    if rnd.random() < 0.5: c.append(conn(2, 0, 0x206, tc(rnd.uniform(0.2, 4)) if False else rnd.randint(-2000, 2000) << 16))
    if rnd.random() < 0.5: c.append(conn(3, 0, 0x207, rnd.randint(-3000, 3000) << 16))
    c.append(conn(0, 0, 0x30a, tc(rnd.choice([0, 0.02, 0.2]))))                   # EG2
    c.append(conn(0, 0, 0x30b, tc(rnd.choice([0.1, 0.5, 1.5]))))
    c.append(conn(0, 0, 0x30e, rnd.randint(0, 1000) << 16))
    c.append(conn(0, 0, 0x30d, tc(rnd.choice([0.05, 0.4]))))
    if rnd.random() < 0.5: c.append(conn(5, 0, 3, rnd.randint(-1200, 1200) << 16))
    if rnd.random() < 0.3: c.append(conn(0, 0, 0x30f, tc(0.02)))
    if rnd.random() < 0.3: c.append(conn(0, 0, 0x310, tc(0.05)))
    if rnd.random() < 0.3: c.append(conn(3, 0, 0x30b, rnd.randint(-2000, 2000) << 16))
    if rnd.random() < 0.3: c.append(conn(2, 0, 0x30a, rnd.randint(-2000, 2000) << 16))
    if rnd.random() < 0.7:
        c.append(conn(0, 0, 0x104, pc(rnd.uniform(0.5, 9))))
        c.append(conn(0, 0, 0x105, tc(rnd.choice([0.0, 0.1, 0.5]))))
        if rnd.random() < 0.6: c.append(conn(1, 0, 3, rnd.randint(-100, 100) << 16))
        if rnd.random() < 0.5: c.append(conn(1, 0, 1, rnd.randint(-60, 60) << 16))
        if rnd.random() < 0.5: c.append(conn(1, 0x81, 3, rnd.randint(-200, 200) << 16))
        if rnd.random() < 0.4: c.append(conn(1, 0x81, 1, rnd.randint(-100, 100) << 16))
        if rnd.random() < 0.4: c.append(conn(1, 8, 3, rnd.randint(-200, 200) << 16))
        if rnd.random() < 0.3: c.append(conn(1, 8, 1, rnd.randint(-100, 100) << 16))
    if rnd.random() < 0.6:
        c.append(conn(0, 0, 0x114, pc(rnd.uniform(2, 8))))
        c.append(conn(0, 0, 0x115, tc(rnd.choice([0.0, 0.2]))))
        c.append(conn(9, 0, 3, rnd.randint(-50, 50) << 16))
        if rnd.random() < 0.5: c.append(conn(9, 0x81, 3, rnd.randint(-100, 100) << 16))
        if rnd.random() < 0.4: c.append(conn(9, 8, 3, rnd.randint(-100, 100) << 16))
    if rnd.random() < 0.8:     # low-pass
        c.append(conn(0, 0, 0x500, rnd.randint(5000, 13000) << 16))
        c.append(conn(0, 0, 0x501, rnd.randint(0, 225) << 16))
        if rnd.random() < 0.5: c.append(conn(2, 0, 0x500, rnd.randint(-3000, 3000) << 16))
        if rnd.random() < 0.5: c.append(conn(3, 0, 0x500, rnd.randint(-3000, 3000) << 16))
        if rnd.random() < 0.5: c.append(conn(5, 0, 0x500, rnd.randint(-4000, 4000) << 16))
        if rnd.random() < 0.5: c.append(conn(1, 0, 0x500, rnd.randint(-1000, 1000) << 16))
        if rnd.random() < 0.3: c.append(conn(1, 0x81, 0x500, rnd.randint(-1000, 1000) << 16))
        if rnd.random() < 0.3: c.append(conn(1, 8, 0x500, rnd.randint(-1000, 1000) << 16))
    if rnd.random() < 0.5: c.append(conn(0, 0, 4, rnd.randint(-500, 500) << 16))
    c = [x for x in c if x]
    return lst(b'lar2', chunk(b'art2', struct.pack('<II', 8, len(c)) + b''.join(c)))

def art1():
    c = [conn(0, 0, 0x206, tc(rnd.choice([0.001, 0.05]))), conn(0, 0, 0x207, tc(rnd.choice([0.2, 1.0]))),
         conn(0, 0, 0x20a, rnd.randint(0, 1000) << 16), conn(0, 0, 0x209, tc(rnd.choice([0.05, 0.5]))),
         conn(0, 0, 0x104, pc(rnd.uniform(1, 7))), conn(0, 0, 0x105, tc(0.1)), conn(1, 0, 3, rnd.randint(-50, 50) << 16),
         conn(1, 0x81, 3, 50 << 16), conn(0, 0, 0x30a, tc(0.01)), conn(0, 0, 0x30b, tc(0.3)), conn(5, 0, 3, 200 << 16),
         conn(0, 0, 4, rnd.randint(-500, 500) << 16)]
    c = [x for x in c if x]
    return lst(b'lart', chunk(b'art1', struct.pack('<II', 8, len(c)) + b''.join(c)))

def region(lo, hi, vlo, vhi, dls1):
    wi = rnd.randrange(len(waves))
    opts = 1 if rnd.random() < 0.2 else 0
    group = rnd.choice([0, 0, 0, 1, 2])
    rg = chunk(b'rgnh', struct.pack('<HHHHHHH', lo, hi, vlo, vhi, opts, group, 0))
    parts = [rg]
    if rnd.random() < 0.5:   # region wsmp overriding the wave's
        nl = rnd.choice([0, 1])
        wsmp = struct.pack('<IHhiII', 20, rnd.randint(36, 84), rnd.randint(-100, 100), -rnd.randint(0, 200) * 65536 // 10, 0, 0)
        parts.append(chunk(b'wsmp', wsmp))
    parts.append(chunk(b'wlnk', struct.pack('<HHII', 0, 0, 1, wi)))
    if rnd.random() < 0.5: parts.append(art1() if dls1 else art2())
    return lst(b'rgn2' if not dls1 else b'rgn ', *parts)

insts = []
used = set()
for k in range(12):
    dls1 = rnd.random() < 0.25
    drum = k >= 10
    while True:
        bank = 0x80000000 if drum else rnd.choice([0, 1, 0x100])
        prog = rnd.randint(0, 20)
        if (bank, prog) not in used: break
    used.add((bank, prog))
    regs = []
    split = sorted(rnd.sample(range(1, 127), rnd.randint(0, 3)))
    edges = [0] + split + [128]
    for a, b in zip(edges, edges[1:]):
        if rnd.random() < 0.3:
            vs = rnd.randint(30, 100)
            regs += [region(a, b - 1, 0, vs, dls1), region(a, b - 1, vs + 1, 127, dls1)]
        else: regs.append(region(a, b - 1, 0, 127, dls1))
    parts = [chunk(b'insh', struct.pack('<III', len(regs), bank, prog)), lst(b'lrgn', *regs)]
    parts.append(art1() if dls1 else art2())
    insts.append(lst(b'ins ', *parts))
# dmusic.dll rejects (DMUS_E_INVALIDOFFSET) a wave that ends the pool: pad with an unused one
pad = lst(b'wave', chunk(b'fmt ', struct.pack('<HHIIHH', 1, 1, 22050, 44100, 2, 16)), chunk(b'data', b'\0' * 64))
waves.append((pad, b''))
offs = []; pos = 0
for w, _ in waves: offs.append(pos); pos += len(w)
ptbl = chunk(b'ptbl', struct.pack('<II', 8, len(waves)) + b''.join(struct.pack('<I', o) for o in offs))
body = b'DLS ' + chunk(b'colh', struct.pack('<I', len(insts))) + lst(b'lins', *insts) + ptbl + lst(b'wvpl', *[w for w, _ in waves])
open(sys.argv[1], 'wb').write(chunk(b'RIFF', body))
