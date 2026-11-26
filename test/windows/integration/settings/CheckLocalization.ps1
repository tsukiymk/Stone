$ErrorActionPreference = 'Stop'
$settings = Join-Path $PSScriptRoot '../../../../src/windows/settings'
$catalogs = @{}
foreach ($language in @('en-US', 'zh-CN', 'ja-JP')) {
    [xml]$document = Get-Content (Join-Path $settings "Resources/$language/Resources.resw") -Raw
    $catalog = @{}
    foreach ($entry in $document.root.data) {
        if ($catalog.ContainsKey($entry.name) -or [string]::IsNullOrWhiteSpace($entry.value)) {
            throw "Duplicate or empty resource: $language/$($entry.name)"
        }
        $catalog[$entry.name] = [string]$entry.value
    }
    $catalogs[$language] = $catalog
}
$reference = $catalogs['en-US']
$uidTypes = @{}
foreach ($language in @('zh-CN', 'ja-JP')) {
    $catalog = $catalogs[$language]
    if (Compare-Object @($reference.Keys | Sort-Object) @($catalog.Keys | Sort-Object)) {
        throw "Resource keys differ for $language"
    }
    foreach ($key in $reference.Keys) {
        $expected = @([regex]::Matches($reference[$key], '\{\d+(?:[^{}]*)\}').Value | Sort-Object)
        $actual = @([regex]::Matches($catalog[$key], '\{\d+(?:[^{}]*)\}').Value | Sort-Object)
        if (($expected -join '|') -cne ($actual -join '|')) { throw "Format arguments differ: $language/$key" }
    }
}
foreach ($file in Get-ChildItem $settings -Recurse -Filter '*.xaml') {
    [xml]$document = Get-Content $file.FullName -Raw
    foreach ($element in $document.SelectNodes('//*')) {
        $uid = $element.GetAttribute('Uid', 'http://schemas.microsoft.com/winfx/2006/xaml')
        if ($uid) {
            if ($uidTypes.ContainsKey($uid) -and $uidTypes[$uid] -cne $element.LocalName) {
                throw "x:Uid $uid is shared by different control types"
            }
            $uidTypes[$uid] = $element.LocalName
        }
        if ($uid -and !@($reference.Keys | Where-Object { $_.StartsWith($uid + '.', [StringComparison]::Ordinal) }).Count) {
            throw "No resources for x:Uid $uid in $($file.Name)"
        }
        foreach ($attribute in $element.Attributes) {
            if ($attribute.Value -match '[\p{IsCJKUnifiedIdeographs}]') { throw "Hardcoded XAML text in $($file.Name): $attribute" }
        }
    }
}
foreach ($file in Get-ChildItem $settings -Recurse -Filter '*.cs') {
    $text = Get-Content $file.FullName -Raw
    foreach ($match in [regex]::Matches($text, 'UiText\.(?:Get|Format)\("([A-Za-z0-9]+)"')) {
        if (!$reference.ContainsKey($match.Groups[1].Value)) { throw "Missing code resource in $($file.Name): $match" }
    }
}
Write-Host "PASS: $($reference.Count) resources per language; matching keys, placeholders, XAML identifiers and code references."
