<#
  Send-Report.ps1 - make a play report from your last session (opt-in; nothing is uploaded by this script).

  WHY WE ASK
    BSI-Quest is tested on one headset by one person. Your play sessions cover parts of the game we have not reached.
    A report tells us three things we cannot find out ourselves:
      1. Game code the translator has not seen yet. When the game reaches such code the app falls back to a slow
         emulator and writes the address to its log. Each address you find gets translated in the next build.
      2. Where the frame rate is below target (by level), on your headset.
      3. Where it crashed.

  WHAT GOES INTO THE REPORT (and nothing else)
      bsi.log, bench.txt, session.txt   the app's own logs of your LAST session (technical: renderer, frame times, levels)
      readmap.txt                       only if you turned on BSI_READMAP=1 yourself: which parts of which game FILES were
                                        read (file names and block numbers, no file contents). Tells us what data is never used.
      env.txt, vr_stereo.txt, mods.txt, ini_overrides.txt, resolution.txt   your BSI-Quest settings files
      device.txt                        headset model, OS build, BSI-Quest build number
      summary.txt                       a short digest of the above (shown to you before anything is saved)
      note.txt                          only if you pass -Note "what happened"

  WHAT DOES NOT GO IN
      No saves, no screenshots, no game files, no serial number, no account or network information.
      The script reads only /sdcard/BSI on the headset and changes nothing there.

  WHAT HAPPENS TO IT
      The script writes one zip file into the "reports" folder next to it and stops. Nothing is sent anywhere.
      If you want to share it, open the zip, check it, and attach it to an issue on the project's GitHub page.

  Usage:
    powershell -ExecutionPolicy Bypass -File Send-Report.ps1
    powershell -ExecutionPolicy Bypass -File Send-Report.ps1 -Note "froze on the airship after the cutscene"
    powershell -ExecutionPolicy Bypass -File Send-Report.ps1 -FromFolder C:\logs     # use files you pulled yourself
#>
param(
  [string]$Note = "",
  [string]$FromFolder = "",
  [string]$Adb = "",
  [string]$OutDir = (Join-Path $PSScriptRoot "reports"),
  [switch]$Yes
)
$ErrorActionPreference = "Stop"
$Package = "com.bsiquest.game"
$Logs    = @("bsi.log", "bench.txt", "session.txt", "readmap.txt")
$Config  = @("env.txt", "vr_stereo.txt", "mods.txt", "ini_overrides.txt", "resolution.txt")

function Fail($t) { Write-Host ""; Write-Host "STOPPED: $t" -ForegroundColor Red; exit 1 }

function Find-Adb {
  if ($Adb) { if (Test-Path $Adb) { return $Adb } else { Fail "adb not found at $Adb" } }
  $c = Get-Command adb -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in "$env:APPDATA\SideQuest\platform-tools\adb.exe",
                 "$env:ProgramFiles\SideQuest\resources\platform-tools\adb.exe",
                 "$env:LOCALAPPDATA\Programs\SideQuest\resources\platform-tools\adb.exe",
                 "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe") { if (Test-Path $p) { return $p } }
  Fail "adb not found. Install SideQuest, or pass -Adb <path to adb.exe>, or use -FromFolder."
}

# ---------------------------------------------------------------- consent
Write-Host ""
Write-Host "BSI-Quest play report (opt-in)" -ForegroundColor Cyan
Write-Host @"

Why: your session can show us game code we have not translated yet, slow levels and crashes
     that we cannot find on our single test headset.
What is collected: the app's logs of your last session (bsi.log, bench.txt, session.txt), your
     BSI-Quest settings files, and the headset model / OS build / app build number.
Not collected: saves, screenshots, game files, serial number, account or network information.
Nothing is uploaded. The script only writes a zip file on this PC; sharing it is a separate step you do yourself.
"@
if (-not $Yes) {
  $a = Read-Host "Create the report? (y/n)"
  if ($a -notmatch '^(y|yes)$') { Write-Host "No report created."; exit 0 }
}

