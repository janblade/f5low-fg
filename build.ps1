# Builds build\f5low.dll from src\: OptiScaler's F5Low frame generation (OptiScaler\plugins\f5low.dll).
# Needs Visual Studio 2022 (-VsPath, default D:\DEV\VS2022) and Intel's public XeSS-FG SDK headers, which are not
# copied here: -XessInc, else $env:F5LOW_XESS_INC, else an OptiScaler-NR checkout next to this repo
# (..\OptiScaler-NR\external\xess\inc\xess_fg). The dlssg generator also needs NVIDIA's public Streamline SDK headers (headers
# only, MIT: github.com/NVIDIA-RTX/Streamline, include\): -StreamlineInc, else $env:F5LOW_STREAMLINE_INC, else the same OptiScaler-NR
# checkout (..\OptiScaler-NR\external\streamline).
#
# The DLL carries its version (src\xefg_version.h + this repo's commit count and commit): build from a committed tree,
# or the version says -dirty.
#
# -EmbedPack <xefg_pack folder>: builds a pack into the DLL (tools\pack_embed.py -> resource 101). The pack lives
# OUTSIDE this repo and is Intel-derived: a DLL built with it is for personal use only, never shared or committed.
# Without it the DLL has no pack (the own generator then needs an xefg_pack folder next to the DLL).
#
# Always embedded (resource 102, plain text): our recipe (recipe\i0) and the kernels WE generate at build time
# (tools\recipe\build_own.py: tools\kernels generators, and our HLSL templates through dxc and src\translate.cpp via
# build\xlate_tool.exe) plus the noise texture. At run time the DLL adds the Intel parts from the player's own
# libxess_fg.dll (their DXBC modules, weights, root constants - nothing is translated, no dxcompiler.dll is needed or
# loaded) and caches the pack (src\pack_build.cpp). The translator (src\translate.cpp, src\dxc_disasm.cpp) is a
# BUILD-time tool and a test oracle only; it is not linked into f5low.dll. Needs dxc.exe at BUILD time (-Dxc,
# $env:XEFG_DXC, or the WinGet Microsoft.DirectX.ShaderCompiler package) and Python with numpy.
#
# Also writes build\own_blobs.txt: SHA-256 of every shader container we compile into the DLL and the SHA-256 of every
# file we put into resource 102 (our generated PTX among them), which tools\audit.py --dll accepts (anything else inside the DLL fails the audit).
param([string]$EmbedPack = '', [string]$Python = 'python', [string]$XessInc = '', [string]$VsPath = 'D:\DEV\VS2022', [string]$Dxc = '',
      [string]$StreamlineInc = '', [switch]$TuningTests)
