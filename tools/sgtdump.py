"""sgtdump.py <segment.sgt> [pchannel]: segment header, tempo/time signature tracks, sequence notes and curves."""
import sys, struct
d = open(sys.argv[1], 'rb').read()
pf = int(sys.argv[2]) if len(sys.argv) > 2 else None
def chunks(o, e):
    while o + 8 <= e:
        cid = d[o:o+4]; sz = struct.unpack_from('<I', d, o + 4)[0]
        yield cid, o + 8, sz
        o += 8 + sz + (sz & 1)
def find(o, e, path):
    for cid, b, sz in chunks(o, e):
        if cid in (b'RIFF', b'LIST'):
            yield from find(b + 4, b + sz, path + [d[b:b+4]])
        elif cid == b'seqt':
            yield from find(b, b + sz, path + [cid])
        else: yield path, cid, b, sz
for path, cid, b, sz in find(0, len(d), []):
    if cid == b'segh':
        print('segh repeats %d length %d playstart %d loopstart %d loopend %d resolution %d' % struct.unpack_from('<IiiiiI', d, b))
        if sz >= 40: print('    rtLength %d flags %x' % struct.unpack_from('<qI', d, b + 24))
    elif cid == b'tetr':
        es = struct.unpack_from('<I', d, b)[0]
        for o in range(b + 4, b + sz, es): print('tempo mt %d bpm %f' % (struct.unpack_from('<i', d, o)[0], struct.unpack_from('<d', d, o + 8)[0]))
    elif cid == b'tims':
        es = struct.unpack_from('<I', d, b)[0]
        for o in range(b + 4, b + sz, es): print('timesig mt %d %d/%d grid %d' % struct.unpack_from('<iBBH', d, o))
    elif cid == b'evtl':
        es = struct.unpack_from('<I', d, b)[0]
        for o in range(b + 4, b + sz, es):
            mt, dur, pch, off, st, b1, b2 = struct.unpack_from('<iiIhBBB', d, o)
            if pf is None or pch == pf: print('note mt %d dur %d pch %d off %d %02x %d %d' % (mt, dur, pch, off, st, b1, b2))
    elif cid == b'curl':
        es = struct.unpack_from('<I', d, b)[0]
        for o in range(b + 4, b + sz, es):
            mt, dur, rst, pch, off, cs, ce, rv, tp, shape, cc, fl = struct.unpack_from('<iiiIhhhhBBBB', d, o)
            extra = struct.unpack_from('<HH', d, o + 28) if es >= 32 else ()
            if pf is None or pch == pf:
                print('curve mt %d dur %d reset %d pch %d off %d %d->%d resetval %d type %d shape %d cc %d flags %d %s' % (mt, dur, rst, pch, off, cs, ce, rv, tp, shape, cc, fl, extra))