# ---------------------------------------------------------------- collect
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$work  = Join-Path $env:TEMP "bsi_report_$stamp"
$work = (New-Item -ItemType Directory -Force -Path $work).FullName   # long path: a short (8.3) TEMP path breaks Remove-Item
$device = @()

if ($FromFolder) {
  if (-not (Test-Path $FromFolder)) { Fail "Folder not found: $FromFolder" }
  foreach ($f in ($Logs + $Config)) {
    $src = Join-Path $FromFolder $f
    if (Test-Path $src) { Copy-Item $src (Join-Path $work $f) }
  }
  # a single *.log file in the folder counts as bsi.log
  if (-not (Test-Path (Join-Path $work "bsi.log"))) {
    $one = @(Get-ChildItem -Path $FromFolder -Filter *.log -File)
    if ($one.Count -eq 1) { Copy-Item $one[0].FullName (Join-Path $work "bsi.log") }
  }
  $device += "source: local folder (no headset queried)"
} else {
  $adbExe = Find-Adb
  # Windows PowerShell turns stderr of a native program into errors; with "Stop" a missing file on the headset would abort.
  function Nat { $ErrorActionPreference = "Continue"; & $adbExe @args 2>&1 | ForEach-Object { "$_" } }
  $devs = @(Nat devices | Where-Object { $_ -match "\tdevice$" })
  if ($devs.Count -ne 1) { Fail "Expected exactly one connected headset, found $($devs.Count)." }
  foreach ($f in ($Logs + $Config)) {
    Nat pull "/sdcard/BSI/$f" (Join-Path $work $f) | Out-Null     # a missing file is fine
  }
  $device += "model: " + ((Nat shell getprop ro.product.model) -join "").Trim()
  $device += "os build: " + ((Nat shell getprop ro.build.display.id) -join "").Trim()
  $device += "android: " + ((Nat shell getprop ro.build.version.release) -join "").Trim()
  $vc = (Nat shell dumpsys package $Package | Where-Object { $_ -match "versionCode=(\d+)" } | Select-Object -First 1)
  if ($vc -match "versionCode=(\d+)") { $device += "bsi-quest build: " + $Matches[1] } else { $device += "bsi-quest build: not installed?" }
}
$logPath = Join-Path $work "bsi.log"
if (-not (Test-Path $logPath)) { Remove-Item -Recurse -Force -LiteralPath $work; Fail "No bsi.log found. Play a session first, then run this again." }

# ---------------------------------------------------------------- scrub
# The only per-install token the logs carry is Android's random app-install folder name. Replace it.
$lines = Get-Content $logPath
$lines = $lines | ForEach-Object { $_ -replace '/data/app/~~[^/]+/[^/]+/', '/data/app/<app>/' }
Set-Content -Path $logPath -Value $lines -Encoding ascii

# ---------------------------------------------------------------- summary
$renderer = ($lines | Where-Object { $_ -match '^\[xr\] setup:' } | Select-Object -First 1)
$driver   = ($lines | Where-Object { $_ -match 'Found device:|Turnip driver ready|packaged libvulkan' } | Select-Object -First 2)

# levels visited, with the frame-time lines that fall inside each
$map = "(before the first level)"; $order = New-Object System.Collections.ArrayList; $stats = @{}
foreach ($l in $lines) {
  if ($l -match '^\[gear\] world (\S+) ' -or $l -match '^\[legend\] status: map (\S+),' -or $l -match '^\[autopilot\] map -> (\S+)') {
    $m = $Matches[1].TrimEnd(',')
    if ($m -ne $map) { $map = $m }
    continue
  }
  if ($l -match '^\[bench\] ([\d.]+) fps avg, ([\d.]+) fps 1%-low, worst (\d+) ms, hitches>100ms (\d+)') {
    if (-not $stats.ContainsKey($map)) { $stats[$map] = @{ n = 0; fps = 0.0; low = 1e9; worst = 0; hitches = 0 }; [void]$order.Add($map) }
    $s = $stats[$map]; $s.n++; $s.fps += [double]$Matches[1]
    if ([double]$Matches[2] -lt $s.low) { $s.low = [double]$Matches[2] }
    if ([int]$Matches[3] -gt $s.worst) { $s.worst = [int]$Matches[3] }
    $s.hitches += [int]$Matches[4]
  }
}

