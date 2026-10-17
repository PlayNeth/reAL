#!/usr/bin/env python3
"""Ground truth for tests/test_cmss3.c: ALchemy's CCMSS3Upmix (dsound.dll, upmixer object 0xb6be4 bytes,
component at +0x76104) run in the emulator.
   gen_cmss3_gt.py tests/gt/cmss3_gt.bin
Input: the LCG noise of tests/test_cmss3.c; output: 512 frames of every produced speaker from frame 10 bs."""
import os
import sys, struct
import numpy as np
sys.path.insert(0, os.path.expanduser('~/re/tools'))
from emu import Emu

class CM3:
    """constructor 0x1003a050, init 0x1003a1d0, configure 0x1003a2d0, process 0x1003a740.  The object is
    placed so the component is 12 mod 16: there the rear gate (a raw pointer, component+0x3a674)
    is the 16-byte aligned level-similarity array, as reAL assumes."""
    def __init__(self, omask, rate, inten=0, hp=False, variant=0, enable=1):
        e = self.e = Emu(os.path.expanduser('~/re/bin/dsound.dll'))
        e.uc.mem_map(0, 0x1000)
        self.conv = conv = e.alloc(0x2000)
        e.call(0x10035cb0, edi=conv)
        self.cont = cont = e.alloc(0x40)
        obj = e.alloc(0xb6be4 + 32)
        while (obj + 0x76104) % 16 != 12: obj += 4
        self.obj = obj
        self.C = obj + 0x76104
        e.call(0x1003a050, cont, esi=obj, ecx=conv)
        e.w32(cont + 0, obj); e.w32(cont + 8, obj)
        e.w32(cont + 0xc, 1 if hp else 0); e.w32(cont + 0x10, variant)
        e.w32(cont + 0x14, omask); e.w32(cont + 0x18, rate); e.w32(cont + 0x1c, 0x3f)
        for m in range(3):
            e.w32(obj + 0x20 + 4 * m, enable); e.w32(obj + 0x2c + 4 * m, inten)
        e.call(0x1003a1d0, 30000, ecx=obj)
        e.call(0x1003a2d0, ecx=obj)
        self.n = 0

    def alloc_io(self, n):
        e = self.e
        self.n = n
        self.ind = e.alloc(0x40); self.outd = e.alloc(0x40)
        self.inl = e.alloc(4 * n); self.inr = e.alloc(4 * n)
        e.w32(self.ind + 4, n); e.w32(self.ind + 8, self.inl); e.w32(self.ind + 0xc, self.inr)
        e.w32(self.ind + 0x10, 2); e.w32(self.ind + 0x14, 3)
        self.obuf = []
        for b in range(12):
            p = e.alloc(4 * n); self.obuf.append(p); e.w32(self.outd + 8 + 4 * b, p)
        e.w32(self.outd + 4, n)

    def block(self, L, R):
        """one call of the process method; returns 12 lists (per speaker bit)"""
        e = self.e
        n = len(L)
        if n != self.n: self.alloc_io(n)
        e.wfa(self.inl, L); e.wfa(self.inr, R)
        for b in range(12): e.wfa(self.obuf[b], [0.0] * n)
        res = e.call(0x1003a740, self.outd, self.ind, ecx=self.obj)
        src = self.outd if res == 0 else res
        out = []
        for b in range(12):
            p = e.r32(src + 8 + 4 * b)
            out.append(e.rfa(p, n) if p else [0.0] * n)
        return out

CASES = [(48000, 0x3f, 0), (48000, 0x63f, -50), (44100, 0x33, 30), (96000, 0x63f, 0), (22050, 0x3f, -100),
         (48000, 0x3, -20)]
START_BLOCKS, COUNT = 10, 512

def noise(n):
    seed = 1
    out = np.zeros((2, n), np.float32)
    def rnd():
        nonlocal seed
        seed = (seed * 1103515245 + 12345) & 0xffffffff
        return np.float32(((seed >> 9) & 0xffff) - 32768) / np.float32(32768.0)
    for i in range(n):
        c, l, r = rnd(), rnd(), rnd()
        out[0, i] = np.float32(0.35) * c + np.float32(0.25) * l
        out[1, i] = np.float32(0.35) * c + np.float32(0.25) * r
    return out

with open(sys.argv[1], 'wb') as f:
    f.write(struct.pack('<I', len(CASES)))
    for rate, mask, inten in CASES:
        bs = ((rate + 23999) // 24000) * 512
        n = bs * (START_BLOCKS + 1)
        x = noise(n)
        c = CM3(mask, rate, inten=inten)
        res = np.zeros((12, n), np.float32)
        # stereo returns ALchemy's internal quad, whose buffers hold 576 frames: smaller calls there
        ch = 512 if mask == 3 else bs
        for s in range(0, n, ch):
            o = c.block(list(map(float, x[0, s:s + ch])), list(map(float, x[1, s:s + ch])))
            for b in range(12): res[b, s:s + ch] = o[b]
        bits = [b for b in range(12) if ((0x33 if mask == 3 else mask) & ~8) >> b & 1]
        s0 = START_BLOCKS * bs
        f.write(struct.pack('<iiiIII', rate, mask, inten, s0, COUNT, len(bits)))
        for b in bits:
            f.write(struct.pack('<I', b))
            f.write(res[b, s0:s0 + COUNT].astype('<f4').tobytes())
        print(rate, hex(mask), inten, 'bits', bits)
