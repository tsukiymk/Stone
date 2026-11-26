[CmdletBinding()]
param([Parameter(Mandatory)][string]$ConfigFile)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../../../../tools/packaging/Common.ps1"
$config = Get-PackageConfig $ConfigFile
$debugRoot = [IO.Path]::GetFullPath((Join-Path $config.sourceDirectory "bin/$($config.architecture)/Debug"))
if ($config.configuration -cne 'Debug' -or
    !([IO.Path]::GetFullPath($config.outputDirectory)).StartsWith($debugRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Packaging checks require outputs inside bin/<architecture>/Debug.'
}
$run = Join-Path $debugRoot 'test/run/packaging/checks'
New-Item -ItemType Directory -Path $run -Force | Out-Null
$oldTemp = $env:TEMP
$oldTmp = $env:TMP
Push-Location $debugRoot
try {
    $env:TEMP = $env:TMP = $run
    $baseName = "Stone_$($config.version)_$($config.architecture)"
    $msixPath = Join-Path $config.outputDirectory ($baseName + '.msix')
    $msiPath = Join-Path $config.outputDirectory ($baseName + '.msi')
    foreach ($path in @($msixPath, $msiPath)) {
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing artifact: $path" }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if (!(Get-Content -LiteralPath ($path + '.sha256') -Raw).StartsWith($hash)) { throw "Hash mismatch: $path" }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($msixPath)
    try {
        $names = @($archive.Entries.FullName)
        foreach ($required in @('AppxManifest.xml', 'resources.pri', 'StoneClient.exe', 'StoneService.exe',
                'stone-art-runtime.jar', 'StoneImageTool.exe',
                'settings/StoneSettings.exe', 'settings/StoneSettings.pri', 'settings/App.xbf',
                'settings/Views/SystemPage.xbf', 'settings/Controls/SystemImageDownloadCard.xbf', 'settings/Controls/ApplicationIcon.xbf',
                'Assets/Stone.png', 'settings/Assets/Stone.png', 'settings/Assets/Stone.ico',
                'settings/licenses/WindowsAppSDK-license.txt', 'settings/licenses/DotNET-MIT.txt',
                'settings/licenses/DotNET-third-party-notices.txt', 'vcruntime140.dll', 'msvcp140.dll')) {
            if ($required -notin $names) { throw "MSIX missing payload: $required" }
        }
        foreach ($asset in @('Assets/Stone.png', 'settings/Assets/Stone.png', 'settings/Assets/Stone.ico')) {
            $stream = $archive.GetEntry($asset).Open()
            try { $actual = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)) }
            finally { $stream.Dispose() }
            $source = Join-Path $config.sourceDirectory ('src/windows/settings/Assets/' + [IO.Path]::GetFileName($asset))
            if ($actual -cne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash) { throw "Incorrect Stone brand asset: $asset" }
        }
        if ($config.architecture -eq 'x64' -and 'StoneArtCompiler.exe' -notin $names) { throw 'Missing AOT compiler.' }
        if ($names | Where-Object { $_ -match '(?i)(^|/)(test|logs|obj|cache|data)/|\.(pdb|apk|img|msi)$|stone-(java-runtime|image-tools)\.jar$|(^|/)jvm\.dll$' }) {
            throw 'MSIX contains development/application data.'
        }
        $reader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
        try { [xml]$manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
        if ($manifest.Package.Identity.Name -cne $config.name -or
            $manifest.Package.Identity.Version -cne $config.version -or
            $manifest.Package.Identity.Publisher -cne $config.publisher -or
            $manifest.Package.Identity.ProcessorArchitecture -cne $config.architecture) { throw 'MSIX identity mismatch.' }
        $services = @($manifest.SelectNodes('//*[local-name()="Extension" and @Category="windows.service"]'))
        if ($services.Count -ne 1 -or $services[0].Executable -cne 'StoneService.exe' -or
            $services[0].Service.Name -cne 'StoneService' -or $services[0].Service.StartupType -cne 'auto' -or
            $services[0].Service.StartAccount -cne 'localSystem' -or $services[0].Service.Arguments -cne '--service') {
            throw 'MSIX service registration mismatch.'
        }
        foreach ($capability in @('packagedServices', 'localSystemServices')) {
            if (!$manifest.SelectSingleNode("//*[local-name()='Capability' and @Name='$capability']")) {
                throw "MSIX service capability missing: $capability"
            }
        }
    } finally { $archive.Dispose() }

    $installer = New-Object -ComObject WindowsInstaller.Installer
    $database = $installer.OpenDatabase($msiPath, 0)
    try {
        $view = $database.OpenView('SELECT `Value` FROM `Property` WHERE `Property` = ''ProductVersion''')
        $view.Execute(); $record = $view.Fetch()
        if (!$record -or $record.StringData(1) -cne $config.version) {
            throw 'MSI must retain the four-field package version for installed-app display.'
        }
        $view.Close()
        $view = $database.OpenView('SELECT `Value` FROM `Property` WHERE `Property` = ''ARPPRODUCTICON''')
        $view.Execute(); $record = $view.Fetch()
        if (!$record -or $record.StringData(1) -cne 'StoneProductIcon') { throw 'MSI product icon is missing.' }
        $view.Close()
        $view = $database.OpenView('SELECT `Icon_` FROM `Shortcut` WHERE `Shortcut` = ''StoneSettingsShortcut''')
        $view.Execute(); $record = $view.Fetch()
        if (!$record -or $record.StringData(1) -cne 'StoneProductIcon') { throw 'MSI shortcut icon is missing.' }
        $view.Close()
        $view = $database.OpenView('SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`')
        $view.Execute()
        $publisherDirectory = $false; $installDirectory = $false
        while ($record = $view.Fetch()) {
            if ($record.StringData(1) -ceq 'PublisherFolder' -and $record.StringData(2) -ceq 'ProgramFiles64Folder' -and
                $record.StringData(3).Split('|')[-1] -ceq 'Trebledot Productions') { $publisherDirectory = $true }
            if ($record.StringData(1) -ceq 'INSTALLFOLDER' -and $record.StringData(2) -ceq 'PublisherFolder' -and
                $record.StringData(3).Split('|')[-1] -ceq 'Stone') { $installDirectory = $true }
        }
        $view.Close()
        if (!$publisherDirectory -or !$installDirectory) { throw 'MSI default installation directory mismatch.' }
        $view = $database.OpenView('SELECT `FileName` FROM `File`')
        $view.Execute()
        $msiFiles = @()
        while ($record = $view.Fetch()) { $msiFiles += $record.StringData(1).Split('|')[-1] }
        $view.Close()
        foreach ($file in @('StoneService.exe', 'StoneClient.exe', 'StoneSettings.exe', 'StoneSettings.pri',
                'WindowsAppSDK-license.txt', 'DotNET-MIT.txt', 'vcruntime140.dll', 'stone-art-runtime.jar', 'StoneImageTool.exe')) {
            if ($file -notin $msiFiles) { throw "MSI missing payload: $file" }
        }
        if ($msiFiles | Where-Object { $_ -match '\.(pdb|apk|img)$|^stone-(java-runtime|image-tools)\.jar$|^jvm\.dll$' }) { throw 'Unexpected MSI payload.' }
        $view = $database.OpenView('SELECT `Name`, `ServiceType`, `StartType`, `ErrorControl`, `StartName`, `Arguments`, `Component_` FROM `ServiceInstall`')
        $view.Execute()
        $record = $view.Fetch()
        if (!$record -or $record.StringData(1) -cne 'StoneService' -or $record.IntegerData(2) -ne 16 -or
            $record.IntegerData(3) -ne 2 -or $record.IntegerData(4) -ne 32769 -or
            $record.StringData(5) -cne 'LocalSystem' -or $record.StringData(6) -cne '--service') {
            throw 'MSI service installation mismatch.'
        }
        $component = $record.StringData(7)
        if ($view.Fetch()) { throw 'Unexpected extra MSI service.' }
        $view.Close()
        $view = $database.OpenView('SELECT `Name`, `Event`, `Wait`, `Component_` FROM `ServiceControl`')
        $view.Execute()
        $record = $view.Fetch()
        if (!$record -or $record.StringData(1) -cne 'StoneService' -or $record.IntegerData(2) -ne 163 -or
            $record.IntegerData(3) -ne 1 -or $record.StringData(4) -cne $component -or $view.Fetch()) {
            throw 'MSI must start on install, stop on upgrade/removal, and delete on uninstall.'
        }
        $view.Close()
        $view = $database.OpenView('SELECT `Component`.`Component`, `Component`.`KeyPath`, `File`.`File`, `File`.`FileName` FROM `Component`, `File` WHERE `Component`.`KeyPath` = `File`.`File`')
        $view.Execute()
        $serviceKeyPath = $false
        while ($record = $view.Fetch()) {
            if ($record.StringData(1) -ceq $component -and $record.StringData(4).Split('|')[-1] -ceq 'StoneService.exe') {
                $serviceKeyPath = $true
            }
        }
        if (!$serviceKeyPath) { throw 'MSI service executable is not its component key path.' }
        $view.Close()
    } finally {
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) | Out-Null
        [Runtime.InteropServices.Marshal]::FinalReleaseComObject($installer) | Out-Null
    }

    function Assert-Rejected([string]$Name, [hashtable]$Changes, [string]$Expected, [string]$Script = 'BuildMsixBundle.ps1') {
        $candidate = $config | ConvertTo-Json -Depth 10 | ConvertFrom-Json
        foreach ($key in $Changes.Keys) { $candidate.$key = $Changes[$key] }
        $path = Join-Path $run ($Name + '.json')
        $candidate | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $path -Encoding utf8
        $arguments = @('-NoProfile', '-File', (Join-Path $config.sourceDirectory "tools/packaging/$Script"), '-ConfigFile', $path)
        if ($Script -eq 'BuildPackages.ps1') { $arguments += @('-Format', 'MSI') }
        $result = & pwsh @arguments 2>&1 | Out-String
        $result | Set-Content -LiteralPath (Join-Path $run ($Name + '.log')) -Encoding utf8
        if ($LASTEXITCODE -eq 0 -or $result -notmatch $Expected) { throw "Expected rejection $Name`: $result" }
    }
    Assert-Rejected 'missing-input' @{ bundleInputs = @((Join-Path $run 'missing.msix')) } 'Missing bundle input'
    Assert-Rejected 'identity-mismatch' @{ name = 'Trebledot.Other'; bundleInputs = @($msixPath) } 'identity/version mismatch'
    Assert-Rejected 'duplicate-architecture' @{ bundleInputs = @($msixPath, $msixPath) } 'duplicate bundle architecture'
    Assert-Rejected 'fourth-version-field' @{ version = '1.0.0.1' } 'major.minor.build.0'
    Assert-Rejected 'disabled-service-msi' @{ installService = $false } 'require service registration' 'BuildPackages.ps1'
    Assert-Rejected 'disabled-service-bundle' @{ installService = $false } 'require service registration'
    Write-Host 'PASS: MSI/MSIX payloads, identities, four-field versions, hashes, service registration/lifecycle, six failure paths.'
} finally {
    Pop-Location
    $env:TEMP = $oldTemp
    $env:TMP = $oldTmp
}
