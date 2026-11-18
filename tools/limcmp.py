"""src/limiter.c itself against ALchemy's limiter in the emulator (limref.py compares a model in Python; this is the
C code, built here as a shared library and called through ctypes): random blocks at every rate, of every size
around the delay, with full scale exactly, denormals, 3e38, infinities and NaNs among the samples.

    ALCHEMY_DSOUND=/path/to/ALchemy/dsound.dll python3 tools/limcmp.py [seed] [cases]

Needs a C compiler besides what limref.py needs."""
import sys, os, struct, random, math, ctypes, subprocess, tempfile, shutil
import numpy as np
import limref
here=os.path.dirname(os.path.abspath(__file__))
tmp=tempfile.mkdtemp()
open(os.path.join(tmp,'w.c'),'w').write('''#include <stdlib.h>
#include "limiter.h"
rl_limiter *w_new(unsigned rate) { rl_limiter *l = malloc(sizeof *l); rl_limiter_init(l, rate, 1, 0); return l; }
void w_proc(rl_limiter *l, float *b, unsigned n) { rl_limiter_chan_process(l, &l->ch[0], b, n, 1); }
float w_gain(rl_limiter *l) { return l->ch[0].gain; }
unsigned w_at(rl_limiter *l) { return l->ch[0].at; }
''')
subprocess.check_call(['cc','-O1','-shared','-fPIC','-ffp-contract=off','-I',os.path.join(here,'..','src'),'-o',os.path.join(tmp,'libw.so'),
                       os.path.join(tmp,'w.c'),os.path.join(here,'..','src','limiter.c'),'-lm'])
L=ctypes.CDLL(os.path.join(tmp,'libw.so'))
L.w_new.restype=ctypes.c_void_p; L.w_new.argtypes=[ctypes.c_uint]
L.w_proc.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.c_uint]
L.w_gain.restype=ctypes.c_float; L.w_gain.argtypes=[ctypes.c_void_p]
L.w_at.restype=ctypes.c_uint; L.w_at.argtypes=[ctypes.c_void_p]
seed=int(sys.argv[1]) if len(sys.argv)>1 else 1; cases=int(sys.argv[2]) if len(sys.argv)>2 else 50
rng=random.Random(seed)
SP=[1.0,-1.0,float(np.float32(1.0000001)),float(np.float32(0.99999994)),0.98,0.995,1e-8,9e-9,1e-40,3e38,-3e38,1e20,2.0,2.5,2.6,100.0,
    0.0,-0.0,math.inf,-math.inf,math.nan]
bad=0; tot=0
for case in range(cases):
    rate=rng.choice([8000,11025,16000,22050,32000,44100,48000,88200,96000,176400,192000]); D=rate*3//1000
    e=limref.Emu(rate); p=L.w_new(rate)
    level=rng.choice([0.3,0.9,1.0,1.02,1.5,3.0,8.0,1e6])
    for blk in range(80):
        n=max(1,rng.choice([1,1,2,3,4,5,7,8,D-1,D,D+1,D+2,2*D,2*D+1,64,128,100,rate//100,480,512,1024,3000,rng.randrange(1,4*D+8)]))
        lv=level*rng.choice([0,0.0,0.2,1,1,1,2,0.5])
        kind=rng.randrange(8)
        if kind==0: x=[lv*math.sin(2*math.pi*440*i/rate) for i in range(n)]
        elif kind==1: x=[lv*(rng.random()*2-1) for i in range(n)]
        elif kind==2: x=[lv*(1 if i%50<25 else -1) for i in range(n)]
        elif kind==3: x=[(lv*4 if i==n//2 else 0.0) for i in range(n)]
        elif kind==4: x=[lv]*n
        elif kind==5: x=[0.0]*n; x[rng.randrange(n)]=lv*rng.choice([1,3,10]); x[0]=rng.choice([0,lv*2]); x[-1]=rng.choice([0.0,lv*5])
        elif kind==6: x=[rng.choice(SP) if rng.random()<0.1 else 0.0 for i in range(n)]
        else: x=[rng.choice(SP)*rng.choice([1,1,0.5,lv]) if rng.random()<0.5 else 0.3*rng.random() for i in range(n)]
        with np.errstate(all='ignore'):
            xa=np.asarray(x,dtype='<f4'); a=e.process(xa)
        b=xa.copy(); L.w_proc(p,b.ctypes.data,n); tot+=1
        ga=e.gain(); gb=L.w_gain(p)
        ia=struct.unpack('<I',e.mu.mem_read(e.ST+0x904,4))[0]
        if not (a.tobytes()==b.tobytes() and struct.pack('<f',ga)==struct.pack('<f',gb) and ia==L.w_at(p)):
            bad+=1
            d=[i for i in range(n) if a[i:i+1].tobytes()!=b[i:i+1].tobytes()]
            print('case',case,'rate',rate,'block',blk,'n',n,'kind',kind,'level',lv,'gain',ga,gb,'first differences',
                  [(i,float(a[i]),float(b[i])) for i in d[:4]],'of',len(d))
            break       # (the two are out of step from here on)
print('blocks',tot,'cases that differ',bad)
shutil.rmtree(tmp,ignore_errors=True)
sys.exit(1 if bad else 0)
