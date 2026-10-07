import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from pe import *
# Usage: python3 tools/rtti.py path/to/dsound.dll  -> prints class/vtable map, writes vtables.json
rdata=[s for s in P.sections if s.Name.startswith(b'.rdata')][0]
RS,RE_=IB+rdata.VirtualAddress, IB+rdata.VirtualAddress+rdata.Misc_VirtualSize
# type descriptors: name string at TD+8
tds={}
for m in re.finditer(rb'\.\?A[VU][^\0]+\0', DATA):
    td=IB+m.start()-8; tds[td]=m.group(0)[:-1].decode()
# COLs: signature 0, offset, cdOffset, pTD, pCHD
vt={}
for a in range(RS, RE_-20, 4):
    if u32(a)==0 and u32(a+12) in tds:
        col=a
        # vtable = location after pointer to col
        pat=struct.pack('<I',col)
        for mm in re.finditer(re.escape(pat), DATA[RS-IB:RE_-IB]):
            p=RS+mm.start()
            if p%4: continue
            v=p+4; ents=[]
            while TS<=u32(v+4*len(ents))<TE: ents.append(u32(v+4*len(ents)))
            if ents: vt.setdefault(tds[u32(col+12)],[]).append((v,u32(col+4),ents))
import json
for k in sorted(vt):
    for v,off,ents in vt[k]:
        print(f'{k:60} vtbl={v:08x} off={off} n={len(ents)}  first={ents[0]:08x}')
json.dump({k:[(v,o,e) for v,o,e in L] for k,L in vt.items()}, open('vtables.json','w'))
