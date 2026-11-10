#!/usr/bin/env python3
"""BSI-Quest setup check.

Checks your game folder and tools and reports which pipeline stages (PIPELINE.md) can run on this PC.
It reads the game folder only; it never copies, changes or uploads anything from it.

    python setup.py --game "C:\\Program Files (x86)\\Steam\\steamapps\\common\\BioShock Infinite" [--ndk <path>] [--ghidra <path>]
"""
import argparse
import hashlib
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work")
EXE_REL = os.path.join("Binaries", "Win32", "BioShockInfinite.exe")
DLLS = ["PhysX3_x86.dll", "APEX_ClothingSHIPPING_x86.dll", "ApexFrameworkSHIPPING_x86.dll", "binkw32.dll", "nvtt.dll"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def has(*parts):
    return os.path.exists(os.path.join(*parts))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="BioShock Infinite install folder (Steam)")
    ap.add_argument("--ndk", default=os.environ.get("ANDROID_NDK_HOME", ""), help="Android NDK r27c folder")
    ap.add_argument("--ghidra", default=os.environ.get("GHIDRA_INSTALL_DIR", ""), help="Ghidra install folder")
    args = ap.parse_args()

    problems = []
    exe = os.path.join(args.game, EXE_REL)
    print("== Game")
    if not os.path.isfile(exe):
        sys.exit(f"Not found: {exe}\nPoint --game at the folder that contains Binaries\\Win32.")
    print(f"  exe      {exe}")
    print(f"  size     {os.path.getsize(exe)} bytes")
    print(f"  sha256   {sha256(exe)}   (report this value if a later stage fails)")
    bindir = os.path.dirname(exe)
    for d in DLLS:
        ok = os.path.isfile(os.path.join(bindir, d))
        print(f"  {'ok     ' if ok else 'MISSING'}  {d}")
        if not ok:
            problems.append(f"game DLL {d}")

    print("== Tools")
    tools = {
        "python 3.11+": sys.version_info >= (3, 11),
        "git": shutil.which("git") is not None,
        "adb": shutil.which("adb") is not None,
        "Ghidra (--ghidra)": bool(args.ghidra) and has(args.ghidra, "support"),
        "Android NDK (--ndk)": bool(args.ndk) and has(args.ndk, "toolchains", "llvm", "prebuilt"),
    }
    for name, ok in tools.items():
        print(f"  {'ok     ' if ok else 'MISSING'}  {name}")
        if not ok:
            problems.append(name)

    print("== Pipeline stages (outputs under work/, see PIPELINE.md)")
    stages = [
        ("1 Ghidra export", has(WORK, "bodies.tsv") and has(WORK, "functions.tsv") and has(WORK, "graph.tsv")),
        ("2 translation", has(WORK, "gen_promo")),
        ("3 ARM64 objects", has(WORK, "objs", "functab.o")),
        ("4 stereo hook parts", has(WORK, "objs_vr", "part007.o") and has(WORK, "objs_vr", "part012.o")),
        ("5 dependencies (DXVK, Unicorn, OpenXR)", has(WORK, "prebuilt")),
        ("6 libbsi.so", has(WORK, "out", "libbsi.so")),
        ("7 signed APK", any(f.endswith(".apk") for f in os.listdir(os.path.join(WORK, "out")))
            if has(WORK, "out") else False),
    ]
    first_missing = None
    for name, done in stages:
        print(f"  {'done   ' if done else 'to do  '}  {name}")
        if not done and first_missing is None:
            first_missing = name
    os.makedirs(WORK, exist_ok=True)

    print()
    if problems:
        print("Missing before you can start: " + ", ".join(problems))
    if first_missing:
        print(f"Next stage: {first_missing}. The steps and the known gaps are in PIPELINE.md.")
    else:
        print("All stages have outputs. Install with adb (PIPELINE.md, stage 8).")


if __name__ == "__main__":
    main()
