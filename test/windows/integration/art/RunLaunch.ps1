param(
    [Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][string]$Apk,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$DataDirectory,
    [switch]$WarmWithoutJdk,
    [switch]$EnableAot,
    [switch]$EnableJit,
    [int]$TimeoutSeconds = 180,
    [ValidateRange(0, 3600)][int]$ObserveSeconds = 0,
    [string[]]$ReadyMarkers = @()
)
$ErrorActionPreference = 'Stop'
$runtimePath = (Resolve-Path -LiteralPath $Runtime).Path
$apkPath = (Resolve-Path -LiteralPath $Apk).Path
$imagePath = (Resolve-Path -LiteralPath $SystemImage).Path
$dataPath = [IO.Path]::GetFullPath($DataDirectory)
[IO.Directory]::CreateDirectory($dataPath) | Out-Null
$logs = Join-Path $dataPath 'launch-test'
[IO.Directory]::CreateDirectory($logs) | Out-Null
if (-not ('StoneArtLaunchWindow' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class StoneArtLaunchWindow {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);
    [DllImport("user32.dll")]
    public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
}
'@
}
$originalJava = $env:JAVA_HOME
$originalLogs = $env:STONE_LOG_DIRECTORY
if ($WarmWithoutJdk) { $env:JAVA_HOME = Join-Path $dataPath 'deliberately-missing-jdk' }
$arguments = '--apk "{0}" --data-dir "{1}" --system-image "{2}"' -f $apkPath, $dataPath, $imagePath
if ($EnableAot) { $arguments += ' --enable-aot' }
if ($EnableJit) { $arguments += ' --enable-jit' }
$errorLog = Join-Path $logs 'launch.stderr.txt'
$process = $null
$timings=[ordered]@{windowReadyMs=$null;readinessMs=$null;vmReadyMs=$null;apkEntryMs=$null;firstFrameMs=$null;warmWithoutJdk=[bool]$WarmWithoutJdk;outcome='starting';observation='polled log, upper-bound milliseconds';startPoint='before Start-Process'}
function SaveLaunchTiming {
    $timings|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $logs 'launch-timing.json') -Encoding UTF8
}
function ObserveLaunchTiming {
    if(!(Test-Path -LiteralPath $errorLog)){return}
    $markers=@{vmReadyMs='[art] JNI_CreateJavaVM ready';apkEntryMs='Stone: starting the original APK on image ART';firstFrameMs='[win32] first Android native frame painted'}
    $changed=$false
    foreach($key in $markers.Keys){
        if($null -eq $timings[$key] -and (Select-String -LiteralPath $errorLog -SimpleMatch $markers[$key] -Quiet)){
            $timings[$key]=$launchClock.ElapsedMilliseconds;$changed=$true
        }
    }
    if($changed){SaveLaunchTiming}
}
try {
    $env:STONE_LOG_DIRECTORY = Join-Path $logs 'Logs'
    $launchClock = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $runtimePath -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardError $errorLog -RedirectStandardOutput (Join-Path $logs 'launch.stdout.txt')
    $null = $process.Handle
    $timings.processId=$process.Id;$timings.processStartedUtc=$process.StartTime.ToUniversalTime().ToString('o');SaveLaunchTiming
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $window = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline) {
        ObserveLaunchTiming
        $process.Refresh()
        if ($process.HasExited) { throw "ART launch exited before opening its window. See $errorLog" }
        $window = $process.MainWindowHandle
        if ($window -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 100
    }
    if ($window -eq [IntPtr]::Zero) { throw "ART window startup timed out. See $errorLog" }
    $windowReadyMs=$launchClock.ElapsedMilliseconds
    $timings.windowReadyMs=$windowReadyMs;SaveLaunchTiming
    $name = [Text.StringBuilder]::new(256)
    [void][StoneArtLaunchWindow]::GetClassName($window, $name, 256)
    if ($name.ToString() -ne 'Stone.Windows.Runtime') { throw "Unexpected ART window class: $name" }
    foreach($marker in $ReadyMarkers){
        while(!(Select-String -LiteralPath $errorLog -SimpleMatch $marker -Quiet)){
            ObserveLaunchTiming
            $process.Refresh()
            if($process.HasExited){throw "ART exited before readiness marker '$marker'. See $errorLog"}
            if([DateTime]::UtcNow -ge $deadline){throw "ART readiness marker timed out: '$marker'. See $errorLog"}
            Start-Sleep -Milliseconds 100
        }
    }
    ObserveLaunchTiming
    $launchClock.Stop()
    $timings.readinessMs=$launchClock.ElapsedMilliseconds;$timings.outcome='ready';SaveLaunchTiming
    if (@($process.Modules | Where-Object ModuleName -eq 'jvm.dll').Count) { throw 'The ART application loaded a Windows JVM' }
    $duplicate = Start-Process -FilePath $runtimePath -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardError (Join-Path $logs 'duplicate.stderr.txt') -RedirectStandardOutput (Join-Path $logs 'duplicate.stdout.txt')
    $null = $duplicate.Handle
    try {
        if (!$duplicate.WaitForExit(15000)) { throw 'Duplicate ART launch did not exit' }
        if ($duplicate.ExitCode -ne 1 -or !(Select-String -LiteralPath (Join-Path $logs 'duplicate.stderr.txt') -SimpleMatch 'APK already running' -Quiet)) {
            throw 'Duplicate ART launch was not rejected'
        }
    } finally {
        if (!$duplicate.HasExited) { Stop-Process -Id $duplicate.Id -Force }
        $duplicate.Dispose()
    }
    $observeUntil = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
    while ([DateTime]::UtcNow -lt $observeUntil) {
        $process.Refresh()
        if ($process.HasExited) { throw "ART application exited during observation. See $errorLog" }
        Start-Sleep -Milliseconds 100
    }
    Start-Sleep -Milliseconds 300
    if (![StoneArtLaunchWindow]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) { throw 'Could not close the ART window' }
    if (!$process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw "ART window did not exit successfully. See $errorLog" }
    if ($EnableAot) {
        if (!(Select-String -LiteralPath $errorLog -Pattern '\[art-aot\] loaded /data/dalvik-cache/x86_64/' -Quiet) `
                -or !(Select-String -LiteralPath $errorLog -Pattern "kOatUpToDate with filter 'speed' executable 'true'" -Quiet)) {
            throw "ART did not accept executable AOT code. See $errorLog"
        }
    }
    if ($EnableJit -and !(Select-String -LiteralPath $errorLog -Pattern 'Stone: experimental (x64|ARM64) ART Java JIT enabled for this launch' -Quiet)) {
        throw "ART did not enable JIT. See $errorLog"
    }
    $timings.outcome='passed';$timings.exitCode=$process.ExitCode;SaveLaunchTiming
    Write-Output "PASS: Stone ART original APK window, no Windows JVM, duplicate rejection and shutdown. WarmWithoutJdk=$WarmWithoutJdk. ObserveSeconds=$ObserveSeconds. Logs: $logs"
} catch {
    $timings.outcome='failed';$timings.failure=$_.Exception.Message
    ObserveLaunchTiming;SaveLaunchTiming
    throw
} finally {
    $env:JAVA_HOME = $originalJava
    $env:STONE_LOG_DIRECTORY = $originalLogs
    if ($process) {
        if (!$process.HasExited) { Stop-Process -Id $process.Id -Force;[void]$process.WaitForExit(5000) }
        $timings.exitCode=$process.ExitCode;SaveLaunchTiming
        $process.Dispose()
    }
}
