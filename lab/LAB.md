# BSI-Quest lab (hands-off Quest testing)

Claude queues test jobs; the PC runs them on the Quest with nobody wearing it; Claude reads the results.
Started 19 Oct 2026 (chat 2). Runner version 10 (v10: save_spot writes a new copy with the newest name; v9: recovery of interrupted jobs, console QuickEdit off; v8: gpu_counters; v7: probe, save_spot/restore_saves, simpleperf, keep_awake; v2: wider adb search; v3: Wi-Fi adb; v4: progress lines, config re-read per job; v5: screenshots in monitor, guardian step, self-restart when lab_runner.ps1 changes; v6: Wi-Fi reconnect + retry, cooldown waits on missing readings, device check before launch). First full unattended run: j0008, 19 Oct, game side = build 78 autopilot.

## The user's part (once)
- Quest reachable by adb: USB, or Wi-Fi as SideQuest uses it (`"connect": "192.168.x.x:5555"` in `lab_config.json`; the runner runs `adb connect` when no device is visible and pins every command to one serial with `-s`, preferring USB when both show). Charger connected for long runs. USB debugging "always allow" for this PC, Guardian/boundary off in the developer settings (the headset sits on the desk).
- Double-click `lab\Start-Lab.bat` and minimise the window (or `Install-Autostart.bat` once: starts at login, `Remove-Autostart.bat` undoes it).
- SideQuest may stay open only if the runner uses SideQuest's own adb.exe (same file = same adb server). Otherwise close SideQuest / MQDH while a job runs (a second adb server breaks adb: the b69 "protocol fault"). The runner waits instead of starting a job while another adb is running (`kill_foreign_adb` in `lab_config.json` makes it stop them).
- Stop: `Stop-Lab.bat` (aborts the current job, runs its cleanup, exits). Pause: create an empty file `lab\PAUSE` (no new jobs until it is deleted).
- Standing OK (19 Oct 2026): lab jobs may build and install test APKs. Every install first backs up `/sdcard/BSI/user` (saves) into the job's outbox and is logged in `lab\install_history.txt`. `allow_install` / `allow_build` in `lab_config.json` switch this off.

## Folders
| Path | What |
|---|---|
| `lab\inbox\` | job files (`*.json`) Claude drops here; run in name order |
| `lab\running\`, `lab\done\` | the job file while it runs / afterwards |
| `lab\outbox\<job id>\` | results: `status.json` (every step: ok, ms, message), `runner.log`, pulled logs, `monitor.csv`, build logs, save backups |
| `lab\runner_status.json` | heartbeat (every 30 s idle, every step while running): state, job, step, adb path, device, game versionCode, note |
| `lab\runner.log` | all jobs |
| `lab\scenarios\` | autopilot scripts and A/B config variants pushed by jobs |
| `lab\install_history.txt` | every install: time, job, versionCode before -> after, APK |

## Job format
```json
{ "id": "j0003_name", "title": "...", "timeout_min": 60,
  "steps":  [ {"do": "devices"}, ... ],
  "always": [ ... cleanup steps, run even when a step failed or STOP was pressed ... ] }
