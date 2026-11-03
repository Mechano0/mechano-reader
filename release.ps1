# Release build of Mechano Reader: installer + portable zip + checksums.
#   .\release.ps1              build (Release x64) + tests, then package
#   .\release.ps1 -SkipBuild   package the existing build\x64\Release output
# Output: ..\02-final product\release\
#   new v<ver> - <date>\                    (same layout as Mechano DM; an older "new ..." folder becomes "old ...")
#     Mechano Reader Setup.exe              ONE file to share: installs + opens the app (Inno Setup, per user, no admin)
#     Mechano Reader.zip                    portable copy (portable.flag inside)
#     SHA256SUMS.txt
#     update.txt                            signed facts for the in-app updater (upload it with the Setup)
# Version: assets\version.h (one place). Inno Setup: tools\InnoSetup6 (inside the project).
param([switch]$SkipBuild, [string]$Notes = '')   # -Notes: "what is new" for the update dialog
$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$bin     = Join-Path $root 'build\x64\Release'
$stage   = Join-Path $root 'build\stage'
$out     = Join-Path (Split-Path $root -Parent) '02-final product\release'
$iscc    = Join-Path $root 'tools\InnoSetup6\ISCC.exe'
if (-not (Test-Path $iscc)) { throw "Inno Setup not found: $iscc" }

# 1) Build + all tests (stops on any failure).
if (-not $SkipBuild) { & (Join-Path $root 'build.ps1') }

$exe = Join-Path $bin 'MechanoZip.exe'
$ver = (Get-Item $exe).VersionInfo.ProductVersion            # "0.9.0 Beta"
$fileVer = $ver.ToLower().Replace(' ', '-')                   # "0.9.0-beta"
Write-Host "Packaging Mechano Reader $ver" -ForegroundColor Cyan

# Versioned folder like Mechano DM: "new v0.9.0-beta - 2026-11-02"; other "new ..." folders are renamed "old ...".
$relRoot = $out
$out = Join-Path $relRoot ("new v$fileVer - " + (Get-Date -Format 'yyyy-MM-dd'))
New-Item -ItemType Directory -Force $relRoot | Out-Null
Get-ChildItem $relRoot -Directory -Filter 'new v*' | Where-Object { $_.FullName -ne $out } | ForEach-Object {
    $old = Join-Path $relRoot ('old' + $_.Name.Substring(3))
    if (-not (Test-Path $old)) { Rename-Item $_.FullName $old }
}

# 2) Stage the files that ship.
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage, $out | Out-Null
Copy-Item $exe, (Join-Path $bin '7z.dll') $stage
Copy-Item (Join-Path $root 'LICENSE.txt') $stage
Copy-Item (Join-Path $root 'installer\README.txt') $stage

# F7: DWG drawing view helper (LibreDWG dwg2dxf, GPL-3, runs as its own process) in "cad\".
$lw = Join-Path $root 'third_party\libredwg-0.14.8739'
$cadDir = Join-Path $stage 'cad'
New-Item -ItemType Directory -Force $cadDir | Out-Null
Copy-Item (Join-Path $lw 'win64\dwg2dxf.exe'), (Join-Path $lw 'win64\libredwg-0.dll'), (Join-Path $lw 'win64\libiconv-2.dll') $cadDir
Copy-Item (Join-Path $lw 'COPYING') (Join-Path $cadDir 'COPYING-GPL3.txt')
@"
GNU LibreDWG 0.14.8739 - dwg2dxf.exe (with libredwg-0.dll and libiconv-2.dll)
Used by Mechano Reader to show DWG drawings. It runs as a separate program; Mechano Zip
Reader itself is not linked with it.

License: GNU General Public License version 3 (COPYING-GPL3.txt). libiconv: GNU LGPL.
Source code (exact version): libredwg-0.14.8739.tar.xz in third_party/libredwg-0.14.8739 of the
Mechano Reader source on GitHub, and https://github.com/LibreDWG/libredwg/releases/tag/0.14.8739
(SHA-256 b4fecdfb236afa803a9f668624b930aaeebb1a0b349a6f77c6a5eef4e4c1fc27).
Binaries: the official LibreDWG 0.14.8739 win64 release, unchanged.
You may replace these files with your own build of the same programs.
"@ | Set-Content (Join-Path $cadDir 'README-LibreDWG.txt') -Encoding utf8

