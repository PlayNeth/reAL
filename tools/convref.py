"""src/outconv.c against ALchemy's own conversions of the mix to 16 and 32 bits (CStaticHspSystem's kernels 0x100789f0
and 0x10078dc0, run in an emulator): random samples, every value near a half, near full scale and beyond.

    ALCHEMY_DSOUND=/path/to/ALchemy/dsound.dll python3 tools/convref.py [seed] [samples]

Needs unicorn, pefile, numpy and a C compiler.  Beyond 65536 times full scale ALchemy's 16 bits wrap around and
reAL's hold; those are left out."""
import sys, os, struct, ctypes, subprocess, tempfile, shutil
import numpy as np
import pefile
from unicorn import *
from unicorn.x86_const import *
DLL=os.environ.get('ALCHEMY_DSOUND','dsound.dll')
pe=pefile.PE(DLL); base=pe.OPTIONAL_HEADER.ImageBase; img=pe.get_memory_mapped_image()
mu=Uc(UC_ARCH_X86, UC_MODE_32)
mu.mem_map(base,(len(img)+0xfff)&~0xfff); mu.mem_write(base,bytes(img))
STK=0x20000000; mu.mem_map(STK,0x10000); SRC=0x40000000; mu.mem_map(SRC,0x4000000); DST=0x48000000; mu.mem_map(DST,0x4000000)
RET=0x50000000; mu.mem_map(RET,0x1000); mu.mem_write(RET,b'\xf4')
def dll(fn, x, width):
    n=len(x); mu.mem_write(SRC,x.tobytes()); sp=STK+0x8000
    mu.mem_write(sp,struct.pack('<IIII',RET,DST,SRC,n))
    mu.reg_write(UC_X86_REG_ESP,sp); mu.reg_write(UC_X86_REG_FPCW,0x27f); mu.reg_write(UC_X86_REG_FPTAG,0xffff)
    mu.emu_start(fn,RET)
    return np.frombuffer(mu.mem_read(DST,width*n),dtype='<i%d'%width).copy()
here=os.path.dirname(os.path.abspath(__file__)); tmp=tempfile.mkdtemp()
so=os.path.join(tmp,'libc.so')
subprocess.check_call(['cc','-O1','-shared','-fPIC','-ffp-contract=off','-o',so,os.path.join(here,'..','src','outconv.c'),'-lm'])
L=ctypes.CDLL(so)
L.rl_out_s16.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint,ctypes.c_void_p]
L.rl_out_s32.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint,ctypes.c_int]
seed=int(sys.argv[1]) if len(sys.argv)>1 else 1; count=int(sys.argv[2]) if len(sys.argv)>2 else 400000
rng=np.random.default_rng(seed)
def samples(scale):
    x=[rng.uniform(-1.2,1.2,count), rng.uniform(-1,1,count)*1e-3, rng.uniform(-1,1,count)*1e-5,
       (rng.integers(-40000,40000,count)+0.5)/scale, (rng.integers(-40000,40000,count)+0.5)/scale*(1+rng.integers(-3,4,count)*6e-8),
       rng.integers(-40000,40000,count)/scale, 1+rng.integers(-64,65,count)*6e-8, -1+rng.integers(-64,65,count)*6e-8,
       rng.uniform(-2,2,count), np.array([0.0,-0.0,1.0,-1.0,0.5/scale,-0.5/scale,1.5/scale,-1.5/scale,1e-40,-1e-40,3.0,-3.0,100.0,-100.0])]
    return np.concatenate(x).astype('<f4')
bad=0
x=samples(32768.0); a=dll(0x100789f0,x,2); b=np.empty(len(x),dtype='<i2'); L.rl_out_s16(b.ctypes.data,x.ctypes.data,len(x),None)
d=np.nonzero(a!=b)[0]; bad+=len(d); print('16 bits:',len(x),'samples,',len(d),'differ',[(float(x[i]),int(a[i]),int(b[i])) for i in d[:5]])
x=np.concatenate([samples(2147483648.0),samples(32768.0)]); a=dll(0x10078dc0,x,4); b=np.empty(len(x),dtype='<i4'); L.rl_out_s32(b.ctypes.data,x.ctypes.data,len(x),0)
d=np.nonzero(a!=b)[0]; bad+=len(d); print('32 bits:',len(x),'samples,',len(d),'differ',[(float(x[i]),int(a[i]),int(b[i])) for i in d[:5]])
shutil.rmtree(tmp,ignore_errors=True)
sys.exit(1 if bad else 0)
