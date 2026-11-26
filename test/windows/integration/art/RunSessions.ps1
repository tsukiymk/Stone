param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$Apk,
    [Parameter(Mandatory)][string]$SecondApk,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$CacheDirectory,
    [string]$OutputDirectory = 'bin/x64/Debug/test/service-sessions',
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'FixedRuntime.ps1')
$source = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$image = (Resolve-Path -LiteralPath $SystemImage).Path
$cache = (Resolve-Path -LiteralPath $CacheDirectory).Path
$apks = @((Resolve-Path -LiteralPath $Apk).Path, (Resolve-Path -LiteralPath $SecondApk).Path)
$runtime = Join-Path $output 'runtime'
[IO.Directory]::CreateDirectory($runtime) | Out-Null
foreach ($name in @('StoneService.exe', 'StoneClient.exe', 'stone-art-runtime.jar', 'StoneImageTool.exe')) {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $runtime $name) -Force
}
$settingsSource = Join-Path $source 'settings'
if ((Test-Path -LiteralPath $settingsSource) -and !(Test-Path -LiteralPath (Join-Path $runtime 'settings'))) {
    New-Item -ItemType Junction -Path (Join-Path $runtime 'settings') -Target $settingsSource | Out-Null
}
$service = Join-Path $runtime 'StoneService.exe'
if (-not ('StoneSessionWindow' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class StoneSessionWindow {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
}
'@ }
$script:request = 0
function Invoke-Service([string[]]$Arguments, [int]$Expected = 0) {
    $script:request++
    $stdout = Join-Path $output ("request-$script:request.stdout.txt")
    $stderr = Join-Path $output ("request-$script:request.stderr.txt")
    $quoted = @($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $process = Start-Process -FilePath $service -ArgumentList $quoted -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    try {
        if (!$process.WaitForExit(20000)) { throw 'Service request timed out' }
        if ($process.ExitCode -ne $Expected) { throw "Service request returned $($process.ExitCode); see $stderr" }
        $json = Get-Content -LiteralPath $stdout | Where-Object { $_.StartsWith('{') } | Select-Object -Last 1
        if ($json) { return $json | ConvertFrom-Json }
    } finally { $process.Dispose() }
}
function Wait-Window([int]$Id) {
    $process = Get-Process -Id $Id
    $null = $process.Handle # Retain the real process handle before it exits.
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.HasExited) { throw "Client $Id exited before displaying its Activity" }
        $log = Find-StoneClientLog $runtime $Id
        if ($process.MainWindowHandle -ne [IntPtr]::Zero -and $log -and
            (Select-String -LiteralPath $log.FullName -SimpleMatch 'first Android native frame painted' -Quiet -ErrorAction SilentlyContinue)) {
            if ($process.ProcessName -ne 'StoneClient') { throw 'Window is not owned by StoneClient' }
            if (@($process.Modules | Where-Object ModuleName -eq 'jvm.dll').Count) { throw 'Client loaded a Windows JVM' }
            return $process
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Client $Id did not render an Activity"
}
$broker = $null
$clients = @()
$previousLogs = $env:STONE_LOG_DIRECTORY; $previousLocal = $env:LOCALAPPDATA
try {
    $env:STONE_LOG_DIRECTORY = Join-Path $runtime 'Logs'
    $env:LOCALAPPDATA = Join-Path $output 'local'
    $roots = @(Join-Path $output 'first'; Join-Path $output 'second')
    foreach ($root in $roots) {
        [IO.Directory]::CreateDirectory($root) | Out-Null
        if (!(Test-Path -LiteralPath (Join-Path $root 'cache'))) { New-Item -ItemType Junction -Path (Join-Path $root 'cache') -Target $cache | Out-Null }
    }
    $first = Invoke-Service @('--apk', $apks[0], '--data-dir', $roots[0], '--system-image', $image)
    $initial = Invoke-Service @('--status')
    $broker = Get-Process -Id $initial.service
    $null = $broker.Handle
    $clients += Wait-Window $first.client
    $second = Invoke-Service @('--apk', $apks[1], '--data-dir', $roots[1], '--system-image', $image)
    $clients += Wait-Window $second.client
    $state = Invoke-Service @('--status')
    if ($state.clients.Count -ne 2 -or $first.client -eq $second.client -or $broker.Id -in @($clients.Id)) { throw 'Expected one broker and two ART Clients' }
    if (@($broker.Modules | Where-Object ModuleName -eq 'jvm.dll').Count) { throw 'Broker loaded a Windows JVM' }
    $repeat = Invoke-Service @('--apk', $apks[0], '--data-dir', $roots[0], '--system-image', $image)
    if ($repeat.client -ne $first.client) { throw 'Repeated launch created another Client' }
    $package = ($state.clients | Where-Object pid -eq $first.client).package
    if (!$package) { throw 'Client did not register its Android package' }
    $uri = Invoke-Service @('--uri', "stone://$package", '--data-dir', $roots[0], '--system-image', $image)
    if ($uri.client -ne $first.client) { throw 'Package URI failed to activate its existing Client' }
    foreach ($invalid in @('stone://../other', 'stone://com.example.app?command=other', 'stone://com.example.app/%20')) {
        Invoke-Service @('--uri', $invalid) 1 | Out-Null
    }
    $after = Invoke-Service @('--status')
    if ($after.service -ne $state.service -or $after.clients.Count -ne 2) { throw 'Service identity or Client count changed unexpectedly' }
    [void][StoneSessionWindow]::PostMessage($clients[0].MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (!$clients[0].WaitForExit(10000) -or $clients[0].ExitCode -ne 0) { throw "First Client did not exit normally: $($clients[0].ExitCode)" }
    $clients[1].Refresh()
    if ($clients[1].HasExited) { throw 'Closing one application stopped the other' }
    if ((Invoke-Service @('--status')).clients.Count -ne 1) { throw 'Service retained an exited Client' }
    Invoke-Service @('--stop-service') | Out-Null
    if (!$clients[1].WaitForExit(10000) -or !$broker.WaitForExit(10000)) { throw 'Service shutdown did not close its Clients' }
    'PASS: one Service, two real ART Clients, independent windows/lifetimes, duplicate activation, package URI and shutdown.' |
        Set-Content -LiteralPath (Join-Path $output 'passed.txt') -Encoding UTF8
    Get-Content -LiteralPath (Join-Path $output 'passed.txt')
} finally {
    try {
        foreach ($process in $clients) {
            try {
                $process.Refresh()
                if (!$process.HasExited) { [void][StoneSessionWindow]::PostMessage($process.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero); [void]$process.WaitForExit(3000) }
            } finally { $process.Dispose() }
        }
        if ($broker) {
            $broker.Refresh()
            if (!$broker.HasExited) { Invoke-Service @('--stop-service') | Out-Null }
            $broker.Dispose()
        }
    } finally {
        $env:STONE_LOG_DIRECTORY = $previousLogs; $env:LOCALAPPDATA = $previousLocal
    }
}
