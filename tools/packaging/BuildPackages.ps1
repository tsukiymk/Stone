[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ConfigFile,
    [Parameter(Mandatory)][ValidateSet('MSI', 'MSIX')][string]$Format
)
. "$PSScriptRoot/Common.ps1"
$config = Get-PackageConfig $ConfigFile
$tool = if ($Format -eq 'MSI') { Resolve-PackageTool $config.wix 'wix.exe' }
        else { Resolve-PackageTool $config.makeappx 'makeappx.exe' }
$msbuild = Resolve-PackageTool $config.msbuild 'MSBuild.exe'
$work = New-PackageWorkDirectory $config
$payload = Join-Path $work 'payload'
$settings = Join-Path $payload 'settings'
New-Item -ItemType Directory -Path $settings -Force | Out-Null
# Reuse the same CMake component as developer builds, directly from the
# configured imported targets rather than collecting arbitrary runtime DLLs.
$cmake = Resolve-PackageTool $config.cmake 'cmake.exe'
Invoke-PackageTool $cmake @('--install', $config.buildDirectory, '--config', $config.configuration,
    '--prefix', $payload, '--component', 'stone-graphics')
foreach ($name in @('libEGL.dll', 'libGLESv2.dll', 'd3dcompiler_47.dll')) {
    Assert-PackageBinary (Join-Path $payload "$name") $config.architecture
}
foreach ($name in @('LICENSE', 'vcpkg.spdx.json',
        'zlib.txt', 'egl-registry.txt', 'opengl-registry.txt')) {
    if (!(Get-Item -LiteralPath (Join-Path $payload "licenses/angle/$name")).Length) {
        throw "Empty graphics notice: $name"
    }
}

# Publish into a fresh directory: a Build output may contain obj/, old assemblies,
# diagnostics, or files from a previous non-AOT publish.
$oldLib = $env:LIB
try {
    Remove-Item Env:LIB -ErrorAction SilentlyContinue
    Invoke-PackageTool $msbuild @($config.settingsProject, '/restore', '/t:Publish', '/nologo', '/v:minimal',
        "/p:Configuration=$($config.configuration)", "/p:Platform=$($config.architecture)",
        "/p:PublishDir=$settings/", '/p:Optimize=true', '/p:SelfContained=true', '/p:WindowsAppSDKSelfContained=true')
} finally {
    $env:LIB = $oldLib
}
foreach ($required in @('StoneSettings.exe', 'StoneSettings.pri', 'App.xbf', 'Views/SystemPage.xbf',
        'Controls/SystemImageDownloadCard.xbf', 'Controls/ApplicationIcon.xbf', 'licenses/WindowsAppSDK-license.txt',
        'licenses/DotNET-MIT.txt', 'licenses/DotNET-third-party-notices.txt')) {
    if (!(Test-Path -LiteralPath (Join-Path $settings $required) -PathType Leaf)) {
        throw "Incomplete settings publication: $required"
    }
}
$files = @('StoneService.exe', 'StoneClient.exe', 'stone-art-runtime.jar', 'StoneImageTool.exe', 'THIRD_PARTY_NOTICES.txt')
if ($config.architecture -eq 'x64') { $files += 'StoneArtCompiler.exe' }
foreach ($file in $files) {
    $source = Join-Path $config.runtimeDirectory $file
    Assert-PackageTree $source
    if ([IO.Path]::GetExtension($source) -ieq '.exe') { Assert-PackageBinary $source $config.architecture }
    Copy-Item -LiteralPath $source -Destination $payload -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $payload 'licenses') -Force | Out-Null
