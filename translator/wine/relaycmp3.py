exec(open('/home/claude/bsi/recomp/wine/relaycmp.py').read().split("n=min")[0])
b=[x for x in b if 'bsi_start' not in x]
def keep(x): return not (x.startswith('KERNEL32.RaiseException(406d1388') or x.startswith('msvcr90._except_handler4_common'))
a=[x for x in a if keep(x)]; b=[x for x in b if keep(x)]
POLL=('KERNEL32.QueryPerformanceCounter','KERNEL32.Sleep','KERNEL32.SwitchToThread','KERNEL32.GetTickCount','KERNEL32.QueryPerformanceFrequency')
def collapse(L):
    out=[]; o=[]
    for x in L:
        n=x.split('(')[0]
        if n in POLL:
            if out and out[-1]=='<poll>': continue
            out.append('<poll>'); o.append(x)
        else: out.append(n); o.append(x)
    return out,o
A,a=collapse(a); B,b=collapse(b)
i=j=0; mism=[]; matched=0
while i<len(A) and j<len(B):
    if A[i]==B[j]: i+=1; j+=1; matched+=1; continue
    found=None
    for d in range(1,60):
        for di,dj in ((d,0),(0,d),(d,d)):
            if i+di<len(A) and j+dj<len(B) and A[i+di:i+di+8]==B[j+dj:j+dj+8]: found=(di,dj); break
        if found: break
    mism.append((i,j,found))
    if not found: break
    i+=found[0]; j+=found[1]
print('matched',matched,'native',len(A),'recomp',len(B),'mismatch regions',len(mism))
for i,j,f in mism[:15]:
    print('--- at N',i,'R',j,'resync',f)
    for k in range(i,i+(f[0] if f else 3)+1): print('   N:',a[k][:170])
    for k in range(j,j+(f[1] if f else 3)+1): print('   R:',b[k][:170])
