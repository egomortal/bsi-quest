# Pipeline: building the app yourself from your game folder

Most people should use the release and `install\Install-BSIQuest.ps1` (see `README.md`). This page is for rebuilding the app
from your own exe.

Run `python setup.py --game "<Steam>\steamapps\common\BioShock Infinite"` first. It checks the inputs and prints which of
the stages below can run on your PC. Everything the stages produce goes into `work/` (git-ignored) and must never be
committed or shared: it is derived from the game.

## What you need

- Your own Steam copy of BioShock Infinite (base game; DLC optional), about 40 GB.
- Ghidra 12.x, Python 3.11+, Android NDK r27c, a Linux box or WSL for the translation and object builds (2+ cores, 20 GB).
- DXVK v3.1.1 sources (`git clone --branch v3.1.1 --recurse-submodules https://github.com/doitsujin/dxvk`) + `patches/dxvk/`.
- A Turnip driver for Adreno 740 (Mesa + `patches/mesa/`, or a community build).
- A Quest 3 with developer mode and adb.

## Stages

| # | Stage | Tool | Output (in `work/`) | State |
|---|---|---|---|---|
| 1 | Analyse the exe and its DLLs | Ghidra + `ghidra/*.java` | `bodies.tsv`, `functions.tsv`, `graph.tsv`, imports | Works, manual: import the exe, auto-analyse, run the scripts in the order CreateEntry, CreateFuncs, RemoveBogus, Export* |
| 2 | Translate x86 -> C | `translator/genall.py`, `genall_dll.py`, `x86c.py` | `gen_promo/`, `gen_dll_promo/` | Works. **Gap:** the function list was grown over several rounds (functions found at run time by the emulator fallback were added by hand); those additions are not in this repo because they are lists of game addresses with code bytes |
| 3 | Compile for ARM64 | `translator/build_android_promo.sh` | `objs/part*.o`, `functab.o` | Works, 1-2 hours on 2 cores |
| 4 | Stereo hooks | `kit/src_vr/` + two re-translated parts | `objs_vr/part007.o`, `part012.o` | `kit/vr_hook_parts.sh` rewrites the call sites in the two generated parts and compiles them. Paths inside it still point at the original build machine and need adapting |
| 5 | Build dependencies | `patches/deps/*.sh`, DXVK + `patches/dxvk/` | static DXVK libs, Unicorn, OpenXR loader, libadrenotools | Works |
| 6 | Runtime + link | `kit/build_b1n_vr_link.py --ndk <path>` | `libbsi.so` | Works when stages 3-5 are present |
| 7 | Package | `kit/build_b0_apk.py`, `kit/fix_axml.py` | signed APK | Works. **You must create your own signing key**; ours is not included. The binary manifest is compiled from `kit/AndroidManifest_override.xml` with pyaxml |
| 8 | Install and copy game data | adb, `tools/bsi/sync_game_files.ps1` | app + `/sdcard/Android/data/com.bsiquest.game/files/game` | Works |
| 9 | Settings on the headset | files in `/sdcard/BSI/` | env.txt, ini overrides, resolution | Works; defaults are listed in `lab/LAB.md` |

## Known gaps before this is one command

1. Stage 2 function list: make discovery automatic (run the emulator-fallback log back into the translator until it is empty).
2. Stage 4 hook script: adapt its paths and check its output against build 143's objects.
3. Stage 7: add the manifest compile step and key generation to the packaging script.
4. Exe versions: only the current Steam build has been used. `setup.py` prints the SHA-256 of your exe so mismatches can be
   reported; address-based hooks in `runtime/` and `kit/` assume that build.
5. Nobody has yet rebuilt build 143 end to end on a clean PC. Until that is done, treat this as source code plus a map.
