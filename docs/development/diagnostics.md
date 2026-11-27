# Logging and diagnostics

Diagnostics identify process and application state; they do not establish functional compatibility or performance. When reporting a problem, distinguish the host architecture, image API/ABI, executor, APK identity, actual arguments, and failure stage.

## Client logs

Process output uses `%LOCALAPPDATA%\Trebledot Productions\Stone\Logs` as its root, with a subdirectory named after the executable (without `.exe`):

- Client applications: `StoneClient/Applications/<packageName>-<timestamp>.log`.
- Client operations: `StoneClient/Operations/<operation>-<timestamp>.log`.
- User-session Service broker: `StoneService/<timestamp>.log`.
- Other processes using the process logger use their own executable name and `<timestamp>.log`.

The timestamp is local startup time in `yyyyMMdd-HHmmss-fffffff` format (seven fractional second digits). Filenames do not include process or thread IDs; records retain the process ID as `[p-<pid>]` for diagnostics. A filename collision selects a new timestamp without overwriting the existing file. Each process retains stdout and stderr in one file. Files are created only when a nonempty record is written, so silent runs leave no empty logs. Parent directories are created as needed without requiring a writable installation directory.

For `--package`, Client attaches the requested package name before application launch. For `--apk`, it initially logs as `Operations/launch-apk-...`, then moves that same file into `Applications` with the actual package name when Android resolves it, retaining all startup records. Failures before package resolution, including duplicate APK launches, retain the named operation log.

Package-management logs use operation names `package-list`, `package-install`, and `package-uninstall`. Other operation names are `startup` (before argument handling), `arguments` (argument validation), `help`, `isolated-worker`, and `launch-package` (before a valid package name can be attached). These names identify the command even when no application package is available. Existing logs from earlier layouts are retained in place and are not automatically migrated.

`STONE_LOG_DIRECTORY` can select an absolute log root for isolated diagnostic runs; executable subdirectories are still appended. CTest sets it under the Debug test tree. This override applies to both process logs and Android journals, independently of `--data-dir`. See [Windows packages](../windows/packaging.md) for installation layouts.

When troubleshooting, distinguish image preparation, ELF loading, VM initialization, application components, and window creation. A timeout or process exit does not mean the application became ready.

Missing libraries, unresolved imports, and unsupported ARM64 host calls are different errors. A library being present, a symbol resolving, or a VM being created does not establish correct API semantics or page content. See [native compatibility layers](../architecture/native-layers.md) for loading boundaries.

## Windows service diagnostics

The installed SCM supervisor reports failures to the Windows Application event log with source `StoneService`. It does not create process text logs as LocalSystem. User-session brokers retain the ordinary process log behavior. Check `Get-Service StoneService` and the current user's `StoneService.exe --status` separately: a running SCM host does not prove that a session broker or an Android application is ready. Source-build `--serve` brokers do not require SCM registration.

## Android text logs

Client asynchronously submits accepted Android text logs to Service, isolated by actual package name and data root. Service stores complete lines with a size limit in `Logs/Android/<data-identity>/<package>.log` under the default Stone user directory and deletes the application's journal when it is uninstalled. The data identity is a hash of the normalized application data path, keeping journals from different `--data-dir` roots separate. Existing journals in the former `data/system/stone/logcat` directory are not moved automatically.

A restricted logcat dump is supported for the application's own UID. Exports use immutable paginated snapshots. A read interrupted by Service replacement fails explicitly rather than mixing old and new snapshots. Abrupt Client termination can lose unsubmitted batches. This is not device-wide logd and does not provide general shell/exec, access to other UIDs, continuous streaming, or arbitrary command arguments.

## Software frames and thread information

`STONE_ART_DIAGNOSTICS=1` enables output under `data/stone-diagnostics/<package>/` in the application data root. `frame.png` is a software frame; `state.txt` contains the current Activity, configuration, and ordinary View state, including View and window focus. For Compose, use the actual image as the reference.

In the same directory, `capture.request` requests the next frame and `threads.request` requests a Java thread snapshot. `FLAG_SECURE` masks protected content; diagnostics cannot bypass [capture protection](../android/graphics-input.md#capture-protection).

When frames stop advancing for at least one second, the watchdog appends main-thread samples to `stalls.txt`, up to 120 samples per window loop. Its first sample also replaces `stalled-threads.txt` with a Java thread snapshot, so collecting it does not depend on the stalled main loop servicing a request. Files can survive later launches; check their modification times and sample uptimes before attributing a stall to the current run.

## Performance and experimental options

Screenshots, thread stack collection, per-method logging, and native execution statistics perturb execution. For comparisons, fix the executable, image, APK, application data, window size, cache state, and actual compiler options. Keep troubleshooting diagnostics separate from formal timing runs.

Image preparation, VM initialization, page content, input response, and per-frame drawing are different metrics. JIT publication, cache hits, fewer block dispatches, or sample counts do not directly establish application speedups or CPU-time proportions. See [ART](../runtime/art.md) and [translation caching](../runtime/arm64-translation-cache.md) for experimental configuration.
