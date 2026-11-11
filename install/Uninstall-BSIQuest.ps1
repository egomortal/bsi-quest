<#
  Uninstall-BSIQuest.ps1 - remove BSI-Quest from the headset cleanly, so the next install starts fresh.

  What it removes:
    - the app (com.bsiquest.game)
    - the app's own folder /sdcard/Android/data/com.bsiquest.game, including the game data copied there
      (Android removes it with the app; if an old copy of the folder was left behind, this script removes it too)
    - only with -GameData: the game data in /sdcard/BSI/BioShock Infinite (used on headsets where the app cannot read
      its own folder)

  What it keeps: everything else in /sdcard/BSI - your saves (/sdcard/BSI/user), settings, driver and logs.
  Your Steam folder on the PC is never touched.

  Usage:
    powershell -ExecutionPolicy Bypass -File Uninstall-BSIQuest.ps1
    powershell -ExecutionPolicy Bypass -File Uninstall-BSIQuest.ps1 -GameData   # also remove /sdcard/BSI/BioShock Infinite
    powershell -ExecutionPolicy Bypass -File Uninstall-BSIQuest.ps1 -DryRun     # show what would be removed
#>
param(
  [string]$Adb = "",
  [switch]$GameData,
  [switch]$DryRun
)
$ErrorActionPreference = "Stop"
$Package = "com.bsiquest.game"
$AppDir  = "/sdcard/Android/data/$Package"
$GameAlt = "/sdcard/BSI/BioShock Infinite"

function Say($t)  { Write-Host $t }
function Fail($t) { Write-Host ""; Write-Host "STOPPED: $t" -ForegroundColor Red; exit 1 }
function Find-Adb {
  if ($Adb) { if (Test-Path $Adb) { return $Adb } else { Fail "adb not found at $Adb" } }
  $c = Get-Command adb -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  foreach ($p in "$env:APPDATA\SideQuest\platform-tools\adb.exe",
                 "$env:ProgramFiles\SideQuest\resources\platform-tools\adb.exe",
                 "$env:LOCALAPPDATA\Programs\SideQuest\resources\platform-tools\adb.exe",
                 "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe") { if (Test-Path $p) { return $p } }
  Fail "adb not found. Install SideQuest, or pass -Adb <path to adb.exe>."
}
$adbExe = Find-Adb
# Windows PowerShell turns stderr of a native program into errors; with "Stop" that would abort on harmless adb messages.
function Nat { $ErrorActionPreference = "Continue"; & $adbExe @args 2>&1 | ForEach-Object { "$_" } }
function Exists([string]$p) { return ("$(Nat shell "ls -d '$p' 2>/dev/null")" -match [regex]::Escape($p)) }

$devs = @(Nat devices | Where-Object { $_ -match "\tdevice$" })
if ($devs.Count -ne 1) { Fail "Expected exactly one connected headset, found $($devs.Count)." }

$installed = ("$(Nat shell pm list packages $Package)" -match "package:$Package")
$hasDir = Exists $AppDir
$hasAlt = Exists $GameAlt
Say ("app installed:                {0}" -f $(if ($installed) { "yes" } else { "no" }))
Say ("app folder ($AppDir): {0}" -f $(if ($hasDir) { "present" } else { "not there" }))
Say ("game data in $($GameAlt): {0}" -f $(if ($hasAlt) { "present" } else { "not there" }))
Say "kept: /sdcard/BSI (saves, settings, driver, logs)"

if ($DryRun) {
  Say ""
  Say "Dry run: nothing was changed. A real run would remove:"
  if ($installed) { Say "  the app" }
  if ($hasDir) { Say "  $AppDir (with the game data copied there)" }
  if ($GameData -and $hasAlt) { Say "  $GameAlt" }
  exit 0
}

if ($installed) {
  Nat shell am force-stop $Package | Out-Null
  $o = Nat uninstall $Package
  if (-not ("$o" -match "Success")) { Fail ("adb uninstall failed:`n" + ($o -join "`n")) }
  Say "app removed"
}
Start-Sleep -Seconds 2
if (Exists $AppDir) {
  # Android normally deletes this with the app. A copy left behind by an earlier install belongs to the old app and
  # stops a new install from reading its own game data, so remove it.
  Say "removing the leftover app folder (with the game data in it; this can take a minute)"
  Nat shell "rm -rf '$AppDir'" | Out-Null
  if (Exists $AppDir) { Fail "Could not remove $AppDir. Restart the headset and run this again." }
}
Say "app folder removed"
if ($GameData -and $hasAlt) {
  Say "removing $GameAlt (this can take a minute)"
  Nat shell "rm -rf '$GameAlt'" | Out-Null
  if (Exists $GameAlt) { Fail "Could not remove $GameAlt." }
  Say "game data removed"
}
Say ""
Say "Done. Your saves and settings in /sdcard/BSI are still there. Run Install.bat for a fresh install."
exit 0
