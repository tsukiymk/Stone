#requires -Version 7.2
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../../../..").Path
$work = Join-Path $root "bin/x64/Debug/test/gpg/$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $work -Force | Out-Null
# Keep the agent socket path short while retaining all temporary data under Debug.
$env:TEMP = Join-Path $root 'bin/x64/Debug/t'
New-Item -ItemType Directory -Path $env:TEMP -Force | Out-Null
$env:TMP = $env:TEMP
. "$root/tools/packaging/CertificateGpg.ps1"
$source = Join-Path $work 'input with spaces.pfx'
$encrypted = Join-Path $work 'certificate.pfx.gpg'
$plain = Join-Path $work 'recovered.pfx'
[IO.File]::WriteAllBytes($source, [Security.Cryptography.RandomNumberGenerator]::GetBytes(4096))
$passphrase = ConvertTo-SecureString ([Convert]::ToBase64String(
    [Security.Cryptography.RandomNumberGenerator]::GetBytes(32))) -AsPlainText -Force
Invoke-CertificateGpg -Mode Encrypt -InputFile $source -OutputFile $encrypted -Passphrase $passphrase
Invoke-CertificateGpg -Mode Decrypt -InputFile $encrypted -OutputFile $plain -Passphrase $passphrase
if ((Get-FileHash $source).Hash -cne (Get-FileHash $plain).Hash) { throw 'GPG round-trip changed certificate bytes.' }
$rejected = $false
try {
    Invoke-CertificateGpg -Mode Decrypt -InputFile $encrypted -OutputFile "$work/wrong.pfx" `
        -Passphrase (ConvertTo-SecureString 'deliberately incorrect test password' -AsPlainText -Force)
} catch { $rejected = $true }
if (!$rejected -or (Test-Path -LiteralPath "$work/wrong.pfx")) { throw 'Wrong passphrase accepted or partial plaintext retained.' }
$rejected = $false
try { Invoke-CertificateGpg -Mode Decrypt -InputFile $encrypted -OutputFile $plain -Passphrase $passphrase }
catch { $rejected = $true }
if (!$rejected -or (Get-FileHash $source).Hash -cne (Get-FileHash $plain).Hash) { throw 'Existing certificate output was replaced.' }
Write-Host 'GPG binary round-trip, wrong-passphrase cleanup, and overwrite rejection passed.'
