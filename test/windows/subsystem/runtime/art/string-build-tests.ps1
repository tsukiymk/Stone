param(
    [Parameter(Mandatory)][string]$Compiler,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)
function BuildCase([string]$name, [string]$defaults, [string]$translation, [string]$errorPattern = '', [string]$qualifier = 'values-ja') {
    $root = Join-Path $OutputDirectory $name
    New-Item -ItemType Directory -Force -Path (Join-Path $root 'values'),(Join-Path $root $qualifier) | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'values/strings.xml'),$defaults,$utf8)
    [IO.File]::WriteAllText((Join-Path $root "$qualifier/strings.xml"),$translation,$utf8)
    try {
        & $Compiler -ResourceRoot $root -Output (Join-Path $root 'ArtStrings.java')
    } catch {
        if ($errorPattern -and $_.Exception.Message -match $errorPattern) { return }
        throw
    }
    if ($errorPattern) { throw "String compiler accepted invalid case: $name" }
}
$valid = '<resources><string name="title">Title</string><string name="version">Version %1$s</string></resources>'
BuildCase 'partial-translation' $valid '<resources><string name="title">タイトル</string></resources>'
BuildCase 'reordered-format' '<resources><string name="title">%1$s → %2$s</string></resources>' '<resources><string name="title">%2$s ← %1$s</string></resources>'
BuildCase 'duplicate-name' '<resources><string name="title">One</string><string name="title">Two</string></resources>' '<resources/>' 'Invalid or duplicate'
BuildCase 'unknown-name' $valid '<resources><string name="other">Other</string></resources>' 'Unknown ART string'
BuildCase 'bad-format' $valid '<resources><string name="version">Version %1$d</string></resources>' 'Mismatched format'
BuildCase 'bad-escape' '<resources><string name="title">Bad\q</string></resources>' '<resources/>' 'Unsupported string escape'
BuildCase 'markup' '<resources><string name="title"><b>Title</b></string></resources>' '<resources/>' 'Invalid or duplicate'
BuildCase 'qualifier' $valid '<resources/>' 'Unsupported ART string resource directory' 'values-night'
BuildCase 'dtd' '<!DOCTYPE resources [<!ENTITY text "injected">]><resources><string name="title">&text;</string></resources>' '<resources/>' 'DTD'
Write-Output 'ART string compiler failure-path checks passed'
