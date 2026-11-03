# Builds the unlock test programs into build\: mfgunlock_core_smoke.exe (synthetic images, no NVIDIA files), mfgunlock_check.exe,
# mfgunlock_fatbin_refs.exe, mfgunlock_turing_probe.exe and unlock_hook_probe.exe (these take the player's nvngx_dlssg.dll / sl.dlss_g.dll as arguments; the
# files stay outside this repo). All are ported from OptiScaler-NR's tests (see the header of each); hde64.c is compiled as C.
param([string]$VsPath = 'D:\DEV\VS2022')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$hde = Join-Path $root 'src\unlock\hde\hde64.c'
$cmd = @"
@echo off
cd /d "$out"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "$vcvars" >nul
cl /nologo /c /TC /w /O2 /MT /DNDEBUG /Fohde64.obj "$hde" > _build_mfg.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /wd4701 /O2 /MT /DNDEBUG /Fe:mfgunlock_core_smoke.exe "$root\tests\mfgunlock_core_smoke.cpp" hde64.obj >> _build_mfg.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /wd4701 /O2 /MT /DNDEBUG /Fe:mfgunlock_check.exe "$root\tests\mfgunlock_check.cpp" hde64.obj >> _build_mfg.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /Fe:mfgunlock_fatbin_refs.exe "$root\tests\mfgunlock_fatbin_refs.cpp" >> _build_mfg.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /wd4701 /O2 /MT /DNDEBUG /Fe:mfgunlock_turing_probe.exe "$root\tests\mfgunlock_turing_probe.cpp" hde64.obj >> _build_mfg.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /wd4701 /O2 /MT /DNDEBUG /Fe:unlock_hook_probe.exe "$root\tests\unlock_hook_probe.cpp" hde64.obj >> _build_mfg.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /wd4701 /O2 /MT /DNDEBUG /Fe:xefg_unlock_test.exe "$root\tests\xefg_unlock_test.cpp" hde64.obj >> _build_mfg.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_mfg.bat') -Value $cmd -Encoding ascii
foreach ($e in 'mfgunlock_core_smoke', 'mfgunlock_check', 'mfgunlock_fatbin_refs', 'mfgunlock_turing_probe', 'unlock_hook_probe', 'xefg_unlock_test') { Remove-Item (Join-Path $out "$e.exe") -ErrorAction SilentlyContinue }
cmd /c (Join-Path $out '_build_mfg.bat')
Get-Content (Join-Path $out '_build_mfg.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_mfg.bat'), (Join-Path $out '_build_mfg.log'), (Join-Path $out 'hde64.obj'), (Join-Path $out 'mfgunlock_*.obj') -ErrorAction SilentlyContinue
foreach ($e in 'mfgunlock_core_smoke', 'mfgunlock_check', 'mfgunlock_fatbin_refs', 'mfgunlock_turing_probe', 'unlock_hook_probe', 'xefg_unlock_test') {
    if (Test-Path (Join-Path $out "$e.exe")) { Write-Host "built build\$e.exe" } else { throw "$e build failed" }
}
