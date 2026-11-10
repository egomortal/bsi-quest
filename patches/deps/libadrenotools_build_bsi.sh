#!/bin/bash
set -e
NDK=/home/claude/a64/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64
CC="clang --target=aarch64-linux-android29 --sysroot=$NDK/sysroot -resource-dir $NDK/lib/clang/18 -fPIC -O2"
CXX="clang++ --target=aarch64-linux-android29 --sysroot=$NDK/sysroot -resource-dir $NDK/lib/clang/18 -stdlib=libc++ -std=c++20 -fPIC -O2"
LD="-fuse-ld=lld -rtlib=compiler-rt --unwindlib=libunwind -static-libstdc++ -shared -Wl,--no-undefined -Wl,-z,max-page-size=16384"
O=out; mkdir -p $O
I="-Iinclude -I. -Ilib/linkernsbypass"
$CXX $I -c lib/linkernsbypass/android_linker_ns.cpp -o $O/lns.o
$CXX $I -c lib/linkernsbypass/elf_soname_patcher.cpp -o $O/esp.o
$CXX $I -c src/driver.cpp -o $O/driver.o
llvm-ar rcs $O/libadrenotools_bsi.a $O/lns.o $O/esp.o $O/driver.o
$CXX $I -fvisibility=hidden -c src/hook/hook_impl.cpp -o $O/hook_impl.o
$CXX $LD -Wl,-soname,libhook_impl.so $O/hook_impl.o $O/lns.o $O/esp.o -o $O/libhook_impl.so -ldl -llog -landroid
for h in main_hook file_redirect_hook gsl_alloc_hook; do
  $CC $I -fvisibility=hidden -c src/hook/$h.c -o $O/$h.o
  clang --target=aarch64-linux-android29 --sysroot=$NDK/sysroot -resource-dir $NDK/lib/clang/18 -fuse-ld=lld -rtlib=compiler-rt --unwindlib=libunwind -shared -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,-z,global -Wl,-soname,lib$h.so $O/$h.o -L$O -lhook_impl -o $O/lib$h.so
done
ls -la $O
$CXX $LD -Wl,-soname,libadrenotools.so $O/driver.o $O/lns.o $O/esp.o -o $O/libadrenotools.so -ldl -llog -landroid
ls -la $O/*.so
