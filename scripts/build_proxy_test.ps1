# Builds proxy_test.exe (drives f5low.dll through its API, no game). Needs VS2022 at D:\DEV\VS2022
# and the OptiScaler-NR checkout for Intel's public XeSS-FG SDK headers.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$inc = @('..\..\..\OptiScaler-NR\external\xess\inc\xess_fg',
         '..\..\..\..\OptiScaler-NR\OptiScaler-NR\external\xess\inc\xess_fg') |
    ForEach-Object { Join-Path $here $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $inc) { throw 'OptiScaler-NR checkout (XeSS-FG SDK headers) not found' }
$inc = Resolve-Path $inc
$vcvars = 'D:\DEV\VS2022\VC\Auxiliary\Build\vcvars64.bat'
$installer = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$cmd = @"
@echo off
set "PATH=$installer;%PATH%"
call "$vcvars" >nul
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$inc" proxy_test.cpp /link /OUT:proxy_test.exe d3d12.lib dxgi.lib user32.lib > _build.log 2>&1
"@
Remove-Item 'proxy_test.exe' -ErrorAction SilentlyContinue
$bat = Join-Path $here '_build.bat'
Set-Content -Path $bat -Value $cmd -Encoding ascii
cmd /c $bat
Get-Content '_build.log' | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item $bat,'_build.log','proxy_test.obj' -ErrorAction SilentlyContinue
if (Test-Path 'proxy_test.exe') { Write-Host 'built proxy_test.exe' } else { throw 'build failed' }
