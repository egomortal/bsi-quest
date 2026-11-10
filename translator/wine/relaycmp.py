import re,collections
rx=re.compile(r'^([0-9a-f]{4}):Call ([\w.]+)\((.*)\) ret=([0-9a-f]+)')
BASE={}
def load(path, limit=3_000_000):
    rows=[]; tid=None
    with open(path,errors='replace') as f:
        for l in f:
            m=rx.match(l)
            if not m: continue
            t,fn,args,ret=m.groups()
            if tid is None:
                if '"bsirecomp.dll"' in args: tid=t
                continue
            if fn=='KERNEL32.GetProcAddress' and '"bsi_start"' in args: BASE[path]=int(args.split(',')[0],16)
            if t==tid: rows.append((fn,args,int(ret,16)))
            if len(rows)>=limit: break
    return rows
N=load('relay_native.txt'); R=load('relay_recomp.txt')
a=[f+"("+x+")" for f,x,r in N if 0x400000<=r<0x1700000 and "bsi_start" not in x]
bb=BASE['relay_recomp.txt']; gb=collections.Counter(r for f,x,r in R if bb<=r<bb+0x4300000).most_common(1)[0][0]
b=[f+"("+x+")" for f,x,r in R if r==gb or 0x400000<=r<0x1700000]
print('native game calls',len(a),'recomp game calls',len(b),'gate ret',hex(gb))
n=min(len(a),len(b)); k=next((i for i in range(n) if a[i]!=b[i]),n)
print('identical prefix:',k)
for i in range(max(0,k-3),min(n,k+5)): print(i,'N:',a[i][:200]); print(i,'R:',b[i][:200])
