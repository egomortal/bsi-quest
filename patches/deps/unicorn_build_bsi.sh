#!/bin/sh
# Rebuild Unicorn 2.1.1 (x86 guest only, static) for Android aarch64 -> /tmp/claude-0/uni-and
# Needs: the NDK sysroot in /home/claude/a64/ndk (transfer\ndk_r27c_arm64.tgz), Ubuntu clang 18, cmake, ninja.
# Source = PyPI sdist (pip download unicorn==2.1.1 --no-binary :all: --no-deps); headers stay in unisrc/unicorn-2.1.1/src/include.
# Takes ~1 min on 2 cores. Recreated 2026-10-05 in a fresh chat; the original recipe was not written down.
set -e
S=/home/claude/a64/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64
RD=$(ls -d $S/lib/clang/* | head -1)
mkdir -p /tmp/claude-0/unisrc && cd /tmp/claude-0/unisrc
[ -f unicorn-2.1.1.tar.gz ] || pip download unicorn==2.1.1 --no-binary :all: --no-deps --no-build-isolation -d .
[ -d unicorn-2.1.1 ] || tar xzf unicorn-2.1.1.tar.gz
# the sdist flattens cmake/ into src/; CMakeLists still includes cmake/bundle_static.cmake
mkdir -p unicorn-2.1.1/src/cmake && cp unicorn-2.1.1/src/bundle_static.cmake unicorn-2.1.1/src/zig.cmake unicorn-2.1.1/src/mingw-w64.cmake unicorn-2.1.1/src/cmake/
# wrapper: qemu's configure links test programs, which need lld + compiler-rt (no libgcc in the NDK)
cat > aclang <<EOF
#!/bin/sh
exec clang --target=aarch64-linux-android29 --sysroot=$S/sysroot -resource-dir $RD -fuse-ld=lld -rtlib=compiler-rt --unwindlib=none -Wno-unused-command-line-argument "\$@"
EOF
chmod +x aclang
cat > android-aarch64.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(ANDROID_ABI arm64-v8a)
set(CMAKE_C_COMPILER /tmp/claude-0/unisrc/aclang)
set(CMAKE_C_COMPILER_TARGET aarch64-linux-android29)
set(CMAKE_SYSROOT $S/sysroot)
set(CMAKE_C_FLAGS_INIT "-fPIC")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
EOF
rm -rf build-and
cmake -S unicorn-2.1.1/src -B build-and -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/android-aarch64.cmake \
  -DCMAKE_BUILD_TYPE=Release -DUNICORN_ARCH=x86 -DBUILD_SHARED_LIBS=OFF -DUNICORN_BUILD_TESTS=OFF -DUNICORN_INSTALL=OFF
nice -n 10 ninja -C build-and -j2
mkdir -p /tmp/claude-0/uni-and
cp build-and/libunicorn-static.a build-and/libx86_64-softmmu.a build-and/libunicorn-common.a /tmp/claude-0/uni-and/
ls -la /tmp/claude-0/uni-and
