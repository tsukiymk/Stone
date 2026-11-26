# Stone Repository Guidelines

These guidelines apply to the entire repository. Before making changes, read [CONTRIBUTING.md](CONTRIBUTING.md) and the documentation for the affected features. For build details, follow the [build instructions](docs/development/build.md) and current CMake rules.

## Project and Directories

Stone is an Android compatibility layer for Windows, primarily built with C++20/MSVC, Java, and C#/WinUI. Windows x64 is the primary runtime platform. ARM64 can be cross-compiled, but native ART APK execution and the package management backend are not yet implemented.

| Directory | Responsibilities |
| --- | --- |
| `src/windows/service/` | Windows SCM host and per-user session brokers for host sampling and persistent queues; only the SCM supervisor runs as LocalSystem, and neither loads ART |
| `src/windows/client/` | StoneClient launch coordination, application windows, input, and interactions tied to its own windows |
| `src/windows/subsystem/` | Android ELF loading, CPU execution, ABI, HLE, ART, and framework integration |
| `src/windows/host/` | Shared Windows file, audio, graphics, and Shell capabilities |
| `src/windows/ipc/` | Restricted interprocess protocols and state snapshots |
| `src/windows/settings/` | C#/WinUI settings, permissions, and native confirmation UI |
| `src/android/runtime/` | Java entry points, lifecycle handling, and framework adaptation within the image's ART |
| `src/windows/image-tools/` | Standalone C++ system image preparation tool |
| `test/windows/` | Component tests, APK fixtures, and integration drivers |
| `tools/`, `cmake/` | Build, formatting, and packaging scripts, and CMake support |
| `oss/` | Third-party source code, with provenance and licenses preserved |
| `build/`, `bin/` | Generated projects, intermediate files, and runtime artifacts; do not edit manually or commit |

See the [architecture overview](docs/architecture/overview.md) and [native layers](docs/architecture/native-layers.md) for full responsibilities. Within the subsystem, `loader` owns loading and symbol scopes, `cpu/arm64` owns instruction execution, `abi` owns call bridges, `hle` owns host library implementations, `runtime/art` owns the ART VM/JIT/AOT, `framework` owns Android component semantics, and `platform/windows` owns in-process Windows implementations. Android objects and callbacks remain in the Client's ART; host sampling and recoverable queues belong to the Service.

## Workflow and Implementation Conventions

