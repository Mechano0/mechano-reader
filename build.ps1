# Build MECHANO ZIP READER and run the tests.
#   .\build.ps1                 Release build + tests (builds 7z.dll from source first if needed)
#   .\build.ps1 -Config Debug
#   .\build.ps1 -Arch Win32     32-bit build for old PCs
#   .\build.ps1 -NoTests
param(
    [ValidateSet('Release','Debug')] [string]$Config = 'Release',
    [ValidateSet('x64','Win32')]     [string]$Arch   = 'x64',
    [switch]$NoTests
)
$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$build = Join-Path $root "build\$Arch"

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools not found. Install the "Desktop development with C++" workload.' }

# 1) 7z.dll from the pinned 7-Zip source (third_party\7zip-26.05), once per architecture.
$platform = if ($Arch -eq 'x64') { 'x64' } else { 'x86' }
$fmtDir   = Join-Path $root 'third_party\7zip-26.05\CPP\7zip\Bundles\Format7zF'
$dll      = Join-Path $fmtDir "$platform\7z.dll"
if (-not (Test-Path $dll)) {
    Write-Host "Building 7z.dll ($platform) from source..." -ForegroundColor Cyan
    $vcvars = Join-Path $vs ("VC\Auxiliary\Build\" + $(if ($Arch -eq 'x64') { 'vcvars64.bat' } else { 'vcvars32.bat' }))
    # vcvars looks for vswhere.exe on PATH and prints a harmless warning to stderr if it is missing;
    # with ErrorActionPreference=Stop PowerShell would treat that line as a failure. So: put the
    # Installer folder on PATH, and let only the exit code / the DLL decide success.
    $env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;" + $env:PATH
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$fmtDir`" && nmake /nologo PLATFORM=$platform 2>&1"
    $code = $LASTEXITCODE; $ErrorActionPreference = $eap
    if ($code -or -not (Test-Path $dll)) { throw '7z.dll build failed' }
}

# 2) CMake (PATH first, then the copy bundled with Visual Studio).
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }

& $cmake -S $root -B $build -G 'Visual Studio 17 2022' -A $Arch
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& $cmake --build $build --config $Config --parallel
if ($LASTEXITCODE) { throw 'Build failed' }

# 3) Tests.
if (-not $NoTests) {
    & (Join-Path $build "$Config\mzr_tests.exe")
    if ($LASTEXITCODE) { throw 'Tests failed' }
}
Write-Host "`nOK: $build\$Config\MechanoZip.exe" -ForegroundColor Green
