# Pacer A/B matrix (plans/2026-11-02-study-dlssg-pacer.md, S2): runs build\repro_test.exe against one f5low.dll for every
# generator x multiplier x scenario x PaceLead setting, and collects what the DLL logs into results.csv and results.md.
# GPU + a window in front: nothing else may use the GPU or the foreground while it runs (REPRO_FRONT=1).
#
#   powershell -File tests\pacer_ab.ps1 -Dll build\f5low.dll                      # the whole matrix (hours)
#   powershell -File tests\pacer_ab.ps1 -Dll build\f5low.dll -Gens own -Mults 6 -Scenarios steady30,hitch50 -Configs off,auto,lead3 -Seconds 30
#   powershell -File tests\pacer_ab.ps1 -Dll build\f5low.dll -DryRun               # only list the runs and their environment
#
# -Gens       own, dlssg. PaceLead (-Configs) acts in the own generator's pacer only; dlssg runs once per cell with `off`.
# -Mults      2, 4, 6 (REPRO_INTERP = mult - 1).
# -Scenarios  steady30, steady60, ramp30-60 (30 -> 60 fps over half the run), hitch50 (60 fps, one 50 ms frame in the middle),
#             capstep (60 fps, 30 fps from the middle), jitter2 (60 fps, +-2 ms per frame).
# -Configs    PaceLead: off (XEFG_CUDA_PACELEAD=0: one whole step, as before), auto (the default, no variable), lead3 (fixed 3 ms),
#             lead5 (fixed 5 ms).
# -Seconds    seconds of frame generation per run (each run also has ~2-4 s off before it and the DLL's start-up).
# -Path / -Gate  the own generator's run path and Gate=: the clock pacer runs on -Path cuda (Show) and with -Gate 1 (release thread);
#             -Path d3d12 -Gate 0 (default) uses the schedule pacer, the only one PaceLead acts in.
# -DlssgPath  folder with Streamline + nvngx_dlssg (XEFG_CUDA_DLSSGPATH) for -Gens dlssg.
# PresentMon (-PresentMon <exe>, default H:\Tools\PresentMon\PresentMon-2.6.0-x64.exe, else presentmon on PATH) is used when found, if
# the session may start an ETW trace (administrator or the Performance Log Users group); -NoPresentMon skips it. It runs with
# --stop_existing_session: any other PresentMon/ETW session of the machine is ended by every run. Needs Python for the analysis (python -I tests\pacer_stats.py).
#
# Output (-Out, default build\pacer_ab\<stamp>): runs\<id>\{f5low.log, repro.out.txt, presentmon.csv}, results.jsonl, results.csv,
# results.md. Every run is analysed as soon as it ends (python -I tests\pacer_stats.py run ...); `report <Out>` rebuilds the tables.
param([Parameter(Mandatory)][string]$Dll,
      [string]$Out = '',
      [string[]]$Gens = @('own', 'dlssg'),
      [int[]]$Mults = @(2, 4, 6),
      [string[]]$Scenarios = @('steady30', 'steady60', 'ramp30-60', 'hitch50', 'capstep', 'jitter2'),
      [string[]]$Configs = @('off', 'auto'),
      [int]$Seconds = 40,
      [int]$Repeat = 1,
      [string]$Path = 'd3d12',
      [int]$Gate = 0,
      [string]$DlssgPath = '',
      [string]$Python = 'python',
      [string]$PresentMon = 'H:\Tools\PresentMon\PresentMon-2.6.0-x64.exe',
      [switch]$NoPresentMon,
      [switch]$DryRun)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root 'build\repro_test.exe'
