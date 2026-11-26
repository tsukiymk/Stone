param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$SourceApk,
    [Parameter(Mandatory)][string]$TargetApk,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$CacheDirectory,
    [string]$AngleDirectory,
    [string]$OutputDirectory='bin/x64/Debug/test/activity-routes',
    [switch]$RestartService,
    [switch]$ResolveIntents,
    [switch]$BackResult,
    [int]$TimeoutSeconds=180
)
$ErrorActionPreference='Stop'
if ($BackResult -and ($RestartService -or $ResolveIntents)) { throw 'Run the root Back result case separately' }
$target=(Resolve-Path -LiteralPath $TargetApk).Path
$output=[IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath (Join-Path $output 'profile/data')) { throw 'Choose a fresh isolated route output directory' }
. (Join-Path $PSScriptRoot 'RunBackground.ps1') -RuntimeDirectory $RuntimeDirectory -FixtureApk $SourceApk `
    -SystemImage $SystemImage -CacheDirectory $CacheDirectory -AngleDirectory $AngleDirectory -OutputDirectory $output -TimeoutSeconds $TimeoutSeconds -LibraryOnly
$sourceEvents=Join-Path $root 'data/data/com.trebledot.stone.route.source/files/events.txt'
$targetEvents=Join-Path $root 'data/data/com.trebledot.stone.route.target/files/events.txt'
try {
    [IO.Directory]::CreateDirectory((Join-Path $root 'data')) | Out-Null
    [IO.File]::WriteAllText((Join-Path $root 'data/stone-route-fixture.txt'),"Stone disposable Activity route fixture`n",[Text.Encoding]::ASCII)
    if ($RestartService) { [IO.File]::WriteAllText((Join-Path $root 'data/stone-route-restart.txt'),'1',[Text.Encoding]::ASCII) }
    if ($ResolveIntents) { [IO.File]::WriteAllText((Join-Path $root 'data/stone-route-resolution.txt'),'1',[Text.Encoding]::ASCII) }
    if ($BackResult) { [IO.File]::WriteAllText((Join-Path $root 'data/stone-route-back.txt'),'1',[Text.Encoding]::ASCII) }
    Copy-Item -LiteralPath $target -Destination (Join-Path $root 'data/route-target.apk')
    if ($AngleDirectory) { $env:STONE_ANGLE_DIR=(Resolve-Path -LiteralPath $AngleDirectory).Path }
    $launch=Control @('--apk',$fixture,'--data-dir',$root,'--system-image',$image) | ConvertFrom-Json
    $client=Get-Process -Id $launch.client; $null=$client.Handle; $clients+=$client
    if ($RestartService) {
        Await { (Test-Path -LiteralPath $targetEvents) -and (Select-String -LiteralPath $targetEvents -SimpleMatch 'restart-target-ready' -Quiet) } 'Target did not establish a real caller before Service replacement'
        $state=Control @('--status') | ConvertFrom-Json
        if (@($state.clients).Count -ne 2) { throw 'Restart fixture did not have two live ART Clients' }
        $broker=Get-Process -Id $state.service; $null=$broker.Handle
        if (![string]::Equals($broker.Path,$service,[StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to replace an unrelated Service' }
        Stop-Process -Id $broker.Id -Force
        if (!$broker.WaitForExit(5000)) { throw 'Owned Service did not terminate' }
        Await {
            $current=Control @('--status') -AllowMissing
            return $current -and ($current | ConvertFrom-Json).service -ne $state.service
        } 'Clients did not restore a replacement Service'
        Await { (Select-String -LiteralPath $sourceEvents -SimpleMatch 'result-service-restart-0' -Quiet) -and
            (Select-String -LiteralPath $targetEvents -SimpleMatch 'restart-caller-revoked' -Quiet) } 'Service replacement did not cancel the old result and caller identity'
        if (@(Get-Content -LiteralPath $sourceEvents | Where-Object { $_ -ceq 'result-service-restart-0' }).Count -ne 1) { throw 'Service replacement duplicated cancellation' }
        Await { @((Control @('--status') | ConvertFrom-Json).clients).Count -eq 0 } 'Restart fixture Clients did not retire'
        Write-Output 'PASS Service replacement with two live ART Clients: pending result canceled once, remote caller identity revoked and both Clients retired.'
        return
    }
    if ($BackResult) {
        Await {
            if ($client.HasExited -and $client.ExitCode -ne 0) { throw 'Root Back source failed; inspect its runtime log' }
            return (Test-Path -LiteralPath $sourceEvents) -and (Test-Path -LiteralPath $targetEvents) -and
                (Select-String -LiteralPath $sourceEvents -SimpleMatch 'result-back--1' -Quiet) -and
                (Select-String -LiteralPath $targetEvents -SimpleMatch 'back-destroyed' -Quiet)
        } 'Cold root Activity Back did not preserve the actual SDK result'
        $sourceLines=@(Get-Content -LiteralPath $sourceEvents); $targetLines=@(Get-Content -LiteralPath $targetEvents)
        foreach ($expected in @('start-back','result-back--1')) {
            if (@($sourceLines | Where-Object { $_ -ceq $expected }).Count -ne 1) { throw "Missing or duplicate source Back event: $expected" }
        }
        foreach ($expected in @('back-framework-finish','back-destroyed')) {
            if (@($targetLines | Where-Object { $_ -ceq $expected }).Count -ne 1) { throw "Missing or duplicate target Back event: $expected" }
        }
        $targetPid=@($targetLines | Where-Object { $_ -match '^target-pid-[0-9]+-back$' })
        if ($targetPid.Count -ne 1 -or [int]($targetPid[0].Split('-')[2]) -eq $client.Id) { throw 'Root Back did not use a separate cold target Client' }
        Await { @((Control @('--status') | ConvertFrom-Json).clients).Count -eq 0 } 'Root Back Clients did not retire'
        if (!$client.WaitForExit(10000) -or $client.ExitCode -ne 0) { throw 'Root Back source did not exit normally' }
        Write-Output 'PASS cold root cross-Client Back: authenticated alias caller, framework finishAfterTransition, saved typed RESULT_OK, exact destruction/result and idle retirement.'
        return
    }
    Await {
        if ($client.HasExited -and $client.ExitCode -ne 0) { throw "Source Activity failed: $($client.ExitCode); inspect runtime logs" }
        if ($client.HasExited -and !(Test-Path -LiteralPath $targetEvents)) { throw 'Source exited before the target started' }
        return (Test-Path -LiteralPath $targetEvents) -and (Select-String -LiteralPath $targetEvents -SimpleMatch 'caller-revoked' -Quiet)
    } 'Real cross-process Activity fixture did not finish; inspect runtime logs and events'
    $sourceLines=@(Get-Content -LiteralPath $sourceEvents); $targetLines=@(Get-Content -LiteralPath $targetEvents)
    if ($ResolveIntents -and @($sourceLines | Where-Object { $_ -ceq 'public-filter-resolution-passed' }).Count -ne 1) { throw 'Public manifest filter checks did not run' }
    foreach ($expected in @('source-recreated-0','result-echo--1','result-cancel-0','result-die-0','result-echo-again--1','private-target-rejected','parcelable-rejected','source-finished-with-pending-result')) {
        if (@($sourceLines | Where-Object { $_ -ceq $expected }).Count -ne 1) { throw "Missing or duplicate source event: $expected" }
    }
    foreach ($expected in @('target-recreated-echo','target-recreated-echo-again','ordinary-null-caller','target-exiting','caller-revoked')) {
        if (@($targetLines | Where-Object { $_ -ceq $expected }).Count -ne 1) { throw "Missing or duplicate target event: $expected" }
    }
    $first=@($targetLines | Where-Object { $_ -match '^target-pid-([0-9]+)-echo$' })[0]
    $again=@($targetLines | Where-Object { $_ -match '^target-pid-([0-9]+)-echo-again$' })[0]
    if ($first.Split('-')[2] -eq $again.Split('-')[2]) { throw 'Target exit did not exercise a fresh ART process' }
    Await { @((Control @('--status') | ConvertFrom-Json).clients).Count -eq 0 } 'Finished Activity Clients did not retire'
    Write-Output 'PASS two real ART APKs: trusted alias caller, spoof isolation, typed Intent/result, both Activity recreations, cancel, no-result start, target exit/cold restart, private/payload rejection and caller revocation.'
} finally {
    try { Control @('--shell-shortcut',$root,$image,'com.trebledot.stone.route.target','','0') -AllowMissing | Out-Null }
    finally { Clear-BackgroundRuntime }
}
