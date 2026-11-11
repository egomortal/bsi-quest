# BSI-Quest

Tools that turn **your own Steam copy** of BioShock Infinite into a native ARM64 app for Meta Quest 3, with stereo VR
at full PC Ultra settings.

The Windows game (32-bit x86, Unreal Engine 3) is statically recompiled: x86 machine code -> C -> ARM64. The translated game
runs on our own runtime (Win32 and Direct3D 11 layers) on top of DXVK 3.1.1 and the Turnip Vulkan driver.

**You need your own Steam copy of the game.** The source tree here holds only our own code: no executables, no assets, no
shaders from the game, no Ghidra exports. The app on the Releases page contains the game's program code in translated form and
nothing else; all game data (levels, textures, sound, the exe itself) is copied from your Steam folder by the installer, which
refuses to run without the matching game. There is no DRM bypass: the loader starts the game at its real entry point and Steam
is reported as unavailable.

## What you need

| | |
|---|---|
| Headset | Meta Quest 3 (Quest 3S: same chip, untested) |
| Free space on the headset | about **45 GB** with the DLC, about **20 GB** without |
| Game | BioShock Infinite, **Steam version only**, installed on your PC (about 43 GB on the PC with DLC) |
| PC | Windows, with adb (SideQuest includes it) and a USB cable |
| Headset setting | Developer mode on |

**GOG and Epic copies are not supported yet.** The app is made from the Steam version's `BioShockInfinite.exe` and needs
that exact file. If you own the game on GOG or Epic, you can help us find out what it would take: run the installer with
`-Game <your game folder> -DryRun`. It will stop and print your exe's SHA-256 fingerprint; open an issue with that line
and the store you bought it from. Please send only the fingerprint, never the file.

## Install (Windows PC + Quest 3)

1. Install BioShock Infinite from Steam on your PC (DLC optional).
2. Turn on developer mode on the Quest, connect it by USB and accept the debugging prompt. Install SideQuest if you have no adb.
3. Download `BSI-Quest-Installer.zip` from the stable release ("Latest") on the Releases page and unzip it. It holds the
   app, the driver, the settings and the installer in one folder.
4. Double-click `Install.bat` in the unzipped folder. It asks three things (VR or Flat, DLC or not, install or test
   only) and then does the rest. If it cannot find your game, drag your `BioShock Infinite` folder onto `Install.bat`.

Windows may show a "Windows protected your PC" or security prompt for a downloaded `.bat` file: that is normal for any
unsigned script. Every file in the zip is a plain text script you can open and read first.

**From a clone of this repository instead:** put `bsi-quest.apk` and `vulkan.ad07xx.so` from the release into the
`install` folder and double-click `install\Install.bat`, or run the script directly:

```
powershell -ExecutionPolicy Bypass -File install\Install-BSIQuest.ps1
```

Options for the script: `-DryRun` to see what it would do, `-NoDLC` to skip the DLC, `-Mode Flat` for the big-screen
mode, `-Game <folder>` if Steam is not found.

The script checks that your `BioShockInfinite.exe` is the build the app was made from, installs the app, copies the game data
(re-running continues where it stopped), and puts the driver and the Ultra settings in `/sdcard/BSI`. It does not change your
Steam folder and does not delete or overwrite anything already on the headset.

### If the install stops

Every fix below is safe to try: re-running the installer skips what is already done and never touches your saves
(they are in `/sdcard/BSI/user` on the headset).

**`remote secure_mkdirs failed: Operation not permitted`** while copying game data (at any file, for example
`ApexFrameworkSHIPPING_x86.dll` or `vcredist_x86_vs2008sp1.exe`)

*Why:* the installer first tries the app's own folder on the headset, `/sdcard/Android/data/com.bsiquest.game`. Current
Quest system software does not let adb create folders there at all, even after the app has been opened once. Installers
downloaded before 12 Nov 2026 had no way around this. Found by the first players to install: thank you.

