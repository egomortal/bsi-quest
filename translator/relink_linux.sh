#!/bin/sh
cd /home/claude/bsi/recomp
for f in linux/rt_linux.c linux/hle_crt.c linux/hle_k32.c linux/hle_file.c linux/hle_more.c linux/hle_ui.c linux/hle_input.c linux/hle_crt2.c linux/memdump.c linux/patches.c linux/xaudio.c linux/emu.c linux/com.c linux/com_tab.c linux/imps.c linux/hle_dll.c linux/hle_k32b.c linux/hle_eh.c eh/eh_rt.c linux/bridge.c linux/bridge_gen.c; do [ -f $f ] && gcc -O1 -g -w -fno-strict-aliasing -Igen -Ilinux -c $f -o objl/$(basename $f .c).o; done
gcc -o bsi_linux objl/*.o -rdynamic -lm -lpthread -lunicorn -ldl -lSDL2