foreach ($notice in Get-ChildItem -LiteralPath (Join-Path $config.runtimeDirectory 'licenses')) {
    if ($notice.Name -ieq 'angle') { continue }
    Assert-PackageTree $notice.FullName
    Copy-Item -LiteralPath $notice.FullName -Destination (Join-Path $payload 'licenses') -Recurse
}
Copy-Item -LiteralPath (Join-Path $config.sourceDirectory 'docs/third-party/THIRD_PARTY_NOTICES.txt') -Destination $payload -Force
if (!$config.crt.Count) { throw 'No MSVC redistributable DLLs were found by CMake.' }
foreach ($runtime in $config.crt) { Copy-Item -LiteralPath $runtime -Destination $payload }
Assert-PackageTree $settings
# PDBs are build artifacts, including Native AOT publication symbols.
Get-ChildItem -LiteralPath $settings -Recurse -File -Filter '*.pdb' | Remove-Item
$baseName = "Stone_$($config.version)_$($config.architecture)"
if ($Format -eq 'MSI') {
    # Component IDs depend on relative destination paths, never on staging paths.
    $directories = @{}
    $components = [Collections.Generic.List[string]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName) {
        $relative = [IO.Path]::GetRelativePath($payload, $file.FullName)
        # The executable must be the key path of the service's own component.
        if ($relative -ceq 'StoneService.exe') { continue }
        $parent = [IO.Path]::GetDirectoryName($relative)
        $directory = 'INSTALLFOLDER'
        while ($parent) {
            $directories[$parent] = Get-PackageId ('directory/' + $parent)
            $parent = [IO.Path]::GetDirectoryName($parent)
        }
        $parent = [IO.Path]::GetDirectoryName($relative)
        if ($parent) { $directory = $directories[$parent] }
        $id = Get-PackageId ('file/' + $relative)
        $source = ConvertTo-PackageXml $file.FullName
        $components.Add("      <Component Id=`"$id`" Directory=`"$directory`" Guid=`"*`" Bitness=`"always64`"><File Id=`"f$id`" Source=`"$source`" KeyPath=`"yes`" /></Component>")
    }
    function Write-PackageDirectories([string]$Parent, [string]$Indent) {
        foreach ($relative in $directories.Keys | Sort-Object) {
            if ([IO.Path]::GetDirectoryName($relative) -ne $Parent) { continue }
            $name = ConvertTo-PackageXml ([IO.Path]::GetFileName($relative))
            "$Indent<Directory Id=`"$($directories[$relative])`" Name=`"$name`">"
            Write-PackageDirectories $relative ($Indent + '  ')
            "$Indent</Directory>"
        }
    }
    # Separate, stable product families for x64 and ARM64.
    $upgrade = if ($config.architecture -eq 'x64') { '70CB4D3C-7765-44DC-A906-04BAA1018196' }
               else { 'EAE756BE-1919-49E7-BDCB-CA48C6879FE4' }
    $template = Join-Path $config.sourceDirectory 'msipackage/package.wix.in'
    $source = Expand-PackageTemplate $template @{
        # Keep all four fields for installed-app display; MSI compares only the first three.
        MSI_VERSION = $config.version; UPGRADE_CODE = $upgrade
        ARCH = $config.architecture; COMPONENTS = $components -join "`n"
        SERVICE_SOURCE = ConvertTo-PackageXml (Join-Path $payload 'StoneService.exe')
        BRAND_ICON_SOURCE = ConvertTo-PackageXml (Join-Path $config.sourceDirectory 'src/windows/settings/Assets/Stone.ico')
        DIRECTORIES = (Write-PackageDirectories '' '        ') -join "`n"
    }
    $wxs = Join-Path $work 'package.wxs'
    Set-Content -LiteralPath $wxs -Value $source -Encoding utf8
    $output = Join-Path $work ($baseName + '.msi')
    Invoke-PackageTool $tool @('build', $wxs, '-arch', $config.architecture, '-o', $output, '-pdbtype', 'none')
} else {
    $assets = Join-Path $payload 'Assets'
    New-Item -ItemType Directory -Path $assets -Force | Out-Null
    foreach ($asset in @('StoreLogo.png', 'Square150x150Logo.scale-200.png', 'Square44x44Logo.scale-200.png', 'Stone.png')) {
        Copy-Item -LiteralPath (Join-Path $config.sourceDirectory "src/windows/settings/Assets/$asset") -Destination $assets
    }
    $manifest = Expand-PackageTemplate (Join-Path $config.sourceDirectory 'msixgluepackage/AppxManifest.in') @{
        NAME = ConvertTo-PackageXml $config.name; PUBLISHER = ConvertTo-PackageXml $config.publisher
        VERSION = $config.version; ARCH = $config.architecture
    }
    Set-Content -LiteralPath (Join-Path $payload 'AppxManifest.xml') -Value $manifest -Encoding utf8
    # Packaged MRT looks for resources.pri at the package root. Merge the
    # published PRI files with the package identity and adjusted relative paths.
    $makepri = Resolve-PackageTool $config.makepri 'makepri.exe'
    $priConfigFile = Join-Path $work 'priconfig.xml'
    Invoke-PackageTool $makepri @('createconfig', '/cf', $priConfigFile, '/dq', 'en-US', '/o')
    [xml]$priConfig = Get-Content -LiteralPath $priConfigFile -Raw
    $priConfig.resources.RemoveChild($priConfig.resources.packaging) | Out-Null
    $priConfig.Save($priConfigFile)
    Invoke-PackageTool $makepri @('new', '/pr', $payload, '/cf', $priConfigFile, '/in', $config.name,
        '/of', (Join-Path $payload 'resources.pri'), '/o')
    $output = Join-Path $work ($baseName + '.msix')
    Invoke-PackageTool $tool @('pack', '/d', $payload, '/p', $output, '/o')
}
Sign-Package $config $output
Publish-Package $output (Join-Path $config.outputDirectory ([IO.Path]::GetFileName($output)))
