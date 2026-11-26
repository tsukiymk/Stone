param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$path = Join-Path $OutputDirectory 'runtime-features.properties'
if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
& $Executable $OutputDirectory false false
if ($LASTEXITCODE -ne 0) { throw 'Default ART preferences failed.' }
foreach ($executor in @('', 'executor=art', 'executor=java-runtime', "executor=art`nexecutor=art")) {
    $text = "format=1`naot=true`njit=false`n"
    if ($executor) { $text += "$executor`n" }
    [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
    $result = & $Executable $OutputDirectory true false 2>&1
    $code = $LASTEXITCODE
    $valid = $executor -in @('', 'executor=art')
    if (($code -eq 0) -ne $valid) { throw "Unexpected configuration result: $executor ($result)" }
    if ($executor -eq 'executor=java-runtime' -and ($result -join '') -notmatch 'has been removed') {
        throw 'Removed JVM configuration did not report the migration error.'
    }
}
Write-Host 'PASS ART preferences and explicit rejection of the removed Windows JVM executor.'
