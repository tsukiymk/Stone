# All ART UI/background runs share executable paths. Profiles and logs remain per run.
function Find-StoneClientLog([string]$RuntimeDirectory, [int]$ProcessId) {
    # Filenames identify applications and time; process identity stays in records.
    Get-ChildItem -LiteralPath (Join-Path $RuntimeDirectory 'Logs/StoneClient/Applications') -Filter '*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Where-Object { Select-String -LiteralPath $_.FullName -SimpleMatch "[p-$ProcessId]" -Quiet -ErrorAction SilentlyContinue } |
        Select-Object -First 1
}

function New-StoneTestRuntime([string]$SourceRuntime,[string]$OutputDirectory) {
    $repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..')).TrimEnd('\','/')
    $source=[IO.Path]::GetFullPath($SourceRuntime).TrimEnd('\','/')
    $debugRoot=Join-Path $repository 'bin/x64/Debug'
    if (![string]::Equals($source,$debugRoot,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'ART tests require the x64 Debug runtime from bin/x64/Debug'
    }
    $output=[IO.Path]::GetFullPath($OutputDirectory)
    if (!$output.StartsWith($debugRoot+'\',[StringComparison]::OrdinalIgnoreCase)) {
        throw 'ART test output must be inside bin/x64/Debug'
    }
    $directory=Join-Path $debugRoot 'test/windows-runtime'
    $identity='Stone shared Windows test runtime: '+$repository
    $marker=Join-Path $directory '.stone-test-runtime'
    if (!(Test-Path -LiteralPath $marker)) {
        if ((Test-Path -LiteralPath $directory) -and @(Get-ChildItem -LiteralPath $directory -Force).Count) { throw 'Shared test runtime is not marked as owned' }
        [IO.Directory]::CreateDirectory($directory) | Out-Null
        [IO.File]::WriteAllText($marker,$identity)
    }
    if ([IO.File]::ReadAllText($marker) -cne $identity) { throw 'Shared test runtime ownership mismatch' }
    $lease=[IO.File]::Open((Join-Path $directory '.runtime-lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $previousLocal=$env:LOCALAPPDATA; $previousLogs=$env:STONE_LOG_DIRECTORY
    try {
        $env:LOCALAPPDATA=Join-Path $output 'local'
        $env:STONE_LOG_DIRECTORY=Join-Path $directory 'Logs'
        foreach ($process in @(Get-Process StoneClient,StoneService -ErrorAction SilentlyContinue)) {
            try {
                if ($process.Path -and [string]::Equals([IO.Path]::GetDirectoryName($process.Path),$directory,[StringComparison]::OrdinalIgnoreCase)) {
                    throw 'The shared test runtime still has an active process; finish its cleanup before starting another run'
                }
            } finally { $process.Dispose() }
        }
        foreach ($name in @('StoneService.exe','StoneClient.exe','stone-art-runtime.jar','StoneImageTool.exe')) {
            Copy-StoneTestFile (Join-Path $SourceRuntime $name) (Join-Path $directory $name)
        }
        foreach ($library in Get-ChildItem -LiteralPath $SourceRuntime -Filter '*.dll' -File) {
            Copy-StoneTestFile $library.FullName (Join-Path $directory $library.Name)
        }
        $settings=Join-Path $directory 'settings'; $target=(Resolve-Path -LiteralPath (Join-Path $SourceRuntime 'settings')).Path
        if (Test-Path -LiteralPath $settings) {
            $item=Get-Item -LiteralPath $settings -Force
            if ($item.LinkType -ne 'Junction') { throw 'Shared settings path must be an owned junction' }
            if (![string]::Equals([string]@($item.Target)[0],$target,[StringComparison]::OrdinalIgnoreCase)) {
                [IO.Directory]::Delete($settings) # Remove the junction itself, without traversing its target.
            }
        }
        if (!(Test-Path -LiteralPath $settings)) { New-Item -ItemType Junction -Path $settings -Target $target | Out-Null }
        # A fixed Service path has one profile catalog. Clear only its own state
        # before a new run, so old fixtures cannot wake during an unrelated test.
        $stdout=Join-Path $OutputDirectory 'runtime-profile-store.txt'
        $query=Start-Process (Join-Path $directory 'StoneService.exe') -ArgumentList '--profile-store' -WorkingDirectory $directory -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $OutputDirectory 'runtime-profile-store.stderr.txt')
        $null=$query.Handle
        try {
            if (!$query.WaitForExit(15000)) { $query.Kill(); $null=$query.WaitForExit(5000); throw 'Shared test Service state query timed out' }
            if ($query.ExitCode -ne 0) { throw 'Cannot locate shared test Service state' }
        }
        finally { $query.Dispose() }
        $catalog=[IO.Path]::GetFullPath([IO.File]::ReadAllText($stdout).Trim())
        $boundary=[IO.Path]::GetFullPath((Join-Path $env:LOCALAPPDATA 'Trebledot Productions/Stone/Service')).TrimEnd('\')+'\'
        if (!$catalog.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($catalog) -ne 'job-profiles.bin') { throw 'Unexpected shared test state path' }
        foreach ($name in @('job-profiles.bin','pending-intents.json')) {
            $path=Join-Path ([IO.Path]::GetDirectoryName($catalog)) $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
        }
        return [pscustomobject]@{Directory=$directory;Lease=$lease;PreviousLocal=$previousLocal;PreviousLogs=$previousLogs}
    } catch { $lease.Dispose(); $env:LOCALAPPDATA=$previousLocal; $env:STONE_LOG_DIRECTORY=$previousLogs; throw }
}

function Close-StoneTestRuntime($Runtime) {
    try { $Runtime.Lease.Dispose() }
    finally { $env:LOCALAPPDATA=$Runtime.PreviousLocal; $env:STONE_LOG_DIRECTORY=$Runtime.PreviousLogs }
}

function Copy-StoneTestFile([string]$Source,[string]$Destination) {
    # Windows can briefly retain an image section after process exit or a file
    # scan. Active test processes were checked above; bound the remaining wait.
    $deadline=[DateTime]::UtcNow.AddSeconds(3)
    while ($true) {
        try { Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop; return }
        catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) { throw }
            Start-Sleep -Milliseconds 100
        }
    }
}

function Assert-StoneTestFirewallReady([string]$Program,[switch]$AllowPrompt) {
    $profiles=0
    foreach ($network in @(Get-NetConnectionProfile -ErrorAction Stop)) {
        # CIM exposes this field as a numeric value on some PowerShell versions.
        switch ([int]$network.NetworkCategory) { 0 {$profiles=$profiles -bor 4}; 1 {$profiles=$profiles -bor 2}; 2 {$profiles=$profiles -bor 1}; default { throw 'Unknown Windows network category' } }
    }
    if (!$profiles) { throw 'LAN test stopped: no active Windows network profile' }
    $rules=@(Get-NetFirewallApplicationFilter -PolicyStore ActiveStore -ErrorAction Stop | Where-Object {
        [string]::Equals($_.Program,$Program,[StringComparison]::OrdinalIgnoreCase)
    } | Get-NetFirewallRule | Where-Object { $_.Enabled -eq 'True' -and $_.Direction -eq 'Inbound' })
    $unconfigured=$false
    foreach ($bit in @(1,2,4)) {
        if (!($profiles -band $bit)) { continue }
        foreach ($protocol in @('TCP','UDP')) {
            $allowed=$false
            foreach ($rule in $rules) {
                $mask=[int]$rule.Profile
                if ($mask -ne 0 -and !($mask -band $bit)) { continue }
                $port=$rule | Get-NetFirewallPortFilter
                if ([string]$port.Protocol -notin @('Any','256',$protocol,$(if ($protocol -eq 'TCP') {'6'} else {'17'}))) { continue }
                if ($rule.Action -eq 'Block') { throw "LAN test stopped: inbound traffic is blocked for fixed program $Program" }
                $address=$rule | Get-NetFirewallAddressFilter
                $service=$rule | Get-NetFirewallServiceFilter
                $interface=$rule | Get-NetFirewallInterfaceFilter
                $interfaceType=$rule | Get-NetFirewallInterfaceTypeFilter
                if ($rule.Action -eq 'Allow' -and [string]$port.LocalPort -eq 'Any' -and [string]$port.RemotePort -eq 'Any' -and
                        [string]$address.LocalAddress -eq 'Any' -and [string]$address.RemoteAddress -eq 'Any' -and
                        [string]$service.Service -eq 'Any' -and [string]$interface.InterfaceAlias -eq 'Any' -and
                        [string]$interfaceType.InterfaceType -eq 'Any') { $allowed=$true }
            }
            if (!$allowed) { $unconfigured=$true }
        }
    }
    if ($unconfigured) {
        if (!$AllowPrompt) { throw "LAN test stopped: configure the fixed test program's firewall rule before running while attended: $Program" }
        Write-Warning "Attended LAN test: Windows may request firewall permission for the fixed program $Program"
    }
}
