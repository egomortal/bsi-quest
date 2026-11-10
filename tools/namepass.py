import sys,json,collections,struct,re; sys.path.insert(0,'/home/claude/bsi'); from lib import *
from capstone import *
md=Cs(CS_ARCH_X86,CS_MODE_32); md.detail=False
G=load_graph()
RD=SECS['.rdata']; RLO=IB+RD.VirtualAddress; RHI=RLO+RD.Misc_VirtualSize
cls=json.load(open('classes.json'))
byreg={c['reg']:c for c in cls}
names={}   # addr -> (name, source, confidence)
labels={}  # data addr -> name
def setn(a,n,src):
  if a in names and names[a][0]!=n:
    names[a]=(names[a][0]+'|'+n,'conflict'); return
  names[a]=(n,src)
def insns(a,n=0x400,lim=400):
  o=va2off(a); k=0
  for i in md.disasm(data[o:o+n],a):
    yield i; k+=1
    if i.mnemonic=='ret' or k>lim: return
# --- class tree
for c in cls:
  c['parent']=byreg.get(c['ctor'],{}).get('name')
# group classes by registration function (dedupe multiple regs within same fn)
regcount=collections.Counter(c['reg'] for c in cls)
for c in cls:
  n=c['name']
  if regcount[c['reg']]==1: setn(c['reg'],f'{n}::StaticRegisterClass','classreg')
  labels[c['staticclass']]=f'{n}::PrivateStaticClass'
# InternalConstructor + C++ ctor
ctor_users=collections.defaultdict(list)
for c in cls:
  f=c['f9']; setn(f,f"{c['name']}::InternalConstructor",'classreg') if sum(1 for d in cls if d['f9']==f)==1 else None
  tgt=None
  for i in insns(f,0x40,10):
    if i.mnemonic=='jmp' and i.op_str.startswith('0x') and not (f<=int(i.op_str,16)<f+0x40): tgt=int(i.op_str,16)
  c['cppctor']=tgt
  if tgt: ctor_users[tgt].append(c['name'])
parent={c['name']:c['parent'] for c in cls}
def ancestors(n):
  r=[]; 
  while n in parent and parent[n] and parent[n]!=n and len(r)<60: n=parent[n]; r.append(n)
  return r
for tgt,us in ctor_users.items():
  if len(us)==1: setn(tgt,f'{us[0]}::{us[0][1:]}','ctor')
  else:
    base=[u for u in us if all(u==v or u in ancestors(v) for v in us)]
    if base: setn(tgt,f'{base[0]}::{base[0][1:]}','ctor-shared')
sc=collections.Counter(c['staticctor'] for c in cls)
for c in cls:
  if sc[c['staticctor']]==1: setn(c['staticctor'],f"{c['name']}::StaticConstructor",'classreg')
# --- vtables from ctors
STARTS=set(int(x,16) for x in open('/home/claude/bsi/vt_starts.txt').read().split())
def vt_len(v):
  n=0
  while True:
    p=u32(v+4*n)
    if p is None or not (TLO<=p<THI): break
    if n>0 and v+4*n in STARTS: break
    n+=1
    if n>2000: break
  return n
vt={}
def last_vt(fn,n,lim):
  last=None; thisr=None; k=0
  for i in insns(fn,n,lim):
    k+=1
    if thisr is None and i.mnemonic=='mov':
      m=re.fullmatch(r'(e[a-z]{2}), (ecx|dword ptr \[esp \+ (4|8)\])',i.op_str)
      if m and m.group(1) not in ('esp','ebp'): thisr=m.group(1)
    if i.mnemonic=='mov':
      m=re.fullmatch(r'dword ptr \[(e[a-z]{2})\], (0x[0-9a-f]+)',i.op_str)
      if m and m.group(1) in (thisr,'ecx' if thisr is None else thisr):
        v=int(m.group(2),16)
        if RLO<=v<RHI and TLO<=(u32(v) or 0)<THI: last=v
  return last
for c in cls:
  v=last_vt(c['f9'],0x80,30)
  if v is None and c['cppctor'] and len(ctor_users[c['cppctor']])==1: v=last_vt(c['cppctor'],0x3000,1500)
  if v: vt[c['name']]=v
vcount=collections.Counter(vt.values())
by=collections.defaultdict(list)
for n,v in vt.items(): by[v].append(n)
vt={}
for v,us in by.items():
  base=[u for u in us if all(u==w or u in ancestors(w) for w in us)]
  if base: vt[base[0]]=v
for n,v in vt.items(): labels[v]=f'{n}::vftable'
vtl={n:vt_len(v) for n,v in vt.items()}
newslots=0
for n,v in vt.items():
  pv=None
  for a in ancestors(n):
    if a in vt: pv=vt[a]; break
  plen=vtl.get([a for a in ancestors(n) if a in vt][0],0) if pv else 0
  for k in range(vtl[n]):
    f=u32(v+4*k)
    if pv and k<plen and u32(pv+4*k)==f: continue
    # function introduced/overridden here
    if f not in names: setn(f,f'{n}::vf{k:03d}','vtable'); newslots+=1

# parent C++ ctor = call right before the inline vtable store in InternalConstructor
pc=collections.defaultdict(set)
for c in cls:
  prev=None
  for i in insns(c['f9'],0x80,30):
    if i.mnemonic=='call' and i.op_str.startswith('0x'): prev=int(i.op_str,16)
    if i.mnemonic=='mov' and re.fullmatch(r'dword ptr \[e[a-z]{2}\], 0x[0-9a-f]+',i.op_str) and prev:
      if c['parent']: pc[prev].add(c['parent'])
      break
pcn=0
for a,ps in pc.items():
  if len(ps)==1 and a not in names:
    p=list(ps)[0]; setn(a,f'{p}::{p[1:]}','parent-ctor'); pcn+=1
print('parent ctors',pcn)
# --- exec natives + callee propagation
ex={}
for l in open('names_exec.tsv'):
  a,n=l.split(); ex[int(a,16)]=n; names[int(a,16)]=(n,'native')
calledby=collections.Counter()
for a in ex:
  for c in set(G.get(a,{}).get('calls',[])): calledby[c]+=1
prop=0
for a,n in ex.items():
  cs=[c for c in G.get(a,{}).get('calls',[]) if calledby[c]==1 and c not in names]
  if len(cs)==1:
    cl,m=n.split('::'); setn(cs[0],f'{cl}::{m[4:]}','native-callee'); prop+=1
json.dump({'parent':parent,'size':{c['name']:c['size'] for c in cls},'pkg':{c['name']:c['pkg'] for c in cls},'vftable':{n:hex(v) for n,v in vt.items()}},open('class_tree.json','w'),indent=0)
with open('names_all.tsv','w') as f:
  for a,(n,s) in sorted(names.items()):
    if s!='conflict': f.write(f'{a:08x}\t{n}\t{s}\n')
with open('labels_all.tsv','w') as f:
  for a,n in sorted(labels.items()): f.write(f'{a:08x}\t{n}\n')
real=[a for a,f in G.items() if not f['name'].startswith('Unwind@') and not f['name'].startswith('Catch')]
cov=sum(1 for a in real if a in names and names[a][1]!='conflict')
print('classes',len(cls),'with parent',sum(1 for c in cls if c['parent']),'vtables',len(vt),'new vslots',newslots,'native-callees',prop)
print(collections.Counter(s for n,s in names.values()))
print(f'coverage {cov}/{len(real)} = {100*cov/len(real):.1f}% of real functions')