*Fix:* download the current `BSI-Quest-Installer.zip` from the release and run it again. When the headset refuses the
app's folder, it now copies the game to `/sdcard/BSI/BioShock Infinite` instead, which the app finds by itself. Nothing
else to do: no need to open the app first or to uninstall. Game data in that folder stays when the app is uninstalled
(delete the folder yourself with a file manager if you want the space back).

**`adb not found`**

*Why:* newer versions of SideQuest keep adb in `%APPDATA%\SideQuest\platform-tools`, where installers from before
11 Nov 2026 did not look. *Fix:* use the current installer, or tell the old one where adb is:

```
powershell -ExecutionPolicy Bypass -File install\Install-BSIQuest.ps1 -Adb "%APPDATA%\SideQuest\platform-tools\adb.exe"
```

(In a PowerShell window write `$env:APPDATA` instead of `%APPDATA%`.)

**`The argument 'install\Install-BSIQuest.ps1' to the -File parameter does not exist`**

*Why:* the command was run from another folder (a new PowerShell window starts in `C:\WINDOWS\system32`). *Fix:*
double-click `Install.bat` instead, or first go to the folder you unzipped, for example
`cd "$env:USERPROFILE\Desktop\bsi-quest-main"`, then run the command.

**`No headset found`** or **`More than one adb device`**

Unplug and replug the USB cable, put the headset on and accept "Allow USB debugging" (tick "Always allow"). Close
SideQuest's wireless connection or any emulator, so that only the headset is connected.

**Anything else:** open an issue and paste the whole text of the installer window.

## Two ways to play: VR or Flat

| Mode | What you get | Speed |
|---|---|---|
| **VR** (default) | Stereo 3D: you stand inside Columbia, your head moves the view, the HUD floats in front of you | Two views are drawn, so this is the heavy mode: 24-49 fps depending on the area |
| **Flat** | The game on a big virtual screen in front of you, like a private cinema | One view is drawn, so it runs roughly twice as fast |

Pick one when you install, and switch whenever you like: double-click `Switch-Mode.bat` (next to `Install.bat`), or run:

```
powershell -ExecutionPolicy Bypass -File install\Set-Mode.ps1 -Mode Flat
powershell -ExecutionPolicy Bypass -File install\Set-Mode.ps1 -Mode VR
```

Close the game first; the mode is read when it starts. Switching keeps your VR settings and your saves. An in-headset
menu for this is on the list below; for now it is a script on the PC.

## Controls

The game is played with the two Touch controllers. BSI-Quest presents them to the game as an Xbox gamepad, so the game's
own button prompts (A, B, X, Y, LB, RB, LT, RT) match what is printed on your controllers, with the grips standing in
for the bumpers.

| Touch controller | Gamepad button | In the game (default layout) |
|---|---|---|
| Left stick | Left stick | Move |
| Right stick | Right stick | Turn and look |
| Head | - | In VR, looking around with your head moves the view |
| Right trigger | RT | Fire weapon |
| Left trigger | LT | Use vigor (hold to charge) |
| Right grip (squeeze firmly) | RB | Switch weapon. Hold: weapon wheel (see below) |
| Left grip (squeeze firmly) | LB | Switch vigor. Hold: vigor wheel |
| A (right) | A | Jump; ride and leave Sky-Lines |
| B (right) | B | Crouch |
| X (left) | X | Reload; hold to use, pick up, open a tear |
| Y (left) | Y | Melee (Sky-Hook) |
| Right stick click | RS | Aim down sights |
| Left stick click (tap) | LS | Sprint (starts when you release the click) |
| Menu button (left) | Start | Pause menu |
| Meta button (right) | - | Headset system menu, as in every app |

Touch controllers have no D-pad and no Back button, so the left stick click doubles as a modifier:

| Hold left stick click and... | Gamepad button | In the game |
|---|---|---|
| push the right stick up | D-pad up | Navigation arrow |
| push the right stick down | D-pad down | Voxophone / tear shortcut |
| push the right stick left or right | D-pad left / right | Quick toggles |
| press Y | Back | Objectives and map |
| press Y and keep holding about a second | Back (held) | BSI-Quest mod menu |

The view is held still while the left stick click is down.

Notes:

- **Grips need a firm squeeze** (85 %), so resting your hand on the controller does not switch weapons. A file
  `/sdcard/BSI/grip.txt` containing two numbers, for example `0.7 0.5` (press at, release at), changes that.
- **Weapon wheel** (a BSI-Quest addition, on by default): hold the right grip for about a third of a second, pick with
  the stick, let go. Time is paused while it is open.
- **Mod menu:** stick up/down selects, A toggles, B closes. It switches the BSI-Quest additions (weapon wheel,
  legendary drops, economy, weapon perks, gear) on and off.
- **Aiming:** by default the gun points where you look. Aiming with the right controller exists as an experimental
  option: create an empty file `/sdcard/BSI/vr_aim.txt`.
- **The actions in the right-hand column are the game's default gamepad layout.** If you pick another layout in the
  game's options, the middle column still tells you which gamepad button each Touch control is.
- Hand tracking is not supported for playing; use the controllers.

## Help us: play reports (opt-in)

The app never sends anything by itself. If you want to help, `install\Send-Report.ps1` makes a zip of your last session's
logs on your PC, which you can attach to an issue. We ask because the project is tested on one headset at about 30 spots:
your sessions show us game code that still has to be translated, levels that run slowly, and crashes. `REPORTS.md` lists
exactly what is in a report, what is not (no saves, screenshots, serial number or account data), and what we do with it.

## Why Quest 3 only

This is the full PC game at PC Ultra settings, rendered twice (once per eye), on a phone-class chip. The Quest 3's GPU
(Adreno 740) is the least that can do it, and it is still about a third short of the target at the heaviest spots. There
is no headroom to give away: Quest 2 and Quest Pro have roughly half the GPU, so they are not supported, and we are not
planning lower-quality modes to make them fit.

The **Quest 3S** uses the same chip as the Quest 3, so it should run the same; it has not been tested yet. If you
have one, a play report would tell us.

The picture is not allowed to get worse to gain speed. That rules out the usual shortcuts of standalone VR: no foveated
rendering, no lower eye resolution, no frame generation (SpaceWarp), no TAA. Every optimisation in this project gives
the same picture as before, proven by a bit-for-bit comparison, and that is the rule for contributions too.

## Current state

- The whole game runs natively on Quest 3 in stereo VR at full Ultra, floating HUD, 72 Hz display. Saves, DLC and all
  languages work.
- Target: a steady 36 fps (27.8 ms per frame, every second refresh at 72 Hz). Measured at fixed test spots (4 Nov 2026):

  | Spot | Frame ms | fps |
  |---|---|---|
  | Downtown Emporia | 42.2 | 23.7 |
  | Fair entrance (crowd) | 40.7 | 24.6 |
  | Rooftops | 39.6 | 25.3 |
  | Gondola | 32.7 | 30.6 |
  | Cab ride | 31.5 | 31.7 |
  | Battleship Bay lift | 26.5 | 37.7 |
  | Monument Island library | 25.3 | 39.5 |
  | Garden | 23.5 | 42.6 |
  | Raffle crowd | 23.4 | 42.7 |
  | Founders Lodge | 20.3 | 49.3 |

  Five of the ten spots meet the target. The heaviest need about a third less GPU time.
