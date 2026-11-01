# Same pictures on screen, same speed? Runs build\repro_test.exe against one f5low.dll (with a pack: embedded or
# xefg_pack\ next to it) on both run paths (d3d12, cuda) and writes, into -Out:
#   hashes.txt      SHA-256 of every picture presented for app frame -Frame (XEFG_CUDA_DUMP_SHOWN), per path
#   <path>.log      the DLL's f5low.log of that run (Prof=2: GPU time per app frame)
# Compare two builds: diff the two hashes.txt (must be equal) and the logs' per-frame times.
#   powershell -File tests\shown_check.ps1 -Dll build\base\f5low.dll -Out build\check_base
param([Parameter(Mandatory)][string]$Dll, [Parameter(Mandatory)][string]$Out, [int]$Interp = 2, [int]$Frame = 200,
      [string[]]$Paths = @('d3d12', 'cuda'))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root 'build\repro_test.exe'
if (-not (Test-Path $exe)) { throw 'build\repro_test.exe missing: run tests\build_repro_test.ps1' }
$Dll = (Resolve-Path $Dll).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$log = Join-Path (Split-Path -Parent $Dll) 'f5low.log'
$lines = @()
foreach ($p in $Paths) {
    Get-ChildItem $Out -Filter "$p.k*.bin" -ErrorAction SilentlyContinue | Remove-Item
    Remove-Item $log -ErrorAction SilentlyContinue
    $env:XEFG_CUDA_PATH = $p
    $env:XEFG_CUDA_LOG = '1'
    $env:XEFG_CUDA_PROF = '2'
    $env:XEFG_CUDA_DUMP_SHOWN = Join-Path $Out $p
    $env:XEFG_CUDA_DUMP_FRAME = "$Frame"
    $env:REPRO_INTERP = "$Interp"
    $env:REPRO_OFF = '60'
    $env:REPRO_ON = '360'
    & $exe $Dll | Out-Null
    $code = $LASTEXITCODE
    if (Test-Path $log) { Copy-Item $log (Join-Path $Out "$p.log") -Force }
    $dumps = Get-ChildItem $Out -Filter "$p.k*.bin" | Sort-Object Name
    if ($code -ne 0 -or -not $dumps) { $lines += "$p FAILED (exit $code, $($dumps.Count) dumps)"; continue }
    foreach ($d in $dumps) {
        $lines += "$p $($d.Name) $((Get-FileHash $d.FullName -Algorithm SHA256).Hash.ToLower())"
        Remove-Item $d.FullName
    }
}
'XEFG_CUDA_PATH','XEFG_CUDA_LOG','XEFG_CUDA_PROF','XEFG_CUDA_DUMP_SHOWN','XEFG_CUDA_DUMP_FRAME','REPRO_INTERP','REPRO_OFF','REPRO_ON' |
    ForEach-Object { Remove-Item "env:$_" -ErrorAction SilentlyContinue }
Set-Content -Path (Join-Path $Out 'hashes.txt') -Value $lines -Encoding ascii
$lines
