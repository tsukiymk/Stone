# Architecture and source responsibilities

Stone separates application execution, Windows host functionality, and cross-process coordination. Android objects and lifecycle state remain in the application's ART instance. Host state sampling and recoverable queues are managed by StoneService under the current user.

## Process responsibilities

| Process | Responsibilities |
| --- | --- |
| StoneService | Windows SCM supervisor and user-session singleton brokers; brokers own Client registration, host state sampling, job and notification queues, and persistent launch profiles; does not load ART |
| StoneClient | APK and image coordination, ART, application components, Win32 windows, input, and drawing |
| StoneArtCompiler | Independently execute the image dex2oat compiler to prepare AOT artifacts; does not load a Windows JVM |
| StoneSettings | WinUI settings and confirmation interfaces; does not perform Android installation transactions or execute components |

Each Client hosts the execution context of one APK. Service uses a restricted named-pipe protocol to verify the same user, Windows session, installation executable, actual process, and application data identity. It does not directly access Java objects in another process.

## Host state and recovery

Service samples power, networking, language, and theme in the background. Client reads asynchronous snapshots, so the Android UI thread does not wait directly for host polling. Android Binder objects, listeners, thresholds, and callbacks remain managed by the corresponding image and ART.

Closing a window does not force-stop the application. Client continues running while its Application, Service, JobService, or power requests remain active, and asks Service to authorize idle exit when no work remains. Persistent jobs and supported notification actions can restore Client within the current login session after rechecking data, image, code, and revocable launch identifiers.

The installed `StoneService` SCM service starts automatically under LocalSystem. This host enumerates signed-in Windows sessions and uses their user tokens and environments to launch `StoneService.exe --serve` brokers on `winsta0\default`. It supervises brokers with a Windows job object; Clients launched by brokers leave that job. Brokers retain the existing user/session/installation pipe identities, user data paths, Shell access, notifications, and clipboard handling. The supervisor accepts no application IPC and never loads ART or processes APK paths as LocalSystem.

On SCM stop or shutdown, protected events ask brokers to stop Clients and drain completions before exiting. Shutdown is bounded; unresponsive brokers are terminated after the grace period. Closing or crashing the supervisor also terminates its brokers through the job object. Brokers are recreated while their login sessions exist, including disconnected sessions. No Android components run before login, and complete persistent recovery across boot or a new login remains unimplemented. Source builds retain an on-demand user broker when no SCM registration points to that build.

## Source directories

| Directory | Responsibilities |
| --- | --- |
| `src/android/runtime` | Project Java entry points, lifecycle, and framework adaptation inside image ART |
| `src/windows/image-tools` | Standalone C++ tool for reading and preparing system images |
| `src/windows/subsystem` | ELF, execution backends, ABI, ART, Android native semantics, and guest memory |
| `src/windows/host` | Shared Windows audio, graphics, files, handles, Shell, and process logging |
| `src/windows/ipc` | Restricted protocols, snapshots, launch identity, and Client control events |
| `src/windows/service` | SCM supervision, user-session coordination, host sampling, and persistent queues |
| `src/windows/client` | Launch coordination, windows, input, and the Service client |
| `src/windows/settings` | Windows settings application |
| `oss` | Third-party code and build inputs |
| `test/windows` | Component tests, fixtures, and integration driver sources |

`src/windows/subsystem` is compiled into the `stone-host` target for use by Client and the ART compiler; it is not a separate process. See [native compatibility layers](native-layers.md) for detailed native boundaries and [building from source](../development/build.md) for artifacts and dependencies.
