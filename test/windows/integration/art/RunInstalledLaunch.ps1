param(
    [Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$DataDirectory,
    [switch]$WarmWithoutJdk
)
$ErrorActionPreference = 'Stop'
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$imagePath = (Resolve-Path -LiteralPath $SystemImage).Path
$dataPath = (Resolve-Path -LiteralPath $DataDirectory).Path
function Read-Result([string]$Path) {
    $result = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^(uid|source|data|count|pid|component|staleSource)=(.*)$') { $result[$matches[1]] = $matches[2] }
    }
    return $result
}
function Managed-Apk([string]$Guest) {
    if ($Guest -notmatch '^/data/app/stone/[0-9a-f-]{36}/base\.apk$') { throw 'Invalid managed APK fixture path' }
    return Join-Path (Join-Path $dataPath 'data') $Guest.Substring('/data/'.Length)
}
$expected = Read-Result (Join-Path $dataPath 'data/installed-launch-expected.properties')
$apkPath = Managed-Apk $expected.source
$resultPath = Join-Path $dataPath 'data/installed-launch-result.properties'
$previous = if (Test-Path -LiteralPath $resultPath) { Read-Result $resultPath } else { $null }
for ($round = 0; $round -lt 2; $round++) {
    & (Join-Path $PSScriptRoot 'RunLaunch.ps1') -Runtime $runtimePath -Apk $apkPath -SystemImage $imagePath `
        -DataDirectory $dataPath -WarmWithoutJdk:($WarmWithoutJdk -or $round -gt 0)
    $current = Read-Result $resultPath
    if ($current.uid -ne $expected.uid -or $current.source -ne $expected.source -or $current.data -ne $expected.data `
            -or !$current.pid -or $current.component -ne 'com.trebledot.stone.install.launcher/com.trebledot.stone.install.launcher.Start') {
        throw 'Installed application did not report the registered UID, source, data and alias'
    }
    if ($previous -and [int]$current.count -ne [int]$previous.count + 1) {
        throw 'Installed application restart did not preserve its counter in a new process'
    }
    $previous = $current
}
$logs = Join-Path $dataPath 'launch-test'
$stale = Managed-Apk $expected.staleSource
$arguments = '--apk "{0}" --data-dir "{1}" --system-image "{2}"' -f $stale, $dataPath, $imagePath
$staleLog = Join-Path $logs 'stale.stderr.txt'
$originalLogs = $env:STONE_LOG_DIRECTORY
$process = $null
try {
    $env:STONE_LOG_DIRECTORY = Join-Path $logs 'Logs'
    $process = Start-Process -FilePath $runtimePath -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardError $staleLog -RedirectStandardOutput (Join-Path $logs 'stale.stdout.txt')
    if (!$process.WaitForExit(120000)) { throw 'Stale managed APK launch did not terminate' }
    if ($process.ExitCode -ne 1 -or !(Select-String -LiteralPath $staleLog -SimpleMatch `
            'Managed APK is no longer the registered application version' -Quiet)) {
        throw 'Stale managed APK was not rejected by the package registry'
    }
    if ((Read-Result $resultPath).count -ne $previous.count) { throw 'Rejected stale APK executed application code' }
} finally {
    $env:STONE_LOG_DIRECTORY = $originalLogs
    if ($process) {
        if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }
        $process.Dispose()
    }
}
Write-Output "PASS: registered UID in Application/Activity, original data, alias, two process launches, persistent counter and stale-code rejection. UID=$($previous.uid) Count=$($previous.count)"