# untranslated code reached (emulator fallback)
$emu = @{}
foreach ($l in $lines) {
  if ($l -match '^\[emu\] enter ([0-9a-f]{8}) from ([0-9a-f]{8})' -or $l -match 'emulating untranslated code at ([0-9a-f]{8})') {
    $k = $Matches[1]; if ($emu.ContainsKey($k)) { $emu[$k]++ } else { $emu[$k] = 1 }
  }
}
$emustat = ($lines | Where-Object { $_ -match '^\[emustat\]' } | Select-Object -Last 1)

# crash
$crash = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
  if ($lines[$i] -match '^\*\*\* host signal|Fatal signal|\[crash\]') { $crash += $lines[$i..([Math]::Min($i + 3, $lines.Count - 1))]; break }
}

$sum = New-Object System.Collections.ArrayList
[void]$sum.Add("BSI-Quest play report $stamp")
foreach ($d in $device) { [void]$sum.Add($d) }
if ($renderer) { [void]$sum.Add($renderer) }
foreach ($d in $driver) { [void]$sum.Add($d) }
[void]$sum.Add("")
[void]$sum.Add("Levels and frame rate (each [bench] line covers about 10 s):")
if ($order.Count -eq 0) { [void]$sum.Add("  no frame-rate lines in this log") }
foreach ($m in $order) {
  $s = $stats[$m]
  [void]$sum.Add(("  {0,-28} {1,4} samples  avg {2,5:N1} fps  lowest 1%-low {3,5:N1}  worst frame {4,5} ms  hitches {5}" -f $m, $s.n, ($s.fps / $s.n), $s.low, $s.worst, $s.hitches))
}
[void]$sum.Add("")
[void]$sum.Add("Untranslated game code reached (emulator fallback): $($emu.Count) address(es)")
foreach ($k in ($emu.Keys | Sort-Object)) { [void]$sum.Add("  $k  x$($emu[$k])") }
if ($emustat) { [void]$sum.Add("  $emustat") }
$rmPath = Join-Path $work "readmap.txt"
if (Test-Path $rmPath) {
  $rmFiles = 0; $rmRead = 0.0; $rmTotal = 0.0
  foreach ($l in Get-Content $rmPath) { if ($l -match '^F (\d+) (\d+) (\d+) ') { $rmFiles++; $rmRead += [double]$Matches[2]; $rmTotal += [double]$Matches[3] } }
  [void]$sum.Add("")
  [void]$sum.Add(("Read map (BSI_READMAP): {0} game files opened, {1:N1} of {2:N1} GB read in them, over all sessions so far" -f $rmFiles, ($rmRead * 65536 / 1GB), ($rmTotal * 65536 / 1GB)))
}
[void]$sum.Add("")
if ($crash.Count) { [void]$sum.Add("Crash:"); foreach ($c in $crash) { [void]$sum.Add("  $c") } } else { [void]$sum.Add("Crash: none found in the log") }
if ($Note) { [void]$sum.Add(""); [void]$sum.Add("Note from the player: $Note"); Set-Content -Path (Join-Path $work "note.txt") -Value $Note -Encoding ascii }

Set-Content -Path (Join-Path $work "summary.txt") -Value $sum -Encoding ascii
Set-Content -Path (Join-Path $work "device.txt") -Value $device -Encoding ascii

Write-Host ""
Write-Host "---------------- summary.txt ----------------" -ForegroundColor Cyan
$sum | ForEach-Object { Write-Host $_ }
Write-Host "---------------------------------------------" -ForegroundColor Cyan

# ---------------------------------------------------------------- zip
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "bsi-quest-report-$stamp.zip"
Compress-Archive -Path (Join-Path $work "*") -DestinationPath $zip
Remove-Item -Recurse -Force -LiteralPath $work
Write-Host ""
Write-Host "Report written: $zip"
Write-Host "Nothing was uploaded. To share it, open the zip, check the files, and attach it to a new issue on the project's GitHub page."
