#!/usr/bin/env python3
"""patch_dsdmo.py <dsdmo.dll> <out.dll>: a copy of Microsoft's dsdmo.dll that imports msvcrx.dll (tools/dsdmo/msvcrx.c)
instead of msvcrt.dll, so it runs under Wine (whose msvcrt has no _libm_sse2_*_precise)."""
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
pe = struct.unpack_from('<I', d, 0x3c)[0]
ns = struct.unpack_from('<H', d, pe + 6)[0]
so = pe + 0x18 + struct.unpack_from('<H', d, pe + 0x14)[0]
secs = [struct.unpack_from('<8sIIII', d, so + 40 * i) for i in range(ns)]
def off(r):
    for n, vs, va, rs, ro in secs:
        if va <= r < va + max(vs, rs): return r - va + ro
o = off(struct.unpack_from('<I', d, pe + 0x80)[0])
done = 0
while True:
    nm = struct.unpack_from('<I', d, o + 12)[0]
    if not nm: break
    p = off(nm)
    if bytes(d[p:p + 11]).lower() == b'msvcrt.dll\0':
        d[p:p + 10] = b'msvcrx.dll'; done += 1
    o += 20
if done != 1: sys.exit('msvcrt.dll import not found')
struct.pack_into('<I', d, pe + 0x58, 0)     # the checksum no longer holds
open(sys.argv[2], 'wb').write(d)
