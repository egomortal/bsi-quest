#!/usr/bin/env python3
"""Build an isolated B0 diagnostic libbsi.so from saved v0.9.1 inputs."""
import glob
import argparse
import os
import platform
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

KIT = os.path.dirname(os.path.abspath(__file__))
RUNTIME = """
linux/rt_linux.c linux/hle_crt.c linux/hle_k32.c linux/hle_file.c linux/hle_more.c linux/hle_ui.c
linux/hle_input.c linux/hle_crt2.c linux/memdump.c linux/patches.c linux/natives.c linux/xaudio.c linux/emu.c linux/com.c
linux/com_tab.c linux/imps.c linux/hle_dll.c linux/hle_k32b.c linux/hle_eh.c eh/eh_rt.c linux/bridge.c linux/bridge_gen.c
linux/android_glue.c linux/xr_glue.c linux/vulkan_caps_probe.c linux/prof_android.c linux/dsp_probe.c linux/memwatch.c linux/leakwatch.c
linux/vr_head.c linux/setsweep.c linux/audio_native.c linux/rhi_hook.c linux/rhi_native.c linux/wwise_tune.c
linux/hle_crypt.c linux/mb_off.c
""".split()
DXVK_CPP = "linux/dxvk_core_probe.cpp"


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ndk", required=True)
    ap.add_argument("--output-dir", required=True, help="new directory under this kit")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    host = {"Windows": "windows-x86_64", "Linux": "linux-x86_64", "Darwin": "darwin-x86_64"}.get(platform.system())
    if not host:
        sys.exit("unsupported host for the NDK build")
    prebuilt = os.path.join(args.ndk, "toolchains", "llvm", "prebuilt", host)
    clang = os.path.join(prebuilt, "bin", "clang" + (".exe" if host.startswith("windows") else ""))
    sysroot = os.path.join(prebuilt, "sysroot")
    resource_root = os.path.join(prebuilt, "lib", "clang")
    resources = os.path.join(resource_root, "18")
    if not os.path.isfile(clang) or not os.path.isdir(sysroot) or not os.path.isfile(os.path.join(resources, "include", "stddef.h")):
        sys.exit(f"NDK clang/sysroot/resource headers not found under {prebuilt}")
    out = os.path.abspath(os.path.join(KIT, args.output_dir))
    if os.path.commonpath([KIT, out]) != KIT:
        sys.exit("output directory must stay inside this v0.9.1 build folder")
    if os.path.exists(out):
        sys.exit(f"refusing to overwrite existing output directory: {out}")
    os.makedirs(os.path.join(out, "rt"))
    lib = os.path.join(out, "lib")
    os.makedirs(lib)
    src = os.path.join(KIT, "src")
    base = [clang, "--target=aarch64-linux-android29", "--sysroot=" + sysroot,
            "-resource-dir=" + resources, "-fPIC"]
    cflags = ["-O2", "-mcpu=cortex-a78c", "-g", "-w", "-Wno-error=implicit-function-declaration",
              "-fno-strict-aliasing", "-DBSI_ANDROID", '-DBSI_VERSION="0.9.1-b0core"',
              "-I" + os.path.join(src, "gen"), "-I" + os.path.join(src, "linux"), "-I" + os.path.join(KIT, "include")]

    def compile_one(rel):
        obj = os.path.join(out, "rt", os.path.splitext(os.path.basename(rel))[0] + ".o")
        return rel, *run(base + cflags + ["-c", os.path.join(src, rel), "-o", obj])

    print(f"Compiling {len(RUNTIME)} v0.9.1 runtime files...")
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        failures = [(rel, msg) for rel, code, msg in pool.map(compile_one, RUNTIME) if code]
    for rel, msg in failures:
        print(f"FAILED {rel}\n{msg}\n")
    if failures:
        sys.exit(f"{len(failures)} runtime compile(s) failed")

    dxvk = os.path.join(os.path.dirname(KIT), "dxvk_android_build_005")
    dxvk_src = os.path.join(os.path.dirname(KIT), "deps", "dxvk-v3.1.1")
    cpp = os.path.join(prebuilt, "bin", "clang++.exe" if host.startswith("windows") else "clang++")
    cpp_out = os.path.join(out, "rt", "dxvk_core_probe.o")
    cpp_inc = [
        "-I" + os.path.join(dxvk, "src/dxvk/libdxvk.a.p"), "-I" + os.path.join(dxvk, "src/dxvk"), "-I" + os.path.join(dxvk_src, "src/dxvk"),
        "-I" + os.path.join(dxvk, "include"), "-I" + os.path.join(dxvk_src, "include"),
        "-I" + os.path.join(dxvk_src, "include/vulkan/include"),
        "-I" + os.path.join(dxvk_src, "include/spirv/include"),
        "-I" + os.path.join(dxvk, "include/native"), "-I" + os.path.join(dxvk_src, "include/native"),
        "-I" + os.path.join(dxvk_src, "include/native/windows"),
        "-I" + os.path.join(dxvk_src, "include/native/directx"),
        "-I" + os.path.join(dxvk, "subprojects/dxbc-spirv"), "-I" + os.path.join(dxvk_src, "subprojects/dxbc-spirv"),
        "-I" + dxvk,
    ]
    # B1.1 (2026-10-12): keep every DXVK/C++ symbol of the probe local to libbsi.so (the bridge loads its own DXVK).
    cpp_defs = ["-std=c++20", "-fvisibility=hidden", "-fvisibility-inlines-hidden", "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST", "-DDXVK_WSI_ANDROID",
                "-fPIC", "-pthread", "-O2"]
    code, msg = run([cpp, *base[1:], *cpp_defs, *cpp_inc, "-c", os.path.join(src, DXVK_CPP), "-o", cpp_out])
    if code:
        sys.exit("DXVK probe compile failed:\n" + msg)

    objdir = os.path.join(KIT, "objs")
    objs = sorted(os.path.join(objdir, f) for f in os.listdir(objdir) if f.endswith(".o"))
    objs += sorted(os.path.join(out, "rt", f) for f in os.listdir(os.path.join(out, "rt")) if f.endswith(".o"))
    unidir = os.path.join(KIT, "prebuilt", "unicorn")
    objs += [os.path.join(unidir, f) for f in ("libunicorn-static.a", "libx86_64-softmmu.a", "libunicorn-common.a")]
    rsp = os.path.join(out, "link.rsp")
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"' + p.replace("\\", "/") + '"' for p in objs))
    so = os.path.join(lib, "libbsi.so")
    core_libs = [
        os.path.join(dxvk, "src/dxvk/libdxvk.a"), os.path.join(dxvk, "src/wsi/libwsi.a"),
        os.path.join(dxvk, "src/spirv/libspirv.a"), os.path.join(dxvk, "src/vulkan/libvkcommon.a"),
        os.path.join(dxvk, "src/util/libutil.a"), os.path.join(dxvk, "subprojects/dxbc-spirv/libdxbc_spv.a"),
        os.path.join(dxvk, "subprojects/libdisplay-info/libdisplay-info.a"),
    ]
    missing = [p for p in core_libs if not os.path.isfile(p)]
    if missing:
        sys.exit("DXVK Android static libraries missing: " + ", ".join(missing))
    android_lib = os.path.join(sysroot, "usr", "lib", "aarch64-linux-android")
    unwind_cands = [os.path.join(android_lib, "libunwind.a")] + sorted(
        glob.glob(os.path.join(resources, "lib", "linux", "aarch64", "libunwind.a")) +
        glob.glob(os.path.join(resources, "lib", "linux", "libunwind-aarch64.a")))
    unwind_lib = next((c for c in unwind_cands if os.path.exists(c)), None)
    if not unwind_lib:
        sys.exit("libunwind.a not found in the NDK (looked in: " + ", ".join(unwind_cands) + ")")
    cmd = base + ["-shared", "-o", so, "@" + rsp, "-fuse-ld=lld", "-rtlib=compiler-rt", "--unwindlib=none",
                  "-Wl,--start-group", *core_libs, "-Wl,--end-group",
                  # 2026-10-12 fix (Claude): no "-L" + android_lib. That folder holds Android's STATIC libc.a/libm.a/libdl.a,
                  # and -L made -lm/-ldl/-lc pick them, which put bionic's gwp_asan initial-exec TLS into libbsi.so and made
                  # Android refuse to load it (dlopen "TLS symbol ... using IE access model"; builds 45-48 never started).
                  # Link only the C++ runtime archives by full path; libc/libm/libdl stay the shared system libraries.
                  os.path.join(android_lib, "libc++_static.a"), os.path.join(android_lib, "libc++abi.a"), unwind_lib,
                  "-Wl,--no-undefined", "-Wl,-Bsymbolic-functions", "-landroid", "-laaudio", "-llog", "-lEGL", "-lm", "-ldl",
                  "-Wl,-z,max-page-size=16384", "-Wl,--export-dynamic",
                  # B1.1: symbols from static archives (DXVK core, dxbc-spirv, libc++, unicorn) are not exported, so the
                  # bridge's libdxvk_d3d11.so can never bind to our DXVK copy. Our own objects stay exported as before.
                  "-Wl,--exclude-libs,ALL"]
    print("Linking v0.9.1 plus the opt-in probe...")
    code, msg = run(cmd)
    if code:
        sys.exit("link failed:\n" + msg)
    # Load-safety check (2026-10-12): Android refuses a dlopen'd library that carries initial-exec TLS
    # (static bionic, gwp_asan). Fail the build here instead of on the Quest.
    readelf = os.path.join(os.path.dirname(clang), "llvm-readelf" + (".exe" if os.name == "nt" else ""))
    rc, dyn = run([readelf, "--dynamic", so])
    rc2, syms = run([readelf, "--syms", so])
    if rc or rc2:
        sys.exit("llvm-readelf failed; cannot verify the library:\n" + dyn + syms)
    if "STATIC_TLS" in dyn or "gwp_asan" in syms:
        sys.exit("libbsi.so contains static libc / initial-exec TLS (STATIC_TLS or gwp_asan): Android would refuse to load it")
    rc3, dsyms = run([readelf, "--dyn-syms", "--wide", so])
    leaked = [l for l in dsyms.splitlines() if "N4dxvk" in l and " UND " not in l]
    if rc3 or leaked:
        sys.exit("libbsi.so exports DXVK symbols (B1.1 requires none):\n" + "\n".join(leaked[:20]))
    print("load-safety check passed (no STATIC_TLS, no gwp_asan, no exported DXVK symbols)")
    for name in os.listdir(os.path.join(KIT, "prebuilt", "so")):
        if name.endswith(".so"):
            shutil.copy2(os.path.join(KIT, "prebuilt", "so", name), os.path.join(lib, name))
    print(f"linked {so} ({os.path.getsize(so)} bytes); no APK was installed")


if __name__ == "__main__":
    main()
