# Builds and runs convert_test.exe (xefg_convert.hlsl on WARP, no game, no pack). Needs VS2022 at D:\DEV\VS2022.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$vcvars = 'D:\DEV\VS2022\VC\Auxiliary\Build\vcvars64.bat'
$installer = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$cmd = @"
@echo off
set "PATH=$installer;%PATH%"
call "$vcvars" >nul
type nul > _build.log
fxc /nologo /T cs_5_0 /E main /O3 /Fh xefg_convert_cs.h /Vn g_xefgConvertCS xefg_convert.hlsl >> _build.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG convert_test.cpp /link /OUT:convert_test.exe d3d12.lib dxgi.lib >> _build.log 2>&1
"@
Remove-Item 'convert_test.exe' -ErrorAction SilentlyContinue
$bat = Join-Path $here '_build.bat'
Set-Content -Path $bat -Value $cmd -Encoding ascii
cmd /c $bat
Get-Content '_build.log' | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item $bat,'_build.log','convert_test.obj' -ErrorAction SilentlyContinue
if (-not (Test-Path 'convert_test.exe')) { throw 'build failed' }
& (Join-Path $here 'convert_test.exe')
exit $LASTEXITCODE