```
A failing step ends the job unless it has `"optional": true`. If the proximity sensor was overridden and `always` does not restore it, the runner adds `{"do":"proximity","mode":"on"}` itself.

| Step | Fields | Does |
|---|---|---|
| `devices` | `timeout_sec` (120) | waits for the Quest in adb state `device` (says "unauthorized" in the heartbeat if the USB prompt is pending) |
| `info` | `dst` | battery %, battery temp, thermal status, GPU clock, game versionCode, /sdcard free -> json |
| `build` | `version` (>= 77), `flavor` vr/flat, `verify` (MD5SUMS file), `rebuild` | the kit's two python commands (link + APK) exactly as in the READMEs; reuses an existing `artifacts\b1n_0NNvr\bsi_b0probe.apk` unless `rebuild`; new output folders get `_r2`, `_r3` |
| `install` | `apk` (`@last_build` or a path under `artifacts\`), `expect_version`, `backup_saves` (true) | save backup, force-stop, `adb install -r`, versionCode check, history line |
| `push` | `src` (PC path under BSI-Quest), `dst` (/sdcard/BSI/...) | adb push |
| `push_text` | `text` (string or list of lines), `dst` | writes LF, no BOM, pushes (copy kept in the outbox) |
| `rm` | `path` (one or a list; no wildcards) | `rm -f` inside /sdcard/BSI |
| `pull`, `pull_dir` | `src` (/sdcard/BSI/...), `dst` (name in the outbox) | adb pull |
| `backup_saves` | | pulls /sdcard/BSI/user into the outbox |
| `stop_game`, `launch` | | `am force-stop` / `am start -n com.bsiquest.game/android.app.NativeActivity` (+ waits for the process) |
| `wait` | `sec`, `require_game` | sleep |
| `wait_log` | `text` or `regex`, `fail_regex` [list], `timeout_sec`, `poll_sec`, `require_game` (true) | pulls bsi.log every poll until the pattern appears |
| `wait_exit` | `timeout_sec`, `force_stop_on_timeout` (true) | until the game process is gone |
| `monitor` | `sec`, `every_sec`, `until_exit`, `require_game`, `max_battery_temp_c` (50), `screencap_every_sec`, `screencap_max`, `dst` | vitals to `monitor.csv`; ends at exit (until_exit) or after sec; aborts on overheating |
| `cooldown` | `max_c` (36), `timeout_sec` | waits until the battery temperature is at or below max_c (no reading = keeps waiting) |
| `guardian` | `mode` pause/resume | `setprop debug.oculus.guardian_pause 1/0` (resumed automatically at job end) |
| `proximity` | `mode` off/on | off = `prox_close` broadcast + wake (screen and app stay on with nobody inside); on = `automation_disable` (normal sensor) |
| `logcat` | `dst`, `lines`, or `clear` | `logcat -d -t N` |
| `screencap` | `dst` | Quest screenshot (both eyes + system layers), saved as a 1032-px JPEG |
| `note` | `text` | a line in the log |
| `probe` | `dst` | fixed read-only checks: Android/toybox versions, simpleperf, ovrgpuprofiler (`-m` metric list), CPU/GPU clocks, save listing, toybox `touch -d @epoch` test |
| `save_spot` | `spot` (checkpoint name, e.g. `LizT`, `TC_FairEntrance`) | makes that checkpoint the save Continue loads: newest `.sav` of it in `user/cloud/`, else a copy of the newest one in `user/cloud/.deleted/` (the game's trash, kept by our Steam shim); its mtime is set 60 s past every other `.sav`. Backs up saves first; writes `saves_before.txt` + `save_spot_state.json` |
| `restore_saves` | | undoes save_spot and whatever the game did during the run: lab copies removed, new game saves moved to `/sdcard/BSI/lab_saves_trash/`, files the game moved into `.deleted/` moved back, every mtime set back; then compares names, sizes and times (`saves_after.txt`, `restore_saves.txt`) and fails if anything differs. Added to `always` automatically when a job used save_spot |
| `simpleperf` | `sec` (30), `freq` (1000), `callgraph` none/fp, `dst` (perf), `pull_data` | `simpleperf record --app com.bsiquest.game -e cpu-clock` on the running game (the APK is debuggable + profileable, libbsi.so keeps its 121k `f_XXXXXXXX` symbols), then on-device reports by thread / object / function -> `perf_*.txt` (+ `perf.data`) |
| `gpu_counters` | `sec` (60), `metrics` (id list), `dst` | `timeout <sec> ovrgpuprofiler --realtime=<ids>`: one line per second (GPU %, clock, read/write bandwidth, texture stalls, full/half ALU, LRZ kills ...; ids in probe.txt) |

Safety rules in the runner: no free-form commands; device paths only `/sdcard/BSI/...` (letters, digits, `._-`); PC sources only inside BSI-Quest; installs only from `build\bsi_b0_v091\artifacts\`; one runner at a time (`runner.lock`); job timeout (default 90 min) and STOP checked in every wait loop.

## Game side: autopilot (build 78+)
See `build\bsi_b0_v091\README_B78.md` for the full command list. `autopilot.txt` is one-shot (renamed to `autopilot_last.txt` when read). Logs: `[autopilot] ...` lines in bsi.log (steps, MARK, map changes, status every 10 s, TIMEOUT, quit), `[xr] autopilot: exit session requested`.

Gotchas learned from the code (agent report, 19 Oct):
- `vr_stereo.txt` keys are never reset when they disappear: A/B variants must spell out every value (the `lab_vr_stereo_*.txt` files do).
- Without the proximity override the session drops out of FOCUSED (pad zeroed) and may stop rendering (SYNCHRONIZED = no frames).
- Keep presses of Back / RB short: holding Back 0.6 s opens the mod menu, holding RB opens the weapon wheel (pauses the game).
- Continue loads the newest save (file time in /sdcard/BSI/user/cloud). Scenario saves = later: a job that touches the wanted save first (and restores the times afterwards).
- Startup-only files (env.txt, resolution.txt, ini_overrides.txt, args.txt, ...) need a relaunch; live ones (vr_stereo.txt, vr_head.txt, ao_sel.txt, motionblur.txt, bloom.txt, mod files) can be switched by autopilot `copy`.

## Quest facts learned (19 Oct)
- Menu: startup movies (skip with `skip_intro.txt`) -> PRESS ANY KEY (A) -> Login Failed (**A = EXIT GAME**, B = proceed) -> Sign In (A) -> main menu (A = Main Game) -> sign (A = Continue).
- Tracking: the headset must see a lit room, or the system "Finding position in room" dialog stops the game's XR session.
- Wi-Fi adb drops when the headset sleeps: keep it awake (prox override) during cooldown, or use USB.
- Temperatures: start runs at <= 44 C battery on Wi-Fi (charging keeps it ~40-43), abort > 50 C.

## Scenario library (start)
| Scenario | File | What |
|---|---|---|
| **title -> Continue A/B v4 (working)** | `scenarios\title_continue_ab_v4.txt` (+ `skip_intro.txt`), job template `done\j0008_b78_autopilot_v4b.json` | menu flow above, Continue (newest save), 45 s settle, 60 s static, 60 s head sweep, shadow_eye A/B/A/B 60 s each, quit (~8 min) |
| v1-v3 | history | v1/v2: presses too early; v3: missed the Sign In dialog |

Planned (lab plan): fixed saves per spot (boat, Welcome Center, Fair, Monument Island), flat + VR variants, profiled runs (`env_profiled.txt`), frame dumps at markers, white room scene.

## Job options (v7)
- `"keep_awake": true`: the proximity override stays on after the job (Wi-Fi adb drops when the headset sleeps, so a series must keep it awake). The runner releases it after `keep_awake_idle_min` (lab_config.json, default 20) minutes without a job, also across a runner restart (`lab\awake_held.txt`). The last job of a series has `keep_awake` false and `proximity on` in `always`.

## Gotchas learned in chat 3 (19 Oct)
- **Never delete `/sdcard/BSI/bsi.log` before a launch.** The game uses `/sdcard/BSI` as its data folder only while `bsi.log` (or the exe) exists there (`android_glue.c` line 656); without it the game used its app folder, saw no env.txt, ran the old D3D11 bridge and exited within 1 s (j0013-j0016). Instead push a one-line placeholder `bsi.log` before `launch`, so `wait_log` cannot match the previous session's lines (j0011 matched j0008's "quit" line).
- Re-committing a file from the same cloud staging path can deliver the OLD content (runner v8 arrived as v7 the first time). Use a new staging path for every version and check the md5 on the PC.
- `BSI_HITCHLOG` (hitch.csv) writes nothing in the native renderer (it hooks the old bridge path only); hitches come from `[bench]` (worst frame, >100 ms count per 10 s).
- Deleting files inside BSI-Quest from Claude's side is off (Operation not permitted): Claude moves files into `hold\...\moved\` instead.
- `env.txt` on the Quest (19 Oct): `BSI_HLEFAST=1`, `BSI_DLC=1`, `DXVK_LOG_PATH=/sdcard/BSI`, `BSI_RHI=b1n`. Chat 3 jobs add `BSI_HITCHLOG=/sdcard/BSI/hitch.csv` for the run and push the original back in `always`.
- Quest 3 facts (probe): Android 14, toybox 0.8.9 (`touch -d @epoch` works), simpleperf at /system/bin (hardware counters too), ovrgpuprofiler at /system_ext/bin with 81 realtime metrics and a draw-call mode (`-x`, needs `-e` detailed mode before the app starts), 6 CPU cores in two clusters (policy0 = cpus 0-1, policy2), 8.9 GB free.
- Saves: **Continue loads the `.sav` whose NAME has the newest date / highest sequence number** (`SaveData%5C<checkpoint>-0_<M>_<D>_<YYYY>_<h>_<m>_<s>_<seq>.sav`), NOT the newest mtime (chat 2 assumed mtime; j0012-j0022 all loaded `TC_Gondola-..._52.sav`). Runner v10 `save_spot` therefore copies the chosen save to a new name with today's date and sequence max+1 (verified 19 Oct, `a_m3_continue_probe_v10`: Continue loaded `TC_FairEntrance-0_10_19_2026_17_53_30_53.sav`); restore_saves removes the copy. Check every run with the last `[profile] loaded ... .sav` line before `MARK loaded` (m3_analyze.py warns). Earlier: `.bkm` files are chapter bookmarks. About 30 checkpoints exist as `.sav` (most in `.deleted/`): Lighthouse, Welcome Center, Fair, Raffle, Rooftops, Founders Lodge, Gondola, Zeppelin crash, Monument Island (LizT_*), Battleship Bay (BW_*).

## Chat 3 measurement series
- Scenario `scenarios\measure_spot_vr_v1.txt` (build 78): Continue, 60 s settle, 90 s each at head yaw 0 / 90 / 180 / -90, 90 s head sweep, 80 s cpuprof (simpleperf 45 s), quit. `measure_spot_vr_v2.txt` (build 79): + `x_census` window and `x_draw` phase (gpuprof.txt).
- Jobs j0011-j0029 (one per spot), analysis `m3_analyze.py` (cloud backup `cloud_backup\m3\`).

## Interrupted jobs (v9)
- If the runner window is closed (or the runner crashes) mid-job, the job file stays in `running\`. At the next start the runner moves it to `done\<name>.interrupted.json` and queues `inbox\a_recover_<id>.json` first: devices, stop_game, the job's own `always` steps and `restore_saves`, working in the interrupted job's outbox (`use_outbox`). Saves and config files are put back before any new job runs.
- Learned 19 Oct (j0017): a click into the runner's console window starts a QuickEdit selection and pauses the script at its next output line; the runner was then restarted and j0017 never ran its cleanup (its Founders Lodge save copy stayed newest). v9 switches QuickEdit off at start. Minimise the window; Esc ends a selection.

## More gotchas (chat 3, 19-20 Oct)
- **"Switch to Controllers" system dialog** blocks app launches (`launch FAILED: game process did not appear after am start`) when the controllers fell asleep and hand tracking took over (j0035-j0043; screenshot at_end.jpg). The user turned hand tracking OFF (19 Oct). If it ever returns: PAUSE the lab, ask the user to wake the controllers and press Continue. Build idea: declare hand-tracking support in the manifest.
- Check `charging` in info*.json: the cable came loose once (battery fell to 70 %).
- **DLC "unlocked" popups come as a chain** after loading some saves (Lighthouse: golden weapons, then Comstock's Eye, ...). Scenarios press A six times, 3 s apart, after `mark loaded` (A = jump in gameplay: harmless).
- Moving checkpoints (Gondola ride, crowds) vary more between runs than static ones (5 Gondola repeats on b78 agreed within ~3 %; the b79 Gondola run was lighter).
- Final results of the chat 3 series: `claude/bsi-savings-mode-plan.md` "Step 0 RESULTS", tables in `analysis\m3\`.

## Driver (3 Nov 2026)
Default Turnip = `/sdcard/BSI/turnip/vulkan.ad07xx.so` = our own build `drivers\own-mesa-bfd3-bsi-on` (both fixes ON; user decision 3 Nov). Driver-experiment jobs: step `pull` the current file to `turnip_prev.so`, `push` the test driver, and push `turnip_prev.so` back in `always` - from the job's OWN outbox folder (make_jobs.py rewrites it; hand-copied jobs must too, or they restore the template's old driver). Fallback / switch-back adb lines: `plans\bsi-own-turnip-driver.md` section 7.
