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

### WebView image resources

Image preparation examines the immediate package subdirectories under `app` and `priv-app`. It selects directories whose names contain `webview` or `trichrome` (case insensitive) and copies regular files ending in `.apk` directly inside them. Other application APKs and providers stored under different directory names are not copied.

It reads `system`, `product`, and `system_ext` partitions from GPT/super images, including `_a` slots and sparse containers. For `product` and `system_ext`, each partition is resolved independently: a separate partition takes precedence; if it is absent, the corresponding directory embedded in the system filesystem is used instead. A separate partition and its embedded counterpart are not merged.

Copied APKs retain their original bytes and guest paths. Provider libraries under each package's `lib/<architecture>/` directory are preserved too. Library symlinks must resolve inside the selected image's system, product, or system_ext partitions; cycles and escaping links are rejected. The image's optional `libwebviewchromium_plat_support.so` and its dependencies are also prepared. The sorted candidate APK paths are published in `/system/etc/stone-webview-apks.list` after all copies succeed. Successful preparation without candidates produces an empty list.

Some Google Play images contain only small `WebViewGoogle-Stub` and `TrichromeLibrary-Stub` APKs with no WebView engine. Copying or verifying those packages does not supply one. A complete provider, its shared-library dependencies, and native loading/rendering support are still required.

## Execution and compilation

For x86_64 images, Java JIT is not proactively enabled by default; explicit `--enable-jit` enables it for one launch. ARM64 images enable Java JIT automatically. The interpreter path defaults to warmup=100 and optimization=1000; the experimental native block path uses 1000/10000. Explicit parameters take precedence.

`--enable-aot` uses image dex2oat to generate persistent OAT/VDEX for the actual APK; see [APK AOT](art-aot.md). ARM64-to-x64 native blocks and disk caching are independent of ART Java JIT; see [translation caching](arm64-translation-cache.md).

ELF/TLS, calling conventions, JNI, signal recovery, and Windows executable-page adaptation are managed by the [native compatibility layers](../architecture/native-layers.md). Application native class-loader namespaces remain limited; complete static imports do not establish JNI application compatibility.

## WebView provider selection and loading

The in-process `webviewupdate` service reads the image's `config_webview_packages` resource and provides independent copies of its configured/default provider descriptions. At application startup, it checks the prepared candidate paths and uses the image's manifest parser and full-content APK signature verifier. It requires WebView library metadata, DEX code, and an embedded or adjacent native library for the active ABI. Image resource overlays and installed provider updates are not applied.

Automatic selection requires a matching configured package name, any configured signer certificates, and `availableByDefault=true` in the provider configuration. The loader currently requires the Chromium factory class `com.android.webview.chromium.WebViewChromiumFactoryProviderForT`; providers with other factory implementations are unsupported.

On native x64, an eligible provider receives its own reserved image-package UID and a retained image-code context. Its native libraries are hashed and sealed in the running application's code cache, and loaded through ART with a registered provider ClassLoader. Only after native loading succeeds is the provider exposed by current-package queries and `FEATURE_WEBVIEW`. The update response then reports missing shared RELRO, rather than claiming RELRO preparation succeeded. Failed loading leaves no selected provider and no advertised feature. Privileged provider administration and installed updates are rejected. Discovery and loading results are logged under `StoneWebView`.

Native loading alone does not prove page compatibility. Renderer process launch and page rendering remain experimental; Android isolated renderer services are not connected. A debuggable Chromium provider can read `/data/local/tmp/webview-command-line` from the chosen profile's `data/local/tmp/` directory, including Chromium's `--single-process` option for experiments with the in-process renderer. This does not provide Android renderer isolation.

See [WebView provider and rendering checks](../development/build.md#webview-provider-and-rendering-checks) for test prerequisites, invocation, and coverage.

## Compatibility limits

Stone bridges Android and Windows semantics individually; it does not implement a complete Linux kernel or all Android system services. General fork/exec, full remote Binder, complete hardware Surface/HardwareBuffer interoperability, the main WebView flow, and Windows ARM64 native ART remain incomplete.

Image versions, ABI, and private JNI layouts affect compatibility. Documented support applies only to the corresponding path, not every APK, system image, GPU, device, or system CA scenario. See the grouped [Android feature documentation](../README.md#android-features) for specific limits.
