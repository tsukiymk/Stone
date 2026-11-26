# Building from source

Use CMake presets from the repository root. Windows x64 is the primary execution host. ARM64 can be cross-compiled, but native ART APK execution is not implemented. See [Using Stone](../usage.md) for project usage and runtime arguments.

## Tool dependencies

- Visual Studio 2026 desktop C++ tools and the Windows SDK. CMake must support the `Visual Studio 18 2026` generator. ARM64 cross-compilation also requires the MSVC ARM64 tools.
- Windows JDK 21. An explicit `STONE_JDK_HOME` takes precedence. Otherwise, `JAVA_HOME` is used; local installations are searched only when that environment variable is unset. The resolved path is stored in the CMake cache, so update that cache entry when switching JDKs.
- Android SDK `android.jar` and D8, configured through `STONE_ANDROID_SDK`, `STONE_ART_ANDROID_JAR`, and `STONE_ART_D8_JAR`. APK fixtures also require aapt2 and apksigner from the corresponding build-tools installation.
- The settings application requires the .NET 10 SDK, C# build tools, and NuGet restore. `STONE_BUILD_SETTINGS=OFF` builds the C++ components, including StoneImageTool, without the .NET SDK.

## Configuration and build

```powershell
$ARCH = 'x64' # or 'ARM64'
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
