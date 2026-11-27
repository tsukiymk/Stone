#include "hle/SystemLibraryRegistry.h"
#include "abi/AndroidLibraryImports.h"
#include "loader/AndroidSystemElf.h"
#include "framework/ArtFramework.h"
#include "hle/system/stub-libmedia_jni.h"
#include "hle/system/stub-libmediandk-format.h"
#include "hle/system/stub-libnativeloader.h"
#include "hle/system/stub-libvulkan.h"
#include "hle/system/stub-libz.h"
#include "hle/system/stub-libc.h"
#include <algorithm>

namespace stone::windows {
bool ArtNativeLoaderAvailable();
const std::array<SystemLibrary, SystemLibraryCount> systemLibraries{{
    {"libdl.so", Libdl},
    {"libc.so",
     [](std::string_view name) {
         return Libc(name);
     }},
    {"libdl_android.so", LibdlAndroid},
    {"libnativeloader.so", Libnativeloader, ArtNativeLoaderAvailable},
    {"libmedia_jni.so", AndroidMediaJniImport, ArtMediaJniAvailable},
    {"libm.so", Libm},
    {"libandroid.so", Libandroid},
    {"libGLESv1_CM.so",
     [](std::string_view name) {
         return Libgles(name, false);
     }},
    {"libGLESv2.so",
     [](std::string_view name) {
         return Libgles(name, false);
     }},
    {"libGLESv3.so",
     [](std::string_view name) {
         return Libgles(name, false);
     }},
    {"libEGL.so",
     [](std::string_view name) {
         return Libegl(name, false);
     }},
    {"liblog.so", AndroidLogImport},
    {"libstdc++.so", Libstdcxx},
    {"libOpenSLES.so", Libopensles},
    {"libaaudio.so",
     [](std::string_view name) {
         return AndroidAudioImport(name);
     }},
    {"libmediandk.so", AndroidMediaImport},
    {"libOpenMAXAL.so",
     +[](std::string_view) -> uintptr_t {
         return 0;
     }},
    {"libvulkan.so", Libvulkan, WindowsArm64VulkanAvailable},
    {"libz.so", Libz, NativeImageZlibLoaded},
    {"libnativewindow.so",
     [](std::string_view name) -> uintptr_t {
         // Both public NDK libraries must share the same opaque host handles.
         // Loading image libnativewindow would enter its unavailable gralloc HAL.
         return name.starts_with("AHardwareBuffer_") || name.starts_with("ANativeWindow_") ? Libandroid(name) : 0;
     }},
}};

size_t SystemLibraryIndex(std::string_view name)
{
    const auto found = std::ranges::find(systemLibraries, name, &SystemLibrary::name);
    if (found != std::end(systemLibraries) && found->available && !found->available())
        return SystemLibraryCount;
    return found - std::begin(systemLibraries);
}

size_t SystemLibraryPathIndex(std::string_view path)
{
    constexpr std::string_view system = "/system/lib64/";
    constexpr std::string_view bionic = "/apex/com.android.runtime/lib64/bionic/";
    if (path.starts_with(system))
        return SystemLibraryIndex(path.substr(system.size()));
    if (path.starts_with(bionic)) {
        const auto name = path.substr(bionic.size());
        if (name == "libc.so" || name == "libm.so" || name == "libdl.so")
            return SystemLibraryIndex(name);
    }
    return SystemLibraryCount;
}

uintptr_t LookupHleSystemSymbol(const SystemLibrary& library, std::string_view symbol, SystemSymbolContext context)
{
    uintptr_t address = 0;
    if (library.name == "libnativewindow.so" && !symbol.starts_with("AHardwareBuffer_") &&
        !symbol.starts_with("ANativeWindow_"))
        return 0;
    if (context.artRuntime && (library.name == "libandroid.so" || library.name == "libnativewindow.so"))
        address = ArtFrameworkImport(symbol);
    // Keep the selected image's assets and ART-specific JNI adapters ahead of
    // the standalone ARM64 configuration/legacy host providers.
    if (context.translate && !address) {
        if (library.name == "libm.so")
            address = Arm64FenvImport(symbol);
        if (library.name == "libc.so")
            address = Arm64LibcDataImport(symbol);
        if (library.name == "liblog.so")
            address = Arm64LogImport(symbol);
        if (library.name == "libandroid.so")
            address = Arm64ConfigurationImport(symbol);
        if (library.name == "libmediandk.so")
            address = Arm64MediaImport(symbol);
    }
    if (!address)
        address = library.lookup(symbol);
    if (!address && context.translate && library.name == "libc.so")
        address = Arm64LibcFunctionImport(symbol);
    return address;
}

}
