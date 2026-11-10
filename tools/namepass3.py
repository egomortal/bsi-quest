import sys,json,re,collections; sys.path.insert(0,'/home/claude/bsi'); from lib import *
G=load_graph()
t=json.load(open('class_tree.json')); P=t['parent']
gl={int(k,16):v for k,v in json.load(open('fname_globals.json')).items()}
rows=[l.rstrip('\n').split('\t') for l in open('names_all.tsv')]
names={int(a,16):(n,s) for a,n,s in rows}
labels={int(a,16):n for a,n in (l.rstrip('\n').split('\t') for l in open('labels_all.tsv'))}
FFC=0x4d1090
known={0x4d1090:'UObject::FindFunctionChecked',0x4c5e70:'FName::FName',0x8b9ff0:'XRegisterClass',
 0x48fc20:'UObject::operator_delete',0x5d76d0:'nullsub',0x91e900:'AutoInitializeRegistrants_XGame',
 0x5d8c70:'InitNames_Engine',0x9173d0:'InitNames_XGame'}
for a,n in known.items():
  if a not in names or names[a][1] in ('vtable',): names[a]=(n,'manual')
labels.update({0x136dcb0:'GNatives',0x139dde4:'GNativeLookupFuncs'})
for g,s in gl.items(): labels[g]=f'NAME_{s}'
# callers map
callers=collections.defaultdict(set)
for a,f in G.items():
  for c in f['calls']: callers[c].add(a)
def anc(n):
  r=[n]
  while P.get(n) and P[n]!=n and len(r)<60: n=P[n]; r.append(n)
  return r
def lca(cs):
  cs=[c for c in cs if c]
  if not cs: return None
  common=anc(cs[0])
  for c in cs[1:]:
    a=set(anc(c)); common=[x for x in common if x in a]
  return common[0] if common else None
ev=0; evc=0
for a,f in G.items():
  if FFC not in f['calls']: continue
  gs=set(gl[r] for r in f['refs'] if r in gl)
  if len(gs)!=1 or len(f['calls'])>3: continue
  if a in names and names[a][1] not in ('vtable',): continue
  e=gs.pop()
  cl=[names[c][0].split('::')[0] for c in callers[a] if c in names and '::' in names[c][0]]
  owner=lca(cl) if cl and len(cl)>=1 else None
  names[a]=(f'{owner}::event{e}' if owner else f'event{e}','event'); ev+=1; evc+= bool(owner)
print('event thunks',ev,'with owner class',evc)
with open('names_all.tsv','w') as f:
  for a,(n,s) in sorted(names.items()): f.write(f'{a:08x}\t{n}\t{s}\n')
with open('labels_all.tsv','w') as f:
  for a,n in sorted(labels.items()): f.write(f'{a:08x}\t{n}\n')
print(collections.Counter(s for n,s in names.values()))
