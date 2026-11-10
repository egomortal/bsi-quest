import pefile,struct,re
data=open('/home/claude/bsi/BioShockInfinite.exe','rb').read()
pe=pefile.PE(data=data,fast_load=True); IB=pe.OPTIONAL_HEADER.ImageBase
SECS={s.Name.rstrip(b'\0').decode():s for s in pe.sections}
def va2off(va):
  for s in pe.sections:
    if s.VirtualAddress<=va-IB<s.VirtualAddress+s.SizeOfRawData: return va-IB-s.VirtualAddress+s.PointerToRawData
def astr(va):
  o=va2off(va)
  if o is None: return None
  e=data.find(b'\0',o); s=data[o:e]
  return s.decode() if 2<=len(s)<200 and all(31<c<127 or c in (9,10,13) for c in s) else None
def wstr(va):
  o=va2off(va)
  if o is None: return None
  e=o
  while e+1<len(data) and data[e:e+2]!=b'\0\0': e+=2
  s=data[o:e]
  try: s=s.decode('utf-16le')
  except: return None
  return s if 2<=len(s)<200 and all(31<ord(c)<127 or c in '\t\r\n' for c in s) else None
def u32(va):
  o=va2off(va); return struct.unpack_from('<I',data,o)[0] if o is not None else None
T=SECS['.text']; TLO=IB+T.VirtualAddress; THI=TLO+T.Misc_VirtualSize
def load_graph():
  G={}
  for l in open('/home/claude/bsi/graph.tsv'):
    a,n,c,r=l.rstrip('\n').split('\t')
    G[int(a,16)]=dict(name=n,calls=[int(x,16) for x in c.split(',') if x and ':' not in x],
      refs=[int(x,16) for x in r.split(',') if x and not x.startswith('Stack') and re.fullmatch(r'[0-9a-f]+',x)])
  return G
