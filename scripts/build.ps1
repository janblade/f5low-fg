# Builds f5low.dll from xefg_cuda.cpp: OptiScaler's F5Low frame generation (OptiScaler\plugins\f5low.dll).
# Needs Visual Studio 2022 at D:\DEV\VS2022 and the OptiScaler-NR checkout next
# to this repo (for Intel's public XeSS-FG SDK headers, which are not copied here).
#
# The DLL carries its version (xefg_version.h + this repo's commit count and commit): build from a committed tree, or
# the version says -dirty.
#
# -EmbedPack <xefg_pack folder>: builds the pack into the DLL (pack_embed.py -> resource 101), so the one file
# is all a game needs ([Libraries] XeFGPath). An xefg_pack folder next to the DLL still wins over it. The pack
# is Intel's network translated: a DLL built with it is for personal use only, never shared or committed.
param([string]$EmbedPack = '', [string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$inc = @('..\..\..\OptiScaler-NR\external\xess\inc\xess_fg',
         '..\..\..\..\OptiScaler-NR\OptiScaler-NR\external\xess\inc\xess_fg') |
    ForEach-Object { Join-Path $here $_ } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $inc) { throw 'OptiScaler-NR checkout (XeSS-FG SDK headers) not found' }
$inc = Resolve-Path $inc
# The version (xefg_version.h): MAJOR.MINOR.PATCH from the header, BUILD = this repo's commit count, the short commit,
# -dirty when tracked files have uncommitted changes. Written to _version_build.h for the compiler and to the version
# resource below; both files are removed after the build.
$header = Get-Content (Join-Path $here 'xefg_version.h') -Raw
$part = { param($n) if ($header -match "#define XEFG_CUDA_VERSION_$n (\d+)") { [int]$Matches[1] } else { throw "xefg_version.h: no $n" } }
$major = & $part 'MAJOR'; $minor = & $part 'MINOR'; $patch = & $part 'PATCH'
$build = [int](git -C $here rev-list --count HEAD)
$commit = (git -C $here rev-parse --short HEAD).Trim()
if (git -C $here status --porcelain --untracked-files=no) { $commit += '-dirty' }
$version = "$major.$minor.$patch.$build+$commit"
Set-Content -Path (Join-Path $here '_version_build.h') -Encoding ascii -Value "#define XEFG_CUDA_VERSION_TEXT `"$version`""

$rc = @"
1 VERSIONINFO
FILEVERSION $major,$minor,$patch,$build
PRODUCTVERSION $major,$minor,$patch,$build
FILEOS 0x40004
FILETYPE 0x2
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "CompanyName", "OptiScaler-NR"
      VALUE "FileDescription", "F5Low frame generation for OptiScaler (XeSS-FG API)"
      VALUE "FileVersion", "$version"
      VALUE "InternalName", "f5low"
      VALUE "OriginalFilename", "f5low.dll"
      VALUE "ProductName", "F5Low"
      VALUE "ProductVersion", "$version"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
if ($EmbedPack) {
    & $Python -I (Join-Path $here 'pack_embed.py') $EmbedPack (Join-Path $here 'xefg_pack.bin')
    if ($LASTEXITCODE -ne 0) { throw 'pack_embed.py failed' }
    $rc += "`r`n101 RCDATA `"xefg_pack.bin`"`r`n"
}
Set-Content -Path (Join-Path $here '_res.rc') -Value $rc -Encoding ascii
$rcLine = 'rc /nologo /fo _res.res _res.rc >> _build.log 2>&1'
$res = '_res.res'
$vcvars = 'D:\DEV\VS2022\VC\Auxiliary\Build\vcvars64.bat'
$installer = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$cmd = @"
@echo off
set "PATH=$installer;%PATH%"
call "$vcvars" >nul
type nul > _build.log
$rcLine
fxc /nologo /T cs_5_0 /E main /O3 /Fh xefg_convert_cs.h /Vn g_xefgConvertCS xefg_convert.hlsl >> _build.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$inc" /LD xefg_cuda.cpp /link /OUT:f5low.dll d3d12.lib dxgi.lib cabinet.lib user32.lib $res >> _build.log 2>&1
"@
Remove-Item 'f5low.dll' -ErrorAction SilentlyContinue
$bat = Join-Path $here '_build.bat'
Set-Content -Path $bat -Value $cmd -Encoding ascii
cmd /c $bat
Get-Content '_build.log' | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
Remove-Item $bat,'_build.log','xefg_cuda.obj','f5low.exp','f5low.lib','_res.rc','_res.res','xefg_pack.bin','_version_build.h' -ErrorAction SilentlyContinue
if (Test-Path 'f5low.dll') {
    $what = if ($EmbedPack) { 'with the pack built in' } else { 'without a pack' }
    Write-Host "built f5low.dll $version ($((Get-Item 'f5low.dll').Length) bytes, $what)"
} else { throw 'build failed' }
