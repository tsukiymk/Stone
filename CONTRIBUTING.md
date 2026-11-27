# Contributing to Stone

You can contribute to Stone by reporting issues, providing compatibility evidence, improving documentation, reviewing code, and submitting fixes. Before starting, read the [project introduction](README.md) and relevant [feature documentation](docs/README.md).

## Reporting Issues

Before submitting an issue, search existing issues and read the relevant feature documentation. If the same problem has already been reported, add reproduction details or logs to the existing issue. Use 👍 to express interest rather than filing duplicates or posting replies that only say "+1".

Use the [bug report](https://github.com/Trebledot/Stone/issues/new?template=bug_report.yml) to describe a problem. Keep each report focused on one issue where possible, and include:

- The Stone version or commit, how you obtained it, and the target architecture and build configuration if you built it yourself.
- The Windows version, OS build, and host architecture.
- The APK's source, package name, version, and ABI; the system image's source, version/API, and ABI. Include SHA-256 hashes if available to identify the exact samples, and note any fields that do not apply.
- Minimal reproduction steps, expected behavior, actual results, and frequency. If it worked previously, include the last working version or commit.
- Relevant error logs, screenshots, or recordings. For build problems, include configuration commands, toolchain versions, and the first actual error.

See [logging and diagnostics](docs/development/diagnostics.md) for log collection. Distinguish between image preparation, ELF loading, VM initialization, component execution, window creation, and application content readiness. A visible window or successful VM creation alone does not prove APK compatibility.

Remove personal information, credentials, and private application data before sharing logs or screenshots. For samples, prefer lawful public download sources or minimal reproduction fixtures that can be shared publicly.

## Proposing Features or Improvements

Use a [feature request](https://github.com/Trebledot/Stone/issues/new?template=feature_request.yml) to explain a concrete scenario: who encounters the problem, what is missing from the current behavior, the desired outcome, and alternatives you have tried. Relevant Android API documentation, application samples, or measurements can help the discussion.

For substantial features, architectural changes, or changes to protocols or persistent formats, discuss the scope and approach in an issue before implementing them. The proposal should explain component responsibilities, user-visible behavior, failure and recovery paths, compatibility impact, and validation. If you plan to work on an existing issue, leave a comment describing your plan to help coordinate the work.

Spelling corrections, broken links, and small, well-scoped fixes can be submitted directly as PRs with an explanation of the problem. Keep discussions and reviews focused on reproducible behavior and technical evidence, and respect other contributors.

## Setting Up the Development Environment

Stone targets Windows. Its native code uses C++20 and the MSVC toolchain. Prepare the following dependencies:

- Visual Studio 2026 C++ desktop development tools and the Windows SDK. CMake must support the `Visual Studio 18 2026` generator. ARM64 cross-compilation also requires MSVC ARM64 tools.
- An existing vcpkg checkout selected by `VCPKG_ROOT`, plus Git and PowerShell 7. See the [dependency setup](docs/development/build.md#third-party-dependencies).
- Windows JDK 21. Specify it explicitly with `STONE_JDK_HOME`; otherwise, `JAVA_HOME` takes precedence, followed by a search for local installations if it is unset. Update the CMake cache when switching JDKs.
- The Android SDK's `android.jar` and D8, configured through `STONE_ANDROID_SDK`, `STONE_ART_ANDROID_JAR`, and `STONE_ART_D8_JAR`. APK test fixtures also require aapt2 and apksigner from the corresponding build-tools.
- Building StoneSettings requires the .NET 10 SDK, C# tools, and NuGet restore. Set `STONE_BUILD_SETTINGS=OFF` to build only the native components.
- Use clang-format 22.x when modifying C/C++.

From the repository root, select the actual target architecture and build Debug artifacts:

```powershell
$ARCH = 'x64' # or 'ARM64'
cmake --preset $ARCH
cmake --build "build/$ARCH" --config Debug -- /restore
```

Check the exit code after each step and resolve any errors before continuing. `build/<preset>/` contains generated projects and intermediate files; `bin/<architecture>/<configuration>/` contains runtime artifacts. Do not manually edit generated projects.

Windows x64 is the primary execution host. ARM64 can be built, but native ART APK execution and the package management backend are not yet implemented. See the [build instructions](docs/development/build.md) for detailed dependencies, settings application publishing, and other build options.

## Changing Code and Documentation

Before making changes, read the relevant source code, CMake rules, and feature documentation. Check workspace differences and preserve existing edits. Keep each PR focused on one problem and follow the component's naming, indentation, resource management, and error handling conventions.

### Component Responsibilities

| Location | Primary Responsibilities |
| --- | --- |
| `src/windows/service/` | Windows SCM supervisor and user-session brokers for host sampling and recoverable queues; does not load ART |
| `src/windows/client/` | StoneClient launch coordination, application windows, input, and interactions tied to its own windows |
| `src/windows/subsystem/` | Android ELF, CPU execution, ABI, HLE, ART, and framework integration |
| `src/windows/host/`, `src/windows/ipc/` | Shared Windows capabilities and restricted interprocess protocols |
| `src/windows/settings/` | C#/WinUI settings, permissions, and native confirmation UI |
| `src/android/runtime/` | Java entry points, lifecycle handling, and framework adaptation within the image's ART |
| `src/windows/image-tools/` | Standalone C++ system image preparation tool |
| `test/windows/`, `docs/` | Component tests, application fixtures, integration drivers, and feature documentation |

Place native code according to the [native layers](docs/architecture/native-layers.md). Android objects and component callbacks remain in the Client's ART; host state sampling and persistent queues belong to the Service. See the [architecture overview](docs/architecture/overview.md) for the complete process relationships.

### Implementation and Formatting

- Project-owned Java packages use `com.trebledot.stone`.
- When adding, moving, or deleting source files, update CMake targets, fixture manifests, and related documentation. When changing Java classes, packages, or native methods, check JNI signatures, reflection, resource paths, and string references in startup code.
- Unimplemented interfaces must return an explicit unsupported status or errors consistent with Android semantics. Preserve actual ABI behavior, symbol scopes, ownership, and failure cleanup; never hide compatibility gaps by faking success.
- IPC changes must update senders, receivers, encoding and decoding, length limits, and behavioral tests together. Changes to package signatures, UIDs, path authorization, executable memory, cache identity, or Shell activation require checking all consumers and failure paths while preserving identity and permission checks.
- ART uses a complete, read-only system image selected by the user. Image preparation and execution must not require a Windows JVM.
- When adding or modifying third-party components, preserve their provenance, versions, licenses, and build dependencies, and update the [third-party notices](docs/third-party/THIRD_PARTY_NOTICES.txt). Do not commit local APKs, system images, or generated artifacts.

C/C++ follows [.clang-format](.clang-format), preserving include order. Format only project files involved in the change, excluding third-party and generated directories. For example, replace the following path with the file you actually changed:

```powershell
clang-format -i --style=file src/windows/client/ArtLaunch.cpp
```

Use `pwsh -File tools/ClangFormat.ps1 -Check` for a repository-wide read-only check. Without `-Check`, it formats all tracked C/C++ files; use the single-file command above for ordinary feature changes.

Documentation should describe actual available behavior, prerequisites, and unimplemented areas. Check commands and paths against the current source and build rules. AI tools may assist, but contributors must understand and verify all changes and be able to explain the implementation, validation results, and limitations.

## Validating Changes

### Component Tests

Component tests are currently registered only on Windows x64. Enable and build them from the repository root:

```powershell
cmake --preset x64 -DSTONE_TESTS=ON
cmake --build build/x64 --config Debug --target stone-windows-tests -- /restore
cmake --build build/x64 --config Debug --target stone-test-fixtures
```

After a successful build, discover and run the relevant tests from the Debug directory:

```powershell
Push-Location bin/x64/Debug
try {
    ctest --test-dir test/ctest/Debug -C Debug -N
    ctest --test-dir test/ctest/Debug -C Debug -L unit --output-on-failure
} finally {
    Pop-Location
}
```

CTest component rules set the working directory and TEMP/TMP. Select tests using `-R` name filters or `-L` labels as appropriate for the change; `unit` does not cover all components, images, or application behavior. Behavioral tests should verify observable results, failure paths, or regressions. Do not use old artifacts to validate new code after a build or link failure.

### Application Compatibility and Performance

See [test/windows/integration/](test/windows/integration/) for ART and settings application integration drivers. Read script arguments and path logic before running them, and prepare current Debug artifacts and inputs according to the isolation requirements above. Do not assume the scripts cover the full functionality.

- Record the host architecture, image identity/API/ABI, APK identity, executor and compilation options, cold preparation or warm cache, whether the Windows JVM was loaded, repeated launches, and normal exit.
- Validate actual application outcomes relevant to the change, such as the first frame, input, Android callbacks, or background task completion or recovery. A successful build, passing component tests, or queued task cannot replace application acceptance checks.
- When modifying library bindings, run the corresponding behavioral tests, update the [library compatibility samples](test/windows/subsystem/hle/library-compatibility.json), and check them with the [catalog validation tool](test/windows/subsystem/hle/check-library-compatibility.ps1). The version, ABI, source, and SHA-256 of the original `.so` actually tested must be traceable; retain `compatibility: null` for samples checked only for identity.
- For performance comparisons, hold the executable, image, APK, data, window size, cache, and compilation options constant. Distinguish image preparation, VM, page content, input response, and drawing. Disable sampling, per-method logging, screenshots, and stack capture during formal timing; retain data from multiple runs and records of failures.

Documentation-only changes may be validated by checking content, links, command paths, and diffs, without running product builds or APKs. Explain any checks that were not run, skipped, or failed.

## Submitting a Pull Request

Submit PRs from your own branch. Before committing, stage only files for the current task, review the staged diff, and run:

```powershell
git diff --check
git diff --cached --check
```

Use the commit message format `<type>: <short English description>`, choosing a type prefix appropriate to the change, for example:

- `fix: correct activity routing after restart`: a bug fix.
- `feat: add application notification settings`: a new feature.
- `docs: update contribution guidelines`: a documentation change.

Do not include build artifacts, images, local APKs, logs, or unrelated formatting in the commit.

PR descriptions should make the changes understandable to reviewers who did not participate in the discussion. Include:

- **Problem and related issues**: triggering conditions, previous behavior, and impact.
- **Changes and results**: the final approach and user-visible behavior; explain design tradeoffs and migration impact where needed.
- **Validation**: the actual configuration, build, and test commands run, and their results. Application changes require sample identities, options, and behavioral evidence; UI changes require appropriate screenshots or recordings.
- **Limitations**: cases not yet covered, checks not performed, and the reasons.

Before submitting, confirm:

- [ ] Changes respect component responsibilities, and affected callers, JNI/ABI, IPC, and persistent formats have been checked together.
- [ ] Relevant targets build successfully, and behavioral checks use current Debug artifacts with the required test isolation; inapplicable or blocked items are explained.
- [ ] New behavior and regressions have appropriate validation, and compatibility and performance conclusions match the actual evidence.
- [ ] Build rules, documentation, and third-party notices have been updated as needed.
- [ ] The diff contains no unrelated changes, sensitive information, or test artifacts, and diff checks pass.

Respond to questions during review, and update the PR description and validation results when the approach or scope changes. Thank you for helping improve Stone.
