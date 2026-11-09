# Builds tests\drop_late_test.exe into build\ (host-only: the schedule pieces of src\pace_sched.h) and runs it; throws if it fails.
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
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /Fe:drop_late_test.exe "$root\tests\drop_late_test.cpp" > _build_dl.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_dl.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'drop_late_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_dl.bat')
Get-Content (Join-Path $out '_build_dl.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_dl.bat'), (Join-Path $out '_build_dl.log'), (Join-Path $out 'drop_late_test.obj') -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out 'drop_late_test.exe'))) { throw 'drop_late_test build failed' }
& (Join-Path $out 'drop_late_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'drop_late_test failed' }
