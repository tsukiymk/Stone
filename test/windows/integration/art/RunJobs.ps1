param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$Service,
    [Parameter(Mandatory)][string]$PreparedSystem,
    [Parameter(Mandatory)][string]$ProbeDex,
    [Parameter(Mandatory)][string]$FixtureApk,
    [Parameter(Mandatory)][string]$ArtRuntimeJar,
    [string]$AngleDirectory,
    [string]$OutputDirectory = 'bin/x64/Debug/test/jobs',
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
$runtime = Join-Path $output 'runtime'; [IO.Directory]::CreateDirectory($runtime) | Out-Null
$probePath = (Resolve-Path -LiteralPath $Probe).Path
$servicePath = (Resolve-Path -LiteralPath $Service).Path
$prepared = (Resolve-Path -LiteralPath $PreparedSystem).Path
$dex = (Resolve-Path -LiteralPath $ProbeDex).Path
$fixture = (Resolve-Path -LiteralPath $FixtureApk).Path
$jar = (Resolve-Path -LiteralPath $ArtRuntimeJar).Path
$client = Join-Path $runtime 'StoneClient.exe'; $broker = Join-Path $runtime 'StoneService.exe'
Copy-Item -LiteralPath $probePath -Destination $client -Force
Copy-Item -LiteralPath $servicePath -Destination $broker -Force
$saved = @{}
foreach ($name in @('STONE_ART_DATA_DIRECTORY','STONE_PROBE_SERVICE','STONE_ANGLE_DIR')) { $saved[$name] = [Environment]::GetEnvironmentVariable($name) }
$script:request = 0; $script:live = $null; $completed = $false
function Control([string]$Operation) {
    $script:request++; $stdout = Join-Path $output "request-$script:request.stdout.txt"
    $process = Start-Process -FilePath $broker -ArgumentList $Operation -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $output "request-$script:request.stderr.txt")
    $null = $process.Handle
    try {
        if (!$process.WaitForExit(15000) -or $process.ExitCode -ne 0) { throw "Job Service request failed: $Operation" }
        return [IO.File]::ReadAllText($stdout).Trim()
    } finally { $process.Dispose() }
}
function Stop-Broker {
    $status = Control '--status' | ConvertFrom-Json
    $process = Get-Process -Id $status.service; $null = $process.Handle
    try { Control '--stop-service' | Out-Null; if (!$process.WaitForExit(5000)) { throw 'Job Service did not exit' } }
    finally { $process.Dispose() }
}
function Run-Stage([string]$Stage, [string]$Apk, [string]$Profile) {
    $data = Join-Path $output "$Profile/data"; [IO.Directory]::CreateDirectory($data) | Out-Null
    $env:STONE_ART_DATA_DIRECTORY = $data
    $arguments = '"{0}" "{1}" "{2}" --jobs "{3}" {4}' -f $prepared, $dex, $Apk, $jar, $Stage
    $script:live = Start-Process -FilePath $client -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $output "$Stage.stdout.txt") -RedirectStandardError (Join-Path $output "$Stage.stderr.txt")
    $null = $script:live.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    try {
        while (!$script:live.WaitForExit(1000)) { if ([DateTime]::UtcNow -ge $deadline) { throw "Job $Stage timed out" } }
        if ($script:live.ExitCode -ne 0) { throw "Job $Stage failed; see $output" }
        if (!(Select-String -LiteralPath (Join-Path $output "$Stage.stdout.txt") -Pattern '^PASS ' -Quiet)) { throw "Job $Stage did not report semantic checks" }
        Get-Content -LiteralPath (Join-Path $output "$Stage.stdout.txt") | Where-Object { $_ -match '^(PASS |ART APK sample)' }
    } finally {
        if (!$script:live.HasExited) { Stop-Process -Id $script:live.Id -Force }
        $script:live.Dispose(); $script:live = $null
    }
}
try {
    $passed = Join-Path $output 'passed.txt'; if (Test-Path -LiteralPath $passed) { Remove-Item -LiteralPath $passed -Force }
    $env:STONE_PROBE_SERVICE = $broker
    if ($AngleDirectory) { $env:STONE_ANGLE_DIR = (Resolve-Path -LiteralPath $AngleDirectory).Path }
    Run-Stage 'fixture' $fixture 'fixture'
    Stop-Broker
    Run-Stage 'recover' $fixture 'fixture'
    'PASS actual image JobService callbacks and persisted fixture recovery.' |
        Set-Content -LiteralPath $passed -Encoding UTF8
    $completed = $true
} finally {
    foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    try { Stop-Broker } catch { if ($completed) { throw } else { Write-Warning $_ } }
}
