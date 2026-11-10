#!/bin/sh
# ARM64 (aarch64 Linux/glibc) build of the runtime + link with objl_a64/*.o -> bsi_a64
cd /home/claude/bsi/recomp
A=/home/claude/a64
for f in linux/rt_linux.c linux/hle_crt.c linux/hle_k32.c linux/hle_file.c linux/hle_more.c linux/hle_ui.c linux/hle_input.c linux/hle_crt2.c linux/memdump.c linux/patches.c linux/xaudio.c linux/emu.c linux/com.c linux/com_tab.c linux/imps.c linux/hle_dll.c linux/hle_k32b.c linux/hle_eh.c eh/eh_rt.c linux/bridge.c linux/bridge_gen.c; do
  [ -f $f ] && aarch64-linux-gnu-gcc -O1 -g -w -fno-strict-aliasing -Igen -Ilinux -I$A/sdl2/include -I/usr/include/unicorn/.. -c $f -o objl_a64/rt_$(basename $f .c).o || echo "FAILED $f"
done
aarch64-linux-gnu-gcc -o bsi_a64 objl_a64/*.o -rdynamic -L$A/sdl2/lib -L$A/lib -lm -lpthread -l:libunicorn.so.2 -ldl -lSDL2 -Wl,-rpath,'$ORIGIN/a64lib'
ls -la bsi_a64
