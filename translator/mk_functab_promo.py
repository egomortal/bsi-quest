"""mk_functab_promo.py <exe functab.c or bodies.tsv> <dll dir>... -o <out.c>
Combined exe + DLL function table for the promoted builds (what build_dll_linux.sh does for gen_dll/functab_all.c,
but from the promoted sources). Each <dll dir> holds a funcs.txt (gen_dll_promo/<name>/ or a scratch copy)."""
import sys, re, os
args = sys.argv[1:]; out = args[args.index('-o') + 1]; args = args[:args.index('-o')]
src = args[0]
if src.endswith('.tsv'): addrs = [int(l.split('\t')[0], 16) for l in open(src) if l.strip()]
else: addrs = [int(m, 16) for m in re.findall(r'\{0x([0-9a-f]+)u,f_', open(src).read())]
for d in args[1:]: addrs += [int(l, 16) for l in open(os.path.join(d, 'funcs.txt')) if l.strip()]
addrs = sorted(set(addrs))
with open(out, 'w') as f:
    f.write('#include "cpu.h"\n' + ''.join(f'void f_{a:08x}(CPU*c);\n' for a in addrs))
    f.write('typedef struct { uint32_t addr; gfn fn; } FEnt;\nconst FEnt g_functab[]={\n' + ''.join(f'{{0x{a:x}u,f_{a:08x}}},\n' for a in addrs) + '};\n')
    f.write(f'const uint32_t g_functab_n={len(addrs)};\n')
print('functab entries', len(addrs), '->', out)
