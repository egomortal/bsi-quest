# BSI-Quest settings: what we use and why

These are the settings the test headset plays with. They are the most optimized combination measured so far: full PC
Ultra, the fastest exact code paths, and the VR fixes. `Install.bat` puts them on the headset; after every new installer,
run `Install.bat` and choose **U = most optimized settings** to bring yours up to date. Files that change are replaced and
your old copy is kept next to it as `<name>.bak`. Two files are yours and are never replaced: `mods.txt` (the mod menu's
choices) and `resolution.txt` (the Flat screen size). Your VR/Flat mode is kept.

All files live in `/sdcard/BSI` on the headset. The game reads them when it starts, so close it before changing them.

## The settings

| File | What we use | What it does |
|---|---|---|
| `ini_overrides.txt` | Full Ultra | Forces the game's own top graphics settings: quality level 5, dynamic shadows 5, ambient occlusion 4, shadow resolution 2048, FXAA on, light shafts on. |
| `ao_sel.txt` + `ao/gtao_match/` | `3` = GTAO match | Replaces the game's ambient occlusion (HDAO) with GTAO tuned to look like it and cheaper to run (the reference picture since 2 Nov). `0` = the game's own HDAO. Source in `tools/gtao/`. |
| `env.txt` | `BSI_RHI=b1n`, `BSI_HLEFAST=1`, `BSI_DLC=1` | `b1n` = our native renderer (drives the Vulkan layer directly instead of going through Direct3D 11). `HLEFAST` = fast paths for the hottest Windows functions the game calls (on by default anyway). `DLC` = load Burial at Sea and Clash in the Clouds when they are installed. |
| `vr_stereo.txt` | `res=auto eye_mpix=1.04 pad=3`, `ao_eye=2 ls_eye=1 shadow_eye=1 fxaa_eye=1 bloom_eye=1`, `aa=off` | VR mode (it exists only in VR mode; Flat mode renames it to `vr_stereo.txt.off`). About 1.04 megapixels per eye shaped like the headset's recommended image, with 3 degrees of extra view for head movement. The `*_eye` switches make ambient occlusion, light shafts, the sun's shadows, FXAA and bloom correct for each eye (without them the right eye reuses the left eye's result). `aa=off`: no extra anti-aliasing beyond the game's FXAA. |
| `vr_head.txt` | `roll=1` | Tilting your head tilts the game camera too. |
| `dxvk.conf` | see the file | DXVK tuning for the Quest: 3 shader compiler threads, compiled shaders are kept (no recompiles = fewer hitches), the game is told it has 2 GB of video memory so its texture pool fits, frame pacing. Each line has a comment. |
| `skip_intro.txt` | present | Skips the logo movies at start (as on the test headset). Delete it to watch them. |
| `nohud.txt` | present | Keeps DXVK's own frame-rate overlay off. It does not hide the game's HUD. |
| `mods.txt` | perks on, gear on | The mod menu's choices (hold left stick click + Y about a second in game). The weapon wheel is on by default; the economy mod is off. Yours: never replaced. |
| `resolution.txt` | `1600x900` | Flat mode only: the size of the big virtual screen. Yours: never replaced. |
| `turnip/vulkan.ad07xx.so` | the release driver | Our build of the open-source Turnip Vulkan driver for the Quest's GPU. Always updated. |

Built into the app (no file needed): shader pre-warming (the app remembers every shader it has seen and prepares them
at the next start, so your second start is smoother than the first), faster audio and file access, and short CPU boosts
while loading.

## Optional files you can add yourself

| File | Effect |
|---|---|
| `vr_aim.txt` (empty) | Aim with the right controller instead of your head (experimental). |
| `grip.txt` | Two numbers, for example `0.7 0.5`: how hard to squeeze the grips (press at, release at). Default `0.85 0.60`. |
| `motionblur.txt` containing `1` | Motion blur on (off by default). |
| `bloom.txt` containing `0` | Bloom off (on by default). |

Not on the list on purpose: lower resolution, foveated rendering, frame generation and TAA. The project's rule is that
the picture is never made worse to gain speed.
