#!/bin/sh
# Android (bionic, arm64-v8a) -O2 build of the register/flag-promoted code (gen_promo/ + gen_dll_promo/) -> objl_and_promo/
# Link a Quest build from it with:  OBJDIR=objl_and_promo ./link_android.sh   (objl_and/ and objl_and_o2/ are not touched)
cd /home/claude/bsi/recomp
S=/home/claude/a64/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64
CC="clang --target=aarch64-linux-android29 --sysroot=$S/sysroot -resource-dir $(ls -d $S/lib/clang/* | head -1) -fPIC"
export CC
# FIXED_M=<hex address> (default 0x2000000000): constant guest memory base in the generated code (cpu.h BSI_FIXED_M); needs a runtime with
# guest_reserve() (rt_linux.c, step 4), which link_android.sh compiles. FIXED_M= (empty) keeps the variable base.
# Changing it recompiles every generated object (stamp objl_and_promo/.genflags).
FIXED_M=${FIXED_M-0x2000000000}
GENF="-O2 -mcpu=cortex-a78c -w -fno-strict-aliasing${FIXED_M:+ -DBSI_FIXED_M=${FIXED_M}ULL}"
export GENF
mkdir -p objl_and_promo
if [ "$(cat objl_and_promo/.genflags 2>/dev/null)" != "$GENF" ]; then echo "generated-code flags changed -> full rebuild of objl_and_promo"; rm -f objl_and_promo/part*.o objl_and_promo/missing.o objl_and_promo/dll_*.o objl_and_promo/functab.o; echo "$GENF" > objl_and_promo/.genflags; fi
cp cpu.h gen_promo/; for d in gen_dll_promo/*/; do cp cpu.h $d; done
{ for c in gen_promo/part*.c gen_promo/missing.c; do echo "$c objl_and_promo/$(basename $c .c).o -Igen_promo"; done
  for d in gen_dll_promo/*/; do n=$(basename $d); for c in $d/part*.c $d/missing.c; do echo "$c objl_and_promo/dll_${n}_$(basename $c .c).o -I$d"; done; done
  FT=gen_dll_promo/functab_all.c; [ -f $FT ] || FT=gen_dll/functab_all.c   # gen_dll_promo/: mk_functab_promo.py (2026-10-04 emulator census)
  echo "$FT objl_and_promo/functab.o -Igen_promo"
} | while read c o inc; do [ -f $o ] && [ $o -nt $c ] || echo "$c $o $inc"; done \
  | xargs -r -P ${JOBS:-2} -n 3 sh -c '$CC $GENF $2 -c $0 -o $1 || echo "FAILED $0"'
for o in objl_and_promo/part*.o; do [ -f gen_promo/$(basename $o .o).c ] || rm -f $o; done
echo ANDROID_PROMO_OBJS_DONE
