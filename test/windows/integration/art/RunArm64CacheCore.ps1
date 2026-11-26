param(
    [Parameter(Mandatory)][string]$Probe,
    [Parameter(Mandatory)][string]$PreparedSystem,
    [Parameter(Mandatory)][string]$Dex,
    [Parameter(Mandatory)][string]$Output,
    [ValidateRange(60,1800)][int]$TimeoutSeconds=600,
    [switch]$Timing,
    [ValidateSet('0','imports','1')][string]$Profile='imports',
    [switch]$OffLast,
    [ValidateSet('default','direct','four-way')][string]$LocalPolicy='default',
    [switch]$DiskDisabledOnly,
    [ValidateSet('0','1')][string]$NativeBlocks='1',
    [ValidateSet('hot','eager')][string]$CacheAdmission='hot',
    [ValidateSet('lazy','preload')][string]$CacheLoading='lazy',
    [ValidateSet('on','off')][string]$BlockChaining='off',
    [ValidateSet('first','late')][string]$MappingLookup='first',
    [switch]$Compression,
    [switch]$StackStoreTrace,
    [switch]$ElfDataCache,
    [ValidateSet('0','1')][string]$CompactPc='1'
)
$ErrorActionPreference='Stop'
$repository=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$debug=[IO.Path]::GetFullPath((Join-Path $repository 'bin/x64/Debug'))
function DebugPath([string]$Path){
    $full=[IO.Path]::GetFullPath($Path)
    if(!$full.StartsWith($debug+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw "Core test path is outside x64 Debug: $full"}
    return $full
}
$probePath=DebugPath $Probe;$system=DebugPath $PreparedSystem;$dexPath=DebugPath $Dex;$destination=DebugPath $Output
if($NativeBlocks -eq '0' -and !$DiskDisabledOnly){throw 'NativeBlocks 0 requires DiskDisabledOnly'}
foreach($inputPath in @($probePath,$system,$dexPath)){if(!(Test-Path -LiteralPath $inputPath)){throw "Core input does not exist: $inputPath"}}
if(Test-Path -LiteralPath (Join-Path $destination 'cached-data')){throw 'Use a fresh output directory so the cold stage has no previous native cache'}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$saved=@{}
foreach($name in @('STONE_ARM64_BLOCK_JIT','STONE_ARM64_BLOCK_CHAIN','STONE_ARM64_MAPPING_FIRST','STONE_ARM64_DISK_CACHE','STONE_ARM64_LOCAL_DIRECT','STONE_ARM64_CACHE_EAGER','STONE_ARM64_CACHE_PRELOAD','STONE_ARM64_CACHE_TIMING','STONE_ARM64_PROFILE','STONE_ART_PROBE_JIT','STONE_ART_PROBE_JIT_WARMUP','STONE_ART_PROBE_JIT_THRESHOLD','STONE_ART_DATA_DIRECTORY','TEMP','TMP')){$saved[$name]=[Environment]::GetEnvironmentVariable($name,'Process')}
try{
    $saved['STONE_ARM64_COMPACT_PC']=$env:STONE_ARM64_COMPACT_PC
    $env:STONE_ARM64_COMPACT_PC=$CompactPc
    $saved['STONE_ARM64_ELF_DATA_CACHE']=$env:STONE_ARM64_ELF_DATA_CACHE
    $env:STONE_ARM64_ELF_DATA_CACHE=if($ElfDataCache){'1'}else{'0'}
    $saved['STONE_ARM64_STACK_STORE_TRACE']=$env:STONE_ARM64_STACK_STORE_TRACE
    $env:STONE_ARM64_STACK_STORE_TRACE=if($StackStoreTrace){'1'}else{'0'}
    $saved['STONE_ARM64_CACHE_COMPRESSION']=$env:STONE_ARM64_CACHE_COMPRESSION
    $env:STONE_ARM64_CACHE_COMPRESSION=if($Compression){'1'}else{'0'}
    $env:STONE_ARM64_BLOCK_JIT=$NativeBlocks;$env:STONE_ART_PROBE_JIT='1'
    $env:STONE_ARM64_BLOCK_CHAIN=if($BlockChaining -eq 'off'){'0'}else{'1'}
    $env:STONE_ARM64_MAPPING_FIRST=if($MappingLookup -eq 'late'){'0'}else{'1'}
    $env:STONE_ART_PROBE_JIT_WARMUP='1000';$env:STONE_ART_PROBE_JIT_THRESHOLD='10000'
    # Identical local correctness instrumentation in each stage; timings are diagnostic.
    $env:STONE_ARM64_PROFILE=$Profile
    $env:STONE_ARM64_CACHE_TIMING=if($Timing){'1'}else{'0'}
    $env:STONE_ARM64_LOCAL_DIRECT=if($LocalPolicy -eq 'direct'){'1'}else{'0'}
    $env:STONE_ARM64_CACHE_EAGER=if($CacheAdmission -eq 'eager'){'1'}else{'0'}
    $env:STONE_ARM64_CACHE_PRELOAD=if($CacheLoading -eq 'preload'){'1'}else{'0'}
    $results=@()
    $stages=if($DiskDisabledOnly){@('off')}elseif($OffLast){@('cold','warm','off')}else{@('off','cold','warm')}
    foreach($stage in $stages){
        $env:STONE_ARM64_DISK_CACHE=if($stage -eq 'off'){'0'}else{'1'}
        $env:STONE_ART_DATA_DIRECTORY=Join-Path $destination $(if($stage -eq 'off'){'off-data'}else{'cached-data'})
        $env:TEMP=Join-Path $destination 'temp';$env:TMP=$env:TEMP
        New-Item -ItemType Directory -Force -Path $env:TEMP | Out-Null
        $stdout=Join-Path $destination "$stage.stdout.txt";$stderr=Join-Path $destination "$stage.stderr.txt"
        $clock=[Diagnostics.Stopwatch]::StartNew()
        $p=Start-Process -FilePath $probePath -ArgumentList ('"{0}" "{1}"' -f $system,$dexPath) -WorkingDirectory $destination -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        $null=$p.Handle
        @{stage=$stage;pid=$p.Id;startedUtc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $destination 'live.json') -Encoding UTF8
        try{
            $next=30
            while(!$p.WaitForExit(500)){
                if($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds){Stop-Process -Id $p.Id -Force;throw "Core stage timed out: $stage"}
                if($clock.Elapsed.TotalSeconds -ge $next){Write-Output "ARM64 cache core $stage PID $($p.Id) running ($([int]$clock.Elapsed.TotalSeconds)s)";$next+=30}
            }
            $effectiveLoading=if($CacheAdmission -eq 'eager' -or $CacheLoading -eq 'preload'){'preload'}else{'lazy'}
            $result=@{stage=$stage;pid=$p.Id;exitCode=$p.ExitCode;elapsedMs=$clock.ElapsedMilliseconds;compactPc=$CompactPc;elfDataCache=[bool]$ElfDataCache;stackStoreTrace=[bool]$StackStoreTrace;compression=[bool]$Compression;profile=$Profile;cacheTiming=[bool]$Timing;localPolicy=$LocalPolicy;nativeBlocks=$NativeBlocks;blockChaining=$BlockChaining;mappingLookup=$MappingLookup;cacheAdmission=$CacheAdmission;cacheLoading=$effectiveLoading;requestedCacheLoading=$CacheLoading;
                binarySha256=(Get-FileHash -LiteralPath $probePath).Hash;dexSha256=(Get-FileHash -LiteralPath $dexPath).Hash}
            $results+=$result;$results|ConvertTo-Json -Depth 5|Set-Content -LiteralPath (Join-Path $destination 'results.json') -Encoding UTF8
            if($p.ExitCode){throw "ARM64 cache core failed: $stage; see $destination"}
            foreach($marker in @('ART JNI_CreateJavaVM completed without a host JVM.','PASS compiled Java method, JNI callbacks, two managed threads, GC and exception recovery.','ART DEX and JNI probe returned 42.')){
                if(!(Select-String -LiteralPath $stdout -SimpleMatch $marker -Quiet)){throw "Core evidence missing ($stage): $marker"}
            }
            if($stage -ne 'off'){
                $line=Select-String -LiteralPath $stderr -Pattern '\[arm64-cache\] images-loaded='|Select-Object -Last 1
                if(!$line -or $line.Line -notmatch 'errors=0 limited=0'){throw "Cache reported errors/limits: $stage"}
                if($stage -eq 'cold' -and $line.Line -notmatch 'files-written=[1-9]'){throw 'Cold core did not persist machine code'}
                if($stage -eq 'warm' -and $line.Line -notmatch 'hits=[1-9]'){throw 'Warm core did not reuse persisted native code'}
                if($stage -eq 'warm' -and $line.Line -notmatch 'images-loaded=[1-9][0-9]* blocks-loaded=[1-9]'){throw 'Warm core did not load persisted image artifacts'}
                Write-Output $line.Line
            }
            if($Timing){Select-String -LiteralPath $stderr -Pattern '\[arm64-cache(?:-io)?-timing\]' | ForEach-Object {$_.Line}}
        }finally{if(!$p.HasExited){Stop-Process -Id $p.Id -Force};$p.Dispose()}
    }
    if($DiskDisabledOnly){Write-Output "PASS: ARM64 image core returned 42 with disk cache disabled, native blocks $NativeBlocks, local policy $LocalPolicy. Logs: $destination"}
    else{Write-Output "PASS: ARM64 image core returned 42 with disk cache disabled/cold/warm, same JIT parameters and original DEX; warm native artifact reuse verified. Logs: $destination"}
}finally{foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}}
