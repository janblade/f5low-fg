# Builds build\f5low.dll from src\: OptiScaler's F5Low frame generation (OptiScaler\plugins\f5low.dll).
# Needs Visual Studio 2022 (-VsPath, default D:\DEV\VS2022) and Intel's public XeSS-FG SDK headers, which are not
# copied here: -XessInc, else $env:F5LOW_XESS_INC, else an OptiScaler-NR checkout next to this repo
# (..\OptiScaler-NR\external\xess\inc\xess_fg).
#
# The DLL carries its version (src\xefg_version.h + this repo's commit count and commit): build from a committed tree,
# or the version says -dirty.
#
# -EmbedPack <xefg_pack folder>: builds a pack into the DLL (tools\pack_embed.py -> resource 101). The pack lives
# OUTSIDE this repo and is Intel-derived: a DLL built with it is for personal use only, never shared or committed.
# Without it the DLL has no pack (the own generator then needs an xefg_pack folder next to the DLL).
#
# Also writes build\own_blobs.txt: SHA-256 of every shader container we compile into the DLL, which
# tools\audit.py --dll accepts (anything else inside the DLL fails the audit).
param([string]$EmbedPack = '', [string]$Python = 'python', [string]$XessInc = '', [string]$VsPath = 'D:\DEV\VS2022')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'src'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
if ($EmbedPack) { $EmbedPack = (Resolve-Path $EmbedPack).Path }
Set-Location $out

$inc = @($XessInc, $env:F5LOW_XESS_INC, (Join-Path $root '..\OptiScaler-NR\external\xess\inc\xess_fg')) |
    Where-Object { $_ -and (Test-Path (Join-Path $_ 'xefg_swapchain.h')) } | Select-Object -First 1
if (-not $inc) { throw 'XeSS-FG SDK headers not found (-XessInc, F5LOW_XESS_INC or ..\OptiScaler-NR)' }
$inc = Resolve-Path $inc

# The version: MAJOR.MINOR.PATCH from the header, BUILD = this repo's commit count, the short commit, -dirty when
# tracked files have uncommitted changes. Written to _version_build.h for the compiler and to the version resource.
$header = Get-Content (Join-Path $src 'xefg_version.h') -Raw
$part = { param($n) if ($header -match "#define XEFG_CUDA_VERSION_$n (\d+)") { [int]$Matches[1] } else { throw "xefg_version.h: no $n" } }
$major = & $part 'MAJOR'; $minor = & $part 'MINOR'; $patch = & $part 'PATCH'
$build = [int](git -C $root rev-list --count HEAD)
$commit = (git -C $root rev-parse --short HEAD).Trim()
if (git -C $root status --porcelain --untracked-files=no) { $commit += '-dirty' }
$version = "$major.$minor.$patch.$build+$commit"
Set-Content -Path '_version_build.h' -Encoding ascii -Value "#define XEFG_CUDA_VERSION_TEXT `"$version`""

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
      VALUE "LegalCopyright", "GPL-3.0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@
if ($EmbedPack) {
    & $Python -I (Join-Path $root 'tools\pack_embed.py') $EmbedPack (Join-Path $out 'xefg_pack.bin')
    if ($LASTEXITCODE -ne 0) { throw 'pack_embed.py failed' }
    $rc += "`r`n101 RCDATA `"xefg_pack.bin`"`r`n"
}
Set-Content -Path '_res.rc' -Value $rc -Encoding ascii
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$installer = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
$cmd = @"
@echo off
cd /d "$out"
set "PATH=$installer;%PATH%"
call "$vcvars" >nul
type nul > _build.log
rc /nologo /fo _res.res _res.rc >> _build.log 2>&1
fxc /nologo /T cs_5_0 /E main /O3 /Fh xefg_convert_cs.h /Fo xefg_convert.cso /Vn g_xefgConvertCS "$src\xefg_convert.hlsl" >> _build.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$out" /I"$src" /I"$inc" /LD "$src\xefg_cuda.cpp" /link /OUT:f5low.dll d3d12.lib dxgi.lib cabinet.lib user32.lib _res.res >> _build.log 2>&1
"@
Remove-Item 'f5low.dll' -ErrorAction SilentlyContinue
Set-Content -Path '_build.bat' -Value $cmd -Encoding ascii
cmd /c (Join-Path $out '_build.bat')
Get-Content '_build.log' | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
if (Test-Path 'xefg_convert.cso') {
    $h = (Get-FileHash 'xefg_convert.cso' -Algorithm SHA256).Hash.ToLower()
    Set-Content -Path 'own_blobs.txt' -Encoding ascii -Value "$h  xefg_convert.cso (src\xefg_convert.hlsl, fxc cs_5_0)"
}
Remove-Item '_build.bat','_build.log','xefg_cuda.obj','f5low.exp','f5low.lib','xefg_cuda.exp','xefg_cuda.lib','_res.rc','_res.res','xefg_pack.bin','_version_build.h','xefg_convert.cso' -ErrorAction SilentlyContinue
if (Test-Path 'f5low.dll') {
    $what = if ($EmbedPack) { 'with a pack built in: PERSONAL USE ONLY' } else { 'without a pack' }
    Write-Host "built build\f5low.dll $version ($((Get-Item 'f5low.dll').Length) bytes, $what)"
} else { throw 'build failed' }
