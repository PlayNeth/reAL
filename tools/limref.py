"""ALchemy's output limiter (CLimiterChan's block function, 0x1002cb30 in ALchemy's dsound.dll, file version 2.4.2.1) run in
an emulator, and a model of it in Python (what src/limiter.c is a translation of); the two compared on random blocks.

    ALCHEMY_DSOUND=/path/to/ALchemy/dsound.dll python3 tools/limref.py [seed] [cases]

Needs unicorn, pefile and numpy.  The DLL is not part of reAL.  tools/limvec.py makes the reference hashes of
tests/test_limiter.c the same way."""
import struct, random, math, sys, os
import numpy as np
import pefile
from unicorn import *
from unicorn.x86_const import *
DLL=os.environ.get('ALCHEMY_DSOUND','dsound.dll')
pe=pefile.PE(DLL); base=pe.OPTIONAL_HEADER.ImageBase
img=pe.get_memory_mapped_image()
class Emu:
    def __init__(self, rate):
        self.mu=mu=Uc(UC_ARCH_X86, UC_MODE_32)
        size=(len(img)+0xfff)&~0xfff
        mu.mem_map(base,size); mu.mem_write(base,bytes(img))
        self.STK=0x20000000; mu.mem_map(self.STK,0x10000)
        self.ST=0x30000000; mu.mem_map(self.ST,0x10000)      # chan state
        self.BUF=0x40000000; mu.mem_map(self.BUF,0x100000)
        self.RET=0x50000000; mu.mem_map(self.RET,0x1000); mu.mem_write(self.RET,b'\xf4')
        D=rate*3//1000
        st=bytearray(0x924)
        struct.pack_into('<I',st,0x904,0); struct.pack_into('<I',st,0x914,D)
        struct.pack_into('<f',st,0x918,np.float32(1.0/(np.float64(np.float32(0.005)) *rate)))
        struct.pack_into('<f',st,0x91c,np.float32(1.0/(np.float64(np.float32(0.05))*rate)))
        struct.pack_into('<f',st,0x920,1.0)
        mu.mem_write(self.ST,bytes(st))
        # x87 control word 0x27f (53-bit, round to nearest), as MSVC's runtime sets it
        mu.reg_write(UC_X86_REG_FPCW,0x27f)
    def process(self, x):
        mu=self.mu; n=len(x)
        mu.mem_write(self.BUF,np.asarray(x,dtype='<f4').tobytes())
        sp=self.STK+0x8000
        mu.mem_write(sp,struct.pack('<III',self.RET,self.BUF,n))
        mu.reg_write(UC_X86_REG_ESP,sp); mu.reg_write(UC_X86_REG_ECX,self.ST)
        mu.reg_write(UC_X86_REG_FPTAG,0xffff)
        mu.emu_start(0x1002cb30,self.RET)
        return np.frombuffer(mu.mem_read(self.BUF,4*n),dtype='<f4').copy()
    def gain(self): return struct.unpack('<f',self.mu.mem_read(self.ST+0x920,4))[0]

