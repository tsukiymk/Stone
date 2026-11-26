# Windows packages

Stone provides MSI, MSIX, and MSIXBundle targets for x64 and ARM64. This document describes installation behavior, packaging dependencies, build configuration, and validation requirements. See [building from source](../development/build.md) for native and settings application build requirements.

## Installation methods and runtime requirements

| Format | Installation behavior | Application entry points |
| --- | --- | --- |
| MSI | Per-machine installation to `%ProgramFiles%\Trebledot Productions\Stone`; registers and starts the automatic `StoneService` Windows service | Stone settings shortcut in the Start menu |
| MSIX | Windows manages the installation directory and registers the packaged automatic `StoneService` service | Stone settings entry and the `StoneClient.exe` execution alias |
| MSIXBundle | Combines MSIX packages for multiple architectures; Windows selects the applicable package | Same as MSIX |

Choose either MSI or MSIX. Migration between them and concurrent operation are not guaranteed. Before installing, upgrading, or uninstalling, close StoneSettings and application Clients belonging to that installation. The installer stops the service and its brokers during replacement or removal; brokers request graceful Client shutdown and have a bounded exit deadline. Uninstalling a package retains existing Stone application data.

Packages do not include Android system images or a JDK. Installation does not automatically download these components or external runtime dependencies such as ANGLE DLLs. StoneSettings provides a user-initiated API 37 image download to `%LOCALAPPDATA%\Trebledot Productions\Stone\System Images`; see [image downloads](settings.md#downloading-an-android-image). The package includes native `StoneImageTool.exe`; initial preparation and execution require no Windows JDK/JVM or installed .NET runtime. APK execution on ARM64 hosts is not implemented. Package architecture support does not establish APK execution support. See [Using Stone](../usage.md) for runtime requirements.

Packages register `StoneService` as an automatic own-process Windows service under LocalSystem, with the `--service` argument. The service only supervises brokers launched under signed-in users' tokens and environments; application execution, host state, clipboard, notifications, and user files remain in those user sessions. No Android runtime is loaded in the LocalSystem host. Before login the supervisor waits without launching Android applications.

MSI uses Windows Installer's `ServiceInstall` and `ServiceControl` tables to register and start the service, stop it during replacement or removal, and delete it on uninstall. MSIX declares a `windows.service` extension and the `packagedServices`/`localSystemServices` restricted capabilities, with registration and removal managed by Windows. Installing these services requires administrator approval; distribution through the Microsoft Store is subject to restricted-capability approval. The default `STONE_PACKAGE_INSTALL_SERVICE=ON` is required; `OFF` is rejected. If an existing build cache sets it to `OFF`, reconfigure with `-DSTONE_PACKAGE_INSTALL_SERVICE=ON`.

Use elevated `Stop-Service StoneService`, `Start-Service StoneService`, and `Restart-Service StoneService` for maintenance. Stop requests graceful Client shutdown, with bounded broker cleanup. A broker that exits while the supervisor is running is recreated in its login session. Source builds whose directory differs from the registered service retain on-demand brokers. See [architecture](../architecture/overview.md) for process boundaries and recovery limits.

## Packaging dependencies

The following tools are required in addition to normal build dependencies. Build scripts do not install packaging tools automatically.

| Tool | Purpose | CMake path setting |
| --- | --- | --- |
| PowerShell 7 (`pwsh`) | Run packaging scripts | `STONE_PACKAGE_POWERSHELL` |
| WiX Toolset 4.0.6 and its .NET runtime | Generate MSI packages | `STONE_WIX_EXECUTABLE` |
| Windows SDK MakeAppx | Generate MSIX and MSIXBundle packages | `STONE_MAKEAPPX_EXECUTABLE` |
| Windows SDK MakePri | Generate MSIX resource indexes | `STONE_MAKEPRI_EXECUTABLE` |
| Windows SDK SignTool | Sign packages and verify signatures | `STONE_SIGNTOOL_EXECUTABLE` |

Tools can be found through `PATH` or specified with these variables. Windows SDK tools are also searched for in the SDK directory selected for the current build. SignTool is unnecessary when signing is disabled.

## Building packages

Run all commands below from the repository root. Use Release for distribution. Debug packages are only for isolated validation, depend on the development environment's Debug CRT, and are unsuitable for distribution.

### Single-architecture packages

This example installs WiX in a local tools directory and generates x64 MSI and MSIX packages:

```powershell
dotnet tool install wix --version 4.0.6 --tool-path build/tools/wix
cmake --preset x64 -DSTONE_PACKAGING=ON `
  -DSTONE_BUILD_SETTINGS=ON `
  -DSTONE_PACKAGE_VERSION=1.0.0.0 `
  -DSTONE_WIX_EXECUTABLE="$PWD/build/tools/wix/wix.exe"
cmake --build build/x64 --config Release --target msipackage -- /restore
cmake --build build/x64 --config Release --target msixinstallerpackage -- /restore
```

Packaging is explicitly enabled with `STONE_PACKAGING=ON`, is excluded from the default build, and requires `STONE_BUILD_SETTINGS=ON`. Targets and aliases are:

| Artifact | Build target | Alias |
| --- | --- | --- |
| MSI | `msipackage` | `StoneMSI` |
| MSIX | `msixinstallerpackage` | `StoneMSIX` |
| MSIXBundle | `bundle` | `StoneMSIXBundle` |

Packaging targets build the required native programs and Publish the settings application into a fresh staging directory. Settings Native AOT and trimming use the configured `STONE_SETTINGS_AOT` and `STONE_SETTINGS_TRIM` values.

### Multi-architecture MSIXBundle

After configuring x64 as above, configure ARM64 with the same version and MSIX identity, then create the bundle:

```powershell
cmake --preset ARM64 -DSTONE_PACKAGING=ON -DSTONE_PACKAGE_VERSION=1.0.0.0
cmake --build build/ARM64 --config Release --target msixinstallerpackage -- /restore
cmake --build build/x64 --config Release --target bundle -- /restore
```

`STONE_PACKAGE_BUNDLE_ARCHITECTURES` defaults to `x64;ARM64`. The `bundle` target rebuilds the current architecture's MSIX and reads previously generated artifacts for the other selected architectures in the same configuration. It does not start cross-architecture builds automatically. For x64-only distribution, configure the build directory producing the bundle with `-DSTONE_PACKAGE_BUNDLE_ARCHITECTURES=x64`.

All inputs must have `Name`, `Publisher`, and `Version` matching the current configuration, and each must have a unique x64 or ARM64 architecture. Validation reads `AppxManifest.xml` inside each package. Missing inputs or failed checks terminate bundling.

## GitHub Actions release builds

[release.yml](../../.github/workflows/release.yml) uses the GitHub-hosted
[`windows-2025-vs2026` runner](https://github.com/actions/runner-images/blob/main/images/windows/Windows2025-VS2026-Readme.md)
with Visual Studio 2026. The repository's [.vsconfig](../../.vsconfig) declares
the native and managed desktop workloads, x64/ARM64 C++ tools, CMake support,
Windows SDK 26100, and WinUI C# tools. The workflow checks these components with
`vswhere`, adds missing components through Visual Studio Installer
[`modify --config`](https://learn.microsoft.com/en-us/visualstudio/install/import-export-installation-configurations),
and verifies the result before configuring CMake. A required restart fails the
job. Local developers can import the same file in Visual Studio Installer.
It also sets up
JDK 21, .NET SDK 10, the .NET 8 runtime needed by WiX 4.0.6, Android SDK platform
`android-37.0`, and build-tools `36.0.0`. Android SDK licenses must be accepted on
the runner; the GitHub-hosted image supplies these. CMake presets and packaging
targets remain the source of build rules.

Push a version tag to build a release, for example:

```powershell
git tag v1.0.0.0
git push origin v1.0.0.0
```

Tags accept `v<major>.<minor>.<build>.0` and a `-previewNN` suffix, for example
`v1.0.0.0-preview01`. The fourth field must be `0`; the first three fields cannot
have leading zeros. The preview sequence may have leading zeros.
Alternatively, run **Actions > Release build > Run workflow**, choose the source branch or tag,
and enter the same version with or without a single lowercase `v` prefix.
The workflow passes this display version as `STONE_VERSION`; numeric installer
versions are mapped as described below. Preview tags create prerelease drafts.
Manual runs upload build artifacts without creating a GitHub Release.

The pipeline builds Release MSI/MSIX packages for x64 and ARM64, publishes the
settings application with Native AOT and trimming, and creates the combined
MSIXBundle. Release builds do not compile or run Debug tests or APK fixtures;
the [main branch workflow](../../.github/workflows/build.yml) retains those checks.
All five packages are checksum-verified and uploaded as
`Stone_<version>_<architecture>` files inside the `Stone-<version>-windows`
Actions artifact, retained for 30 days.
Only explicitly collected release assets are uploaded; staging directories are
excluded.

Successful tag builds create a **draft** GitHub Release with the packages
and GitHub-generated release notes. Checksums are passed between workflow jobs
for verification; `.sha256` files are not uploaded to Actions artifacts or Releases.
Review the draft and complete installation/application acceptance before publishing.
The release job alone receives `contents: write`; the build job uses read-only
repository access. An existing release for the tag causes the release step to
fail instead of replacing its assets. Use a new version for a replacement
release. Rerunning failed jobs before a draft is created is supported.

Signing is optional for build inspection. Configure distribution signing as
described under [encrypted signing certificate](#encrypted-signing-certificate).
Without signing configuration, packages are unsigned; MSIX/MSIXBundle deployment
requires trusted signing.

The pipeline does not install packages, run image-dependent ART integration
tests, or establish APK compatibility. See the validation requirements below.

## Versions and installation identity

| Setting | Default | Constraints |
| --- | --- | --- |
| `STONE_VERSION` | Empty (use legacy numeric version) | Display/release version `major.minor.build.0[-previewNN]`; major 1–255, minor 0–255, build 0–64, preview 1–998 |
| `STONE_PACKAGE_VERSION` | `1.0.0.0` | Legacy numeric input when `STONE_VERSION` is empty; major 1–255, minor 0–255, build 0–65535, revision 0 |
| `STONE_PACKAGE_NAME` | `Trebledot.Stone` | Stable MSIX `Name`; keep unchanged across upgrades |
| `STONE_PACKAGE_PUBLISHER` | `CN=Trebledot` | Stable MSIX `Publisher`; keep unchanged across upgrades and match the signing certificate Subject |

When `STONE_VERSION` is provided, it controls the application display version and
derives the numeric package version; do not also set `STONE_PACKAGE_VERSION`.
The numeric third field is `build * 1000 + stage`, where stage is the preview
sequence or `999` for a stable release. For example:

| Display version | MSI ProductVersion | MSIX and executable file version |
| --- | --- | --- |
| `1.0.0.0-preview01` | `1.0.1.0` | `1.0.1.0` |
| `1.0.0.0-preview02` | `1.0.2.0` | `1.0.2.0` |
| `1.0.0.0` | `1.0.999.0` | `1.0.999.0` |
| `1.0.1.0-preview01` | `1.0.1001.0` | `1.0.1001.0` |

This preserves preview-to-stable upgrade ordering for both formats because MSI
compares only the first three numeric fields. MSI retains all four fields in
`ProductVersion` so Windows installed-app lists display the complete numeric
version, including the final `.0`. With legacy `STONE_PACKAGE_VERSION=1.0.0.0`
and an empty `STONE_VERSION`, this is `1.0.0.0`. The fourth field must remain zero;
changing it alone cannot identify an upgrade. See Microsoft's
[ProductVersion rules](https://learn.microsoft.com/windows/win32/msi/productversion).
Client, Service, ImageTool, and ArtCompiler
product version strings retain the full display version. StoneSettings uses it
as its assembly informational version and in its About text; numeric file versions
match the installer. Local package filenames use the numeric version. Release
asset filenames, Actions artifact names, and GitHub Release titles use the
display version.

For local builds, configure x64 as follows and use the same version for ARM64:

```powershell
cmake --preset x64 -DSTONE_PACKAGING=ON '-DSTONE_VERSION=1.0.0.0-preview01'
```

The mapping applies to stable versions too. Existing users of the legacy numeric
input must compare the installed numeric version before migrating; a mapped
version must exceed it. After adopting `STONE_VERSION`, continue using it for
future previews and stable releases. To return to legacy numeric input, explicitly
clear the cached value with `-DSTONE_VERSION=`.

MSI uses a separate fixed `UpgradeCode` for x64 and ARM64, blocks downgrades, and
supports rollback if removal of the older version fails.
Existing installations made with a three-field MSI retain their registered
display version. To refresh the display using a rebuilt package with the same
first three fields, uninstall the existing MSI before installing the rebuilt
package. For a normal upgrade, increase one of the first three numeric fields.

## Signing

Packages are unsigned by default for content inspection or a separate signing workflow. Before deployment, MSIX and MSIXBundle packages must be signed with a certificate trusted by the target device whose Subject matches `Publisher`.

### Encrypted signing certificate

The release workflow uses the password-protected PFX encrypted with GPG at
`tools/packaging/signature.pfx.gpg`, following the persistent-key approach in
[Surgo's scripts](https://github.com/Trebledot/surgo/tree/master/scripts).
Keep the same signing identity for subsequent releases. Only the encrypted
certificate belongs in source control; private keys and passwords remain local
or in GitHub Secrets.

Configure these repository Actions secrets and variables:

| Kind | Name | Value |
| --- | --- | --- |
| Secret | `STONE_SIGNING_CERTIFICATE_PASSWORD` | PFX password |
| Secret | `STONE_SIGNING_GPG_PASSPHRASE` | GPG symmetric-decryption passphrase for `tools/packaging/signature.pfx.gpg` |
| Variable | `STONE_SIGNING_TRUST_SELF_SIGNED` | Set to `true` only for self-signed CI signing; enables temporary trust on the disposable GitHub-hosted runner |
| Variable | `STONE_PACKAGE_PUBLISHER` | Certificate Subject; defaults to `CN=Trebledot` and must remain stable across upgrades |
| Variable | `STONE_PACKAGE_TIMESTAMP_URL` | Required RFC 3161 timestamp server URL when signing |

GnuPG must be on PATH or available at Git for Windows' `usr/bin/gpg.exe`.
The helper handles native Windows and Git MSYS paths, passes the GPG passphrase
through standard input, and rejects failed decryptions or existing outputs.
The workflow decrypts to the runner's temporary directory, imports the PFX into
the current user's certificate store, and deletes the plaintext even on failure.
Passwords are supplied through secret environment variables. The certificate
must be valid and trusted for SignTool verification. For self-signed signing,
the explicit variable above permits temporary `LocalMachine/Root` trust on the
GitHub-hosted runner so SignTool `/pa` can verify MSI and MSIX signatures.
An `always()` step removes imported certificates, private keys, and temporary
trust; the build never installs trust on an end user's device.

Signed build artifacts and draft Releases include only the public certificate
`Stone.cer` alongside the packages. Its SHA-256
is passed between workflow jobs for verification without uploading a checksum file.
Certificate installation instructions are documented below rather than uploaded
as a separate text file.
Without the GPG file and signing secrets, inspection builds remain unsigned; supplying
signing secrets without the file fails. The ordinary `main` build never
decrypts certificates or uses signing secrets.

### User installation with a self-signed certificate

Download `Stone.cer` and the signed package from the same Release. Verify the
certificate thumbprint through a trusted publisher channel. From an administrator
PowerShell in the download directory, import the public certificate:

```powershell
Import-Certificate -FilePath .\Stone.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople
```

Then double-click the MSIX/MSIXBundle. End users should use `TrustedPeople`,
not `Root`. A normally trusted CA certificate does not need this manual trust
step. Self-signing does not establish public CA trust; see Microsoft's
[package signing guidance](https://learn.microsoft.com/windows/msix/package/sign-msix-package-guide).

### Local package signing

For built-in signing, first prepare the certificate and private key in the current user's `My` certificate store, then configure its thumbprint and an optional RFC 3161 timestamp server:

```powershell
cmake --preset x64 -DSTONE_PACKAGING=ON `
  '-DSTONE_PACKAGE_PUBLISHER=CN=Your Publisher' `
  '-DSTONE_PACKAGE_CERTIFICATE_THUMBPRINT=REPLACE_WITH_CERTIFICATE_THUMBPRINT' `
  '-DSTONE_PACKAGE_TIMESTAMP_URL=REPLACE_WITH_TIMESTAMP_SERVER_URL'
```

Rebuild the required packaging target to invoke SignTool signing and verification. Private keys and passwords must not enter source code, command lines, or the CMake cache. Packaging scripts do not create certificates or install certificate trust; SignTool verification requires an already trusted chain on the signing machine. Use the same `Publisher` for every architecture in a multi-architecture package.

## Artifacts and package contents

Packages are written to `bin/<architecture>/<configuration>/packages/`. Directory architecture names are `x64` or `ARM64`; architecture names in filenames are lowercase.

| Format | Filename |
| --- | --- |
| MSI | `Stone_<version>_<architecture>.msi` |
| MSIX | `Stone_<version>_<architecture>.msix` |
| MSIXBundle | `Stone_<version>_<sorted_architectures_joined_by_underscores>.msixbundle`, for example `Stone_1.0.0.0_arm64_x64.msixbundle` |

Each package has a matching `.sha256` file. Bundles are written under the architecture directory that initiated bundling. `packages/work/<unique-directory>/` retains staging files and generated manifests for each packaging run. Failed builds, packaging, or signing do not replace a previously successful package with the same name.

Contents include StoneService, StoneClient, StoneArtCompiler on x64, `stone-art-runtime.jar`, StoneImageTool, the self-contained settings Publish output, MSVC redistributable DLLs, and third-party licenses. Only explicitly listed native output files are collected. Settings publication dependencies are retained, with PDBs removed. Tests, logs, APKs, images, application data, and caches are excluded. Symbolic links and junctions in input trees are rejected.

MSIX runs the packaged desktop programs directly and does not embed MSI. Its manifest disables file and registry write virtualization. Application data roots, Shell registration, and cross-process identity checks follow Stone's runtime rules. StoneService manages application shortcuts and protocols by application identity; see [Windows desktop integration](integration.md).

Stone's user state is under `%LOCALAPPDATA%\Trebledot Productions\Stone`. Android's default data root (`--data-dir`), including system/application data, image caches, and the saved image selection, is its `User Data` subdirectory. Process text logs and Android journals use the sibling `Logs` directory. Broker queues and Shell icon caches use the `Service` and `Shell` subdirectories; the SCM supervisor writes failures to the Windows Application event log. Existing data in the former `%LOCALAPPDATA%\Stone` directory is retained and is not automatically migrated; `--data-dir` can explicitly select an existing data root. See [logging and diagnostics](../development/diagnostics.md) for log paths and isolated test overrides.

## Validation

Use current Debug build artifacts for validation. Executables, packages, configuration input copies, working directories, TEMP/TMP, and outputs must remain under `bin/<actual-architecture>/Debug/`. See the [repository guidelines](../../AGENTS.md) for isolation requirements.

After generating Debug MSI and MSIX packages for the same architecture, run [CheckPackages.ps1](../../test/windows/integration/packaging/CheckPackages.ps1). Its required `-ConfigFile` argument must point to a copy of the packaging configuration inside the Debug tree. CMake generates the original at `build/<preset>/Debug/package.json`. The script checks contents, identity, SHA-256, service registration and lifecycle declarations, executable key-path ownership, and rejection of invalid inputs. It neither installs packages nor changes certificate trust.

Installation, upgrade, removal, actual MSIX activation, and APK execution require separate validation in a dedicated Windows test environment. Passing package content checks does not replace these behavioral checks.

## Source locations

| Path | Responsibilities |
| --- | --- |
| [msipackage/](../../msipackage/) | WiX MSI manifests and build targets |
| [msixgluepackage/](../../msixgluepackage/) | MSIX application identity, manifest, and payload packaging target |
| [msixinstaller/](../../msixinstaller/) | MSIX package entry point and MSIXBundle target |
| [cmake/WindowsPackaging.cmake](../../cmake/WindowsPackaging.cmake) | Versions, identity, tool discovery, and configuration generation |
| [tools/packaging/](../../tools/packaging/) | Publication, file collection, signing, and bundle validation scripts |
