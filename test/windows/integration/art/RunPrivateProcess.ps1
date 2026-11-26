param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$FixtureApk,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$CacheDirectory,
    [string]$OutputDirectory='bin/x64/Debug/test/private-process',
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath (Join-Path $output 'profile/data')) { throw 'Choose a fresh private-process fixture output directory' }
. (Join-Path $PSScriptRoot 'RunBackground.ps1') -RuntimeDirectory $RuntimeDirectory -FixtureApk $FixtureApk `
    -SystemImage $SystemImage -CacheDirectory $CacheDirectory -OutputDirectory $output -TimeoutSeconds $TimeoutSeconds -LibraryOnly
try {
    [IO.Directory]::CreateDirectory((Join-Path $root 'data')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'data/stone-route-private.txt'),'1',[Text.Encoding]::ASCII)
    $events=Join-Path $root 'data/data/com.trebledot.stone.route.source/files/events.txt'
    $launch=Control @('--apk',$fixture,'--data-dir',$root,'--system-image',$image) | ConvertFrom-Json
    $client=Get-Process -Id $launch.client; $null=$client.Handle; $clients+=$client
    Await {
        if ($client.HasExited -and $client.ExitCode -ne 0) { throw 'Private-process source failed; inspect runtime logs' }
        return (Test-Path -LiteralPath $events) -and (Select-String -LiteralPath $events -SimpleMatch 'private-complete' -Quiet)
    } 'Private Activity/service, process exit or cold restart did not finish'
    $lines=@(Get-Content -LiteralPath $events)
    foreach ($expected in @('private-result-0--1','private-result-1-0','private-result-2--1','private-game-exit','private-complete')) {
        if (@($lines | Where-Object { $_ -ceq $expected }).Count -ne 1) { throw "Missing or duplicate private-process event: $expected" }
    }
    $gamePids=@($lines | Where-Object { $_ -match '^private-game-pid-[0-9]+$' } | ForEach-Object { [int]($_.Split('-')[-1]) })
    if ($gamePids.Count -ne 3 -or $gamePids[-1] -eq $gamePids[0] -or $gamePids -contains $client.Id) { throw 'Game did not use independent and cold-restarted ART processes' }
    Await { @((Control @('--status') | ConvertFrom-Json).clients).Count -eq 0 } 'Private-process Clients did not retire'
    if (!$client.WaitForExit(5000) -or $client.ExitCode -ne 0) { throw 'Source did not exit normally after private game termination' }
    Write-Output 'PASS same-APK private ART process: real process identity, private Activity/service, application Serializable request/result, caller identity, game exit isolation, cold restart and idle retirement.'
} finally { Clear-BackgroundRuntime }
