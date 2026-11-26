param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$FixtureApk,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$CacheDirectory,
    [string]$AngleDirectory,
    [switch]$LibraryOnly,
    [switch]$WakeRecovery,
    [switch]$KeepService,
    [switch]$ForegroundRace,
    [string]$BusyApk,
    [string]$OutputDirectory = 'bin/x64/Debug/test/background-session',
    [int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$fixture = (Resolve-Path -LiteralPath $FixtureApk).Path
$image = (Resolve-Path -LiteralPath $SystemImage).Path
$cache = (Resolve-Path -LiteralPath $CacheDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$root = Join-Path $output 'profile'
[IO.Directory]::CreateDirectory($output) | Out-Null; [IO.Directory]::CreateDirectory($root) | Out-Null
. (Join-Path $PSScriptRoot 'FixedRuntime.ps1')
$sharedRuntime=New-StoneTestRuntime $source $output
$runtime=$sharedRuntime.Directory
try {
if (!(Test-Path -LiteralPath (Join-Path $root 'cache'))) { New-Item -ItemType Junction -Path (Join-Path $root 'cache') -Target $cache | Out-Null }
$service = Join-Path $runtime 'StoneService.exe'; $package = 'com.trebledot.stone.job.fixture'
$files = Join-Path $root "data/data/$package/files"; [IO.Directory]::CreateDirectory($files) | Out-Null
$events = Join-Path $files 'session-events.txt'
$savedAngle = $env:STONE_ANGLE_DIR; $savedJava = $env:JAVA_HOME; $script:request = 0; $clients = @(); $brokers = @(); $completed = $false
$registry = @()
foreach ($entry in @(@('Software\Classes\stone',''), @('Software\Classes\stone','URL Protocol'), @('Software\Classes\stone\shell\open\command',''))) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($entry[0])
    try {
        $exists = $key -and $entry[1] -in $key.GetValueNames()
        $registry += @{ Path=$entry[0]; Name=$entry[1]; Exists=$exists; Value=$(if ($exists) { $key.GetValue($entry[1]) }); Kind=$(if ($exists) { $key.GetValueKind($entry[1]) }) }
    } finally { if ($key) { $key.Dispose() } }
}
if (-not ('StoneBackgroundWindow' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StoneBackgroundWindow {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr wparam, IntPtr lparam);
}
'@ }
} catch { Close-StoneTestRuntime $sharedRuntime; throw }
function Control([string[]]$Arguments, [switch]$AllowMissing) {
    $script:request++; $stdout = Join-Path $output "request-$script:request.stdout.txt"
    $argumentsText = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $process = Start-Process -FilePath $service -ArgumentList $argumentsText -WorkingDirectory $runtime -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $output "request-$script:request.stderr.txt")
    $null = $process.Handle
    try {
        if (!$process.WaitForExit(15000)) { throw 'Background service request timed out' }
        if ($process.ExitCode -ne 0) { if ($AllowMissing) { return $null }; throw "Service request failed: $Arguments" }
        return [IO.File]::ReadAllText($stdout).Trim()
    } finally { $process.Dispose() }
}
function Await([scriptblock]$Condition, [string]$Message) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (!(& $Condition)) { if ([DateTime]::UtcNow -ge $deadline) { throw $Message }; Start-Sleep -Milliseconds 100 }
}
function Events { if (Test-Path -LiteralPath $events) { return @(Get-Content -LiteralPath $events) }; return @() }
function Ready([Diagnostics.Process]$Process) {
    if ($Process.HasExited) { throw "Background Client exited early: $($Process.ExitCode)" }
    $log = Find-StoneClientLog $runtime $Process.Id
    return $log -and (Select-String -LiteralPath $log.FullName -SimpleMatch 'Background Application ready; no launcher Activity' -Quiet -ErrorAction SilentlyContinue)
}
function Painted([Diagnostics.Process]$Process) {
    $log = Find-StoneClientLog $runtime $Process.Id
    return $log -and (Select-String -LiteralPath $log.FullName -SimpleMatch 'first Android native frame painted' -Quiet -ErrorAction SilentlyContinue)
}
function Background([int]$Duration, [long]$Latency = 100, [bool]$Persisted = $false, [bool]$Once = $false) {
    [IO.File]::WriteAllText($events, '')
    $marker = Join-Path $files 'session-scheduled.txt'
    if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
    [IO.File]::WriteAllText((Join-Path $files 'session-mode.properties'), "duration=$Duration`nlatency=$Latency`npersisted=$($Persisted.ToString().ToLowerInvariant())`nscheduleOnce=$($Once.ToString().ToLowerInvariant())`n")
    $launch = Control @('--apk',$fixture,'--data-dir',$root,'--system-image',$image,'--background') | ConvertFrom-Json
    $process = Get-Process -Id $launch.client; $null = $process.Handle
    $script:clients += $process
    Await { Ready $process } 'Background Application did not initialize'
    $process.Refresh()
    if ($process.MainWindowHandle -ne [IntPtr]::Zero -or 'activity-created' -in (Events)) { throw 'Background boot created a launcher Activity/window' }
    if (@($process.Modules | Where-Object ModuleName -eq 'jvm.dll').Count) { throw 'Background ART loaded a Windows JVM' }
    return $process
}
function Clear-BackgroundRuntime {
    param([switch]$KeepLease)
    $env:STONE_ANGLE_DIR = $savedAngle
    $env:JAVA_HOME = $savedJava
    try {
    foreach ($process in $clients) {
        try { if (!$process.HasExited) { Stop-Process -Id $process.Id -Force; $null = $process.WaitForExit(5000) } }
        finally { $process.Dispose() }
    }
    # Automatic wakes may fail before the test can retain their handles. Restrict
    # cleanup to the exact executable paths in the leased shared test runtime.
    for ($pass = 0; $pass -lt 4; $pass++) {
        $found = $false
        foreach ($process in @(Get-Process StoneClient,StoneService -ErrorAction SilentlyContinue | Sort-Object ProcessName)) {
            try {
                $path = $process.Path
                if (![string]::Equals($path,(Join-Path $runtime 'StoneClient.exe'),[StringComparison]::OrdinalIgnoreCase) -and
                    ![string]::Equals($path,$service,[StringComparison]::OrdinalIgnoreCase)) { continue }
                $null = $process.Handle
                if (!$process.HasExited) { $found = $true; Stop-Process -InputObject $process -Force; $null = $process.WaitForExit(5000) }
            } finally { $process.Dispose() }
        }
        if (!$found) { break }
    }
    foreach ($process in $brokers) {
        try { if (!$process.HasExited) { Stop-Process -Id $process.Id -Force; $null = $process.WaitForExit(5000) } }
        finally { $process.Dispose() }
    }
    # Include a broker started before a failed Background() could capture it.
    $remaining = Control @('--status') -AllowMissing
    if ($remaining) {
        $owned = Get-Process -Id (($remaining | ConvertFrom-Json).service); $null = $owned.Handle
        try {
            if (![string]::Equals($owned.Path,$service,[StringComparison]::OrdinalIgnoreCase)) { throw 'Cleanup found an unrelated Service' }
            if (!$owned.HasExited) { Stop-Process -Id $owned.Id -Force; $null = $owned.WaitForExit(5000) }
        } finally { $owned.Dispose() }
    }
    } finally {
    try { foreach ($entry in $registry) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($entry.Path)
        try { if ($entry.Exists) { $key.SetValue($entry.Name,$entry.Value,$entry.Kind) } else { $key.DeleteValue($entry.Name,$false) } }
        finally { $key.Dispose() }
    } } finally { if (!$KeepLease -and $sharedRuntime.Lease) { Close-StoneTestRuntime $sharedRuntime } }
    }
}
if ($LibraryOnly) { return }
try {
    $passed = Join-Path $output 'passed.txt'; if (Test-Path -LiteralPath $passed) { Remove-Item -LiteralPath $passed -Force }
    if ($AngleDirectory) { $env:STONE_ANGLE_DIR = (Resolve-Path -LiteralPath $AngleDirectory).Path }
    if ($WakeRecovery) {
        if (!$BusyApk) { throw 'Wake recovery needs a distinct BusyApk, such as Stone CIT' }
        if ($ForegroundRace -and !$KeepService) { throw 'Foreground race needs KeepService' }
        $catalog = Control @('--profile-store')
        $catalogBoundary = [IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Trebledot Productions/Stone/Service')) + [IO.Path]::DirectorySeparatorChar
        if (![IO.Path]::GetFullPath($catalog).StartsWith($catalogBoundary,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($catalog) -ne 'job-profiles.bin') { throw 'Unexpected test catalog location' }
        if (Test-Path -LiteralPath $catalog) { Remove-Item -LiteralPath $catalog -Force }
        $queue = Join-Path $root 'data/system/stone-jobs'
        if (Test-Path -LiteralPath $queue) { foreach ($entry in (Get-ChildItem -LiteralPath $queue -File -Filter '*.bin')) { Remove-Item -LiteralPath $entry.FullName -Force } }
        $inputApk = Join-Path $output 'incoming-job.apk'; Copy-Item -LiteralPath $fixture -Destination $inputApk -Force; $fixture = $inputApk
        $client = Background 3000 40000 (!$KeepService) $true
        $state = Control @('--status') | ConvertFrom-Json; $broker = Get-Process -Id $state.service; $null = $broker.Handle; $brokers += $broker
        if (!$client.WaitForExit(15000) -or $client.ExitCode -ne 0 -or 'job-start' -in (Events)) { throw 'Scheduled Client did not retire before its deferred job' }
        if (!$KeepService) { Control @('--stop-service') | Out-Null; if (!$broker.WaitForExit(5000)) { throw 'Initial test Service did not exit' } }
        Remove-Item -LiteralPath $inputApk -Force # The wake must use its immutable staged APK.
        $env:JAVA_HOME = Join-Path $output 'missing-jdk'
        if ($KeepService) { $replacement = $broker }
        else {
            $replacement = Start-Process -FilePath $service -ArgumentList '--serve' -WorkingDirectory $runtime -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput (Join-Path $output 'restored.stdout.txt') -RedirectStandardError (Join-Path $output 'restored.stderr.txt')
            $null = $replacement.Handle; $brokers += $replacement
        }
        $busyRoot = Join-Path $output 'busy'; [IO.Directory]::CreateDirectory($busyRoot) | Out-Null
        if (!(Test-Path -LiteralPath (Join-Path $busyRoot 'cache'))) { New-Item -ItemType Junction -Path (Join-Path $busyRoot 'cache') -Target $cache | Out-Null }
        $busyStart = Control @('--apk',(Resolve-Path -LiteralPath $BusyApk).Path,'--data-dir',$busyRoot,'--system-image',$image) | ConvertFrom-Json
        $busy = Get-Process -Id $busyStart.client; $null = $busy.Handle; $clients += $busy
        Await { $busy.Refresh(); $busy.MainWindowHandle -ne [IntPtr]::Zero -and (Painted $busy) } 'Busy Client did not display its Activity'
        $manual = $null
        if ($ForegroundRace) {
            Copy-Item -LiteralPath (Resolve-Path -LiteralPath $FixtureApk).Path -Destination $inputApk -Force
            $scheduledTime = [long]((Events | Where-Object { $_.StartsWith('job-scheduled-at-') } | Select-Object -First 1).Substring('job-scheduled-at-'.Length))
            Await { [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() -ge $scheduledTime + 38000 } 'Foreground race did not reach its launch point'
            if ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() -ge $scheduledTime + 40000) { throw 'Foreground race was not armed before job due time' }
            $manualStart = Control @('--apk',$inputApk,'--data-dir',$root,'--system-image',$image) | ConvertFrom-Json
            $manual = Get-Process -Id $manualStart.client; $null = $manual.Handle; $clients += $manual
        }
        Await {
            $state = Control @('--status') | ConvertFrom-Json
            if ($ForegroundRace -and @($state.clients | Where-Object pid -ne $busy.Id).Count -gt 1) { throw 'Due job duplicated a starting foreground Client' }
            return @($state.clients | Where-Object package -eq $package).Count -eq 1
        } 'Service did not automatically start a Client for its due job'
        $state = Control @('--status') | ConvertFrom-Json
        $wakeId = ($state.clients | Where-Object package -eq $package).pid
        if ($wakeId -eq $client.Id) { throw 'Wake test did not create a fresh ART process' }
        if ($manual -and $manual.Id -ne $wakeId) { throw 'Due job did not use the already-starting foreground Client' }
        $woken = Get-Process -Id $wakeId; $null = $woken.Handle; $clients += $woken
        Await { 'job-start' -in (Events) } 'Automatically woken ART did not bind the real JobService'
        $woken.Refresh()
        if (!$ForegroundRace -and ($woken.MainWindowHandle -ne [IntPtr]::Zero -or 'activity-created' -in (Events))) { throw 'Automatic job wake opened a launcher Activity' }
        if (@($woken.Modules | Where-Object ModuleName -eq 'jvm.dll').Count) { throw 'Automatic wake loaded a Windows JVM' }
        $scheduledAt = [long]((Events | Where-Object { $_.StartsWith('job-scheduled-at-') } | Select-Object -First 1).Substring('job-scheduled-at-'.Length))
        $startedAt = [long]((Events | Where-Object { $_.StartsWith('job-start-at-') } | Select-Object -First 1).Substring('job-start-at-'.Length))
        if ($startedAt -lt $scheduledAt + 39500) { throw 'Service executed the job before its minimum latency' }
        if ((Control @('--host-state')).Split('|')[4] -ne '1') { throw 'Automatically woken job did not hold Windows power' }
        Await { 'job-finish' -in (Events) } 'Automatically woken JobService did not complete'
        if ($ForegroundRace) {
            Await { $woken.Refresh(); $woken.MainWindowHandle -ne [IntPtr]::Zero -and (Painted $woken) } 'Foreground race Client did not render'
            [void][StoneBackgroundWindow]::PostMessage($woken.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)
        }
        if (!$woken.WaitForExit(15000) -or $woken.ExitCode -ne 0) { throw 'Automatically woken Client did not retire' }
        if (@((Events) | Where-Object { $_ -eq 'application-created' }).Count -ne 2 -or @((Events) | Where-Object { $_ -eq 'job-start' }).Count -ne 1 -or
            @((Events) | Where-Object { $_ -eq 'job-scheduled' }).Count -ne 1) { throw 'Wake duplicated Application scheduling or job execution' }
        if ($busy.HasExited) { throw 'Job wake interrupted the unrelated Client' }
        Control @('--stop-service') | Out-Null
        if (!$busy.WaitForExit(10000) -or !$replacement.WaitForExit(10000)) { throw 'Wake test Service did not stop normally' }
        $(if ($ForegroundRace) { 'PASS due job reused an already-starting foreground Client without duplicate ART startup or job execution.' }
          elseif ($KeepService) { 'PASS non-persisted due job woke a fresh windowless ART in the same Service, using staged APK and exact image cache while another Client kept the broker busy.' }
          else { 'PASS persisted due job woke a fresh windowless ART after Service replacement, using staged APK and exact image cache while another Client kept the broker busy.' }) |
            Set-Content -LiteralPath $passed -Encoding UTF8
        Get-Content -LiteralPath $passed; $completed = $true; return
    }
    $client = Background 30000
    $state = Control @('--status') | ConvertFrom-Json; $broker = Get-Process -Id $state.service; $null = $broker.Handle; $brokers += $broker
    Await { 'job-start' -in (Events) } 'Headless Application did not execute its actual JobService'
    if ((Control @('--host-state')).Split('|')[4] -ne '1') { throw 'Background job did not hold a Windows power request' }
    # Only the isolated Service created by this test is deliberately replaced.
    if (![string]::Equals($broker.Path, $service, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to replace an unrelated Service' }
    Stop-Process -Id $broker.Id -Force; if (!$broker.WaitForExit(5000)) { throw 'Owned Service did not terminate' }
    Await {
        $text = Control @('--status') -AllowMissing
        if (!$text) { return $false }; $current = $text | ConvertFrom-Json
        return $current.service -ne $broker.Id -and @($current.clients | Where-Object { $_.pid -eq $client.Id -and $_.package -eq $package }).Count -eq 1
    } 'Live headless Client did not register with the replacement Service'
    $state = Control @('--status') | ConvertFrom-Json; $replacement = Get-Process -Id $state.service; $null = $replacement.Handle; $brokers += $replacement
    $activation = Control @('--uri',"stone://$package",'--data-dir',$root,'--system-image',$image) | ConvertFrom-Json
    if ($activation.client -ne $client.Id) { throw 'URI duplicated a live headless ART process' }
    $repeat = Control @('--apk',$fixture,'--data-dir',$root,'--system-image',$image) | ConvertFrom-Json
    if ($repeat.client -ne $client.Id) { throw 'Service replacement lost the explicit APK source identity' }
    Await { $client.Refresh(); $client.MainWindowHandle -ne [IntPtr]::Zero -and 'activity-created' -in (Events) -and (Painted $client) } 'Headless Client did not display its Activity'
    [void][StoneBackgroundWindow]::PostMessage($client.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    Await { 'activity-destroyed' -in (Events) } 'Window close did not destroy its Activity'
    if ($client.HasExited -or 'job-finish' -in (Events)) { throw 'Test did not exercise window close during an active background job' }
    Await { 'job-finish' -in (Events) } 'Job was interrupted when its window closed'
    if (!$client.WaitForExit(15000) -or $client.ExitCode -ne 0) { throw 'Idle Client did not retire cleanly' }
    if (@((Events) | Where-Object { $_ -eq 'application-created' }).Count -ne 1 -or @((Events) | Where-Object { $_ -eq 'job-start' }).Count -ne 1) { throw 'Activation duplicated Application or job execution' }
    if ((Control @('--status') | ConvertFrom-Json).clients.Count -ne 0) { throw 'Retired Client retained a managed session' }
    $client = Background 0
    Control @('--stop-service') | Out-Null
    if (!$client.WaitForExit(10000) -or $client.ExitCode -ne 0 -or !$replacement.WaitForExit(10000)) { throw 'Service stop did not close the windowless Client and broker' }
    $client = Background 0
    $state = Control @('--status') | ConvertFrom-Json; $lastBroker = Get-Process -Id $state.service; $null = $lastBroker.Handle; $brokers += $lastBroker
    if (!$client.WaitForExit(15000) -or $client.ExitCode -ne 0) { throw 'Unused headless Application did not retire' }
    if ($lastBroker.HasExited) { throw 'Idle Client retirement stopped Service' }
    Control @('--stop-service') | Out-Null; if (!$lastBroker.WaitForExit(5000)) { throw 'Final owned Service did not exit' }
    'PASS headless real JobService, Windows power, Service replacement, same-ART URI activation, background completion after window close, idle retirement and windowless shutdown.' |
        Set-Content -LiteralPath $passed -Encoding UTF8
    Get-Content -LiteralPath $passed; $completed = $true
} finally {
    Clear-BackgroundRuntime
}
