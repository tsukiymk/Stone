[CmdletBinding()]
param(
    [switch]$Check,
    [string]$ClangFormat
)

$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

if (-not $ClangFormat) {
    $command = Get-Command clang-format -CommandType Application -ErrorAction SilentlyContinue
    if ($command) {
        $ClangFormat = $command.Source
    } else {
        # Visual Studio's LLVM tools are not normally added to PATH.
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $candidates = @(& $vswhere -latest -products '*' -find 'VC/Tools/Llvm/x64/bin/clang-format.exe')
            if ($LASTEXITCODE -ne 0) { throw 'Visual Studio clang-format discovery failed' }
            if ($candidates.Count) { $ClangFormat = $candidates[0] }
        }
    }
}
if (-not $ClangFormat) {
    throw 'Install clang-format 22.x (or the Visual Studio LLVM tools), or pass -ClangFormat <path>.'
}
$version = & $ClangFormat --version
if ($LASTEXITCODE -ne 0 -or "$version" -notmatch '\bversion 22\.') {
    throw "Stone formatting requires clang-format 22.x. Found: $version"
}

# Use tracked project sources only, excluding oss, generated files and other languages.
$tracked = @(& git -c core.quotePath=false -C $sourceRoot ls-files --cached --deduplicate -- src test)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate tracked Stone sources with git' }
$sources = @($tracked | Where-Object { $_ -match '\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|inc)$' })
if (-not $sources.Count) { throw 'No tracked C/C++ sources found' }

$arguments = @('--style=file', '--fallback-style=none', '--Werror')
if ($Check) {
    $arguments += @('--dry-run', '--fail-on-incomplete-format')
} else {
    $arguments += @('-i', '--fail-on-incomplete-format')
}

# Batches stay below Windows' command-line length limit.
$failed = $false
for ($offset = 0; $offset -lt $sources.Count; $offset += 20) {
    $last = [Math]::Min($offset + 19, $sources.Count - 1)
    $batch = @($sources[$offset..$last] | ForEach-Object { Join-Path $sourceRoot $_ })
    & $ClangFormat @arguments @batch
    if ($LASTEXITCODE -ne 0) {
        $failed = $true
        Write-Warning "clang-format failed for batch: $($sources[$offset..$last] -join ', ')"
    }
}
if ($failed) { throw 'C/C++ formatting failed; see clang-format diagnostics above.' }
$action = if ($Check) { 'Checked' } else { 'Formatted' }
Write-Output "$action $($sources.Count) C/C++ files with $version"