$ErrorActionPreference = 'Stop'
$tuneDef = if ($TuningTests) { '/DF5LOW_TUNING_TESTS' } else { '' }       # -TuningTests: the co-runner knob (XEFG_CUDA_CORUN), never in a release
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $root 'src'
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null
if ($EmbedPack) { $EmbedPack = (Resolve-Path $EmbedPack).Path }
$sdkDxc = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\dxc.exe" -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
$Dxc = @($Dxc, $env:XEFG_DXC, (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Microsoft.DirectX.ShaderCompiler_Microsoft.Winget.Source_8wekyb3d8bbwe\bin\x64\dxc.exe'), $sdkDxc) |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $Dxc) { throw 'dxc.exe not found (needed at build time for our HLSL templates): -Dxc or XEFG_DXC' }
Set-Location $out

$inc = @($XessInc, $env:F5LOW_XESS_INC, (Join-Path $root '..\OptiScaler-NR\external\xess\inc\xess_fg')) |
    Where-Object { $_ -and (Test-Path (Join-Path $_ 'xefg_swapchain.h')) } | Select-Object -First 1
if (-not $inc) { throw 'XeSS-FG SDK headers not found (-XessInc, F5LOW_XESS_INC or ..\OptiScaler-NR)' }
$inc = Resolve-Path $inc
$slInc = @($StreamlineInc, $env:F5LOW_STREAMLINE_INC, (Join-Path $root '..\OptiScaler-NR\external\streamline')) |
    Where-Object { $_ -and (Test-Path (Join-Path $_ 'sl.h')) } | Select-Object -First 1
if (-not $slInc) { throw 'Streamline SDK headers not found (-StreamlineInc, F5LOW_STREAMLINE_INC or ..\OptiScaler-NR\external\streamline)' }
$slInc = Resolve-Path $slInc

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
$vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
$installer = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
# Step 1: the build tool that turns our HLSL templates' disassembly into PTX with the run-time translator
$cmd1 = @"
@echo off
cd /d "$out"
set "PATH=$installer;%PATH%"
call "$vcvars" >nul
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG /I"$src" "$root\tools\xlate_tool.cpp" "$src\translate.cpp" /link /OUT:xlate_tool.exe > _build_tool.log 2>&1
"@
Set-Content -Path '_build_tool.bat' -Value $cmd1 -Encoding ascii
Remove-Item 'xlate_tool.exe' -ErrorAction SilentlyContinue
cmd /c (Join-Path $out '_build_tool.bat')
Get-Content '_build_tool.log' | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
if (-not (Test-Path 'xlate_tool.exe')) { throw 'building xlate_tool failed' }
# Step 2: our part of the pack (generators + templates + noise) as one plain-text resource
& $Python (Join-Path $root 'tools\recipe\build_own.py') --out (Join-Path $out 'own') --xlate (Join-Path $out 'xlate_tool.exe') --dxc $Dxc --recipe (Join-Path $root 'recipe\i0')
if ($LASTEXITCODE -ne 0) { throw 'build_own.py failed' }
$rc += "`r`n102 RCDATA `"own\own_bundle.bin`"`r`n"
Set-Content -Path '_res.rc' -Value $rc -Encoding ascii
$cmd = @"
@echo off
cd /d "$out"
set "PATH=$installer;%PATH%"
call "$vcvars" >nul
type nul > _build.log
rc /nologo /fo _res.res _res.rc >> _build.log 2>&1
fxc /nologo /T cs_5_0 /E main /O3 /Fh xefg_convert_cs.h /Fo xefg_convert.cso /Vn g_xefgConvertCS "$src\xefg_convert.hlsl" >> _build.log 2>&1
cl /nologo /c /TC /w /O2 /MT /DNDEBUG /Fo_zstd.obj "$root\third_party\zstd\zstddeclib.c" >> _build.log 2>&1
cl /nologo /c /TC /w /O2 /MT /DNDEBUG /Fo_hde.obj "$src\unlock\hde\hde64.c" >> _build.log 2>&1
cl /nologo /std:c++20 /EHsc /W4 /O2 /MT /DNDEBUG $tuneDef /I"$out" /I"$src" /I"$inc" /I"$slInc" /I"$root\third_party\zstd" /LD "$src\xefg_cuda.cpp" "$src\intel_modules.cpp" "$src\pack_build.cpp" "$src\intel_file_source.cpp" _zstd.obj _hde.obj /link /OUT:f5low.dll d3d12.lib dxgi.lib cabinet.lib user32.lib bcrypt.lib _res.res >> _build.log 2>&1
"@
Remove-Item 'f5low.dll' -ErrorAction SilentlyContinue
Set-Content -Path '_build.bat' -Value $cmd -Encoding ascii
cmd /c (Join-Path $out '_build.bat')
Get-Content '_build.log' | Select-String -Pattern 'error|warning' | ForEach-Object { $_.Line }
if (Test-Path 'xefg_convert.cso') {
    $h = (Get-FileHash 'xefg_convert.cso' -Algorithm SHA256).Hash.ToLower()
    Set-Content -Path 'own_blobs.txt' -Encoding ascii -Value "$h  xefg_convert.cso (src\xefg_convert.hlsl, fxc cs_5_0)"
    Add-Content -Path 'own_blobs.txt' -Encoding ascii -Value (Get-Content 'own\own_files.txt')
}
Remove-Item '_build.bat','_build.log','xefg_cuda.obj','intel_modules.obj','translate.obj','_zstd.obj','_hde.obj','f5low.exp','f5low.lib','xefg_cuda.exp','xefg_cuda.lib','_res.rc','_res.res','xefg_pack.bin','_version_build.h','xefg_convert.cso','pack_build.obj','intel_file_source.obj','xlate_tool.exe','xlate_tool.obj','_build_tool.bat','_build_tool.log' -ErrorAction SilentlyContinue
Remove-Item 'own' -Recurse -ErrorAction SilentlyContinue
if (Test-Path 'f5low.dll') {
    $what = if ($EmbedPack) { 'with a pack built in: PERSONAL USE ONLY' } else { 'without a pack' }
    Write-Host "built build\f5low.dll $version ($((Get-Item 'f5low.dll').Length) bytes, $what)"
} else { throw 'build failed' }
