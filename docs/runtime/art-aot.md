# Experimental ART APK AOT

On Windows x64, `--enable-aot` runs the actual `dex2oat64` from the selected x86_64 or experimental ARM64 image to generate `speed` OAT/VDEX before creating the APK PathClassLoader. Compilation is not proactively enabled by default. It does not change the APK's native ABI or select the ARM64-to-x64 native block disk cache.

## Compilation and loading

The image preparer extracts the compiler and ELF dependencies from the ART APEX. The standalone `StoneArtCompiler.exe` does not load a Windows JVM and locates the compiler entry point from symbols in the same file or mini debug data. Native x86_64 execution and ARM64 translation use their respective image compilers and ART implementations. They do not execute the general Bionic process entry point or provide shell/exec.

The production entry point resolves the currently registered APK and passes its actual `applicationInfo.sourceDir` to the compiler host. Compilation uses the complete boot JAR set and Stone runtime with `--class-loader-context=PCL[]`, `--compiler-filter=speed`, `--generate-debug-info`, and `--copy-dex-files=false`. DEX remains in the original APK. ART runs without a boot image and does not generate boot image AOT.

The ARM64 compiler uses checked guest memory for arguments, options, and output. Its termination bridge is restricted to the compiler and does not expose process exit to arbitrary libraries.

## Persistent cache

Artifacts and locks are separated by ISA under `<data-dir>/data/dalvik-cache/x86_64` or `arm64`. Cache identity includes the ISA and SHA-256 hashes of the APK, host and image compiler, libart, all boot JARs, and OAT/VDEX artifacts.

Cross-process locks prevent concurrent publication. Compilation finishes in a separate staging directory before replacing artifacts, and identity markers are published last. Compilation failure explicitly terminates the request and retains compiler output. A Windows Job cleans up compiler child processes when the parent Client exits.

ART checks OAT versions, DEX checksums, boot dependencies, and class-loader context. Native loading preserves namespaces, code metadata, and executable-page adaptation. Missing metadata causes unadapted executable mappings to be rejected while allowing safe DEX fallback. OAT system scope does not expose APK-owned JNI namespaces.

Valid OAT can be loaded by later processes without the flag. The flag controls proactive preparation and cache reuse checks. Background wakeups do not inherit explicit command-line arguments; defaults are read from `runtime-features.properties` in the data root. See [runtime configuration](../usage.md#runtime-configuration-by-data-root) for the format.

## Coexistence with JIT

`--enable-aot --enable-jit` allows persistent OAT methods to execute as AOT while the image JIT publishes hot methods from uncompiled DEX. JIT code remains in the current process and is not automatically written back to OAT. ARM64 images enable Java JIT automatically, independently of native block caching.

Preparation, compilation, and reuse do not require a Windows JDK/JVM, emulator, or ADB. Compilation can increase foreground waiting time.

## Scope

Compilation covers the actual application APK. It does not include framework/boot image precompilation, complex split-APK class-loader contexts, profile-guided compilation, background maintenance, or capacity reclamation. The Windows ARM64 native ART backend is not implemented. Artifacts or successful compilation do not establish improved application performance.

See [AOSP ART configuration](https://source.android.com/docs/core/runtime/configure) for artifact and filter semantics.
