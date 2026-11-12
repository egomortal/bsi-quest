# BSI-Quest questions and answers

What players have run into so far, and the most likely cause of each. "Most likely" means it fits everything we have
seen but is not proven yet. Install errors and their fixes are in the README under "If the install stops"; this page
covers the reasons behind them and everything after the install.

If something here does not match what you see, run `Send-Report.bat` after playing and open an issue with the zip or
its summary.

## Installing

**The copy stopped with `remote secure_mkdirs failed: Operation not permitted`.**
Cause (confirmed): current Quest system software does not let adb create folders inside an app's own folder
(`/sdcard/Android/data/<app>`), not even after the app has been opened. Fixed in the installer from 11 Nov: it creates
the folders itself first, then copies into them.

**The install said `Done`, but the game shows only black.**
Three causes found so far, all handled by the current installer:
- The copy was stopped halfway (the DLC is 24 GB in about 7,000 files and can look frozen), and older installers copied
  the driver and settings last. Cause confirmed; the installer now copies them first.
- The app had no storage access, so it could not read `/sdcard/BSI`. Cause confirmed on a fresh install; the installer
  now grants it.
- The app was not allowed to read the game in its own folder (the log says `app-folder read test ... FAILED Permission
  denied` and `game folder missing`). Most likely cause: a folder left behind by an earlier install still belongs to the
  old copy of the app. Fix: `Uninstall.bat`, then `Install.bat`. The installer also detects this and moves the game to
  `/sdcard/BSI/BioShock Infinite`, where the app can read it.

**The copy seems stuck at `-> DLC`.**
Most likely it is still running: adb shows one percentage for the whole folder. If nothing moves for 15-20 minutes,
replug the cable and run the installer again; it continues where it stopped.

**Do I have to re-copy everything after an update?**
No. Run `Install.bat` and choose **U = most optimized settings**. It updates the settings in seconds and keeps your
choices. `install/SETTINGS.md` lists every setting.

## Playing

**The first minutes stutter.**
Cause (expected): the game's shaders are compiled for the Quest's GPU the first time each one is used. The app
remembers them and prepares them at the next start, so later sessions are smoother. We cannot ship a ready-made cache:
it would contain the game's own shader code.

**Turning, even slowly, looks choppy or shows double images.**
Most likely cause: the frame rate. The headset shows 72 pictures a second, and the game currently makes about 24-49 a
second depending on the area, so each game picture is shown two or three times. When you turn, the headset shifts the
old picture to follow your head, and the repeats show up as doubled edges. The project's goal is a steady 36 fps (every
picture shown exactly twice); five of the ten test spots are there. Heat makes it worse: during the headset's thermal
warnings (about a minute at a time) the GPU drops from 640 to 492 MHz. Playing with the headset plugged in helps keep
the GPU at its full 640 MHz.

**There are no motion controls, and moving my head or body in the room does not move me.**
Not a fault: they are not in yet. The Touch controllers act as an Xbox gamepad (see Controls in the README). Aiming
with the right controller exists as an experimental option (an empty file `/sdcard/BSI/vr_aim.txt`). Full room-scale
movement and hand-driven weapons are not built yet.

**Shadows and lights are sometimes only in the left eye.**
A bug, being looked at. Most likely cause: the game draws some lighting work once for the view it thinks of as the
main one, and the VR renderer has to redo it for the right eye. That is already done for the sun's shadows, light
shafts, ambient occlusion, FXAA and bloom (the `*_eye` switches in `vr_stereo.txt`); some other lights and shadows are
apparently not covered yet. Telling us where you saw it (level, what kind of light) helps a lot.

**I can't see the HUD or button prompts.**
A bug, being looked at. The HUD is meant to float in front of you. Cause not known yet. A report from a session where
it happened (`Send-Report.bat`) helps.

**Can the frame rate be raised by forcing the CPU/GPU to a higher level?**
No. The app already asks the headset for its highest level an app can hold ("sustained high"), and the GPU then runs
at its maximum, 640 MHz. Forcing it higher was measured and gave nothing.

**Can I play with lower settings to make it smoother?**
Not as an option from us. The project keeps the full Ultra picture and gains speed only by doing the same work faster
(no lower resolution, foveated rendering, frame generation or TAA). Flat mode (`Switch-Mode.bat`) draws one view instead
of two and runs roughly twice as fast, if you want smooth play now.

**Why only Quest 3?**
It is the full PC game at Ultra, drawn twice, on a phone-class chip. The Quest 3 (and 3S, same chip, untested) is the
least that can do it. Quest 2 and Pro have about half the GPU.
