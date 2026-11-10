import sys,re,collections,json; sys.path.insert(0,'/home/claude/bsi'); from lib import *
G=load_graph()
names={int(l.split('\t')[0],16):l.split('\t')[1] for l in open('names_all.tsv')}
ANCH=[('Morpheme',r'^(node|Task|queue)[A-Z]\w+|NMPlatform|morpheme|\\NMP|MR::'),
('Kynapse',r'kyruntime|kypathdata|Lpf|PathFinder|FrameWatch|BlobAgg|PathObject|Kynapse'),
('libcurl',r'CURLOPT|Connection #|curl|Set-Cookie|httponly|tailmatch|splay node|pipeline'),
('2K_WebAPI',r'/user/accounts|X-2k|SsoAccount|isAccountLinked|application/json'),
('mbedTLS',r'mbedtls|ssl_tls|client hello|DES-EDE3|ENCRYPTED|MGF1|x509|ssl_srv|ssl_cli'),
('AMD_Tootle',r'Tootle|soup\.cpp|overdraw\.cpp|clustering\.cpp|tootlelib'),
('ATI_ADL',r'ADL_|atiadl'),
('Scaleform',r'GFx|gfx|ActionScript|attachMovie|SWF|MovieClip|movie clip|DefineFont|DefineBits|_level0|TEXTFORMAT|scale9|tabChildren'),
('Wwise',r'^AK::|Wwise|WG-\d+|779AD1D9'),
('libpng_zlib',r'png|PNG|IHDR|IDAT|zlib|inflate|deflate'),
('PhysX',r'PhysX|PxFoundation|PxWorker|Ps[A-Z]\w+\.cpp|Ext[A-Z]\w+\.cpp|stripped to save memory'),
('FaceFX',r'^Fx[A-Z]|FaceFX|FaceGraph|Phoneme')]
PRIOR=[(0x400000,0x470000,'Morpheme'),(0xd00000,0xda0000,'Kynapse'),(0xda0000,0xdd0000,'libcurl'),(0xdd0000,0xde0000,'2K_WebAPI'),
(0xde0000,0xe10000,'mbedTLS'),(0xe10000,0xe40000,'AMD_Tootle'),(0xe40000,0xe50000,'ATI_ADL'),(0xe50000,0xf30000,'Scaleform'),
(0xf30000,0xfc0000,'Wwise'),(0xfc0000,0xfe0000,'libpng_zlib'),(0xfe0000,0x1000000,'PhysX'),(0x1000000,0x1040000,'FaceFX'),
(0x1040000,0x1100000,'StaticInit_CRT')]
lab={}; anch={}
for a,f in G.items():
  if f['name'].startswith(('Unwind@','Catch')): continue
  if a in names and re.match(r'[AUF]\w*::',names[a]): lab[a]='UE3'; continue
  hits=collections.Counter()
  for r in f['refs']:
    s=astr(r) or wstr(r)
    if not s: continue
    for k,p in ANCH:
      if re.search(p,s): hits[k]+=1
  if hits: anch[a]=hits.most_common(1)[0][0]
for a in G:
  if G[a]['name'].startswith(('Unwind@','Catch')) or a in lab: continue
  pr=next((m for lo,hi,m in PRIOR if lo<=a<hi),None)
  an=anch.get(a)
  if an and not any(m==an and lo-0x10000<=a<hi+0x10000 for lo,hi,m in PRIOR): an=None   # lib strings used by UE glue code
  lab[a]=an or pr or '?'
# boundary refinement: for '?' funcs and funcs in first/last 0x4000 of a prior range, vote by call neighbours
callers=collections.defaultdict(set)
for a,f in G.items():
  for c in f['calls']: callers[c].add(a)
for it in range(4):
  ch=0
  for a in list(lab):
    if a in anch or lab[a]=='UE3': continue
    edge=any(abs(a-lo)<0x4000 or abs(a-hi)<0x4000 for lo,hi,m in PRIOR)
    if not edge: continue
    v=collections.Counter(lab.get(n) for n in set(G[a]['calls'])|callers[a] if lab.get(n) not in (None,'?'))
    if v:
      m=v.most_common(1)[0][0]
      if not any(m2==m and lo-0x10000<=a<hi+0x10000 for lo,hi,m2 in PRIOR) and m!='UE3': continue
      if m!=lab[a]: lab[a]=m; ch+=1
  if not ch: break
# '?' in main range = UE3/game engine code
for a in lab:
  if lab[a]=='?': lab[a]='UE3'
cnt=collections.Counter(lab.values())
rng=collections.defaultdict(lambda:[1<<40,0])
for a,m in lab.items(): rng[m][0]=min(rng[m][0],a); rng[m][1]=max(rng[m][1],a)
for m,n in cnt.most_common(): print(f'{m:15s} {n:6d}  {rng[m][0]:#x}-{rng[m][1]:#x}')
json.dump({f'{a:08x}':m for a,m in lab.items()},open('modules.json','w'))
