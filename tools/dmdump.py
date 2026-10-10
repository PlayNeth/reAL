"""dmdump.py - list the RIFF structure of DirectMusic files (.sgt/.sty/.dls/.bnd...)
usage: python3 dmdump.py [-s] file...   (-s: one summary line per segment: track types, refs)"""
import struct, sys, uuid

TRACKS = {
    'd2ac2897-b39b-11d1-8704-00600893b1bd': 'Pattern',
    'bae4d665-4ea1-11d3-8bda-00600893b1b6': 'SegTrigger',
    'd2ac2894-b39b-11d1-8704-00600893b1bd': 'Band',
    'd2ac288b-b39b-11d1-8704-00600893b1bd': 'Chord',
    'd2ac2896-b39b-11d1-8704-00600893b1bd': 'ChordMap',
    'd2ac288c-b39b-11d1-8704-00600893b1bd': 'Command',
    '995c1cf5-54ff-11d3-8bda-00600893b1b6': 'Lyrics',
    '55a8fd00-4288-11d3-9bd1-8a0d61c88835': 'Marker',
    'd2ac288e-b39b-11d1-8704-00600893b1bd': 'Motif',
    'd2ac2898-b39b-11d1-8704-00600893b1bd': 'Mute',
    '4be0537b-5c19-11d3-8bdc-00600893b1b6': 'ParamControl',
    '4108fa85-3586-11d3-8bd7-00600893b1b6': 'Script',
    'd2ac2886-b39b-11d1-8704-00600893b1bd': 'Seq',
    'f17e8672-c3b4-11d1-870b-00600893b1bd': 'SignPost',
    'd2ac288d-b39b-11d1-8704-00600893b1bd': 'Style',
    'd2ac2887-b39b-11d1-8704-00600893b1bd': 'SysEx',
    'd2ac2885-b39b-11d1-8704-00600893b1bd': 'Tempo',
    'd2ac2888-b39b-11d1-8704-00600893b1bd': 'TimeSig',
    'eed36461-9ea5-11d3-9bd1-0080c7150a74': 'Wave',
    'b0684266-b57f-11d2-97f9-00c04fa36e58': 'MelodyFormulation',
}

def guid(b): return str(uuid.UUID(bytes_le=bytes(b)))

def walk(d, off, end, depth, out, summ):
    while off + 8 <= end:
        cid = d[off:off+4].decode('latin1'); size = struct.unpack_from('<I', d, off+4)[0]
        body = off + 8
        if cid in ('RIFF', 'LIST'):
            typ = d[body:body+4].decode('latin1')
            out.append('  ' * depth + f'{cid} {typ} ({size})')
            walk(d, body + 4, body + size, depth + 1, out, summ)
        else:
            extra = ''
            if cid == 'trkh':
                g = guid(d[body:body+16]); n = TRACKS.get(g, g)
                fcc = d[body+24:body+28].decode('latin1', 'replace'); lst = d[body+28:body+32].decode('latin1', 'replace')
                extra = f' {n} ckid={fcc!r} list={lst!r}'
                summ['tracks'].append(n)
            elif cid in ('UNAM', 'UFIL', 'UCAT', 'name', 'file'):
                extra = ' ' + d[body:body+size].decode('utf-16-le', 'replace').rstrip('\0')
                if cid == 'file' and extra.strip() not in summ['refs']: summ['refs'].append(extra.strip())
            elif cid == 'guid' and size == 16:
                extra = ' ' + guid(d[body:body+16])
            elif cid == 'refh':
                extra = ' class=' + guid(d[body:body+16])
            elif cid == 'segh':
                v = struct.unpack_from('<IIIQQ', d, body)
                extra = f' repeats={v[0]} length(mt)={v[1]} playstart={v[2]}'
            out.append('  ' * depth + f'{cid} ({size}){extra}')
        off = body + size + (size & 1)

for a in sys.argv[1:]:
    if a == '-s': continue
    d = open(a, 'rb').read(); out = []; summ = {'tracks': [], 'refs': []}
    walk(d, 0, len(d), 0, out, summ)
    if '-s' in sys.argv:
        print(f'{a.split("/")[-1]:24s} {",".join(summ["tracks"])}  refs={summ["refs"]}')
    else:
        print('==', a); print('\n'.join(out))
