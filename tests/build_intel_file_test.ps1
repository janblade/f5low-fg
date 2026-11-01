# Builds build\intel_file_test.exe (rebuilds every weight upload of the recipe from a libxess_fg.dll file and checks the hashes).
# Visual Studio as build.ps1: -VsPath. Run: build\intel_file_test.exe <libxess_fg.dll> recipe\i0\parts.json
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
cl /nologo /c /TC /w /O2 /MT /DNDEBUG /Fo_zstd_ift.obj "$zstd\zstddeclib.c" > _build_ift.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$zstd" "$root\tests\intel_file_test.cpp" "$src\intel_file_source.cpp" "$src\intel_modules.cpp" _zstd_ift.obj /link /OUT:intel_file_test.exe >> _build_ift.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_ift.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'intel_file_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_ift.bat')
Get-Content (Join-Path $out '_build_ift.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_ift.bat'), (Join-Path $out '_build_ift.log'), (Join-Path $out '_zstd_ift.obj'), (Join-Path $out 'intel_file_test.obj'), (Join-Path $out 'intel_file_source.obj'), (Join-Path $out 'intel_modules.obj') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $out 'intel_file_test.exe')) { Write-Host 'built build\intel_file_test.exe' } else { throw 'build failed' }