f32=np.float32
class Model:
    def __init__(self, rate):
        self.D=rate*3//1000; self.d=np.zeros(self.D,dtype=f32); self.idx=0; self.g=f32(1.0)
        self.A=f32(1.0/(np.float64(f32(0.005))*rate)); self.B=f32(1.0/(np.float64(f32(0.05))*rate))
    def process(self, x):
        x=np.asarray(x,dtype=f32); n=len(x); D=self.D; out=np.empty(n,dtype=f32)
        P=0.0; posP=0; Pout=0.0; posOut=0
        for i in range(D):
            a=abs(float(self.d[(self.idx+i)%D]))
            if a>P: P=a; posP=i
            if i==n-1: Pout=P; posOut=posP
        for k in range(n):
            a=abs(float(x[k])); pos=D+k
            if a>P: P=a; posP=pos
            if pos==n-1: Pout=P; posOut=posP
        g=float(self.g); s=None; target=0; mode='const'
        K98=float(f32(0.98))
        if g==1.0 and P<1.0: mode='delay'
        elif P>1.0:
            if Pout<float(f32(1e-8)): self.g=f32(K98/P)
            elif g*P>1.0:
                if posP and posOut and (P/posP < Pout/posOut) and Pout*g>K98: target=posOut; s=(K98/Pout-g)/posOut
                else:
                    # (the comparisons as the x87 makes them with a zero divisor: inf)
                    a=P/posP if posP else math.inf; b=Pout/posOut if posOut else math.inf
                    if a<b and Pout*g>K98: target=posOut; s=(K98/Pout-g)/posOut if posOut else -math.inf
                    else: target=posP; s=(K98/P-g)/posP if posP else -math.inf
                s=float(f32(s))
                if float(self.A)<abs(s): s=-float(self.A)
            else:
                if P*(g+posP*float(self.B))<K98: s=float(f32(float(self.B)*0.5))
        else:
            if g<1.0:
                s=float(f32((1.0-g)/n))
                if float(self.B)<s: s=float(self.B)
        if mode=='delay':
            for k in range(n):
                out[k]=self.d[self.idx]; self.d[self.idx]=x[k]; self.idx=(self.idx+1)%D
            return out
        if s is None or s==0.0:
            gg=self.g
            for k in range(n):
                out[k]=f32(float(self.d[self.idx])*float(gg)); self.d[self.idx]=x[k]; self.idx=(self.idx+1)%D
            return out
        if s>0:
            s=f32(s); K995=float(f32(0.995))
            for k in range(n):
                out[k]=f32(float(self.d[self.idx])*float(self.g)); self.d[self.idx]=x[k]
                g2=f32(float(s)+float(self.g)); self.g=g2
                if not (float(g2)<K995): self.g=f32(1.0); s=f32(0.0)
                self.idx=(self.idx+1)%D
            return out
        s=f32(s)
        for k in range(n):
            out[k]=f32(float(self.d[self.idx])*float(self.g)); self.d[self.idx]=x[k]
            if k>target: s=f32(0.0)
            self.g=f32(float(s)+float(self.g)); self.idx=(self.idx+1)%D
        if float(self.g)<0.0: self.g=f32(1.1754943508222875e-38)
        return out

if __name__=='__main__':
    rng=random.Random(int(sys.argv[1]) if len(sys.argv)>1 else 1)
    bad=0; tot=0
    for case in range(int(sys.argv[2]) if len(sys.argv)>2 else 30):
        rate=rng.choice([22050,44100,48000,96000]); e=Emu(rate); m=Model(rate)
        level=rng.choice([0.3,0.9,1.0,1.5,3.0,8.0]); t=0
        for blk in range(60):
            n=rng.choice([1,2,3,5,64,100,rate//100,rate//100,480,512,1024,rate*3//1000-1,rate*3//1000,rate*3//1000+1])
            lv=level*rng.choice([0,0.0,0.2,1,1,1,2])
            kind=rng.randrange(4)
            if kind==0: x=[lv*math.sin(2*math.pi*440*(t+i)/rate) for i in range(n)]
            elif kind==1: x=[lv*(rng.random()*2-1) for i in range(n)]
            elif kind==2: x=[lv*(1 if (t+i)%50<25 else -1) for i in range(n)]
            else: x=[(lv*4 if i==n//2 else 0.0) for i in range(n)]
            t+=n
            a=e.process(x); b=m.process(x); tot+=1
            ga=e.gain(); gb=float(m.g)
            if not (np.array_equal(a,b) and (ga==gb)):
                bad+=1
                if bad<=8:
                    dmax=float(np.max(np.abs(a-b))) if len(a) else 0
                    print('case',case,'rate',rate,'block',blk,'n',n,'kind',kind,'lv',lv,'max diff',dmax,'gain',ga,gb)
                # resync the model to the emulator so that one mismatch is one mismatch
                m.g=f32(ga); raw=e.mu.mem_read(e.ST+4,4*m.D); m.d=np.frombuffer(raw,dtype='<f4').copy(); m.idx=struct.unpack('<I',e.mu.mem_read(e.ST+0x904,4))[0]
    print('blocks',tot,'mismatched',bad)
