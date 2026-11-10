import re,sys
exec(open('/home/claude/bsi/recomp/wine/relaycmp.py').read().split("n=min")[0].replace("R=load('relay_recomp.txt')","R=[]").replace("bb=BASE['relay_recomp.txt']; gb=collections.Counter(r for f,x,r in R if bb<=r<bb+0x4300000).most_common(1)[0][0]","gb=0"))
ALIAS={'rtlinitializecriticalsection':'initializecriticalsection','rtlsetcriticalsectionspincount':'setcriticalsectionspincount','rtlentercriticalsection':'entercriticalsection','rtlleavecriticalsection':'leavecriticalsection','rtltryentercriticalsection':'tryentercriticalsection','rtldeletecriticalsection':'deletecriticalsection','rtlinitializeslisthead':'initializeslisthead','rtlinterlockedpushentryslist':'interlockedpushentryslist','rtlinterlockedpopentryslist':'interlockedpopentryslist'}
def nm(x):
    n=re.split(r'[.!]',x.split('(')[0])[-1].lower(); return ALIAS.get(n,n)
POLL={'getsystemtimeasfiletime','getcurrentprocessid','getcurrentthreadid','getlasterror','setlasterror','tlsgetvalue','tlssetvalue','queryperformancecounter','sleep','gettickcount','timegettime','switchtothread','entercriticalsection','leavecriticalsection','tryentercriticalsection'}
def skip(n): return n in POLL or n.startswith('__libm') or n.startswith('_ci')
A=[nm(x) for x in a if not skip(nm(x))]
import collections
_raw=[l.strip() for l in open('/home/claude/bsigame/Binaries/Win32/bsi_linux.log') if l.startswith('[')]
_main=collections.Counter(l.split(']')[0] for l in _raw[:50]).most_common(1)[0][0]
L=[l.split('] ',1)[1] for l in _raw if l.split(']')[0]==_main]
B=[nm(x) for x in L if not skip(nm(x))]
Lf=[x for x in L if not skip(nm(x))]; Af=[x for x in a if not skip(nm(x))]
n=min(len(A),len(B)); k=next((i for i in range(n) if A[i]!=B[i]),n)
print('wine calls',len(A),'linux calls',len(B),'identical prefix (names, polling removed):',k)
for i in range(max(0,k-4),min(n,k+4)): print(i,'W:',Af[i][:150]); print(i,'L:',Lf[i][:150])
# resync alignment
i=j=0; mism=[]; matched=0
while i<len(A) and j<len(B):
    if A[i]==B[j]: i+=1; j+=1; matched+=1; continue
    found=None
    for d in range(1,80):
        for di,dj in ((d,0),(0,d),(d,d)):
            if i+di<len(A) and j+dj<len(B) and A[i+di:i+di+6]==B[j+dj:j+dj+6]: found=(di,dj); break
        if found: break
    mism.append((i,j,found))
    if not found: break
    i+=found[0]; j+=found[1]
print('aligned: matched',matched,'mismatch regions',len(mism),'stopped at wine',i,'linux',j)
for i,j,f in mism[-4:]:
    print('--- W',i,'L',j,f)
    for k in range(i,i+(f[0] if f else 4)+1): print('   W:',Af[k][:160])
    for k in range(j,j+(f[1] if f else 4)+1): print('   L:',Lf[k][:160])
