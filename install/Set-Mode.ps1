<#
  Set-Mode.ps1 - choose how BSI-Quest shows the game: VR or Flat.

    VR    stereo 3D: you are inside the world, the HUD floats in front of you. Two views are rendered, so it is the
          heavier mode (24-49 fps depending on the area).
    Flat  the game on a big virtual screen in front of you, like a cinema. One view is rendered, so it runs
          roughly twice as fast. Head movement does not move the camera.

  The mode is read when the game starts: close the game, run this, start the game again.

  How it works: VR mode is on while /sdcard/BSI/vr_stereo.txt exists. Flat renames that file to vr_stereo.txt.off
  (nothing is deleted, your VR settings are kept); VR renames it back, or copies the default if there is none.

  Usage:
    powershell -ExecutionPolicy Bypass -File Set-Mode.ps1 -Mode Flat
    powershell -ExecutionPolicy Bypass -File Set-Mode.ps1 -Mode VR
    powershell -ExecutionPolicy Bypass -File Set-Mode.ps1              # shows the current mode
#>
param(
  [ValidateSet("VR", "Flat", "Show")][string]$Mode = "Show",
  [string]$Adb = ""
)
$ErrorActionPreference = "Stop"
$Dir = "/sdcard/BSI"
$Package = "com.bsiquest.game"

function Fail($t) { Write-Host ""; Write-Host "STOPPED: $t" -ForegroundColor Red; exit 1 }
function Find-Adb {
  if ($Adb) { if (Test-Path $Adb) { return $Adb } else { Fail "adb not found at $Adb" } }
  $c = Get-Command adb -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in "$env:ProgramFiles\SideQuest\resources\platform-tools\adb.exe",
                 "$env:LOCALAPPDATA\Programs\SideQuest\resources\platform-tools\adb.exe",
                 "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe") { if (Test-Path $p) { return $p } }
  Fail "adb not found. Install SideQuest, or pass -Adb <path to adb.exe>."
}
$adbExe = Find-Adb
# Windows PowerShell turns stderr of a native program into errors; with "Stop" that would abort on harmless adb messages.
function Nat { $ErrorActionPreference = "Continue"; & $adbExe @args 2>&1 | ForEach-Object { "$_" } }
function Sh([string]$cmd) {
  $out = Nat shell $cmd
  if ($LASTEXITCODE -ne 0) { Fail ("adb shell $cmd`n" + ($out -join "`n")) }
  return $out
}

$devs = @(Nat devices | Where-Object { $_ -match "\tdevice$" })
if ($devs.Count -ne 1) { Fail "Expected exactly one connected headset, found $($devs.Count)." }

$names = @(Sh "ls $Dir")
$hasOn  = $names -contains "vr_stereo.txt"
$hasOff = $names -contains "vr_stereo.txt.off"
$current = $(if ($hasOn) { "VR" } else { "Flat" })
Write-Host "Current mode: $current"
if ($Mode -eq "Show") { Write-Host "Change it with -Mode VR or -Mode Flat."; exit 0 }
if ($Mode -eq $current) { Write-Host "Already in $Mode mode. Nothing changed."; exit 0 }

if ($Mode -eq "Flat") {
  # keep the player's VR settings: rename, never delete. An older .off copy is kept under a numbered name.
  if ($hasOff) { Sh "mv $Dir/vr_stereo.txt.off $Dir/vr_stereo.txt.off.old" | Out-Null }
  Sh "mv $Dir/vr_stereo.txt $Dir/vr_stereo.txt.off" | Out-Null
  if (-not ($names -contains "resolution.txt")) {
    $tmp = Join-Path $env:TEMP "bsi_resolution.txt"
    Set-Content -Path $tmp -Value "1600x900" -Encoding ascii -NoNewline
    Nat push $tmp "$Dir/resolution.txt" | Out-Null
    Remove-Item -LiteralPath (Get-Item -LiteralPath $tmp).FullName
  }
  Write-Host "Flat mode set. Screen size comes from $Dir/resolution.txt (default 1600x900)."
} else {
  if ($hasOff) {
    Sh "mv $Dir/vr_stereo.txt.off $Dir/vr_stereo.txt" | Out-Null
  } else {
    $def = Join-Path $PSScriptRoot "config\vr_stereo.txt"
    if (-not (Test-Path $def)) { Fail "Default VR settings not found: $def" }
    Nat push $def "$Dir/vr_stereo.txt" | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Could not copy the VR settings to the headset." }
  }
  Write-Host "VR mode set."
}
$running = @(Nat shell pidof $Package | Where-Object { $_ -match "^\d+" })
if ($running.Count) { Write-Host "The game is running: close it and start it again for the change to take effect." }
else { Write-Host "Start the game to play in $Mode mode." }
exit 0
