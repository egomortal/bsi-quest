#!/bin/sh
# Android (bionic, arm64-v8a) build of all recompiled code -> objl_and/  (for the Quest app's libbsi.so)
cd /home/claude/bsi/recomp
S=/home/claude/a64/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64
CC="clang --target=aarch64-linux-android29 --sysroot=$S/sysroot -resource-dir $(ls -d $S/lib/clang/* | head -1) -fPIC"
export CC
mkdir -p objl_and
{ for c in gen/part*.c gen/missing.c; do echo "$c objl_and/$(basename $c .c).o -Igen"; done
  for d in gen_dll/*/; do n=$(basename $d); for c in $d/part*.c $d/missing.c; do echo "$c objl_and/dll_${n}_$(basename $c .c).o -I$d"; done; done
  echo "gen_dll/functab_all.c objl_and/functab.o -Igen"
} | while read c o inc; do [ -f $o ] && [ $o -nt $c ] || echo "$c $o $inc"; done \
  | xargs -r -P 2 -n 3 sh -c '$CC -O1 -w -fno-strict-aliasing -Igen $2 -c $0 -o $1 || echo "FAILED $0"'
echo ANDROID_OBJS_DONE
