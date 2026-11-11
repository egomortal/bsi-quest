# Changelog

What the project can do today (build 143), grouped by area. This is a summary of the current state, not a build-by-build history.

## Game

- BioShock Infinite (Steam, 32-bit x86, Unreal Engine 3) statically recompiled to ARM64 together with its DLLs
  (PhysX 3, APEX, Bink, nvtt). A Unicorn emulator remains as a fallback for code the translator has not seen.
- Win32 layer with about 300 APIs (files, threads, synchronisation, heap, time); Steam reported as unavailable.
- Audio: XAudio2 2.7 layer on AAudio; Wwise runs translated.
- Saves, DLC and all languages work. Local profile instead of Steam cloud.

## Rendering

- Native renderer: the game's D3D11 calls go straight into the DXVK 3.1.1 core (no 32-bit COM round trip), on Turnip.
- Full Ultra preset: FXAA, light shafts, contact-hardening shadows, ambient occlusion.
- Zero-copy buffer maps through `VK_EXT_map_memory_placed`.
- Optional SMAA / TAA paths exist; the default picture is the game's own.

## VR

- Engine-level two-view stereo on Quest 3 through OpenXR, head tracking, controller aim, 72 Hz.
- Floating HUD kept as in the flat game.
- Hand-tracking declared in the manifest so the system controller dialog does not block launch.

## Performance (all changes give the same picture; each has a switch)

- Ambient occlusion computed once for the shared part of the two views.
- Eye-column scissors, shadow-quad bounds and depth bounds, removal of clears that nothing reads.
- Pipeline libraries off; one submit per 1024 draws.
- Sun shadow skip where the surface faces away from the sun, proven bit-exact by an in-shader check.
- Shadow filter reuses the blocker-search samples where that is exact.
- Render-thread CPU trims: inline lock, direct COM calls, cached lookups, thread pinning that follows a restarted render thread.
- Frame time at the heaviest spots: Fair about 38-43 ms, Rooftops about 39 ms, Gondola about 31 ms, Cab ride about 30 ms.
  Target is 27.8 ms (36 fps).

## Measured and rejected (no gain or not exact)

Tile-based (GMEM) rendering, low-resolution Z, fp16 materials, shadow-map caching between frames, tiled shadow projection,
adaptive or single submit per frame, more frames in flight, shadow-mask pass merging, bloom or post-chain removal for speed.

## Install

- Two display modes, chosen at install and switchable later: VR (stereo) and Flat (big virtual screen, one view).
- One-script install from a Windows PC: checks the game build, installs the app, copies game data, driver and settings.
  One-zip download with double-click starters (`Install.bat`, `Switch-Mode.bat`, `Send-Report.bat`).
- Fixed after the first real install (11 Nov 2026): on a fresh install the copy failed with `secure_mkdirs failed:
  Operation not permitted`, because only the headset may create the app's data folder and it does so on the app's first
  start. The installer now opens the app once first. It also finds adb where newer SideQuest keeps it
  (`%APPDATA%\SideQuest\platform-tools`). README "If the install stops" has the manual fix for older downloads.

## Tooling

- Hands-off test lab: a PC runner installs builds over adb, loads any of about 30 checkpoints, measures per-pass GPU time and
  per-thread CPU time, and never touches the player's saves.
- Read map (build 144, off by default): records which blocks of which game files are read, to find data that is never used.
- In-game profiler (per-pass, per-shader GPU time; asset census), dump comparison for exactness checks.
- Own Turnip build with KGSL fixes.

## Not done yet

- A permanent 36 fps at full Ultra in every area.
- One-command rebuild of the app from a game folder (see `PIPELINE.md`).
- Anti-aliasing beyond the game's FXAA; gameplay mods.
