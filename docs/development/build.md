# Building from source

Use CMake presets from the repository root. Windows x64 is the primary execution host. ARM64 can be cross-compiled, but native ART APK execution is not implemented. See [Using Stone](../usage.md) for project usage and runtime arguments.

## Tool dependencies

- Visual Studio 2026 desktop C++ tools and the Windows SDK. CMake must support the `Visual Studio 18 2026` generator. ARM64 cross-compilation also requires the MSVC ARM64 tools.
- Windows JDK 21. An explicit `STONE_JDK_HOME` takes precedence. Otherwise, `JAVA_HOME` is used; local installations are searched only when that environment variable is unset. The resolved path is stored in the CMake cache, so update that cache entry when switching JDKs.
- Android SDK `android.jar` and D8, configured through `STONE_ANDROID_SDK`, `STONE_ART_ANDROID_JAR`, and `STONE_ART_D8_JAR`. APK fixtures also require aapt2 and apksigner from the corresponding build-tools installation.
- The settings application requires the .NET 10 SDK, C# build tools, and NuGet restore. `STONE_BUILD_SETTINGS=OFF` builds the C++ components, including StoneImageTool, without the .NET SDK.
- An existing vcpkg checkout, selected by the `VCPKG_ROOT` environment variable, plus Git and PowerShell 7. vcpkg supplies its Python, CMake, and Ninja helpers; uncached dependencies require network access and the installed MSVC tools.

## Configuration and build

```powershell
$ARCH = 'x64' # or 'ARM64'
$env:VCPKG_ROOT = 'C:/dev/vcpkg' # existing external checkout
cmake --preset $ARCH
cmake --build "build/$ARCH" --config Debug -- /restore
```

`build/<preset>/` contains the CMake cache, projects, and intermediate files. `bin/<architecture>/<configuration>/` contains executables and JARs. The main x64 artifacts are `StoneService.exe`, `StoneClient.exe`, `StoneArtCompiler.exe`, `stone-art-runtime.jar`, and `StoneImageTool.exe`.

