#!/usr/bin/env python3
"""Level per speaker for each step of tests/surround.c, from a recording of the device.
   surround_check.py rec.wav out.txt [channel names...]
The recording (parecord --format=float32le --file-format=wav from the sink's monitor) is lined up on the first
sound, which is the first step."""
import sys, struct, math, re

def read_wav(path):
    d = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, sz = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + sz] if sz != 0xffffffff else d[pos + 8:]
        if cid == b'fmt ': fmt = struct.unpack('<HHIIHH', body[:16])
        if cid == b'data': data = body
        if sz == 0xffffffff: break
        pos += 8 + sz + (sz & 1)
    tag, ch, rate, _, align, bits = fmt
    n = len(data) // align
    vals = struct.unpack('<%df' % (n * ch), data[:n * align])
    return rate, ch, [vals[c::ch] for c in range(ch)]

rate, ch, x = read_wav(sys.argv[1])
names = sys.argv[3:] or ['FL', 'FR', 'FC', 'LFE', 'BL', 'BR', 'SL', 'SR'][:ch]
steps = []
for l in open(sys.argv[2]):
    m = re.match(r'\s*(\d+) (.*)', l)
    if m: steps.append((int(m.group(1)), m.group(2).strip()))
start = next(i for i in range(len(x[0])) if any(abs(x[c][i]) > 1e-3 for c in range(ch)))
print('%-22s' % 'step' + ''.join('%8s' % n for n in names))
for t, what in steps:
    if what == 'end': break
    a = start + int((t + 150) * rate / 1000); b = a + int(0.35 * rate)
    lv = []
    for c in range(ch):
        s = x[c][a:b]
        r = math.sqrt(sum(v * v for v in s) / max(len(s), 1))
        lv.append('%8s' % ('-' if r < 1e-4 else '%.1f' % (20 * math.log10(r / (8000 / 32768 / math.sqrt(2))))))
    print('%-22s' % what + ''.join(lv))