- Two release channels on the Releases page:
  - **Stable** (marked "Latest"): build 142, the build that has been played on a headset. Use this one.
  - **Experimental** (marked "Pre-release"): newer builds with the latest optimisations and fixes, less tested, may be
    broken. None is published yet. The first will be build 143, which fixes an occasional slow state at the Fair (about
    12 fps until the checkpoint is reloaded); it is waiting for its headset test.
  A build moves to stable after it has been played without problems. Both use the same installer. Android does not
  install an older build over a newer one: going back from experimental to stable means uninstalling the app first,
  which also removes the game data copied to the headset, so the installer has to copy it again. Saves and settings
  live in `/sdcard/BSI` and are kept.
- Tested on one Quest 3 with the current Steam build of the game. Other game builds are rejected by the installer.
- What has been done so far is in `CHANGELOG.md`, including the ideas that were measured and rejected.

## What's next

Roughly in this order. None of it has a date; each item ships to the experimental channel when it is measured and proven.

1. **36 fps everywhere in VR.** The biggest costs left are the soft-shadow pass (11-14 ms at the heaviest spots), the
   scene colour and G-buffer passes, and a render-thread wait on occlusion-query results (about 7 ms per frame at the Fair).
2. **One pass for both eyes** (single-pass stereo). Today every draw is issued twice; this is the main render-thread lever.
3. **Get rid of the "ants".** At headset resolution, thin and shiny things crawl and shimmer as you move your head:
   railings, wires, fences, foliage, specular highlights. The game's own answer on PC is FXAA, which blurs edges but does
   not stop the crawl, and the usual VR fix (TAA) smears the picture, so we ruled it out. The plan is to remove the
   causes instead: multisampling where the hardware gives it cheaply, specular anti-aliasing for the shimmer on shiny
   surfaces, and special handling for thin geometry. This starts once the frame rate is there, because all of it costs
   GPU time. This is the one area where the picture is meant to change, so it will be judged on side-by-side captures
   and stay behind a switch.
4. **Translate the rest of the game code.** Play reports (see above) tell us which code players reach that the
   translator has not seen, so nobody hits the slow fallback emulator.
5. **A smaller install.** Build 144 can record which parts of the 43 GB of game data are ever read (`REPORTS.md`).
   Once maps cover the whole game, the installer can leave unused data off the headset.
6. **Choose VR or Flat inside the headset**, instead of with a script on the PC.
7. **Build from your own copy in one command.** `PIPELINE.md` lists the stages and the gaps; `setup.py` reports which
   stages are ready on your PC.
8. **Gameplay mods** (gear, weapon wheel, new enemy variants), after the VR goal is met.

## Contributing

Fixes and improvements are welcome **in this repository**. Please do not publish your own fork or your own builds as a
separate version: players then have to work out which one to install, fixes stop reaching each other, and a build made
from a different tree cannot be compared with our measurements. Send the change here and it ships to everyone in the
next experimental release, with your name on the commit.

- Small fix: open a pull request against `main`. Regular contributors get push access.
- Bigger idea: open an issue first so nobody builds something that was already measured and rejected (`CHANGELOG.md`).
- Using an AI assistant: `AGENTS.md` has the exact text to give it.

The rules (same picture, a switch for every change, measured before and after) and the practical steps are in
`CONTRIBUTING.md`.

## Layout

| Folder | What it is |
|---|---|
| `install/` | Installer, VR/Flat mode switch, play-report script, and the default settings files for the headset |
| `setup.py` | For building from source: checks the game folder and the tools, reports the state of each pipeline stage |
| `ghidra/` | Ghidra scripts that export function bodies, the call graph and imports from your exe |
| `translator/` | x86 -> C translator (`x86c.py`, `vec_ops.py`, `genall.py`), exception-handling scan (`eh/`), build and link scripts |
| `runtime/linux/` | Base runtime: Win32 layer, COM/D3D11 bridge, audio, input, OpenXR glue |
| `kit/` | Current build kit: the native renderer on DXVK (`src_b1n/linux/rhi_b1_*`), stereo VR (`src_vr/`), link and APK scripts. Files here override same-named files in `runtime/linux/` |
| `patches/dxvk/` | Our changes to DXVK 3.1.1 as patch files (Android WSI, app-provided libvulkan, zero-copy Map) |
| `patches/mesa/` | Our changes to Mesa/Turnip as patch files (KGSL fixes) |
| `patches/deps/` | Build scripts for Unicorn, the OpenXR loader and libadrenotools |
| `lab/` | Hands-off Quest test runner (PowerShell + adb): queued jobs, scenarios, automatic measurement |
| `tools/` | Analysis and workflow tools (job tables, dump comparison, build bookkeeping) |

