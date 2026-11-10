import sys,re,collections; sys.path.insert(0,'/home/claude/bsi'); from lib import *
G=load_graph()
names={int(l.split('\t')[0],16):l.split('\t')[1] for l in open('names_all.tsv')}
B=0x10000
S=collections.defaultdict(list); N=collections.Counter(); U=collections.Counter()
for a in sorted(G):
  f=G[a]
  if f['name'].startswith(('Unwind@','Catch')): continue
  N[a//B]+=1
  if a in names: U[a//B]+=1
  for r in f['refs']:
    s=astr(r) or wstr(r)
    if s and len(s)>=5 and re.search('[a-zA-Z]{3}',s): S[a//B].append(s)
lo,hi=int(sys.argv[1],16)//B,int(sys.argv[2],16)//B
for k in range(lo,hi):
  ss=list(dict.fromkeys(S[k]))
  print(f'{k*B:#x} n={N[k]} ue={U[k]} :: '+' | '.join(x[:40] for x in ss[:10]))
