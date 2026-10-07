"""Tiny RE helpers for ALchemy's dsound.dll: string xrefs, annotated disassembly.
   dis(va, n), xrefs(va), find_str(s, wide=False), cstr(va), u32(va)
   Requires: pip install pefile capstone"""
import pefile, capstone, struct, sys, re
# Usage: python3 -i tools/pe.py path/to/ALchemy/dsound.dll   (or set ALCHEMY_DLL)
import os
_path = next((a for a in sys.argv[1:] if a.lower().endswith('.dll')), os.environ.get('ALCHEMY_DLL', 'dsound.dll'))
P = pefile.PE(_path)
IB = P.OPTIONAL_HEADER.ImageBase
DATA = P.get_memory_mapped_image()
txt = [s for s in P.sections if s.Name.startswith(b'.text')][0]
TS, TE = IB+txt.VirtualAddress, IB+txt.VirtualAddress+txt.Misc_VirtualSize
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail=False
def rd(va,n): return DATA[va-IB:va-IB+n]
def u32(va): return struct.unpack('<I',rd(va,4))[0]
def cstr(va):
    b=DATA[va-IB:va-IB+512]; return b.split(b'\0')[0].decode('latin1')
def wstr(va):
    b=DATA[va-IB:va-IB+1024]; i=0
    while i<len(b)-1 and b[i:i+2]!=b'\0\0': i+=2
    return b[:i].decode('utf-16le','replace')
def find_str(s, wide=False):
    pat = s.encode('utf-16le') if wide else s.encode()+b'\0'
    return [IB+m.start() for m in re.finditer(re.escape(pat), DATA)]
def xrefs(va):
    pat = struct.pack('<I',va)
    out=[]
    for m in re.finditer(re.escape(pat), DATA[TS-IB:TE-IB]): out.append(TS+m.start())
    return out
IMPORTS={}
for d in P.DIRECTORY_ENTRY_IMPORT:
    for i in d.imports: IMPORTS[i.address]=(d.dll.decode()+'!'+(i.name.decode() if i.name else str(i.ordinal)))
def annotate(ins):
    s=f'{ins.address:08x}  {ins.mnemonic:6} {ins.op_str}'
    for m in re.finditer(r'0x[0-9a-f]{6,8}', ins.op_str):
        v=int(m.group(0),16)
        if v in IMPORTS: s+=f'   ; {IMPORTS[v]}'
        elif IB<=v<IB+len(DATA):
            c=cstr(v)
            if len(c)>=4 and all(32<=ord(x)<127 for x in c): s+=f'   ; "{c[:80]}"'
            else:
                w=wstr(v)
                if len(w)>=3 and all(32<=ord(x)<127 for x in w): s+=f'   ; L"{w[:80]}"'
    return s
def dis(va, n=80, stop_ret=True):
    out=[]
    for ins in md.disasm(rd(va, n*8), va):
        out.append(annotate(ins))
        if len(out)>=n: break
        if stop_ret and ins.mnemonic=='ret': break
    return '\n'.join(out)
def func_start(va):
    # walk back to find typical prologue after int3/ret padding
    a=va
    while a>TS:
        a-=1
        b=rd(a,3)
        if rd(a-1,1) in (b'\xcc',b'\xc3',b'\x90') and (b[:3]==b'\x55\x8b\xec' or b[:1] in (b'\x83',b'\x81',b'\x56',b'\x53',b'\x57',b'\x6a',b'\x51',b'\x8b',b'\xa1',b'\x55')):
            if rd(a-2,1) in (b'\xcc',b'\xc3',b'\x90') or rd(a-3,3)[0:1]==b'\xc2': return a
    return None
