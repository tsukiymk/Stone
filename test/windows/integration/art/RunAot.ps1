param(
    [Parameter(Mandatory)][string]$PreparedSystem,
    [Parameter(Mandatory)][string]$Output,
    [string]$JdkHome = $env:JAVA_HOME,
    [string]$AndroidSdk = (Join-Path $env:LOCALAPPDATA 'Android/Sdk'),
    [string]$Compiler,
    [string]$Probe,
    [switch]$EnableJit
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$debug = Join-Path $repository 'bin/x64/Debug'
if (!$Compiler) { $Compiler = Join-Path $debug 'StoneArtCompiler.exe' }
if (!$Probe) { $Probe = Join-Path $debug 'test/subsystem/Debug/stone-art-aot-tests.exe' }
function DebugPath([string]$Path) {
    $value = [IO.Path]::GetFullPath($Path)
    if (!$value.StartsWith($debug + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "AOT test path must be under bin/x64/Debug: $value"
    }
    return $value
}
$system = DebugPath $PreparedSystem
$destination = DebugPath $Output
$compilerPath = DebugPath $Compiler
$probePath = DebugPath $Probe
foreach ($file in @($compilerPath, $probePath, (Join-Path $system 'bin/dex2oat64'))) {
    if (![IO.File]::Exists($file)) { throw "Missing AOT input: $file" }
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$data = Join-Path $destination 'data'
$classes = Join-Path $destination 'classes'
$dex = Join-Path $destination 'dex'
New-Item -ItemType Directory -Force -Path $data, $classes, $dex, (Join-Path $data 'oat/x86_64') | Out-Null
$previousDirectory = Get-Location
try {
    Set-Location -LiteralPath $destination
    & (Join-Path $JdkHome 'bin/javac.exe') --release 8 -Xlint:-options -encoding UTF-8 -d $classes (Join-Path $PSScriptRoot 'AotProbe.java')
    if ($LASTEXITCODE) { throw 'AOT probe Java compilation failed' }
    $archive = Join-Path $destination 'probe.jar'
    & (Join-Path $JdkHome 'bin/jar.exe') --create --file $archive -C $classes .
    if ($LASTEXITCODE) { throw 'AOT probe JAR creation failed' }
    & (Join-Path $JdkHome 'bin/java.exe') -cp (Join-Path $AndroidSdk 'build-tools/36.0.0/lib/d8.jar') com.android.tools.r8.D8 `
        --min-api 23 --lib (Join-Path $AndroidSdk 'platforms/android-37.0/android.jar') --output $dex $archive
    if ($LASTEXITCODE) { throw 'AOT probe DEX creation failed' }
    $code = Join-Path $data 'probe.dex'
    if ([IO.File]::Exists($code)) { (Get-Item -LiteralPath $code).IsReadOnly = $false }
    Copy-Item -LiteralPath (Join-Path $dex 'classes.dex') -Destination $code -Force
    (Get-Item -LiteralPath $code).IsReadOnly = $true
    if ($EnableJit) {
        $jitClasses = Join-Path $destination 'jit-classes'
        $jitDex = Join-Path $destination 'jit-dex'
        New-Item -ItemType Directory -Force -Path $jitClasses, $jitDex | Out-Null
        & (Join-Path $JdkHome 'bin/javac.exe') --release 8 -Xlint:-options -encoding UTF-8 -d $jitClasses (Join-Path $PSScriptRoot 'JitFallbackProbe.java')
        if ($LASTEXITCODE) { throw 'JIT fallback Java compilation failed' }
        $jitArchive = Join-Path $destination 'jit.jar'
        & (Join-Path $JdkHome 'bin/jar.exe') --create --file $jitArchive -C $jitClasses .
        if ($LASTEXITCODE) { throw 'JIT fallback JAR creation failed' }
        & (Join-Path $JdkHome 'bin/java.exe') -cp (Join-Path $AndroidSdk 'build-tools/36.0.0/lib/d8.jar') com.android.tools.r8.D8 `
            --min-api 23 --lib (Join-Path $AndroidSdk 'platforms/android-37.0/android.jar') --output $jitDex $jitArchive
        if ($LASTEXITCODE) { throw 'JIT fallback DEX creation failed' }
        $jitCode = Join-Path $data 'jit.dex'
        if ([IO.File]::Exists($jitCode)) { (Get-Item -LiteralPath $jitCode).IsReadOnly = $false }
        Copy-Item -LiteralPath (Join-Path $jitDex 'classes.dex') -Destination $jitCode -Force
        (Get-Item -LiteralPath $jitCode).IsReadOnly = $true
    }
    function InvokeChecked([string]$Executable, [string[]]$Arguments, [string]$Name) {
        $quoted = @($Arguments | ForEach-Object { if ($_.Contains('"')) { throw 'Unexpected quote in test argument' }; '"' + $_ + '"' }) -join ' '
        $process = Start-Process -FilePath $Executable -ArgumentList $quoted -WorkingDirectory $destination -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $destination "$Name.stdout.txt") -RedirectStandardError (Join-Path $destination "$Name.stderr.txt")
        $null = $process.Handle
        try {
            if (!$process.WaitForExit(120000)) { Stop-Process -Id $process.Id -Force; throw "AOT stage timed out: $Name" }
            if ($process.ExitCode) { throw "AOT stage failed: $Name ($($process.ExitCode)); see $destination" }
        } finally { $process.Dispose() }
    }
    $boot = '/apex/com.android.art/javalib/core-oj.jar:/apex/com.android.art/javalib/core-libart.jar:/apex/com.android.art/javalib/okhttp.jar:/apex/com.android.art/javalib/bouncycastle.jar:/apex/com.android.art/javalib/apache-xml.jar:/apex/com.android.i18n/javalib/core-icu4j.jar:/apex/com.android.conscrypt/javalib/conscrypt.jar'
    InvokeChecked $compilerPath @('--system-directory', $system, '--data-dir', $data, '--', '--dex-file=/data/probe.dex',
        '--oat-file=/data/oat/x86_64/probe.odex', '--instruction-set=x86_64', '--compiler-filter=speed', '--copy-dex-files=false',
        '--generate-debug-info', '--class-loader-context=PCL[]', '--boot-image=/system/nonexistent.art',
        '--runtime-arg', '-Xnoimage-dex2oat', '--runtime-arg', "-Xbootclasspath:$boot", '-j2') 'compile'
    $oat = Join-Path $data 'oat/x86_64/probe.odex'
    $vdex = Join-Path $data 'oat/x86_64/probe.vdex'
    $before = @((Get-FileHash -LiteralPath $oat).Hash, (Get-FileHash -LiteralPath $vdex).Hash)
    $probeArguments = @($system, $data)
    if ($EnableJit) { $probeArguments += '--enable-jit' }
    InvokeChecked $probePath $probeArguments 'first'
    InvokeChecked $probePath $probeArguments 'second'
    $after = @((Get-FileHash -LiteralPath $oat).Hash, (Get-FileHash -LiteralPath $vdex).Hash)
    if (($before -join ':') -cne ($after -join ':')) { throw 'AOT artifacts changed during runtime reuse' }
    $mode = if ($EnableJit) { 'alongside JIT' } else { 'with JIT disabled' }
    foreach ($stage in @('first', 'second')) {
        if (!(Select-String -LiteralPath (Join-Path $destination "$stage.stdout.txt") -SimpleMatch "PASS: persisted x64 AOT code executed $mode" -Quiet)) {
            throw "AOT execution evidence is missing: $stage"
        }
    }
    Write-Output "PASS: x64 AOT compilation and two fresh runtimes $mode; actual OAT entry executed, no Windows JVM, thread/GC/exception/shutdown; unchanged artifacts. Logs: $destination"
} finally { Set-Location -LiteralPath $previousDirectory }
