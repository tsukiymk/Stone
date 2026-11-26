#include "../../support/test-suite.h"

#include "abi/AndroidLibraryImports.h"
#include "hle/system/stub-libc.h"
#include "hle/system/stub-libOpenSLES.h"
#include "hle/system/stub-libmediandk-format.h"
#include "hle/system/stub-libmediandk-image-reader.h"
#include "hle/system/stub-libsqlite.h"
#include "hle/system/stub-libz.h"
#include "cpu/arm64/Arm64Initializer.h"
#include <sqlite3.h>

#include <array>
#include <iostream>

namespace stone::windows::test {
namespace {
void CheckMediaFormat()
{
    const auto create = Guest<void* (*)()>(AndroidMediaImport("AMediaFormat_new"), "()L");
    const auto destroy = Guest<void (*)(void*)>(AndroidMediaImport("AMediaFormat_delete"), "(L)V");
    const auto set =
        Guest<void (*)(void*, const char*, int32_t)>(AndroidMediaImport("AMediaFormat_setInt32"), "(LLI)V");
    const auto get =
        Guest<bool (*)(void*, const char*, int32_t*)>(AndroidMediaImport("AMediaFormat_getInt32"), "(LLL)Z");
    const auto format = create();
    STONE_CHECK(format);
    set(format, "width", 1920);
    int32_t width = 0;
    const auto found = get(format, "width", &width);
    const auto absent = get(format, "height", &width);
    destroy(format);
    STONE_CHECK(found && !absent && width == 1920);
}

void CheckImageReader()
{
    const auto create = Guest<int (*)(int, int, int, int, void**)>(AndroidMediaImport("AImageReader_new"), "(IIIIL)I");
    const auto next = Guest<int (*)(void*, void**)>(AndroidMediaImport("AImageReader_acquireNextImage"), "(LL)I");
    const auto destroy = Guest<void (*)(void*)>(AndroidMediaImport("AImageReader_delete"), "(L)V");
    void* reader = nullptr;
    STONE_CHECK(create(0, 8, 1, 2, &reader) == -10004 && !reader);
    STONE_CHECK(create(8, 8, 1, 2, &reader) == 0 && reader);
    void* image = nullptr;
    const int result = next(reader, &image);
    STONE_CHECK(result == -30001 && !image);
    const auto getWindow = Guest<int (*)(void*, void**)>(AndroidMediaImport("AImageReader_getWindow"), "(LL)I");
    const auto dimension = Guest<int (*)(void*)>(Libandroid("ANativeWindow_getWidth"), "(L)I");
    const auto geometry =
        Guest<int (*)(void*, int, int, int)>(Libandroid("ANativeWindow_setBuffersGeometry"), "(LIII)I");
    const auto lock = Guest<int (*)(void*, AndroidNativeWindowBuffer*, AndroidWindowRect*)>(
        Libandroid("ANativeWindow_lock"), "(LLL)I");
    const auto post = Guest<int (*)(void*)>(Libandroid("ANativeWindow_unlockAndPost"), "(L)I");
    const auto hardware = Guest<int (*)(void*, void**)>(AndroidMediaImport("AImage_getHardwareBuffer"), "(LL)I");
    const auto lockBuffer =
        Guest<int (*)(void*, uint64_t, int, const void*, void**)>(Libandroid("AHardwareBuffer_lock"), "(LJILL)I");
    const auto unlockBuffer = Guest<int (*)(void*, int*)>(Libandroid("AHardwareBuffer_unlock"), "(LL)I");
    const auto deleteImage = Guest<void (*)(void*)>(AndroidMediaImport("AImage_delete"), "(L)V");
    void* window = nullptr;
    STONE_CHECK(getWindow(reader, &window) == 0 && window && dimension(window) == 8);
    STONE_CHECK(geometry(window, 0, 0, 0) == 0 && geometry(window, 4, 8, 1) == -22);
    AndroidNativeWindowBuffer pixels{};
    STONE_CHECK(lock(window, &pixels, nullptr) == 0 && pixels.width == 8 && pixels.height == 8 && pixels.stride >= 8 &&
                pixels.format == 1 && pixels.bits);
    auto* rgba = static_cast<uint8_t*>(pixels.bits);
    rgba[0] = 0x12;
    rgba[1] = 0x34;
    rgba[2] = 0x56;
    rgba[3] = 0xff;
    STONE_CHECK(post(window) == 0 && next(reader, &image) == 0 && image);
    void* storage = nullptr;
    void* address = nullptr;
    STONE_CHECK(hardware(image, &storage) == 0 && storage && lockBuffer(storage, 3, -1, nullptr, &address) == 0 &&
                address);
    const auto* bytes = static_cast<const uint8_t*>(address);
    STONE_CHECK(bytes[0] == 0x12 && bytes[1] == 0x34 && bytes[2] == 0x56 && bytes[3] == 0xff);
    STONE_CHECK(unlockBuffer(storage, nullptr) == 0);
    deleteImage(image);
    destroy(reader);
}
}

void TestMedia(Suite& suite)
{
    suite.Run("libaaudio-callback-free-streams-fail-explicitly", [] {
        const auto create = Guest<int (*)(void**)>(AndroidAudioImport("AAudio_createStreamBuilder"), "(L)I");
        const auto open = Guest<int (*)(void*, void**)>(AndroidAudioImport("AAudioStreamBuilder_openStream"), "(LL)I");
        const auto destroy = Guest<int (*)(void*)>(AndroidAudioImport("AAudioStreamBuilder_delete"), "(L)I");
        void* builder = nullptr;
        STONE_CHECK(create(&builder) == 0 && builder);
        void* stream = builder;
        const int result = open(builder, &stream);
        STONE_CHECK(destroy(builder) == 0);
        STONE_CHECK(result == -890 && stream == nullptr);
    });
    suite.Run("libaaudio-actual-shared-stream-performance-capability", [] {
        const auto create = Guest<int (*)(void**)>(AndroidAudioImport("AAudio_createStreamBuilder"), "(L)I");
        const auto callback = Guest<void (*)(void*, uintptr_t, void*)>(
            AndroidAudioImport("AAudioStreamBuilder_setDataCallback"), "(LJL)V");
        const auto hint =
            Guest<void (*)(void*, int)>(AndroidAudioImport("AAudioStreamBuilder_setPerformanceMode"), "(LI)V");
        const auto open = Guest<int (*)(void*, void**)>(AndroidAudioImport("AAudioStreamBuilder_openStream"), "(LL)I");
        const auto remove = Guest<int (*)(void*)>(AndroidAudioImport("AAudioStreamBuilder_delete"), "(L)I");
        const auto close = Guest<int (*)(void*)>(AndroidAudioImport("AAudioStream_close"), "(L)I");
        const auto mode = Guest<int (*)(void*)>(AndroidAudioImport("AAudioStream_getPerformanceMode"), "(L)I");
        const auto mmap = Guest<bool (*)(void*)>(AndroidAudioImport("AAudioStream_isMMapUsed"), "(L)Z");
        void* builder = nullptr;
        STONE_CHECK(create(&builder) == 0);
        // The stream is never started. No callback or audible output is used.
        const auto silent = +[](void*, void*, void*, int) -> int {
            return 1;
        };
        callback(builder, reinterpret_cast<uintptr_t>(silent), nullptr);
        hint(builder, 12);
        void* stream = nullptr;
        const int result = open(builder, &stream);
        STONE_CHECK(remove(builder) == 0);
        if (result == -889) {
            std::cout << "SKIP actual AAudio capability query: no Windows output endpoint\n";
            return;
        }
        STONE_CHECK(result == 0 && stream);
        const bool truthful = mode(stream) == 10 && !mmap(stream);
        STONE_CHECK(close(stream) == 0 && truthful);
    });
    suite.Run("libopensles-native-x86-64-fallback", [] {
        const auto create = Guest<uint32_t (*)(void**, uint32_t, const void*, uint32_t, const void*, const void*)>(
            Libopensles("slCreateEngine"), "(LILILL)I");
        void* engine = nullptr;
        STONE_CHECK(create(&engine, 0, nullptr, 0, nullptr, nullptr) == 12); // SL_RESULT_FEATURE_UNSUPPORTED.
        STONE_CHECK(engine == nullptr && Libopensles("SL_IID_ENGINE"));
    });
    suite.Run("libmediandk-media-format-round-trip", CheckMediaFormat);
    suite.Run("libmediandk-codec-capability", [] {
        const auto decoder =
            Guest<void* (*)(const char*)>(AndroidMediaImport("AMediaCodec_createDecoderByType"), "(L)L");
        const auto encoder =
            Guest<void* (*)(const char*)>(AndroidMediaImport("AMediaCodec_createEncoderByType"), "(L)L");
        STONE_CHECK(decoder("video/avc") == nullptr);
        STONE_CHECK(encoder("application/stone-unknown") == nullptr);
    });
    suite.Run("libmediandk-image-reader-software-window-pixel-round-trip", CheckImageReader);
    suite.Run("libsqlite-image-required-and-win32-vfs-registration-contract", [] {
        sqlite3_vfs local{};
        STONE_CHECK(WindowsSqliteVfs(&local) == &local);
        STONE_CHECK(std::string_view(local.zName) == "stone-win32");
        STONE_CHECK(local.iVersion == 2 && local.szOsFile >= sizeof(sqlite3_file) && !local.pNext);
        STONE_CHECK(local.xOpen && local.xAccess && local.xFullPathname && local.xCurrentTimeInt64);
        STONE_CHECK(NativeSystemElfExport("libsqlite.so", "sqlite3_open") == 0);
        const auto random =
            Guest<int (*)(sqlite3_vfs*, int, char*)>(reinterpret_cast<uintptr_t>(local.xRandomness), "(LIL)I");
        std::array<char, 16> bytes{};
        STONE_CHECK(random(&local, static_cast<int>(bytes.size()), bytes.data()) == bytes.size());
    });
    suite.Run("libz-image-required-and-arm64-error-before-native-dispatch", [] {
        STONE_CHECK(Libz("crc32") == 0);
        constexpr std::array<uint64_t, 8> args{};
        const auto result = CallArm64Zlib("inflate", args, {}, {});
        STONE_CHECK(static_cast<int32_t>(result) == -2); // Z_STREAM_ERROR.
    });
}
}
