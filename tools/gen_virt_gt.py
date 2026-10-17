#!/usr/bin/env python3
"""Ground truth for tests/test_virt.c: ALchemy's channel converter (fn 0x10036740) on a stereo output in
its virtualizer modes, run in the emulator: mode 0 (headphones, CMSS-3D's) and mode 3 (speakers).
   gen_virt_gt.py tests/gt/virt_gt.bin
Input: quad (FL FR BL BR) LCG noise as in tests/test_virt.c; output: the last 1024 frames of L and R."""
import os
import sys, struct
import numpy as np
sys.path.insert(0, os.path.expanduser('~/re/tools'))
from emu import Emu

def bits(m): return [b for b in range(12) if m >> b & 1]

class Conv:
    """converter object (constructor 0x10035cb0, buffers 0x10035ef0) set up like CMSS-3D's on a stereo device
    (fn 0x1003a2d0): out mask 3, in mask 0x33, virtualizer on (+8), headphones flag (+1), variant 0"""
    def __init__(self, rate, hp):
        e = self.e = Emu(os.path.expanduser('~/re/bin/dsound.dll'))
        e.uc.mem_map(0, 0x1000)
        cv = self.cv = e.alloc(0x2000)
        e.call(0x10035cb0, edi=cv)
        e.uc.mem_write(cv + 1, bytes([hp]))
        e.w32(cv + 4, 0)
        e.uc.mem_write(cv + 8, b'\x01')
        e.w32(cv + 0xc, rate)
        e.w32(cv + 0x14, 3); e.w32(cv + 0x18, 0x33); e.w32(cv + 0x20, 4)
        e.call(0x10035ef0, esi=cv, edi=4096)
        e.w32(cv + 0x28, 0x1f)
        self.n = 0

    def run(self, chans):
        e = self.e
        n = len(chans[0])
        if n != self.n:
            self.n = n
            self.din, self.dout = e.alloc(0x40), e.alloc(0x40)
            self.ib, self.ob = {}, {}
            for b in range(12):
                p = (e.alloc(4 * n + 64) + 15) & ~15; self.ib[b] = p; e.w32(self.din + 8 + 4 * b, p if 0x33 >> b & 1 else 0)
                p = (e.alloc(4 * n + 64) + 15) & ~15; self.ob[b] = p; e.w32(self.dout + 8 + 4 * b, p)
            e.w32(self.din + 4, n); e.w32(self.dout + 4, n)
        for b in range(12):
            e.wfa(self.ib[b], list(map(float, chans.get(b, [0.0] * n))))
            e.wfa(self.ob[b], [0.0] * n)
        e.call(0x10036740, self.dout, self.din, eax=self.cv)
        return e.rfa(self.ob[0], n), e.rfa(self.ob[1], n)

# rate, headphones, chunk (512: ALchemy's early reflections with taps shorter than the block)
CASES = [(48000, 1, 64), (44100, 1, 64), (22050, 1, 64), (48000, 1, 512), (48000, 0, 64)]
N, KEEP = 8192, 1024

def noise(n):
    seed = 1
    out = np.zeros((4, n), np.float32)
    for i in range(n):
        for c in range(4):
            seed = (seed * 1103515245 + 12345) & 0xffffffff
            out[c, i] = np.float32(((seed >> 9) & 0xffff) - 32768) / np.float32(32768.0) * np.float32(0.25)
    return out

x = noise(N)
with open(sys.argv[1], 'wb') as f:
    f.write(struct.pack('<I', len(CASES)))
    for rate, hp, chunk in CASES:
        c = Conv(rate, hp)
        L = np.zeros(N, np.float32); R = np.zeros(N, np.float32)
        for s in range(0, N, chunk):
            l, r = c.run({0: x[0, s:s + chunk], 1: x[1, s:s + chunk], 4: x[2, s:s + chunk], 5: x[3, s:s + chunk]})
            L[s:s + chunk] = l; R[s:s + chunk] = r
        f.write(struct.pack('<IIIII', rate, hp, chunk, N, KEEP))
        f.write(L[N - KEEP:].astype('<f4').tobytes()); f.write(R[N - KEEP:].astype('<f4').tobytes())
        print(rate, 'headphones' if hp else 'speakers', chunk, 'mode', c.e.r32(c.cv + 0x10))
