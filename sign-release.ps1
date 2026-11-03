# Signs a release for the in-app updater (like Mechano DM's tools/sign-release.js).
#   .\sign-release.ps1 -Notes "What is new"     signs the newest "new v..." release folder -> update.txt
#   .\sign-release.ps1 -NewKey                   makes the signing key ONCE (prints the public key for the source)
# Upload update.txt to the GitHub release next to "Mechano Reader Setup.exe" (GitHub renames it
# "Mechano.Reader.Setup.exe": that is the name the app downloads from releases/latest).
#
# Safety: update.txt = the facts of the Setup (version, size, SHA-256, notes) + an ECDSA P-256 / SHA-256
# signature over those exact lines. The app only knows the PUBLIC key (src/core/update/Updater.cpp), so a
# changed update.txt or another Setup.exe is refused. The PRIVATE key stays in .private\ inside this project
# folder (never copied to GitHub, never in the installer). Keep a backup copy off this PC: losing it means
# installed copies cannot verify new releases (they would need one manual install).
param([string]$Notes = '', [switch]$NewKey, [string]$Folder = '', [string]$SetupUrl = '')   # -SetupUrl: local tests only
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Core
$root = $PSScriptRoot
$keyFile = Join-Path $root '.private\update-signing-key.blob'
$repo = 'Mechano0/mechano-reader'

if ($NewKey) {
    if (Test-Path $keyFile) { throw "A key already exists: $keyFile (delete it yourself only if you really want a new one)" }
    $p = New-Object System.Security.Cryptography.CngKeyCreationParameters
    $p.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
    # [NullString]: an EPHEMERAL key (PowerShell would turn $null into "" = a key saved in the Windows key store).
    $k = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256, [NullString]::Value, $p)
    New-Item -ItemType Directory -Force (Split-Path $keyFile) | Out-Null
    [IO.File]::WriteAllBytes($keyFile, $k.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob))
    $pub = [Convert]::ToBase64String($k.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob))
    Write-Host "Key saved: $keyFile" -ForegroundColor Green
    Write-Host "Public key (BCRYPT_ECCPUBLIC_BLOB, base64) for src/core/update/Updater.cpp:`n$pub"
    return
}
if (-not (Test-Path $keyFile)) { throw "No signing key: $keyFile (run .\sign-release.ps1 -NewKey once)" }

$rel = Join-Path (Split-Path $root -Parent) '02-final product\release'
if (-not $Folder) { $Folder = (Get-ChildItem $rel -Directory -Filter 'new v*' | Sort-Object LastWriteTime | Select-Object -Last 1).FullName }
$setup = Join-Path $Folder 'Mechano Reader Setup.exe'
if (-not (Test-Path $setup)) { throw "Setup not found: $setup" }
$vi = (Get-Item $setup).VersionInfo
$ver = '{0}.{1}.{2}' -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart
$size = (Get-Item $setup).Length
$sha = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
$oneLine = ($Notes -replace "`r", '' -replace "`n", '\n')
$body = "MechanoReaderUpdate 1`n" +
        "version=$ver`n" +
        "setup_url=$(if ($SetupUrl) { $SetupUrl } else { "https://github.com/$repo/releases/latest/download/Mechano.Reader.Setup.exe" })`n" +
        "setup_size=$size`n" +
        "setup_sha256=$sha`n" +
        "notes=$oneLine`n"
$bytes = [Text.Encoding]::UTF8.GetBytes($body)
$key = [System.Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($keyFile), [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
$ec = New-Object System.Security.Cryptography.ECDsaCng $key
$ec.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256
$sig = [Convert]::ToBase64String($ec.SignData($bytes))           # IEEE P1363: r || s, 64 bytes
$out = Join-Path $Folder 'update.txt'
[IO.File]::WriteAllText($out, $body + "signature=$sig`n", (New-Object Text.UTF8Encoding $false))
Write-Host "Signed: $out  (version $ver, $size bytes)" -ForegroundColor Green
