import sys,json,re,collections; sys.path.insert(0,'/home/claude/bsi'); from lib import *
from capstone import *
md=Cs(CS_ARCH_X86,CS_MODE_32)
t=json.load(open('class_tree.json')); P=t['parent']; V={n:int(v,16) for n,v in t['vftable'].items()}
rows=[l.rstrip('\n').split('\t') for l in open('names_all.tsv')]
names={int(a,16):(n,s) for a,n,s in rows}
def anc(n):
  r=[]
  while P.get(n) and P[n]!=n and len(r)<60: n=P[n]; r.append(n)
  return r
STARTS=set(int(x,16) for x in open('/home/claude/bsi/vt_starts.txt').read().split())
def vlen(v):
  k=0
  while True:
    p=u32(v+4*k)
    if p is None or not TLO<=p<THI: return k
    if k>0 and v+4*k in STARTS: return k
    k+=1
VL={n:vlen(v) for n,v in V.items()}
for n in list(P.keys()):
  if not P.get(n) and n!='UObject': P[n]='UObject'
VL['UObject']=35
def vt_of(c):
  for x in [c]+anc(c):
    if x in V: return x
def intro(c,k):
  top=None
  for x in [c]+anc(c):
    if x in VL and VL[x]>k: top=x
  return top
CLOB={'eax','ecx','edx'}
def this_vcalls(fa):
  o=va2off(fa); thisr={'ecx'}; vtr=set(); fp={}; out=[]; n=0
  for i in md.disasm(data[o:o+0x1500],fa):
    n+=1
    if n>80: return None   # only small natives
    mn,op=i.mnemonic,i.op_str
    dst=op.split(',')[0].strip() if ',' in op else (op if mn in('pop','inc','dec','not','neg') else None)
    if mn=='mov':
      m=re.fullmatch(r'(e[a-z]{2}), (e[a-z]{2})',op)
      if m:
        d,s=m.groups()
        for S in (thisr,vtr): S.discard(d)
        fp.pop(d,None)
        if s in thisr: thisr.add(d)
        continue
      m=re.fullmatch(r'(e[a-z]{2}), dword ptr \[(e[a-z]{2})\]',op)
      if m and m.group(2) in thisr:
        thisr.discard(m.group(1)); vtr.add(m.group(1)); continue
      m=re.fullmatch(r'(e[a-z]{2}), dword ptr \[(e[a-z]{2})(?: \+ (0x[0-9a-f]+|\d+))?\]',op)
      if m and m.group(2) in vtr:
        vtr.discard(m.group(1)); thisr.discard(m.group(1)); fp[m.group(1)]=int(m.group(3),0)//4 if m.group(3) else 0; continue
    if mn=='call':
      if op in fp: out.append(fp.pop(op))
      else:
        m=re.fullmatch(r'dword ptr \[(e[a-z]{2})(?: \+ (0x[0-9a-f]+|\d+))?\]',op)
        if m and m.group(1) in vtr: out.append(int(m.group(2),0)//4 if m.group(2) else 0)
      for S in (thisr,vtr): S.difference_update(CLOB)
      for r in list(fp):
        if r in CLOB: fp.pop(r)
      continue
    if dst and re.fullmatch(r'e[a-z]{2}',dst):
      for S in (thisr,vtr): S.discard(dst)
      fp.pop(dst,None)
    if mn=='ret': break
  return out
votes=collections.defaultdict(collections.Counter)
for a,(n,s) in names.items():
  if s!='native': continue
  c,m=n.split('::exec'); vc=this_vcalls(a)
  if not vc or len(vc)!=1: continue
  k=vc[0]; root=intro(c,k)
  if root is None or k==0: continue
  votes[(root,k)][m]+=1
slotname={}
for key,c in votes.items():
  (m,n),*rest=c.most_common()
  if not rest or n>=2*rest[0][1]: slotname[key]=m
print('slot names',len(slotname))
for (r,k),m in sorted(slotname.items(), key=lambda x:(x[0][0],x[0][1]))[:80]: print(f'{r}[{k}]={m}',end='  ')
for (r,k),m in sorted(slotname.items()):
  if r=='UObject': print('UObject',k,m, dict(votes[(r,k)]))
json.dump({f'{r}|{k}':m for (r,k),m in slotname.items()},open('slotnames.json','w'),indent=0)
# rename
ren=0; newn={}
for a,(n,s) in names.items():
  m=re.fullmatch(r'(\w+)::vf(\d{3})',n)
  if m:
    c,k=m.group(1),int(m.group(2))
    root=intro(c,k)
    if root and (root,k) in slotname: newn[a]=(f'{c}::{slotname[(root,k)]}','vslot'); ren+=1
# slot0: dtors
dt=0
for c,v in V.items():
  f=u32(v)
  if f in names and names[f][0].endswith('::vf000') or f not in names:
    newn[f]=(f'{c}::scalar_deleting_dtor','vslot0')
  for i in md.disasm(data[va2off(f):va2off(f)+0x30],f):
    if i.mnemonic=='call' and i.op_str.startswith('0x'):
      d=int(i.op_str,16)
      if d not in names or names[d][0].startswith(c+'::vf'): newn[d]=(f'{c}::~{c[1:]}','dtor'); dt+=1
      break
names.update(newn)
with open('names_all.tsv','w') as f:
  for a,(n,s) in sorted(names.items()): f.write(f'{a:08x}\t{n}\t{s}\n')
print('renamed vslots',ren,'dtors',dt)
print(collections.Counter(s for n,s in names.values()))
