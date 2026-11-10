#!/bin/sh
# Linux x86-64 build of the register/flag-promoted code (gen_promo/ + gen_dll_promo/) -> objl_promo/ -> bsi_linux_promo
# (generate first: BSI_PROMO=1 python3 genall.py && ./gen_dll_promo.sh). Leaves gen/, objl/ and bsi_linux untouched.
# FIXED_M=<hex address> (default 0x2000000000) compiles the generated code with a constant guest memory base (cpu.h BSI_FIXED_M; the runtime
# then reserves guest memory exactly there). FIXED_M= (empty) keeps the variable base. OBJ/OUT pick the object dir and
# binary name. Changing FIXED_M recompiles all generated objects (stamp file $OBJ/.genflags).
set -e
cd /home/claude/bsi/recomp
FIXED_M=${FIXED_M-0x2000000000}
OBJ=${OBJ:-objl_promo}; OUT=${OUT:-bsi_linux_promo}
GENF="-O1 -w -fno-strict-aliasing${FIXED_M:+ -DBSI_FIXED_M=${FIXED_M}ULL}"
mkdir -p $OBJ
if [ "$(cat $OBJ/.genflags 2>/dev/null)" != "$GENF" ]; then echo "generated-code flags changed -> full rebuild of $OBJ"; rm -f $OBJ/part*.o $OBJ/missing.o $OBJ/dll_*.o $OBJ/functab.o; echo "$GENF" > $OBJ/.genflags; fi
export GENF
cp cpu.h gen_promo/; for d in gen_dll_promo/*/; do cp cpu.h $d; done
{ for c in gen_promo/part*.c gen_promo/missing.c; do echo "$c $OBJ/$(basename $c .c).o -Igen_promo"; done
  for d in gen_dll_promo/*/; do n=$(basename $d); for c in $d/part*.c $d/missing.c; do echo "$c $OBJ/dll_${n}_$(basename $c .c).o -I$d"; done; done
} | while read c o inc; do [ -f $o ] && [ $o -nt $c ] || echo "$c $o $inc"; done \
  | xargs -r -P 2 -n 3 sh -c 'gcc $GENF $2 -c $0 -o $1 || echo "FAILED $0"'
for o in $OBJ/part*.o; do [ -f gen_promo/$(basename $o .o).c ] || rm -f $o; done
# combined exe+DLL function table: gen_dll_promo/functab_all.c (mk_functab_promo.py, includes the functions added by the
# 2026-10-04 emulator census) when present, else the older gen_dll/functab_all.c
FT=gen_dll_promo/functab_all.c; [ -f $FT ] || FT=gen_dll/functab_all.c
[ -f $OBJ/functab.o ] && [ $OBJ/functab.o -nt $FT ] || gcc $GENF -Igen_promo -c $FT -o $OBJ/functab.o
for f in linux/rt_linux.c linux/hle_crt.c linux/hle_k32.c linux/hle_file.c linux/hle_more.c linux/hle_ui.c linux/hle_input.c linux/hle_crt2.c linux/memdump.c linux/patches.c linux/natives.c linux/xaudio.c linux/emu.c linux/com.c linux/com_tab.c linux/imps.c linux/hle_dll.c linux/hle_k32b.c linux/hle_eh.c eh/eh_rt.c linux/bridge.c linux/bridge_gen.c linux/rhi_hook.c linux/rhi_native.c linux/prof_android.c linux/wwise_tune.c linux/hle_crypt.c linux/mb_off.c; do
  [ -f $f ] && gcc -O1 -g -w -fno-strict-aliasing -Igen_promo -Ilinux -c $f -o $OBJ/$(basename $f .c).o
done
gcc -o $OUT $OBJ/*.o -Wl,-Map=$OBJ/$OUT.map -rdynamic -lm -lpthread -lunicorn -ldl -lSDL2
ls -la $OUT
