import re
# wine main-thread (name, retval) for game-originated calls
rx=re.compile(r'^([0-9a-f]{4}):(Call|Ret) +([\w.]+)\((.*?)\)(?: retval=([0-9a-f]+))? ret=([0-9a-f]+)')
W=[]; tid=None; pend={}
for l in open('/tmp/claude-0/relay_native.txt',errors='replace'):
    m=rx.match(l)
    if not m: continue
    t,kind,fn,args,rv,ret=m.groups()
    if tid is None:
        if '"bsirecomp.dll"' in args: tid=t
        continue
    if t!=tid: continue
    r=int(ret,16)
    if not (0x400000<=r<0x1700000): continue
    if kind=='Call': W.append([fn,args,None])
    else:
        for e in reversed(W[-50:]):
            if e[0]==fn and e[2] is None: e[2]=rv; break
    if len(W)>12000: break
L=[]; cur=None
for l in open('/home/claude/bsigame/Binaries/Win32/bsi_linux.log'):
    if l.startswith('  = '):
        if cur: cur[2]=l.split('=')[1].strip()
        continue
    if '(' in l and '!' in l.split('(')[0]:
        cur=[l.split('(')[0], l.strip(), None]; L.append(cur)
def nm(x): return re.split(r'[.!]',x)[-1].lower().replace('rtl','',1) if x.split('.')[-1].startswith('Rtl') else re.split(r'[.!]',x)[-1].lower()
SKIP={'getlasterror','setlasterror','tlsgetvalue','tlssetvalue','entercriticalsection','leavecriticalsection','tryentercriticalsection','getsystemtimeasfiletime','getcurrentprocessid','getcurrentthreadid','queryperformancecounter','gettickcount','timegettime','sleep','raiseexception','_except_handler4_common'}
W=[e for e in W if 'bsi_start' not in e[1] and nm(e[0]) not in SKIP and not nm(e[0]).startswith('__libm')]
L=[e for e in L if nm(e[0]) not in SKIP and not nm(e[0]).startswith('__libm')]
i=j=0; shown=0
while i<len(W) and j<len(L) and shown<15:
    a,b=W[i],L[j]
    if nm(a[0])!=nm(b[0]):
        # try small resync
        ok=False
        for d in range(1,6):
            if i+d<len(W) and nm(W[i+d][0])==nm(b[0]): i+=d; ok=True; break
            if j+d<len(L) and nm(L[j+d][0])==nm(a[0]): j+=d; ok=True; break
        if ok: continue
        print('NAME DIVERGE at',i,j,a[0],a[1][:80],'|',b[1][:120]); break
    ra=int(a[2],16) if a[2] else None; rb=int(b[2],16) if b[2] else None
    interesting = ra is not None and rb is not None and ra!=rb and not (ra>0x10000 and rb>0x10000)
    if interesting: print(i,'retval differs:',a[0],a[1][:90],'wine=',hex(ra),'linux=',hex(rb)); shown+=1
    i+=1; j+=1
print('compared',i)
