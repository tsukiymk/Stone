[CmdletBinding()]
param([Parameter(Mandatory)][string]$ConfigFile)
. "$PSScriptRoot/Common.ps1"
$config = Get-PackageConfig $ConfigFile
$tool = Resolve-PackageTool $config.makeappx 'makeappx.exe'
if (!$config.bundleInputs.Count) { throw 'No bundle architectures were selected.' }
$architectures = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$inputs = @()
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($path in $config.bundleInputs) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing bundle input: $path. Build msixinstallerpackage for every selected architecture first."
    }
    $archive = [IO.Compression.ZipFile]::OpenRead($path)
    try {
        $entry = $archive.GetEntry('AppxManifest.xml')
        if (!$entry) { throw "Missing AppxManifest.xml: $path" }
        $reader = [IO.StreamReader]::new($entry.Open())
        try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $identity = $manifest.Package.Identity
        if ($identity.Name -cne $config.name -or $identity.Publisher -cne $config.publisher -or
            $identity.Version -cne $config.version) { throw "Bundle identity/version mismatch: $path" }
        $arch = [string]$identity.ProcessorArchitecture
        if ($arch -notin @('x64', 'arm64') -or !$architectures.Add($arch)) {
            throw "Unsupported or duplicate bundle architecture: $arch"
        }
        $inputs += [pscustomobject]@{ Path = (Resolve-Path -LiteralPath $path).Path; Architecture = $arch }
    } finally { $archive.Dispose() }
}
$work = New-PackageWorkDirectory $config
$mapping = @('[Files]')
foreach ($inputPackage in $inputs | Sort-Object Architecture) {
    $mapping += '"{0}" "Stone_{1}_{2}.msix"' -f $inputPackage.Path, $config.version, $inputPackage.Architecture
}
$mappingFile = Join-Path $work 'bundle.map'
Set-Content -LiteralPath $mappingFile -Value $mapping -Encoding utf8
$archName = ($inputs.Architecture | Sort-Object) -join '_'
$output = Join-Path $work "Stone_$($config.version)_$archName.msixbundle"
Invoke-PackageTool $tool @('bundle', '/f', $mappingFile, '/bv', $config.version, '/p', $output, '/o')
Sign-Package $config $output
Publish-Package $output (Join-Path $config.outputDirectory ([IO.Path]::GetFileName($output)))
