#include "../../support/test-suite.h"

#include "abi/AndroidLibraryImports.h"
#include "hle/system/stub-libandroid-hardware-buffer.h"
#include "hle/system/stub-libc.h"
#include "hle/system/stub-libc-properties.h"
#include <windows.h>
#include <array>
#include <string>

namespace stone::windows::test {
namespace {
void CheckSharedMemory()
{
    const auto create = Guest<int (*)(const char*, size_t)>(Libandroid("ASharedMemory_create"), "(LJ)I");
    const auto size = Guest<size_t (*)(int)>(Libandroid("ASharedMemory_getSize"), "(I)J");
    const auto protect = Guest<int (*)(int, int)>(Libandroid("ASharedMemory_setProt"), "(II)I");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const auto ioctl = Guest<int (*)(int, unsigned, uintptr_t)>(Libc("ioctl"), "(IIJ)I");
    const auto readlink = Guest<intptr_t (*)(const char*, char*, size_t)>(Libc("readlink"), "(LLJ)J");
    const auto duplicate = Guest<int (*)(int)>(Libc("dup"), "(I)I");
    const int fd = create("translation-test", 4096);
    STONE_CHECK(fd >= 0);
    const auto link = "/proc/self/fd/" + std::to_string(fd);
    const std::string expected = "/memfd:translation-test (deleted)";
    std::array<char, 128> target{};
    STONE_CHECK(readlink(link.c_str(), target.data(), target.size()) == expected.size());
    STONE_CHECK(std::string_view(target.data(), expected.size()) == expected);
    target.fill('!');
    STONE_CHECK(readlink(link.c_str(), target.data(), 7) == 7 && std::string_view(target.data(), 7) == "/memfd:" &&
                target[7] == '!');
    const int copy = duplicate(fd);
    STONE_CHECK(copy >= 0);
    const auto ownLink = "/proc/" + std::to_string(GetCurrentProcessId()) + "/fd/" + std::to_string(copy);
    STONE_CHECK(readlink(ownLink.c_str(), target.data(), target.size()) == expected.size());
    STONE_CHECK(std::string_view(target.data(), expected.size()) == expected);
    STONE_CHECK(close(copy) == 0 && readlink(ownLink.c_str(), target.data(), target.size()) == -1);
    const auto capacity = size(fd);
    STONE_CHECK(ioctl(fd, 0x7704, 0) == 4096);  // ASHMEM_GET_SIZE
    STONE_CHECK(ioctl(fd, 0x40087705, 1) == 0); // ASHMEM_SET_PROT_MASK, PROT_READ
    const auto protectResult = protect(fd, 1);
    STONE_CHECK(ioctl(fd, 0x40087705, 3) == -1); // The read-only region cannot regain write access.
    STONE_CHECK(close(fd) == 0);
    STONE_CHECK(readlink(link.c_str(), target.data(), target.size()) == -1);
    STONE_CHECK(capacity == 4096);
    STONE_CHECK(protectResult == 0);
}

void CheckHardwareBuffer()
{
    using Description = AndroidHardwareBufferDesc;
    const auto allocate =
        Guest<int (*)(const Description*, AndroidHardwareBuffer**)>(Libandroid("AHardwareBuffer_allocate"), "(LL)I");
    const auto describe =
        Guest<void (*)(AndroidHardwareBuffer*, Description*)>(Libandroid("AHardwareBuffer_describe"), "(LL)V");
    const auto release = Guest<void (*)(AndroidHardwareBuffer*)>(Libandroid("AHardwareBuffer_release"), "(L)V");
    const auto supported = Guest<int (*)(const Description*)>(Libandroid("AHardwareBuffer_isSupported"), "(L)I");
    Description description{};
    description.width = 4;
    description.height = 2;
    description.layers = 1;
    description.format = 1; // R8G8B8A8_UNORM.
    description.usage = 3;  // CPU_READ_OFTEN.
    STONE_CHECK(supported(&description) == 1);
    Description actual{};
    AndroidHardwareBuffer* buffer = nullptr;
    STONE_CHECK(allocate(&description, &buffer) == 0 && buffer);
    describe(buffer, &actual);
    release(buffer);
    STONE_CHECK(actual.width == 4 && actual.height == 2 && actual.stride >= actual.width);
    description.layers = 0;
    STONE_CHECK(supported(&description) == 0);
}

void CheckLooper()
{
    const auto create = Guest<int (*)(unsigned, int)>(AndroidLooperImport("eventfd"), "(II)I");
    const auto write = Guest<int (*)(int, uint64_t)>(AndroidLooperImport("eventfd_write"), "(IJ)I");
    const auto read = Guest<int (*)(int, uint64_t*)>(AndroidLooperImport("eventfd_read"), "(IL)I");
    const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
    const int fd = create(3, 0);
    STONE_CHECK(fd >= 0);
    uint64_t count = 0;
    const int written = write(fd, 5);
    const int received = read(fd, &count);
    STONE_CHECK(close(fd) == 0);
    STONE_CHECK(written == 0 && received == 0 && count == 8);
}

}

void TestAndroid(Suite& suite, int api)
{
    STONE_CHECK(InitializeAndroidNativeBackend());
    SetAndroidTargetSdk(api);
    SetAndroidSystemProperties({{"ro.build.version.sdk", std::to_string(api)}});
    suite.Run("libandroid-shared-memory", CheckSharedMemory);
    suite.Run("libandroid-hardware-buffer", CheckHardwareBuffer);
    suite.Run("libandroid-looper-eventfd", CheckLooper);
    suite.Run("libandroid-fonts-without-a-mounted-image", [] {
        const auto open = Guest<void* (*)()>(Libandroid("ASystemFontIterator_open"), "()L");
        const auto next = Guest<void* (*)(void*)>(Libandroid("ASystemFontIterator_next"), "(L)L");
        const auto close = Guest<void (*)(void*)>(Libandroid("ASystemFontIterator_close"), "(L)V");
        const auto iterator = open();
        STONE_CHECK(iterator);
        const auto font = next(iterator);
        close(iterator);
        STONE_CHECK(font == nullptr);
    });
    suite.Run("libandroid-sensors-and-configuration", [] {
        const auto manager = Guest<void* (*)()>(Libandroid("ASensorManager_getInstance"), "()L");
        const auto sensors = Guest<int (*)(void*, const void***)>(Libandroid("ASensorManager_getSensorList"), "(LL)I");
        const void** list = nullptr;
        STONE_CHECK(manager() && sensors(manager(), &list) == 0 && list == nullptr);
        STONE_CHECK(Arm64ConfigurationImport("AConfiguration_new"));
        STONE_CHECK(Arm64ConfigurationImport("AConfiguration_unknown") == 0);
    });
    suite.Run("libc-native-device-api-from-system-properties", [api] {
        const auto getApi = Guest<int (*)()>(Libc("android_get_device_api_level"), "()I");
        STONE_CHECK(getApi() == api);
        // Java/NDK agreement and MotionEvent conversion are covered on real ART
        // by the application probe, without the removed custom Android classes.
    });
}
}
