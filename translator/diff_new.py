import sys, collections
sys.path.insert(0,'/home/claude/bsi/recomp')
import x86c
from difftest import *
x86c.FUNC_STARTS=set(F)
funcs=[int(l,16) for l in open(__import__('os').environ.get('NFILE','/home/claude/bsi/newfuncs.txt')) if l.strip()]
funcs=[a for a in funcs if a in F]
import random; random.Random(1).shuffle(funcs); funcs=funcs[:int(__import__('os').environ.get('NDIFF','100000'))]
cl=set(funcs)|closure(funcs, limit=1500)
so=build(cl,'newf'); H=Harness(so); res=collections.Counter()
for a in funcs:
    v='skip'; why=''
    for k in range(5):
        st=H.state(a*31+k); u,ue=H.run_uc(a,st)
        if u is None: why=ue; continue
        c,ce=H.run_c(a,st)
        if c is None: v='cfault'; why=ce; break
        d=diff(u,c)
        if d: v='FAIL'; why='; '.join(d[:3]); break
        v='pass'
    res[v]+=1
    if v in ('FAIL','cfault'): print(hex(a), v, why[:300])
print(dict(res))
