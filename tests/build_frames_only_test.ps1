# Builds build\frames_only_test.exe (drives build\f5low.dll's frames-only API, include\f5low_frames.h, with a moving bar, no game).
# Run: build\frames_only_test.exe build\f5low.dll   (the player's libxess_fg.dll: XEFG_CUDA_INTEL_LIBRARY=<full path>, else as for the XeFG API)
# Visual Studio as build.ps1: -VsPath.
param([string]$VsPath = 'D:\DEV\VS2022')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$cmd = @"
@echo off
cd /d "$out"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "$vcvars" >nul
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$root\include" "$root\tests\frames_only_test.cpp" /link /OUT:frames_only_test.exe d3d12.lib dxgi.lib > _build_frames.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_frames.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'frames_only_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_frames.bat')
Get-Content (Join-Path $out '_build_frames.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_frames.bat'), (Join-Path $out '_build_frames.log'), (Join-Path $out 'frames_only_test.obj') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $out 'frames_only_test.exe')) { Write-Host 'built build\frames_only_test.exe' } else { throw 'build failed' }
