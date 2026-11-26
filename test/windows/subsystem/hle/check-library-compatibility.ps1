[CmdletBinding(DefaultParameterSetName = 'Catalog')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Apk')][string] $Apk,
    [Parameter(Mandatory, ParameterSetName = 'Apk')][string] $Aapt2,
    [Parameter(Mandatory, ParameterSetName = 'Library')][string] $Library,
    [Parameter(Mandatory, ParameterSetName = 'Library')][string] $PackageName,
    [Parameter(Mandatory, ParameterSetName = 'Library')][string] $Version,
    [Parameter(Mandatory, ParameterSetName = 'Library')]
    [Parameter(ParameterSetName = 'Apk')][string] $LibraryFile
)

$ErrorActionPreference = 'Stop'

function Test-VersionInRange($Entry) {
    $range = $Entry.compatibility
    if ($Entry.versionSource -eq 'elf-build-id') {
        return $range.minVersion -ceq $Entry.version -and $range.maxVersion -ceq $Entry.version
    }
    $parts = @($range.minVersion, $Entry.version, $range.maxVersion)
    if ($parts -notmatch '^\d+(\.\d+){0,3}$') {
        return $range.minVersion -ceq $Entry.version -and $range.maxVersion -ceq $Entry.version
    }
    $versions = foreach ($part in $parts) {
        if ($part -notmatch '\.') { $part += '.0' }
        [version] $part
    }
    return $versions[0] -le $versions[1] -and $versions[1] -le $versions[2]
}

function Confirm-Hash($Entry, [string] $Actual) {
    if ($Entry.sha256 -cne $Actual.ToLowerInvariant()) {
        throw "SHA-256 mismatch for $($Entry.packageName) $($Entry.version) $($Entry.libraryFile). Actual $Actual"
    }
    $scope = if ($Entry.compatibility) {
        "$($Entry.compatibility.minVersion)..$($Entry.compatibility.maxVersion) ($($Entry.compatibility.scope -join ', '))"
    } else { 'pending behavior verification' }
    Write-Output "$($Entry.packageName) $($Entry.version) $($Entry.libraryFile) SHA-256 matched; compatibility $scope"
}

function Test-ScopeArray($Scopes) {
    if ($Scopes -isnot [array] -or !$Scopes.Count) { return $false }
    foreach ($scopeItem in $Scopes) {
        if ($scopeItem -isnot [string] -or [string]::IsNullOrWhiteSpace($scopeItem)) { return $false }
    }
    return $true
}

function Get-LibrarySha256([IO.Stream] $Stream) {
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha256.ComputeHash($Stream)).Replace('-', '').ToLowerInvariant() }
    finally { $sha256.Dispose() }
}

function Get-LibraryAbi([IO.Stream] $Stream) {
    $header = New-Object byte[] 20
    if ($Stream.Read($header, 0, $header.Length) -ne $header.Length -or
        $header[0] -ne 0x7f -or $header[1] -ne 0x45 -or $header[2] -ne 0x4c -or $header[3] -ne 0x46 -or
        $header[4] -ne 2 -or $header[5] -ne 1 -or $header[6] -ne 1 -or $header[16] -ne 3 -or $header[17] -ne 0) {
        throw 'Expected a little-endian ELF64 shared library.'
    }
    $Stream.Position = 0
    switch ([int]$header[18] -bor ([int]$header[19] -shl 8)) {
        62 { return 'x86_64' }
        183 { return 'arm64-v8a' }
        default { throw 'Unsupported ELF library architecture.' }
    }
}

