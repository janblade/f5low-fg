# Proof that f5low.dll writes nothing Intel-derived to disk, and that it removes what an older version left.
#
#   1. Run A: the repro (build\repro_test.exe, a real generation run) with f5low.dll in a fresh folder, a fresh LOCALAPPDATA, the
#      log on. Then every file under the DLL folder and under LOCALAPPDATA is scanned (tests\no_intel_on_disk.py): no DXBC
#      container, no file whose SHA-256 is one of Intel's modules / weights / constants (the recipe's and the pack's), no file
#      holding a 64-byte window of the pack's Intel-derived files, and no file at all but f5low.log (and the inputs: the DLL,
#      libxess_fg.dll, the test exe).
#   2. Self-test of the checker: pack_samples.exe writes a native module and a weight whole into a folder; scanning that folder
#      must FAIL for both (a checker that passes everything proves nothing).
#   3. Run B: old on-disk caches (the pack folders of format 2/3 with their `complete` marker, a stale unfinished build folder) are
#      planted in both cache roots next to things that are not ours (a folder without our marker, a file); after a run the old
#      ones are gone and the others are untouched.
#
#   powershell -File tests\no_intel_on_disk.ps1 [-Dll build\f5low.dll] [-Lib <libxess_fg.dll>] [-Work H:\Temp\f5low_nointel]
# Needs build\f5low.dll, build\repro_test.exe (tests\build_repro_test.ps1) and build\pack_samples.exe (tests\build_pack_samples.ps1).
param([string]$Dll = '', [string]$Lib = '', [string]$Work = 'H:\Temp\f5low_nointel')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Dll) { $Dll = Join-Path $root 'build\f5low.dll' }
if (-not $Lib) { $Lib = 'D:\DEV\OptiScaler-NR\_builds\xefg_cuda-7738416B-release\xefg_cuda-kit\xefg_cuda\libxess_fg.dll' }
$repro = Join-Path $root 'build\repro_test.exe'
$samplesExe = Join-Path $root 'build\pack_samples.exe'
foreach ($f in $Dll, $Lib, $repro, $samplesExe) { if (-not (Test-Path $f)) { throw "missing: $f" } }
$Dll = (Resolve-Path $Dll).Path
$Lib = (Resolve-Path $Lib).Path
if (Test-Path $Work) { Remove-Item $Work -Recurse -Force }
New-Item -ItemType Directory -Force $Work | Out-Null
$Work = (Resolve-Path $Work).Path
$fail = 0
$locks = @()
# The pack_samples outputs hold Intel-derived hashes and bytes: deleted at the end, also when a check throws or fails.
try {

function Run-Repro([string]$dir, [string]$localAppData) {
    New-Item -ItemType Directory -Force $dir, $localAppData | Out-Null
    Copy-Item $Dll (Join-Path $dir 'f5low.dll') -Force
    Copy-Item $Lib (Join-Path $dir 'libxess_fg.dll') -Force
    $saved = @{}
    foreach ($k in 'LOCALAPPDATA', 'XEFG_CUDA_LOG', 'XEFG_CUDA_PATH', 'XEFG_CUDA_PROF', 'REPRO_INTERP', 'REPRO_OFF', 'REPRO_ON') { $saved[$k] = [Environment]::GetEnvironmentVariable($k) }
    $env:LOCALAPPDATA = $localAppData; $env:XEFG_CUDA_LOG = '1'; $env:XEFG_CUDA_PATH = 'd3d12'; $env:XEFG_CUDA_PROF = '1'
    $env:REPRO_INTERP = '2'; $env:REPRO_OFF = '30'; $env:REPRO_ON = '200'
    try { $out = & $repro (Join-Path $dir 'f5low.dll'); $code = $LASTEXITCODE }
    finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
    return @{ Code = $code; Out = $out }
}

# ---- 1. run A ------------------------------------------------------------------------------------------------------------
$a = Join-Path $Work 'A'
$aLocal = Join-Path $Work 'A_localappdata'
$r = Run-Repro $a $aLocal
$status = ($r.Out | Select-String 'f5lowGetStatus ready').Line
Write-Host "run A: exit $($r.Code); $status"
if ($r.Code -ne 0 -or $status -notmatch ': 1 ') { Write-Host 'FAIL: the repro did not run on the own generator'; $fail++ }
$log = Join-Path $a 'f5low.log'
if (-not (Test-Path $log)) { Write-Host 'FAIL: no f5low.log'; $fail++ }
elseif (-not (Select-String -Path $log -SimpleMatch 'generated frame(s) per app frame' -Quiet)) { Write-Host 'FAIL: no frame was generated'; $fail++ }

$samples = Join-Path $Work 'samples.txt'          # outside the scanned folders
& $samplesExe $Lib $Dll $samples
if ($LASTEXITCODE -ne 0) { throw 'pack_samples failed' }
$parts = @((Join-Path $root 'recipe\i0\parts.json'), (Join-Path $root 'recipe\i0\hdr\parts.json'))
$args1 = @('-I', (Join-Path $root 'tests\no_intel_on_disk.py'), '--samples', $samples, '--allow-name', 'f5low.dll', '--allow-name', 'libxess_fg.dll')
foreach ($p in $parts) { $args1 += @('--parts', $p) }
Write-Host 'files f5low left behind:'
Get-ChildItem $a, $aLocal -Recurse -File | ForEach-Object { '  {0} ({1} bytes)' -f $_.FullName, $_.Length }
& python @args1 $a $aLocal
if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL: Intel-derived data on disk'; $fail++ }

# ---- 2. self-test of the checker ---------------------------------------------------------------------------------------------
$plant = Join-Path $Work 'planted'
& $samplesExe $Lib $Dll (Join-Path $Work 'samples_plant.txt') $plant | Out-Null
$planted = @(Get-ChildItem $plant -File -ErrorAction SilentlyContinue)
$args2 = @('-I', (Join-Path $root 'tests\no_intel_on_disk.py'), '--samples', (Join-Path $Work 'samples_plant.txt'))
foreach ($p in $parts) { $args2 += @('--parts', $p) }
$res = & python @args2 $plant
Write-Host ($res -join "`n")
if ($planted.Count -ne 2 -or $LASTEXITCODE -ne 1 -or ($res | Select-String 'VIOLATION').Count -ne 2) { Write-Host 'FAIL: the checker did not catch both planted files'; $fail++ }
else { Write-Host "self-test: the checker flags both planted files ($($planted.Name -join ', '))" }

# ---- 3. run B: old caches are removed, other things stay -------------------------------------------------------------------------
$b = Join-Path $Work 'B'
$bLocal = Join-Path $Work 'B_localappdata'
$rootsB = @((Join-Path $b 'f5low_cache'), (Join-Path $bLocal 'f5low\f5low_cache'))
$oldKeys = @()
foreach ($rt in $rootsB) {
    New-Item -ItemType Directory -Force $rt | Out-Null
    foreach ($fmt in 2, 3) {
        $key = ('c' * 30) + $fmt + [Array]::IndexOf($rootsB, $rt)
        $oldKeys += Join-Path $rt $key
        New-Item -ItemType Directory -Force (Join-Path $rt "$key\data") | Out-Null
        Set-Content -Path (Join-Path $rt "$key\data\R1.bin") -Value 'stand-in for an old weight file' -Encoding ascii
        Set-Content -Path (Join-Path $rt "$key\complete") -Value "f5low pack cache $fmt`nfiles 1`n10`tdata/R1.bin`n" -Encoding ascii
    }
    # a build folder that never finished, untouched for two hours (the old naming <32 hex>.tmp<pid>_<tick>)
    $stale = Join-Path $rt (('d' * 31) + [Array]::IndexOf($rootsB, $rt) + '.tmp4242_99')
    New-Item -ItemType Directory -Force (Join-Path $stale 'data') | Out-Null
    Set-Content -Path (Join-Path $stale 'data\R1.bin') -Value 'half a pack' -Encoding ascii
    (Get-Item $stale).LastWriteTime = (Get-Date).AddHours(-2); (Get-Item (Join-Path $stale 'data\R1.bin')).LastWriteTime = (Get-Date).AddHours(-2)
    (Get-Item (Join-Path $stale 'data')).LastWriteTime = (Get-Date).AddHours(-2)
    $oldKeys += $stale
    # things that are not ours
    New-Item -ItemType Directory -Force (Join-Path $rt 'not_ours') | Out-Null
    Set-Content -Path (Join-Path $rt 'not_ours\keep.txt') -Value 'a user file' -Encoding ascii
    New-Item -ItemType Directory -Force (Join-Path $rt 'cubins') | Out-Null
    Set-Content -Path (Join-Path $rt 'cubins\x.cubin') -Value 'no marker, not a pack' -Encoding ascii
    Set-Content -Path (Join-Path $rt 'readme.txt') -Value 'a loose file' -Encoding ascii
}
# folders with a file held open: one that allows deleting while open (FileShare.Delete) and one that does not (FileShare.None)
$locks = @()
$lockedDirs = @()
$shareDel = [System.IO.FileShare]([int][System.IO.FileShare]::ReadWrite -bor [int][System.IO.FileShare]::Delete)
foreach ($spec in @(@('e', $shareDel), @('f', [System.IO.FileShare]::None))) {
    $ld = Join-Path $rootsB[0] (($spec[0] * 30) + '00')
    New-Item -ItemType Directory -Force (Join-Path $ld 'data') | Out-Null
    Set-Content -Path (Join-Path $ld 'complete') -Value "f5low pack cache 3`nfiles 1`n10`tdata/R1.bin`n" -Encoding ascii
    $locks += [System.IO.File]::Open((Join-Path $ld 'data\R1.bin'), [System.IO.FileMode]::Create, [System.IO.FileAccess]::ReadWrite, $spec[1])
    $lockedDirs += $ld
}
$r2 = Run-Repro $b $bLocal
Write-Host "run B: exit $($r2.Code)"
Start-Sleep -Milliseconds 500
$left = @($oldKeys | Where-Object { Test-Path $_ })
$gone = $oldKeys.Count - $left.Count
Write-Host "old cache folders planted: $($oldKeys.Count), removed: $gone"
if ($left.Count) { Write-Host "FAIL: not removed: $($left -join ', ')"; $fail++ }
$kept = 0
foreach ($rt in $rootsB) { foreach ($k in 'not_ours\keep.txt', 'cubins\x.cubin', 'readme.txt') { if (Test-Path (Join-Path $rt $k)) { $kept++ } else { Write-Host "FAIL: $rt\$k was deleted"; $fail++ } } }
Write-Host "things that are not ours and were kept: $kept of 6"
# the locked ones after run 1: gone, renamed to .f5low-del, or (a lock that blocks the rename) emptied but for the held file
foreach ($ld in $lockedDirs) {
    $state = if (-not (Test-Path $ld) -and -not (Test-Path "$ld.f5low-del")) { 'gone' } elseif (Test-Path "$ld.f5low-del") { 'renamed to .f5low-del' } else { 'still there under its name (marker kept: ' + (Test-Path (Join-Path $ld 'complete')) + ')' }
    Write-Host "locked $(Split-Path -Leaf $ld) after run 1: $state"
}
foreach ($l in $locks) { $l.Dispose() }
$r3 = Run-Repro $b $bLocal
Write-Host "run B2 (locks released): exit $($r3.Code)"
Start-Sleep -Milliseconds 500
foreach ($ld in $lockedDirs) { if ((Test-Path $ld) -or (Test-Path "$ld.f5low-del")) { Write-Host "FAIL: $ld survived the second run"; $fail++ } }
$dels = @(Get-ChildItem $rootsB -Directory -Filter '*.f5low-del' -ErrorAction SilentlyContinue)
if ($dels.Count) { Write-Host "FAIL: leftover .f5low-del folders: $($dels.Name -join ', ')"; $fail++ } else { Write-Host 'no .f5low-del folder left; locked folders removed after the release' }
Select-String -Path (Join-Path $b 'f5low.log') -Pattern 'removed the old on-disk' | ForEach-Object { '  ' + $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) }

if ($fail) { Write-Host "no_intel_on_disk: FAIL ($fail)"; exit 1 }
Write-Host 'no_intel_on_disk: PASS'
} finally {
    foreach ($l in $locks) { try { $l.Dispose() } catch {} }
    Remove-Item (Join-Path $Work 'samples*.txt'), (Join-Path $Work 'planted') -Recurse -Force -ErrorAction SilentlyContinue
}
