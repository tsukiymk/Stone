#include "abi/AndroidLibraryImports.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "stub-libandroid.h"
#include "stub-libandroid-fonts.h"
#include "stub-libandroid-hardware-buffer.h"
#include "stub-libandroid-input.h"
#include "stub-libc.h"
#include "stub-libmediandk-image-reader.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace stone::windows {
// Native windows here belong only to Stone's software image queues.
constexpr int AndroidInvalidArgument = -22;

void AcquireWindow(NativeWindow* window)
{
    ++window->references;
}

void ReleaseWindow(NativeWindow* window)
{
    if (window && --window->references == 0)
        delete window;
}

template <bool Width> int WindowDimension(NativeWindow* window)
{
    if (!window || !window->imageQueue)
        return AndroidInvalidArgument;
    return Width ? AndroidImageQueueWidth(*window->imageQueue) : AndroidImageQueueHeight(*window->imageQueue);
}

int WindowFormat(NativeWindow* window)
{
    return window && window->imageQueue ? AndroidImageQueueFormat(*window->imageQueue) : AndroidInvalidArgument;
}

int SetWindowGeometry(NativeWindow* window, int width, int height, int format)
{
    if (!window || !window->imageQueue || width < 0 || height < 0 || (width == 0) != (height == 0))
        return AndroidInvalidArgument;
    if (width && (width != AndroidImageQueueWidth(*window->imageQueue) ||
                  height != AndroidImageQueueHeight(*window->imageQueue)))
        return AndroidInvalidArgument;
    return format == 0 || format == WindowFormat(window) ? 0 : AndroidInvalidArgument;
}

int LockWindow(NativeWindow* window, AndroidNativeWindowBuffer* buffer, AndroidWindowRect* dirty)
{
    if (!window || !window->imageQueue || !buffer)
        return AndroidInvalidArgument;
    return LockAndroidImageQueue(*window->imageQueue, *buffer, dirty);
}

int UnlockAndPostWindow(NativeWindow* window)
{
    if (!window || !window->imageQueue)
        return AndroidInvalidArgument;
    return PostAndroidImageQueue(*window->imageQueue);
}

uintptr_t Libandroid(std::string_view name)
{
    if (name == "ASharedMemory_create")
        return Import(name, &CreateAndroidSharedMemory);
    if (name == "ASharedMemory_getSize")
        return Import(name, &AndroidSharedMemorySize);
    if (name == "ASharedMemory_setProt")
        return Import(name, &SetAndroidSharedMemoryProtection);
    if (name.starts_with("AFont_") || name.starts_with("ASystemFontIterator_"))
        return AndroidFontImport(name);
    if (name.starts_with("AHardwareBuffer_"))
        return AndroidHardwareBufferImport(name);
    if (name.starts_with("AMotionEvent_") || name.starts_with("AInputEvent_"))
        return AndroidInputImport(name);
    if (name == "ANativeWindow_acquire")
        return Import(name, &AcquireWindow);
    if (name == "ANativeWindow_release")
        return Import(name, &ReleaseWindow);
    if (name == "ANativeWindow_getWidth")
        return Import(name, &WindowDimension<true>);
    if (name == "ANativeWindow_getHeight")
        return Import(name, &WindowDimension<false>);
    if (name == "ANativeWindow_getFormat")
        return Import(name, &WindowFormat);
    if (name == "ANativeWindow_setBuffersGeometry")
        return Import(name, &SetWindowGeometry);
    if (name == "ANativeWindow_lock")
        return Import(name, &LockWindow);
    if (name == "ANativeWindow_unlockAndPost")
        return Import(name, &UnlockAndPostWindow);
    if (name.starts_with("ALooper_"))
        return AndroidLooperImport(name);
    return AndroidSensorImport(name, false);
}