- Check the workspace before making changes and preserve existing edits. Prefer `rg` and restrict searches to source directories to avoid scanning large caches and historical artifacts in `bin/` and `build/`.
- Keep changes focused on the current task. Follow the component's naming, resource management, and error handling conventions. Avoid unrelated refactoring or repository-wide formatting.
- When adding, moving, or deleting source files, update CMake targets, fixture manifests, and related documentation. Do not edit generated Visual Studio projects.
- Follow the [documentation standards](docs/README.md#documentation-standards). Describe current behavior, prerequisites, and limits in the relevant feature page; keep progress reports, debugging transcripts, and one-off validation output in issues or PRs. Retain actionable migration guidance and third-party provenance.
- Project-owned Java packages use `com.trebledot.stone`. When changing Java classes, packages, or native methods, check JNI signatures, reflection, resource paths, and string references used during startup.
- Preserve actual ABI behavior, symbol scopes, ownership, thread state, and failure cleanup. Unimplemented interfaces must explicitly report that they are unsupported or return errors consistent with Android semantics; never fake success.
- IPC changes must update senders, receivers, encoding and decoding, length limits, and behavioral tests together. Changes to identity, signatures, UIDs, path authorization, executable memory, cache formats, or Shell activation require checking all consumers and failure paths.
- ART uses a complete, read-only Android system image selected by the user. Image preparation and execution must not require a Windows JVM. Stone does not provide a complete Android sandbox.
- When modifying third-party components, preserve their provenance, versions, licenses, and build dependencies, and update the [third-party notices](docs/third-party/THIRD_PARTY_NOTICES.txt). Do not commit local APKs, system images, logs, credentials, or private application data.

## Building

Use PowerShell from the repository root. Requirements include Visual Studio 2026 C++ desktop tools, the Windows SDK, CMake with support for the `Visual Studio 18 2026` generator, Windows JDK 21, and the Android SDK's `android.jar` and D8. The settings application also requires the .NET 10 SDK, C# tools, and NuGet restore; APK fixtures require aapt2 and apksigner.

```powershell
cmake --preset x64
cmake --build build/x64 --config Debug -- /restore
```

- For ARM64, use `cmake --preset ARM64` and `cmake --build build/ARM64 --config Debug -- /restore`. MSVC ARM64 tools are also required.
- `-- /restore` restores StoneSettings NuGet dependencies before building, including when Settings is a dependency of a test or packaging target. Native-only builds do not require this option.
- To build only the native components, add `-DSTONE_BUILD_SETTINGS=OFF` when configuring.
- Specify the JDK explicitly with `STONE_JDK_HOME`; otherwise, `JAVA_HOME` takes precedence. Update the CMake cache when switching JDKs. See the build instructions for Android SDK path configuration.
- `build/<preset>/` contains build projects; `bin/<architecture>/<configuration>/` contains runtime artifacts, with the settings application in its `settings/` subdirectory.
- MSI/MSIX packaging must be explicitly enabled with `STONE_PACKAGING=ON`. See [Windows packages](docs/windows/packaging.md) for the rules.

Check exit codes after configuration, compilation, and testing. If a step fails, resolve the error before continuing; do not use old artifacts to validate new code.

## Formatting

C/C++ uses clang-format 22.x and the root `.clang-format`: 4 spaces, 120 columns, Stroustrup braces, and preserved include order. Format only project files involved in the current change, for example:

```powershell
clang-format -i --style=file src/windows/client/ArtLaunch.cpp
```

Use `pwsh -File tools/ClangFormat.ps1 -Check` for a repository-wide read-only check; this script requires a Git workspace. Without `-Check`, it rewrites tracked files in bulk. Third-party and generated directories are excluded from formatting. For Java, C#, and PowerShell, follow the style of neighboring files.

## Testing and Isolation

Component tests are registered only on Windows x64 with `STONE_TESTS=ON`:

```powershell
cmake --preset x64 -DSTONE_TESTS=ON
cmake --build build/x64 --config Debug --target stone-windows-tests -- /restore
cmake --build build/x64 --config Debug --target stone-test-fixtures
```

After a successful build, discover and run tests from the Debug directory:

```powershell
Push-Location bin/x64/Debug
try {
    ctest --test-dir test/ctest/Debug -C Debug -N
    ctest --test-dir test/ctest/Debug -C Debug -L unit --output-on-failure
} finally {
    Pop-Location
}
```

- Select tests using `-R` name filters or `-L` labels as appropriate for the change. The `unit` label does not cover all components, images, or application behavior. For new behavior and regressions, prioritize observable results and failure paths.
- Test executables, input copies, working directories, `TEMP`/`TMP`, application data, caches, and outputs must reside under `bin/<architecture>/Debug/` for the actual target architecture. CTest sets component working and temporary directories; maintain the same isolation when running tests or integration scripts directly.
- When debugging APKs, explicitly specify an isolated `--data-dir` to avoid using everyday data in the default `%LOCALAPPDATA%\Trebledot Productions\Stone\User Data` directory, and set `STONE_LOG_DIRECTORY` to an absolute path under the Debug test tree. Read the [integration drivers](test/windows/integration/) for their arguments and path logic before running them.
- When modifying library bindings, run the corresponding behavioral tests, update `test/windows/subsystem/hle/library-compatibility.json`, and run `check-library-compatibility.ps1` in the same directory. Record the version, ABI, source, and SHA-256 of the original `.so`; keep `compatibility: null` for samples whose identity alone has been checked.
- Documentation-only changes require checking content, links, command paths, and diffs; product builds and APK tests are not required.

## Compatibility and Performance Acceptance

- A successful build, VM creation, visible window, or queued task alone does not prove application compatibility. Validate actual outcomes relevant to the task, such as first-frame content, input, Android callbacks, and background task completion or recovery.
- Record the host architecture, image identity/API/ABI, APK identity, executor, JIT/AOT options, cold preparation or warm cache, whether the Windows JVM was loaded, repeated launches, and normal exit.
- For performance comparisons, hold the executable, image, APK, data, window size, cache, and compilation options constant. Measure image preparation, VM, page content, input response, and drawing separately. Disable sampling, per-method logging, screenshots, and stack capture during formal timing; retain data from multiple runs and records of failures.
- Start troubleshooting with [logging and diagnostics](docs/development/diagnostics.md). See [using Stone](docs/usage.md) for launch arguments and ART AOT/JIT configuration. Conclusions must match the actual evidence.

## Delivery

- Review the final diff and describe the changes, validation results, and actual limitations. Explain any checks that were not run, skipped, or failed.
- In a Git workspace, run `git diff --check`. When preparing a commit, stage only files for the current task and run `git diff --cached --check`. If the current directory is a source copy without Git metadata, state that Git checks cannot be run.
- When the user requests a commit, use `<type>: <short English description>`, such as `fix: correct activity routing after restart` or `docs: update repository guidelines`.
- PR descriptions must cover the problem, final behavior, related issues, actual validation commands and results, and areas not covered. Application changes require sample identities and behavioral evidence; UI changes require appropriate screenshots or recordings.
