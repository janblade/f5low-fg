# Builds build\modules_test.exe (decodes a libxess_fg.dll with src\intel_modules.cpp, checks the recipe's module hashes).
# Visual Studio as build.ps1: -VsPath. Run: build\modules_test.exe <libxess_fg.dll> recipe [hashes.txt]
param([string]$VsPath = 'D:\DEV\VS2022')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$zstd = Join-Path $root 'third_party\zstd'
$cmd = @"
@echo off
cd /d "$out"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "$vcvars" >nul
cl /nologo /c /TC /w /O2 /MT /DNDEBUG /Fo_zstd_mt.obj "$zstd\zstddeclib.c" > _build_modules.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$zstd" "$root\tests\modules_test.cpp" "$root\src\intel_modules.cpp" _zstd_mt.obj /link /OUT:modules_test.exe >> _build_modules.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_modules.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'modules_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_modules.bat')
Get-Content (Join-Path $out '_build_modules.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_modules.bat'), (Join-Path $out '_build_modules.log'), (Join-Path $out '_zstd_mt.obj'), (Join-Path $out 'modules_test.obj'), (Join-Path $out 'intel_modules.obj') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $out 'modules_test.exe')) { Write-Host 'built build\modules_test.exe' } else { throw 'build failed' }
