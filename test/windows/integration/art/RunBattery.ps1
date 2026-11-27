param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$PreparedSystem,
    [Parameter(Mandatory)][string]$ProbeDex,
    [Parameter(Mandatory)][string]$Apk,
    [Parameter(Mandatory)][string]$ArtRuntimeJar,
    [Parameter(Mandatory)][string]$DataDirectory,
    [string]$OutputDirectory = 'bin/x64/Debug/test/battery/results',
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$probePath = (Resolve-Path -LiteralPath $Probe).Path
$prepared = (Resolve-Path -LiteralPath $PreparedSystem).Path
$dex = (Resolve-Path -LiteralPath $ProbeDex).Path
$apkPath = (Resolve-Path -LiteralPath $Apk).Path
$runtime = (Resolve-Path -LiteralPath $ArtRuntimeJar).Path
$data = [IO.Path]::GetFullPath($DataDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($data) | Out-Null
[IO.Directory]::CreateDirectory($output) | Out-Null
$passed = Join-Path $output 'passed.txt'
if (Test-Path -LiteralPath $passed -PathType Leaf) { Remove-Item -LiteralPath $passed -Force }
$previous = $env:STONE_ART_DATA_DIRECTORY
$process = $null
try {
    $env:STONE_ART_DATA_DIRECTORY = $data
    $arguments = '"{0}" "{1}" "{2}" --battery "{3}"' -f $prepared, $dex, $apkPath, $runtime
    $process = Start-Process -FilePath $probePath -ArgumentList $arguments -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $output 'stdout.txt') -RedirectStandardError (Join-Path $output 'stderr.txt')
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (!$process.WaitForExit(1000)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw "Battery test timed out; see $output" }
    }
    if ($process.ExitCode -ne 0) { throw "Battery test returned $($process.ExitCode); see $output" }
    if (!(Select-String -LiteralPath (Join-Path $output 'stdout.txt') -SimpleMatch 'PASS framework battery state' -Quiet)) {
        throw 'Battery test did not report semantic checks'
    }
    "APK SHA-256: $((Get-FileHash -LiteralPath $apkPath -Algorithm SHA256).Hash.ToLowerInvariant())" |
        Set-Content -LiteralPath (Join-Path $output 'passed.txt') -Encoding UTF8
    Get-Content -LiteralPath (Join-Path $output 'stdout.txt') | Where-Object { $_ -match '^(Windows battery|Image battery|PASS |ART APK sample)' }
    Get-Content -LiteralPath (Join-Path $output 'passed.txt')
} finally {
    $env:STONE_ART_DATA_DIRECTORY = $previous
    if ($process) { if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }; $process.Dispose() }
}
