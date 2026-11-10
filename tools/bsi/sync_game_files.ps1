<#
  sync_game_files.ps1 - keep a LOCAL copy of the BioShock Infinite game files inside BSI-Quest so they are always there.
  Source (READ-ONLY, never modified): the Steam install.  Target: BSI-Quest\game_local\  (stays on this PC - never upload/publish it).
  Usage (PowerShell):
    powershell -ExecutionPolicy Bypass -File tools\bsi\sync_game_files.ps1            # exe + binaries only (small, default)
    powershell -ExecutionPolicy Bypass -File tools\bsi\sync_game_files.ps1 -All       # whole game install (large)
  Re-running only copies files that changed. A manifest with SHA-256 hashes is written to game_local\MANIFEST.txt.
#>
param(
  [string]$Source = "C:\Program Files (x86)\Steam\steamapps\common\BioShock Infinite",
  [string]$Target = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "game_local"),
  [switch]$All
)
$ErrorActionPreference = "Stop"
if (-not (Test-Path $Source)) { throw "Game install not found: $Source (pass -Source <path>)" }
New-Item -ItemType Directory -Force -Path $Target | Out-Null

# robocopy: /E subfolders, /XO skip older, /R:1 /W:1 quick retry, /NFL /NDL quiet. Copies FROM Steam TO game_local only.
if ($All) {
  robocopy "$Source" "$Target" /E /XO /R:1 /W:1 /NFL /NDL /NP | Out-Host
} else {
  robocopy "$Source\Binaries" "$Target\Binaries" /E /XO /R:1 /W:1 /NFL /NDL /NP | Out-Host
}
if ($LASTEXITCODE -ge 8) { throw "robocopy failed with code $LASTEXITCODE" }

# Manifest: path, size, SHA-256 (so tools can verify they work on the same exe as before)
$manifest = Join-Path $Target "MANIFEST.txt"
"# game_local manifest - $(Get-Date -Format s) - source: $Source" | Set-Content $manifest
Get-ChildItem -Path $Target -Recurse -File | Where-Object { $_.Name -ne "MANIFEST.txt" } | ForEach-Object {
  $h = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
  "{0}`t{1}`t{2}" -f $_.FullName.Substring($Target.Length + 1), $_.Length, $h
} | Add-Content $manifest
Write-Host "Done. Local copy: $Target  (manifest: $manifest). This folder must never be uploaded or published."
