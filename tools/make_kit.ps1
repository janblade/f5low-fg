# Builds the release kit: f5low-kit-<version>.zip (+ .sha256) with f5low.dll, Intel's libxess_fg.dll, both licences and a
# README. NOT part of CI: Intel's DLL is not in this repo. The Intel DLL and licence are read from the paths you give.
#   powershell -File tools\make_kit.ps1 -IntelLib <libxess_fg.dll 1.3.4.95> -XessLicense <XeSS LICENSE.txt> -Out H:\Temp\f5low_kit
param([Parameter(Mandatory)][string]$IntelLib, [Parameter(Mandatory)][string]$XessLicense, [Parameter(Mandatory)][string]$Out,
      [string]$Dll = '', [switch]$AllowDirty)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Dll) { $Dll = Join-Path $root 'build\f5low.dll' }
# libxess_fg.dll 1.3.4.95, the build the own generator's pack is made from (SHA-256 of the file Intel signed)
$intelSha256 = '752B1491350A29FB2850FA4D0D14F62D79980E0B3CD067D147BB2469C1EC3182'
foreach ($f in $Dll, $IntelLib, $XessLicense) { if (-not (Test-Path $f)) { throw "missing: $f" } }
$Dll = (Resolve-Path $Dll).Path; $IntelLib = (Resolve-Path $IntelLib).Path; $XessLicense = (Resolve-Path $XessLicense).Path

$hash = (Get-FileHash $IntelLib -Algorithm SHA256).Hash
if ($hash -ne $intelSha256) { throw "libxess_fg.dll SHA-256 $hash is not the expected $intelSha256" }
$sig = Get-AuthenticodeSignature $IntelLib
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'CN=Intel Corporation(,|$)') {
    throw "libxess_fg.dll is not validly signed by Intel Corporation (status $($sig.Status))"
}
$version = (Get-Item $Dll).VersionInfo.FileVersion
if (-not $version) { throw "$Dll has no FileVersion" }
# The source line in README-KIT must be true: the DLL's FileVersion ends in +<shortsha>[-dirty] (build.ps1); that commit
# must be HEAD of this checkout and the DLL must not come from a dirty tree.
$commit = (& git -C $root rev-parse HEAD).Trim()
$short = if ($version -match '\+([0-9a-f]{7,40})(-dirty)?$') { $Matches[1] } else { '' }
$dirty = $version -match '-dirty$'
$problem = ''
if (-not $short) { $problem = "FileVersion '$version' carries no +<commit>" }
elseif ($dirty) { $problem = "f5low.dll was built from a dirty tree ($version)" }
elseif (-not $commit.StartsWith($short)) { $problem = "f5low.dll is built from $short, this checkout is at $($commit.Substring(0, 7))" }
$notForRelease = $false
if ($problem) {
    if (-not $AllowDirty) { throw "$problem. Build from a clean, committed tree (build.ps1), or pass -AllowDirty for a local test kit." }
    Write-Warning "$problem - NOT FOR RELEASE"
    $notForRelease = $true
}
if (-not $notForRelease) {
    $st = (& git -C $root status --porcelain --untracked-files=no)
    if ($st) { throw "the checkout has uncommitted changes (the kit's licences and README come from it); commit them or pass -AllowDirty" }
}
$tag = if ($notForRelease) { "$version-NOT-FOR-RELEASE" } else { $version }

New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$stage = Join-Path $Out "stage-$tag"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage | Out-Null

Copy-Item $Dll (Join-Path $stage 'f5low.dll')
Copy-Item $IntelLib (Join-Path $stage 'libxess_fg.dll')
Copy-Item $XessLicense (Join-Path $stage 'XeSS_LICENSE.txt')
# f5low.dll statically links zstd (third_party\zstd, build.ps1) - the only third-party code compiled in; its BSD notice ships
Copy-Item (Join-Path $root 'third_party\zstd\LICENSE') (Join-Path $stage 'zstd_LICENSE.txt')
# LICENSE = GPL-3.0 text, then the additional permission
$nl = [Environment]::NewLine
$text = (Get-Content (Join-Path $root 'LICENSE') -Raw) + $nl + ('=' * 78) + $nl + $nl + (Get-Content (Join-Path $root 'LICENSE-EXCEPTION.md') -Raw)
[IO.File]::WriteAllText((Join-Path $stage 'LICENSE'), $text, (New-Object Text.UTF8Encoding($false)))

$banner = if ($notForRelease) { "*** NOT FOR RELEASE: local test kit ($problem) ***$nl$nl" } else { '' }
$readme = @"
${banner}f5low $version
=================

f5low.dll is the frame generation host behind OptiScaler-NR's F5Low FG output (our own pacer, gate and presenter, with
our own XeFG-compatible generator for NVIDIA GPUs). On the first run it builds its working data from libxess_fg.dll
on your PC (about half a second, once; cached in plugins\f5low_cache, or in %LOCALAPPDATA%\f5low\f5low_cache when the
plugins folder is not writable), then runs on its own.

Source: https://github.com/janblade/f5low-fg  (commit $commit)
Licence: GPL-3.0 with a linking exception, see LICENSE. libxess_fg.dll is Intel's, unchanged and signed by Intel
Corporation (version 1.3.4.95); it stays under Intel's licence, see XeSS_LICENSE.txt. It is not covered by the GPL.

Files
  f5low.dll        this project
  libxess_fg.dll   Intel XeSS frame generation 1.3.4.95 (SHA-256 $intelSha256)
  LICENSE          GPL-3.0 + additional permission (section 7)
  XeSS_LICENSE.txt Intel's licence for libxess_fg.dll
  zstd_LICENSE.txt BSD notice of zstd, which f5low.dll contains (statically linked)
  kit_hashes.txt   SHA-256 of every file above (OptiScaler's package_release.ps1 checks them)

How OptiScaler uses it
  f5low.dll       -> <game>\OptiScaler\plugins\f5low.dll
  libxess_fg.dll  -> <game>\OptiScaler\libxess_fg.dll
  OptiScaler's FG Output "F5Low" then loads f5low.dll; if the own generator cannot run on your PC (older GPU, another
  vendor's GPU, a libxess_fg.dll build it does not know) it hands the frames to Intel's library instead.
"@
[IO.File]::WriteAllText((Join-Path $stage 'README-KIT.txt'), ($readme -replace "`r?`n", $nl), (New-Object Text.UTF8Encoding($false)))
# kit_hashes.txt: "<sha256>  <file>" per file, lower case (package_release.ps1 checks f5low.dll and libxess_fg.dll by it)
$lines = Get-ChildItem $stage -File | Sort-Object Name |
    ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)" }
Set-Content -Path (Join-Path $stage 'kit_hashes.txt') -Value $lines -Encoding ascii

$zip = Join-Path $Out "f5low-kit-$tag.zip"
Remove-Item $zip, "$zip.sha256" -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$zh = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$zip.sha256" -Value "$zh  $(Split-Path -Leaf $zip)" -Encoding ascii
Remove-Item $stage -Recurse -Force
Write-Host "kit: $zip"
Write-Host "sha256: $zh"
