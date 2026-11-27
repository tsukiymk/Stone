#include "../../support/test-suite.h"

#include "abi/AndroidLibraryImports.h"
#include "hle/SystemLibraryRegistry.h"
#include "hle/system/stub-libandroid-hardware-buffer.h"
#include "hle/system/stub-libc.h"
#include "hle/system/stub-libc-properties.h"
#include <windows.h>
#include <array>
#include <future>
#include <string>
#include <thread>

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

void CheckNativeWindowLibrary()
{
    void* library = DlOpen("libnativewindow.so", 2);
    STONE_CHECK(library);
    const auto allocateAddress = reinterpret_cast<uintptr_t>(DlSym(library, "AHardwareBuffer_allocate"));
    STONE_CHECK(allocateAddress == Libandroid("AHardwareBuffer_allocate"));
    const auto allocate =
        Guest<int (*)(const AndroidHardwareBufferDesc*, AndroidHardwareBuffer**)>(allocateAddress, "(LL)I");
    const auto describe = Guest<void (*)(const AndroidHardwareBuffer*, AndroidHardwareBufferDesc*)>(
        Libandroid("AHardwareBuffer_describe"), "(LL)V");
    const auto release = Guest<void (*)(const AndroidHardwareBuffer*)>(
        reinterpret_cast<uintptr_t>(DlSym(library, "AHardwareBuffer_release")), "(L)V");
    const auto supported = Guest<int (*)(const AndroidHardwareBufferDesc*)>(
        reinterpret_cast<uintptr_t>(DlSym(library, "AHardwareBuffer_isSupported")), "(L)I");
    AndroidHardwareBufferDesc description{4, 2, 1, 1, 0x33};
    AndroidHardwareBuffer* buffer = nullptr;
    STONE_CHECK(supported(&description) == 1 && allocate(&description, &buffer) == 0 && buffer);
    AndroidHardwareBufferDesc actual{};
    describe(buffer, &actual);
    STONE_CHECK(actual.width == 4 && actual.height == 2 && actual.usage == 0x33);
    release(buffer);
    description.usage = 0x300; // GPU_SAMPLED_IMAGE | GPU_COLOR_OUTPUT
    buffer = reinterpret_cast<AndroidHardwareBuffer*>(1);
    STONE_CHECK(supported(&description) == 0 && allocate(&description, &buffer) == -22 && buffer == nullptr);
    STONE_CHECK(DlSym(library, "ANativeWindow_getWidth") ==
                reinterpret_cast<void*>(Libandroid("ANativeWindow_getWidth")));
    STONE_CHECK(DlSym(library, "AAsset_read") == nullptr);
    STONE_CHECK(DlError() != nullptr && DlClose(library) == 0);
    const auto& nativeWindow = systemLibraries[SystemLibraryIndex("libnativewindow.so")];
    const auto& android = systemLibraries[SystemLibraryIndex("libandroid.so")];
    STONE_CHECK(LookupHleSystemSymbol(nativeWindow, "AAsset_read", {true, false}) == 0);
    const auto conversion = LookupHleSystemSymbol(nativeWindow, "AHardwareBuffer_fromHardwareBuffer", {true, false});
    STONE_CHECK(conversion &&
                conversion == LookupHleSystemSymbol(android, "AHardwareBuffer_fromHardwareBuffer", {true, false}));
}

