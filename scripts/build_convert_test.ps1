# Builds build\convert_test.exe and runs it: xefg_convert.hlsl on WARP (every Kind of the motion / depth conversion against exact values) and
# the format table of src\xefg_formats.h on the host. No game, no pack, no NVIDIA GPU. Throws if it fails.
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
type nul > _build_ct.log
fxc /nologo /T cs_5_0 /E main /O3 /Fh xefg_convert_cs.h /Vn g_xefgConvertCS "$root\src\xefg_convert.hlsl" >> _build_ct.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$out" "$root\tests\convert_test.cpp" /link /OUT:convert_test.exe d3d12.lib dxgi.lib >> _build_ct.log 2>&1
"@
Set-Content -Path (Join-Path $out '_build_ct.bat') -Value $cmd -Encoding ascii
Remove-Item (Join-Path $out 'convert_test.exe') -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_ct.bat')
Get-Content (Join-Path $out '_build_ct.log') | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item (Join-Path $out '_build_ct.bat'), (Join-Path $out '_build_ct.log'), (Join-Path $out 'convert_test.obj') -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $out 'convert_test.exe'))) { throw 'convert_test build failed' }
& (Join-Path $out 'convert_test.exe')
if ($LASTEXITCODE -ne 0) { throw 'convert_test failed' }
