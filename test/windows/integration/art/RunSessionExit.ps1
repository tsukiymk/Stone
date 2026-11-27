param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$FixtureApk,
    [Parameter(Mandatory)][string]$SystemImage,
    [ValidateSet('plain','nested','cleanup-failure','shutdown')][string[]]$Cases = @('plain','nested','cleanup-failure','shutdown'),
    [string]$OutputDirectory = 'bin/x64/Debug/test/session-exit'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'FixedRuntime.ps1')
$source = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$fixture = (Resolve-Path -LiteralPath $FixtureApk).Path
$image = (Resolve-Path -LiteralPath $SystemImage).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($output) | Out-Null
$savedTemp = $env:TEMP; $savedTmp = $env:TMP; $savedJava = $env:JAVA_HOME
$runtime = New-StoneTestRuntime $source $output
try {
    $env:TEMP = Join-Path $output 'temp'; $env:TMP = $env:TEMP
    [IO.Directory]::CreateDirectory($env:TEMP) | Out-Null
    $env:JAVA_HOME = Join-Path $output 'missing-jdk'
    foreach ($case in $Cases) {
        # Reuse a real data root and its prepared image across cases. Image
        # preparation rejects cache junctions; each case has separate log copies.
        $data = Join-Path $output 'profile'
        [IO.Directory]::CreateDirectory($data) | Out-Null
        $files = Join-Path $data 'data/data/com.trebledot.stone.sessionexit.fixture/files'
        [IO.Directory]::CreateDirectory($files) | Out-Null
        [IO.File]::WriteAllText((Join-Path $files 'session-exit-mode'), $case)
        $drained = Join-Path $files 'queued-work-finished'
        if (Test-Path -LiteralPath $drained) { Remove-Item -LiteralPath $drained -Force }
        $expected = if ($case -eq 'cleanup-failure') { 1 } else { 0 }
        & (Join-Path $PSScriptRoot 'RunLaunch.ps1') -Runtime (Join-Path $runtime.Directory 'StoneClient.exe') `
            -Apk $fixture -SystemImage $image -DataDirectory $data -WarmWithoutJdk -SkipDuplicate `
            -ExpectedExitCode $expected -ShutdownService:($case -eq 'shutdown') `
            -ReadyMarkers @("session-exit ready: $case", '[win32] first Android native frame painted')
        $log = Join-Path $data 'launch-test/launch.stderr.txt'
        $timing = Get-Content -LiteralPath (Join-Path $data 'launch-test/launch-timing.json') -Raw | ConvertFrom-Json
        if ($timing.forcedTermination -or $timing.exitCode -ne $expected) { throw "Unexpected termination for $case" }
        if (Select-String -LiteralPath $log -SimpleMatch 'session-exit caught Throwable' -Quiet) {
            throw "APK crash handler intercepted host retirement for $case"
        }
        if (!(Test-Path -LiteralPath $drained) -or [IO.File]::ReadAllText($drained) -ne 'drained') {
            throw "QueuedWork was not drained before exit for $case"
        }
        if ($case -eq 'cleanup-failure' -and !(Select-String -LiteralPath $log -SimpleMatch 'session-exit deliberate cleanup failure' -Quiet)) {
            throw 'Cleanup failure was not reported'
        }
        $caseOutput = Join-Path $output $case
        [IO.Directory]::CreateDirectory($caseOutput) | Out-Null
        Copy-Item -LiteralPath (Join-Path $data 'launch-test') -Destination $caseOutput -Recurse -Force
        Write-Output "PASS session retirement: $case, exit=$expected, queued writes drained, no forced termination"
    }
} finally {
    try {
        $service = Join-Path $runtime.Directory 'StoneService.exe'
        $stop = Start-Process $service -ArgumentList '--stop-service' -PassThru -WindowStyle Hidden `
            -RedirectStandardOutput (Join-Path $output 'stop.stdout.txt') -RedirectStandardError (Join-Path $output 'stop.stderr.txt')
        $null = $stop.Handle
        try { if (!$stop.WaitForExit(15000)) { $stop.Kill(); $null = $stop.WaitForExit(5000) } }
        finally { $stop.Dispose() }
    } finally {
        $env:TEMP = $savedTemp; $env:TMP = $savedTmp; $env:JAVA_HOME = $savedJava
        Close-StoneTestRuntime $runtime
    }
}
