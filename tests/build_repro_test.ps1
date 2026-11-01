# Builds build\repro_test.exe (drives any XeSS-FG-API DLL through the API with a moving gradient, no game).
# Headers and Visual Studio as build.ps1: -XessInc / F5LOW_XESS_INC / ..\OptiScaler-NR, -VsPath.
param([string]$XessInc = '', [string]$VsPath = 'D:\DEV\VS2022')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$inc = @($XessInc, $env:F5LOW_XESS_INC, (Join-Path $root '..\OptiScaler-NR\external\xess\inc\xess_fg')) |
    Where-Object { $_ -and (Test-Path (Join-Path $_ 'xefg_swapchain.h')) } | Select-Object -First 1
if (-not $inc) { throw 'XeSS-FG SDK headers not found (-XessInc, F5LOW_XESS_INC or ..\OptiScaler-NR)' }
$inc = Resolve-Path $inc
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$cmd = @"
@echo off
cd /d "$out"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "$vcvars" >nul
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$inc" "$root\tests\repro_test.cpp" /link /OUT:repro_test.exe d3d12.lib dxgi.lib user32.lib > _build_repro.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_repro.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'repro_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_repro.bat')
Get-Content (Join-Path $out '_build_repro.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_repro.bat'), (Join-Path $out '_build_repro.log'), (Join-Path $out 'repro_test.obj') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $out 'repro_test.exe')) { Write-Host 'built build\repro_test.exe' } else { throw 'build failed' }