# THIRD-PARTY-NOTICES.txt: the license texts exactly as shipped by each project.
$tp = Join-Path $root 'third_party'
$sep = "`r`n" + ('=' * 79) + "`r`n"
$notices = @"
Third-party software in Mechano Reader
==========================================

1) 7-Zip 26.05 - 7z.dll
   Copyright (C) 1999-2026 Igor Pavlov. https://www.7-zip.org
   7z.dll is built from the 7-Zip 26.05 source code (folder third_party/7zip-26.05 of the
   Mechano Reader source on GitHub; also available from https://www.7-zip.org).
   It is a separate DLL loaded at run time, so it can be replaced by the user.
   Licenses: GNU LGPL 2.1 or later; the RAR decoder is under the GNU LGPL with the
   "unRAR license restriction"; some files BSD 2/3-clause or public domain (details below).

2) pugixml 1.16 - XML parser used for the Word / Excel / PowerPoint preview
   Copyright (c) 2006-2026 Arseny Kapoulkine. https://pugixml.org. MIT License.

3) GNU LibreDWG 0.14.8739 - cad\dwg2dxf.exe, cad\libredwg-0.dll (+ cad\libiconv-2.dll, GNU LGPL)
   Copyright (C) Free Software Foundation, Inc. https://www.gnu.org/software/libredwg/
   Converts DWG drawings to DXF for the drawing view. A separate program (own process),
   not linked into Mechano Reader. License: GNU GPL version 3 - full text in
   cad\COPYING-GPL3.txt; where to get the source: cad\README-LibreDWG.txt.
"@
$parts = @(
    @('7-Zip License.txt', (Join-Path $tp '7zip-26.05\DOC\License.txt')),
    @('7-Zip unRarLicense.txt', (Join-Path $tp '7zip-26.05\DOC\unRarLicense.txt')),
    @('GNU LGPL 2.1 (7-Zip copying.txt)', (Join-Path $tp '7zip-26.05\DOC\copying.txt')),
    @('pugixml LICENSE.md', (Join-Path $tp 'pugixml-1.16\LICENSE.md')))
foreach ($p in $parts) { $notices += $sep + $p[0] + $sep + (Get-Content $p[1] -Raw) }
Set-Content (Join-Path $stage 'THIRD-PARTY-NOTICES.txt') $notices -Encoding utf8

# 3) Installer (+ uninstaller) with Inno Setup.
& $iscc /Q "/DStage=$stage" "/DOutDir=$out" (Join-Path $root 'installer\MechanoZip.iss')
if ($LASTEXITCODE) { throw 'Inno Setup failed' }
$setup = Join-Path $out "Mechano Reader Setup.exe"
if (-not (Test-Path $setup)) { throw "Installer not produced: $setup" }

# 4) Portable zip: same files + portable.flag (settings next to the exe, no registry).
$pdir = Join-Path $root 'build\stage-portable\Mechano Reader'
if (Test-Path (Split-Path $pdir)) { Remove-Item (Split-Path $pdir) -Recurse -Force }
New-Item -ItemType Directory -Force $pdir | Out-Null
Copy-Item (Join-Path $stage '*') $pdir -Recurse
Set-Content (Join-Path $pdir 'portable.flag') "This file switches Mechano Reader to portable mode (settings in the Data folder, no registry). Delete it to use the normal per-user settings." -Encoding ascii
$zip = Join-Path $out "Mechano Reader.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $pdir -DestinationPath $zip -CompressionLevel Optimal

# 5) Checksums (users can verify the download).
$sums = foreach ($f in $setup, $zip) { '{0}  {1}' -f (Get-FileHash $f -Algorithm SHA256).Hash.ToLower(), (Split-Path $f -Leaf) }
Set-Content (Join-Path $out 'SHA256SUMS.txt') $sums -Encoding ascii

# 6) update.txt for the in-app updater (signed with the key in .private\; skipped on a PC without the key).
if (Test-Path (Join-Path $root '.private\update-signing-key.blob')) {
    & (Join-Path $root 'sign-release.ps1') -Folder $out -Notes $Notes
} else { Write-Host "No update signing key: update.txt not made." -ForegroundColor Yellow }

Write-Host "`nRelease files in: $out" -ForegroundColor Green
Get-ChildItem $out | Format-Table Name, @{n='Size (KB)'; e={[math]::Round($_.Length / 1KB)}} -AutoSize
