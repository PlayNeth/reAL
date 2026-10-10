"""dlcanon.py <dl.bin>: canonical text of captured synth download buffers (everything the synth reads; offsets,
extension chunks and copyright strings left out), for comparing dmusic's downloads with reAL-Music's"""
import sys, struct, hashlib
d = open(sys.argv[1], 'rb').read()
p = 0
while p + 8 <= len(d):
    tag, sz = struct.unpack_from('<II', d, p); b = d[p + 8:p + 8 + sz]; p += 8 + sz
    typ, dlid, nofs, cb = struct.unpack_from('<IIII', b, 0)
    ofs = struct.unpack_from('<%dI' % nofs, b, 16)
    if typ == 2:
        o = ofs[0]; wdi = struct.unpack_from('<I', b, o + 8)[0]
        fmt = struct.unpack_from('<HHIIHH', b, o + 12)
        n = struct.unpack_from('<I', b, ofs[wdi])[0]
        print('WAVE id %d fmt %s len %d md5 %s' % (dlid, fmt, n, hashlib.md5(b[ofs[wdi] + 4:ofs[wdi] + 4 + n]).hexdigest()))
        continue
    patch, firstreg, glob, ext, copy, flags = struct.unpack_from('<IIIIII', b, ofs[0])
    print('INSTR type %d patch %06x flags %x' % (typ, patch, flags))
    def art(i, ind):
        while i:
            ai, ext, nxt = struct.unpack_from('<III', b, ofs[i])
            if ai:
                cb2, n = struct.unpack_from('<II', b, ofs[ai])
                print(ind + 'connlist cb %d n %d' % (cb2, n))
                for k in range(n):
                    print(ind + ' conn src %x ctl %x dst %x tr %x scale %d' % struct.unpack_from('<HHHHi', b, ofs[ai] + cb2 + 12 * k))
            i = nxt
    art(glob, '  g ')
    r = firstreg
    while r:
        o = ofs[r]
        nxt = struct.unpack_from('<I', b, o + 0x10)[0]
        print('  region', b[o:o + 12].hex(), b[o + 0x18:o + 0x48].hex())
        art(struct.unpack_from('<I', b, o + 0xc)[0], '    ')
        r = nxt
