Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-PackageTool([string]$Tool, [string[]]$Arguments) {
    & $Tool @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE" }
}

function Get-PackageConfig([string]$Path) {
    $config = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($config.installService -isnot [bool] -or !$config.installService) {
        throw 'Stone packages require service registration.'
    }
    if ($config.architecture -notin @('x64', 'arm64')) { throw 'Unsupported package architecture.' }
    if ($config.version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.0$') {
        throw 'Package version must be major.minor.build.0 (no leading zeros).'
    }
    $version = [version]$config.version
    if ($version.Major -lt 1 -or $version.Major -gt 255 -or $version.Minor -gt 255 -or $version.Build -gt 65535) {
        throw 'Shared MSI/MSIX version requires major 1..255, minor 0..255, build 0..65535, revision 0.'
    }
    if ($config.name -notmatch '^[A-Za-z0-9][A-Za-z0-9.-]{2,49}$') { throw 'Invalid MSIX identity name.' }
    if ([string]::IsNullOrWhiteSpace($config.publisher)) { throw 'MSIX publisher is required.' }
    return $config
}

function Resolve-PackageTool([string]$Configured, [string]$Name) {
    if ($Configured) {
        if (!(Test-Path -LiteralPath $Configured -PathType Leaf)) { throw "Missing $Name tool: $Configured" }
        return (Resolve-Path -LiteralPath $Configured).Path
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (!$command) { throw "Cannot find $Name. Set its STONE_*_EXECUTABLE CMake cache path or add it to PATH." }
    return $command.Source
}

function ConvertTo-PackageXml([string]$Value) {
    return [System.Security.SecurityElement]::Escape($Value)
}

function Expand-PackageTemplate([string]$Path, [hashtable]$Values) {
    $content = Get-Content -LiteralPath $Path -Raw
    foreach ($key in $Values.Keys) { $content = $content.Replace("@$key@", [string]$Values[$key]) }
    if ($content -match '@[A-Z_]+@') { throw "Unexpanded template token: $($Matches[0])" }
    return $content
}

function Get-PackageId([string]$Value) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Value.ToLowerInvariant())
    return 'p' + [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).Substring(0, 32)
}

function Assert-PackageTree([string]$Path) {
    # Do not follow junctions/symlinks into developer or application data.
    $entry = Get-Item -LiteralPath $Path -Force
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Package input is a reparse point: $Path" }
    if ($entry.PSIsContainer) {
        foreach ($child in Get-ChildItem -LiteralPath $Path -Force) { Assert-PackageTree $child.FullName }
    }
}

function Assert-PackageBinary([string]$Path, [string]$Architecture) {
    $machine = switch ($Architecture.ToLowerInvariant()) {
        'x64' { 0x8664 }
        'arm64' { 0xaa64 }
        default { throw "Unsupported package architecture: $Architecture" }
    }
    $entry = Get-Item -LiteralPath $Path -Force
    while ($entry) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Package input is a reparse point: $($entry.FullName)"
        }
        $entry = if ($entry -is [IO.DirectoryInfo]) { $entry.Parent } else { $entry.Directory }
    }
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Path))
    try {
        if ($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid PE file: $Path" }
        $reader.BaseStream.Position = 0x3c
        $offset = $reader.ReadUInt32()
        if ($offset -gt $reader.BaseStream.Length - 24) { throw "Invalid PE header: $Path" }
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne $machine) {
            throw "PE architecture mismatch: $Path (expected $Architecture)"
        }
        $reader.BaseStream.Position = $offset + 22
        if ([IO.Path]::GetExtension($Path) -ieq '.dll' -and !($reader.ReadUInt16() -band 0x2000)) {
            throw "Package input is not a DLL: $Path"
        }
    } finally { $reader.Dispose() }
}

function Sign-Package($Config, [string]$Path) {
    if (!$Config.certificateThumbprint) {
        Write-Host "Unsigned package (sign before deployment): $Path"
        return
    }
    $tool = Resolve-PackageTool $Config.signtool 'signtool.exe'
    $arguments = @('sign', '/fd', 'SHA256', '/s', 'My', '/sha1', $Config.certificateThumbprint)
    if ($Config.timestampUrl) { $arguments += @('/tr', $Config.timestampUrl, '/td', 'SHA256') }
    Invoke-PackageTool $tool ($arguments + @($Path))
    Invoke-PackageTool $tool @('verify', '/pa', '/v', $Path)
}

function New-PackageWorkDirectory($Config) {
    $directory = Join-Path $Config.outputDirectory ('work/' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    return $directory
}

function Publish-Package([string]$Source, [string]$Destination) {
    # An unsuccessful tool/signing step never replaces an earlier good artifact.
    Move-Item -LiteralPath $Source -Destination $Destination -Force
    (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash + '  ' + [IO.Path]::GetFileName($Destination) |
        Set-Content -LiteralPath ($Destination + '.sha256') -Encoding utf8
    Write-Host "Created $Destination"
}
