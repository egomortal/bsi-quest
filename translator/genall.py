import sys, os, collections, time
sys.path.insert(0,'/home/claude/bsi/recomp')
from x86c import *
from multiprocessing import Pool
# BSI_BODIES_TSV / BSI_GEN_OUT (optional): other bodies file / output dir; BSI_GEN_CHUNKS=i,j,..: emit only these parts
# (scratch builds; missing.c, funcs.h and functab.c are then not written)
F = load_bodies(os.environ.get('BSI_BODIES_TSV', '/home/claude/bsi/bodies.tsv')); IMP = load_imports()
import x86c, json; x86c.FUNC_STARTS = set(F)
x86c.EH_CONTS = {int(k,16):[int(x,16) for x in v] for k,v in json.load(open('/home/claude/bsi/recomp/eh/eh_conts.json')).items()}
x86c.TRACE_FUNCS = set(int(a,16) for a in __import__('os').environ.get('BSI_TRACE_FUNCS','').split(',') if a)
keys = sorted(F); CH = 1500
# BSI_PROMO=1: register/flag-promoted code goes to gen_promo/ (same chunking as gen/); gen/ is left untouched
OUTD = '/home/claude/bsi/recomp/' + ('gen_promo' if x86c.PROMO else 'gen')
if os.environ.get('BSI_GEN_OUT'): OUTD = os.environ['BSI_GEN_OUT']
# chunk boundaries are frozen in gen/chunk_starts.json so that adding functions only rewrites the chunks they land in
import bisect
_cs = '/home/claude/bsi/recomp/gen/chunk_starts.json'
if os.path.exists(_cs): starts = json.load(open(_cs))
else:
    starts = [keys[i] for i in range(0, len(keys), CH)]; os.makedirs(os.path.dirname(_cs), exist_ok=True); json.dump(starts, open(_cs, 'w'))
chunks = [[] for _ in starts]
for a in keys: chunks[max(0, bisect.bisect_right(starts, a) - 1)].append(a)
os.makedirs(OUTD, exist_ok=True)
# Promoted mode: functions with a native replacement (linux/natives.c, step 5 of OVERNIGHT.md) get their recompiled body
# emitted as f_XXXXXXXX_x86; natives.c defines f_XXXXXXXX (weak) as a dispatcher (env BSI_NATIVE) that calls either.
NATIVE_X86 = {0x00e106e0, 0x00ddc170, 0x00480850, 0x00889a50} if x86c.PROMO else set()
def native_rename(a, src):
    h = f'void f_{a:08x}(CPU*c){{'
    assert src.startswith(h), hex(a)
    return f'void f_{a:08x}_x86(CPU*c);\nvoid f_{a:08x}_x86(CPU*c){{' + src[len(h):]
def work(ci):
    ks = chunks[ci]; srcs=[]; refs=set(); bad=0
    for a in ks:
        rg, sw = F[a]
        try:
            fn = Fn(a, rg, sw, None, IMP); ok, src = fn.emit(); refs |= fn.calls
            if fn.promo_fallback: print('promo fallback (emitted unpromoted)', hex(a), fn.promo_fallback, flush=True)
        except Exception as e:
            src = f'void f_{a:08x}(CPU*c){{ unimpl(c,0x{a:x}u,"translator crash"); }}\n'; bad+=1
        if a in NATIVE_X86: src = native_rename(a, src)
        srcs.append(src)
    txt='#include "funcs.h"\n' + ''.join(srcs); fn=f'{OUTD}/part{ci:03d}.c'
    old=open(fn).read() if os.path.exists(fn) else None
    if old!=txt: open(fn,'w').write(txt)
    return refs, bad
if __name__=='__main__':
    t=time.time()
    only = [int(x) for x in os.environ.get('BSI_GEN_CHUNKS', '').split(',') if x]
    with Pool(2) as p: res = p.map(work, only or range(len(chunks)))
    if only: print('chunks written', only, 'crashes', sum(b for r, b in res), 'time', round(time.time()-t)); sys.exit(0)
    refs=set(); bad=0
    for r,b in res: refs|=r; bad+=b
    allf = set(keys)
    missing = sorted(refs - allf)
    hdr='#include "cpu.h"\n'+''.join(f'void f_{a:08x}(CPU*c);\n' for a in sorted(allf | refs))
    if not os.path.exists(f'{OUTD}/funcs.h') or open(f'{OUTD}/funcs.h').read()!=hdr:
        open(f'{OUTD}/funcs.h','w').write(hdr)
    with open(f'{OUTD}/missing.c','w') as f:
        f.write('#include "funcs.h"\n')
        for a in missing: f.write(f'void f_{a:08x}(CPU*c){{ call_indirect(c,0x{a:x}u); }}\n')
    with open(f'{OUTD}/functab.c','w') as f:
        f.write('#include "funcs.h"\ntypedef struct { uint32_t addr; gfn fn; } FEnt;\nconst FEnt g_functab[]={\n')
        for a in sorted(allf): f.write(f'{{0x{a:x}u,f_{a:08x}}},\n')
        f.write(f'}};\nconst uint32_t g_functab_n={len(allf)};\n')
    print('chunks',len(chunks),'crashes',bad,'call targets outside known functions',len(missing),'time',round(time.time()-t))
