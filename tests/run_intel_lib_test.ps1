# Checks where a pack-less f5low.dll takes Intel's libxess_fg.dll from (RealLibraryPath in xefg_cuda.cpp):
# XEFG_CUDA_INTEL_LIBRARY, then next to itself, then the folder above. One fresh process per case.
#   .\run_intel_lib_test.ps1 -IntelLibrary <a libxess_fg.dll> [-Work <empty scratch folder>]
# Build first: .\build.ps1 (no -EmbedPack) and cl /nologo /std:c++20 /EHsc /W4 intel_lib_test.cpp psapi.lib
param([Parameter(Mandatory)] [string]$IntelLibrary, [string]$Work = (Join-Path $env:TEMP 'xefg_intel_lib_test'))
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here 'intel_lib_test.exe'
$cuda = Join-Path $here 'f5low.dll'
foreach ($f in $exe, $cuda, $IntelLibrary) { if (-not (Test-Path -LiteralPath $f)) { throw "missing: $f" } }

if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
$opti = Join-Path $Work 'OptiScaler'
$plugins = Join-Path $opti 'plugins'
$elsewhere = Join-Path $Work 'elsewhere'
New-Item -ItemType Directory -Force -Path $plugins, $elsewhere | Out-Null
Copy-Item -LiteralPath $cuda -Destination $plugins
Copy-Item -LiteralPath $IntelLibrary -Destination $opti
Copy-Item -LiteralPath $IntelLibrary -Destination $elsewhere

$fails = 0
function Run-Case([string]$name, [string]$envValue, [bool]$ownCopy, [string]$expect) {
    $own = Join-Path $plugins 'libxess_fg.dll'
    if ($ownCopy) { Copy-Item -LiteralPath $IntelLibrary -Destination $own -Force }
    elseif (Test-Path -LiteralPath $own) { Remove-Item -LiteralPath $own -Force }

    if ($envValue) { $env:XEFG_CUDA_INTEL_LIBRARY = $envValue } else { Remove-Item Env:XEFG_CUDA_INTEL_LIBRARY -ErrorAction SilentlyContinue }
    $out = & $exe (Join-Path $plugins 'f5low.dll') | Select-Object -Last 1
    Remove-Item Env:XEFG_CUDA_INTEL_LIBRARY -ErrorAction SilentlyContinue

    $loaded = if ($out -match 'loaded=(.+)$') { $Matches[1].Trim() } else { '' }
    $ok = $loaded -ieq $expect
    if (-not $ok) { $script:fails++ }
    "{0}  {1}`n      got: {2}`n      want: {3}" -f ($(if ($ok) { 'ok  ' } else { 'FAIL' })), $name, $out, $expect
}

$optiLib = (Join-Path $opti 'libxess_fg.dll')
$ownLib = (Join-Path $plugins 'libxess_fg.dll')
$elseLib = (Join-Path $elsewhere 'libxess_fg.dll')

Run-Case 'env set: its file wins over the own copy' $elseLib $true $elseLib
Run-Case 'env set, no own copy' $optiLib $false $optiLib
Run-Case 'no env: own copy (kits, older installs)' '' $true $ownLib
Run-Case 'no env, no own copy: the folder above' '' $false $optiLib
Run-Case 'env names f5low.dll itself: ignored' (Join-Path $plugins 'f5low.dll') $false $optiLib
Run-Case 'env names a missing file: ignored' (Join-Path $Work 'missing\libxess_fg.dll') $true $ownLib

if ($fails -eq 0) { 'intel_lib_test: PASS' } else { "intel_lib_test: $fails FAIL"; exit 1 }
