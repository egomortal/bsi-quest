<#
  Install-BSIQuest.ps1 - put BioShock Infinite (your own Steam copy) on a Meta Quest 3.

  What it does, in order:
    1. checks your game folder and that BioShockInfinite.exe is the build this app was made from
    2. finds adb and exactly one connected headset
    3. installs the APK from the release (bsi-quest.apk, next to this script)
    4. copies the Vulkan driver and the settings files to /sdcard/BSI and sets VR or Flat
    5. copies your game data to the headset (about 42 GB with DLC; re-running only copies what is missing or changed)
       into the app's own folder, or into /sdcard/BSI/BioShock Infinite if the headset does not let adb write there

  It never changes your Steam folder, never deletes anything on the headset (it may move its own game copy from the app's
  folder to /sdcard/BSI/BioShock Infinite when the app cannot read it), and keeps settings files that are already there
  (use -ResetSettings to replace them with the defaults).

  Usage (PowerShell, headset connected by USB with developer mode on):
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1 -Game "D:\SteamLibrary\steamapps\common\BioShock Infinite" -NoDLC
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1 -DryRun      # show what would happen, change nothing
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1 -Mode Flat   # big-screen mode instead of VR (Set-Mode.ps1 switches later)
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1 -SettingsOnly   # after an update: add new settings files only (keeps the VR/Flat mode)
#>
param(
  [string]$Game = "",
  [string]$Apk = (Join-Path $PSScriptRoot "bsi-quest.apk"),
  [string]$Driver = (Join-Path $PSScriptRoot "vulkan.ad07xx.so"),
  [string]$Adb = "",
  [ValidateSet("VR", "Flat")][string]$Mode = "VR",
  [switch]$NoDLC,
  [switch]$SkipGameData,
  [switch]$SettingsOnly,   # only the driver, settings and storage access: no game check, no app install, no game copy
  [switch]$ResetSettings,
  [switch]$DryRun
)
$ErrorActionPreference = "Stop"

# The app contains the translated code of exactly this executable (Steam build, 18,368,840 bytes).
$ExeSha256 = "c20a42529c4181b1d10eed416d9847a823f9453dd267ca478efb3a10014a0e77"
$Package   = "com.bsiquest.game"
$GameDst   = "/sdcard/Android/data/$Package/files/game"
$CfgDst    = "/sdcard/BSI"
# Second place the app looks for the game (android_glue.c): used when the headset will not let adb write into the app's
# own folder. Reads there go through Android's storage layer, so loading can be a little slower.
$GameAlt   = "$CfgDst/BioShock Infinite"
$CfgSrc    = Join-Path $PSScriptRoot "config"

function Say($t)  { Write-Host $t }
function Step($t) { Write-Host ""; Write-Host "== $t" -ForegroundColor Cyan }
function Fail($t) { Write-Host ""; Write-Host "STOPPED: $t" -ForegroundColor Red; exit 1 }

function Run-Adb {
  # Runs adb and returns its output lines; stops the script when adb fails.
  $ErrorActionPreference = "Continue"   # Windows PowerShell turns native stderr into errors; do not abort on adb progress text
  $out = & $script:Adb @args 2>&1 | ForEach-Object { "$_" }
  if ($LASTEXITCODE -ne 0) { Fail ("adb " + ($args -join " ") + "`n" + ($out -join "`n")) }
  return $out
}

function Find-Adb {
  if ($Adb) { if (Test-Path $Adb) { return $Adb } else { Fail "adb not found at $Adb" } }
  $c = Get-Command adb -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  $candidates = @(
    "$env:APPDATA\SideQuest\platform-tools\adb.exe",
    "$env:ProgramFiles\SideQuest\resources\platform-tools\adb.exe",
    "$env:LOCALAPPDATA\Programs\SideQuest\resources\platform-tools\adb.exe",
    "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
  )
  foreach ($p in $candidates) { if (Test-Path $p) { return $p } }
  Fail "adb not found. Install SideQuest (it includes adb) or Android platform-tools, or pass -Adb <path to adb.exe>."
}

