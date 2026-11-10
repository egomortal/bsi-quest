<#
  Install-BSIQuest.ps1 - put BioShock Infinite (your own Steam copy) on a Meta Quest 3.

  What it does, in order:
    1. checks your game folder and that BioShockInfinite.exe is the build this app was made from
    2. finds adb and exactly one connected headset
    3. installs the APK from the release (bsi-quest.apk, next to this script)
    4. copies your game data to the headset (about 42 GB with DLC; re-running only copies what is missing or changed)
    5. copies the Vulkan driver and the settings files to /sdcard/BSI

  It never changes your Steam folder, never deletes anything on the headset, and keeps settings files that are already there
  (use -ResetSettings to replace them with the defaults).

  Usage (PowerShell, headset connected by USB with developer mode on):
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1 -Game "D:\SteamLibrary\steamapps\common\BioShock Infinite" -NoDLC
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1 -DryRun      # show what would happen, change nothing
    powershell -ExecutionPolicy Bypass -File Install-BSIQuest.ps1 -Mode Flat   # big-screen mode instead of VR (Set-Mode.ps1 switches later)
#>
param(
  [string]$Game = "",
  [string]$Apk = (Join-Path $PSScriptRoot "bsi-quest.apk"),
  [string]$Driver = (Join-Path $PSScriptRoot "vulkan.ad07xx.so"),
  [string]$Adb = "",
  [ValidateSet("VR", "Flat")][string]$Mode = "VR",
  [switch]$NoDLC,
  [switch]$SkipGameData,
  [switch]$ResetSettings,
  [switch]$DryRun
)
$ErrorActionPreference = "Stop"

# The app contains the translated code of exactly this executable (Steam build, 18,368,840 bytes).
$ExeSha256 = "c20a42529c4181b1d10eed416d9847a823f9453dd267ca478efb3a10014a0e77"
$Package   = "com.bsiquest.game"
$GameDst   = "/sdcard/Android/data/$Package/files/game"
$CfgDst    = "/sdcard/BSI"
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
if (-not (Test-Path $Apk))    { Fail "APK not found: $Apk`nDownload bsi-quest.apk from the Releases page and put it next to this script." }
if (-not (Test-Path $Driver)) { Fail "Driver not found: $Driver`nDownload vulkan.ad07xx.so from the Releases page and put it next to this script." }
if (-not (Test-Path $CfgSrc)) { Fail "Settings folder not found: $CfgSrc" }
Say ("apk:    {0} ({1:N1} MB)" -f $Apk, ((Get-Item $Apk).Length / 1MB))
Say ("driver: {0} ({1:N1} MB)" -f $Driver, ((Get-Item $Driver).Length / 1MB))

# ---------------------------------------------------------------- space check
if (-not $SkipGameData) {
  $need = 0
  foreach ($f in $folders) { $need += (Get-ChildItem -Path (Join-Path $Game $f) -Recurse -File | Measure-Object Length -Sum).Sum }
  $have = 0
  $du = Run-Adb shell "du -sk $GameDst 2>/dev/null || echo 0"
  if ("$du" -match "^(\d+)") { $have = [int64]$Matches[1] * 1024 }
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
  Say "  install $Apk"
  if (-not $SkipGameData) { Say ("  copy " + ($folders -join ", ") + " to $GameDst") }
  Say "  copy the driver to $CfgDst/turnip/ and the settings in $CfgSrc to $CfgDst"
  Say "  set the display mode to $Mode (VR = stereo 3D, Flat = big virtual screen)"
  exit 0
}

# ---------------------------------------------------------------- 4. install
Step "Installing the app"
Run-Adb shell am force-stop $Package | Out-Null
Run-Adb install -r $Apk | ForEach-Object { Say $_ }

# ---------------------------------------------------------------- 5. game data
if (-not $SkipGameData) {
  Step "Copying game data (this takes a long time the first time; safe to re-run if it is interrupted)"
  Run-Adb shell mkdir -p $GameDst | Out-Null
  foreach ($f in $folders) {
    Say "-> $f"
    $ErrorActionPreference = "Continue"; & $script:Adb push --sync (Join-Path $Game $f) "$GameDst/"; $ErrorActionPreference = "Stop"
    if ($LASTEXITCODE -ne 0) { Fail "Copying '$f' failed. Check the cable and run the script again; it continues where it stopped." }
  }
}

# ---------------------------------------------------------------- 6. driver + settings
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
# The app uses /sdcard/BSI as its settings folder only when bsi.log exists there. Never delete that file.
if (-not ($existing -contains "bsi.log")) {
  $tmp = Join-Path $env:TEMP "bsi_placeholder.log"
  Set-Content -Path $tmp -Value "placeholder written by Install-BSIQuest.ps1" -Encoding ascii
  Run-Adb push $tmp "$CfgDst/bsi.log" | Out-Null
  Remove-Item -LiteralPath (Get-Item -LiteralPath $tmp).FullName
  Say "created  bsi.log"
}

# ---------------------------------------------------------------- 7. display mode
Step "Display mode: $Mode"
& (Join-Path $PSScriptRoot "Set-Mode.ps1") -Mode $Mode -Adb $script:Adb
if ($LASTEXITCODE -ne 0) { Fail "Could not set the display mode. Run install\Set-Mode.ps1 -Mode $Mode yourself." }

Step "Done"
Say "Put the headset on and start 'BSI Quest' from Unknown Sources in your app library."
Say "Switch between VR and the big-screen Flat mode any time with install\Set-Mode.ps1 -Mode VR / -Mode Flat."
Say "The first start compiles shaders and stutters for a few minutes. Logs: $CfgDst/bsi.log"
