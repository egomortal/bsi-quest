#!/bin/sh
# Full Linux build: regenerate C, compile gen/*.c (host gcc, 2 jobs), compile runtime, link -> bsi_linux
set -e
cd /home/claude/bsi/recomp
python3 genall.py && cp cpu.h gen/
ls gen/part*.c gen/missing.c gen/functab.c | xargs -P 2 -I{} sh -c 'o=objl/$(basename {} .c).o; [ -f $o ] && [ $o -nt {} ] || gcc -O1 -w -fno-strict-aliasing -Igen -c {} -o $o'
# drop objects of chunks that no longer exist
for o in objl/part*.o; do [ -f gen/$(basename $o .o).c ] || rm -f $o; done
sh build_dll_linux.sh   # combined exe+DLL function table, then relink
ls -la bsi_linux