## AI assistance

Most of this project's code was written with AI coding assistants, directed, measured and tested by a human on real
hardware. Every performance claim here comes from a measurement on the headset, not from a model. If you would rather
not use AI-assisted software, that is completely fine; this project is probably not for you.

## Licence

BSI-Quest's own code is free software under the **GNU GPL version 3** with one additional permission that allows it to
be combined with the translated code of the game (`LICENSE`, `LICENSE-EXCEPTION.md`). In short: you may use, change and
share it, and if you share a changed version you must share your source under the same terms. The licence covers our
code only; it gives no rights to the game.

## Shout-outs

BSI-Quest stands on a lot of other people's work. Thank you to the authors of everything below.

**Runs inside the app**

| Project | What we use it for | Licence |
|---|---|---|
| [DXVK](https://github.com/doitsujin/dxvk) 3.1.1 and dxbc-spirv, by Philip Rebohle and contributors | Direct3D 11 on Vulkan: the core our native renderer drives, and the shader translation | zlib |
| [Mesa](https://www.mesa3d.org/) Turnip, by the freedreno developers | The open-source Vulkan driver for the Quest's Adreno GPU; without it none of this would run | MIT |
| [Unicorn Engine](https://www.unicorn-engine.org/) 2.1.1 (built on [QEMU](https://www.qemu.org/)) | Fallback emulator for the rare game code the translator has not seen | GPL-2.0 |
| [OpenXR](https://www.khronos.org/openxr/) SDK and loader, by Khronos | VR session, head and controller tracking, compositor layers | Apache-2.0 |
| [libadrenotools](https://github.com/bylaws/libadrenotools), by Billy Laws | Loading our own Turnip build in place of the system driver | BSD-2-Clause |
| [SMAA](https://github.com/iryoku/smaa), by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez | Optional anti-aliasing path | MIT |

**Used to build and study it**

| Project | What we use it for |
|---|---|
| [Ghidra](https://ghidra-sre.org/), by the NSA | Analysing the game's executable: functions, call graph, imports |
| [Capstone](https://www.capstone-engine.org/) | Disassembler inside the translator |
| [pefile](https://github.com/erocarrera/pefile), by Ero Carrera | Reading the Windows executable and its DLLs |
| Android NDK (LLVM/clang) | Compiling everything for ARM64 |
| [glslang](https://github.com/KhronosGroup/glslang), Meson and Ninja | Compiling our own shaders; building DXVK |
| [pyaxml](https://github.com/androguard/pyaxml) and [cryptography](https://cryptography.io/) | Building the Android manifest and signing the APK |
| [SideQuest](https://sidequestvr.com/) and adb | Installing and talking to the headset |
| Meta's ovrgpuprofiler and Android's simpleperf | GPU and CPU measurements on the headset |

**And**

- The Turnip builders in the Android emulation community, whose driver builds we tested before making our own.
- The static recompilation projects that showed this approach works for whole games (N64: Recompiled, Xbox 360 recompilation).
- Irrational Games, for the game.

The licence column lists each project's own licence; our patches to DXVK and Mesa are under those same licences
(`patches/`). If we use your work and missed you here, open an issue and we will fix it.

BSI-Quest is an unofficial fan project. BioShock Infinite is a trademark of Take-Two Interactive; this project is not
affiliated with or endorsed by Take-Two Interactive or Irrational Games.
