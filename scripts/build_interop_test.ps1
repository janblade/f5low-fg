# Builds interop_pack_test.exe (D3D12 <-> CUDA sharing check). Needs VS2022 at D:\DEV\VS2022.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$vcvars = 'D:\DEV\VS2022\VC\Auxiliary\Build\vcvars64.bat'
$installer = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$cmd = @"
@echo off
set "PATH=$installer;%PATH%"
call "$vcvars" >nul
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DNDEBUG interop_pack_test.cpp /link /OUT:interop_pack_test.exe d3d12.lib dxgi.lib > _build.log 2>&1
"@
Remove-Item 'interop_pack_test.exe' -ErrorAction SilentlyContinue
$bat = Join-Path $here '_build.bat'
Set-Content -Path $bat -Value $cmd -Encoding ascii
cmd /c $bat
Get-Content '_build.log' | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item $bat,'_build.log','interop_pack_test.obj' -ErrorAction SilentlyContinue
if (Test-Path 'interop_pack_test.exe') { Write-Host 'built interop_pack_test.exe' } else { throw 'build failed' }
