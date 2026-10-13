#!/usr/bin/env python3
"""gen.py <out.raw> <rate> <channels> <bits>: a 3 s test signal (tones, a sweep, noise bursts, silence, full-scale
peaks) as raw PCM (8/16-bit integer or 32-bit float)"""
import math, random, struct, sys
out, rate, ch, bits = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
random.seed(7)
n = rate * 3
s = []
for i in range(n):
    t = i / rate
    for c in range(ch):
        if t < 0.8: v = 0.6 * math.sin(2 * math.pi * (220 + 110 * c) * t) + 0.2 * math.sin(2 * math.pi * 3000 * t)
        elif t < 1.6: v = 0.7 * math.sin(2 * math.pi * (50 + 4000 * (t - 0.8) ** 2) * t)
        elif t < 2.0: v = (random.random() * 2 - 1) * (0.9 if int(t * 20) % 2 else 0.1)
        elif t < 2.4: v = 0.0
        else: v = 1.0 if int(t * 200) % 2 else -1.0
        s.append(v)
if bits == 8: data = bytes(max(0, min(255, int(round(v * 127)) + 128)) for v in s)
elif bits == 16: data = struct.pack('<%dh' % len(s), *[max(-32768, min(32767, int(round(v * 32767)))) for v in s])
else: data = struct.pack('<%df' % len(s), *s)
open(out, 'wb').write(data)
