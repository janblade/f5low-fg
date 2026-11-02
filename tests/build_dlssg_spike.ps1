# Builds build\dlssg_spike.exe (the standalone DLSS-G / sink check, tests\dlssg_spike.cpp). The exe is not committed; run it with a
# folder that holds the player's own Streamline files (never copied into this repo): build\dlssg_spike.exe <folder>.
# Needs Visual Studio 2022 (-VsPath) and NVIDIA's public Streamline SDK headers: -StreamlineInc, else $env:F5LOW_STREAMLINE_INC, else
# an OptiScaler-NR checkout next to this repo (..\OptiScaler-NR\external\streamline).
param([string]$StreamlineInc = '', [string]$VsPath = 'D:\DEV\VS2022')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$inc = @($StreamlineInc, $env:F5LOW_STREAMLINE_INC, (Join-Path $root '..\OptiScaler-NR\external\streamline')) |
    Where-Object { $_ -and (Test-Path (Join-Path $_ 'sl.h')) } | Select-Object -First 1
if (-not $inc) { throw 'Streamline SDK headers not found (-StreamlineInc, F5LOW_STREAMLINE_INC or ..\OptiScaler-NR\external\streamline)' }
$inc = Resolve-Path $inc
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$cmd = @"
@echo off
cd /d "$out"
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "$vcvars" >nul
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$inc" "$root\tests\dlssg_spike.cpp" /link /OUT:dlssg_spike.exe d3d12.lib dxgi.lib user32.lib > _build_spike.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_spike.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'dlssg_spike.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_spike.bat')
Get-Content (Join-Path $out '_build_spike.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_spike.bat'), (Join-Path $out '_build_spike.log'), (Join-Path $out 'dlssg_spike.obj') -ErrorAction SilentlyContinue
if (Test-Path (Join-Path $out 'dlssg_spike.exe')) { Write-Host 'built build\dlssg_spike.exe' } else { throw 'build failed' }
