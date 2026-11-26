#requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-CertificateGpg {
    param(
        [Parameter(Mandatory)][ValidateSet('Encrypt', 'Decrypt')][string]$Mode,
        [Parameter(Mandatory)][string]$InputFile,
        [Parameter(Mandatory)][string]$OutputFile,
        [Parameter(Mandatory)][Security.SecureString]$Passphrase
    )
    $inputPath = (Get-Item -LiteralPath $InputFile -ErrorAction Stop).FullName
    $outputPath = [IO.Path]::GetFullPath($OutputFile)
    if (Test-Path -LiteralPath $outputPath) { throw "Refusing to overwrite: $outputPath" }
    if ($Passphrase.Length -eq 0) { throw 'GPG passphrase must not be empty.' }
    $command = Get-Command gpg -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $tool = if ($command) { $command.Source } else { Join-Path $env:ProgramFiles 'Git/usr/bin/gpg.exe' }
    if (!(Test-Path -LiteralPath $tool -PathType Leaf)) { throw 'GnuPG is required (gpg on PATH or Git for Windows).' }
    $msysGpg = $tool.Replace('\', '/').EndsWith('/usr/bin/gpg.exe', [StringComparison]::OrdinalIgnoreCase)
    function ConvertTo-GpgPath([string]$path) {
        if ($msysGpg -and $path -match '\A[A-Za-z]:[\\/]') {
            return '/' + $path.Substring(0, 1).ToLowerInvariant() + $path.Substring(2).Replace('\', '/')
        }
        return $path
    }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($outputPath)) -Force | Out-Null
    # Isolate GPG configuration/cache from the user's normal keyring. No keys are imported.
    # Git's MSYS gpg-agent uses Unix sockets with a short path limit.
    do {
        $gpgHome = Join-Path ([IO.Path]::GetTempPath()) "g$([guid]::NewGuid().ToString('N').Substring(0, 4))"
    } while (Test-Path -LiteralPath $gpgHome)
    New-Item -ItemType Directory -Path $gpgHome | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new($tool)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('--no-options', '--homedir', (ConvertTo-GpgPath $gpgHome), '--batch', '--no-tty',
        '--pinentry-mode', 'loopback', '--no-symkey-cache', '--passphrase-fd', '0', '--output', (ConvertTo-GpgPath $outputPath))) {
        $start.ArgumentList.Add($argument)
    }
    if ($Mode -eq 'Encrypt') {
        foreach ($argument in @('--cipher-algo', 'AES256', '--symmetric')) { $start.ArgumentList.Add($argument) }
    } else { $start.ArgumentList.Add('--decrypt') }
    $start.ArgumentList.Add('--')
    $start.ArgumentList.Add((ConvertTo-GpgPath $inputPath))
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (!$process.Start()) { throw 'Unable to start GnuPG.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $credential = [Management.Automation.PSCredential]::new('GPG', $Passphrase)
        $process.StandardInput.WriteLine($credential.GetNetworkCredential().Password)
        $process.StandardInput.Close()
        if (!$process.WaitForExit(60000)) {
            $process.Kill($true)
            $process.WaitForExit()
            throw 'GnuPG exceeded the 60-second time limit.'
        }
        $null = $stdout.GetAwaiter().GetResult()
        $diagnostic = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) { throw "GnuPG $Mode failed: $diagnostic" }
        if (!(Test-Path -LiteralPath $outputPath) -or (Get-Item -LiteralPath $outputPath).Length -eq 0) {
            throw 'GnuPG produced no certificate data.'
        }
    } catch {
        if (Test-Path -LiteralPath $outputPath) { Remove-Item -LiteralPath $outputPath -Force }
        throw
    } finally {
        # A detached MSYS agent can retain CTest/CI output handles after GPG exits.
        # Stop only the agent belonging to this invocation's isolated home.
        $gpgConf = Join-Path ([IO.Path]::GetDirectoryName($tool)) 'gpgconf.exe'
        if (Test-Path -LiteralPath $gpgConf) {
            & $gpgConf --homedir (ConvertTo-GpgPath $gpgHome) --kill gpg-agent *> $null
        }
        $process.Dispose()
    }
}