function Find-Game {
  if ($Game) { return $Game }
  $roots = @()
  foreach ($key in "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam", "HKLM:\SOFTWARE\Valve\Steam", "HKCU:\SOFTWARE\Valve\Steam") {
    try {
      $p = Get-ItemProperty -Path $key -ErrorAction Stop
      foreach ($n in "InstallPath", "SteamPath") { if ($p.$n) { $roots += $p.$n } }
    } catch {}
  }
  $libs = @()
  foreach ($r in ($roots | Select-Object -Unique)) {
    $libs += $r
    $vdf = Join-Path $r "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
      foreach ($line in Get-Content $vdf) {
        if ($line -match '"path"\s+"(.+)"') { $libs += ($Matches[1] -replace '\\\\', '\') }
      }
    }
  }
  foreach ($l in ($libs | Select-Object -Unique)) {
    $g = Join-Path $l "steamapps\common\BioShock Infinite"
    if (Test-Path (Join-Path $g "Binaries\Win32\BioShockInfinite.exe")) { return $g }
  }
  Fail "BioShock Infinite not found in your Steam libraries. Pass -Game <folder that contains Binaries and XGame>."
}

# ---------------------------------------------------------------- 1. game folder (first: needs no headset, so the exe check works on its own)
if ($SettingsOnly) { $SkipGameData = $true } else {
Step "Game folder"
$Game = Find-Game
$exe = Join-Path $Game "Binaries\Win32\BioShockInfinite.exe"
if (-not (Test-Path $exe)) { Fail "Not found: $exe" }
Say "game: $Game"
$hash = (Get-FileHash -Algorithm SHA256 $exe).Hash.ToLower()
if ($hash -ne $ExeSha256) {
  Fail ("Your BioShockInfinite.exe is a different build than the one this app was made from.`n" +
        "  yours:    $hash`n  expected: $ExeSha256`n" +
        "Steam copy: verify the game files in Steam, then run again.`n" +
        "GOG or Epic copy: not supported yet. Please open an issue on the project page with the 'yours' line above and the`n" +
        "store you bought it from (only that line, never the file), so we can find out what supporting it would take.")
}
Say "exe: matches the supported Steam build"

$folders = @("Binaries", "Engine", "XGame")
if (-not $NoDLC -and (Test-Path (Join-Path $Game "DLC"))) { $folders += "DLC" }
foreach ($f in $folders) { if (-not (Test-Path (Join-Path $Game $f))) { Fail "Game folder is incomplete: '$f' is missing." } }
}

# ---------------------------------------------------------------- 2. adb + headset
Step "Headset"
$script:Adb = Find-Adb
Say "adb: $script:Adb"
$devices = @(Run-Adb devices | Where-Object { $_ -match "\tdevice$" })
if ($devices.Count -eq 0) { Fail "No headset found. Connect the Quest by USB, turn on developer mode, and accept the 'Allow USB debugging' prompt inside the headset." }
if ($devices.Count -gt 1) { Fail "More than one adb device is connected. Disconnect the others and run again." }
$model = (Run-Adb shell getprop ro.product.model | Select-Object -First 1).Trim()
Say "device: $model"
if ($model -eq "Quest 3S") { Say "NOTE: the Quest 3S has the same chip as the Quest 3 (Snapdragon XR2 Gen 2, Adreno 740), so it should work, but it has not been tested yet. Please send a play report (REPORTS.md)." }
elseif ($model -ne "Quest 3") { Say "WARNING: this build was made and tested on a Quest 3 only (Adreno 740). '$model' has a slower GPU and is not expected to work." }

# ---------------------------------------------------------------- 3. release files
Step "Release files"
if (-not $SettingsOnly -and -not (Test-Path $Apk)) { Fail "APK not found: $Apk`nDownload bsi-quest.apk from the Releases page and put it next to this script." }
if (-not (Test-Path $Driver)) { Fail "Driver not found: $Driver`nDownload vulkan.ad07xx.so from the Releases page and put it next to this script." }
if (-not (Test-Path $CfgSrc)) { Fail "Settings folder not found: $CfgSrc" }
if (-not $SettingsOnly) { Say ("apk:    {0} ({1:N1} MB)" -f $Apk, ((Get-Item $Apk).Length / 1MB)) }
Say ("driver: {0} ({1:N1} MB)" -f $Driver, ((Get-Item $Driver).Length / 1MB))

# ---------------------------------------------------------------- space check
if (-not $SkipGameData) {
  $need = 0
  foreach ($f in $folders) { $need += (Get-ChildItem -Path (Join-Path $Game $f) -Recurse -File | Measure-Object Length -Sum).Sum }
  $have = 0
  foreach ($d in $GameDst, $GameAlt) {
    $du = Run-Adb shell "du -sk '$d' 2>/dev/null || echo 0"
    if ("$du" -match "^(\d+)") { $have += [int64]$Matches[1] * 1024 }
  }
  $df = Run-Adb shell "df -k /sdcard" | Select-Object -Last 1
  $free = 0
  if ($df -match "^\S+\s+\d+\s+\d+\s+(\d+)") { $free = [int64]$Matches[1] * 1024 }
  Say ("game data to copy: {0:N1} GB; already on the headset: {1:N1} GB; free on the headset: {2:N1} GB" -f ($need / 1GB), ($have / 1GB), ($free / 1GB))
  if (($need - $have) -gt ($free - 2GB)) {
    Fail ("Not enough free space on the headset. Free up about {0:N0} GB, or run with -NoDLC (the base game needs about 18 GB)." -f ((($need - $have) - $free + 2GB) / 1GB))
  }
}

if ($DryRun) {
  Step "Dry run: nothing was changed. A real run would"
  if (-not $SettingsOnly) { Say "  install $Apk" }
  Say "  copy the driver to $CfgDst/turnip/ and the settings in $CfgSrc to $CfgDst (settings already there are kept)"
  if (-not $SettingsOnly) { Say "  set the display mode to $Mode (VR = stereo 3D, Flat = big virtual screen)" }
  if (-not $SkipGameData) { Say ("  copy " + ($folders -join ", ") + " to $GameDst (or to $GameAlt if the headset does not let adb write there)") }
  exit 0
}

# ---------------------------------------------------------------- 4. install
if ($SettingsOnly) {
  $ErrorActionPreference = "Continue"
  $isIn = "$(& $script:Adb shell pm list packages $Package 2>&1)"
  $ErrorActionPreference = "Stop"
  if (-not ($isIn -match "package:$Package")) { Fail "The app is not installed on this headset. Run the full install (I) instead." }
  Step "Settings update (the app and the game data are left as they are)"
  Run-Adb shell am force-stop $Package | Out-Null
} else {
Step "Installing the app"
Run-Adb shell am force-stop $Package | Out-Null
Run-Adb install -r -g $Apk | ForEach-Object { Say $_ }
}
# The app has no Java code to ask for storage access, and it reads its settings, driver and (on newer headsets) the game
# from /sdcard/BSI. Grant that here: without it the app cannot see /sdcard/BSI and stops at start.
$ErrorActionPreference = "Continue"
foreach ($p in "READ_EXTERNAL_STORAGE", "WRITE_EXTERNAL_STORAGE") { & $script:Adb shell pm grant $Package "android.permission.$p" 2>&1 | Out-Null }
& $script:Adb shell appops set $Package MANAGE_EXTERNAL_STORAGE allow 2>&1 | Out-Null
$ErrorActionPreference = "Stop"
$perm = "$(& $script:Adb shell appops get $Package MANAGE_EXTERNAL_STORAGE 2>&1)"
if ($perm -match "allow") { Say "storage access: granted" }
else { Say "WARNING: could not grant storage access ($perm). If the game stops at start: Settings > Apps > BSI Quest > Permissions > allow file access." }

# ---------------------------------------------------------------- 5. driver + settings (before the long copy, so a copy stopped halfway still leaves a working setup)
Step "Driver and settings"
Run-Adb shell mkdir -p "$CfgDst/turnip" | Out-Null
Run-Adb push $Driver "$CfgDst/turnip/vulkan.ad07xx.so" | Out-Null
Say "driver copied"
$existing = @(Run-Adb shell ls $CfgDst)
foreach ($file in Get-ChildItem -Path $CfgSrc -File) {
  if (($existing -contains $file.Name) -and -not $ResetSettings) { Say "kept     $($file.Name) (already on the headset)"; continue }
  Run-Adb push $file.FullName "$CfgDst/$($file.Name)" | Out-Null
  Say "copied   $($file.Name)"
}
# Settings folders (config\ao\gtao_match = GTAO ambient occlusion, selected by ao_sel.txt = 3): same rule, file by file.
$cfgRoot = (Get-Item -LiteralPath $CfgSrc).FullName
foreach ($file in Get-ChildItem -Path $cfgRoot -File -Recurse | Where-Object { $_.DirectoryName -ne $cfgRoot }) {
  $rel = $file.FullName.Substring($cfgRoot.Length + 1) -replace '\\', '/'
  $remote = "$CfgDst/$rel"
  $ErrorActionPreference = "Continue"
  $there = "$(& $script:Adb shell "ls '$remote' 2>/dev/null" 2>&1)"
  $ErrorActionPreference = "Stop"
  if (($there -match [regex]::Escape($file.Name)) -and -not $ResetSettings) { Say "kept     $rel (already on the headset)"; continue }
  Run-Adb shell "mkdir -p '$($remote.Substring(0, $remote.LastIndexOf('/')))'" | Out-Null
  Run-Adb push $file.FullName $remote | Out-Null
  Say "copied   $rel"
}
# The app uses /sdcard/BSI as its settings folder only when bsi.log exists there. Never delete that file.
if (-not ($existing -contains "bsi.log")) {
  $tmp = Join-Path $env:TEMP "bsi_placeholder.log"
  Set-Content -Path $tmp -Value "placeholder written by Install-BSIQuest.ps1" -Encoding ascii
  Run-Adb push $tmp "$CfgDst/bsi.log" | Out-Null
  Remove-Item -LiteralPath (Get-Item -LiteralPath $tmp).FullName
  Say "created  bsi.log"
}

# ---------------------------------------------------------------- 6. display mode
if (-not $SettingsOnly) {
Step "Display mode: $Mode"
& (Join-Path $PSScriptRoot "Set-Mode.ps1") -Mode $Mode -Adb $script:Adb
if ($LASTEXITCODE -ne 0) { Fail "Could not set the display mode. Run install\Set-Mode.ps1 -Mode $Mode yourself." }
}

# ---------------------------------------------------------------- 7. game data
function Test-AppDir {
  $ErrorActionPreference = "Continue"
  $o = & $script:Adb shell "ls -d /sdcard/Android/data/$Package/files 2>/dev/null" 2>&1 | ForEach-Object { "$_" }
  return ("$o" -match "/files")
}
function Test-AdbCanWrite([string]$dir) {
  # Current Quest software refuses folders made by adb push itself (adbd "secure_mkdirs") inside Android/data, but
  # allows "adb shell mkdir" and pushes into folders that already exist. So the copy makes every folder with the shell
  # first (New-RemoteDirs). Probe the same way: shell mkdir, push a tiny file, then remove only that probe.
  $ErrorActionPreference = "Continue"
  $tmp = Join-Path $env:TEMP "bsi_probe.txt"
  Set-Content -Path $tmp -Value "probe written by Install-BSIQuest.ps1" -Encoding ascii
  & $script:Adb shell "mkdir -p '$dir/.bsi_probe'" 2>&1 | Out-Null
  $o = & $script:Adb push $tmp "$dir/.bsi_probe/probe.txt" 2>&1 | ForEach-Object { "$_" }
  $ok = ($LASTEXITCODE -eq 0)
  & $script:Adb shell "rm -f '$dir/.bsi_probe/probe.txt'; rmdir '$dir/.bsi_probe'" 2>&1 | Out-Null
  Remove-Item -LiteralPath (Get-Item -LiteralPath $tmp).FullName
  if (-not $ok) { Say ("  adb cannot write to $dir`n  " + ($o -join "`n  ")) }
  return $ok
}
function New-RemoteDirs([string]$local, [string]$remote) {
  # Make $remote/<name of $local> and every folder below it with "adb shell mkdir -p", a few dozen per call.
  $base = (Get-Item -LiteralPath $local).FullName
  $leaf = Split-Path $base -Leaf
  $rel = @($leaf) + @(Get-ChildItem -LiteralPath $base -Directory -Recurse | ForEach-Object {
    $leaf + "/" + ($_.FullName.Substring($base.Length + 1) -replace '\\', '/') })
  for ($i = 0; $i -lt $rel.Count; $i += 40) {
    $batch = $rel[$i..([Math]::Min($i + 39, $rel.Count - 1))] | ForEach-Object { "'" + ("$remote/$_" -replace "'", "'\''") + "'" }
    Run-Adb shell ("mkdir -p " + ($batch -join " ")) | Out-Null
  }
}
function Test-AppCanRead {
  # On some headsets the app is not allowed to read files that adb put into its own folder (first player install: the
  # app's log said "app-folder read test ... FAILED Permission denied", found no game and stayed black). Ask the app
  # itself: put readtest.txt there, start the app once, and read the line it writes to /sdcard/BSI/bsi.log at start.
  # Returns "OK", "FAILED" or "unknown".
  $ErrorActionPreference = "Continue"
  $tmp = Join-Path $env:TEMP "bsi_readtest.txt"
  Set-Content -Path $tmp -Value "readable" -Encoding ascii
  & $script:Adb shell "mkdir -p '$GameDst'" 2>&1 | Out-Null
  & $script:Adb push $tmp "$GameDst/readtest.txt" 2>&1 | Out-Null
  Remove-Item -LiteralPath (Get-Item -LiteralPath $tmp).FullName
  $before = "$(& $script:Adb shell "stat -c %Y $CfgDst/bsi.log 2>/dev/null" 2>&1)".Trim()
  & $script:Adb shell am force-stop $Package 2>&1 | Out-Null
  & $script:Adb shell am start -n "$Package/android.app.NativeActivity" 2>&1 | Out-Null
  $result = "unknown"
  for ($i = 0; $i -lt 40 -and $result -eq "unknown"; $i++) {
    Start-Sleep -Seconds 1
    $now = "$(& $script:Adb shell "stat -c %Y $CfgDst/bsi.log 2>/dev/null" 2>&1)".Trim()
    if ($now -eq $before) { continue }
    $line = "$(& $script:Adb shell "grep -m1 'app-folder read test' $CfgDst/bsi.log" 2>&1)"
    if ($line -match "read test .*: OK") { $result = "OK" } elseif ($line -match "read test .*: FAILED") { $result = "FAILED" }
  }
  & $script:Adb shell am force-stop $Package 2>&1 | Out-Null
  return $result
}
if (-not $SkipGameData) {
  $target = $GameDst
  # A copy already started in the fallback folder (earlier run): keep using it.
  $altExe = & $script:Adb shell "ls '$GameAlt/Binaries' 2>/dev/null" 2>&1 | ForEach-Object { "$_" }
  if ("$altExe" -match "Win32") { $target = $GameAlt }
  else {
    # Android 11+ lets only the system create /sdcard/Android/data/<app>; it does so the first time the app starts.
    # adb cannot create it ("secure_mkdirs failed: Operation not permitted"), so start the app once and wait for it.
    if (-not (Test-AppDir)) {
      Step "Starting the app once so the headset creates its data folder"
      $ErrorActionPreference = "Continue"
      & $script:Adb shell am start -W -n "$Package/android.app.NativeActivity" 2>&1 | Out-Null
      $ErrorActionPreference = "Stop"
      $ok = $false
      for ($i = 0; $i -lt 30 -and -not $ok; $i++) { Start-Sleep -Seconds 1; $ok = Test-AppDir }
      Run-Adb shell am force-stop $Package | Out-Null
      if ($ok) { Say "data folder created" }
    }
    if (-not (Test-AppDir) -or -not (Test-AdbCanWrite $GameDst)) {
      Say "This headset does not let adb write into the app's own folder. Using $GameAlt instead (the app finds it there)."
      $target = $GameAlt
    }
    else {
      Step "Checking that the app can read its own folder (starts the app for a few seconds)"
      $read = Test-AppCanRead
      if ($read -eq "OK") { Say "the app can read its folder" }
      elseif ($read -eq "unknown") { Say "WARNING: could not check (the app did not write its log in time). Copying to $GameDst anyway." }
      else {
        Say "This headset does not let the app read files copied into its own folder. Using $GameAlt instead (the app finds it there)."
        # Most likely cause: the folder was left behind by an earlier install and belongs to the old app's user ID.
        $ErrorActionPreference = "Continue"
        $owner = "$(& $script:Adb shell "ls -lnd /sdcard/Android/data/$Package" 2>&1)"
        $appId = "$(& $script:Adb shell "dumpsys package $Package | grep -m1 appId=" 2>&1)"
        $ErrorActionPreference = "Stop"
        if ($owner -match "^\S+\s+\d+\s+(\d+)" -and ($o = $Matches[1]) -and $appId -match "appId=(\d+)" -and $o -ne $Matches[1]) {
          Say ("NOTE: the app's folder belongs to an earlier install (owner $o, app $($Matches[1])). For a clean setup like the`n" +
               "      tested one, run Uninstall.bat and then Install.bat again. This install still works without that.")
        }
        $target = $GameAlt
        # Game data already copied there by an earlier run: move it on the headset instead of copying 42 GB again.
        Run-Adb shell "mkdir -p '$GameAlt'" | Out-Null
        foreach ($f in $folders) {
          $ErrorActionPreference = "Continue"
          $src = "$(& $script:Adb shell "ls -d '$GameDst/$f' 2>/dev/null" 2>&1)"
          $dst = "$(& $script:Adb shell "ls -d '$GameAlt/$f' 2>/dev/null" 2>&1)"
          $ErrorActionPreference = "Stop"
          if ($src -match "/$f" -and -not ($dst -match "/$f")) {
            Say "moving $f from the app's folder to $GameAlt on the headset (can take several minutes)"
            Run-Adb shell "mv '$GameDst/$f' '$GameAlt/$f'" | Out-Null
          }
        }
      }
    }
  }
  if ($target -eq $GameAlt -and -not (Test-AdbCanWrite $GameAlt)) {
    Fail "adb cannot write to $GameAlt either. Unplug and replug the cable, accept 'Allow USB debugging' in the headset, and run again."
  }
  Step "Copying game data to $target (this takes a long time the first time; safe to re-run if it is interrupted)"
  Run-Adb shell "mkdir -p '$target'" | Out-Null
  foreach ($f in $folders) {
    Say "-> $f"
    if ($f -eq "DLC") { Say "   (the DLC is about 24 GB in about 7,000 files: the percentage can sit still for several minutes. Keep the headset awake and let it run until it says Done.)" }
    New-RemoteDirs (Join-Path $Game $f) $target
    $ErrorActionPreference = "Continue"; & $script:Adb push --sync (Join-Path $Game $f) "$target/"; $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0) { Fail "Copying '$f' failed. Check the cable and run the script again; it continues where it stopped." }
  }
}

Step "Done"
Say "Put the headset on and start 'BSI Quest' from Unknown Sources in your app library."
Say "Switch between VR and the big-screen Flat mode any time with install\Set-Mode.ps1 -Mode VR / -Mode Flat."
Say "The first start compiles shaders and stutters for a few minutes. Logs: $CfgDst/bsi.log"
