# Builds tests\pacer_api_test.exe into build\ (host-only: the pacer export core src\pace_api.h, include5low_pacer.h) and runs it; pass build5low.dll to test the exports too; throws if it fails.
param([string]$Dll = '', [string]$VsPath = 'D:\DEV\VS2022')
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
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /I"$root\include" /I"$root\src" /Fe:pacer_api_test.exe "$root\tests\pacer_api_test.cpp" > _build_pa.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_pa.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'pacer_api_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_pa.bat')
Get-Content (Join-Path $out '_build_pa.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_pa.bat'), (Join-Path $out '_build_pa.log'), (Join-Path $out 'pacer_api_test.obj') -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out 'pacer_api_test.exe'))) { throw 'pacer_api_test build failed' }
if ($Dll) { & (Join-Path $out 'pacer_api_test.exe') (Resolve-Path $Dll).Path } else { & (Join-Path $out 'pacer_api_test.exe') }
if ($LASTEXITCODE -ne 0) { throw 'pacer_api_test failed' }
