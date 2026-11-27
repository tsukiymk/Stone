# Stone native compatibility layers

The Android compatibility subsystem for Windows is under `src/windows/subsystem/`. It is compiled into the shared `stone-host` target and used by Client and the ART compiler. Service does not load ART.

| Directory | Responsibilities and boundaries |
| --- | --- |
| `loader` | ELF mapping, relocation, TLS, permissions, and symbol metadata in `ElfImage`; Android loading coordination, namespaces, handles, dependencies, and symbol scope |
| `cpu/arm64` | Instruction interpretation, leaf-function translation, block compilation, hotness, local indexes, chaining, budgets, and execution statistics |
| `cpu/arm64/cache` | Persistent artifact formats, validation, source association, admission, capacity, and merge policies; executable blocks in memory are managed by `Arm64BlockJit` |
| `abi` | Android/Windows call descriptions, assembly entry points, typed imports, callbacks, JNI tables, signatures, and class-loader JNI integration; `abi/x64` contains TLS instruction encoding and trampolines |
| `hle/system`, `hle/external` | Host implementations for system and third-party libraries; no arbitrary ARM64 instruction execution |
| `runtime/art` | VM creation and shutdown, ART JNI, Java JIT/AOT, and GS scanning, patch ownership, and publication checks for ART ELF/OAT/JIT methods |
| `framework` | Android framework registration, application and Activity lifecycle, Parcel, audio, Surface, notifications, and process semantics |
| `memory` | Guest heap ownership, free generations, range queries, and mapping permission/code-generation query interfaces |
| `signals` | Android signal layouts, ARM64 signal frames, x64 TLS fault recovery, and Windows exception entry points |
| `platform/windows` | DPAPI, file locks and identity, atomic publication, Windows path boundaries, allocation and executable memory, and native settings confirmation |
| `filesystem` | Android paths, read-only prepared system files, writable data mounts, and shared-storage access checks |

Shared Windows audio, graphics, file, and Shell operations belong to `src/windows/host`. Protocols and snapshots belong to `src/windows/ipc`. Client owns launch coordination; Service owns host sampling and notification queues. `platform/windows` provides in-process platform implementations. Cross-process access must use Service's asynchronous snapshots or restricted IPC.

## Loading, exports, and ABI

`SystemLibraryRegistry` manages module order, dynamic availability, path aliases, and context-aware queries. Symbol selection proceeds through ART's `libandroid` and public `libnativewindow` framework adaptation, special ARM64 exports (including libc data, libm fenv, logging, configuration, and media), module host queries, and ARM64 libc function fallback. Loading coordination registers ARM64 import names with nonzero addresses. Queries must distinguish data symbols, indirect resolvers, and specialized call bridges; they cannot be reduced to a context-free function-address table.

`AndroidSymbolResolver` preserves global scope, breadth-first dependency order, image logging policy, and Bionic unwind fallback. `AndroidLibraries` owns a single process state and recursive loading lock. `AndroidCallBridge`, `LibraryJni`, `LibraryIntegration`, `Arm64LibraryBridge`, and exception entry points cooperate through private `AndroidLibraryState`, without duplicating namespaces, JNI registration records, or code publication generations.

`AndroidLoadedImage` coordinates Android loading. It joins the ARM64 cache after `ElfImage` construction succeeds and unregisters before the base class releases mappings. Failed staged loads use the same destruction path. Generic `ElfImage` neither registers caches nor implicitly participates in persistent caching. Coordination objects are held by their actual type and must not be deleted through the `ElfImage` base, which has no virtual destructor.

## Caching and session completion

`Arm64CacheFormat` handles little-endian encoding, length/flag/relocation validation, and serialization. `Arm64BlockCache` manages static ELF source associations, source instruction byte comparisons, admission hints, memory/disk capacity, dirty state, and merging. `Arm64CacheStorage` owns DPAPI, CPU/host identity hashes, authenticated file handles, file identity comparisons, exclusive writer leases, and atomic replacement. If the filesystem cannot replace files while retaining old handles, all baselines are released before fallback publication.

Executing blocks rechecks code identity, source instruction bytes, relocations, mapping generations, and permissions. Dynamic ART JIT mappings use a zero persistent identity; managed objects and in-process host pointers are not saved. See [ARM64 translation caching](../runtime/arm64-translation-cache.md) for experimental options.

`DumpArm64ExecutionProfile` only reads and displays statistics. Explicit `FinishArm64ExecutionSession` saves dirty artifacts before printing statistics and is called at these exit boundaries:

- After `DestroyJavaVM` succeeds and main-thread TLS cleanup completes.
- Before the production Client calls `ExitProcess`.
- Before an application requests `SIGKILL` for the current process.

Normal Shutdown clears the VM, so destruction does not complete the session again. Repeated completion does not write files if the session has no new artifacts. Single-function execution and diagnostic snapshots do not trigger saving.

Faults/abort, startup failures, external forced termination, and other abnormal exits do not guarantee a save. They may occur inside loading locks, signal recovery, or storage operations, where disk writes are not guaranteed to run.

## Memory and execution boundaries

`Arm64Initializer` manages registers, instruction budgets, signal polling, precise fault recovery, and code/stack ownership during execution. `abi/Arm64ExecutionBridge.h` defines typed loading, JNI, asset, configuration, and window bridges. JNI token/thread tables and signature parsing are provided by `abi`; LP64 signal frame layouts are defined by `signals`. The executor validates JNI references, direct buffers, and signal-return PC/SP because those checks depend on the current instruction, stack, and recovery state.

`GuestMappings` declares mapping permission and code-generation query interfaces. The Android mmap/mprotect APIs, mapping registry, and related Windows operations are implemented in `hle/system/stub-libc-bionic.cpp`. `GuestHeap` manages heap ownership, thread-local range caches, and free generations. Windows aligned allocation is implemented in `platform/windows/GuestAllocation`.

`ArtGs` determines the scannable ranges and patch lifetimes of ART ELF/OAT/JIT code. Generic instruction encoding and trampolines are under `abi/x64`; FS recovery is under `signals`; Windows allocation and publication belong to the platform implementation. ELF mini-debug reading and ART scanning share the corresponding parser implementation.

## Image libraries and host bridges

ART, the framework, and directly usable Android native libraries come from the selected image. Host bridges provide Windows file, thread, networking, signal, handle, window, and device semantics. Matching export names do not justify replacing or removing a bridge: symbol selection must preserve loading scope, ABI, ownership, and the actual calling convention.

The libc bridge initializes the host's narrow environment before exposing it to image code. Native x64 `environ`, `getenv`, `setenv`, and `unsetenv` share the CRT's live storage; ARM64 receives a guest-owned environment snapshot.

The pthread condition-variable bridge records notifications across its internal signal and clock polling intervals. A Windows timed wait can report a timeout when a notification arrives at the same deadline; the bridge checks the notification sequence before waiting again, so an untimed Android wait cannot lose that wakeup.

NDK Bitmap uses image libjnigraphics.so; assets and configuration use image libandroid.so. Stone's software image queues, NativeWindow, and HardwareBuffer cannot be mixed with image-native handles. Unconnected Java Surface/HardwareBuffer conversions fail explicitly.

Windows x64 thread-state recovery accepts only registered FS/GS recovery points and preserves registers, flags, return PC, and code lifetime. ART scanning and the signal layer jointly maintain the boundaries between image code, dynamic JIT code, and fault mappings.
