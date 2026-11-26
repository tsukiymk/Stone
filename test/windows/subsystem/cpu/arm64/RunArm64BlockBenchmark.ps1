param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [ValidateRange(2,30)][int]$Rounds=6,
    [string]$RunDirectory=''
)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../..'))
$debug=[IO.Path]::GetFullPath((Join-Path $repo 'bin/x64/Debug'))
function DebugPath([string]$Path) {
    $absolute=[IO.Path]::GetFullPath($Path)
    if(!$absolute.StartsWith($debug+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Benchmark executables and outputs must stay inside bin/x64/Debug'
    }
    return $absolute
}
$exe=DebugPath $Executable
if(!(Test-Path -LiteralPath $exe -PathType Leaf) -or [IO.Path]::GetExtension($exe) -ne '.exe') {
    throw 'Specify the staged Debug stone-core-tests executable'
}
if(!$RunDirectory){$RunDirectory=Join-Path $debug 'test/arm64-block-benchmark'}
$out=DebugPath $RunDirectory
New-Item -ItemType Directory -Path (Join-Path $out 'temp') -Force | Out-Null
$saved=@{}
foreach($name in @('TEMP','TMP','STONE_ARM64_BLOCK_JIT','STONE_ARM64_PROFILE')) {
    $saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
$records=@()
try {
    $env:TEMP=Join-Path $out 'temp';$env:TMP=$env:TEMP
    $env:STONE_ARM64_PROFILE='0'
    for($round=0;$round -lt $Rounds;++$round) {
        # Alternate order across fresh processes; each case validates its result.
        $order=if($round%2){@(1,0)}else{@(0,1)}
        foreach($enabled in $order) {
            $env:STONE_ARM64_BLOCK_JIT=[string]$enabled
            $file=Join-Path $out "round-$round-$enabled"
            $process=Start-Process -FilePath $exe -ArgumentList 'arm64-block-benchmark' -WorkingDirectory $out `
                -WindowStyle Hidden -PassThru -RedirectStandardOutput "$file.stdout.txt" -RedirectStandardError "$file.stderr.txt"
            try {
                $null=$process.Handle;$process.WaitForExit()
                if($process.ExitCode -ne 0){throw "Benchmark failed; inspect $file.stderr.txt"}
                $cases=@()
                foreach($line in Get-Content -LiteralPath "$file.stdout.txt") {
                    if($line -match '^arm64-benchmark (\S+) enabled=(\d) iterations=(\d+) elapsed-us=(\d+) native-calls=(\d+) native-instructions=(\d+)$') {
                        $record=[pscustomobject]@{round=$round;pid=$process.Id;case=$Matches[1];enabled=[int]$Matches[2];
                            iterations=[long]$Matches[3];elapsedUs=[long]$Matches[4];
                            nativeCalls=[long]$Matches[5];nativeInstructions=[long]$Matches[6]}
                        if($record.enabled -ne $enabled -or ($record.nativeInstructions -gt 0) -ne [bool]$enabled) {
                            throw 'Benchmark did not execute the requested backend'
                        }
                        $records+=$record;$cases+=$record.case
                    }
                }
                if(($cases|Sort-Object) -join ',' -ne 'guarded-byte-sum,guarded-load-store-loop,integer-conditional-loop') {
                    throw 'Missing, duplicate or unknown benchmark cases'
                }
            } finally {$process.Dispose()}
        }
    }
    $summary=@()
    foreach($case in ($records.case|Select-Object -Unique)) {
        foreach($enabled in @(0,1)) {
            $times=@($records|Where-Object {$_.case -eq $case -and $_.enabled -eq $enabled}|
                ForEach-Object {[long]$_.elapsedUs}|Sort-Object)
            $median=($times[[int][Math]::Floor(($times.Count-1)/2)]+$times[[int][Math]::Floor($times.Count/2)])/2
            $summary+=[pscustomobject]@{case=$case;enabled=$enabled;rounds=$times.Count;
                medianUs=$median;minUs=$times[0];maxUs=$times[-1]}
        }
    }
    @{executable=$exe;sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash;
        records=$records;summary=$summary}|ConvertTo-Json -Depth 5|Set-Content -Encoding UTF8 -LiteralPath (Join-Path $out 'results.json')
    $summary|Format-Table -AutoSize
} finally {
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
