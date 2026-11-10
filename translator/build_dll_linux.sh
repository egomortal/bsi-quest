#!/bin/sh
# Compile recompiled game DLLs (gen_dll/<name>/) into objl/ and build a combined function table (exe + DLLs) for the Linux runtime.
cd /home/claude/bsi/recomp
for d in gen_dll/*/; do n=$(basename $d)
  for c in $d/part*.c $d/missing.c; do o=objl/dll_${n}_$(basename $c .c).o; [ -f $o ] && [ $o -nt $c ] || echo "$c $o"; done
done | xargs -r -P 2 -n 2 sh -c 'gcc -O1 -w -fno-strict-aliasing -Igen -I$(dirname $0) -c $0 -o $1'
python3 - <<'P'
import glob,re
addrs=[int(m,16) for m in re.findall(r'\{0x([0-9a-f]+)u,f_',open('gen/functab.c').read())]
for f in glob.glob('gen_dll/*/funcs.txt'): addrs+=[int(l,16) for l in open(f) if l.strip()]
addrs=sorted(set(addrs))
with open('gen_dll/functab_all.c','w') as f:
    f.write('#include "cpu.h"\n'+''.join(f'void f_{a:08x}(CPU*c);\n' for a in addrs))
    f.write('typedef struct { uint32_t addr; gfn fn; } FEnt;\nconst FEnt g_functab[]={\n'+''.join(f'{{0x{a:x}u,f_{a:08x}}},\n' for a in addrs)+'};\n')
    f.write(f'const uint32_t g_functab_n={len(addrs)};\n')
print('functab entries',len(addrs))
P
gcc -O1 -w -Igen -c gen_dll/functab_all.c -o objl/functab.o
./relink_linux.sh
ls -la bsi_linux