uintptr_t AndroidSensorImport(std::string_view name, bool hostAbi)
{
    const auto bind = [name, hostAbi](auto function) {
        return hostAbi ? reinterpret_cast<uintptr_t>(function) : Import(name, function);
    };
    // The Windows host has no NDK sensor devices. Report an empty inventory
    // using the NDK's normal optional-device results.
    static int sensorManager;
    if (name == "ASensorManager_getInstance")
        return bind(+[]() -> void* {
            return &sensorManager;
        });
    if (name == "ASensorManager_getDefaultSensor")
        return bind(+[](void*, int) -> void* {
            return nullptr;
        });
    if (name == "ASensorManager_getSensorList")
        return bind(+[](void* manager, const void*** list) {
            if (!manager || !list)
                return AndroidInvalidArgument;
            *list = nullptr;
            return 0;
        });
    if (name == "ASensorManager_createEventQueue")
        return bind(+[](void*, void*, int, void*, void*) -> void* {
            return nullptr;
        });
    if (name == "ASensorManager_destroyEventQueue" || name == "ASensorEventQueue_disableSensor" ||
        name == "ASensorEventQueue_enableSensor")
        return bind(+[](void*, const void*) {
            return AndroidInvalidArgument;
        });
    if (name == "ASensorEventQueue_getEvents")
        return bind(+[](void*, void*, size_t) -> intptr_t {
            return AndroidInvalidArgument;
        });
    if (name == "ASensorEventQueue_setEventRate")
        return bind(+[](void*, const void*, int) {
            return AndroidInvalidArgument;
        });
    if (name == "ASensorEventQueue_hasEvents")
        return bind(+[](void*) {
            return AndroidInvalidArgument;
        });
    if (name == "ASensor_getType" || name == "ASensor_getMinDelay")
        return bind(+[](const void*) {
            return AndroidInvalidArgument;
        });
    if (name == "ASensor_getName" || name == "ASensor_getVendor")
        return bind(+[](const void*) -> const char* {
            return nullptr;
        });
    if (name == "ASensor_getResolution")
        return bind(+[](const void*) {
            return 0.0f;
        });
    return 0;
}

int WindowWidth(NativeWindow* window)
{
    return WindowDimension<true>(window);
}

int WindowHeight(NativeWindow* window)
{
    return WindowDimension<false>(window);
}

void* CreateAndroidImageWindow(const std::shared_ptr<AndroidImageQueue>& queue)
{
    auto* window = new (std::nothrow) NativeWindow;
    if (window)
        window->imageQueue = queue;
    return window;
}

void ReleaseAndroidImageWindow(void* window)
{
    ReleaseWindow(static_cast<NativeWindow*>(window));
}

Arm64WindowBridge MakeArm64WindowBridge()
{
    return {nullptr, // ART Surface conversion needs an ARM64 JNI bridge.
            nullptr,
            +[](void* window) {
                AcquireWindow(static_cast<NativeWindow*>(window));
            },
            +[](void* window) {
                ReleaseWindow(static_cast<NativeWindow*>(window));
            },
            +[](void* window, bool width) {
                return width ? WindowDimension<true>(static_cast<NativeWindow*>(window))
                             : WindowDimension<false>(static_cast<NativeWindow*>(window));
            },
            +[](void* window) {
                return WindowFormat(static_cast<NativeWindow*>(window));
            },
            +[](void* window, int width, int height, int format) {
                return SetWindowGeometry(static_cast<NativeWindow*>(window), width, height, format);
            },
            +[](void* window, AndroidNativeWindowBuffer* buffer, AndroidWindowRect* dirty) {
                return LockWindow(static_cast<NativeWindow*>(window), buffer, dirty);
            },
            +[](void* window) {
                return UnlockAndPostWindow(static_cast<NativeWindow*>(window));
            }};
}

uintptr_t Arm64ConfigurationImport(std::string_view symbol)
{
    static constexpr std::string_view names[]{"AConfiguration_delete",           "AConfiguration_fromAssetManager",
                                              "AConfiguration_getDensity",       "AConfiguration_getScreenHeightDp",
                                              "AConfiguration_getScreenWidthDp", "AConfiguration_new"};
    static const std::array<uint8_t, std::size(names)> tokens{};
    const auto found = std::ranges::find(names, symbol);
    return found == std::end(names) ? 0 : reinterpret_cast<uintptr_t>(&tokens[found - names]);
}

}
