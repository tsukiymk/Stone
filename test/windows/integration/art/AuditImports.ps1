param(
    [Parameter(Mandatory)][string]$PreparedDirectory,
    [string]$Probe = (Join-Path $PSScriptRoot '../../../../bin/x64/Debug/test/subsystem/Debug/stone-art-probe.exe'),
    [string]$Output = (Join-Path $PSScriptRoot '../../../../bin/x64/Debug/test/art-import-audit'),
    [string[]]$Libraries = @('libart.so', 'libandroid_runtime.so', 'libhwui.so')
)
$ErrorActionPreference = 'Stop'
$prepared = (Resolve-Path -LiteralPath $PreparedDirectory).Path
$probePath = (Resolve-Path -LiteralPath $Probe).Path
$outputPath = [IO.Path]::GetFullPath($Output)
[void][IO.Directory]::CreateDirectory($outputPath)
$allBindings = @()
$roots = @()
foreach ($library in $Libraries) {
    if ($library -notmatch '^[A-Za-z0-9_+@.-]+\.so$') { throw "Invalid library: $library" }
    $rows = & $probePath --audit-imports $prepared $library 2> (Join-Path $outputPath "$library.stderr.txt")
    if ($LASTEXITCODE -ne 0) { throw "Import audit failed for $library; see $outputPath" }
    $rows | Set-Content -LiteralPath (Join-Path $outputPath "$library.tsv") -Encoding UTF8
    $parsed = @($rows | ConvertFrom-Csv -Delimiter "`t")
    $allBindings += @($parsed | Where-Object { $_.Kind -in @('host', 'image') })
    $roots += [pscustomobject]@{
        Library = $library
        SHA256 = (Get-FileHash -LiteralPath (Join-Path $prepared "lib64/$library") -Algorithm SHA256).Hash.ToLowerInvariant()
        Dependencies = [int]($parsed | Where-Object Kind -eq 'library-count').Symbol
        Missing = @($parsed | Where-Object Kind -eq 'missing').Count
    }
}
$overlapRows = & $probePath --stub-overlaps $prepared 2> (Join-Path $outputPath 'overlaps.stderr.txt')
if ($LASTEXITCODE -ne 0) { throw "Stub export audit failed; see $outputPath" }
$overlapRows | Set-Content -LiteralPath (Join-Path $outputPath 'overlaps.tsv') -Encoding UTF8
$overlaps = @($overlapRows | ConvertFrom-Csv -Delimiter "`t")
$bindings = @($allBindings | Sort-Object Consumer,Provider,Symbol,Kind -Unique)
$bindings | Export-Csv -LiteralPath (Join-Path $outputPath 'bindings.csv') -NoTypeInformation -Encoding UTF8
$providers = @($bindings | Group-Object Kind,Provider | ForEach-Object {
    [pscustomobject]@{
        Kind = $_.Group[0].Kind
        Provider = $_.Group[0].Provider
        Imports = $_.Count
        Symbols = @($_.Group.Symbol | Sort-Object -Unique).Count
    }
} | Sort-Object Kind,Provider)
$summary = [pscustomobject]@{
    Scope = 'Windows x64 ART dependency relocations; no constructors, IFUNC resolvers or JNI executed'
    PreparedDirectory = $prepared
    Roots = $roots
    Providers = $providers
    Overlaps = @($overlaps | Where-Object Kind -eq 'overlap-count' | ForEach-Object {
        $library = $_.Consumer
        $path = Join-Path $prepared "lib64/$library"
        [pscustomobject]@{
            Library = $library
            SHA256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
            Preference = $_.Provider
            SameNamedExports = [int]$_.Symbol
        }
    })
    UnavailableLibraries = @($overlaps | Where-Object Kind -eq 'image-unavailable' | ForEach-Object Consumer)
    Limits = 'Overlapping exports do not prove ABI or behavior compatibility. Conditional bridges, libc-only loaders, dlopen/dlsym and translated ARM64 paths require separate checks. liblog image dependencies retain Windows log interposition.'
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'summary.json') -Encoding UTF8
$providers | Format-Table -AutoSize
Write-Output "Audit files: $outputPath"
