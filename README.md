# BSI-Quest

Tools that turn **your own Steam copy** of BioShock Infinite into a native ARM64 app for Meta Quest 3, with stereo VR.

The Windows game (32-bit x86, Unreal Engine 3) is statically recompiled: x86 machine code -> C -> ARM64. The translated game
runs on our own runtime (Win32 and Direct3D 11 layers) on top of DXVK 3.1.1 and the Turnip Vulkan driver.

**This repository contains no game content.** No executables, no translated game code, no assets, no shaders from the game,
no Ghidra exports. You need your own copy of the game; everything game-derived is produced on your PC and stays there.
There is no DRM bypass: the loader starts the game at its real entry point and Steam is reported as unavailable.

## State

- Build 143: the whole game runs natively on Quest 3 in stereo VR at full Ultra, floating HUD, 72 Hz display.
- Frame time at the heaviest spots is 30-43 ms against a 27.8 ms (36 fps) target; see `CHANGELOG.md`.
- The conversion is **not one command yet**. `setup.py` checks your inputs and tells you which stages are ready;
  `PIPELINE.md` lists every stage and the known gaps honestly.

## Layout

| Folder | What it is |
|---|---|
| `setup.py` | Entry point: checks the game folder and the tools, reports the state of each pipeline stage |
| `ghidra/` | Ghidra scripts that export function bodies, the call graph and imports from your exe |
| `translator/` | x86 -> C translator (`x86c.py`, `vec_ops.py`, `genall.py`), exception-handling scan (`eh/`), build and link scripts |
| `runtime/linux/` | Base runtime: Win32 layer, COM/D3D11 bridge, audio, input, OpenXR glue |
| `kit/` | Current build kit: the native renderer on DXVK (`src_b1n/linux/rhi_b1_*`), stereo VR (`src_vr/`), link and APK scripts. Files here override same-named files in `runtime/linux/` |
| `patches/dxvk/` | Our changes to DXVK 3.1.1 as patch files (Android WSI, app-provided libvulkan, zero-copy Map) |
| `patches/mesa/` | Our changes to Mesa/Turnip as patch files (KGSL fixes) |
| `patches/deps/` | Build scripts for Unicorn, the OpenXR loader and libadrenotools |
| `lab/` | Hands-off Quest test runner (PowerShell + adb): queued jobs, scenarios, automatic measurement |
| `tools/` | Analysis and workflow tools (job tables, dump comparison, build bookkeeping) |

## Third-party code

DXVK (zlib licence) and Mesa (MIT) are not included; only our patches against them are. SMAA (MIT, see
`kit/src_b1n/linux/SMAA_LICENSE.txt`) is included. Unicorn (GPL-2.0) is built separately and linked as a fallback emulator.

BioShock Infinite is a trademark of Take-Two Interactive. This project is not affiliated with or endorsed by them.
