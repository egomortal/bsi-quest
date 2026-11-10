#!/bin/bash
set -e
NDK=/home/claude/a64/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64
CXX="clang++ --target=aarch64-linux-android29 --sysroot=$NDK/sysroot -resource-dir $NDK/lib/clang/18 -stdlib=libc++ -std=c++17 -fPIC -O2 -fvisibility=hidden"
D="-DXR_OS_ANDROID -DXR_USE_PLATFORM_ANDROID -DXR_KHR_LOADER_INIT_SUPPORT -DXR_USE_TIMESPEC -DXR_USE_GRAPHICS_API_VULKAN -DXRLOADER_DISABLE_EXCEPTION_HANDLING -DJSON_USE_EXCEPTION=0 -DXRAPI_DLL_EXPORT -DXR_CURRENT_API_MAJOR_VERSION=1 -DXR_CURRENT_API_MINOR_VERSION=1 -DXR_CURRENT_API_PATCH_VERSION=63"
I="-Iinclude -Isrc -Isrc/common -Isrc/loader -Isrc/external/jsoncpp/include -Isrc/external/jnipp -Isrc/external/android-jni-wrappers"
SRCS="$(ls src/loader/*.cpp) src/common/object_info.cpp src/common/filesystem_utils.cpp src/external/jsoncpp/src/lib_json/json_reader.cpp src/external/jsoncpp/src/lib_json/json_value.cpp src/external/jsoncpp/src/lib_json/json_writer.cpp src/external/jnipp/jnipp.cpp $(ls src/external/android-jni-wrappers/wrap/*.cpp)"
OBJS=""
for f in $SRCS; do o=bout/$(echo $f | tr / _).o; $CXX $D $I -Wno-everything -c $f -o $o; OBJS="$OBJS $o"; done
NDK2="clang --target=aarch64-linux-android29 --sysroot=$NDK/sysroot -resource-dir $NDK/lib/clang/18 -fPIC -O2 -fvisibility=hidden"
$NDK2 $D $I -c src/xr_generated_dispatch_table_core.c -o bout/dtc.o; OBJS="$OBJS bout/dtc.o"
$CXX -shared -fuse-ld=lld -rtlib=compiler-rt --unwindlib=libunwind -static-libstdc++ -Wl,--no-undefined -Wl,-z,max-page-size=16384 -Wl,-soname,libopenxr_loader.so $OBJS -o bout/libopenxr_loader.so -llog -landroid -ldl
ls -la bout/libopenxr_loader.so
