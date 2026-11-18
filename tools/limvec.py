"""reference outputs of ALchemy's limiter for tests/test_limiter.c: the same blocks through the DLL in the emulator
(ALCHEMY_DSOUND=/path/to/ALchemy/dsound.dll python3 tools/limvec.py; see limref.py)"""
import limref, numpy as np, struct
def lcg(s): return (s*1664525+1013904223)&0xffffffff
def run(rate, level_q, pattern, nblocks, seed):
    e=limref.Emu(rate); s=seed; h=0x811c9dc5; t=0
    sizes=[128,128,128,96,1,480,rate*3//1000,rate*3//1000+1,64,1024,2000,7]
    for b in range(nblocks):
        n=sizes[b%len(sizes)]; x=np.empty(n,dtype=np.float32)
        lv=np.float32(level_q*[1,1,0,2,1,0.25,4,1][(b//3)%8]/4.0)
        if pattern==4: s=lcg(s); kind=(s>>12)%6; pos=(s>>16)%n
        for i in range(n):
            s=lcg(s)
            if pattern==4:      # a block of one kind: a click in silence, a steady level, silence, noise, both ends, a ramp
                v=np.float32([1.0 if i==pos else 0.0, 1.0, 0.0, (((s>>8)&0xffff)-32768)/32768.0, 1.0 if i in (0,n-1) else 0.0, i/n][kind])
                x[i]=np.float32(v*lv); continue
            if pattern==0: v=np.float32((((s>>8)&0xffff)-32768)/32768.0)
            elif pattern==1: v=np.float32(1.0 if ((t+i)//37)%2 else -1.0)
            elif pattern==2: v=np.float32(1.0 if (s>>28)==0 else 0.0)
            else: v=np.float32((1.0 if (s>>8)&1 else -0.75) if (s>>22)==0 else 0.0)
            x[i]=np.float32(v*lv)
        t+=n
        y=e.process(x)
        for w in np.frombuffer(y.tobytes(),dtype='<u4'): h=((h^int(w))*0x01000193)&0xffffffff
        h=((h^struct.unpack('<I',struct.pack('<f',e.gain()))[0])*0x01000193)&0xffffffff
    return h
for rate in (8000,11025,22050,32000,44100,48000,88200,96000,192000):
    for pat in (0,1,2,3,4):
        for lq in (3,4,6,16,4000,4000000):
            print('    { %d, %d, %d, 0x%08xu },'%(rate,pat,lq,run(rate,lq,pat,200,rate+pat)))
