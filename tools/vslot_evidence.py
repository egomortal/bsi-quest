import sys,re,collections; sys.path.insert(0,'/home/claude/bsi'); from lib import *
from capstone import *
md=Cs(CS_ARCH_X86,CS_MODE_32)
names={int(l.split('\t')[0],16):l.split('\t')[1] for l in open('/home/claude/bsi/names_all.tsv')}
def this_vcalls(fa,maxn=0x1500):
  o=va2off(fa); thisr={'ecx'}; vtr=set(); fp={}; out=[]; k=0
  for i in md.disasm(data[o:o+maxn],fa):
    k+=1
    if k>600: break
    op=i.op_str; mn=i.mnemonic
    if mn=='mov':
      m=re.fullmatch(r'(e[a-z]{2}), (e[a-z]{2})',op)
      if m:
        d,s=m.groups()
        if s in thisr and k<30: thisr.add(d)
        elif d in thisr and k>=2: thisr.discard(d)
        continue
      m=re.fullmatch(r'(e[a-z]{2}), dword ptr \[(e[a-z]{2})\]',op)
      if m and m.group(2) in thisr: vtr.add(m.group(1)); continue
      m=re.fullmatch(r'(e[a-z]{2}), dword ptr \[(e[a-z]{2})(?: \+ (0x[0-9a-f]+|\d+))?\]',op)
      if m and m.group(2) in vtr: fp[m.group(1)]=int(m.group(3),0)//4 if m.group(3) else 0; continue
    if mn=='call':
      if op in fp: out.append(fp.pop(op)); continue
      m=re.fullmatch(r'dword ptr \[(e[a-z]{2})(?: \+ (0x[0-9a-f]+|\d+))?\]',op)
      if m and m.group(1) in vtr: out.append(int(m.group(2),0)//4 if m.group(2) else 0)
    if mn=='ret': break
  return out
if __name__=='__main__':
  ev=collections.defaultdict(collections.Counter)
  for a,n in names.items():
    if '::exec' not in n: continue
    meth=n.split('::exec')[1]
    vc=this_vcalls(a)
    if len(set(vc))==1: ev[vc[0]][(n.split('::')[0],meth)]+=1
  for k in sorted(ev):
    print(k, ev[k].most_common(6))
