#!/bin/sh
# ARM64 build of all recompiled code (exe + DLLs) -> objl_a64/, then relink_a64.sh
cd /home/claude/bsi/recomp
mkdir -p objl_a64
{ for c in gen/part*.c gen/missing.c; do echo "$c objl_a64/$(basename $c .c).o -Igen"; done
  for d in gen_dll/*/; do n=$(basename $d); for c in $d/part*.c $d/missing.c; do echo "$c objl_a64/dll_${n}_$(basename $c .c).o -I$d"; done; done
  echo "gen_dll/functab_all.c objl_a64/functab.o -Igen"
} | while read c o inc; do [ -f $o ] && [ $o -nt $c ] || echo "$c $o $inc"; done \
  | xargs -r -P 2 -n 3 sh -c 'aarch64-linux-gnu-gcc -O1 -w -fno-strict-aliasing -Igen $2 -c $0 -o $1 || echo "FAILED $0"'
sh relink_a64.sh
