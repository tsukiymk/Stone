param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$OriginalApk,
    [Parameter(Mandatory)][string]$PackageName,
    [Parameter(Mandatory)][string]$ExpectedApkSha256,
    [Parameter(Mandatory)][string]$DataDirectory,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$CacheDirectory,
    [string]$AngleDirectory,
    [string]$OutputDirectory = 'bin/x64/Debug/test/upgrade-restart',
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
if ($PackageName -notmatch '^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+$' -or $ExpectedApkSha256 -notmatch '^[a-f0-9]{64}$') { throw 'Explicit package and SHA-256 required' }
$original = (Resolve-Path -LiteralPath $OriginalApk).Path
$profile = (Resolve-Path -LiteralPath $DataDirectory).Path
$owned = @(Get-ChildItem -LiteralPath (Join-Path $profile 'data/app/stone') -Recurse -Filter base.apk |
    Where-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -eq $ExpectedApkSha256 })
if ($owned.Count -ne 1) { throw 'Expected exactly one installed APK matching the verified new bytes' }
$expectedSource = '/data/app/stone/' + $owned[0].Directory.Name + '/base.apk'
$summary = Get-Content -LiteralPath (Join-Path $profile 'data/system/stone-packages/packages.json') -Raw | ConvertFrom-Json
$installed = @($summary.apps | Where-Object packageName -eq $PackageName)
if ($installed.Count -ne 1) { throw 'Expected registered upgraded package' }
$expectedUid = $installed[0].uid
$sentinel = Join-Path $profile "data/data/$PackageName/files/stone-upgrade-probe.data"
if (!(Test-Path -LiteralPath $sentinel -PathType Leaf)) { throw 'Prepare a private upgrade data sentinel before this restart test' }
$privateHash = (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
. (Join-Path $PSScriptRoot 'RunBackground.ps1') -RuntimeDirectory $RuntimeDirectory -FixtureApk $original `
    -SystemImage $SystemImage -CacheDirectory $CacheDirectory -AngleDirectory $AngleDirectory -OutputDirectory $OutputDirectory -TimeoutSeconds $TimeoutSeconds -LibraryOnly
# Reuse the existing upgrade profile; the shared helper owns only this isolated
# runtime's processes and saves/restores the current user's protocol values.
$root = $profile
$diagnostics = Join-Path $root "data/stone-diagnostics/$PackageName"
$savedDiagnostics = $env:STONE_ART_DIAGNOSTICS
try {
    $env:STONE_ART_DIAGNOSTICS = '1'
    if ($AngleDirectory) { $env:STONE_ANGLE_DIR = (Resolve-Path -LiteralPath $AngleDirectory).Path }
    foreach ($mode in @('apk','uri')) {
        $statePath = Join-Path $diagnostics 'state.txt'
        foreach ($file in @('state.txt','frame.png')) {
            $path = Join-Path $diagnostics $file
            if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path -Force }
        }
        $arguments = if ($mode -eq 'apk') { @('--apk',$original) } else { @('--uri',"stone://$PackageName") }
        $launch = Control ($arguments + @('--data-dir',$root,'--system-image',$image)) | ConvertFrom-Json
        $client = Get-Process -Id $launch.client; $null = $client.Handle; $clients += $client
        Await { if ($client.HasExited) { throw "Upgraded $mode launch exited before painting: $($client.ExitCode)" }; Painted $client } "Upgraded $mode launch did not paint"
        Await { (Test-Path -LiteralPath $statePath -PathType Leaf) -and (Test-Path -LiteralPath (Join-Path $diagnostics 'frame.png') -PathType Leaf) } 'Missing actual software-rendered launch identity'
        Copy-Item -LiteralPath $statePath -Destination (Join-Path $output "$mode-state.txt") -Force
        Copy-Item -LiteralPath (Join-Path $diagnostics 'frame.png') -Destination (Join-Path $output "$mode-frame.png") -Force
        $state = [IO.File]::ReadAllLines($statePath)
        if ("source=$expectedSource" -notin $state -or "uid=$expectedUid" -notin $state) { throw "Upgraded $mode launch used stale APK resources or a different UID" }
        foreach ($field in @('applicationClassLoader=','activityClassLoader=')) {
            $loader = @($state | Where-Object { $_.StartsWith($field) })
            if ($loader.Count -ne 1 -or !$loader[0].Contains($expectedSource)) { throw "Upgraded $mode launch used stale application or Activity classes" }
        }
        if (@($client.Modules | Where-Object ModuleName -eq 'jvm.dll').Count) { throw 'Upgraded application loaded a Windows JVM' }
        $client.Refresh()
        if (![StoneBackgroundWindow]::PostMessage($client.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)) { throw 'Could not close upgraded application window' }
        if (!$client.WaitForExit($TimeoutSeconds * 1000) -or $client.ExitCode -ne 0) { throw 'Upgraded application did not exit normally' }
        Await { (Control @('--status') | ConvertFrom-Json).clients.Count -eq 0 } 'Upgraded application retained its session'
        if ((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $privateHash) { throw 'Application private bytes changed across upgraded launch' }
        Write-Output "PASS upgraded $mode launch uses actual new APK resources, Application/Activity classes and retained UID $expectedUid; private SHA-256=$privateHash; normal close and no Windows JVM."
    }
    Control @('--stop-service') | Out-Null
} finally {
    $env:STONE_ART_DIAGNOSTICS = $savedDiagnostics
    Clear-BackgroundRuntime
}
