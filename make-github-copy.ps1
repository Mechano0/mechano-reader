# Refreshes the clean source copy for GitHub in ..\02-final product\github (no build output,
# no internal notes, no tools). Safe to run again: the target is mirrored (files removed from
# 01-wip disappear there too). It does NOT run git and does NOT upload anything.
$ErrorActionPreference = 'Stop'
$src = $PSScriptRoot
$dst = Join-Path (Split-Path $src -Parent) '02-final product\github'
New-Item -ItemType Directory -Force $dst | Out-Null

function Mirror($rel, [string[]]$xd = @(), [string[]]$xf = @()) {
    $from = Join-Path $src $rel; $to = Join-Path $dst $rel
    $a = @($from, $to, '/MIR', '/NFL', '/NDL', '/NJH', '/NJS', '/NP', '/R:1', '/W:1')
    if ($xd) { $a += '/XD'; $a += $xd }
    if ($xf) { $a += '/XF'; $a += $xf }
    & robocopy @a | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $rel ($LASTEXITCODE)" }
}

Mirror 'src'
Mirror 'tests'
Mirror 'benchmarks'
Mirror 'assets' @() @('*.aps')
Mirror 'installer' @() @('README-github.md')
Mirror 'docs' @((Join-Path $src 'docs\research'))          # research notes stay private
# Third-party SOURCE only (7z.dll is LGPL: its source ships with ours). Build output excluded;
# Scintilla / Lexilla are not used by the build, so they are left out.
Mirror 'third_party\7zip-26.05' @((Join-Path $src 'third_party\7zip-26.05\CPP\7zip\Bundles\Format7zF\x64'),
                                  (Join-Path $src 'third_party\7zip-26.05\CPP\7zip\Bundles\Format7zF\x86')) @('*.obj', '*.res', '*.pdb', '*.exp')
Mirror 'third_party\pugixml-1.16'
# LibreDWG: the exact GPL source tarball + license go in; the 35 MB Windows binaries do not (README says where).
Mirror 'third_party\libredwg-0.14.8739' @((Join-Path $src 'third_party\libredwg-0.14.8739\win64'))

foreach ($f in 'CMakeLists.txt', 'build.ps1', 'release.ps1', 'sign-release.ps1', 'make-github-copy.ps1', 'LICENSE.txt') {
    Copy-Item (Join-Path $src $f) $dst -Force
}
Copy-Item (Join-Path $src 'installer\README-github.md') (Join-Path $dst 'README.md') -Force
@'
build/
out/
.vs/
*.user
*.aps
tools/
.private/
third_party/7zip-26.05/CPP/7zip/Bundles/Format7zF/x64/
third_party/7zip-26.05/CPP/7zip/Bundles/Format7zF/x86/
'@ | Set-Content (Join-Path $dst '.gitignore') -Encoding ascii

$size = (Get-ChildItem $dst -Recurse -File | Measure-Object Length -Sum)
Write-Host ("GitHub copy ready: {0}  ({1} files, {2:N1} MB)" -f $dst, $size.Count, ($size.Sum / 1MB)) -ForegroundColor Green

# robocopy leaves exit code 1 ("files copied") behind: that is success.
exit 0
