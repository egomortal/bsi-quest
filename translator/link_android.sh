#!/bin/sh
# Android runtime + link -> apk/lib/libbsi.so (NativeActivity), with DXVK + Unicorn, then build the APK
cd /home/claude/bsi/recomp
S=/home/claude/a64/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64
CC="clang --target=aarch64-linux-android29 --sysroot=$S/sysroot -resource-dir $(ls -d $S/lib/clang/* | head -1) -fPIC"
U=/tmp/claude-0/uni-and; UI=/tmp/claude-0/unisrc/unicorn-2.1.1/src/include
VER=$(grep -o 'versionName="[^"]*"' /home/claude/apk/AndroidManifest.xml | cut -d'"' -f2)
mkdir -p objl_and_rt /home/claude/apk/lib
for f in linux/rt_linux.c linux/hle_crt.c linux/hle_k32.c linux/hle_file.c linux/hle_more.c linux/hle_ui.c linux/hle_input.c linux/hle_crt2.c linux/memdump.c linux/patches.c linux/natives.c linux/xaudio.c linux/emu.c linux/com.c linux/com_tab.c linux/imps.c linux/hle_dll.c linux/hle_k32b.c linux/hle_eh.c eh/eh_rt.c linux/bridge.c linux/bridge_gen.c linux/rhi_hook.c linux/rhi_native.c linux/android_glue.c linux/xr_glue.c linux/prof_android.c linux/dsp_probe.c linux/memwatch.c linux/wwise_tune.c linux/hle_crypt.c linux/mb_off.c; do
  $CC -O1 -g -w -Wno-error=implicit-function-declaration -fno-strict-aliasing -DBSI_ANDROID -DBSI_VERSION=\"$VER\" -Igen -Ilinux -I$UI -I/home/claude/tools/OpenXR-SDK/include -c $f -o objl_and_rt/$(basename $f .c).o || echo "FAILED $f"
done
OBJS=$(for o in ${OBJDIR:-objl_and}/*.o; do b=$(basename $o); if [ -n "$OVR" ] && [ -f $OVR/$b ]; then echo $OVR/$b; else echo $o; fi; done)
$CC -shared -o /home/claude/apk/lib/libbsi.so $OBJS objl_and_rt/*.o $U/libunicorn-static.a $U/libx86_64-softmmu.a $U/libunicorn-common.a \
  -fuse-ld=lld -rtlib=compiler-rt --unwindlib=none -Wl,--no-undefined -Wl,-Bsymbolic-functions -landroid -laaudio -llog -lEGL -lm -ldl -Wl,-z,max-page-size=16384 -Wl,--export-dynamic
ls -la /home/claude/apk/lib/libbsi.so
