"""Generate COM method tables for the null D3D11/DXGI runtime from mingw-w64 headers."""
import re, json
HDR='/usr/share/mingw-w64/include/'
files=['d3dcommon.h','dxgi.h','dxgi1_2.h','d3d11.h']
ifaces={}; order=[]; iids={}
BIG={'UINT64':8,'INT64':8,'LARGE_INTEGER':8,'ULARGE_INTEGER':8,'double':8,'DOUBLE':8,'LONGLONG':8,'ULONGLONG':8}
for f in files:
    src=open(HDR+f).read()
    for name,body in re.findall(r'typedef struct (\w+)Vtbl \{(.*?)\} \1Vtbl;',src,re.S):
        if name in ifaces: continue
        ms=[]
        for ret,mname,params in re.findall(r'(\w[\w\s\*]*?)\s*\(STDMETHODCALLTYPE \*(\w+)\)\(\s*(.*?)\);',body,re.S):
            ps=[p.strip() for p in params.replace('\n',' ').split(',') if p.strip()]
            nbytes=0; outiface=None; types=[]
            for p in ps:
                p=re.sub(r'\s+',' ',p)
                t=p.rsplit(' ',1)[0] if ' ' in p else p
                isptr='*' in p or '[' in p
                sz=4 if isptr else BIG.get(t.replace('const ','').strip(),4)
                nbytes+=sz; types.append(p)
            last=ps[-1] if ps else ''
            m=re.match(r'(?:const )?(I\w+)\s*\*\*',re.sub(r'\s+',' ',last))
            if m: outiface=m.group(1)
            ms.append(dict(name=mname,ret=ret.strip(),bytes=nbytes,out=outiface,params=types))
        ifaces[name]=ms; order.append(name)
    for m in re.finditer(r'__CRT_UUID_DECL\((\w+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*((?:0x[0-9a-fA-F]+,\s*){7}0x[0-9a-fA-F]+)\)',src):
        n=m.group(1); d1,d2,d3=int(m.group(2),16),int(m.group(3),16),int(m.group(4),16)
        b=[int(x,16) for x in m.group(5).replace(' ','').split(',')]
        iids[n]=[d1,d2,d3]+b
# DECLARE_INTERFACE_ style headers (DirectInput 8)
src=open(HDR+'dinput.h').read()
for name,body in re.findall(r'DECLARE_INTERFACE_\((\w+),\s*\w+\)\s*\{(.*?)\};',src,re.S):
    if name not in ('IDirectInput8W','IDirectInputDevice8W','IDirectInput8A','IDirectInputDevice8A') or name in ifaces: continue
    ms=[]
    for ret,mname,params in re.findall(r'STDMETHOD_?\((?:(\w+),)?(\w+)\)\((.*?)\)\s*PURE;',body,re.S):
        ps=[p.strip() for p in params.replace('THIS_','').replace('THIS','').split(',') if p.strip()]
        last=ps[-1] if ps else ''
        mo=re.match(r'(?:LP)?(IDirectInputDevice8W|IDirectInputDevice8A)\s*\*',last) or re.match(r'LP(DIRECTINPUTDEVICE8)W\s*\*',last)
        out='IDirectInputDevice8W' if mo and 'W' in last[:30] else ('IDirectInputDevice8A' if mo else None)
        ms.append(dict(name=mname,ret=ret or 'HRESULT',bytes=4+4*len(ps),out=out,params=ps))
    ifaces[name]=ms; order.append(name)
for m in re.finditer(r'DEFINE_GUID\(IID_(\w+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*((?:0x[0-9a-fA-F]+,\s*){7}0x[0-9a-fA-F]+)\)',src):
    n=m.group(1); iids[n]=[int(m.group(2),16),int(m.group(3),16),int(m.group(4),16)]+[int(x,16) for x in m.group(5).replace(' ','').split(',')]
json.dump({'order':order,'ifaces':ifaces,'iids':iids},open('com_spec.json','w'))
# C tables
with open('com_tab.c','w') as f:
    f.write('#include <stdint.h>\n#include "com.h"\n')
    allm=[]
    for i,n in enumerate(order):
        for k,m in enumerate(ifaces[n]): allm.append((i,k,n,m))
    f.write(f'const int g_com_nifaces={len(order)};\nconst ComIface g_com_ifaces[]={{\n')
    base=0
    for i,n in enumerate(order):
        iid=iids.get(n,[0]*11)
        f.write(f'{{"{n}",{base},{len(ifaces[n])},{{0x{iid[0]:x}u,0x{iid[1]:x},0x{iid[2]:x},{{{",".join(str(x) for x in iid[3:])}}}}}}},\n')
        base+=len(ifaces[n])
    f.write('};\n')
    f.write(f'const int g_com_nmethods={len(allm)};\nconst ComMethod g_com_methods[]={{\n')
    for i,k,n,m in allm:
        rk = 0 if m['ret']=='HRESULT' else (1 if m['ret']=='void' else 2)
        oi = order.index(m['out']) if m['out'] in order else -1
        f.write(f'{{"{n}::{m["name"]}",{i},{k},{m["bytes"]},{rk},{oi}}},\n')
    f.write('};\n')
print(len(order),'interfaces', sum(len(v) for v in ifaces.values()),'methods', len(iids),'iids')
