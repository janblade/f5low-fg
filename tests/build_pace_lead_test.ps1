# Builds tests\pace_lead_test.exe into build\ (host-only: the PaceLead rule of src\pace_lead.h) and runs it; throws if it fails.
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
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /Fe:pace_lead_test.exe "$root\tests\pace_lead_test.cpp" > _build_pl.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_pl.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'pace_lead_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_pl.bat')
Get-Content (Join-Path $out '_build_pl.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_pl.bat'), (Join-Path $out '_build_pl.log'), (Join-Path $out 'pace_lead_test.obj') -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out 'pace_lead_test.exe'))) { throw 'pace_lead_test build failed' }
& (Join-Path $out 'pace_lead_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'pace_lead_test failed' }
