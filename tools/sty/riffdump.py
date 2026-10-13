#!/usr/bin/env python3
"""riffdump.py <file>: print a RIFF file's chunk tree (DirectMusic files), with the first bytes of each chunk."""
import struct, sys

def dump(d, off, end, depth):
    while off + 8 <= end:
        cid, size = struct.unpack_from('<4sI', d, off)
        if cid in (b'RIFF', b'LIST'):
            print('  ' * depth + '%s %s (%d)' % (cid.decode(), d[off + 8:off + 12].decode('latin1'), size))
            dump(d, off + 12, off + 8 + size, depth + 1)
        else:
            body = d[off + 8:off + 8 + size]
            print('  ' * depth + '%s (%d) %s' % (cid.decode('latin1'), size, body[:48].hex()))
        off += 8 + size + (size & 1)

d = open(sys.argv[1], 'rb').read()
dump(d, 0, len(d), 0)
