# Builds tests\nvapi_fatbin_test.exe into build\ (host-only: the NVAPI kernel container rewrite of src\unlock\NvapiFatbin.h) and runs it; throws if it fails.
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
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /Fe:nvapi_fatbin_test.exe "$root\tests\nvapi_fatbin_test.cpp" > _build_nf.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_nf.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'nvapi_fatbin_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_nf.bat')
Get-Content (Join-Path $out '_build_nf.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_nf.bat'), (Join-Path $out '_build_nf.log'), (Join-Path $out 'nvapi_fatbin_test.obj') -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out 'nvapi_fatbin_test.exe'))) { throw 'nvapi_fatbin_test build failed' }
& (Join-Path $out 'nvapi_fatbin_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'nvapi_fatbin_test failed' }

