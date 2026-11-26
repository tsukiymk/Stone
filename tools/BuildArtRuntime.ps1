param(
    [Parameter(Mandatory)][string]$JdkHome,
    [Parameter(Mandatory)][string]$AndroidJar,
    [Parameter(Mandatory)][string]$D8Jar,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'
$javac = Join-Path $JdkHome 'bin/javac.exe'
$java = Join-Path $JdkHome 'bin/java.exe'
$jar = Join-Path $JdkHome 'bin/jar.exe'
foreach ($inputFile in @($javac, $java, $jar, $AndroidJar, $D8Jar)) {
    if (-not (Test-Path -LiteralPath $inputFile -PathType Leaf)) { throw "Missing ART runtime build input: $inputFile" }
}
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../src/android/runtime/java'))
$sources = @(Get-ChildItem -LiteralPath $sourceRoot -Recurse -File -Filter '*.java' | Sort-Object FullName | ForEach-Object FullName)
if (-not $sources.Count) { throw 'No Stone ART runtime Java sources found' }
$target = [IO.Path]::GetFullPath($Output)
$destination = [IO.Path]::GetDirectoryName($target)
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$stage = Join-Path $destination ('.art-build-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
try {
    $classes = Join-Path $stage 'classes'
    $dex = Join-Path $stage 'dex'
    New-Item -ItemType Directory -Path $classes,$dex | Out-Null
    $strings = Join-Path $stage 'generated/ArtStrings.java'
    & (Join-Path $PSScriptRoot 'BuildArtStrings.ps1') -ResourceRoot (Join-Path $sourceRoot '../res') -Output $strings
    $sources += $strings
    # Hidden image API signatures are compile inputs only. In particular, the
    # dollar-named stub has the binary name of Parcel's real nested helper;
    # it must never replace an Android class in the shipped runtime archive.
    $apiClasses = Join-Path $stage 'compile-api'
    New-Item -ItemType Directory -Path $apiClasses | Out-Null
    $apiSources = @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot '../compile') -Recurse -File -Filter '*.java' | ForEach-Object FullName)
    & $javac --release 8 -Xlint:-options -encoding UTF-8 -classpath $AndroidJar -d $apiClasses @apiSources
    if ($LASTEXITCODE -ne 0) { throw 'ART compile-only API compilation failed' }
    $apiJar = Join-Path $stage 'compile-api.jar'
    & $jar --create --file $apiJar -C $apiClasses .
    if ($LASTEXITCODE -ne 0) { throw 'ART compile-only API archive creation failed' }
    & $javac --release 8 -Xlint:-options -encoding UTF-8 -classpath "$AndroidJar;$apiJar" -d $classes @sources
    if ($LASTEXITCODE -ne 0) { throw 'ART runtime Java compilation failed' }
    $archive = Join-Path $stage 'stone-art-runtime.jar'
    & $jar --create --file $archive -C $classes .
    if ($LASTEXITCODE -ne 0) { throw 'ART runtime class archive creation failed' }
    $entries = @(& $jar --list --file $archive)
    if ($LASTEXITCODE -ne 0 -or ($entries | Where-Object { $_.StartsWith('android/') })) {
        throw 'Compile-only Android API classes must not enter the ART runtime archive'
    }
    & $java -cp $D8Jar com.android.tools.r8.D8 --release --min-api 30 --lib $AndroidJar --lib $apiJar --output $dex $archive
    if ($LASTEXITCODE -ne 0) { throw 'ART runtime DEX compilation failed' }
    & $jar --update --file $archive -C $dex classes.dex
    if ($LASTEXITCODE -ne 0) { throw 'ART runtime DEX archive creation failed' }
    Copy-Item -LiteralPath $archive -Destination $target -Force
    Write-Output $target
} finally {
    $resolvedStage = [IO.Path]::GetFullPath($stage)
    if (-not $resolvedStage.StartsWith($destination + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'ART runtime staging directory escaped its output directory'
    }
    Remove-Item -LiteralPath $resolvedStage -Recurse -Force
}
