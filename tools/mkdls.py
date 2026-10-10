"""Writes a tiny DLS level 1 collection for tests: every GM program (and the drum kit) is a looped sine.
usage: mkdls.py out.dls"""
import math, struct, sys

def chunk(cid, data): return cid + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')
def lst(typ, *parts): return chunk(b'LIST', typ + b''.join(parts))

RATE, PERIOD = 26163, 100                       # 100 samples = one cycle of middle C (note 60)
wave = b''.join(struct.pack('<h', int(12000 * math.sin(2 * math.pi * i / PERIOD))) for i in range(PERIOD))
wsmp = struct.pack('<IHhiII', 20, 60, 0, 0, 0, 1) + struct.pack('<IIII', 16, 0, 0, PERIOD)
fmt = struct.pack('<HHIIHH', 1, 1, RATE, RATE * 2, 2, 16)
wvpl = lst(b'wvpl', lst(b'wave', chunk(b'fmt ', fmt), chunk(b'wsmp', wsmp), chunk(b'data', wave)))

def instrument(bank, prog):
    rgn = lst(b'rgn ', chunk(b'rgnh', struct.pack('<HHHHHH', 0, 127, 0, 127, 0, 0)), chunk(b'wsmp', wsmp),
              chunk(b'wlnk', struct.pack('<HHII', 0, 0, 1, 0)))
    return lst(b'ins ', chunk(b'insh', struct.pack('<III', 1, bank, prog)), lst(b'lrgn', rgn))

insts = [instrument(0, p) for p in range(128)] + [instrument(0x80000000, 0)]   # F_INSTRUMENT_DRUMS
ptbl = chunk(b'ptbl', struct.pack('<II', 8, 1) + struct.pack('<I', 0))
body = b'DLS ' + chunk(b'colh', struct.pack('<I', len(insts))) + lst(b'lins', *insts) + ptbl + wvpl
open(sys.argv[1], 'wb').write(chunk(b'RIFF', body))
