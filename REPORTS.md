# Play reports (opt-in)

BSI-Quest never sends anything anywhere by itself. Our code contains no upload or telemetry code, and the game's own
network functions are not implemented (Steam is reported as unavailable). The app does declare Android's internet
permission, for one thing: a small debug console that listens only on the headset itself (`127.0.0.1:27183`) and can
be reached only through the USB debugging cable (`adb forward`). You can check this in `kit/src_b1n/linux/android_glue.c`.

If you want to help, you can make a report of your last play session and share it. This page says exactly why we ask,
what is in a report, and what happens to it.

## Why we ask

The project is developed and tested on one headset by one person, at about 30 fixed spots in the game. Players cover the
rest. A report gives us three things we cannot get on our own:

1. **Game code we have not translated yet.** The game's program code was translated ahead of time. When play reaches code
   the translator never saw, the app falls back to a slow built-in emulator and writes the address to its log. Every
   address a report contains is translated in the next build. When reports stop containing any, the emulator can be removed.
2. **Where it is slow.** Frame rate per level on your headset, so optimisation goes where players actually have problems.
3. **Where it crashes.**

## What is in a report

| File | Contents |
|---|---|
| `bsi.log` | The app's log of your **last** session: renderer setup, driver, levels loaded, frame-time lines, emulator fallbacks, errors |
| `bench.txt`, `session.txt` | Frame-rate summary and which graphics driver was used |
| `env.txt`, `vr_stereo.txt`, `mods.txt`, `ini_overrides.txt`, `resolution.txt` | Your BSI-Quest settings files |
| `device.txt` | Headset model, OS build, Android version, BSI-Quest build number |
| `summary.txt` | A short digest of the above; the script prints it on screen before it saves anything |
| `note.txt` | Only if you add a note describing what happened |

## What is not in a report

No saves, no screenshots or video, no game files, no headset serial number, no account names, no network information.
The logs are technical output of the app. The one per-install value they contain (Android's random name for the app's
install folder) is replaced by the script. You can open the zip and read every file before you share it.

## How to make one

Connect the headset by USB after a session and run:

```
powershell -ExecutionPolicy Bypass -File install\Send-Report.ps1 -Note "what happened, where"
```

The script explains itself, asks for a yes, reads `/sdcard/BSI` on the headset (it changes nothing there), and writes
one zip into `install\reports\`. **It uploads nothing.**

## How to share it

Open a new issue on this repository using the "Play report" form and attach the zip. Issues are visible to everyone who
can see the repository, so check the zip first. If you would rather not post it publicly, do not attach it; the text of
`summary.txt` alone is already useful.

## What we do with it

- Addresses of untranslated code go into the translator's function list for the next build.
- Frame-rate and crash information goes into the list of spots to measure and fix.
- Reports are not passed on to anyone else and are not used for anything other than fixing BSI-Quest.