Set `'-DSTONE_VERSION=1.0.0.0-preview01'` when configuring to synchronize application
product versions, the settings About text, and numeric installer/file versions.
See [package version mapping](../windows/packaging.md#versions-and-installation-identity)
for supported ranges and preview-to-stable upgrade ordering.

JDK 21 supplies build-time JNI headers and compiles the ART bootstrap module with D8. The image reader is built as a standalone C++ executable. Installed users need neither a Windows JDK/JVM nor .NET.

`-- /restore` lets MSBuild restore NuGet dependencies before building StoneSettings.
Use it on clean builds, including when StoneSettings is an indirect dependency
of a test or packaging target. Native-only builds do not require NuGet restore.

## Third-party dependencies

[`CMakePresets.json`](../../CMakePresets.json) selects the official [vcpkg CMake toolchain](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration) from `$env{VCPKG_ROOT}`, the target triplet, and Stone's static CRT before `project()`. Provide the vcpkg checkout before configuring; Stone does not clone it or change its revision. The toolchain can bootstrap a missing vcpkg executable. Use a recent compatible vcpkg checkout; CI checks out the root manifest's baseline commit to also pin its build-tool versions.

Configuration restores the manifest's dependencies and makes them available to standard `find_package()`, `find_path()`, and `find_library()` calls. A binary-cache miss builds the package from source; later configurations reuse installed packages or vcpkg's binary cache. First-time source builds can take several minutes.

The root [`vcpkg.json`](../../vcpkg.json) owns the dependency list, version overrides, and overlay paths. Overlay ports are under `dep/vcpkg-overlay-ports/`, and Windows triplets are under `dep/vcpkg-overlay-triplets/`. The triplets and Stone use a static CRT; libraries are static by default and provide both Debug and Release configurations. Ports that need DLL deployment can select their own linkage and configuration.

To add a dependency, add its port to `vcpkg.json`, reconfigure, then use its documented CMake package and imported target in the consuming component. Review its licenses and deployment requirements, and update the [third-party notices](../third-party/THIRD_PARTY_NOTICES.txt). Statically linked libraries become part of the consumer; additional runtime DLLs need explicit packaging rules and notices before they can be shipped in MSI/MSIX.

The presets leave vcpkg's cache locations at their official defaults:

- Installed packages: `build/<preset>/vcpkg_installed/<triplet>/`.
- Downloads, tools, buildtrees, and temporary package outputs: under `VCPKG_ROOT`.
- Windows binary archives: `%LOCALAPPDATA%/vcpkg/archives`, unless overridden by the developer's vcpkg environment configuration.

Configure architectures sequentially when sharing one external checkout. Use `-DVCPKG_MANIFEST_INSTALL=OFF` to consume an already installed dependency tree without restoring it. Missing packages still fail configuration. An additional toolchain can be supplied through `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`.

Set machine-specific paths through the environment or an untracked `CMakeUserPresets.json`.

## Building the graphics runtime

Manifest restoration supplies the pinned ANGLE EGL/GLES DLLs during configuration, building from source on binary-cache misses. The triplets select Release DLLs for all Stone configurations, including Debug; ANGLE's implementation dependencies remain static. Native compilation uses headers under `<VCPKG_INSTALLED_DIR>/<VCPKG_TARGET_TRIPLET>/include`. Configuration fails if the required EGL/GLES headers or SDK D3DCompiler redistributable are missing. See [ANGLE sources and adaptation](../third-party/angle.md).

Building StoneClient installs `libEGL.dll`, `libGLESv2.dll`, and the SDK's `d3dcompiler_47.dll` into `bin/<architecture>/<configuration>/` beside `StoneClient.exe`. The `stone-graphics` install component deploys these DLLs, licenses, and vcpkg SPDX metadata. [Package collection](../windows/packaging.md#artifacts-and-package-contents) uses the same component.

To stage the configured graphics runtime:

```powershell
cmake --build build/x64 --config Debug --target stone-angle
```

To collect only that component into another directory, replace the example destination with the required path:

```powershell
cmake --install build/x64 --config Debug --prefix 'C:/stage/Stone' --component stone-graphics
```

## Publishing the settings application

```powershell
cmake --build build/x64 --config Debug --target StoneSettings -- /restore
cmake --build build/x64 --config Release --target StoneSettingsPublish
```

The settings application is written to `bin/<architecture>/<configuration>/settings/`. A normal Build is for debugging. Publish applies Native AOT and trimming according to `STONE_SETTINGS_AOT` and `STONE_SETTINGS_TRIM`. Both default to enabled, and AOT requires trimming. Neither controls ART Java AOT/JIT.

`StoneSettingsPublish` and package creation explicitly enable code optimization, including Debug packages. Ordinary Debug Build retains its debugging settings.

Publication retains the application's generated PRI resource index and XBF views alongside the executable. Packaging rejects outputs missing the settings resource index or the system image download view.

Windows App SDK and SDK BuildTools versions are pinned in CMake. Override them with `STONE_SETTINGS_WINDOWS_APP_SDK_VERSION` and `STONE_SETTINGS_WINDOWS_SDK_BUILDTOOLS_VERSION`, respectively. Publication retains a self-contained file layout.

## Installation packages

MSI, MSIX, and MSIXBundle targets are explicitly enabled with `STONE_PACKAGING=ON` and are not part of the default build. See [Windows packages](../windows/packaging.md) for dependencies, commands, artifacts, and signing.

## GitHub Actions branch builds

[build.yml](../../.github/workflows/build.yml) runs only on pushes to `main`.
It uses the same Windows runner and Visual Studio component checks as the
[release workflow](../../.github/workflows/release.yml), with JDK 21, .NET SDK 10,
Android SDK platform `android-37.0`, and build-tools `36.0.0`.

The workflow builds x64 Debug component tests and APK fixtures, runs the CTest
`unit` label, and retains test reports for 14 days, including failed runs.
After tests pass, it builds x64 and ARM64 Release applications and publishes
StoneSettings locally on the runner with Native AOT and trimming enabled.
Packaging is disabled; branch builds do not sign packages or create GitHub
Releases. Only test reports are uploaded. Tag pushes, other branches, and pull
request events do not trigger this workflow. A newer push to `main` cancels any
previous branch build still running.

## Component tests and fixtures

The CIT APK uses `com.trebledot.stone.cit` and its subpackages for its Java components and test identifiers. Its optional JNI fixtures load `libnative_test.so` on x86_64 and `libnative_jit_test.so` on arm64-v8a. After changing fixture identities, rebuild both the CIT APK and the ART probe DEX so manifests, reflection strings, and JNI names agree.

Component and fixture sources are under `test/windows/`. They are registered only for Windows x64 configurations with `STONE_TESTS=ON`. Build them with:

```powershell
cmake --preset x64 -DSTONE_TESTS=ON
cmake --build build/x64 --config Debug --target stone-windows-tests -- /restore
cmake --build build/x64 --config Debug --target stone-test-fixtures
```

Core tests use `bin/x64/Debug/test/subsystem/core/Debug/` with their fixture DLLs. The ART probe uses `bin/x64/Debug/test/subsystem/Debug/`; its graphics runtime is deployed beside it through the `stone-graphics` install component.

After a successful build, list and run the relevant CTest cases from the Debug directory:

```powershell
Push-Location bin/x64/Debug
try {
    ctest --test-dir test/ctest/Debug -C Debug -N
    ctest --test-dir test/ctest/Debug -C Debug -L unit --output-on-failure
} finally {
    Pop-Location
}
```

CTest component rules set the working directory and TEMP/TMP. Executables, input copies, working directories, TEMP/TMP, application data, caches, and outputs must remain under `bin/<architecture>/Debug/` for the actual architecture. The same isolation requirements apply when running tests or integration scripts directly. The `unit` label does not cover every component or application behavior; select names, labels, and integration drivers appropriate to the change.

Configuration, compilation, and testing each require a zero exit code to succeed. Resolve build or link failures before testing; do not use old artifacts to validate new code. A successful build does not establish target-device or APK compatibility. See the [repository guidelines](../../AGENTS.md) for full isolation and acceptance requirements.

### WebView provider and rendering checks

The ART probe's `--webview-providers` mode exercises the image parser/verifier, update-service contracts, and declared network permissions. With a selected provider it loads HTML from its own loopback HTTP server and checks JavaScript, touch focus, committed text, button clicks, alpha drawing, navigation, reload, scrolling, and software pixels. After those interactions it requires at least 60 seconds of advancing animation and repeated pixel checks; the rendering scenario has a 180-second callback deadline. Images with only stubs exercise explicit unavailability instead. These checks require a Windows x64 Debug build and a complete x86_64 system image. Rendering additionally requires a provider supported by the [current loader](../runtime/art.md#webview-provider-selection-and-loading) and an interactive Windows session; selection alone does not establish rendering compatibility.

From the repository root, configure tests and build the image tool, ART probe, runtime JAR, CIT APK, and signed archive fixtures:

```powershell
cmake --preset x64 -DSTONE_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw 'Test configuration failed.' }
cmake --build build/x64 --config Debug --target StoneImageTool stone-art-webview-tests -- /restore
if ($LASTEXITCODE -ne 0) { throw 'WebView test build failed.' }
```

Create `bin/x64/Debug/test/webview/input/` and copy the selected system image there as `system.img`. Keep the generated fixture layout: the probe reads signature fixtures from the `archives/` directory beside `art-probe/`. The following commands prepare that image and run the probe with all five required arguments. They isolate the working directory, data, caches, logs, and temporary files under the Debug test tree and restore environment overrides afterward:

```powershell
$debugRoot = (Resolve-Path 'bin/x64/Debug').Path
$testRoot = Join-Path $debugRoot 'test/webview'
$image = (Resolve-Path (Join-Path $testRoot 'input/system.img')).Path
$overrides = @{
    TEMP = Join-Path $testRoot 'temp'
    TMP = Join-Path $testRoot 'temp'
    LOCALAPPDATA = Join-Path $testRoot 'local'
    STONE_LOG_DIRECTORY = Join-Path $testRoot 'logs'
    STONE_ART_DATA_DIRECTORY = Join-Path $testRoot 'data'
}
$savedEnvironment = @{}
foreach ($name in $overrides.Keys) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    New-Item -ItemType Directory -Path $overrides[$name] -Force | Out-Null
}
Push-Location $testRoot
try {
    foreach ($name in $overrides.Keys) {
        [Environment]::SetEnvironmentVariable($name, $overrides[$name], 'Process')
    }
    $index = Join-Path $testRoot 'prepared-root.txt'
    & "$debugRoot/StoneImageTool.exe" --output-file $index $image "$testRoot/cache"
    if ($LASTEXITCODE -ne 0) { throw 'Image preparation failed.' }
    $prepared = (Get-Content -LiteralPath $index -Raw).Trim()
    & "$debugRoot/test/subsystem/Debug/stone-art-probe.exe" `
        $prepared "$debugRoot/test/fixtures/art-probe/classes.dex" `
        "$debugRoot/test/fixtures/cit/stone-cit.apk" --webview-providers `
        "$debugRoot/stone-art-runtime.jar"
    if ($LASTEXITCODE -ne 0) { throw 'WebView provider or rendering check failed.' }
} finally {
    Pop-Location
    foreach ($name in $overrides.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
```

On successful rendering, the probe writes `test/webview/data/webview-render/frame.png` under the Debug directory. An unavailable-provider result does not exercise page rendering. Record the image identity, provider identity, execution options, and observed outcome when reporting results.

The general `ctest -L unit` command does not run this image-dependent check. To register it with CTest, configure `STONE_CORE_ART_DIRECTORY`, `STONE_CORE_ART_DEX`, and `STONE_CORE_ART_CIT_APK` with absolute paths under the Debug tree and enable `STONE_CORE_ART_APPLICATION=ON`. After rebuilding, run `ctest --test-dir test/ctest/Debug -C Debug -R '^stone-art-webview-providers$' --no-tests=error --output-on-failure` from `bin/x64/Debug/`. CTest uses `test/art-webview-providers/` for `/data`, separately from the direct invocation above.
