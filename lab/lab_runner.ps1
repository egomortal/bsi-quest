<#
  BSI-Quest lab runner (version 1, 19 Oct 2026)
  ---------------------------------------------
  Runs lab jobs that Claude drops into BSI-Quest\lab\inbox\ as JSON files.
  Each job is a list of steps from a FIXED set (see LAB.md). There is no
  "run any command" step: the runner only does adb file transfers inside
  /sdcard/BSI/, game start/stop, the kit's own build commands, APK installs
  from build\bsi_b0_v091\artifacts\, waits and measurements.

  Start:  double-click Start-Lab.bat (or allow autostart with Install-Autostart.bat)
  Stop:   double-click Stop-Lab.bat (creates lab\STOP), or close the window.
  Pause:  create lab\PAUSE (no new jobs start until it is deleted).

  Works with Windows PowerShell 5.1 (built into Windows) and PowerShell 7.
#>

$ErrorActionPreference = 'Stop'
$RunnerVersion = 11   # v11: gpu_detailed (-e/-d) + gpu_trace (render-stage trace) steps; v10: save_spot writes a NEW copy named with the highest sequence number + today's date (Continue picks by name, not mtime: j0012-j0021 all loaded TC_Gondola _52); recovery jobs written with an old time so they run first; v9: recovery job for a job left in running\ by a closed/crashed runner (use_outbox), console QuickEdit off; v8: gpu_counters (ovrgpuprofiler realtime metrics); v7: probe, save_spot/restore_saves (per-spot checkpoint saves), simpleperf, keep_awake between jobs (+ idle watchdog); v6: Wi-Fi reconnect + retry, cooldown waits on missing readings, device check before launch; v5: monitor screenshots (screencap_every_sec, JPEG 1032 wide), guardian pause step; v4: progress lines during long steps, lab_config.json re-read per job; v2: wider adb search, waits instead of crashing; v3: Wi-Fi adb (connect), -s serial pinning

$LabDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root    = Split-Path -Parent $LabDir           # ...\BSI-Quest
$Inbox   = Join-Path $LabDir 'inbox'
$Running = Join-Path $LabDir 'running'
$Done    = Join-Path $LabDir 'done'
$Outbox  = Join-Path $LabDir 'outbox'
$StatusFile = Join-Path $LabDir 'runner_status.json'
$RunnerLog  = Join-Path $LabDir 'runner.log'
$LockFile   = Join-Path $LabDir 'runner.lock'
$StopFile   = Join-Path $LabDir 'STOP'
$PauseFile  = Join-Path $LabDir 'PAUSE'
$InstallHistory = Join-Path $LabDir 'install_history.txt'
$AwakeFile  = Join-Path $LabDir 'awake_held.txt'   # v7: proximity override left on by a keep_awake job (survives a runner restart)
$KitBuildDir = Join-Path $Root 'build\bsi_b0_v091'
$ArtifactsDir = Join-Path $KitBuildDir 'artifacts'

foreach ($d in @($Inbox, $Running, $Done, $Outbox)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}

# ---------------------------------------------------------------- config
$Config = [ordered]@{
    adb              = ''                       # empty = auto-detect (SideQuest's adb first)
    connect          = ''                       # Wi-Fi adb address, e.g. 192.168.x.x:5555 (adb connect when no device is visible)
    python           = 'python'
    ndk              = 'C:\android-ndk-r27c'
    build_jobs       = 6
    package          = 'com.bsiquest.game'
    activity         = 'android.app.NativeActivity'
    allow_install    = $true                    # user's standing OK, 19 Oct 2026 (lab jobs only)
    allow_build      = $true
    kill_foreign_adb = $false                   # true = taskkill other adb servers before a job
    poll_sec         = 5
    default_job_timeout_min = 90
    max_battery_temp_c = 44                     # monitor aborts a job above this
    keep_awake_idle_min = 20                    # v7: a job with keep_awake leaves the proximity override on for the next job; released after this many idle minutes
}
$ConfigFile = Join-Path $LabDir 'lab_config.json'
if (Test-Path $ConfigFile) {
    $cj = Get-Content -Raw -Path $ConfigFile | ConvertFrom-Json
    foreach ($p in $cj.PSObject.Properties) { $Config[$p.Name] = $p.Value }
}

# ---------------------------------------------------------------- helpers
$Utf8NoBom = New-Object System.Text.UTF8Encoding $false

