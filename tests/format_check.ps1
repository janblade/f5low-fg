# Motion and depth in other DXGI formats, through the own generator, on the GPU: the generated frame must be the same as with the
# baseline run (repro_test's own inputs: motion R16G16_TYPELESS (33) holding half floats, depth R32G8X24_TYPELESS (19)), bit for bit, for every format the DLL takes (src\xefg_formats.h), and a format it
# refuses must give no generated frames (GetLastPresentStatus: 1 presented, generation off) and tell XeLL generation is off.
# Runs build\repro_test.exe (tests\build_repro_test.ps1) against build\f5low.dll with REPRO_MVFMT / REPRO_DEPTHFMT and the shown-frame
# dump (XEFG_CUDA_DUMP_SHOWN), one short run per format. The motion scale of a normalized format is 8 (REPRO_MVSCALE) so that the
# bar's -8 px is exactly 1.0 in the encoding: any difference is the decoding, not the 8-bit steps.
#   powershell -File tests\format_check.ps1 -IntelLib "<OptiScaler-style folder>\libxess_fg.dll" [-Dll build\f5low.dll]
# libxell.dll next to the library is handed to repro_test (the DLL then tells XeLL as the game would). Exit code 0 = PASS.
param([Parameter(Mandatory)][string]$IntelLib, [string]$Dll = 'build\f5low.dll', [string]$Out = '', [string]$Only = '')   # -Only <regex>: just the cases whose name (mv35, depth42, ...) matches
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $root 'build\repro_test.exe'
if (-not (Test-Path $exe)) { throw 'build\repro_test.exe missing: run tests\build_repro_test.ps1' }
$dll = (Resolve-Path (Join-Path $root $Dll)).Path
$lib = (Resolve-Path $IntelLib).Path
$xell = Join-Path (Split-Path -Parent $lib) 'libxell.dll'
if (-not $Out) { $Out = Join-Path $root 'build\format_check' }
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path (Split-Path -Parent $dll) 'f5low.log'
$fbLog = if ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'f5low\f5low.log' } else { '' }
$names = 'F5LOW_GENERATOR', 'XEFG_CUDA_INTEL_LIBRARY', 'REPRO_BAR', 'REPRO_ON', 'REPRO_OFF', 'REPRO_INTERP', 'XEFG_CUDA_LOG', 'XEFG_CUDA_PATH',
          'XEFG_CUDA_DUMP_SHOWN', 'XEFG_CUDA_DUMP_FRAME', 'REPRO_MVFMT', 'REPRO_DEPTHFMT', 'REPRO_MVSCALE', 'REPRO_NOFILL'

function Run([string]$name, [string]$mv, [string]$depth, [bool]$nofill = $false) {
    $d = Join-Path $Out $name
    Remove-Item $d -Recurse -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $d | Out-Null
    Remove-Item $log -ErrorAction SilentlyContinue
    if ($fbLog) { Remove-Item $fbLog -ErrorAction SilentlyContinue }
    $v = @{ F5LOW_GENERATOR = 'own'; XEFG_CUDA_INTEL_LIBRARY = $lib; REPRO_BAR = '1'; REPRO_ON = '360'; REPRO_OFF = '60'; REPRO_INTERP = '2'
            XEFG_CUDA_LOG = '1'; XEFG_CUDA_PATH = 'd3d12'; XEFG_CUDA_DUMP_SHOWN = "$d\s"; XEFG_CUDA_DUMP_FRAME = '200'; REPRO_MVSCALE = '8' }
    if ($mv) { $v.REPRO_MVFMT = $mv }
    if ($depth) { $v.REPRO_DEPTHFMT = $depth }
    if ($nofill) { $v.REPRO_NOFILL = '1' }     # a format the test has no encoder for: created and tagged, never filled
    foreach ($k in $names) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    foreach ($k in $v.Keys) { Set-Item "env:$k" $v[$k] }
    $p = Start-Process -FilePath $exe -ArgumentList "`"$dll`"", "`"$xell`"" -NoNewWindow -PassThru -RedirectStandardOutput "$d\out.txt" -RedirectStandardError "$d\err.txt"
    $null = $p.Handle      # keeps the process object able to report the exit code
    if (-not $p.WaitForExit(120000)) { throw "$name still running after 120 s (left alone)" }
    $p.WaitForExit()
    foreach ($k in $names) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    if (Test-Path $log) { Copy-Item $log "$d\f5low.log" -Force } elseif ($fbLog -and (Test-Path $fbLog)) { Copy-Item $fbLog "$d\f5low.log" -Force }
    $hashes = (Get-ChildItem $d -Filter 's.k*.bin' | Sort-Object Name | ForEach-Object { (Get-FileHash $_.FullName -Algorithm SHA256).Hash }) -join ' '
    Get-ChildItem $d -Filter 's.k*.bin' | Remove-Item
    $pres = (Select-String -Path "$d\out.txt" -Pattern 'frame \d+: presented (\d+), frame generation on (\d)' | Select-Object -Last 1)
    $pm = if ($pres) { $pres.Matches[0] } else { $null }       # no such line (the run died): reported as a FAIL by the caller
    [pscustomobject]@{ Name = $name; Exit = $p.ExitCode; Hashes = $hashes; Presented = $(if ($pm) { [int]$pm.Groups[1].Value } else { -1 }); On = $(if ($pm) { [int]$pm.Groups[2].Value } else { -1 })
                       XellOff = [bool](Select-String -Path "$d\f5low.log" -Pattern 'xell: frame generation off for XeLL \(the tagged' -Quiet)
                       Warned = [bool](Select-String -Path "$d\f5low.log" -Pattern 'are not supported - no frames generated|is not supported - no frames generated' -Quiet) }
}

