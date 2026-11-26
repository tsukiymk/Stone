# ART and system images

By default, Stone directly executes an APK's original DEX using ART, the AOSP framework, and system libraries from the user-selected image. The application process does not depend on a Windows JVM, emulator, virtual machine, or ADB.

## Host and image architectures

| Windows host | Android image | Execution path |
| --- | --- | --- |
| x64 | x86_64 | Native x64 ART and ELF adaptation |
| x64 | arm64-v8a | Experimental ARM64 translation with ART Java JIT enabled automatically |
| ARM64 | Any | Native ART backend not implemented |

The backend is selected from the ELF architecture of image libart.so. The Java JIT option does not select an APK's native ABI. A Windows ARM64 cross-build does not establish that APK execution works.

## Image preparation and resources

StoneSettings can download the official stable AOSP API 37.0 x86_64 system image from its welcome and System pages. Downloads default to `%LOCALAPPDATA%\Trebledot Productions\Stone\System Images`; see [settings downloads](../windows/settings.md#downloading-an-android-image) for licensing, verification, and selection. Android data and preparation caches default to the sibling `User Data` directory, with application logs under `Logs`.

Supported formats include ext4 and the EROFS layouts used by the corresponding Android images. The standalone C++ `StoneImageTool.exe` prepares boot JARs, ART and native dependencies, framework-res.apk, fonts, configuration, and system CAs. Cache identity includes the image and preparation tool; caches cannot be shared across images or architectures.

The system image is a read-only resource source, while application data uses separate writable mounts. Cold preparation and execution require no Windows JDK/JVM or installed .NET runtime. On-demand access to all resources in a complete image is not fully connected. Fonts and Canvas use image resources and libhwui.

## Execution and compilation

For x86_64 images, Java JIT is not proactively enabled by default; explicit `--enable-jit` enables it for one launch. ARM64 images enable Java JIT automatically. The interpreter path defaults to warmup=100 and optimization=1000; the experimental native block path uses 1000/10000. Explicit parameters take precedence.

`--enable-aot` uses image dex2oat to generate persistent OAT/VDEX for the actual APK; see [APK AOT](art-aot.md). ARM64-to-x64 native blocks and disk caching are independent of ART Java JIT; see [translation caching](arm64-translation-cache.md).

ELF/TLS, calling conventions, JNI, signal recovery, and Windows executable-page adaptation are managed by the [native compatibility layers](../architecture/native-layers.md). Application native class-loader namespaces remain limited; complete static imports do not establish JNI application compatibility.

## Compatibility limits

Stone bridges Android and Windows semantics individually; it does not implement a complete Linux kernel or all Android system services. General fork/exec, full remote Binder, complete hardware Surface/HardwareBuffer interoperability, the main WebView flow, and Windows ARM64 native ART remain incomplete.

Image versions, ABI, and private JNI layouts affect compatibility. Documented support applies only to the corresponding path, not every APK, system image, GPU, device, or system CA scenario. See the grouped [Android feature documentation](../README.md#android-features) for specific limits.
