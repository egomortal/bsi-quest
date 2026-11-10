"""Recompile one game DLL (rebased copy) to C: BSI_IMAGE=<rebased dll> python3 genall_dll.py <name>
Reads /home/claude/bsi/dlls/bodies_<name>.tsv, writes gen_dll/<name>/part*.c, funcs.h, missing.c, funcs.txt"""
import sys, os, time
sys.path.insert(0,'/home/claude/bsi/recomp')
name = sys.argv[1]
assert 'BSI_IMAGE' in os.environ
from x86c import *
import x86c
from multiprocessing import Pool
# BSI_BODIES_TSV / BSI_GEN_OUT (optional): other bodies file / output dir (scratch generation without touching gen_dll*/)
F = load_bodies(os.environ.get('BSI_BODIES_TSV', f'/home/claude/bsi/dlls/bodies_{name}.tsv')); IMP = load_imports()
x86c.FUNC_STARTS = set(F); x86c.EH_CONTS = {}
keys = sorted(F); CH = 1500
PART_MAX = int(os.environ.get('BSI_PART_MAX', 16 << 20))
chunks = [keys[i:i+CH] for i in range(0, len(keys), CH)]
OUT = f'/home/claude/bsi/recomp/{"gen_dll_promo" if x86c.PROMO else "gen_dll"}/{name}'   # BSI_PROMO=1 -> gen_dll_promo/
if os.environ.get('BSI_GEN_OUT'): OUT = os.environ['BSI_GEN_OUT'] + '/' + name
os.makedirs(OUT, exist_ok=True)
def work(ci):
    srcs=[]; refs=set(); bad=0; unsup=0
    for a in chunks[ci]:
        rg, sw = F[a]
        try:
            fn = Fn(a, rg, sw, None, IMP); ok, src = fn.emit(); refs |= fn.calls; unsup += (not ok)
            if fn.promo_fallback: print('promo fallback (emitted unpromoted)', hex(a), fn.promo_fallback, flush=True)
        except Exception as e:
            src = f'void f_{a:08x}(CPU*c){{ unimpl(c,0x{a:x}u,"translator crash"); }}\n'; bad+=1
        srcs.append(src)
    # parts bigger than PART_MAX bytes are split at function boundaries into part<ci>.c, part<ci>b.c, part<ci>c.c, ...
    # (a 64 MB PhysX part needed > 4.5 GB in cc1); smaller parts are written exactly as before
    pieces=[[]]; size=0
    for src in srcs:
        if pieces[-1] and size+len(src)>PART_MAX: pieces.append([]); size=0
        pieces[-1].append(src); size+=len(src)
    for k,pc in enumerate(pieces):
        txt='#include "funcs.h"\n' + ''.join(pc); fn=f'{OUT}/part{ci:03d}{"" if k==0 else chr(97+k)}.c'
        old=open(fn).read() if os.path.exists(fn) else None
        if old!=txt: open(fn,'w').write(txt)
    return refs, bad, unsup
if __name__=='__main__':
    t=time.time()
    with Pool(2) as p: res = p.map(work, range(len(chunks)))
    refs=set(); bad=0; uns=0
    for r,b,u in res: refs|=r; bad+=b; uns+=u
    allf=set(keys); missing=sorted(refs-allf)
    open(f'{OUT}/funcs.h','w').write('#include "cpu.h"\n'+''.join(f'void f_{a:08x}(CPU*c);\n' for a in sorted(allf|refs)))
    with open(f'{OUT}/missing.c','w') as f:
        f.write('#include "funcs.h"\n')
        for a in missing: f.write(f'void f_{a:08x}(CPU*c){{ call_indirect(c,0x{a:x}u); }}\n')
    open(f'{OUT}/funcs.txt','w').write(''.join(f'{a:08x}\n' for a in keys))
    print(name,'functions',len(keys),'chunks',len(chunks),'crashes',bad,'with unsupported insns',uns,'outside targets',len(missing),'time',round(time.time()-t))