void CheckLooperReadyBatch()
{
    std::packaged_task<void()> task([] {
        const auto prepare = Guest<void* (*)(int)>(Libandroid("ALooper_prepare"), "(I)L");
        const auto add =
            Guest<int (*)(void*, int, int, int, uintptr_t, void*)>(Libandroid("ALooper_addFd"), "(LIIIJL)I");
        const auto remove = Guest<int (*)(void*, int)>(Libandroid("ALooper_removeFd"), "(LI)I");
        const auto wake = Guest<void (*)(void*)>(Libandroid("ALooper_wake"), "(L)V");
        const auto poll = Guest<int (*)(int, int*, int*, void**)>(Libandroid("ALooper_pollOnce"), "(ILLL)I");
        const auto create = Guest<int (*)(unsigned, int)>(AndroidLooperImport("eventfd"), "(II)I");
        const auto read = Guest<int (*)(int, uint64_t*)>(AndroidLooperImport("eventfd_read"), "(IL)I");
        const auto close = Guest<int (*)(int)>(Libc("close"), "(I)I");
        struct Context {
            decltype(read) receive;
            unsigned called = 0;
            bool valid = true;
        } context{read};
        const auto callback = Import(
            "Stone looper ready batch", +[](int fd, int events, void* data) {
                auto& state = *static_cast<Context*>(data);
                uint64_t value = 0;
                state.valid &= events == 1 && state.receive(fd, &value) == 0 && value == 1;
                ++state.called;
                return 1;
            });
        auto* looper = prepare(1);
        STONE_CHECK(looper);
        std::array<int, 4> descriptors{};
        for (size_t i = 0; i < descriptors.size(); ++i) {
            descriptors[i] = create(1, 0x800);
            STONE_CHECK(descriptors[i] >= 0);
            STONE_CHECK(add(looper, descriptors[i], i < 2 ? 0 : static_cast<int>(i + 35), 1, i < 2 ? callback : 0,
                            &context) == 1);
        }
        wake(looper);
        int fd = -1, events = 0;
        void* data = nullptr;
        STONE_CHECK(poll(0, &fd, &events, &data) == 37);
        STONE_CHECK(context.called == 2 && context.valid);
        STONE_CHECK(fd == descriptors[2] && events == 1 && data == &context);
        // Already-ready identifiers are returned once each, even if the caller
        // has not yet consumed the first descriptor.
        STONE_CHECK(poll(0, &fd, &events, &data) == 38 && fd == descriptors[3]);
        for (int descriptor : descriptors) {
            STONE_CHECK(remove(looper, descriptor) == 1);
            STONE_CHECK(close(descriptor) == 0);
        }
        STONE_CHECK(poll(0, nullptr, nullptr, nullptr) == -1);
        STONE_CHECK(poll(0, nullptr, nullptr, nullptr) == -3);
        const auto write = Guest<int (*)(int, uint64_t)>(AndroidLooperImport("eventfd_write"), "(IJ)I");
        struct Replacement {
            void* looper;
            decltype(add) install;
            decltype(read) receive;
            decltype(write) send;
            uintptr_t next;
            unsigned called = 0;
            bool valid = true;
        } replacement{looper, add, read, write, 0};
        replacement.next = Import(
            "Stone looper replacement", +[](int fd, int, void* data) {
                auto& state = *static_cast<Replacement*>(data);
                uint64_t value = 0;
                state.valid &= state.receive(fd, &value) == 0 && value == 1;
                ++state.called;
                return 0;
            });
        const auto replace = Import(
            "Stone looper replace within callback", +[](int fd, int, void* data) {
                auto& state = *static_cast<Replacement*>(data);
                uint64_t value = 0;
                state.valid &= state.receive(fd, &value) == 0 && value == 1;
                state.valid &= state.install(state.looper, fd, 0, 1, state.next, data) == 1;
                state.valid &= state.send(fd, 1) == 0;
                ++state.called;
                return 0;
            });
        const int replaced = create(1, 0x800);
        STONE_CHECK(replaced >= 0 && add(looper, replaced, 0, 1, replace, &replacement) == 1);
        STONE_CHECK(poll(0, nullptr, nullptr, nullptr) == -2 && replacement.called == 1);
        // Returning zero removes only the registration that actually ran.
        STONE_CHECK(poll(0, nullptr, nullptr, nullptr) == -2 && replacement.called == 2 && replacement.valid);
        STONE_CHECK(remove(looper, replaced) == 0 && close(replaced) == 0);
        STONE_CHECK(poll(0, nullptr, nullptr, nullptr) == -3);
        const auto pair = Guest<int (*)(int, int, int, int*)>(Libc("socketpair"), "(IIIL)I");
        const auto readBytes = Guest<intptr_t (*)(int, void*, size_t)>(Libc("read"), "(ILJ)J");
        const auto writeBytes = Guest<intptr_t (*)(int, const void*, size_t)>(Libc("write"), "(ILJ)J");
        std::array<int, 2> sockets{};
        STONE_CHECK(pair(1, 0x801, 0, sockets.data()) == 0);
        struct SocketContext {
            decltype(readBytes) receive;
            bool called = false;
            bool valid = false;
        } socketContext{readBytes};
        const auto socketCallback = Import(
            "Stone looper socket callback", +[](int fd, int events, void* data) {
                auto& state = *static_cast<SocketContext*>(data);
                char value = 0;
                state.called = true;
                state.valid = events == 3 && state.receive(fd, &value, 1) == 1 && value == 'x';
                return 0;
            });
        STONE_CHECK(add(looper, sockets[0], 0, 3, socketCallback, &socketContext) == 1);
        STONE_CHECK(writeBytes(sockets[1], "x", 1) == 1);
        STONE_CHECK(poll(0, nullptr, nullptr, nullptr) == -2 && socketContext.called && socketContext.valid);
        STONE_CHECK(remove(looper, sockets[0]) == 0);
        STONE_CHECK(add(looper, sockets[1], 55, 2, 0, nullptr) == 1);
        STONE_CHECK(poll(0, &fd, &events, nullptr) == 55 && fd == sockets[1] && events == 2);
        STONE_CHECK(remove(looper, sockets[1]) == 1);
        STONE_CHECK(add(looper, sockets[0], 56, 1, 0, nullptr) == 1 && close(sockets[1]) == 0);
        STONE_CHECK(poll(0, &fd, &events, nullptr) == 56 && fd == sockets[0] && events == 9);
        STONE_CHECK(remove(looper, sockets[0]) == 1 && close(sockets[0]) == 0);
    });
    auto result = task.get_future();
    // A dedicated thread releases its guest TLS and Looper on join. MSVC's
    // async pool retains registered signal recipients after a task returns.
    std::thread worker(std::move(task));
    worker.join();
    result.get();
}

}

void TestAndroid(Suite& suite, int api)
{
    STONE_CHECK(InitializeAndroidNativeBackend());
    SetAndroidTargetSdk(api);
    SetAndroidSystemProperties({{"ro.build.version.sdk", std::to_string(api)}});
    suite.Run("libandroid-shared-memory", CheckSharedMemory);
    suite.Run("libandroid-hardware-buffer", CheckHardwareBuffer);
    suite.Run("libnativewindow-public-handle-ownership", CheckNativeWindowLibrary);
    suite.Run("libandroid-looper-eventfd", CheckLooper);
    suite.Run("libandroid-looper-ready-callback-batch", CheckLooperReadyBatch);
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