$fail = 0
$base = Run 'base' '' ''
if (-not $base.Hashes -or $base.Presented -ne 3) { throw 'the baseline run (R16G16_FLOAT / R32G8X24) generated nothing: check the library and the DLL' }
"base            3 frames presented, hashes $($base.Hashes.Substring(0, 12))..."
# format number -> name; taken ones must equal the baseline, refused ones must not generate
$motionTaken = @{ 33 = 'R16G16_TYPELESS'; 34 = 'R16G16_FLOAT'; 35 = 'R16G16_UNORM'; 37 = 'R16G16_SNORM'; 9 = 'R16G16B16A16_TYPELESS'; 10 = 'R16G16B16A16_FLOAT'
                  11 = 'R16G16B16A16_UNORM'; 13 = 'R16G16B16A16_SNORM'; 15 = 'R32G32_TYPELESS'; 16 = 'R32G32_FLOAT'; 5 = 'R32G32B32_TYPELESS'; 6 = 'R32G32B32_FLOAT'
                  1 = 'R32G32B32A32_TYPELESS'; 2 = 'R32G32B32A32_FLOAT'; 48 = 'R8G8_TYPELESS'; 49 = 'R8G8_UNORM'; 51 = 'R8G8_SNORM'; 27 = 'R8G8B8A8_TYPELESS'
                  28 = 'R8G8B8A8_UNORM'; 31 = 'R8G8B8A8_SNORM'; 23 = 'R10G10B10A2_TYPELESS'; 24 = 'R10G10B10A2_UNORM'; 26 = 'R11G11B10_FLOAT' }
$motionRefused = @{ 36 = 'R16G16_UINT'; 38 = 'R16G16_SINT'; 17 = 'R32G32_UINT'; 50 = 'R8G8_UINT'; 52 = 'R8G8_SINT'; 12 = 'R16G16B16A16_UINT'; 41 = 'R32_FLOAT'; 54 = 'R16_FLOAT' }
$depthTaken = @{ 19 = 'R32G8X24_TYPELESS'; 20 = 'D32_FLOAT_S8X24_UINT'; 39 = 'R32_TYPELESS'; 40 = 'D32_FLOAT'; 41 = 'R32_FLOAT'; 44 = 'R24G8_TYPELESS'
                 45 = 'D24_UNORM_S8_UINT'; 53 = 'R16_TYPELESS'; 55 = 'D16_UNORM'; 56 = 'R16_UNORM'; 54 = 'R16_FLOAT' }
foreach ($kind in 'mv', 'depth') {
    $taken = if ($kind -eq 'mv') { $motionTaken } else { $depthTaken }
    foreach ($f in ($taken.Keys | Sort-Object)) {
        if ($Only -and "$kind$f" -notmatch $Only) { continue }
        $r = if ($kind -eq 'mv') { Run "mv$f" "$f" '' } else { Run "depth$f" '' "$f" }
        $ok = $r.Exit -eq 0 -and $r.Presented -eq 3 -and $r.On -eq 1 -and $r.Hashes -eq $base.Hashes -and -not $r.XellOff
        "{0,-6} {1,-24} {2}" -f "$kind$f", $taken[$f], $(if ($ok) { 'ok   same generated frames as the baseline' } else { "FAIL exit $($r.Exit) presented $($r.Presented) on $($r.On) same=$($r.Hashes -eq $base.Hashes) xellOff=$($r.XellOff)" })
        $fail += -not $ok
    }
}
foreach ($f in ($motionRefused.Keys | Sort-Object)) {
    if ($Only -and "mv$f" -notmatch $Only) { continue }
    $r = Run "mv$f" "$f" ''
    $ok = $r.Exit -eq 0 -and $r.Presented -eq 1 -and $r.On -eq 0 -and $r.XellOff -and $r.Warned
    "{0,-6} {1,-24} {2}" -f "mv$f", $motionRefused[$f], $(if ($ok) { 'ok   refused: 1 frame presented, generation off, XeLL told off, warning logged' } else { "FAIL presented $($r.Presented) on $($r.On) xellOff=$($r.XellOff) warned=$($r.Warned)" })
    $fail += -not $ok
}
# refused depth formats (integer): created without a fill (the test has no encoder), tagged, refused
$depthRefused = @{ 42 = 'R32_UINT'; 43 = 'R32_SINT'; 57 = 'R16_UINT' }
foreach ($f in ($depthRefused.Keys | Sort-Object)) {
    if ($Only -and "depth$f" -notmatch $Only) { continue }
    $r = Run "depth$f" '' "$f" $true
    $ok = $r.Exit -eq 0 -and $r.Presented -eq 1 -and $r.On -eq 0 -and $r.XellOff -and $r.Warned
    "{0,-6} {1,-24} {2}" -f "depth$f", $depthRefused[$f], $(if ($ok) { 'ok   refused: 1 frame presented, generation off, XeLL told off, warning logged' } else { "FAIL exit $($r.Exit) presented $($r.Presented) on $($r.On) xellOff=$($r.XellOff) warned=$($r.Warned)" })
    $fail += -not $ok
}
if ($fail) { "FAIL ($fail)"; exit 1 }
'PASS'