$catalog = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'library-compatibility.json') -Raw | ConvertFrom-Json
if ($catalog -isnot [pscustomobject] -or @($catalog.PSObject.Properties).Count -ne 2 -or
    $catalog.system -isnot [pscustomobject] -or $catalog.external -isnot [pscustomobject]) {
    throw 'Expected system and external objects in the library catalog.'
}
$entries = @(foreach ($category in 'system', 'external') {
    foreach ($group in $catalog.$category.PSObject.Properties) {
        if ($group.Name -cnotmatch '^lib[^/\\]+\.so$' -or
            $group.Value -isnot [array] -or !$group.Value.Count) {
            throw "Invalid library catalog group: $($group.Name)"
        }
        foreach ($entry in $group.Value) {
            if ($entry -isnot [pscustomobject]) { throw "Invalid library catalog record: $($group.Name)" }
            if ($category -ceq 'system' -and !$entry.PSObject.Properties['packageName']) {
                $entry | Add-Member -NotePropertyName packageName -NotePropertyValue android
            }
            $path = if ($category -ceq 'system' -or
                ($entry.packageName -ceq 'android' -and $entry.versionSource -cne 'apk-version-name')) {
                "lib64/$($group.Name)"
            } else {
                "lib/$($entry.abi)/$($group.Name)"
            }
            $entry | Add-Member -NotePropertyName libraryFile -NotePropertyValue $path -PassThru
        }
    }
})
if (!$entries.Count) { throw 'Empty library catalog.' }
$keys = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($entry in $entries) {
    if (!$entry.packageName -or !$entry.version -or $entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $entry.versionSource -cnotin @('apk-version-name', 'android-api', 'elf-build-id', 'library-release') -or
        $entry.abi -cnotin @('x86_64', 'arm64-v8a')) {
        throw "Invalid library catalog entry: $($entry.packageName) $($entry.libraryFile)"
    }
    if (!$keys.Add("$($entry.packageName)|$($entry.abi)|$($entry.version)|$($entry.libraryFile)")) {
        throw "Duplicate library catalog entry for $($entry.packageName) $($entry.version) $($entry.libraryFile)"
    }
    if ($null -ne $entry.compatibility -and
        (!$entry.compatibility.minVersion -or !$entry.compatibility.maxVersion -or !(Test-ScopeArray $entry.compatibility.scope) -or
        !(Test-VersionInRange $entry))) {
        throw "Invalid compatibility range for $($entry.libraryFile) $($entry.version)"
    }
}

if ($PSCmdlet.ParameterSetName -eq 'Catalog') {
    Write-Output "Validated $($entries.Count) library catalog records."
    return
}

if ($PSCmdlet.ParameterSetName -eq 'Library') {
    if ([IO.Path]::GetFileName($Library) -cne [IO.Path]::GetFileName($LibraryFile)) {
        throw "Expected library file $([IO.Path]::GetFileName($LibraryFile))"
    }
    $stream = [IO.File]::OpenRead((Resolve-Path -LiteralPath $Library).Path)
    try {
        $abi = Get-LibraryAbi $stream
        $candidates = @($entries | Where-Object {
            $_.packageName -ceq $PackageName -and $_.version -ceq $Version -and $_.abi -ceq $abi -and
            ($_.libraryFile -ceq $LibraryFile -or [IO.Path]::GetFileName($_.libraryFile) -ceq $LibraryFile)
        })
        if (!$candidates.Count) { throw "No catalog record for $PackageName $abi $Version $LibraryFile" }
        if ($candidates.Count -gt 1) { throw "Multiple records for $PackageName $abi $Version $LibraryFile. Pass the full library path." }
        Confirm-Hash $candidates[0] (Get-LibrarySha256 $stream)
    }
    finally { $stream.Dispose() }
    return
}

$apkPath = (Resolve-Path -LiteralPath $Apk).Path
$aaptPath = (Resolve-Path -LiteralPath $Aapt2).Path
try {
    $ErrorActionPreference = 'SilentlyContinue'
    $badging = & $aaptPath dump badging $apkPath 2>$null
} finally {
    $ErrorActionPreference = 'Stop'
}
if ($LASTEXITCODE -ne 0) { throw 'Could not read APK metadata with aapt2.' }
$package = $badging | Select-String "^package: name='([^']+)' versionCode='[^']*' versionName='([^']*)'" |
    Select-Object -First 1 -ExpandProperty Matches
if (!$package) { throw 'APK metadata does not include a package name and version.' }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($apkPath)
try {
    $checks = @($entries | Where-Object {
        $_.versionSource -eq 'apk-version-name' -and $_.packageName -ceq $package.Groups[1].Value -and
        $_.version -ceq $package.Groups[2].Value -and
        (!$LibraryFile -or $_.libraryFile -ceq $LibraryFile -or [IO.Path]::GetFileName($_.libraryFile) -ceq $LibraryFile)
    })
    if (!$checks) { throw "No library catalog record for $($package.Groups[1].Value) $($package.Groups[2].Value) $LibraryFile" }
    foreach ($entry in $checks) {
        $file = $zip.GetEntry($entry.libraryFile)
        if (!$file) { throw "Library is absent from APK: $($entry.libraryFile)" }
        $stream = $file.Open()
        try { Confirm-Hash $entry (Get-LibrarySha256 $stream) }
        finally { $stream.Dispose() }
    }
} finally {
    $zip.Dispose()
}