function Now-Iso { (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss') }

function Write-RunnerLog([string]$msg) {
    $line = "$(Now-Iso) $msg"
    Write-Host $line
    [System.IO.File]::AppendAllText($RunnerLog, $line + "`r`n", $Utf8NoBom)
    if ($script:JobLog) { [System.IO.File]::AppendAllText($script:JobLog, $line + "`r`n", $Utf8NoBom) }
}

function Get-Prop($obj, [string]$name, $default = $null) {
    if ($null -eq $obj) { return $default }
    $p = $obj.PSObject.Properties[$name]
    if ($null -eq $p -or $null -eq $p.Value) { return $default }
    return $p.Value
}

function Write-JsonFile([string]$path, $obj) {
    $tmp = "$path.tmp"
    [System.IO.File]::WriteAllText($tmp, ($obj | ConvertTo-Json -Depth 8), $Utf8NoBom)
    Move-Item -Force -Path $tmp -Destination $path
}

$script:State = [ordered]@{
    runner_version = $RunnerVersion; pid = $PID; state = 'starting'; job = $null; step = $null
    step_index = $null; step_started = $null; adb = $null; device = $null; game_version = $null
    note = $null; last_job = $null; last_job_result = $null; updated = $null; started = (Now-Iso)
}
function Update-Status([hashtable]$changes = @{}) {
    foreach ($k in $changes.Keys) { $script:State[$k] = $changes[$k] }
    # v4: show progress of long steps in the window (once a minute), not only when a step ends
    if ($script:State.note -and $script:State.state -eq 'running') {
        $n = Get-Date
        if (-not $script:LastEcho -or ($n - $script:LastEcho).TotalSeconds -ge 60) {
            Write-Host ("$(Now-Iso)     ... $($script:State.step): $($script:State.note)"); $script:LastEcho = $n }
    }
    $script:State.updated = Now-Iso
    try { Write-JsonFile $StatusFile $script:State } catch { }
}

function Quote-Arg([string]$s) {
    if ($s -eq '') { return '""' }
    if ($s -notmatch '[\s"]') { return $s }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $bs = 0
    foreach ($ch in $s.ToCharArray()) {
        if ($ch -eq '\') { $bs++; continue }
        if ($ch -eq '"') { [void]$sb.Append('\' * ($bs * 2 + 1)); [void]$sb.Append('"'); $bs = 0; continue }
        if ($bs) { [void]$sb.Append('\' * $bs); $bs = 0 }
        [void]$sb.Append($ch)
    }
    [void]$sb.Append('\' * ($bs * 2)); [void]$sb.Append('"')
    return $sb.ToString()
}

class AbortJob : System.Exception { AbortJob([string]$m) : base($m) {} }

function Check-Abort {
    if (-not $script:InAlways -and (Test-Path $StopFile)) { throw [AbortJob]::new('STOP file present') }
    if ($script:JobDeadline -and (Get-Date) -gt $script:JobDeadline) { throw [AbortJob]::new('job timeout reached') }
}

# Runs a program with a timeout. Keeps the heartbeat fresh and honours STOP.
function Invoke-Exe {
    param([string]$Exe, [string[]]$Argv, [int]$TimeoutSec = 120, [string]$StdoutFile = $null,
          [string]$StderrFile = $null, [string]$WorkDir = $null, [switch]$NoAbortCheck)
    if (-not $Exe) { throw 'internal: empty program path' }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (($Argv | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($WorkDir) { $psi.WorkingDirectory = $WorkDir }
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $fsOut = $null; $fsErr = $null
    if ($StdoutFile) { $fsOut = [System.IO.File]::Create($StdoutFile); $outTask = $p.StandardOutput.BaseStream.CopyToAsync($fsOut) }
    else { $outTask = $p.StandardOutput.ReadToEndAsync() }
    if ($StderrFile) { $fsErr = [System.IO.File]::Create($StderrFile); $errTask = $p.StandardError.BaseStream.CopyToAsync($fsErr) }
    else { $errTask = $p.StandardError.ReadToEndAsync() }
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $finished = $false; $aborted = $null
    while ($true) {
        if ($p.WaitForExit(2000)) { $finished = $true; break }
        if ((Get-Date) -gt $deadline) { break }
        Update-Status
        if (-not $NoAbortCheck) { try { Check-Abort } catch { $aborted = $_; break } }
    }
    if (-not $finished) { try { $p.Kill() } catch { }; [void]$p.WaitForExit(5000) }
    try { [void]$outTask.Wait(15000) } catch { }
    try { [void]$errTask.Wait(15000) } catch { }
    if ($fsOut) { $fsOut.Close() }
    if ($fsErr) { $fsErr.Close() }
    if ($aborted) { throw $aborted }
    $out = ''; $err = ''
    if (-not $StdoutFile -and $outTask.IsCompleted) { $out = $outTask.Result }
    if (-not $StderrFile -and $errTask.IsCompleted) { $err = $errTask.Result }
    $code = -999
    if ($finished) { $code = $p.ExitCode }
    return [pscustomobject]@{ Code = $code; Out = $out; Err = $err; TimedOut = (-not $finished) }
}

# ---------------------------------------------------------------- adb
function Find-Adb {
    # Returns the full path of adb.exe or throws a message that lists every place looked at.
    $tried = @()
    $cfg = [string]$Config.adb
    if ($cfg.Trim() -ne '') {
        if (Test-Path -PathType Leaf $cfg) { return (Resolve-Path $cfg).Path }
        throw "lab_config.json adb path not found: $cfg"
    }
    $cands = @()
    foreach ($base in @($env:APPDATA, $env:LOCALAPPDATA, $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:USERPROFILE)) {
        if (-not $base) { continue }
        $cands += (Join-Path $base 'SideQuest\platform-tools\adb.exe')
        $cands += (Join-Path $base 'SideQuest\resources\platform-tools\adb.exe')   # SideQuest desktop installer (found on the user's PC, 19 Oct)
        $cands += (Join-Path $base 'Programs\SideQuest\resources\app.asar.unpacked\build\platform-tools\adb.exe')
        $cands += (Join-Path $base 'SideQuest\resources\app.asar.unpacked\build\platform-tools\adb.exe')
        $cands += (Join-Path $base 'Android\Sdk\platform-tools\adb.exe')
        $cands += (Join-Path $base 'Meta Quest Developer Hub\resources\bin\adb.exe')
        $cands += (Join-Path $base 'platform-tools\adb.exe')
    }
    $cands += @('C:\platform-tools\adb.exe', 'C:\adb\adb.exe', 'C:\Android\platform-tools\adb.exe')
    foreach ($c in $cands) {
        $tried += $c
        if (Test-Path -PathType Leaf $c) { return [string]$c }
    }
    $g = @(Get-Command adb.exe -CommandType Application -ErrorAction SilentlyContinue)
    foreach ($x in $g) { $p = [string]$x.Path; if (-not $p) { $p = [string]$x.Definition }; if ($p -and (Test-Path -PathType Leaf $p)) { return $p } }
    $tried += 'PATH (Get-Command adb.exe)'
    try {
        $w = @(& where.exe adb.exe 2>$null)
        foreach ($p in $w) { if ($p -and (Test-Path -PathType Leaf $p)) { return [string]$p } }
    } catch { }
    $tried += 'where.exe adb.exe'
    throw ("adb.exe not found. Looked in:`r`n  " + ($tried -join "`r`n  ") + "`r`nPut its full path into lab\lab_config.json, e.g. `"adb`": `"C:\\path\\to\\adb.exe`"")
}

function Adb([string[]]$Argv, [int]$TimeoutSec = 60, [string]$StdoutFile = $null) {
    # one Quest can show up twice (USB + Wi-Fi): pin every device command to the chosen serial
    $plain = $Argv
    if ($script:Serial -and $Argv.Count -and @('devices', 'connect', 'disconnect', 'start-server', 'kill-server') -notcontains $Argv[0]) { $Argv = @('-s', $script:Serial) + $Argv }
    $r = Invoke-Exe -Exe $script:AdbPath -Argv $Argv -TimeoutSec $TimeoutSec -StdoutFile $StdoutFile
    # v6: a Wi-Fi Quest drops off adb when it sleeps: reconnect once and retry
    if ($plain.Count -and @('devices', 'connect', 'disconnect', 'start-server', 'kill-server') -notcontains $plain[0] -and ([string]$Config.connect).Trim() -ne '' -and
        (($r.Err + $r.Out) -match "device '.*' not found|device offline|no devices/emulators found|device not found")) {
        $c = Invoke-Exe -Exe $script:AdbPath -Argv @('connect', ([string]$Config.connect).Trim()) -TimeoutSec 30
        Write-RunnerLog "  adb lost the Quest; reconnect: $(($c.Out + $c.Err).Trim())"
        Start-Sleep -Seconds 3
        $r = Invoke-Exe -Exe $script:AdbPath -Argv $Argv -TimeoutSec $TimeoutSec -StdoutFile $StdoutFile
    }
    return $r
}

function Adb-Shell([string[]]$Argv, [int]$TimeoutSec = 30) {
    return Adb (@('shell') + $Argv) $TimeoutSec
}

function Get-ForeignAdb {
    $mine = $script:AdbPath
    $list = @()
    foreach ($p in @(Get-Process -Name adb -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $p.Path } catch { }
        if ($path -and ($path -ne $mine)) { $list += $path }
    }
    return $list
}

function Get-DeviceState {
    $r = Adb @('devices') 20
    $devs = @()
    foreach ($l in @($r.Out -split "`r?`n" | Where-Object { $_ -match '\t' })) { $p = $l -split '\t'; $devs += @{ serial = $p[0].Trim(); state = $p[1].Trim() } }
    if ($devs.Count -eq 0 -and ([string]$Config.connect).Trim() -ne '') {
        $c = Adb @('connect', ([string]$Config.connect).Trim()) 30
        $msg = "adb connect $($Config.connect): $(($c.Out + $c.Err).Trim())"
        if ($msg -ne $script:LastConnectMsg) { Write-RunnerLog "  $msg"; $script:LastConnectMsg = $msg }
        $r = Adb @('devices') 20
        foreach ($l in @($r.Out -split "`r?`n" | Where-Object { $_ -match '\t' })) { $p = $l -split '\t'; $devs += @{ serial = $p[0].Trim(); state = $p[1].Trim() } }
    }
    if ($devs.Count -eq 0) { return @{ state = 'none'; serial = $null; count = 0 } }
    # prefer a ready USB device (no ':' in the serial), then a ready Wi-Fi one, then whatever is there
    $pick = @($devs | Where-Object { $_.state -eq 'device' -and $_.serial -notmatch ':' }) + @($devs | Where-Object { $_.state -eq 'device' }) + $devs
    return @{ state = $pick[0].state; serial = $pick[0].serial; count = $devs.Count }
}

function Get-GameVersion {
    $r = Adb-Shell @('dumpsys', 'package', $Config.package) 30
    if ($r.Out -match 'versionCode=(\d+)') { return [int]$Matches[1] }
    return $null
}

function Test-GameAlive {
    $r = Adb-Shell @('pidof', $Config.package) 15
    return ($r.Code -eq 0 -and $r.Out.Trim() -ne '')
}

function Get-Vitals {
    $v = [ordered]@{ battery_pct = $null; battery_temp_c = $null; charging = $null; thermal_status = $null; gpu_mhz = $null }
    $b = Adb-Shell @('dumpsys', 'battery') 20
    if ($b.Out -match '(?m)^\s*level:\s*(\d+)') { $v.battery_pct = [int]$Matches[1] }
    if ($b.Out -match '(?m)^\s*temperature:\s*(\d+)') { $v.battery_temp_c = [double]$Matches[1] / 10.0 }
    if ($b.Out -match '(?m)^\s*status:\s*(\d+)') { $v.charging = ([int]$Matches[1] -eq 2 -or [int]$Matches[1] -eq 5) }
    $t = Adb-Shell @('dumpsys', 'thermalservice') 20
    if ($t.Out -match 'Thermal Status:\s*(\d+)') { $v.thermal_status = [int]$Matches[1] }
    $g = Adb-Shell @('cat', '/sys/class/kgsl/kgsl-3d0/gpuclk') 10
    if ($g.Code -eq 0 -and $g.Out.Trim() -match '^\d+$') { $v.gpu_mhz = [int]([long]$g.Out.Trim() / 1000000) }
    return $v
}

# ---------------------------------------------------------------- path rules
$DevicePathRx = '^/sdcard/BSI(/[A-Za-z0-9._\-]+)*/?$'
function Assert-DevicePath([string]$p) {
    if ($p -notmatch $DevicePathRx -or $p -match '\.\.') { throw "device path not allowed (only /sdcard/BSI/...): $p" }
    return $p
}
function Resolve-PcPath([string]$p, [string]$mustBeUnder = $Root) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $Root $p }
    $full = [System.IO.Path]::GetFullPath($p)
    $base = [System.IO.Path]::GetFullPath($mustBeUnder).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($base, [System.StringComparison]::OrdinalIgnoreCase)) { throw "PC path outside $mustBeUnder : $p" }
    return $full
}
function Resolve-OutPath([string]$rel) {
    if (-not $rel) { throw 'missing dst' }
    if ([System.IO.Path]::IsPathRooted($rel) -or $rel -match '\.\.') { throw "dst must be a plain name inside the job's outbox folder: $rel" }
    $full = Join-Path $script:JobOut $rel
    $dir = Split-Path -Parent $full
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    return $full
}

# ---------------------------------------------------------------- steps
function Step-Devices($s) {
    $timeout = [int](Get-Prop $s 'timeout_sec' 120)
    $until = (Get-Date).AddSeconds($timeout)
    while ($true) {
        $d = Get-DeviceState
        if ($d.state -eq 'device') {
            $script:Serial = $d.serial
            $script:State.device = $d.serial
            $model = (Adb-Shell @('getprop', 'ro.product.model') 15).Out.Trim()
            $script:State.game_version = Get-GameVersion
            return "device $($d.serial) ($model), game versionCode $($script:State.game_version)"
        }
        if ($d.state -eq 'unauthorized') { Update-Status @{ note = 'Quest says unauthorized: put it on and allow USB debugging (always allow)' } }
        else { Update-Status @{ note = "waiting for the Quest on USB (adb state: $($d.state))" } }
        if ((Get-Date) -gt $until) { throw "no Quest in adb state 'device' after $timeout s (last: $($d.state))" }
        Check-Abort
        Start-Sleep -Seconds 3
    }
}

function Step-Info($s) {
    $v = Get-Vitals
    $ver = Get-GameVersion
    $df = (Adb-Shell @('df', '-h', '/sdcard') 15).Out.Trim()
    $info = [ordered]@{ time = Now-Iso; game_version = $ver; vitals = $v; sdcard = $df; alive = (Test-GameAlive) }
    Write-JsonFile (Resolve-OutPath (Get-Prop $s 'dst' 'info.json')) $info
    return "version $ver, battery $($v.battery_pct)% $($v.battery_temp_c) C, thermal $($v.thermal_status), gpu $($v.gpu_mhz) MHz"
}

function Step-StopGame($s) {
    $r = Adb-Shell @('am', 'force-stop', $Config.package) 30
    Start-Sleep -Seconds 2
    return "force-stop (code $($r.Code))"
}

function Step-Launch($s) {
    $d = Get-DeviceState
    if ($d.state -ne 'device') { throw "the Quest is not reachable over adb before launch (state $($d.state)); on Wi-Fi it drops off when it sleeps: use USB" }
    $r = Adb-Shell @('am', 'start', '-n', "$($Config.package)/$($Config.activity)") 30
    if ($r.Out -match 'Error') { throw "am start: $($r.Out.Trim())" }
    $until = (Get-Date).AddSeconds([int](Get-Prop $s 'timeout_sec' 30))
    while ((Get-Date) -lt $until) {
        if (Test-GameAlive) { $script:GameStarted = Get-Date; return 'game process running' }
        Start-Sleep -Seconds 2
    }
    throw 'game process did not appear after am start'
}

function Step-Wait($s) {
    $sec = [int](Get-Prop $s 'sec' 10)
    $requireGame = [bool](Get-Prop $s 'require_game' $false)
    $until = (Get-Date).AddSeconds($sec)
    $lastCheck = Get-Date
    while ((Get-Date) -lt $until) {
        Check-Abort; Update-Status
        Start-Sleep -Seconds ([Math]::Min(5, [Math]::Max(1, ($until - (Get-Date)).TotalSeconds)))
        if ($requireGame -and ((Get-Date) - $lastCheck).TotalSeconds -ge 20) {
            $lastCheck = Get-Date
            if (-not (Test-GameAlive)) { throw 'game process exited during wait' }
        }
    }
    return "waited $sec s"
}

function Step-Push($s) {
    $src = Resolve-PcPath (Get-Prop $s 'src')
    $dst = Assert-DevicePath (Get-Prop $s 'dst')
    if (-not (Test-Path -PathType Leaf $src)) { throw "push source missing: $src" }
    $r = Adb @('push', $src, $dst) 300
    if ($r.Code -ne 0) { throw "adb push failed: $($r.Err.Trim()) $($r.Out.Trim())" }
    return "pushed $(Split-Path -Leaf $src) -> $dst"
}

function Step-PushText($s) {
    $dst = Assert-DevicePath (Get-Prop $s 'dst')
    $t = Get-Prop $s 'text' ''
    if ($t -is [array]) { $t = ($t -join "`n") }
    $t = ([string]$t) -replace "`r`n", "`n"
    if (-not $t.EndsWith("`n")) { $t += "`n" }
    $tmp = Join-Path $script:JobOut ('pushed_' + (Split-Path -Leaf $dst))
    [System.IO.File]::WriteAllText($tmp, $t, $Utf8NoBom)          # LF, no BOM (the game's parsers want plain text)
    $r = Adb @('push', $tmp, $dst) 60
    if ($r.Code -ne 0) { throw "adb push failed: $($r.Err.Trim())" }
    return "wrote $dst ($($t.Length) bytes, copy kept in outbox)"
}

function Step-Rm($s) {
    $paths = @(Get-Prop $s 'path' @())
    $done = @()
    foreach ($p in $paths) {
        Assert-DevicePath $p | Out-Null
        if ($p -match '[\*\?]' -or $p -match '^/sdcard/BSI/?$') { throw "rm refuses wildcards / the BSI folder itself: $p" }
        $r = Adb-Shell @('rm', '-f', $p) 20
        $done += $p
    }
    return "removed (if present): $($done -join ', ')"
}

function Step-Pull($s) {
    $src = Assert-DevicePath (Get-Prop $s 'src')
    $dst = Resolve-OutPath (Get-Prop $s 'dst' (Split-Path -Leaf $src))
    $r = Adb @('pull', $src, $dst) 600
    if ($r.Code -ne 0) { throw "adb pull failed: $($r.Err.Trim()) $($r.Out.Trim())" }
    $size = 0; if (Test-Path -PathType Leaf $dst) { $size = (Get-Item $dst).Length }
    return "pulled $src -> $(Split-Path -Leaf $dst) ($size bytes)"
}

function Step-PullDir($s) {
    $src = Assert-DevicePath (Get-Prop $s 'src')
    $dst = Resolve-OutPath (Get-Prop $s 'dst' (Split-Path -Leaf $src.TrimEnd('/')))
    if (Test-Path $dst) { throw "pull_dir target exists: $dst" }
    $r = Adb @('pull', $src, $dst) 900
    if ($r.Code -ne 0) { throw "adb pull failed: $($r.Err.Trim()) $($r.Out.Trim())" }
    return "pulled folder $src"
}

function Backup-Saves([string]$why) {
    $dst = Join-Path $script:JobOut ('save_backup_' + (Get-Date).ToString('yyyyMMdd_HHmmss'))
    $r = Adb @('pull', '/sdcard/BSI/user', $dst) 300
    if ($r.Code -ne 0) { throw "save backup failed (nothing installed): $($r.Err.Trim()) $($r.Out.Trim())" }
    $n = @(Get-ChildItem -Recurse -File $dst -ErrorAction SilentlyContinue).Count
    Write-RunnerLog "  save backup ($why): $n files -> $dst"
    return $n
}

function Step-BackupSaves($s) { $n = Backup-Saves 'step'; $script:SavesBackedUp = $true; return "save backup: $n files" }

function Test-KitMd5([string]$sumsName) {
    $sums = Resolve-PcPath (Join-Path 'build\bsi_b0_v091' $sumsName) $KitBuildDir
    if (-not (Test-Path $sums)) { throw "checksum file missing: $sums" }
    $bad = @(); $n = 0
    foreach ($line in Get-Content $sums) {
        if ($line -notmatch '^([0-9a-fA-F]{32})\s+\*?(.+)$') { continue }
        $want = $Matches[1].ToLower(); $rel = $Matches[2].Trim() -replace '^\./', '' -replace '/', '\'
        $f = Join-Path (Join-Path $KitBuildDir 'kit') $rel
        $n++
        if (-not (Test-Path -PathType Leaf $f)) { $bad += "$rel (missing)"; continue }
        $got = (Get-FileHash -Algorithm MD5 -Path $f).Hash.ToLower()
        if ($got -ne $want) { $bad += "$rel ($got)" }
    }
    if ($bad.Count) { throw "kit checksum mismatch: $($bad -join '; ')" }
    return $n
}

function Step-Build($s) {
    if (-not $Config.allow_build) { throw 'builds are disabled in lab_config.json (allow_build)' }
    $ver = [int](Get-Prop $s 'version')
    if ($ver -lt 77 -or $ver -gt 9999) { throw "bad version $ver" }
    $flavor = Get-Prop $s 'flavor' 'vr'
    if ($flavor -ne 'vr' -and $flavor -ne 'flat') { throw "flavor must be vr or flat" }
    $tag = ('b1n_{0:D3}' -f $ver) + ($(if ($flavor -eq 'vr') { 'vr' } else { '' }))
    $script_ = $(if ($flavor -eq 'vr') { 'build_b1n_vr_link.py' } else { 'build_b1n_link.py' })
    $verify = Get-Prop $s 'verify' $null
    if ($verify) { $n = Test-KitMd5 $verify; Write-RunnerLog "  kit checksums OK ($n files, $verify)" }
    $apkDir = Join-Path $ArtifactsDir $tag
    $apk = Join-Path $apkDir 'bsi_b0probe.apk'
    if ((Test-Path $apk) -and -not [bool](Get-Prop $s 'rebuild' $false)) {
        $script:LastApk = $apk
        return "APK already built, reused: $apk"
    }
    # both scripts insist on NEW output folders: pick the first free suffix
    $outName = "out_$tag"; $artName = $tag; $k = 2
    while ((Test-Path (Join-Path $KitBuildDir "kit\$outName")) -or (Test-Path (Join-Path $ArtifactsDir $artName))) {
        $outName = "out_${tag}_r$k"; $artName = "${tag}_r$k"; $k++
    }
    $logA = Resolve-OutPath "build_$outName.log"
    $errA = Resolve-OutPath "build_$outName.err.log"
    Update-Status @{ note = "building $outName (several minutes)" }
    $r = Invoke-Exe -Exe $Config.python -Argv @(".\kit\$script_", '--ndk', $Config.ndk, '--output-dir', $outName, '--jobs', "$($Config.build_jobs)") `
        -TimeoutSec 5400 -StdoutFile $logA -StderrFile $errA -WorkDir $KitBuildDir
    if ($r.Code -ne 0) { throw "link script failed (code $($r.Code)), see $(Split-Path -Leaf $logA)" }
    $logB = Resolve-OutPath "apk_$artName.log"
    $errB = Resolve-OutPath "apk_$artName.err.log"
    $r = Invoke-Exe -Exe $Config.python -Argv @('.\kit\build_b0_apk.py', '--base-apk', '.\base_v091.apk', '--lib-dir', ".\kit\$outName\lib",
        '--output-dir', ".\artifacts\$artName", '--version-code', "$ver", '--key', '.\kit\key\bsiquest_key.pem') `
        -TimeoutSec 1200 -StdoutFile $logB -StderrFile $errB -WorkDir $KitBuildDir
    $apk = Join-Path (Join-Path $ArtifactsDir $artName) 'bsi_b0probe.apk'
    if ($r.Code -ne 0 -or -not (Test-Path $apk)) { throw "APK script failed (code $($r.Code)), see $(Split-Path -Leaf $logB)" }
    $script:LastApk = $apk
    $mb = [Math]::Round((Get-Item $apk).Length / 1MB, 1)
    return "built $artName ($mb MB)"
}

function Step-Install($s) {
    if (-not $Config.allow_install) { throw 'installs are disabled in lab_config.json (allow_install)' }
    $apkArg = Get-Prop $s 'apk' '@last_build'
    if ($apkArg -eq '@last_build') { if (-not $script:LastApk) { throw 'no build in this job to install' }; $apk = $script:LastApk }
    else { $apk = Resolve-PcPath $apkArg $ArtifactsDir }
    if (-not (Test-Path -PathType Leaf $apk)) { throw "APK missing: $apk" }
    $want = Get-Prop $s 'expect_version' $null
    $before = Get-GameVersion
    if ([bool](Get-Prop $s 'backup_saves' $true)) { Backup-Saves "before installing over versionCode $before" | Out-Null }
    Adb-Shell @('am', 'force-stop', $Config.package) 30 | Out-Null
    $r = Adb @('install', '-r', $apk) 600
    if ($r.Out -notmatch 'Success') { throw "adb install failed: $($r.Out.Trim()) $($r.Err.Trim())" }
    $after = Get-GameVersion
    $line = "$(Now-Iso) job=$($script:JobId) versionCode $before -> $after apk=$apk"
    [System.IO.File]::AppendAllText($InstallHistory, $line + "`r`n", $Utf8NoBom)
    $script:State.game_version = $after
    if ($want -and ([int]$want -ne $after)) { throw "installed, but versionCode is $after (expected $want)" }
    return "installed: versionCode $before -> $after"
}

function Read-DeviceLog([string]$devPath, [string]$tmpName) {
    $tmp = Join-Path $script:JobOut $tmpName
    if (Test-Path $tmp) { Remove-Item -Force $tmp }
    $r = Adb @('pull', $devPath, $tmp) 120
    if ($r.Code -ne 0 -or -not (Test-Path $tmp)) { return $null }
    return [System.IO.File]::ReadAllText($tmp)
}

function Step-WaitLog($s) {
    $file = Assert-DevicePath (Get-Prop $s 'file' '/sdcard/BSI/bsi.log')
    $rx = Get-Prop $s 'regex' $null
    $lit = Get-Prop $s 'text' $null
    if (-not $rx -and -not $lit) { throw 'wait_log needs "text" or "regex"' }
    if (-not $rx) { $rx = [regex]::Escape($lit) }
    $failRx = @(Get-Prop $s 'fail_regex' @())
    $timeout = [int](Get-Prop $s 'timeout_sec' 300)
    $poll = [int](Get-Prop $s 'poll_sec' ([Math]::Max(5, $Config.poll_sec)))
    $requireGame = [bool](Get-Prop $s 'require_game' $true)
    $until = (Get-Date).AddSeconds($timeout)
    while ($true) {
        Check-Abort; Update-Status
        $txt = Read-DeviceLog $file '_wait_log_tmp.txt'
        if ($txt) {
            $m = [regex]::Match($txt, $rx, 'Multiline')
            if ($m.Success) { return "found: $($m.Value.Trim())" }
            foreach ($f in $failRx) {
                $fm = [regex]::Match($txt, $f, 'Multiline')
                if ($fm.Success) { throw "fail pattern seen: $($fm.Value.Trim())" }
            }
        }
        if ($requireGame -and -not (Test-GameAlive)) { throw 'game exited before the pattern appeared' }
        if ((Get-Date) -gt $until) { throw "pattern not seen within $timeout s: $rx" }
        Start-Sleep -Seconds $poll
    }
}

function Step-WaitExit($s) {
    $timeout = [int](Get-Prop $s 'timeout_sec' 600)
    $until = (Get-Date).AddSeconds($timeout)
    while (Test-GameAlive) {
        Check-Abort; Update-Status
        if ((Get-Date) -gt $until) {
            if ([bool](Get-Prop $s 'force_stop_on_timeout' $true)) { Adb-Shell @('am', 'force-stop', $Config.package) 30 | Out-Null; return "game still running after $timeout s: force-stopped" }
            throw "game still running after $timeout s"
        }
        Start-Sleep -Seconds 5
    }
    return 'game exited'
}

function Step-Monitor($s) {
    $sec = [int](Get-Prop $s 'sec' 60)
    $every = [int](Get-Prop $s 'every_sec' 30)
    $requireGame = [bool](Get-Prop $s 'require_game' $true)
    $untilExit = [bool](Get-Prop $s 'until_exit' $false)
    $maxT = [double](Get-Prop $s 'max_battery_temp_c' $Config.max_battery_temp_c)
    $csv = Resolve-OutPath (Get-Prop $s 'dst' 'monitor.csv')
    if (-not (Test-Path $csv)) { [System.IO.File]::WriteAllText($csv, "time,battery_pct,battery_temp_c,charging,thermal_status,gpu_mhz,game_alive`r`n", $Utf8NoBom) }
    $until = (Get-Date).AddSeconds($sec)
    $samples = 0; $maxSeen = 0
    $shotEvery = [int](Get-Prop $s 'screencap_every_sec' 0); $shotMax = [int](Get-Prop $s 'screencap_max' 40); $shots = 0; $nextShot = Get-Date
    $t0 = Get-Date
    while ($true) {
        Check-Abort
        if ($shotEvery -gt 0 -and $shots -lt $shotMax -and (Get-Date) -ge $nextShot) {
            $name = 'shot_{0:D4}s.jpg' -f [int]((Get-Date) - $t0).TotalSeconds
            Save-Screen (Resolve-OutPath $name) | Out-Null; $shots++
            $nextShot = (Get-Date).AddSeconds($shotEvery)
        }
        $v = Get-Vitals
        $alive = Test-GameAlive
        $samples++
        if ($v.battery_temp_c -gt $maxSeen) { $maxSeen = $v.battery_temp_c }
        [System.IO.File]::AppendAllText($csv, "$(Now-Iso),$($v.battery_pct),$($v.battery_temp_c),$($v.charging),$($v.thermal_status),$($v.gpu_mhz),$alive`r`n", $Utf8NoBom)
        Update-Status @{ note = "monitor: battery $($v.battery_pct)% $($v.battery_temp_c) C thermal $($v.thermal_status) game $alive" }
        if (-not $alive) {
            if ($untilExit) { return "game exited after $samples samples (max battery $maxSeen C)" }
            if ($requireGame) { throw 'game process exited during monitor' }
        }
        if ($v.battery_temp_c -and $v.battery_temp_c -gt $maxT) { throw "battery $($v.battery_temp_c) C above limit $maxT C" }
        if ((Get-Date) -gt $until) { return "$samples samples, max battery $maxSeen C" }
        $next = (Get-Date).AddSeconds($every)
        while ((Get-Date) -lt $next -and (Get-Date) -lt $until) {
            Check-Abort; Update-Status; Start-Sleep -Seconds 2
            if ($shotEvery -gt 0 -and $shots -lt $shotMax -and (Get-Date) -ge $nextShot) { break }
        }
    }
}

function Step-Cooldown($s) {
    $maxC = [double](Get-Prop $s 'max_c' 36)
    $timeout = [int](Get-Prop $s 'timeout_sec' 1800)
    $until = (Get-Date).AddSeconds($timeout)
    while ($true) {
        Check-Abort
        $v = Get-Vitals
        Update-Status @{ note = "cooldown: battery $($v.battery_temp_c) C (target <= $maxC)" }
        if ($null -eq $v.battery_temp_c) { Update-Status @{ note = 'cooldown: no reading from the Quest (asleep / Wi-Fi adb dropped?), waiting' } }
        elseif ($v.battery_temp_c -le $maxC) { return "battery $($v.battery_temp_c) C" }
        if ((Get-Date) -gt $until) { throw "still $($v.battery_temp_c) C after $timeout s" }
        Start-Sleep -Seconds 20
    }
}

function Step-Proximity($s) {
    $mode = Get-Prop $s 'mode' 'off'
    if ($mode -eq 'off') {
        # pretend the headset is worn: display stays on, app keeps focus with nobody inside
        Adb-Shell @('am', 'broadcast', '-a', 'com.oculus.vrpowermanager.prox_close') 20 | Out-Null
        Adb-Shell @('input', 'keyevent', 'KEYCODE_WAKEUP') 20 | Out-Null
        $script:ProxOff = $true; $script:ProxTouched = $true
        return 'proximity sensor overridden (prox_close) + wake'
    }
    Adb-Shell @('am', 'broadcast', '-a', 'com.oculus.vrpowermanager.automation_disable') 20 | Out-Null
    $script:ProxOff = $false; $script:ProxTouched = $true
    return 'proximity sensor back to normal (automation_disable)'
}

function Step-Guardian($s) {
    $mode = Get-Prop $s 'mode' 'pause'
    $v = $(if ($mode -eq 'pause') { '1' } else { '0' })
    Adb-Shell @('setprop', 'debug.oculus.guardian_pause', $v) 20 | Out-Null
    $now = (Adb-Shell @('getprop', 'debug.oculus.guardian_pause') 20).Out.Trim()
    $script:GuardianPaused = ($v -eq '1')
    return "debug.oculus.guardian_pause = $now"
}

function Step-Logcat($s) {
    if ([bool](Get-Prop $s 'clear' $false)) { Adb @('logcat', '-c') 20 | Out-Null; return 'logcat cleared' }
    $dst = Resolve-OutPath (Get-Prop $s 'dst' 'logcat.txt')
    $lines = [int](Get-Prop $s 'lines' 20000)
    $r = Adb @('logcat', '-d', '-t', "$lines") 120 $dst
    return "logcat -> $(Split-Path -Leaf $dst)"
}

function Save-Screen([string]$dst, [int]$width = 1032) {
    $raw = "$dst.raw.png"
    Adb @('exec-out', 'screencap', '-p') 60 $raw | Out-Null
    if (-not (Test-Path $raw) -or (Get-Item $raw).Length -lt 100) { Remove-Item -Force $raw -ErrorAction SilentlyContinue; return 0 }
    # the System.Drawing code is compiled only when this block runs, so a missing System.Drawing is caught here
    $conv = [scriptblock]::Create(@'
param($raw, $dst, $width)
Add-Type -AssemblyName System.Drawing -ErrorAction Stop
$img = [System.Drawing.Image]::FromFile($raw)
$h = [int]($img.Height * $width / $img.Width)
$bmp = New-Object System.Drawing.Bitmap $width, $h
$g = [System.Drawing.Graphics]::FromImage($bmp); $g.InterpolationMode = 'HighQualityBicubic'; $g.DrawImage($img, 0, 0, $width, $h); $g.Dispose(); $img.Dispose()
$bmp.Save($dst, [System.Drawing.Imaging.ImageFormat]::Jpeg); $bmp.Dispose()
'@)
    try { & $conv $raw $dst $width; Remove-Item -Force $raw }
    catch { if (Test-Path $dst) { Remove-Item -Force $dst -ErrorAction SilentlyContinue }; Move-Item -Force $raw ([System.IO.Path]::ChangeExtension($dst, '.png')) }
    return 1
}

function Step-Screencap($s) {
    $dst = Resolve-OutPath (Get-Prop $s 'dst' ('screen_' + (Get-Date).ToString('HHmmss') + '.jpg'))
    $ok = Save-Screen $dst
    return "screencap $(if ($ok) { 'saved' } else { 'empty' })"
}

function Step-Note($s) { return [string](Get-Prop $s 'text' '') }

# ---------------------------------------------------------------- v7: probe, per-spot saves, simpleperf
# probe: a FIXED list of read-only commands (plus one scratch file inside /sdcard/BSI for the touch test)
function Step-Probe($s) {
    $out = Resolve-OutPath (Get-Prop $s 'dst' 'probe.txt')
    $cmds = @(
        @('getprop', 'ro.build.version.release'), @('getprop', 'ro.build.fingerprint'), @('getprop', 'ro.product.model'),
        @('toybox', '--version'), @('which', 'simpleperf'), @('simpleperf', '--version'), @('simpleperf', 'list', 'hw'),
        @('which', 'ovrgpuprofiler'), @('ovrgpuprofiler', '--help'), @('ovrgpuprofiler', '-h'), @('ovrgpuprofiler', '-m'),
        @('cat', '/sys/devices/system/cpu/possible'), @('cat', '/sys/devices/system/cpu/online'),
        @('ls', '/sys/devices/system/cpu/cpufreq'),
        @('cat', '/sys/devices/system/cpu/cpufreq/policy0/related_cpus', '/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq', '/sys/devices/system/cpu/cpufreq/policy0/cpuinfo_max_freq'),
        @('cat', '/sys/devices/system/cpu/cpufreq/policy3/related_cpus', '/sys/devices/system/cpu/cpufreq/policy3/scaling_cur_freq', '/sys/devices/system/cpu/cpufreq/policy3/cpuinfo_max_freq'),
        @('cat', '/sys/devices/system/cpu/cpufreq/policy5/related_cpus', '/sys/devices/system/cpu/cpufreq/policy5/scaling_cur_freq', '/sys/devices/system/cpu/cpufreq/policy5/cpuinfo_max_freq'),
        @('cat', '/sys/devices/system/cpu/cpufreq/policy7/related_cpus', '/sys/devices/system/cpu/cpufreq/policy7/scaling_cur_freq', '/sys/devices/system/cpu/cpufreq/policy7/cpuinfo_max_freq'),
        @('cat', '/sys/class/kgsl/kgsl-3d0/gpuclk', '/sys/class/kgsl/kgsl-3d0/max_gpuclk', '/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage'),
        @('ls', '/sys/class/kgsl/kgsl-3d0'),
        @('getprop', 'debug.oculus.cpuLevel'), @('getprop', 'debug.oculus.gpuLevel'),
        @('ls', '-la', '/sdcard/BSI'), @('ls', '-la', '/sdcard/BSI/user/cloud'),
        @('stat', '-c', "'%Y %s %n'", '/sdcard/BSI/user/cloud/*'),
        @('date', '+%s'), @('date', '+%z'),
        @('pidof', $Config.package), @('df', '-h', '/data/local/tmp')
    )
    $sb = New-Object System.Text.StringBuilder
    foreach ($c in $cmds) {
        $r = Adb-Shell $c 60
        [void]$sb.Append("===== $($c -join ' ')  (code $($r.Code))`n$($r.Out)$($r.Err)`n")
    }
    # touch test on a scratch file: can toybox touch set an exact epoch (needed to restore save times)?
    $scratch = '/sdcard/BSI/lab_touch_test.txt'
    Adb-Shell @('touch', $scratch) 20 | Out-Null
    $t1 = Adb-Shell @('touch', '-d', '@1700000000', $scratch) 20
    $st1 = (Adb-Shell @('stat', '-c', '%Y', $scratch) 20).Out.Trim()
    $t2 = Adb-Shell @('TZ=UTC', 'touch', '-t', '202311142213.20', $scratch) 20
    $st2 = (Adb-Shell @('stat', '-c', '%Y', $scratch) 20).Out.Trim()
    Adb-Shell @('rm', '-f', $scratch) 20 | Out-Null
    [void]$sb.Append("===== touch test: -d @1700000000 -> $st1 (code $($t1.Code) $($t1.Err.Trim())); TZ=UTC -t 202311142213.20 -> $st2 (want 1700000000, code $($t2.Code) $($t2.Err.Trim()))`n")
    [System.IO.File]::WriteAllText($out, ($sb.ToString() -replace "`r`n", "`n"), $Utf8NoBom)
    return "probe -> $(Split-Path -Leaf $out); touch @epoch -> $st1, TZ=UTC -t -> $st2"
}

$CloudDir = '/sdcard/BSI/user/cloud'
$SaveNameRx = '^[A-Za-z0-9_.%\-]+$'
function Get-CloudListing {
    # returns @{ top = @{ name = @{ mtime; size } }; deleted = @{ ... } }
    $res = @{ top = @{}; deleted = @{} }
    foreach ($pair in @(@('top', "$CloudDir/*"), @('deleted', "$CloudDir/.deleted/*"))) {
        $r = Adb-Shell @('stat', '-c', "'%Y %s %F %n'", $pair[1]) 60
        foreach ($l in @($r.Out -split "`r?`n")) {
            if ($l -notmatch '^(\d+) (\d+) (regular file|regular empty file) (.+)$') { continue }
            $mt = [long]$Matches[1]; $sz = [long]$Matches[2]; $leaf = Split-Path -Leaf ($Matches[4].Trim())   # read before the next -match overwrites $Matches
            if ($leaf -notmatch $SaveNameRx) { continue }
            $res[$pair[0]][$leaf] = @{ mtime = $mt; size = $sz }
        }
    }
    return $res
}
function Write-Listing([string]$path, $lst) {
    $lines = @()
    foreach ($k in ($lst.top.Keys | Sort-Object)) { $lines += "top $($lst.top[$k].mtime) $($lst.top[$k].size) $k" }
    foreach ($k in ($lst.deleted.Keys | Sort-Object)) { $lines += "deleted $($lst.deleted[$k].mtime) $($lst.deleted[$k].size) $k" }
    [System.IO.File]::WriteAllText($path, (($lines -join "`n") + "`n"), $Utf8NoBom)
}
function Set-DeviceMtime([string]$path, [long]$epoch) {
    if ($path -notmatch '^/sdcard/BSI/user/cloud/[A-Za-z0-9_.%\-]+$') { throw "internal: bad save path $path" }
    Adb-Shell @('touch', '-d', "@$epoch", $path) 20 | Out-Null
    $got = (Adb-Shell @('stat', '-c', '%Y', $path) 20).Out.Trim()
    if ($got -eq "$epoch") { return $true }
    $u = [DateTimeOffset]::FromUnixTimeSeconds($epoch).UtcDateTime.ToString('yyyyMMddHHmm.ss')
    Adb-Shell @('TZ=UTC', 'touch', '-t', $u, $path) 20 | Out-Null
    $got = (Adb-Shell @('stat', '-c', '%Y', $path) 20).Out.Trim()
    return ($got -eq "$epoch")
}
function Get-SaveSeq([string]$leaf) { if ($leaf -match '_(\d+)\.sav(\.\d+)?$') { return [int]$Matches[1] }; return -1 }

# save_spot: make the checkpoint save <spot> the one Continue loads (newest .sav by mtime). Uses the newest .sav of that
# checkpoint in cloud/, else copies the newest one out of cloud/.deleted/ (the game's own trash, kept by our Steam shim).
# Everything is recorded in the outbox so restore_saves (always) can put names and times back exactly.
function Step-SaveSpot($s) {
    $spot = [string](Get-Prop $s 'spot' '')
    if ($spot -notmatch '^[A-Za-z0-9_]+$') { throw "save_spot needs a checkpoint name like LizT or TC_Gondola: '$spot'" }
    if (Test-Path (Join-Path $script:JobOut 'save_spot_state.json')) { throw 'only one save_spot per job (its restore record would be overwritten)' }
    if (-not $script:SavesBackedUp) { Backup-Saves "before save_spot $spot" | Out-Null; $script:SavesBackedUp = $true }
    $before = Get-CloudListing
    if ($before.top.Count -eq 0) { throw "no files listed in $CloudDir (adb stat failed?)" }
    foreach ($k in $before.top.Keys) { if ($before.top[$k].mtime -lt 1000000000) { throw "listing of $CloudDir looks wrong (mtime $($before.top[$k].mtime) for $k)" } }
    Write-Listing (Resolve-OutPath 'saves_before.txt') $before
    $prefix = "SaveData%5C$spot-0_"
    # v10: Continue loads the save whose NAME carries the newest date / highest sequence number (the trailing _NN), not the
    # newest mtime. So the chosen save is copied to a new name with today's date and max sequence + 1; the original stays.
    $cands = @($before.top.Keys | Where-Object { $_.StartsWith($prefix) -and $_.EndsWith('.sav') } | Sort-Object { Get-SaveSeq $_ } -Descending)
    $dc = @($before.deleted.Keys | Where-Object { $_.StartsWith($prefix) -and $_ -match '\.sav\.\d+$' } | Sort-Object { Get-SaveSeq $_ } -Descending)
    $src = $null; $how = $null
    if ($cands.Count -and (-not $dc.Count -or (Get-SaveSeq $cands[0]) -ge (Get-SaveSeq $dc[0]))) { $src = "$CloudDir/$($cands[0])"; $how = "copy of $($cands[0])" }
    elseif ($dc.Count) { $src = "$CloudDir/.deleted/$($dc[0])"; $how = "copy of .deleted/$($dc[0])" }
    else { throw "no .sav for checkpoint $spot in cloud/ or cloud/.deleted/" }
    $maxSeq = 0; foreach ($k in @($before.top.Keys) + @($before.deleted.Keys)) { if ($k -match '_(\d+)\.(sav|bkm)(\.\d+)?$') { $q = [int]$Matches[1]; if ($q -gt $maxSeq) { $maxSeq = $q } } }
    [long]$now = 0; [long]::TryParse((Adb-Shell @('date', '+%s') 20).Out.Trim(), [ref]$now) | Out-Null
    $tz = (Adb-Shell @('date', '+%z') 20).Out.Trim(); $offMin = 0
    if ($tz -match '^([+-])(\d\d)(\d\d)$') { $offMin = ([int]$Matches[2] * 60 + [int]$Matches[3]) * $(if ($Matches[1] -eq '-') { -1 } else { 1 }) }
    $lt = [DateTimeOffset]::FromUnixTimeSeconds($now).UtcDateTime.AddMinutes($offMin)
    $target = "{0}{1}_{2}_{3}_{4}_{5}_{6}_{7}.sav" -f $prefix, $lt.Month, $lt.Day, $lt.Year, $lt.Hour, $lt.Minute, $lt.Second, ($maxSeq + 1)
    if ($target -notmatch $SaveNameRx -or $before.top.ContainsKey($target)) { throw "internal: bad target name $target" }
    $r = Adb-Shell @('cp', $src, "$CloudDir/$target") 60
    if ($r.Code -ne 0) { throw "cp failed: $($r.Err.Trim()) $($r.Out.Trim())" }
    $added = @($target); $how += " as sequence $($maxSeq + 1)"
    # newest .sav = target: one minute past the newest other .sav, and never older than the device clock
    $maxOther = 0
    foreach ($k in $before.top.Keys) { if ($k.EndsWith('.sav') -and $k -ne $target -and $before.top[$k].mtime -gt $maxOther) { $maxOther = $before.top[$k].mtime } }
    [long]$now = 0; [long]::TryParse((Adb-Shell @('date', '+%s') 20).Out.Trim(), [ref]$now) | Out-Null
    $want = [Math]::Max($now, $maxOther + 60)
    if (-not (Set-DeviceMtime "$CloudDir/$target" $want)) { throw "could not set the time of $target" }
    $state = [ordered]@{ spot = $spot; target = $target; how = $how; added = $added; set_mtime = $want
                         before_top = $before.top; before_deleted_names = @($before.deleted.Keys) }
    Write-JsonFile (Resolve-OutPath 'save_spot_state.json') $state
    $script:SaveSpotState = $state
    return "Continue -> $target ($how; mtime $want, newest other .sav $maxOther)"
}

# restore_saves: undo save_spot and anything the game did to cloud/ during the run (new saves are moved to
# /sdcard/BSI/lab_saves_trash/, files the game moved to .deleted/ come back, times are set back). Then compares.
function Step-RestoreSaves($s) {
    $sf = Join-Path $script:JobOut 'save_spot_state.json'
    if (-not (Test-Path $sf)) { return 'no save_spot in this job: nothing to restore' }
    $st = Get-Content -Raw $sf | ConvertFrom-Json
    $bt = @{}; foreach ($p in $st.before_top.PSObject.Properties) { $bt[$p.Name] = @{ mtime = [long]$p.Value.mtime; size = [long]$p.Value.size } }
    $bdel = @{}; foreach ($n in @($st.before_deleted_names)) { $bdel[[string]$n] = 1 }
    $now = Get-CloudListing
    $log = @()
    # 1. files that were not there before: ours (copies) are removed, the game's new ones are moved out of the save folder
    foreach ($k in @($now.top.Keys)) {
        if ($bt.ContainsKey($k)) { continue }
        if (@($st.added) -contains $k) { Adb-Shell @('rm', '-f', "$CloudDir/$k") 20 | Out-Null; $log += "removed lab copy $k" }
        else {
            Adb-Shell @('mkdir', '-p', '/sdcard/BSI/lab_saves_trash') 20 | Out-Null
            Adb-Shell @('mv', "$CloudDir/$k", "/sdcard/BSI/lab_saves_trash/$($script:JobId)_$k") 20 | Out-Null; $log += "moved new game file $k to lab_saves_trash"
        }
    }
    # 2. files the game deleted during the run (moved into .deleted/<name>.<time>): move them back
    foreach ($k in @($bt.Keys)) {
        if ($now.top.ContainsKey($k)) { continue }
        $back = @($now.deleted.Keys | Where-Object { -not $bdel.ContainsKey($_) -and $_.StartsWith("$k.") } | Sort-Object -Descending)
        if ($back.Count) { Adb-Shell @('mv', "$CloudDir/.deleted/$($back[0])", "$CloudDir/$k") 20 | Out-Null; $log += "restored $k from .deleted/$($back[0])" }
        else { $log += "MISSING $k (not in .deleted/ either; the job's save backup has it)" }
    }
    # 3. times back
    $now = Get-CloudListing
    foreach ($k in @($bt.Keys)) {
        if (-not $now.top.ContainsKey($k)) { continue }
        if ($now.top[$k].mtime -ne $bt[$k].mtime) {
            if (Set-DeviceMtime "$CloudDir/$k" $bt[$k].mtime) { $log += "time restored $k" } else { $log += "TIME NOT RESTORED $k" }
        }
    }
    # 4. compare
    $fin = Get-CloudListing
    Write-Listing (Resolve-OutPath 'saves_after.txt') $fin
    $diff = @()
    foreach ($k in $bt.Keys) {
        if (-not $fin.top.ContainsKey($k)) { $diff += "missing $k" }
        elseif ($fin.top[$k].mtime -ne $bt[$k].mtime -or $fin.top[$k].size -ne $bt[$k].size) { $diff += "changed $k" }
    }
    foreach ($k in $fin.top.Keys) { if (-not $bt.ContainsKey($k)) { $diff += "extra $k" } }
    [System.IO.File]::WriteAllText((Resolve-OutPath 'restore_saves.txt'), ((@($log) + @('---') + @($diff)) -join "`n"), $Utf8NoBom)
    if ($diff.Count) { throw "saves NOT exactly as before: $($diff -join '; ') (backup in the outbox)" }
    return "saves exactly as before ($($bt.Count) files; $($log.Count) actions)"
}

# gpu_counters: Meta's ovrgpuprofiler realtime GPU metrics (1 line per second) while the game runs; 'timeout' on the
# device ends it. metrics = list of ids from 'ovrgpuprofiler -m' (probe.txt).
function Step-GpuCounters($s) {
    $sec = [int](Get-Prop $s 'sec' 60); if ($sec -lt 2 -or $sec -gt 900) { throw 'gpu_counters sec 2..900' }
    $ids = @(Get-Prop $s 'metrics' @(1,2,3,4,8,9,10,11,12,13,14,15,17,18,19,20,22,23,25,26,32,33,35,36,39,40,41,64,65,67,70,80))
    foreach ($i in $ids) { if (-not ([string]$i -match '^\d{1,3}$')) { throw "bad metric id $i" } }
    $dst = Resolve-OutPath (Get-Prop $s 'dst' 'gpu_counters.txt')
    if (-not (Test-GameAlive)) { throw 'game not running' }
    Update-Status @{ note = "ovrgpuprofiler $sec s" }
    $r = Adb @('shell', 'timeout', "$sec", 'ovrgpuprofiler', ('--realtime=' + ($ids -join ','))) ($sec + 60) $dst
    $n = 0; if (Test-Path $dst) { $n = @(Get-Content $dst).Count }
    if ($n -lt 3) { throw "ovrgpuprofiler gave $n lines (code $($r.Code)) $($r.Err.Trim())" }
    return "ovrgpuprofiler $sec s, $($ids.Count) metrics -> $(Split-Path -Leaf $dst) ($n lines)"
}

# gpu_detailed (v11): ovrgpuprofiler -e <package> / -d = detailed profiling mode on the GPU driver (needed for render-stage
# traces; applies to apps started AFTER it). mode = on|off (default on). Put 'on' before launch and 'off' in always.
function Step-GpuDetailed($s) {
    $mode = [string](Get-Prop $s 'mode' 'on'); if (@('on', 'off') -notcontains $mode) { throw 'gpu_detailed mode on|off' }
    if ($mode -eq 'on') { $r = Adb-Shell @('ovrgpuprofiler', '-e', $Config.package) 60 } else { $r = Adb-Shell @('ovrgpuprofiler', '-d') 60 }
    $msg = (($r.Out + $r.Err).Trim() -replace '\s+', ' '); if ($msg.Length -gt 160) { $msg = $msg.Substring(0, 160) }
    return "ovrgpuprofiler detailed $mode (code $($r.Code)) $msg"
}

# gpu_trace (v11): ovrgpuprofiler -t <sec> [-s ids] = render-stage trace (every pass with GPU start/end timestamps, preemptions)
# of the running game, written to dst (default gpu_trace.txt). sec 0.1..10 (default 1). drawcall=1 adds -x (per draw).
function Step-GpuTrace($s) {
    $sec = [double](Get-Prop $s 'sec' 1); if ($sec -lt 0.05 -or $sec -gt 10) { throw 'gpu_trace sec 0.05..10' }
    $dst = Resolve-OutPath (Get-Prop $s 'dst' 'gpu_trace.txt')
    if (-not (Test-GameAlive)) { throw 'game not running' }
    $argv = @('ovrgpuprofiler', '-t', ([string]$sec))
    $ids = @(Get-Prop $s 'metrics' @())
    if ($ids.Count) { foreach ($i in $ids) { if (-not ([string]$i -match '^\d{1,3}$')) { throw "bad metric id $i" } }; $argv += ('--renderstage-metrics=' + ($ids -join ',')) }
    if ([int](Get-Prop $s 'drawcall' 0) -eq 1) { $argv += '-x' }
    if ([int](Get-Prop $s 'verbose' 0) -eq 1) { $argv += '-v' }
    Update-Status @{ note = "ovrgpuprofiler trace $sec s" }
    $r = Adb (@('shell') + $argv) 180 $dst
    $n = 0; if (Test-Path $dst) { $n = @(Get-Content $dst).Count }
    if ($n -lt 3) { throw "ovrgpuprofiler trace gave $n lines (code $($r.Code)) $($r.Err.Trim())" }
    return "render-stage trace $sec s -> $(Split-Path -Leaf $dst) ($n lines)"
}

# simpleperf: CPU samples of the running game (the APK is debuggable + profileable; libbsi.so keeps its symbols).
# Fixed device file /data/local/tmp/bsi_perf.data; reports go to the outbox.
function Step-Simpleperf($s) {
    $sec = [int](Get-Prop $s 'sec' 30); if ($sec -lt 1 -or $sec -gt 600) { throw "simpleperf sec 1..600" }
    $freq = [int](Get-Prop $s 'freq' 1000); if ($freq -lt 50 -or $freq -gt 10000) { throw "simpleperf freq 50..10000" }
    $cg = [string](Get-Prop $s 'callgraph' 'none'); if (@('none', 'fp') -notcontains $cg) { throw "callgraph none|fp" }
    $base = [string](Get-Prop $s 'dst' 'perf'); if ($base -notmatch '^[A-Za-z0-9_\-]+$') { throw "dst must be a plain name" }
    if (-not (Test-GameAlive)) { throw 'game not running' }
    $dev = '/data/local/tmp/bsi_perf.data'
    Adb-Shell @('rm', '-f', $dev) 20 | Out-Null
    $argv = @('simpleperf', 'record', '--app', $Config.package, '-e', 'cpu-clock', '-f', "$freq", '--duration', "$sec", '-o', $dev)
    if ($cg -eq 'fp') { $argv += @('--call-graph', 'fp') }
    Update-Status @{ note = "simpleperf recording $sec s" }
    $r = Adb-Shell $argv ($sec + 90)
    [System.IO.File]::WriteAllText((Resolve-OutPath "${base}_record.txt"), "$($r.Out)`n$($r.Err)", $Utf8NoBom)
    if ($r.Code -ne 0) { throw "simpleperf record failed (code $($r.Code)): $(($r.Err + $r.Out).Trim())" }
    $reports = @(
        @('thread', @('--sort', 'comm', '-n')),
        @('dso', @('--sort', 'comm,dso', '-n')),
        @('symbol', @('--sort', 'comm,dso,symbol', '-n', '--percent-limit', '0.02')),
        @('symbol_all', @('--sort', 'dso,symbol', '-n', '--percent-limit', '0.01'))
    )
    if ($cg -eq 'fp') { $reports += ,@('callers', @('--sort', 'symbol', '-g', 'caller', '--percent-limit', '0.5', '-n')) }
    foreach ($rep in $reports) {
        $f = Resolve-OutPath "${base}_$($rep[0]).txt"
        Adb (@('shell', 'simpleperf', 'report', '-i', $dev) + $rep[1]) 300 $f | Out-Null
    }
    if ([bool](Get-Prop $s 'pull_data' $true)) { Adb @('pull', $dev, (Resolve-OutPath "$base.data")) 300 | Out-Null }
    Adb-Shell @('rm', '-f', $dev) 20 | Out-Null
    return "simpleperf $sec s at $freq Hz ($cg) -> ${base}_*.txt"
}

$StepTable = @{
    'devices' = ${function:Step-Devices}; 'info' = ${function:Step-Info}; 'stop_game' = ${function:Step-StopGame}
    'launch' = ${function:Step-Launch}; 'wait' = ${function:Step-Wait}; 'push' = ${function:Step-Push}
    'push_text' = ${function:Step-PushText}; 'rm' = ${function:Step-Rm}; 'pull' = ${function:Step-Pull}
    'pull_dir' = ${function:Step-PullDir}; 'backup_saves' = ${function:Step-BackupSaves}; 'build' = ${function:Step-Build}
    'install' = ${function:Step-Install}; 'wait_log' = ${function:Step-WaitLog}; 'wait_exit' = ${function:Step-WaitExit}
    'monitor' = ${function:Step-Monitor}; 'cooldown' = ${function:Step-Cooldown}; 'proximity' = ${function:Step-Proximity}
    'logcat' = ${function:Step-Logcat}; 'guardian' = ${function:Step-Guardian}; 'screencap' = ${function:Step-Screencap}; 'note' = ${function:Step-Note}
    'probe' = ${function:Step-Probe}; 'save_spot' = ${function:Step-SaveSpot}; 'restore_saves' = ${function:Step-RestoreSaves}; 'simpleperf' = ${function:Step-Simpleperf}; 'gpu_counters' = ${function:Step-GpuCounters}; 'gpu_detailed' = ${function:Step-GpuDetailed}; 'gpu_trace' = ${function:Step-GpuTrace}
}

# ---------------------------------------------------------------- job
function Run-Steps($steps, [string]$phase, $result) {
    $i = 0
    foreach ($s in @($steps)) {
        $i++
        $do = Get-Prop $s 'do'
        $optional = [bool](Get-Prop $s 'optional' $false)
        $rec = [ordered]@{ phase = $phase; i = $i; do = $do; name = (Get-Prop $s 'name' $null); ok = $false; ms = 0; msg = $null; started = Now-Iso }
        $result.steps += $rec
        Update-Status @{ step = $do; step_index = "$phase $i/$(@($steps).Count)"; step_started = (Now-Iso); note = $null }
        $t0 = Get-Date
        try {
            if ($phase -eq 'main') { Check-Abort }
            if (-not $StepTable.ContainsKey($do)) { throw "unknown step '$do'" }
            $msg = & $StepTable[$do] $s
            $rec.ok = $true; $rec.msg = [string]$msg
            Write-RunnerLog "  [$phase $i] $do : $msg"
        } catch {
            $rec.msg = $_.Exception.Message
            Write-RunnerLog "  [$phase $i] $do FAILED: $($rec.msg)"
            $isAbort = $_.Exception -is [AbortJob]
            if ($phase -eq 'main' -and ($isAbort -or -not $optional)) {
                $rec.ms = [int]((Get-Date) - $t0).TotalMilliseconds
                Write-JsonFile (Join-Path $script:JobOut $script:StatusName) $result
                throw
            }
        }
        $rec.ms = [int]((Get-Date) - $t0).TotalMilliseconds
        Write-JsonFile (Join-Path $script:JobOut $script:StatusName) $result
    }
}

function Reload-Config {
    if (Test-Path $ConfigFile) {
        try { $cj = Get-Content -Raw -Path $ConfigFile | ConvertFrom-Json; foreach ($p in $cj.PSObject.Properties) { if ($p.Name -ne 'adb') { $Config[$p.Name] = $p.Value } } }
        catch { Write-RunnerLog "lab_config.json not readable, keeping the old settings: $($_.Exception.Message)" }
    }
}

function Run-Job([string]$file) {
    Reload-Config   # v4: edits to lab_config.json (limits, connect address) apply from the next job
    $name = [System.IO.Path]::GetFileNameWithoutExtension($file)
    $runPath = Join-Path $Running (Split-Path -Leaf $file)
    Move-Item -Force -Path $file -Destination $runPath
    $job = $null; $parseErr = $null
    try { $job = Get-Content -Raw -Path $runPath | ConvertFrom-Json } catch { $parseErr = $_.Exception.Message }
    $id = Get-Prop $job 'id' $name
    if ($id -notmatch '^[A-Za-z0-9_.\-]+$') { $id = $name }
    $script:JobId = $id
    $script:JobOut = Join-Path $Outbox $id
    $useOut = [string](Get-Prop $job 'use_outbox' '')   # v9: a recovery job works in the interrupted job's outbox (its save_spot_state.json)
    if ($useOut -match '^[A-Za-z0-9_.\-]+$' -and (Test-Path (Join-Path $Outbox $useOut))) { $script:JobOut = Join-Path $Outbox $useOut }
    else {
        if (Test-Path $script:JobOut) { $script:JobOut = Join-Path $Outbox ("$id" + '_' + (Get-Date).ToString('HHmmss')) }
        New-Item -ItemType Directory -Path $script:JobOut | Out-Null
    }
    $script:JobLog = Join-Path $script:JobOut 'runner.log'
    Copy-Item $runPath (Join-Path $script:JobOut $(if ($useOut) { "job_$id.json" } else { 'job.json' }))
    $script:LastApk = $null; $script:ProxOff = $false; $script:GuardianPaused = $false; $script:GameStarted = $null; $script:Serial = $null
    $script:SavesBackedUp = $false; $script:SaveSpotState = $null; $script:ProxTouched = $false   # AwakeHeldSince survives across jobs
    $tmin = [double](Get-Prop $job 'timeout_min' $Config.default_job_timeout_min)
    $script:JobDeadline = (Get-Date).AddMinutes($tmin)
    $script:StatusName = $(if ($useOut) { "status_$id.json" } else { 'status.json' })
    $result = [ordered]@{ id = $id; title = (Get-Prop $job 'title' ''); state = 'running'; started = Now-Iso; ended = $null
                          runner_version = $RunnerVersion; error = $null; steps = @() }
    Update-Status @{ state = 'running'; job = $id }
    Write-RunnerLog "JOB $id start: $($result.title) (timeout $tmin min)"
    try {
        if ($parseErr) { throw "job file is not valid JSON: $parseErr" }
        Run-Steps (Get-Prop $job 'steps' @()) 'main' $result
        $result.state = 'ok'
    } catch {
        $result.state = $(if ($_.Exception -is [AbortJob]) { 'aborted' } else { 'failed' })
        $result.error = $_.Exception.Message
    }
    # cleanup steps always run (with their own short deadline)
    $script:JobDeadline = (Get-Date).AddMinutes(15)
    $always = @(Get-Prop $job 'always' @())
    $keepAwake = [bool](Get-Prop $job 'keep_awake' $false)   # v7: the next job follows soon; Wi-Fi adb drops if the headset sleeps
    # v7: a job that used save_spot always puts the saves back, even if its always list forgot it
    if ((Test-Path (Join-Path $script:JobOut 'save_spot_state.json')) -and -not (@($always | Where-Object { (Get-Prop $_ 'do') -eq 'restore_saves' }).Count)) {
        $always = @([pscustomobject]@{ do = 'stop_game' }, [pscustomobject]@{ do = 'restore_saves' }) + $always
    }
    if ($script:ProxOff -and -not $keepAwake -and -not (@($always | Where-Object { (Get-Prop $_ 'do') -eq 'proximity' }).Count)) {
        $always += [pscustomobject]@{ do = 'proximity'; mode = 'on' }
    }
    if ($script:GuardianPaused -and -not (@($always | Where-Object { (Get-Prop $_ 'do') -eq 'guardian' }).Count)) {
        $always += [pscustomobject]@{ do = 'guardian'; mode = 'resume' }
    }
    $script:InAlways = $true
    try { Run-Steps $always 'always' $result } catch { }
    $script:InAlways = $false
    if ($script:ProxOff) { $script:AwakeHeldSince = Get-Date; try { Set-Content -Path $AwakeFile -Value (Now-Iso) } catch { }; Write-RunnerLog "  keep_awake: proximity override stays on for the next job (released after $($Config.keep_awake_idle_min) idle minutes)" }
    elseif ($script:ProxTouched) { $script:AwakeHeldSince = $null; Remove-Item -Force $AwakeFile -ErrorAction SilentlyContinue }   # this job put the sensor back
    elseif ($script:AwakeHeldSince) { $script:AwakeHeldSince = Get-Date }   # untouched: still held from an earlier job, idle clock restarts
    $result.ended = Now-Iso
    Write-JsonFile (Join-Path $script:JobOut $script:StatusName) $result
    Write-RunnerLog "JOB $id end: $($result.state) $($result.error)"
    Move-Item -Force -Path $runPath -Destination (Join-Path $Done (Split-Path -Leaf $runPath))
    Update-Status @{ state = 'idle'; job = $null; step = $null; step_index = $null; note = $null; last_job = $id; last_job_result = $result.state }
    $script:JobLog = $null; $script:JobDeadline = $null
}

# ---------------------------------------------------------------- main loop
if (Test-Path $LockFile) {
    $old = (Get-Content $LockFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($old -and (Get-Process -Id ([int]$old) -ErrorAction SilentlyContinue)) {
        Write-Host "Another lab runner is already running (PID $old). Close it first."; exit 1
    }
}
Set-Content -Path $LockFile -Value $PID
if (Test-Path $StopFile) { Remove-Item -Force $StopFile }

try {
    $script:AdbPath = $null
    while (-not $script:AdbPath) {
        try { $script:AdbPath = Find-Adb }
        catch {
            Write-RunnerLog $_.Exception.Message
            Update-Status @{ state = 'error'; note = 'adb.exe not found: set "adb" in lab\lab_config.json (the runner retries every 30 s and re-reads the file)' }
            for ($k = 0; $k -lt 15; $k++) { if (Test-Path $StopFile) { Write-RunnerLog 'STOP file seen while waiting for adb: runner exits'; return }; Start-Sleep -Seconds 2 }
            if (Test-Path $ConfigFile) { $cj = Get-Content -Raw -Path $ConfigFile | ConvertFrom-Json; foreach ($p in $cj.PSObject.Properties) { $Config[$p.Name] = $p.Value } }
        }
    }
    Update-Status @{ state = 'idle'; adb = $script:AdbPath }
    Write-RunnerLog "lab runner v$RunnerVersion started (PID $PID), adb: $($script:AdbPath)"
    # v9: a click into the console window (QuickEdit selection) froze the runner on its next output line (j0017, 19 Oct)
    try {
        Add-Type -Namespace BsiLab -Name Con -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern System.IntPtr GetStdHandle(int n);
[DllImport("kernel32.dll")] public static extern bool GetConsoleMode(System.IntPtr h, out uint m);
[DllImport("kernel32.dll")] public static extern bool SetConsoleMode(System.IntPtr h, uint m);
'@ -ErrorAction Stop
        $h = [BsiLab.Con]::GetStdHandle(-10); $m = 0
        if ([BsiLab.Con]::GetConsoleMode($h, [ref]$m)) { [void][BsiLab.Con]::SetConsoleMode($h, (($m -band (-bnot 0x40)) -bor 0x80)); Write-RunnerLog 'console QuickEdit off (a click into this window can no longer pause the runner)' }
    } catch { }
    # v9: jobs left in running\ (runner closed or crashed mid-job): queue a recovery job that runs their cleanup steps
    foreach ($f in @(Get-ChildItem -Path $Running -Filter '*.json' -File -ErrorAction SilentlyContinue)) {
        try {
            $oj = Get-Content -Raw $f.FullName | ConvertFrom-Json
            $oid = Get-Prop $oj 'id' $f.BaseName; if ($oid -notmatch '^[A-Za-z0-9_.\-]+$') { $oid = $f.BaseName }
            $steps = @([pscustomobject]@{ do = 'devices'; timeout_sec = 3600 }, [pscustomobject]@{ do = 'stop_game' }) + @(Get-Prop $oj 'always' @())
            if ((Test-Path (Join-Path (Join-Path $Outbox $oid) 'save_spot_state.json')) -and -not (@($steps | Where-Object { (Get-Prop $_ 'do') -eq 'restore_saves' }).Count)) { $steps += [pscustomobject]@{ do = 'restore_saves' } }
            $rj = [ordered]@{ id = "recover_$oid"; title = "Recovery: cleanup steps of $oid (interrupted when the runner stopped)"; timeout_min = 90; use_outbox = $oid
                              keep_awake = [bool](Get-Prop $oj 'keep_awake' $false); steps = $steps; always = @() }
            $rf = Join-Path $Inbox "a_recover_$oid.json"; Write-JsonFile $rf $rj
            (Get-Item $rf).LastWriteTime = (Get-Date).AddMinutes(-1)   # v10: old enough to be picked at once (the inbox skips files younger than 3 s)
            Move-Item -Force $f.FullName (Join-Path $Done ($f.BaseName + '.interrupted.json'))
            Write-RunnerLog "interrupted job $oid found in running\: recovery job a_recover_$oid queued first"
        } catch { Write-RunnerLog "could not queue recovery for $($f.Name): $($_.Exception.Message)" }
    }
    Write-RunnerLog "watching $Inbox  (Stop-Lab.bat or lab\STOP stops me; lab\PAUSE holds new jobs)"
    Adb @('start-server') 30 | Out-Null
    $lastBeat = Get-Date '2000-01-01'
    if (Test-Path $AwakeFile) { $script:AwakeHeldSince = (Get-Item $AwakeFile).LastWriteTime }   # v7: a restart keeps the watchdog
    $selfPath = $MyInvocation.MyCommand.Path; if (-not $selfPath) { $selfPath = Join-Path $LabDir 'lab_runner.ps1' }
    $selfHash = (Get-FileHash -Algorithm MD5 -Path (Join-Path $LabDir 'lab_runner.ps1')).Hash
    while (-not (Test-Path $StopFile)) {
        # v5: when Claude updates lab_runner.ps1, restart between jobs (Start-Lab.bat starts it again on exit code 3)
        $h = $null; try { $h = (Get-FileHash -Algorithm MD5 -Path (Join-Path $LabDir 'lab_runner.ps1')).Hash } catch { }
        if ($h -and $h -ne $selfHash) {
            Write-RunnerLog 'lab_runner.ps1 was updated: restarting with the new version'
            $script:Restart = $true; break
        }
        $jobs = @(Get-ChildItem -Path $Inbox -Filter '*.json' -File | Where-Object { ((Get-Date) - $_.LastWriteTime).TotalSeconds -ge 3 } | Sort-Object Name)
        if ($jobs.Count -and -not (Test-Path $PauseFile)) {
            $foreign = @(Get-ForeignAdb)
            if ($foreign.Count) {
                if ($Config.kill_foreign_adb) {
                    Write-RunnerLog "stopping other adb servers: $($foreign -join ', ')"
                    & taskkill /F /IM adb.exe | Out-Null
                    Start-Sleep -Seconds 2
                    Adb @('start-server') 30 | Out-Null
                } else {
                    Update-Status @{ state = 'waiting'; note = "another adb is running ($($foreign -join ', ')): close SideQuest/MQDH, or set kill_foreign_adb in lab_config.json" }
                    Start-Sleep -Seconds 10
                    continue
                }
            }
            Run-Job $jobs[0].FullName
            continue
        }
        if ($script:AwakeHeldSince -and ((Get-Date) - $script:AwakeHeldSince).TotalMinutes -ge [double]$Config.keep_awake_idle_min) {
            try { Adb-Shell @('am', 'broadcast', '-a', 'com.oculus.vrpowermanager.automation_disable') 20 | Out-Null; Write-RunnerLog 'keep_awake: no job for a while: proximity sensor back to normal' } catch { }
            $script:AwakeHeldSince = $null; Remove-Item -Force $AwakeFile -ErrorAction SilentlyContinue
        }
        if (((Get-Date) - $lastBeat).TotalSeconds -ge 30) {
            $d = $null
            try { $d = Get-DeviceState } catch { }
            $st = $(if (Test-Path $PauseFile) { 'paused' } else { 'idle' })
            $dev = $null; if ($d) { $dev = "$($d.serial) $($d.state)" }
            Update-Status @{ state = $st; device = $dev; note = $null }
            $lastBeat = Get-Date
        }
        Start-Sleep -Seconds 2
    }
    if (-not $script:Restart) { Write-RunnerLog 'STOP file seen: runner exits' }
} catch {
    Write-RunnerLog "runner crashed: $($_.Exception.Message)"
    Update-Status @{ state = 'crashed'; note = $_.Exception.Message }
    throw
} finally {
    if ($script:State.state -ne 'crashed') { Update-Status @{ state = $(if ($script:Restart) { 'restarting' } else { 'stopped' }) } }
    Remove-Item -Force $LockFile -ErrorAction SilentlyContinue
}
if ($script:Restart) { exit 3 }
