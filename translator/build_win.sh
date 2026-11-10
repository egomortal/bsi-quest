#!/bin/sh
# Build bsirecomp.dll (i686 Windows) from gen/*.c + win runtime
set -e
cd /home/claude/bsi/recomp
CC=i686-w64-mingw32-gcc
CFLAGS="-O1 -w -fno-strict-aliasing -msse2 -mfpmath=sse -DIDENTITY -I/home/claude/bsi/recomp/gen -fno-asynchronous-unwind-tables"
ls gen/part*.c gen/missing.c gen/functab.c | xargs -P 2 -I{} sh -c 'o=obj/$(basename {} .c).o; [ -f $o ] && [ $o -nt {} ] || '"$CC $CFLAGS"' -c {} -o $o'
$CC $CFLAGS -c win/rt_win.c -o obj/rt_win.o
$CC $CFLAGS -c win/imports.c -o obj/imports.o
$CC $CFLAGS -c eh/eh_rt.c -o obj/eh_rt.o
$CC -c win/gate.S -o obj/gate.o
$CC -shared -o bsirecomp.dll obj/*.o -Wl,--enable-stdcall-fixup -Wl,--image-base,0x30000000 -static-libgcc -lm
ls -la bsirecomp.dll
