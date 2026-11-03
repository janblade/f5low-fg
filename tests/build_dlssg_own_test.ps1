# Builds tests\dlssg_own_test.exe into build\ (host-only: the nvngx_dlssg ownership rule of src\generators\dlssg_own.h) and runs it; throws if it fails.
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
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /Fe:dlssg_own_test.exe "$root\tests\dlssg_own_test.cpp" > _build_do.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_do.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'dlssg_own_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_do.bat')
Get-Content (Join-Path $out '_build_do.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_do.bat'), (Join-Path $out '_build_do.log'), (Join-Path $out 'dlssg_own_test.obj') -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out 'dlssg_own_test.exe'))) { throw 'dlssg_own_test build failed' }
& (Join-Path $out 'dlssg_own_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'dlssg_own_test failed' }
