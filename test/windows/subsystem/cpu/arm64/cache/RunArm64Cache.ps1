param(
    [Parameter(Mandatory)][string]$Output,
    [string]$Probe,
    [switch]$Eager,
    [switch]$Preload,
    [switch]$Compression,
    [ValidateSet('0','1')][string]$CompactPc='1'
)
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../../..'))
$debug=[IO.Path]::GetFullPath((Join-Path $repository 'bin/x64/Debug'))
if(!$Probe){$Probe=Join-Path $debug 'test/subsystem/Debug/stone-arm64-cache-tests.exe'}
function DebugPath([string]$Path){
    $full=[IO.Path]::GetFullPath($Path)
    if(!$full.StartsWith($debug+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw "Test path is outside x64 Debug: $full"}
    return $full
}
$destination=DebugPath $Output;$executable=DebugPath $Probe
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$cache=Join-Path $destination ('cache-'+[guid]::NewGuid().ToString('N'))
$previousBlock=$env:STONE_ARM64_BLOCK_JIT
$previousAdmission=$env:STONE_ARM64_CACHE_EAGER
$previousLoading=$env:STONE_ARM64_CACHE_PRELOAD
$previousCompression=$env:STONE_ARM64_CACHE_COMPRESSION
$previousCompactPc=$env:STONE_ARM64_COMPACT_PC
$env:STONE_ARM64_COMPACT_PC=$CompactPc
$env:STONE_ARM64_CACHE_COMPRESSION=if($Compression){'1'}else{'0'}
$env:STONE_ARM64_BLOCK_JIT='0'
$env:STONE_ARM64_CACHE_EAGER=if($Eager){'1'}else{'0'}
$env:STONE_ARM64_CACHE_PRELOAD=if($Preload){'1'}else{'0'}
function Run([string]$Mode,[string]$Directory=$cache,[string]$Binary=$executable,[string]$Label=$Mode){
    $arguments='"{0}" {1}' -f $Directory,$Mode
    $p=Start-Process -FilePath $Binary -ArgumentList $arguments -WorkingDirectory $destination -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $destination "$Label.stdout.txt") -RedirectStandardError (Join-Path $destination "$Label.stderr.txt")
    $null=$p.Handle
    try{
        if(!$p.WaitForExit(30000)){Stop-Process -Id $p.Id;throw "Cache stage timed out: $Label"}
        if($p.ExitCode){throw "Cache stage failed: $Label; see $destination"}
        Get-Content -LiteralPath (Join-Path $destination "$Label.stdout.txt")
    }finally{$p.Dispose()}
}
try{
    Run 'codec'
    Run 'session' -Directory (Join-Path $destination 'session')
    Run 'cold'
    Run 'inspect' -Label 'cold-inventory'
    $original=@(Get-ChildItem -LiteralPath $cache -Filter '*.stbc');if($original.Count -ne 1){throw 'Expected one image bundle'}
    $hash=(Get-FileHash -LiteralPath $original[0].FullName).Hash
    # Readers must accept both encodings independently of the write policy.
    $writtenMode=$env:STONE_ARM64_CACHE_COMPRESSION
    $env:STONE_ARM64_CACHE_COMPRESSION=if($Compression){'0'}else{'1'}
    $env:STONE_ARM64_COMPACT_PC=if($CompactPc -eq '1'){'0'}else{'1'}
    try{Run 'warm'}finally{$env:STONE_ARM64_CACHE_COMPRESSION=$writtenMode;$env:STONE_ARM64_COMPACT_PC=$CompactPc}
    if((Get-FileHash -LiteralPath $original[0].FullName).Hash -cne $hash){throw 'Read-only warm reuse rewrote the artifact'}
    if(!$Eager -and !$Preload){
        Run 'unused'
        if((Get-FileHash -LiteralPath $original[0].FullName).Hash -cne $hash){throw 'Unused cache bundle was rewritten'}
        Run 'concurrent-warm'
    }
    # Continuous same-process writes must reuse the latest authenticated file;
    # a concurrent process replacing it must instead force a merge.
    $refresh=Join-Path $destination ('refresh-'+[guid]::NewGuid().ToString('N'))
    Run 'cold' -Directory $refresh -Label 'refresh-cold'
    Run 'refresh' -Directory $refresh
    Run 'merge-check' -Directory $refresh -Label 'refresh-check'
    $merged=Join-Path $destination ('merged-'+[guid]::NewGuid().ToString('N'))
    Run 'cold' -Directory $merged -Label 'merge-cold'
    $parent=Start-Process -FilePath $executable -ArgumentList ('"{0}" merge-parent' -f $merged) -WorkingDirectory $destination -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $destination 'merge-parent.stdout.txt') -RedirectStandardError (Join-Path $destination 'merge-parent.stderr.txt')
    $null=$parent.Handle
    try{
        $deadline=[DateTime]::UtcNow.AddSeconds(15)
        while(!(Test-Path -LiteralPath (Join-Path $merged 'parent.ready'))){
            if($parent.HasExited -or [DateTime]::UtcNow -ge $deadline){throw 'Merge parent did not become ready'}
            Start-Sleep -Milliseconds 20
        }
        $env:STONE_ARM64_CACHE_COMPRESSION=if($Compression){'0'}else{'1'}
        try{Run 'merge-child' -Directory $merged}finally{$env:STONE_ARM64_CACHE_COMPRESSION=$writtenMode}
        [IO.File]::WriteAllText((Join-Path $merged 'parent.go'),'continue')
        if(!$parent.WaitForExit(15000) -or $parent.ExitCode){throw 'Merge parent failed'}
        Get-Content -LiteralPath (Join-Path $destination 'merge-parent.stdout.txt')
    }finally{if(!$parent.HasExited){Stop-Process -Id $parent.Id -Force};$parent.Dispose()}
    Run 'merge-check' -Directory $merged
    Run 'auth-empty'
    Run 'corrupt' -Label 'authenticated-empty-rejected'
    Run 'auth-packed-oversized'
    Run 'corrupt' -Label 'authenticated-packed-oversized-rejected'
    # Invalid authentication and excessive input size both fall back to native compilation.
    [IO.File]::WriteAllBytes($original[0].FullName,[byte[]](1,2,3,4))
    if(!$Eager -and !$Preload){
        Run 'unused' -Label 'unused-corrupt'
        if([BitConverter]::ToString([IO.File]::ReadAllBytes($original[0].FullName)) -cne '01-02-03-04'){throw 'Unused corrupt bundle was touched'}
    }
    Run 'corrupt'
    Run 'warm' -Label 'warm-repaired'
    $stream=[IO.File]::Open($original[0].FullName,[IO.FileMode]::Open,[IO.FileAccess]::Write)
    try{$stream.SetLength(32*1024*1024+65537)}finally{$stream.Dispose()}
    Run 'corrupt' -Label 'oversized'
    Run 'changed'
    $other=@(Get-ChildItem -LiteralPath $cache -Filter '*.stbc' | Where-Object FullName -ne $original[0].FullName)
    if($other.Count -ne 1){throw 'Changed ELF did not get a separate identity'}
    Copy-Item -LiteralPath $original[0].FullName -Destination $other[0].FullName -Force
    Run 'wrong-image'
    # Change only the PE build timestamp in a private copy, retaining executable code.
    $revision=Join-Path $destination 'translator-revision.exe'
    $bytes=[IO.File]::ReadAllBytes($executable);$pe=[BitConverter]::ToInt32($bytes,0x3c)
    $bytes[$pe+8]=$bytes[$pe+8] -bxor 1;[IO.File]::WriteAllBytes($revision,$bytes)
    Run 'translator' -Binary $revision
    Run 'disabled' -Directory (Join-Path $destination 'disabled')
    $blocked=Join-Path $destination 'not-a-directory';[IO.File]::WriteAllText($blocked,'test')
    Run 'unavailable' -Directory $blocked
    $bounded=Join-Path $destination ('bounded-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $bounded | Out-Null
    $old=Join-Path $bounded (('f'*64)+'.stbc')
    $stream=[IO.File]::Create($old);try{$stream.SetLength(129*1024*1024)}finally{$stream.Dispose()}
    [IO.File]::SetLastWriteTimeUtc($old,[DateTime]::UtcNow.AddDays(-1))
    Run 'trim' -Directory $bounded
    if([IO.File]::Exists($old)){throw 'Disk capacity fixture was not evicted'}
    Write-Output "PASS: native machine-code cache across fresh processes, relocation, callbacks/unwind, budgets, flags, SIMD, pinned baselines, concurrent merges, corruption, identities, disabled/error paths and disk limit. Logs: $destination"
}finally{$env:STONE_ARM64_BLOCK_JIT=$previousBlock;$env:STONE_ARM64_CACHE_EAGER=$previousAdmission;$env:STONE_ARM64_CACHE_PRELOAD=$previousLoading;$env:STONE_ARM64_CACHE_COMPRESSION=$previousCompression;$env:STONE_ARM64_COMPACT_PC=$previousCompactPc}