if (-not (Test-Path $exe)) { throw 'build\repro_test.exe missing: run tests\build_repro_test.ps1' }
$Dll = (Resolve-Path $Dll).Path
if (-not $Out) { $Out = Join-Path $root ('build\pacer_ab\' + (Get-Date -Format 'yyyyMMdd_HHmmss')) }
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$stats = Join-Path $root 'tests\pacer_stats.py'
$log = Join-Path (Split-Path -Parent $Dll) 'f5low.log'
$fbLog = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'f5low\f5low.log' } else { '' }

$pm = $null
if (-not $NoPresentMon) {
    $pm = @($PresentMon, (Get-Command presentmon -ErrorAction SilentlyContinue).Source) |
        Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}

$cfgEnv = @{
    off   = @{ XEFG_CUDA_PACELEAD = '0' }
    auto  = @{}
    lead3 = @{ XEFG_CUDA_PACELEAD = '3' }
    lead5 = @{ XEFG_CUDA_PACELEAD = '5' }
}
# app frames with generation on, and the scenario's REPRO_* variables, for -Seconds S
function Scenario([string]$name, [int]$s) {
    switch ($name) {
        'steady30'  { return @{ ON = 30 * $s; REPRO_FPS = '30' } }
        'steady60'  { return @{ ON = 60 * $s; REPRO_FPS = '60' } }
        'ramp30-60' { return @{ ON = [int](52.5 * $s); REPRO_FPS = '30'; REPRO_RAMP = "30,60,$($s / 2)" } }
        'hitch50'   { return @{ ON = 60 * $s; REPRO_FPS = '60'; REPRO_HITCH = "$(30 * $s),50" } }
        'capstep'   { return @{ ON = 45 * $s; REPRO_FPS = '60'; REPRO_CAPSTEP = "$(30 * $s),30" } }
        'jitter2'   { return @{ ON = 60 * $s; REPRO_FPS = '60'; REPRO_JITTER = '2' } }
        default     { throw "unknown scenario $name" }
    }
}
$clear = 'XEFG_CUDA_PACELEAD', 'REPRO_TIMER1', 'XEFG_CUDA_PACELOG',
         'REPRO_FPS', 'REPRO_RAMP', 'REPRO_HITCH', 'REPRO_CAPSTEP', 'REPRO_JITTER', 'REPRO_ON', 'REPRO_OFF', 'REPRO_INTERP', 'REPRO_FRONT',
         'REPRO_PACELOG', 'REPRO_BAR', 'XEFG_CUDA_LOG', 'XEFG_CUDA_PATH', 'XEFG_CUDA_GATE', 'XEFG_CUDA_PROF', 'F5LOW_GENERATOR',
         'F5LOW_SINK_STATS', 'SINK_PROBE', 'XEFG_CUDA_DLSSGPATH'

$runs = @()
foreach ($g in $Gens) { foreach ($sc in $Scenarios) { foreach ($m in $Mults) {
    foreach ($c in $(if ($g -eq 'own') { $Configs } else { @('off') })) {
        for ($r = 1; $r -le $Repeat; ++$r) { $runs += [pscustomobject]@{ Gen = $g; Scenario = $sc; Mult = $m; Config = $c; Rep = $r } }
    } } } }
Write-Host ("{0} runs, about {1:N0} minutes; PresentMon: {2}" -f $runs.Count, ($runs.Count * ($Seconds + 12) / 60.0), $(if ($pm) { $pm } else { 'not used' }))

$n = 0
foreach ($run in $runs) {
    ++$n
    $id = '{0:D3}_{1}_{2}_{3}x_{4}{5}' -f $n, $run.Gen, $run.Scenario, $run.Mult, $run.Config, $(if ($Repeat -gt 1) { "_r$($run.Rep)" } else { '' })
    $dir = Join-Path $Out "runs\$id"
    $sc = Scenario $run.Scenario $Seconds
    $env_ = @{ REPRO_TIMER1 = '1'; XEFG_CUDA_LOG = '1'; XEFG_CUDA_PACELOG = '1'; XEFG_CUDA_PATH = $Path; XEFG_CUDA_GATE = "$Gate"; XEFG_CUDA_PROF = '0'
               F5LOW_GENERATOR = $run.Gen; REPRO_FRONT = '1'; REPRO_PACELOG = '1'; REPRO_BAR = '1'; REPRO_OFF = '60'
               REPRO_ON = "$($sc.ON)"; REPRO_INTERP = "$($run.Mult - 1)" }
    foreach ($k in $sc.Keys) { if ($k -ne 'ON') { $env_[$k] = $sc[$k] } }
    foreach ($k in $cfgEnv[$run.Config].Keys) { $env_[$k] = $cfgEnv[$run.Config][$k] }
    if ($run.Gen -eq 'dlssg') {
        $env_['F5LOW_SINK_STATS'] = '1'; $env_['SINK_PROBE'] = '1'
        if ($DlssgPath) { $env_['XEFG_CUDA_DLSSGPATH'] = $DlssgPath }
    }
    Write-Host ("[{0}/{1}] {2}" -f $n, $runs.Count, $id)
    if ($DryRun) { ($env_.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) -join ' ' | Write-Host; continue }
    New-Item -ItemType Directory -Force $dir | Out-Null
    foreach ($k in $clear) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $env_.Keys) { Set-Item "env:$k" $env_[$k] }
    Remove-Item $log -ErrorAction SilentlyContinue
    if ($fbLog) { Remove-Item $fbLog -ErrorAction SilentlyContinue }
    $pmProc = $null
    if ($pm) {
        $pmArgs = @('--process_name', 'repro_test.exe', '--output_file', "`"$dir\presentmon.csv`"", '--stop_existing_session', '--v2_metrics',
                    '--no_console_stats', '--terminate_on_proc_exit')
        $pmProc = Start-Process -FilePath $pm -ArgumentList $pmArgs -PassThru -WindowStyle Hidden -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 1500
    }
    $p = Start-Process -FilePath $exe -ArgumentList @("`"$Dll`"") -PassThru -NoNewWindow -RedirectStandardOutput "$dir\repro.out.txt" -RedirectStandardError "$dir\repro.err.txt"
    if (-not $p.WaitForExit(($Seconds + 150) * 1000)) { $p.Kill(); Write-Warning "$id timed out, killed" }
    $code = if ($p.HasExited) { $p.ExitCode } else { -1 }
    if ($pmProc) { if (-not $pmProc.WaitForExit(15000)) { $pmProc.Kill() } }
    if (Test-Path $log) { Copy-Item $log "$dir\f5low.log" -Force } elseif ($fbLog -and (Test-Path $fbLog)) { Copy-Item $fbLog "$dir\f5low.log" -Force }
    $line = & $Python -I $stats run $dir --gen $run.Gen --mult $run.Mult --skip 60 --meta "scenario=$($run.Scenario)" --meta "config=$($run.Config)" `
        --meta "exit=$code" --meta "id=$id"
    if ($LASTEXITCODE -ne 0 -or -not $line) { Write-Warning "$id : analysis failed"; continue }
    Add-Content -Path (Join-Path $Out 'results.jsonl') -Value $line -Encoding ascii
    if (-not (Select-String -Path "$dir\f5low.log" -SimpleMatch 'pace lead:' -Quiet -ErrorAction SilentlyContinue) -and $run.Gen -eq 'own') {
        Write-Warning "$id : no 'pace lead:' line in the log - an older f5low.dll, or no own-generator context"
    }
}
foreach ($k in $clear) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
if (-not $DryRun -and (Test-Path (Join-Path $Out 'results.jsonl'))) { & $Python -I $stats report $Out }
