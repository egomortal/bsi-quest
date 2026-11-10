import sys,re,collections,json; sys.path.insert(0,'/home/claude/bsi'); from lib import *
from capstone import *
md=Cs(CS_ARCH_X86,CS_MODE_32)
G=load_graph(); FN=0x4c5e70
glob={}
for a,f in G.items():
  if FN not in f['calls']: continue
  last=None; pend=None
  for i in md.disasm(data[va2off(a):va2off(a)+0x40000],a):
    if i.mnemonic=='push' and i.op_str.startswith('0x'):
      v=int(i.op_str,16); s=astr(v) if v>0x1000000 else None
      if s: last=s
    elif i.mnemonic=='call' and i.op_str==hex(FN): pend=last; last=None
    elif i.mnemonic=='mov' and pend:
      m=re.fullmatch(r'dword ptr \[(0x[0-9a-f]+)\], e[a-z]{2}',i.op_str)
      if m: glob[int(m.group(1),16)]=pend; pend=None
    if i.mnemonic=='ret': break
print('FName globals',len(glob))
json.dump({hex(k):v for k,v in glob.items()},open('fname_globals.json','w'))
# event thunk candidates: small funcs referencing exactly one fname global
cand={}; callee=collections.Counter()
for a,f in G.items():
  gs=[glob[r] for r in f['refs'] if r in glob]
  if len(set(gs))==1 and len(f['calls'])<=4:
    cand[a]=gs[0]
    for c in f['calls']: callee[c]+=1
print('candidates',len(cand)); print([(hex(c),n) for c,n in callee.most_common(5)])
