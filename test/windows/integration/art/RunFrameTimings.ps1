param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$Apk,
    [Parameter(Mandatory)][ValidatePattern('^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+$')][string]$PackageName,
    [Parameter(Mandatory)][string]$DataDirectory,
    [Parameter(Mandatory)][string]$SystemImage,
    [Parameter(Mandatory)][string]$CacheDirectory,
    [string]$AngleDirectory,
    [string]$OutputDirectory = 'bin/x64/Debug/test/art-frame-timings',
    [ValidateRange(10,3600)][int]$TimeoutSeconds = 180,
    [switch]$EnableJit
)
$ErrorActionPreference = 'Stop'
$profile = (Resolve-Path -LiteralPath $DataDirectory).Path
$original = (Resolve-Path -LiteralPath $Apk).Path
$timingFile = Join-Path $profile "data/stone-diagnostics/$PackageName/frame-timings.csv"
. (Join-Path $PSScriptRoot 'RunBackground.ps1') -RuntimeDirectory $RuntimeDirectory -FixtureApk $original `
    -SystemImage $SystemImage -CacheDirectory $CacheDirectory -AngleDirectory $AngleDirectory -OutputDirectory $OutputDirectory -TimeoutSeconds $TimeoutSeconds -LibraryOnly
$saved = @{}
foreach ($name in @('STONE_ART_FRAME_TIMINGS','STONE_ART_DIAGNOSTICS','STONE_ANGLE_DIR')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
try {
    $client = Join-Path $runtime 'StoneClient.exe'
    # Navigation timings do not validate inbound LAN traffic. Keep Windows
    # firewall policy unchanged; LAN exchange tests have their own preflight.
    $env:STONE_ART_FRAME_TIMINGS = '1'
    $env:STONE_ART_DIAGNOSTICS = '0'
    if ($AngleDirectory) { $env:STONE_ANGLE_DIR = (Resolve-Path -LiteralPath $AngleDirectory).Path }
    $requested = [DateTime]::UtcNow
    $arguments = '--apk "{0}" --data-dir "{1}" --system-image "{2}"' -f $original,$profile,$image
    if ($EnableJit) { $arguments += ' --enable-jit' }
    $process = Start-Process -FilePath $client -ArgumentList $arguments -WorkingDirectory $runtime -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output 'client.stdout.txt') -RedirectStandardError (Join-Path $output 'client.stderr.txt')
    $null = $process.Handle
    $clients += $process
    @{pid=$process.Id; runtime=$runtime; profile=$profile} | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $output 'ready.json') -Encoding UTF8
    Write-Output "Navigate in the APK window, then close it normally within $TimeoutSeconds seconds. PID=$($process.Id)"
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (!$process.WaitForExit(500) -and [DateTime]::UtcNow -lt $deadline) { }
    $process.Refresh()
    $exited = $process.HasExited
    $exitCode = if ($exited) { $process.ExitCode } else { $null }
    @{pid=$process.Id; exited=$exited; exitCode=$exitCode; timedOut=(!$exited); observedUtc=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'process-result.json') -Encoding UTF8
    if (!$exited -or $exitCode -ne 0) { throw "Timing Client did not exit normally (exited=$exited, code=$exitCode, timedOut=$(!$exited)); see $output" }
    if (!(Test-Path -LiteralPath $timingFile) -or [IO.File]::GetLastWriteTimeUtc($timingFile) -lt $requested) {
        throw 'Client did not produce fresh frame timings'
    }
    $lines = @(Get-Content -LiteralPath $timingFile)
    if ($lines.Count -lt 3 -or $lines[0] -notmatch '^# dropped=\d+$' -or
        $lines[1] -cne 'frame,start_ns,gap_ns,touches,repainted,cancelled,prepare_ns,layout_ns,paint_ns,children_ns,pixels_ns,present_ns,diagnostics_ns,total_ns') {
        throw 'Invalid frame timing header or empty capture'
    }
    $rows = @($lines | Select-Object -Skip 1 | ConvertFrom-Csv)
    if ($rows.Count -gt 8192) { throw 'Unbounded frame timing capture' }
    $previousEnd = 0L
    for ($index = 0; $index -lt $rows.Count; $index++) {
        $row = $rows[$index]
        foreach ($column in $row.PSObject.Properties) {
            $number = 0L
            if (![long]::TryParse($column.Value,[ref]$number) -or $number -lt 0) { throw "Invalid frame timing value: $($column.Name)" }
        }
        $sum = 0L
        foreach ($column in @('prepare_ns','layout_ns','paint_ns','children_ns','pixels_ns','present_ns','diagnostics_ns')) { $sum += [long]$row.$column }
        if ([long]$row.frame -ne $index -or [long]$row.start_ns -lt $previousEnd -or
            [long]$row.gap_ns -ne ([long]$row.start_ns - $previousEnd) -or
            $sum -gt [long]$row.total_ns -or [long]$row.total_ns -le 0 -or
            $row.repainted -notin @('0','1') -or $row.cancelled -notin @('0','1')) {
            throw "Inconsistent frame timing row $index"
        }
        $previousEnd = [long]$row.start_ns + [long]$row.total_ns
    }
    Copy-Item -LiteralPath $timingFile -Destination (Join-Path $output 'frame-timings.csv') -Force
    Write-Output "PASS production window frame timings: $($rows.Count) frames; $($lines[0]); normal shutdown. Output: $output"
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name,$saved[$name]) }
    Clear-BackgroundRuntime
}
