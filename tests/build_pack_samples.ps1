# Builds build\pack_samples.exe (the reference of tests\no_intel_on_disk.ps1: builds the pack in memory like f5low.dll and lists
# the Intel-derived files' hashes and 64-byte windows). Visual Studio as build.ps1: -VsPath.
param([string]$VsPath = 'D:\DEV\VS2022')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$zstd = Join-Path $root 'third_party\zstd'
$src = Join-Path $root 'src'
$cmd = @"
@echo off
cd /d "$out"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "$vcvars" >nul
cl /nologo /c /TC /w /O2 /MT /DNDEBUG /Fo_zstd_ps.obj "$zstd\zstddeclib.c" > _build_ps.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$zstd" "$root\tests\pack_samples.cpp" "$src\pack_build.cpp" "$src\intel_file_source.cpp" "$src\intel_modules.cpp" _zstd_ps.obj /link /OUT:pack_samples.exe >> _build_ps.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_ps.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'pack_samples.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_ps.bat')
Get-Content (Join-Path $out '_build_ps.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_ps.bat'), (Join-Path $out '_build_ps.log'), (Join-Path $out '_zstd_ps.obj'), (Join-Path $out 'pack_samples.obj'), (Join-Path $out 'pack_build.obj'), (Join-Path $out 'intel_file_source.obj'), (Join-Path $out 'intel_modules.obj') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $out 'pack_samples.exe')) { Write-Host 'built build\pack_samples.exe' } else { throw 'build failed' }
