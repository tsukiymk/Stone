$ErrorActionPreference = 'Stop'
$settings = (Resolve-Path (Join-Path $PSScriptRoot '../../../../src/windows/settings')).Path
$viewModels = @(Get-ChildItem (Join-Path $settings 'ViewModels') -File -Recurse)
if (!$viewModels.Count) { throw 'No page view models were found.' }
foreach ($file in $viewModels) {
    if ($file.Name -notmatch '^[A-Za-z][A-Za-z0-9]*PageViewModel\.cs$') {
        throw "ViewModels must contain only page view models: $($file.Name)"
    }
    $content = Get-Content $file.FullName -Raw
    $types = @([regex]::Matches($content, '\b(?:class|record|struct|enum|interface)\s+([A-Za-z][A-Za-z0-9]*)') | ForEach-Object { $_.Groups[1].Value })
    if ($types.Count -ne 1 -or $types[0] -cne $file.BaseName) {
        throw "The view model file must contain its matching page view model: $($file.Name)"
    }
}
foreach ($page in @('Shell', 'Apps', 'System', 'Notifications', 'Permissions', 'Welcome', 'PackageConfirmation')) {
    foreach ($suffix in @('.xaml', '.xaml.cs')) {
        if (!(Test-Path (Join-Path $settings "Views/${page}Page$suffix") -PathType Leaf)) {
            throw "Missing page: $page$suffix"
        }
    }
}
foreach ($file in Get-ChildItem $settings -Recurse -File | Where-Object { $_.Extension -in @('.cs', '.xaml') }) {
    $content = Get-Content $file.FullName -Raw
    if ($content -match 'RuntimeFeaturesPanel|RuntimeExecutor|RuntimeJavaComponents|RuntimeJavaLaunch|RuntimeAot|RuntimeJit') {
        throw "Removed experimental settings surface remains in $($file.FullName)"
    }
}
Write-Host "PASS: $($viewModels.Count) page-only view models; separate views; no experimental runtime controls."
