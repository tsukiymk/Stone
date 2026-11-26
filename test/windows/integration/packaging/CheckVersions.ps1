[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../../../..").Path
$work = Join-Path $root 'bin/x64/Debug/test/version'
New-Item -ItemType Directory -Path $work -Force | Out-Null
$resolver = Join-Path $root 'cmake/StoneVersion.cmake'
$result = Join-Path $work 'resolved.json'
$cases = @(
    @('1.0.0.0-preview01', '1.0.0.0', $true),
    @('v1.0.0.0-preview02', '1.0.0.0', $true),
    @('v1.0.0.0-preview03', '1.0.0.0', $true),
    @('1.0.0.0-preview08', '1.0.0.0', $true),
    @('1.0.0.0-preview998', '1.0.0.0', $true),
    @('1.0.0.0', '1.0.0.0', $false),
    @('1.0.1.0-preview01', '1.0.1.0', $true),
    @('1.0.65.0-preview03', '1.0.65.0', $true),
    @('2.3.42.0-preview03', '2.3.42.0', $true),
    @('255.255.65535.0-preview998', '255.255.65535.0', $true),
    @('255.255.65535.0', '255.255.65535.0', $false)
)
$previous = [version]'0.0.0.0'
foreach ($case in $cases) {
    & cmake "-DSTONE_VERSION=$($case[0])" "-DSTONE_VERSION_OUTPUT=$result" -P $resolver
    if ($LASTEXITCODE -ne 0) { throw "Version rejected: $($case[0])" }
    $identity = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
    if ($identity.version -cne ($case[0] -creplace '^v', '') -or
        $identity.packageVersion -cne $case[1] -or $identity.prerelease -ne $case[2]) {
        throw "Incorrect version identity: $($case[0])"
    }
    # MSI ignores revision: check ordering using only the first three fields.
    $numeric = [version]([version]$identity.packageVersion).ToString(3)
    if ($numeric -lt $previous) { throw 'Installer base version ordering regressed.' }
    $previous = $numeric
}
foreach ($invalid in @('1.0.0.0-preview00', '1.0.0.0-preview999', '1.0.65536.0', '256.0.0.0',
    '1.256.0.0', '01.0.0.0', '1.00.0.0', '1.0.0.1', '1.0.0.0-beta01', 'vv1.0.0.0',
    '1.0.0.0-preview01+build', '1.0.0.0-preview01;injected', '1.0.0.0-preview999999999999999999999')) {
    & cmake "-DSTONE_VERSION=$invalid" -P $resolver *> (Join-Path $work 'rejection.log')
    if ($LASTEXITCODE -eq 0) { throw "Invalid version accepted: $invalid" }
}
& cmake '-DSTONE_PACKAGE_VERSION=1.2.65535.0' "-DSTONE_VERSION_OUTPUT=$result" -P $resolver
if ($LASTEXITCODE -ne 0) { throw 'Legacy numeric version rejected.' }
$identity = Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
if ($identity.version -cne '1.2.65535.0' -or $identity.packageVersion -cne '1.2.65535.0' -or $identity.prerelease) {
    throw 'Legacy numeric version changed.'
}
Write-Host 'Version parsing, limits, prerelease identity, legacy input, and numeric base versions passed.'
