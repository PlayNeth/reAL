"""Ground truth for reAL's speakers.c from ALchemy's own code in the emulator:
   conv lines:  conv <in mask> <out mask> <matrix in x out, row-major over set bits>
   g3d lines:   g3d <layout> <n> <az> <el> <dist> <12 gains>"""
import os
import sys, struct, random
sys.path.insert(0, os.path.expanduser('~/re/tools'))
from emu import Emu
e = Emu(os.path.expanduser('~/re/bin/dsound.dll'))
out = open(sys.argv[1], 'w')
def bits(m): return [b for b in range(12) if m >> b & 1]

def conv(im, om):
    mode = {3: 4, 0xb: 4, 7: 5, 0xf: 5, 0x33: 6, 0x3b: 6}.get(om, 7)
    cv = e.alloc(0x2000)
    e.w32(cv + 0x10, mode); e.w32(cv + 0x14, om); e.w32(cv + 0x18, im); e.w32(cv + 0x20, len(bits(im)))
    e.uc.mem_write(cv + 0x28, b'\0' * 4)
    N = 16
    din, dout = e.alloc(0x40), e.alloc(0x40)
    e.w32(din + 4, N); e.w32(dout + 4, N)
    ib = bits(im)
    for k, b in enumerate(ib):
        p = e.alloc(4 * N); v = [0.0] * N; v[k] = 1.0; e.wfa(p, v); e.w32(din + 8 + 4 * b, p)
    obuf = {}
    for b in range(12):
        p = e.alloc(4 * N); e.wfa(p, [0.0] * N); e.w32(dout + 8 + 4 * b, p); obuf[b] = p
    e.call(0x10036740, dout, din, eax=cv)
    rows = []
    for k, b in enumerate(ib):
        rows.append([e.rfa(obuf[o], N)[k] for o in bits(om)])
    out.write('conv %x %x %s\n' % (im, om, ' '.join('%.9g' % x for r in rows for x in r)))

ins = [0x3f, 0x60f, 0x63f, 0xff, 0x33, 0x7, 0x13f, 0x737, 0x107, 0x83f, 0x6cf, 0x13, 0x603, 0x737 | 0x800, 0xb, 0x3]
outs = [0x3, 0x33, 0x3f, 0x63f, 0x60f, 0x7, 0xb, 0x3b]
for im in ins:
    for om in outs:
        conv(im, om)

def g3d(lay, n, az, el, dist):
    g = e.alloc(64)
    e.wfa(g, [1.0, 1.0] + [0.0] * 14)
    lo, hi = struct.unpack('<II', struct.pack('<d', dist))
    e.call(0x10034900, lay, ('f', az), ('f', el), lo, hi, ecx=g, ebx=n)
    v = e.rfa(g, 12)
    v[3] *= 0.5
    out.write('g3d %x %d %.9g %.9g %.17g %s\n' % (lay, n, az, el, dist, ' '.join('%.9g' % x for x in v)))

random.seed(7)
for lay, n in [(0x33, 4), (0x3f, 6), (0x63f, 8), (0xff, 8)]:
    for az in list(range(-180, 181, 5)) + [0.5, -0.5, 44.5, 45.0, 45.5, 89.999, 134.5, 135.0]:
        g3d(lay, n, float(az), 0.0, 3.0)
    for _ in range(400):
        az = random.uniform(-180, 180); el = random.uniform(-90, 90) if random.random() < 0.7 else random.choice([0.0, 30.0, -44.6, 45.0])
        dist = random.choice([random.uniform(0, 0.5), random.uniform(0.5, 30), 0.0, 0.5])
        g3d(lay, n, struct.unpack('<f', struct.pack('<f', az))[0], struct.unpack('<f', struct.pack('<f', el))[0], dist)
out.close()
