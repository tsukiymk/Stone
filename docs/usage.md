# Using Stone

Stone is an Android compatibility layer for Windows. It runs APKs using ART, the AOSP framework, and native libraries from a selected Android system image, and presents them in a Win32 window.

## Requirements

Windows x64 supports x86_64 images and experimental translation of arm64-v8a images. Windows ARM64 build artifacts can be generated, but the native ART APK execution backend is not implemented. A system image must contain the complete runtime; a standalone framework-res.apk is not a substitute.

Image preparation uses the bundled standalone native `StoneImageTool.exe`. Neither preparation nor APK execution requires a Windows JDK, JVM, or installed .NET runtime.

Release packages include the Windows ANGLE EGL/GLES runtime and its D3DCompiler dependency beside `StoneClient.exe`. No Android SDK or separate ANGLE installation is required. See [graphics runtime loading](android/graphics-input.md#software-drawing-and-bitmaps) for DLL requirements and failure behavior.

StoneSettings can [download a stable AOSP API 37 x86_64 image](windows/settings.md#downloading-an-android-image) to `%LOCALAPPDATA%\Trebledot Productions\Stone\System Images`. After download, choose **Use this image** to validate and save it for the selected Android data root.

## Launching and data

Run from the Stone program directory, replacing the example paths with actual files:

```powershell
.\StoneService.exe --apk 'C:\Apps\sample.apk' --system-image 'C:\Images\system.img'
```

Installed StoneService is an automatic Windows service supervising a singleton broker in each signed-in user's session. Each broker creates a StoneClient for each APK with that user's permissions. Source builds start a user broker on demand without requiring service installation. For debugging, run `StoneClient.exe` directly. If `--system-image` is omitted, Stone first reads the selection saved in `system-image.path` under the data root, then falls back to `system.img` beside the executable if no selection is saved. `--data-dir` selects the data root, which defaults to `%LOCALAPPDATA%\Trebledot Productions\Stone\User Data`.

Use elevated PowerShell `Stop-Service StoneService`, `Start-Service StoneService`, or `Restart-Service StoneService` to manage an installed service. `StoneService.exe --status` reports the current session's broker and Clients; `--stop-service` requests that session's broker to exit and the SCM supervisor recreates it while running. `--serve` is the explicit development broker mode; `--service` (or no arguments) enters the SCM dispatcher and fails when launched outside SCM. An installed build reports an unavailable/stopped Windows service rather than silently starting an unmanaged broker.

A data root holds application registrations, private data, image caches, and background state. Registered applications can also be activated through `stone://<packageName>`. Closing the window destroys the foreground Activity; the application process may remain alive while components or jobs are running.

Foreground Client launches show a white startup window with a centered icon, an animated progress bar, and startup status while the system image and ART are initialized. The fixed window is 608 × 342 logical pixels, scales with the selected monitor's DPI, and is centered in the work area of the monitor containing the pointer. Its title bar has only a close button. The initial title uses the APK filename or package name and the Stone icon; the application's resolved title and icon replace them when available. The screen closes after the first software application frame is painted. Closing the startup window cancels the Client process. Background launches and package-management commands do not show this screen.

Reported foreground launch failures display the error and the current process log path (including a configured `STONE_LOG_DIRECTORY` override). If logging could not be initialized, the dialog says that no startup log is available. Native crashes and forced process termination cannot reliably display this dialog. See [diagnostics](development/diagnostics.md) for log details.

## Execution options

| Option | Effect |
| --- | --- |
| `--enable-jit` | Enable experimental ART Java JIT for an explicit launch; ARM64 images enable Java JIT automatically |
| `--enable-aot` | Use the image compiler to prepare persistent OAT/VDEX for the actual APK; can be combined with JIT |
| `--background` | Create only the ART Application and background components, without automatically creating the launcher Activity |
| `--help` | Show the arguments supported by the current program |

Command-line AOT/JIT options apply only to that launch and are not written back to configuration. ARM64 native block caching has separate experimental options; see [ARM64 translation caching](runtime/arm64-translation-cache.md).

## Runtime configuration by data root

At startup, Client reads `<data-dir>/runtime-features.properties` for ART AOT/JIT defaults. If the file is absent, it uses ART without proactively enabling AOT/JIT. ARM64 images still enable Java JIT automatically. StoneSettings does not expose these options or rewrite this file.

To save defaults, exit applications using that data root, then create or edit the file. For example:

```properties
format=1
aot=false
jit=false
```

`aot` and `jit` accept only `true` or `false`. `format=1`, `aot`, and `jit` are required, and each entry may appear only once. Use UTF-8 without a BOM, with no blank lines, comments, or extra fields. The file must not exceed 256 bytes; invalid formatting causes startup to fail.

Existing files may include the optional `executor=art` entry. Remove `executor=java-runtime` if present; it causes startup to fail. ART is the only supported executor.

ART command-line flags can enable the corresponding feature for one launch; a configured `false` does not override an explicit flag. Saved defaults apply to subsequent launches and ART background wakeups using that data root.

## Limits

APKs run with the permissions of the Windows user who starts Stone. Stone does not provide a complete Android sandbox. Run only applications from trusted sources. Android APIs, the NDK, system services, and device capabilities have compatibility limits; successful installation or a visible window does not establish full functionality.

See [process architecture](architecture/overview.md) for windows, background scheduling, and recovery, [Stone settings](windows/settings.md) for configuration interfaces, and [diagnostics](development/diagnostics.md) for logs and troubleshooting.
