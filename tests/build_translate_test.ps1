# Builds build\translate_test.exe (translates the recipe's Intel modules in-process and compares with a reference).
# Visual Studio as build.ps1: -VsPath. Run: build\translate_test.exe <libxess_fg.dll> recipe <reference folder> <dxcompiler.dll>
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
cl /nologo /c /TC /w /O2 /MT /DNDEBUG /Fo_zstd_tt.obj "$zstd\zstddeclib.c" > _build_tt.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$zstd" "$root\tests\translate_test.cpp" "$src\translate.cpp" "$src\dxc_disasm.cpp" "$src\intel_modules.cpp" _zstd_tt.obj /link /OUT:translate_test.exe >> _build_tt.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_tt.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'translate_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_tt.bat')
Get-Content (Join-Path $out '_build_tt.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_tt.bat'), (Join-Path $out '_build_tt.log'), (Join-Path $out '_zstd_tt.obj'), (Join-Path $out 'translate_test.obj'), (Join-Path $out 'translate.obj'), (Join-Path $out 'dxc_disasm.obj'), (Join-Path $out 'intel_modules.obj') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $out 'translate_test.exe')) { Write-Host 'built build\translate_test.exe' } else { throw 'build failed' }
