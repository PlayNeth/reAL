import sys
def load(p):
    ev=[]
    for l in open(p):
        if l.startswith('#'): continue
        a=l.split(); ev.append((int(a[0]),' '.join(a[1:])))
    return ev
a=load(sys.argv[1]); b=load(sys.argv[2])
lim=max(x[0] for x in b)
a=[x for x in a if x[0]<=lim]
sa=sorted(a,key=lambda x:x[0]); sb=sorted(b,key=lambda x:x[0])
print(len(sa),len(sb))
from collections import Counter
ca=Counter(sa); cb=Counter(sb)
print('only ref:',sum((cb-ca).values()),'only mine:',sum((ca-cb).values()))
d=sorted((cb-ca).elements())[:15]; e=sorted((ca-cb).elements())[:15]
for x in d: print(' ref ',x)
for x in e: print(' mine',x)
# order check
print('same sorted order:', sa==sb)
