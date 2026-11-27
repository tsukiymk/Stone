#include "../../support/test-suite.h"
#include "../../../host/audio/audio-loopback.h"
#include "../../../../../src/windows/client/Documents.h"
#include "abi/AndroidLibraryImports.h"
#include "runtime/art/ArtRuntime.h"
#include "runtime/art/ArtJit.h"
#include "../../../../../src/windows/client/ArtLaunch.h"
#include "runtime/art/ArtJni.h"
#include "framework/ArtFramework.h"
#include "framework/ArtNotifications.h"
#include "framework/ArtApplicationLauncher.h"
#include "framework/ArtSurfaces.h"
#include "hle/system/stub-libandroid.h"
#include "hle/system/stub-libmediandk-image-reader.h"
#include "platform/windows/ArtSettingsConfirmation.h"
#include "abi/TypedAndroidImport.h"
#include "../../../../../src/windows/client/Window.h"
#include "../../../../../src/windows/client/BrokerClient.h"
#include "../../../../../src/windows/client/ClientApplicationLogs.h"
#include "JobProfiles.h"
#include "../../../../../src/windows/ipc/ServiceProtocol.h"
#include "../../../../../src/windows/host/CommandLine.h"
#include "filesystem/AndroidSystemFiles.h"
#include "loader/AndroidSystemElf.h"
#include "signals/AndroidSignalContext.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "hle/system/stub-libc.h"

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <fstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <crtdbg.h>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <iterator>
#include <string_view>
#include <thread>

namespace stone::windows::test {
namespace {
void ConfigureProbeJitThresholds(ArtRuntimeOptions& options)
{
    if (!options.useJit)
        return;
    for (const auto& [variable, flag] : {std::pair{L"STONE_ART_PROBE_JIT_WARMUP", "-Xjitwarmupthreshold:"},
                                         std::pair{L"STONE_ART_PROBE_JIT_THRESHOLD", "-Xjitthreshold:"}}) {
        wchar_t setting[32]{};
        const auto length = GetEnvironmentVariableW(variable, setting, static_cast<DWORD>(std::size(setting)));
        if (!length)
            continue;
        if (length >= std::size(setting) || !std::all_of(setting, setting + length, [](wchar_t c) {
                return c >= L'0' && c <= L'9';
            }))
            throw std::runtime_error("Invalid ART probe JIT threshold");
        const auto value = std::stoul(setting);
        if (!value || value > UINT16_MAX)
            throw std::runtime_error("ART probe JIT threshold must be between 1 and 65535");
        options.vmOptions.push_back(flag + std::to_string(value));
        std::cout << "ART probe JIT option " << options.vmOptions.back() << '\n';
    }
}

// Opt-in counts only: leave fault handling to the actual runtime handler.
std::array<std::atomic_uint64_t, 3> performanceFaults{};

LONG CALLBACK CountPerformanceFault(EXCEPTION_POINTERS* exception)
{
    const auto& fault = *exception->ExceptionRecord;
    const unsigned kind = fault.ExceptionCode == EXCEPTION_BREAKPOINT ? 0
                          : fault.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && fault.NumberParameters >= 2 &&
                                  fault.ExceptionInformation[0] == 0 && fault.ExceptionInformation[1] < 4096
                              ? 1
                              : 2;
    const auto count = performanceFaults[kind].fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1 || (count >= 1024 && !(count & (count - 1))))
        std::fprintf(stderr, "PERF fault kind=%u count=%llu code=%lx pc=%llx address=%llx tick=%llu\n", kind,
                     static_cast<unsigned long long>(count), fault.ExceptionCode,
                     static_cast<unsigned long long>(exception->ContextRecord->Rip),
                     static_cast<unsigned long long>(fault.NumberParameters >= 2 ? fault.ExceptionInformation[1] : 0),
                     static_cast<unsigned long long>(GetTickCount64()));
    return EXCEPTION_CONTINUE_SEARCH;
}

std::atomic_uint64_t batteryFixture = 0;
std::array<jint, 4> batteryHost{};

jintArray BatteryFixture(JNIEnv* env, jclass)
{
    const auto packed = batteryFixture.load();
    jint values[4]{};
    for (size_t index = 0; index < 4; ++index)
        values[index] = static_cast<jint>((packed >> (index * 8)) & 255);
    const auto result = ArtJni<&JNINativeInterface_::NewIntArray>(env, 4);
    if (result)
        ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, result, 0, 4, values);
    return result;
}

void SetBatteryFixture(JNIEnv* env, jclass, jintArray values)
{
    jint raw[4]{};
    ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, values, 0, 4, raw);
    uint64_t packed = 0;
    for (size_t index = 0; index < 4; ++index)
        packed |= static_cast<uint64_t>(raw[index] & 255) << (index * 8);
    batteryFixture = packed;
}

bool RunWindowCheck(HWND hwnd, const std::function<bool()>& run)
{
    constexpr UINT ProbeMessage = WM_APP + 43;
    const auto thread = GetWindowThreadProcessId(hwnd, nullptr);
    if (!thread)
        return false;

    struct WindowCheck {
        HWND hwnd;
        const std::function<bool()>& run;
        std::atomic_bool passed = false;
    } check{hwnd, run};

    static std::atomic<WindowCheck*> pending;
    // Deliver synthetic input and check Win32 state as one operation on the owning thread.
    // A real mouse-leave message can arrive between separate cross-thread sends.
    const auto hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        +[](int code, WPARAM sentFromOwner, LPARAM data) -> LRESULT {
            if (code == HC_ACTION) {
                const auto& message = *reinterpret_cast<const CWPSTRUCT*>(data);
                if (auto* request = pending.load(); request && message.hwnd == request->hwnd &&
                                                    message.message == ProbeMessage &&
                                                    message.wParam == reinterpret_cast<WPARAM>(request)) {
                    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                    request->passed = request->run();
                    if (previous)
                        SetThreadDpiAwarenessContext(previous);
                }
            }
            return CallNextHookEx(nullptr, code, sentFromOwner, data);
        },
        nullptr, thread);
    if (!hook) {
        std::cerr << "Could not install ART window probe hook: " << GetLastError() << '\n';
        return false;
    }
    pending = &check;
    SendMessageW(hwnd, ProbeMessage, reinterpret_cast<WPARAM>(&check), 0);
    pending = nullptr;
    UnhookWindowsHookEx(hook);
    return check.passed;
}

template <class Function> Function ImageApi(void* library, const char* name, const char* signature)
{
    return Guest<Function>(reinterpret_cast<uintptr_t>(DlSym(library, name)), signature);
}

jboolean VerifyBitmap(JNIEnv* env, jclass, jobject bitmap)
{
    struct Info {
        uint32_t width, height, stride;
        int32_t format;
        uint32_t flags;
    };

    const auto library = DlOpen("libjnigraphics.so", 2);
    if (!library) {
        std::cerr << "NDK Bitmap library: " << DlError() << '\n';
        return JNI_FALSE;
    }
    const auto info = ImageApi<int (*)(JNIEnv*, jobject, Info*)>(library, "AndroidBitmap_getInfo", "(LLL)I");
    const auto lock = ImageApi<int (*)(JNIEnv*, jobject, void**)>(library, "AndroidBitmap_lockPixels", "(LLL)I");
    const auto unlock = ImageApi<int (*)(JNIEnv*, jobject)>(library, "AndroidBitmap_unlockPixels", "(LL)I");
    Info value{};
    bool good = info(env, bitmap, &value) == 0 && value.width == 64 && value.height == 64 && value.stride >= 64 * 4 &&
                value.format == 1;
    if (!good)
        std::cerr << "NDK Bitmap info mismatch: " << value.width << ',' << value.height << ',' << value.stride << ','
                  << value.format << '\n';
    void* pixels = nullptr;
    const int locked = good ? lock(env, bitmap, &pixels) : -999;
    if (locked != 0)
        std::cerr << "NDK Bitmap lock: " << locked
                  << " exception=" << int(ArtJni<&JNINativeInterface_::ExceptionCheck>(env)) << '\n';
    if (good && locked == 0) {
        auto* rgba = static_cast<uint8_t*>(pixels);
        good = rgba && rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 255 && rgba[3] == 255;
        if (!good && rgba)
            std::cerr << "NDK Bitmap pixel mismatch: " << int(rgba[0]) << ',' << int(rgba[1]) << ',' << int(rgba[2])
                      << ',' << int(rgba[3]) << '\n';
        if (good) {
            rgba[0] = 0x34;
            rgba[1] = 0x56;
            rgba[2] = 0x78;
            rgba[3] = 0xff;
        }
        const auto unlocked = unlock(env, bitmap);
        if (unlocked != 0)
            std::cerr << "NDK Bitmap unlock: " << unlocked << '\n';
        good = unlocked == 0 && good;
    }
    else
        good = false;
    const auto invalid = info(env, nullptr, &value);
    if (invalid != -1)
        std::cerr << "NDK Bitmap null result: " << invalid << '\n';
    good = invalid == -1 && good;
    const int closed = DlClose(library);
    if (closed != 0)
        std::cerr << "NDK Bitmap close: " << DlError() << '\n';
    return closed == 0 && good ? JNI_TRUE : JNI_FALSE;
}

void UnsupportedSurface(JNIEnv* env, jclass, jboolean toSurface)
{
    const auto convert = Guest<void* (*)(JNIEnv*, void*)>(
        ArtFrameworkImport(toSurface ? "ANativeWindow_toSurface" : "ANativeWindow_fromSurface"), "(LL)L");
    convert(env, nullptr);
}

jboolean VerifyAssets(JNIEnv* env, jclass, jobject manager, jbyteArray content, jint density, jint width, jint height)
{
    const auto library = DlOpen("libandroid.so", 2);
    if (!library)
        return JNI_FALSE;
    const auto convert = ImageApi<void* (*)(JNIEnv*, jobject)>(library, "AAssetManager_fromJava", "(LL)L");
    const auto open = ImageApi<void* (*)(void*, const char*, int)>(library, "AAssetManager_open", "(LLI)L");
    const auto close = ImageApi<void (*)(void*)>(library, "AAsset_close", "(L)V");
    const auto length = ImageApi<int64_t (*)(void*)>(library, "AAsset_getLength64", "(L)J");
    const auto remaining = ImageApi<int64_t (*)(void*)>(library, "AAsset_getRemainingLength64", "(L)J");
    const auto read = ImageApi<int (*)(void*, void*, size_t)>(library, "AAsset_read", "(LLJ)I");
    const auto seek = ImageApi<int64_t (*)(void*, int64_t, int)>(library, "AAsset_seek64", "(LJI)J");
    const auto buffer = ImageApi<const void* (*)(void*)>(library, "AAsset_getBuffer", "(L)L");
    const auto openFd = ImageApi<int (*)(void*, int64_t*, int64_t*)>(library, "AAsset_openFileDescriptor64", "(LLL)I");
    const auto openDir = ImageApi<void* (*)(void*, const char*)>(library, "AAssetManager_openDir", "(LL)L");
    const auto closeDir = ImageApi<void (*)(void*)>(library, "AAssetDir_close", "(L)V");
    const auto next = ImageApi<const char* (*)(void*)>(library, "AAssetDir_getNextFileName", "(L)L");
    const auto rewind = ImageApi<void (*)(void*)>(library, "AAssetDir_rewind", "(L)V");
    const auto createConfig = ImageApi<void* (*)()>(library, "AConfiguration_new", "()L");
    const auto deleteConfig = ImageApi<void (*)(void*)>(library, "AConfiguration_delete", "(L)V");
    const auto fromAssets = ImageApi<void (*)(void*, void*)>(library, "AConfiguration_fromAssetManager", "(LL)V");
    const auto getDensity = ImageApi<int (*)(void*)>(library, "AConfiguration_getDensity", "(L)I");
    const auto getWidth = ImageApi<int (*)(void*)>(library, "AConfiguration_getScreenWidthDp", "(L)I");
    const auto getHeight = ImageApi<int (*)(void*)>(library, "AConfiguration_getScreenHeightDp", "(L)I");
    const auto native = convert(env, manager);
    if (!native) {
        DlClose(library);
        return JNI_FALSE;
    }
    const auto count = ArtJni<&JNINativeInterface_::GetArrayLength>(env, content);
    std::vector<jbyte> expected(static_cast<size_t>(count)), actual(expected.size());
    ArtJni<&JNINativeInterface_::GetByteArrayRegion>(env, content, 0, count, expected.data());
    bool good = count > 4 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env) &&
                reinterpret_cast<uintptr_t>(DlSym(library, "AAssetManager_fromJava")) ==
                    ArtImageLibraryExport("libandroid.so", "AAssetManager_fromJava");
    const auto asset = open(native, "native-check.txt", 3);
    if (asset) {
        good = length(asset) == count && remaining(asset) == count && good;
        const auto data = buffer(asset);
        good = data && std::memcmp(data, expected.data(), expected.size()) == 0 && good;
        if (!good)
            std::cerr << "NDK asset length/buffer mismatch: expected=" << count << " actual=" << length(asset) << '\n';
        good =
            read(asset, actual.data(), actual.size()) == count && actual == expected && remaining(asset) == 0 && good;
        good = seek(asset, -4, SEEK_END) == count - 4 && remaining(asset) == 4 && good;
        good = read(asset, actual.data(), 4) == 4 && std::memcmp(actual.data(), expected.data() + count - 4, 4) == 0 &&
               good;
        if (!good)
            std::cerr << "NDK asset read/seek mismatch\n";
        int64_t offset = -1, bytes = -1;
        const int fd = openFd(asset, &offset, &bytes);
        if (fd >= 0) {
            const auto pread = Guest<int64_t (*)(int, void*, size_t, int64_t)>(Libc("pread64"), "(ILJJ)J");
            const auto closeFd = Guest<int (*)(int)>(Libc("close"), "(I)I");
            good = bytes == count && offset > 0 && pread(fd, actual.data(), actual.size(), offset) == count &&
                   actual == expected && good;
            good = closeFd(fd) == 0 && good;
        }
        else
            good = false;
        if (!good)
            std::cerr << "NDK asset descriptor mismatch: fd=" << fd << " offset=" << offset << " length=" << bytes
                      << '\n';
        close(asset);
    }
    else
        good = false;
    const auto directory = openDir(native, "");
    if (directory) {
        bool found = false;
        const auto first = next(directory);
        const std::string firstName = first ? first : "";
        if (firstName == "native-check.txt")
            found = true;
        while (const auto name = next(directory))
            if (std::string_view(name) == "native-check.txt")
                found = true;
        rewind(directory);
        const auto again = next(directory);
        good = found && again && firstName == again && good;
        if (!found || !again || firstName != again)
            std::cerr << "NDK asset directory mismatch\n";
        closeDir(directory);
    }
    else
        good = false;
    const auto configuration = createConfig();
    if (configuration) {
        fromAssets(configuration, native);
        if (getDensity(configuration) != density || getWidth(configuration) != width ||
            getHeight(configuration) != height)
            std::cerr << "NDK asset configuration mismatch: " << getDensity(configuration) << ','
                      << getWidth(configuration) << ',' << getHeight(configuration) << " Java=" << density << ','
                      << width << ',' << height << '\n';
        good = getDensity(configuration) == density && getWidth(configuration) == width &&
               getHeight(configuration) == height && good;
        deleteConfig(configuration);
    }
    else
        good = false;
    return DlClose(library) == 0 && good ? JNI_TRUE : JNI_FALSE;
}

void CopyReadOnlyCode(const std::filesystem::path& source, const std::filesystem::path& target)
{
    std::filesystem::create_directories(target.parent_path());
    const auto previous = GetFileAttributesW(target.c_str());
    if (previous != INVALID_FILE_ATTRIBUTES && (previous & FILE_ATTRIBUTE_READONLY)) {
        const auto writable = previous & ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileAttributesW(target.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL))
            throw std::runtime_error("could not update cached ART code attributes");
    }
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(target.c_str(), attributes | FILE_ATTRIBUTE_READONLY))
        throw std::runtime_error("could not make cached ART code read-only");
}

void VerifyImageCallbacks()
{
    const auto crc = ArtImageLibraryExport("libz.so", "crc32");
    const auto adler = ArtImageLibraryExport("libz.so", "adler32");
    const auto bound = ArtImageLibraryExport("liblz4.so", "LZ4_compressBound");
    const AndroidHostCallback hostFunction = +[](uintptr_t value, void*, void*, void*) {
        return value + 1;
    };
    const auto host = ImportAndroidCallback("Stone callback scope regression", hostFunction);
    if (IsArm64TranslationEnabled()) {
        const auto ordinary = Import("Stone unmarked callback rejection", hostFunction);
        for (const auto target : {ordinary, reinterpret_cast<uintptr_t>(hostFunction)}) {
            bool rejected = false;
            try {
                CallAndroidCallback(target, reinterpret_cast<void*>(41));
            }
            catch (const std::runtime_error&) {
                rejected = true;
            }
            STONE_CHECK(rejected);
        }
    }
    STONE_CHECK(crc && adler && bound);
    const auto verify = [=](unsigned count) {
        char text[] = "123456789";
        const auto guestText = IsArm64TranslationEnabled() ? AllocateArm64GuestMemory(sizeof(text), true) : 0;
        struct Free {
            uintptr_t address;
            ~Free()
            {
                if (address)
                    FreeArm64GuestMemory(address);
            }
        } free{guestText};
        STONE_CHECK(!IsArm64TranslationEnabled() || guestText);
        auto* input = guestText ? reinterpret_cast<char*>(guestText) : text;
        if (guestText)
            std::memcpy(input, text, sizeof(text));
        for (unsigned i = 0; i < count; ++i) {
            STONE_CHECK(CallAndroidCallback(crc, nullptr, input, reinterpret_cast<void*>(9)) == 0xcbf43926);
            STONE_CHECK(CallAndroidCallback(adler, reinterpret_cast<void*>(1), input, reinterpret_cast<void*>(9)) ==
                        0x091e01de);
            STONE_CHECK(CallAndroidCallback(bound, reinterpret_cast<void*>(100)) == 116);
            STONE_CHECK(CallAndroidCallback(host, reinterpret_cast<void*>(41)) == 42);
        }
    };
    // The same published code must dispatch correctly from another Windows
    // thread and after returning to callbacks in a different image.
    std::exception_ptr failure;
    std::jthread worker([&] {
        try {
            verify(32);
        }
        catch (...) {
            failure = std::current_exception();
        }
    });
    verify(32);
    worker.join();
    if (failure)
        std::rethrow_exception(failure);
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_CALLBACK_BENCHMARK", nullptr, 0)) {
        const auto start = std::chrono::steady_clock::now();
        verify(10000);
        std::cout
            << "ART image callbacks: 40000 checked calls in "
            << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count()
            << " us\n";
    }
    std::cout << "PASS image CRC32, Adler32, LZ4 and host callbacks across native threads.\n";
}

void VerifyArm64ImageSignalChain()
{
    const auto memory = AllocateArm64GuestMemory(256, true);
    STONE_CHECK(memory);
    std::memset(reinterpret_cast<void*>(memory), 0, 256);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto* code = static_cast<uint8_t*>(AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0));
    STONE_CHECK(code != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* address;

        ~Unmap()
        {
            AndroidUnmapArm64(address, 4096);
        }
    } unmap{code};

    // The real image sigchain forwards a libm fault to this guest handler.
    // Record the fault and skip its store; leave the floating result intact.
    const uint32_t handler[]{0x58000203, 0xf9400064, 0x91000484, 0xf9000064, 0xf9400824, 0xf9000464,
                             0xf940dc44, 0xf9000864, 0x91001084, 0xf900dc44, 0xd65f03c0};
    const uintptr_t recordAddress = memory + 128;
    std::memcpy(code, handler, sizeof(handler));
    std::memcpy(code + 64, &recordAddress, sizeof(recordAddress));
    STONE_CHECK(AndroidProtectArm64(code, 4096, 5) == 0);
    auto* requested = reinterpret_cast<AndroidSignalAction*>(memory);
    auto* previous = reinterpret_cast<AndroidSignalAction*>(memory + 64);
    *requested = {4, reinterpret_cast<uintptr_t>(code), 0, 0};
    const auto action = Guest<int (*)(int, const AndroidSignalAction*, AndroidSignalAction*)>(
        ArtImageLibraryExport("libsigchain.so", "sigaction64"), "(ILL)I");
    STONE_CHECK(action(11, requested, previous) == 0);

    struct Restore {
        decltype(action) call;
        AndroidSignalAction* previous;

        ~Restore()
        {
            call(11, previous, nullptr);
        }
    } restore{action, previous};

    const auto frexp = Guest<double (*)(double, int*)>(ArtImageLibraryExport("libm.so", "frexp"), "(DL)D");
    for (unsigned i = 0; i < 16; ++i)
        STONE_CHECK(frexp(8.0, nullptr) == 0.5);
    const auto* record = reinterpret_cast<const uint64_t*>(recordAddress);
    AndroidDlInfo info{};
    STONE_CHECK(record[0] >= 16 && record[1] == 0 && DlAddr(reinterpret_cast<void*>(record[2]), &info) && info.file &&
                std::string_view(info.file).ends_with("libm.so"));
    std::cout << "PASS real ARM64 image sigchain, mapped handler, repeated faults and floating-state restoration.\n";
}

void VerifyImageSignalChain()
{
    if (IsArm64TranslationEnabled()) {
        VerifyArm64ImageSignalChain();
        return;
    }
    const auto action = Guest<int (*)(int, const AndroidSignalAction*, AndroidSignalAction*)>(
        ArtImageLibraryExport("libsigchain.so", "sigaction64"), "(ILL)I");
    const auto mask = Guest<int (*)(int, const uint64_t*, uint64_t*)>(Libc("sigprocmask64"), "(ILL)I");
    const auto map = Guest<void* (*)(void*, size_t, int, int, int, int64_t)>(Libc("mmap"), "(LJIIIJ)L");
    const auto protect = Guest<int (*)(void*, size_t, int)>(Libc("mprotect"), "(LJI)I");
    const auto unmap = Guest<int (*)(void*, size_t)>(Libc("munmap"), "(LJ)I");
    auto* memory = static_cast<uint8_t*>(map(nullptr, 4096, 3, 0x22, -1, 0));
    STONE_CHECK(memory != reinterpret_cast<void*>(-1));

    struct Release {
        decltype(unmap) call;
        void* address;

        ~Release()
        {
            call(address, 4096);
        }
    } release{unmap, memory};

    constexpr uint8_t code[]{0x8b, 0x04, 0x25, 0, 0, 0, 0, 0xc3}; // mov eax,[0]; ret.
    std::memcpy(memory, code, sizeof(code));
    STONE_CHECK(protect(memory, 4096, 5) == 0 && FlushInstructionCache(GetCurrentProcess(), memory, sizeof(code)));
    static uintptr_t expected;
    static unsigned delivered;
    expected = reinterpret_cast<uintptr_t>(memory);
    delivered = 0;
    const auto callback = Import(
        "Stone image sigchain dynamic code handler",
        +[](int signal, AndroidSignalInfo* info, AndroidSignalContext* context) {
            if (signal == 11 && info->number == 11 && info->code == 1 && info->detail[0] == 0 &&
                context->machine.registers[16] == expected && context->machine.registers[22] == 0)
                ++delivered;
            context->machine.registers[16] += 7;
            context->machine.registers[13] = 42;
        });
    const AndroidSignalAction handler{4, callback, 0, 0};
    AndroidSignalAction old{};
    STONE_CHECK(action(11, &handler, &old) == 0);

    struct Restore {
        decltype(action) set;
        AndroidSignalAction old;

        ~Restore()
        {
            set(11, &old, nullptr);
        }
    } restore{action, old};

    uint64_t before = 0, after = 0;
    STONE_CHECK(mask(0, nullptr, &before) == 0);
    const auto run = Guest<int (*)()>(expected, "()I");
    STONE_CHECK(run() == 42 && run() == 42 && delivered == 2);
    STONE_CHECK(mask(0, nullptr, &after) == 0 && after == before);
    std::cout
        << "PASS real image sigchain, dynamic code SIGSEGV, repeated user callbacks and signal-mask restoration.\n";
}

jboolean VerifyMotion(JNIEnv* env, jclass, jobject event, jint sdk)
{
    const auto convert = Guest<void* (*)(JNIEnv*, jobject)>(ArtFrameworkImport("AMotionEvent_fromJava"), "(LL)L");
    const auto native = convert(env, event);
    if (!native)
        return JNI_FALSE;
    const auto integer = [&](const char* name) {
        return Guest<int (*)(const void*)>(Libandroid(name), "(L)I")(native);
    };
    const auto count = [&](const char* name) {
        return Guest<size_t (*)(const void*)>(Libandroid(name), "(L)J")(native);
    };
    const auto time = [&](const char* name) {
        return Guest<int64_t (*)(const void*)>(Libandroid(name), "(L)J")(native);
    };
    const auto coordinate = [&](const char* name) {
        return Guest<float (*)(const void*, size_t)>(Libandroid(name), "(LJ)F")(native, 0);
    };
    const auto id = Guest<int (*)(const void*, size_t)>(Libandroid("AMotionEvent_getPointerId"), "(LJ)I");
    const auto tool = Guest<int (*)(const void*, size_t)>(Libandroid("AMotionEvent_getToolType"), "(LJ)I");
    const auto historyX =
        Guest<float (*)(const void*, size_t, size_t)>(Libandroid("AMotionEvent_getHistoricalX"), "(LJJ)F");
    const auto historyTime =
        Guest<int64_t (*)(const void*, size_t)>(Libandroid("AMotionEvent_getHistoricalEventTime"), "(LJ)J");
    const auto deviceApi = Guest<int (*)()>(Libc("android_get_device_api_level"), "()I");
    const bool valid = deviceApi() == sdk && integer("AInputEvent_getType") == 2 &&
                       integer("AInputEvent_getSource") == 0x2002 && integer("AInputEvent_getDeviceId") == 42 &&
                       integer("AMotionEvent_getAction") == 2 && integer("AMotionEvent_getMetaState") == 1 &&
                       integer("AMotionEvent_getButtonState") == 1 && count("AMotionEvent_getPointerCount") == 1 &&
                       count("AMotionEvent_getHistorySize") == 1 && id(native, 0) == 7 && tool(native, 0) == 3 &&
                       coordinate("AMotionEvent_getX") == 20.0f && coordinate("AMotionEvent_getY") == 26.0f &&
                       coordinate("AMotionEvent_getRawX") == 17.0f && coordinate("AMotionEvent_getRawY") == 22.0f &&
                       coordinate("AMotionEvent_getPressure") == 0.75f && historyX(native, 0, 0) == 18.25f &&
                       time("AMotionEvent_getDownTime") == 10'000'000 &&
                       time("AMotionEvent_getEventTime") == 12'000'000 && historyTime(native, 0) == 11'000'000;
    Guest<void (*)(const void*)>(Libandroid("AInputEvent_release"), "(L)V")(native);
    return valid ? JNI_TRUE : JNI_FALSE;
}
}

static void RunArtAppSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                            const std::filesystem::path& apk, const std::filesystem::path& artHost,
                            const char* probeClass, const wchar_t* defaultData, std::wstring_view argument)
{
    namespace fs = std::filesystem;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    const auto root = fs::canonical(directory);
    const bool notificationSample = std::string_view(probeClass) == "com/trebledot/stone/probe/ForegroundSample";
    wchar_t externalData[32768]{};
    const auto length = GetEnvironmentVariableW(L"STONE_ART_DATA_DIRECTORY", externalData, 32768);
    STONE_CHECK(length < 32768);
    const auto data = length ? fs::absolute(externalData) : root.parent_path() / defaultData;
    CopyReadOnlyCode(fs::canonical(dex), data / "probe.dex");
    CopyReadOnlyCode(fs::canonical(apk), data / "application.apk");
    CopyReadOnlyCode(fs::canonical(artHost), data / "stone-art-runtime.jar");
    if (std::string_view(probeClass) == "com/trebledot/stone/runtime/WebViewProviderProbe") {
        for (const auto* name : {"v2.apk", "unsigned.apk", "tampered-v2.apk", "launcher.apk"})
            CopyReadOnlyCode(fs::canonical(dex.parent_path().parent_path() / "archives" / name),
                             data / "webview-fixtures" / name);
    }
    if (argument == L"codecs")
        CopyReadOnlyCode(fs::canonical(dex.parent_path() / L"opus-fixture.webm"), data / L"opus-fixture.webm");
    if (argument == L"video") {
        CopyReadOnlyCode(fs::canonical(dex.parent_path() / L"avc-fixture.bin"), data / L"avc-fixture.bin");
        CopyReadOnlyCode(fs::canonical(dex.parent_path() / L"aac-fixture.bin"), data / L"aac-fixture.bin");
    }
    if (std::string_view(probeClass) == "com/trebledot/stone/runtime/ResizeWindowProbe") {
        STONE_CHECK(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr);
        for (const auto& fixture : fs::directory_iterator(dex.parent_path().parent_path() / "resize"))
            if (fixture.path().extension() == ".apk")
                CopyReadOnlyCode(fixture.path(), data / "resize-fixtures" / fixture.path().filename());
    }
    ArtRuntimeOptions options{root, "/data/probe.dex:/data/application.apk", {}, {"/data/stone-art-runtime.jar"}};
    options.useImageBootClassPath = true;
    options.dataDirectory = data;
    wchar_t sampleJit[2]{};
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_JIT", sampleJit, 2) == 1 && sampleJit[0] == L'1') {
        options.useJit = true;
    }
    ConfigureProbeJitThresholds(options);
    // APK-owned executors live until this isolated process exits, as in the APK launcher.
    wchar_t recreateSettings[2]{};
    if (GetEnvironmentVariableW(L"STONE_SETTINGS_RECREATE", recreateSettings, 2) == 1 && recreateSettings[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.native-confirmation-recreate=true");
    wchar_t settingsExecutable[32768]{}, settingsArguments[32768]{};
    const auto settingsLength = GetEnvironmentVariableW(L"STONE_SETTINGS_EXECUTABLE", settingsExecutable, 32768);
    const auto argumentsLength = GetEnvironmentVariableW(L"STONE_SETTINGS_TEST_ARGUMENTS", settingsArguments, 32768);
    std::wstring notificationImage(32768, L'\0'), notificationService(32768, L'\0');
    const auto notificationImageLength =
        notificationSample
            ? GetEnvironmentVariableW(L"STONE_NOTIFICATION_SYSTEM_IMAGE", notificationImage.data(), 32768)
            : 0;
    const auto notificationServiceLength =
        notificationImageLength ? GetEnvironmentVariableW(L"STONE_PROBE_SERVICE", notificationService.data(), 32768)
                                : 0;
    STONE_CHECK(notificationImageLength < 32768 && notificationServiceLength < 32768);
    notificationImage.resize(notificationImageLength);
    notificationService.resize(notificationServiceLength);
    if (notificationImageLength) {
        STONE_CHECK(notificationServiceLength && data.filename() == L"data");
        ConfigureClientShell(notificationService, data.parent_path(), notificationImage, true);
        InitializeClientSession();
        ConfigureClientLaunchProfile(root, data / L"application.apk", apk, L"com.trebledot.stone.foreground.fixture");
        RegisterClientLaunchProfile(L"com.trebledot.stone.foreground.fixture");
        for (const auto& invalid : {std::vector<std::wstring>{data.parent_path().wstring(),
                                                              L"com.trebledot.stone.foreground.fixture", L"init"},
                                    std::vector<std::wstring>{data.wstring(), L"com.other.application", L"init"}}) {
            bool denied = false;
            try {
                ClientNotificationRequest(invalid);
            }
            catch (const std::runtime_error&) {
                denied = true;
            }
            STONE_CHECK(denied);
        }
        if (notificationSample)
            options.vmOptions.push_back("-Dstone.probe.service-notifications=true");
    }
    STONE_CHECK(settingsLength < 32768 && argumentsLength < 32768);
    if (settingsLength) {
        ConfigureArtSettings(fs::absolute(settingsExecutable), settingsArguments);
        if (notificationSample) {
            wchar_t self[32768]{};
            const auto size = GetModuleFileNameW(nullptr, self, 32768);
            STONE_CHECK(size && size < 32768);
            ConfigureArtNotifications(settingsExecutable, notificationImageLength ? fs::path{} : fs::path(self));
        }
    }
    // Do not wait for arbitrary application threads in DestroyJavaVM.
    auto* runtime = new ArtRuntime(options);
    auto* env = runtime->GuestEnvironment();
    STONE_CHECK(env && GetModuleHandleW(L"jvm.dll") == nullptr);
    STONE_CHECK(RegisterArtFramework(env) && RegisterArtResources(env) && RegisterArtGraphics(env) &&
                RegisterArtHost(env));
    if (GetEnvironmentVariableW(L"STONE_PROBE_PERFORMANCE_FAULTS", nullptr, 0))
        STONE_CHECK(AddVectoredExceptionHandler(1, CountPerformanceFault) != nullptr);
    const auto looper = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/Looper");
    const auto prepare =
        looper ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, looper, "prepareMainLooper", "()V") : nullptr;
    if (prepare)
        ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, looper, prepare, nullptr);
    const auto assetType = ArtJni<&JNINativeInterface_::FindClass>(env, "android/content/res/AssetManager");
    const auto constructor =
        assetType ? ArtJni<&JNINativeInterface_::GetMethodID>(env, assetType, "<init>", "()V") : nullptr;
    const auto assets =
        constructor ? ArtJni<&JNINativeInterface_::NewObjectA>(env, assetType, constructor, nullptr) : nullptr;
    const auto probe = ArtJni<&JNINativeInterface_::FindClass>(env, probeClass);
    if (std::string_view(probeClass) == "com/trebledot/stone/runtime/ResizeWindowProbe") {
        const auto resize = +[](JNIEnv*, jclass, jint width, jint height, jint command) -> jlong {
            const auto hwnd = WindowsHostWindow();
            if (!hwnd)
                return 0;
            if (command)
                SendMessageW(hwnd, WM_SYSCOMMAND, command == 1 ? SC_MAXIMIZE : SC_RESTORE, 0);
            else {
                RECT bounds{0, 0, width, height};
                if (!AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                                              static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)),
                                              GetDpiForWindow(hwnd)) ||
                    !SetWindowPos(hwnd, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                                  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
                    return 0;
            }
            return static_cast<jlong>(WindowsArtClientSize());
        };
        const auto policy = +[](JNIEnv*, jclass) -> jboolean {
            const auto style = GetWindowLongPtrW(WindowsHostWindow(), GWL_STYLE);
            return (style & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == (WS_THICKFRAME | WS_MAXIMIZEBOX) ? JNI_TRUE
                                                                                                  : JNI_FALSE;
        };
        const auto mask = +[](JNIEnv*, jclass, jint stage) -> jboolean {
            const auto hwnd = WindowsHostWindow();
            if (!hwnd)
                return JNI_FALSE;
            if (stage == 1) {
                RECT bounds{};
                if (!GetWindowRect(hwnd, &bounds))
                    return JNI_FALSE;
                SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&bounds));
            }
            else if (stage == 3)
                SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
            if (!RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW))
                return JNI_FALSE;
            RECT client{};
            if (!GetClientRect(hwnd, &client))
                return JNI_FALSE;
            const auto dc = GetDC(hwnd);
            if (!dc)
                return JNI_FALSE;
            const auto corner = GetPixel(dc, 4, 4), center = GetPixel(dc, client.right / 2, client.bottom / 2);
            ReleaseDC(hwnd, dc);
            const bool masked = stage >= 1 && stage <= 3;
            const auto expectedCorner = masked ? RGB(32, 32, 32) : RGB(0x19, 0x73, 0x45);
            const auto expectedCenter = masked ? RGB(0xee, 0x22, 0) : expectedCorner;
            const bool deferred = stage == 1 || stage == 2;
            // Windows can round interpolated icon channels when the source PNG
            // is enlarged for a high-DPI monitor.
            const bool centerMatches =
                masked ? std::abs(int(GetRValue(center)) - int(GetRValue(expectedCenter))) <= 3 &&
                             std::abs(int(GetGValue(center)) - int(GetGValue(expectedCenter))) <= 3 &&
                             std::abs(int(GetBValue(center)) - int(GetBValue(expectedCenter))) <= 3
                       : center == expectedCenter;
            const bool passed = corner == expectedCorner && centerMatches && (WindowsArtClientSize() == 0) == deferred;
            if (!passed)
                std::fprintf(stderr, "resize mask stage=%d corner=%08lx center=%08lx expected=%08lx/%08lx size=%llu\n",
                             stage, corner, center, expectedCorner, expectedCenter,
                             static_cast<unsigned long long>(WindowsArtClientSize()));
            return passed ? JNI_TRUE : JNI_FALSE;
        };
        const JNINativeMethod methods[]{
            {const_cast<char*>("changeWindowDpi"), const_cast<char*>("(I)I"),
             reinterpret_cast<void*>(Import(
                 "Win32 DPI probe",
                 +[](JNIEnv*, jclass, jint dpi) -> jint {
                     if (dpi) {
                         RECT bounds{};
                         const auto hwnd = WindowsHostWindow();
                         if (!hwnd || !GetWindowRect(hwnd, &bounds))
                             return 0;
                         SendMessageW(hwnd, WM_DPICHANGED, MAKELONG(dpi, dpi), reinterpret_cast<LPARAM>(&bounds));
                     }
                     return AndroidDisplayDensity();
                 }))},
            {const_cast<char*>("clickWindow"), const_cast<char*>("(II)V"),
             reinterpret_cast<void*>(Import(
                 "Win32 DPI input probe",
                 +[](JNIEnv*, jclass, jint x, jint y) {
                     const auto hwnd = WindowsHostWindow();
                     SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
                     SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
                 }))},
            {const_cast<char*>("resizeWindow"), const_cast<char*>("(III)J"),
             reinterpret_cast<void*>(Import("Win32 resize probe", resize))},
            {const_cast<char*>("windowResizable"), const_cast<char*>("()Z"),
             reinterpret_cast<void*>(Import("Win32 resize style probe", policy))},
            {const_cast<char*>("inspectResizeMask"), const_cast<char*>("(I)Z"),
             reinterpret_cast<void*>(Import("Win32 resize mask pixels probe", mask))},
        };
        STONE_CHECK(probe && ArtJni<&JNINativeInterface_::RegisterNatives>(env, probe, methods, 5) == JNI_OK);
    }
    if (std::string_view(probeClass) == "com/trebledot/stone/runtime/NativeLibrariesProbe") {
        const auto paint = +[](JNIEnv* env, jclass, jobject surface, jint color) -> jboolean {
            auto* window = reinterpret_cast<NativeWindow*>(ArtSurfaceFromJava(env, surface));
            if (!window)
                return JNI_FALSE;
            AndroidNativeWindowBuffer buffer{};
            const int locked = Guest<int (*)(void*, void*, void*)>(Libandroid("ANativeWindow_lock"),
                                                                   "(LLL)I")(window, &buffer, nullptr);
            if (locked) {
                ReleaseWindow(window);
                return JNI_FALSE;
            }
            auto* pixels = static_cast<uint8_t*>(buffer.bits);
            for (int y = 0; y < buffer.height; ++y)
                for (int x = 0; x < buffer.width; ++x) {
                    auto* p = pixels + (static_cast<size_t>(y) * buffer.stride + x) * 4;
                    p[0] = static_cast<uint8_t>(color >> 16);
                    p[1] = static_cast<uint8_t>(color >> 8);
                    p[2] = static_cast<uint8_t>(color);
                    p[3] = static_cast<uint8_t>(static_cast<uint32_t>(color) >> 24);
                }
            const int posted = Guest<int (*)(void*)>(Libandroid("ANativeWindow_unlockAndPost"), "(L)I")(window);
            ReleaseWindow(window);
            return posted == 0 ? JNI_TRUE : JNI_FALSE;
        };
        const JNINativeMethod method{const_cast<char*>("paintSurface"), const_cast<char*>("(Landroid/view/Surface;I)Z"),
                                     reinterpret_cast<void*>(Import("SurfaceView native pixel probe", paint))};
        STONE_CHECK(probe && ArtJni<&JNINativeInterface_::RegisterNatives>(env, probe, &method, 1) == JNI_OK);
    }
    if (std::string_view(probeClass) == "com/trebledot/stone/probe/BatterySample") {
        const auto battery = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/runtime/ArtBattery");
        const auto query =
            battery ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, battery, "queryHostState", "()[I") : nullptr;
        const auto host = query ? static_cast<jintArray>(ArtJni<&JNINativeInterface_::CallStaticObjectMethodA>(
                                      env, battery, query, nullptr))
                                : nullptr;
        STONE_CHECK(host);
        ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, host, 0, 4, batteryHost.data());
        SetBatteryFixture(env, nullptr, host);
        // Override only this isolated probe process. Production always queries
        // GetSystemPowerStatus; fixtures never change the host's power settings.
        const JNINativeMethod queryBinding{const_cast<char*>("queryHostState"), const_cast<char*>("()[I"),
                                           reinterpret_cast<void*>(Import("battery fixture", &BatteryFixture))};
        STONE_CHECK(ArtJni<&JNINativeInterface_::RegisterNatives>(env, battery, &queryBinding, 1) == JNI_OK);
        const auto hostState = +[](JNIEnv* env, jclass) -> jintArray {
            const auto result = ArtJni<&JNINativeInterface_::NewIntArray>(env, 4);
            if (result)
                ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, result, 0, 4, batteryHost.data());
            return result;
        };
        const JNINativeMethod methods[]{{const_cast<char*>("host"), const_cast<char*>("()[I"),
                                         reinterpret_cast<void*>(Import("Windows battery snapshot", hostState))},
                                        {const_cast<char*>("fixture"), const_cast<char*>("([I)V"),
                                         reinterpret_cast<void*>(Import("battery fixture state", &SetBatteryFixture))}};
        STONE_CHECK(ArtJni<&JNINativeInterface_::RegisterNatives>(env, probe, methods, 2) == JNI_OK);
    }
    if (std::string_view(probeClass) == "com/trebledot/stone/probe/StartupServicesProbe") {
        const auto interval = +[](JNIEnv*, jclass, jint value) -> jboolean {
            const auto display = reinterpret_cast<void* (*)()>(Libegl("eglGetCurrentDisplay", true));
            const auto set = reinterpret_cast<unsigned (*)(void*, int)>(Libegl("eglSwapInterval", true));
            return display && set && set(display(), value) ? JNI_TRUE : JNI_FALSE;
        };
        const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/GlSurfaceProbe");
        const JNINativeMethod binding{const_cast<char*>("swapInterval"), const_cast<char*>("(I)Z"),
                                      reinterpret_cast<void*>(Import("Probe real EGL swap interval", interval))};
        STONE_CHECK(type && ArtJni<&JNINativeInterface_::RegisterNatives>(env, type, &binding, 1) == JNI_OK);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    }
    if (std::string_view(probeClass) == "com/trebledot/stone/probe/JobSample") {
        const auto count = +[](JNIEnv*, jclass) -> jint {
            return static_cast<jint>(ReadClientHostState().systemPowerRequests);
        };
        const JNINativeMethod method{const_cast<char*>("power"), const_cast<char*>("()I"),
                                     reinterpret_cast<void*>(Import("Job system power request count", count))};
        STONE_CHECK(ArtJni<&JNINativeInterface_::RegisterNatives>(env, probe, &method, 1) == JNI_OK);
    }
    if (notificationSample) {
        const auto history = +[](JNIEnv* environment, jclass) -> jobjectArray {
            try {
                const auto values = ReadArtNotificationHistory();
                const auto stringClass = ArtJni<&JNINativeInterface_::FindClass>(environment, "java/lang/String");
                const auto array = ArtJni<&JNINativeInterface_::NewObjectArray>(
                    environment, static_cast<jsize>(values.size()), stringClass, nullptr);
                for (jsize index = 0; array && index < static_cast<jsize>(values.size()); ++index) {
                    const auto value = ArtJni<&JNINativeInterface_::NewString>(
                        environment, reinterpret_cast<const jchar*>(values[index].data()),
                        static_cast<jsize>(values[index].size()));
                    ArtJni<&JNINativeInterface_::SetObjectArrayElement>(environment, array, index, value);
                    ArtJni<&JNINativeInterface_::DeleteLocalRef>(environment, value);
                }
                return array;
            }
            catch (...) {
                return nullptr;
            }
        };
        const JNINativeMethod binding{const_cast<char*>("history"), const_cast<char*>("()[Ljava/lang/String;"),
                                      reinterpret_cast<void*>(Import("Stone notification history", history))};
        const auto historyType =
            ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/NotificationHistoryProbe");
        STONE_CHECK(historyType &&
                    ArtJni<&JNINativeInterface_::RegisterNatives>(env, historyType, &binding, 1) == JNI_OK);
    }
    const auto run = probe ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(
                                 env, probe, "run", "(Landroid/content/res/AssetManager;Ljava/lang/String;)I")
                           : nullptr;
    const auto name = ArtJni<&JNINativeInterface_::NewString>(env, reinterpret_cast<const jchar*>(argument.data()),
                                                              static_cast<jsize>(argument.size()));
    const jvalue args[]{jvalue{.l = assets}, jvalue{.l = name}};
    const int result =
        assets && prepare && run ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, probe, run, args) : -1;
    const bool failed = ArtJni<&JNINativeInterface_::ExceptionCheck>(env);
    if (failed)
        ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
    const auto jitStats = GetArtJitStatistics();
    // instructions counts x64 GS rewrites; ARM64 JIT publications have no GS.
    const bool jitPublished =
        jitStats.methods && (runtime->Architecture() == ElfImage::Machine::Arm64 || jitStats.instructions);
    const bool passed =
        !failed && result == 42 && GetModuleHandleW(L"jvm.dll") == nullptr && (!runtime->JitEnabled() || jitPublished);
    std::cout << "ART APK return=" << result << " exception=" << failed << '\n';
    if (runtime->JitEnabled())
        std::cout << "ART JIT publication: methods=" << jitStats.methods << " GS=" << jitStats.instructions
                  << " TLS-recoveries=" << jitStats.tlsRecoveries << " breakpoints=" << jitStats.breakpoints << '\n';
    if (GetEnvironmentVariableW(L"STONE_PROBE_PERFORMANCE_FAULTS", nullptr, 0))
        std::fprintf(stderr, "PERF faults total: breakpoints=%llu low-address-reads=%llu other=%llu\n",
                     static_cast<unsigned long long>(performanceFaults[0].load()),
                     static_cast<unsigned long long>(performanceFaults[1].load()),
                     static_cast<unsigned long long>(performanceFaults[2].load()));
    if (notificationSample)
        CleanupArtNotificationTestIdentity();
    std::cout << "ART APK sample " << probeClass << ' ' << (passed ? "passed without a host JVM." : "failed.") << '\n'
              << std::flush;
    std::cerr << std::flush;
    StopClientApplicationLogs();
    if (std::string_view(probeClass) == "com/trebledot/stone/probe/AudioOutputProbe") {
        // The isolated probe owns this ART instance until ExitProcess below.
        if (!passed)
            throw std::runtime_error("AudioTrack image probe failed");
        return;
    }
    ExitProcess(passed ? 0 : 1);
}

void TestArtServiceSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                          const std::filesystem::path& apk, const std::filesystem::path& artHost,
                          std::wstring_view service)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/ServiceSample", L"service-sample-data",
                    service);
}

void TestArtNativeLibrariesSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                                  const std::filesystem::path& apk, const std::filesystem::path& artHost)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/runtime/NativeLibrariesProbe",
                    L"native-library-sample-data", L"");
}

void TestArtResizeWindowSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                               const std::filesystem::path& apk, const std::filesystem::path& artHost)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/runtime/ResizeWindowProbe",
                    L"resize-window-sample-data", L"");
}

void TestArtWebViewProviderSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                                  const std::filesystem::path& apk, const std::filesystem::path& artHost)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/runtime/WebViewProviderProbe",
                    L"webview-provider-sample-data", L"");
}

void TestArtStartupServicesSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                                  const std::filesystem::path& apk, const std::filesystem::path& artHost,
                                  bool nativeOnly, bool videoCodecs)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/StartupServicesProbe",
                    L"startup-services-sample-data",
                    videoCodecs  ? L"video"
                    : nativeOnly ? L"native"
                                 : L"");
}

void TestArtAudioOutputSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                              const std::filesystem::path& apk, const std::filesystem::path& artHost, bool codecs)
{
    WithAudioLoopback(
        [&] {
            RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/AudioOutputProbe",
                            L"audio-output-sample-data", codecs ? L"codecs" : L"");
        },
        dex.parent_path() / L"audio-loopback.wav");
    std::cout << std::flush;
    ExitProcess(0);
}

void TestArtAutomaticUpdateSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                                  const std::filesystem::path& apk, const std::filesystem::path& artHost,
                                  std::wstring_view scenario)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/AutomaticUpdateSample",
                    L"automatic-update-sample-data", scenario);
}

void TestArtBatterySample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                          const std::filesystem::path& apk, const std::filesystem::path& artHost)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/BatterySample", L"battery-sample-data",
                    L"");
}

void TestArtStorageSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                          const std::filesystem::path& apk, const std::filesystem::path& artHost,
                          std::wstring_view stage)
{
    if (stage != L"write" && stage != L"read" && stage != L"private-write" && stage != L"private-read")
        throw std::invalid_argument("Storage probe stage must be write, read, private-write or private-read");
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/StorageSample", L"storage-sample-data",
                    stage);
}

void TestArtJobSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                      const std::filesystem::path& apk, const std::filesystem::path& artHost, std::wstring_view stage)
{
    if (stage != L"fixture" && stage != L"recover")
        throw std::invalid_argument("Job probe stage must be fixture or recover");
    EnableClientJobs();
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/JobSample", L"job-sample-data", stage);
}

void TestArtForegroundSample(const std::filesystem::path& directory, const std::filesystem::path& dex,
                             const std::filesystem::path& apk, const std::filesystem::path& artHost,
                             std::wstring_view expectation)
{
    RunArtAppSample(directory, dex, apk, artHost, "com/trebledot/stone/probe/ForegroundSample",
                    L"foreground-sample-data", expectation);
}

void TestArtRuntime(const std::filesystem::path& directory, const std::filesystem::path& dex,
                    const std::filesystem::path& citApk, bool graphics, bool resources,
                    const std::filesystem::path& artHost)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    namespace fs = std::filesystem;
    const auto root = fs::canonical(directory);
    const auto app = fs::canonical(dex);
    wchar_t externalData[32768]{};
    const auto externalLength = GetEnvironmentVariableW(L"STONE_ART_DATA_DIRECTORY", externalData, 32768);
    STONE_CHECK(externalLength < 32768);
    const auto data = externalLength ? fs::absolute(externalData) : root / "data";
    CopyReadOnlyCode(app, data / "probe.dex");
    STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
    ArtRuntimeOptions options{root, "/data/probe.dex", {}, {}};
    options.dataDirectory = data;
    wchar_t binderProbe[2]{};
    const bool onlyBinder =
        GetEnvironmentVariableW(L"STONE_ART_PROBE_BINDER", binderProbe, 2) == 1 && binderProbe[0] == L'1';
    wchar_t rasterProbe[2]{};
    const bool onlyRaster =
        GetEnvironmentVariableW(L"STONE_ART_PROBE_RASTER", rasterProbe, 2) == 1 && rasterProbe[0] == L'1';
    wchar_t inputProbe[2]{};
    const bool onlyInput =
        GetEnvironmentVariableW(L"STONE_ART_PROBE_INPUT", inputProbe, 2) == 1 && inputProbe[0] == L'1';
    wchar_t priorityProbe[2]{};
    const bool onlyPriority =
        GetEnvironmentVariableW(L"STONE_ART_PROBE_PRIORITY", priorityProbe, 2) == 1 && priorityProbe[0] == L'1';
    wchar_t dnsProbe[2]{};
    const bool onlyDns = GetEnvironmentVariableW(L"STONE_ART_PROBE_DNS", dnsProbe, 2) == 1 && dnsProbe[0] == L'1';
    wchar_t signalProbe[2]{}, killProbe[2]{};
    const bool onlySignal =
        GetEnvironmentVariableW(L"STONE_ART_PROBE_SELF_SIGNAL", signalProbe, 2) == 1 && signalProbe[0] == L'1';
    if (onlyBinder || onlyRaster || onlyInput || onlyPriority || onlySignal || onlyDns)
        options.bootClassPath = {"/system/framework/framework.jar", "/system/framework/framework-graphics.jar"};
    if (onlySignal && GetEnvironmentVariableW(L"STONE_ART_PROBE_SELF_KILL", killProbe, 2) == 1 && killProbe[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.self-kill=true");
    wchar_t windowProbe[2]{};
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_WINDOW", windowProbe, 2) == 1 && windowProbe[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.window=true");
    wchar_t uninstallProbe[2]{};
    wchar_t uncaughtWorker[2]{};
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_UNCAUGHT_WORKER", uncaughtWorker, 2) == 1 &&
        uncaughtWorker[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.uncaught-worker=true");
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_ONLY_UNINSTALL", uninstallProbe, 2) == 1 && uninstallProbe[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.only-uninstall=true");
    wchar_t launcherProbe[2]{};
    wchar_t filesystemProbe[2]{};
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_ONLY_FILESYSTEM", filesystemProbe, 2) == 1 &&
        filesystemProbe[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.only-filesystem=true");
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_ONLY_LAUNCHERS", launcherProbe, 2) == 1 && launcherProbe[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.only-launchers=true");
    wchar_t packageProbe[2]{};
    const bool onlyPackages =
        GetEnvironmentVariableW(L"STONE_ART_PROBE_ONLY_PACKAGES", packageProbe, 2) == 1 && packageProbe[0] == L'1';
    if (onlyPackages)
        options.vmOptions.push_back("-Dstone.probe.only-packages=true");
    wchar_t installSourceProbe[2]{};
    const bool onlyInstallSources =
        onlyPackages && GetEnvironmentVariableW(L"STONE_ART_PROBE_ONLY_INSTALL_SOURCES", installSourceProbe, 2) == 1 &&
        installSourceProbe[0] == L'1';
    if (onlyInstallSources)
        options.vmOptions.push_back("-Dstone.probe.only-install-sources=true");
    wchar_t switchProbe[2]{}, localeBenchmark[2]{};
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_SWITCH_INTERPRETER", switchProbe, 2) == 1 && switchProbe[0] == L'1')
        options.vmOptions.push_back("-Xint");
    wchar_t jitProbe[2]{};
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_JIT", jitProbe, 2) == 1 && jitProbe[0] == L'1') {
        // The core probe additionally waits for known compiled methods and
        // checks JNI, switch tables, managed threads, GC and catch handlers.
        options.useJit = true;
        options.vmOptions.push_back("-Dstone.probe.jit=true");
        options.vmOptions.push_back("-verbose:jit");
        ConfigureProbeJitThresholds(options);
        AddVectoredExceptionHandler(
            0, +[](EXCEPTION_POINTERS* exception) -> LONG {
                if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
                    return EXCEPTION_CONTINUE_SEARCH;
                const auto& context = *exception->ContextRecord;
                std::fprintf(stderr, "[jit-probe] terminal RIP=0x%llx RSP=0x%llx\n", context.Rip, context.Rsp);
                uintptr_t stack[4]{};
                SIZE_T read = 0;
                uint8_t instruction[16]{};
                if (AndroidExecutableMapping(context.Rip, sizeof(instruction)) &&
                    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rip), instruction,
                                      sizeof(instruction), &read)) {
                    std::fprintf(stderr, "[jit-probe] dynamic instruction bytes:");
                    for (const auto byte : instruction)
                        std::fprintf(stderr, " %02x", byte);
                    std::fputc('\n', stderr);
                }
                if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), stack, sizeof(stack),
                                      &read)) {
                    for (const auto pc : stack) {
                        std::fprintf(stderr, "[jit-probe] stack value=0x%llx\n", static_cast<unsigned long long>(pc));
                        uint8_t bytes[16]{};
                        MEMORY_BASIC_INFORMATION memory{};
                        if (pc < sizeof(bytes) ||
                            !VirtualQuery(reinterpret_cast<void*>(pc - sizeof(bytes)), &memory, sizeof(memory)) ||
                            memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) ||
                            !(memory.Protect &
                              (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
                            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pc - sizeof(bytes)), bytes,
                                               sizeof(bytes), &read))
                            continue;
                        std::fprintf(stderr, "[jit-probe] executable return PC=0x%llx preceding bytes:",
                                     static_cast<unsigned long long>(pc));
                        for (const auto byte : bytes)
                            std::fprintf(stderr, " %02x", byte);
                        std::fputc('\n', stderr);
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            });
    }
    if (GetEnvironmentVariableW(L"STONE_ART_PROBE_LOCALE_BENCHMARK", localeBenchmark, 2) == 1 &&
        localeBenchmark[0] == L'1')
        options.vmOptions.push_back("-Dstone.probe.locale-benchmark=true");
    if (!citApk.empty()) {
        CopyReadOnlyCode(fs::canonical(citApk), data / "application.apk");
        options.classPath += ":/data/application.apk";
        if (artHost.empty())
            options.bootClassPath = {"/system/framework/framework.jar", "/system/framework/framework-graphics.jar"};
    }
    if (!artHost.empty()) {
        options.useImageBootClassPath = true;
        CopyReadOnlyCode(fs::canonical(artHost), data / "stone-art-runtime.jar");
        options.bootClassPath.push_back("/data/stone-art-runtime.jar");
        wchar_t archiveFixtures[32768]{};
        const auto archiveLength = GetEnvironmentVariableW(L"STONE_ART_PROBE_ARCHIVES", archiveFixtures, 32768);
        STONE_CHECK(archiveLength < 32768);
        if (archiveLength) {
            const auto fixtures = fs::canonical(archiveFixtures);
            STONE_CHECK(fs::is_regular_file(fixtures / "expected.properties"));
            for (const auto& item : fs::directory_iterator(fixtures)) {
                if (item.is_regular_file() &&
                    (item.path().extension() == ".apk" || item.path().filename() == "expected.properties"))
                    CopyReadOnlyCode(item.path(), data / "archive-fixtures" / item.path().filename());
            }
            options.vmOptions.push_back("-Dstone.probe.archives=/data/archive-fixtures");
        }
    }
    ArtRuntime runtime(options);
    auto* env = runtime.GuestEnvironment();
    STONE_CHECK(env && GetModuleHandleW(L"jvm.dll") == nullptr);
    if (onlyBinder) {
        STONE_CHECK(RegisterArtFramework(env));
        const auto binder = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/Binder");
        const auto holder =
            binder ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, binder, "getNativeBBinderHolder", "()J")
                   : nullptr;
        const auto finalizer =
            binder ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, binder, "getNativeFinalizer", "()J")
                   : nullptr;
        const auto registry = ArtJni<&JNINativeInterface_::FindClass>(env, "libcore/util/NativeAllocationRegistry");
        const auto release =
            registry ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, registry, "applyFreeFunction", "(JJ)V")
                     : nullptr;
        STONE_CHECK(holder && finalizer && release && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        const auto destroy = ArtJni<&JNINativeInterface_::CallStaticLongMethodA>(env, binder, finalizer, nullptr);
        const auto first = ArtJni<&JNINativeInterface_::CallStaticLongMethodA>(env, binder, holder, nullptr);
        const auto second = ArtJni<&JNINativeInterface_::CallStaticLongMethodA>(env, binder, holder, nullptr);
        STONE_CHECK(destroy && first && second && first != second &&
                    !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        for (const auto value : {first, second}) {
            const jvalue arguments[]{jvalue{.j = destroy}, jvalue{.j = value}};
            ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, registry, release, arguments);
            STONE_CHECK(!ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        }
        const auto constructor = ArtJni<&JNINativeInterface_::GetMethodID>(env, binder, "<init>", "()V");
        const auto alive = ArtJni<&JNINativeInterface_::GetMethodID>(env, binder, "isBinderAlive", "()Z");
        STONE_CHECK(constructor && alive && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        for (unsigned i = 0; i < 256; ++i) {
            const auto instance = ArtJni<&JNINativeInterface_::NewObjectA>(env, binder, constructor, nullptr);
            STONE_CHECK(instance && ArtJni<&JNINativeInterface_::CallBooleanMethodA>(env, instance, alive, nullptr) &&
                        !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, instance);
        }
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, binder);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, registry);
        std::cout
            << "PASS image Binder critical JNI holder allocation, NativeAllocationRegistry release and Java construction.\n"
            << std::flush;
        STONE_CHECK(runtime.Shutdown() == JNI_OK);
        return;
    }
    if (onlyDns) {
        STONE_CHECK(RegisterArtFramework(env));
        const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Framework");
        const auto entry =
            type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "networkResolver", "()I") : nullptr;
        const auto result = entry ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, entry, nullptr) : -1;
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(result == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        STONE_CHECK(runtime.Shutdown() == JNI_OK && GetModuleHandleW(L"jvm.dll") == nullptr);
        return;
    }
    if (onlySignal) {
        STONE_CHECK(RegisterArtFramework(env));
        const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Framework");
        const auto entry =
            type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "selfSignals", "()I") : nullptr;
        const auto result = entry ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, entry, nullptr) : -1;
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(result == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        STONE_CHECK(runtime.Shutdown() == JNI_OK && GetModuleHandleW(L"jvm.dll") == nullptr);
        return;
    }
    if (onlyPriority) {
        STONE_CHECK(RegisterArtFramework(env));
        const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Framework");
        const auto entry =
            type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "threadPriorities", "()I") : nullptr;
        const auto result = entry ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, entry, nullptr) : -1;
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(result == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        if (options.useJit) {
            const auto stats = GetArtJitStatistics();
            STONE_CHECK(stats.methods);
            if (IsArm64TranslationEnabled())
                STONE_CHECK(!stats.instructions && !stats.tlsRecoveries && !stats.breakpoints);
            std::cout << "ART priority JIT publication: methods=" << stats.methods << " GS=" << stats.instructions
                      << " TLS-recoveries=" << stats.tlsRecoveries << " breakpoints=" << stats.breakpoints << '\n';
        }
        STONE_CHECK(runtime.Shutdown() == JNI_OK && GetModuleHandleW(L"jvm.dll") == nullptr);
        return;
    }
    if (onlyInput) {
        STONE_CHECK(RegisterArtFramework(env) && RegisterArtGraphics(env));
        for (const auto* name : {"register_android_view_KeyCharacterMap", "register_android_view_KeyEvent",
                                 "register_android_view_MotionEvent", "register_android_view_VelocityTracker"}) {
            const auto symbol = "_ZN7android" + std::to_string(std::strlen(name)) + name + "EP7_JNIEnv";
            const auto address = AndroidRuntimeFunction("libandroid_runtime.so", symbol);
            STONE_CHECK(reinterpret_cast<int (*)(JNIEnv*)>(MakeNativeAndroidThunk(address, "(L)I"))(env) >= 0 &&
                        !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        }
        const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Framework");
        const auto entry =
            type ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "inputEvents", "()I") : nullptr;
        const auto result = entry ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, entry, nullptr) : -1;
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(result == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
        if (options.useJit) {
            const auto stats = GetArtJitStatistics();
            STONE_CHECK(stats.methods);
            if (IsArm64TranslationEnabled())
                STONE_CHECK(!stats.instructions && !stats.tlsRecoveries && !stats.breakpoints);
            std::cout << "ART input JIT publication: methods=" << stats.methods << " GS=" << stats.instructions
                      << " TLS-recoveries=" << stats.tlsRecoveries << " breakpoints=" << stats.breakpoints << '\n';
        }
        STONE_CHECK(runtime.Shutdown() == JNI_OK && GetModuleHandleW(L"jvm.dll") == nullptr);
        return;
    }
    if (onlyRaster) {
        STONE_CHECK(RegisterArtFramework(env) && RegisterArtGraphics(env));
        const auto drawing = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Graphics");
        const auto draw =
            drawing ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, drawing, "raster", "()I") : nullptr;
        const auto result = draw ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, drawing, draw, nullptr) : -1;
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(result == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, drawing);
        if (options.useJit) {
            const auto stats = GetArtJitStatistics();
            STONE_CHECK(stats.methods);
            if (IsArm64TranslationEnabled())
                STONE_CHECK(!stats.instructions && !stats.tlsRecoveries && !stats.breakpoints);
            std::cout << "ART raster JIT publication: methods=" << stats.methods << " GS=" << stats.instructions
                      << " TLS-recoveries=" << stats.tlsRecoveries << " breakpoints=" << stats.breakpoints << '\n';
        }
        STONE_CHECK(runtime.Shutdown() == JNI_OK && GetModuleHandleW(L"jvm.dll") == nullptr);
        return;
    }
    std::cout << "ART JNI_CreateJavaVM completed without a host JVM.\n" << std::flush;
    const auto math = DlOpen("libm.so", 2);
    STONE_CHECK(math);
    const auto imageSqrt = DlSym(math, "sqrt");
    const auto defaultSqrt = DlSym(nullptr, "sqrt");
    STONE_CHECK(imageSqrt && defaultSqrt == imageSqrt);
    STONE_CHECK(reinterpret_cast<uintptr_t>(imageSqrt) != Libm("sqrt"));
    const auto squareRoot =
        reinterpret_cast<double (*)(double)>(MakeNativeAndroidThunk(reinterpret_cast<uintptr_t>(imageSqrt), "(D)D"));
    STONE_CHECK(squareRoot(81.0) == 9.0 && DlClose(math) == 0);
    VerifyImageCallbacks();
    VerifyImageSignalChain();
    const auto type = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Main");
    STONE_CHECK(type && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
    const auto stringType = ArtJni<&JNINativeInterface_::FindClass>(env, "java/lang/String");
    const auto lengthMethod =
        stringType ? ArtJni<&JNINativeInterface_::GetMethodID>(env, stringType, "length", "()I") : nullptr;
    constexpr char utfInput[] = "Stone-\xe7\x9f\xb3\xe5\xa4\xb4";
    for (unsigned i = 0; i < 3; ++i) {
        const auto text = ArtJni<&JNINativeInterface_::NewStringUTF>(env, utfInput);
        STONE_CHECK(text && lengthMethod && ArtJni<&JNINativeInterface_::GetStringLength>(env, text) == 8 &&
                    ArtJni<&JNINativeInterface_::CallIntMethodA>(env, text, lengthMethod, nullptr) == 8);
        const auto characters = ArtJni<&JNINativeInterface_::GetStringUTFChars>(env, text, nullptr);
        STONE_CHECK(characters && std::strcmp(characters, utfInput) == 0);
        ArtJni<&JNINativeInterface_::ReleaseStringUTFChars>(env, text, characters);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
    }
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, stringType);
    STONE_CHECK(!ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
    std::cout << "PASS JNI class/name/signature inputs and repeated modified-UTF-8 string round trips.\n" << std::flush;
    const auto twice = +[](JNIEnv*, jclass, jint value) -> jint {
        if (value < 0)
            throw std::runtime_error("negative host JNI probe");
        return value * 2;
    };
    const auto jitReady = +[](JNIEnv*, jclass) -> jboolean {
        return ArtJitMethodCompiled("com.trebledot.stone.probe.Main.hot") &&
                       ArtJitMethodCompiled("com.trebledot.stone.probe.Main.switchHot") &&
                       ArtJitMethodCompiled("com.trebledot.stone.probe.Main.catchHot")
                   ? JNI_TRUE
                   : JNI_FALSE;
    };
    const JNINativeMethod natives[]{{const_cast<char*>("twice"), const_cast<char*>("(I)I"),
                                     reinterpret_cast<void*>(ImportAndroidScalars<decltype(twice)>(
                                         "Stone ART probe twice", reinterpret_cast<uintptr_t>(twice)))},
                                    {const_cast<char*>("jitReady"), const_cast<char*>("()Z"),
                                     reinterpret_cast<void*>(ImportAndroidScalars<decltype(jitReady)>(
                                         "Stone ART probe JIT method ready", reinterpret_cast<uintptr_t>(jitReady)))}};
    const bool bound = type && ArtJni<&JNINativeInterface_::RegisterNatives>(
                                   env, type, natives, static_cast<jint>(std::size(natives))) == JNI_OK;
    if (bound && IsArm64TranslationEnabled()) {
        const auto native = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "twice", "(I)I");
        jvalue input[]{jvalue{.i = 21}};
        STONE_CHECK(native && ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, native, input) == 42);
        input[0].i = -1;
        ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, native, input);
        STONE_CHECK(ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::ExceptionClear>(env);
        input[0].i = 21;
        STONE_CHECK(ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, native, input) == 42);
        const auto array = ArtJni<&JNINativeInterface_::NewIntArray>(env, 3);
        const jint numbers[]{12, 14, 16};
        ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, array, 0, 3, numbers);

        struct {
            jint before;
            jint values[3];
            jint after;
        } copied{81, {}, 83};

        ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, array, 0, 3, copied.values);
        STONE_CHECK(copied.before == 81 && copied.after == 83 &&
                    std::equal(std::begin(numbers), std::end(numbers), copied.values));
        ArtJni<&JNINativeInterface_::GetIntArrayRegion>(env, array, 2, 2, copied.values);
        STONE_CHECK(ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::ExceptionClear>(env);
        STONE_CHECK(copied.before == 81 && copied.after == 83 &&
                    std::equal(std::begin(numbers), std::end(numbers), copied.values));
        const auto sum = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "sum", "([I)I");
        const jvalue values[]{jvalue{.l = array}};
        STONE_CHECK(sum && ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, sum, values) == 42);
        constexpr jchar unicode[]{'S', 0x77f3, 0x5934, 0xd83e, 0xdd16};
        const auto text = ArtJni<&JNINativeInterface_::NewString>(env, unicode, 5);

        struct {
            jchar before;
            jchar values[5];
            jchar after;
        } output{87, {}, 89};

        ArtJni<&JNINativeInterface_::GetStringRegion>(env, text, 0, 5, output.values);
        STONE_CHECK(output.before == 87 && output.after == 89 &&
                    std::equal(std::begin(unicode), std::end(unicode), output.values));

        struct {
            jboolean before, value, after;
        } copiedFlag{91, 0x7f, 93};

        const auto chars = ArtJni<&JNINativeInterface_::GetStringChars>(env, text, &copiedFlag.value);
        STONE_CHECK(chars && copiedFlag.before == 91 && copiedFlag.after == 93 && copiedFlag.value <= JNI_TRUE &&
                    std::equal(std::begin(unicode), std::end(unicode), chars));
        ArtJni<&JNINativeInterface_::ReleaseStringChars>(env, text, chars);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, array);
        STONE_CHECK(!ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        std::cout
            << "PASS ARM64 registered host JNI, native exception recovery, jvalue arrays, regions and UTF-16 buffers.\n"
            << std::flush;
    }
    const auto entry = bound ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "run", "()I") : nullptr;
    const int result = entry ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, entry, nullptr) : -1;
    if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
        ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
    STONE_CHECK(entry && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env) && result == 42);
    const jint numbers[]{12, 14, 16};
    const auto array = ArtJni<&JNINativeInterface_::NewIntArray>(env, 3);
    STONE_CHECK(array);
    ArtJni<&JNINativeInterface_::SetIntArrayRegion>(env, array, 0, 3, numbers);
    const auto sum = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "sum", "([I)I");
    const jvalue arguments[]{jvalue{.l = array}};
    STONE_CHECK(sum && ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, type, sum, arguments) == 42);
    const auto fraction = ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, type, "fraction", "()D");
    STONE_CHECK(fraction &&
                ArtJni<&JNINativeInterface_::CallStaticDoubleMethodA>(env, type, fraction, nullptr) == 42.5);
    constexpr jchar characters[]{'S', 't', 'o', 'n', 'e', 0xd83e, 0xdd16};
    const auto text = ArtJni<&JNINativeInterface_::NewString>(env, characters, 7);
    STONE_CHECK(text && ArtJni<&JNINativeInterface_::GetStringLength>(env, text) == 7);
    jchar copied[7]{};
    ArtJni<&JNINativeInterface_::GetStringRegion>(env, text, 0, 7, copied);
    STONE_CHECK(std::equal(std::begin(characters), std::end(characters), std::begin(copied)));
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, text);
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, array);
    ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, type);
    STONE_CHECK(!ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
    if (!citApk.empty()) {
        const auto activity = ArtJni<&JNINativeInterface_::FindClass>(env, "android/app/Activity");
        STONE_CHECK(RegisterArtFramework(env));
        const auto looper = ArtJni<&JNINativeInterface_::FindClass>(env, "android/os/Looper");
        const auto prepare =
            looper ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, looper, "prepareMainLooper", "()V") : nullptr;
        if (prepare)
            ArtJni<&JNINativeInterface_::CallStaticVoidMethodA>(env, looper, prepare, nullptr);
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(prepare && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, looper);
        const auto cit = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/cit/CitActivity");
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(activity && cit && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        STONE_CHECK(ArtJni<&JNINativeInterface_::IsAssignableFrom>(env, cit, activity));
        const auto constructor = ArtJni<&JNINativeInterface_::GetMethodID>(env, cit, "<init>", "()V");
        const auto instance =
            constructor ? ArtJni<&JNINativeInterface_::NewObjectA>(env, cit, constructor, nullptr) : nullptr;
        if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
            ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
        STONE_CHECK(instance && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, instance);
        // Package transactions do not depend on the network probes in Framework.run.
        if (!onlyPackages) {
            const auto framework = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Framework");
            const auto frameworkRun =
                framework ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, framework, "run", "()I") : nullptr;
            const auto frameworkResult =
                frameworkRun ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, framework, frameworkRun, nullptr)
                             : -1;
            if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
            STONE_CHECK(frameworkResult == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, framework);
            std::cout << "ART dispatched a framework Handler message.\n";
        }
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, cit);
        ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, activity);
        std::cout << "ART created Stone CIT Activity.\n";
        if (resources || !artHost.empty()) {
            std::cout << "Registering image Android resources.\n" << std::flush;
            try {
                STONE_CHECK(RegisterArtResources(env));
            }
            catch (const std::exception& error) {
                std::cerr << "ART resource registration: " << error.what() << '\n' << std::flush;
                throw;
            }
            const auto ndkResources =
                ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Resources");
            const JNINativeMethod resourceNative{
                const_cast<char*>("verifyAssets"), const_cast<char*>("(Landroid/content/res/AssetManager;[BIII)Z"),
                reinterpret_cast<void*>(ImportAndroidScalars<decltype(&VerifyAssets)>(
                    "Stone image NDK asset and configuration probe", reinterpret_cast<uintptr_t>(&VerifyAssets)))};
            STONE_CHECK(ndkResources &&
                        ArtJni<&JNINativeInterface_::RegisterNatives>(env, ndkResources, &resourceNative, 1) == JNI_OK);
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, ndkResources);
            if (!artHost.empty()) {
                STONE_CHECK(RegisterArtGraphics(env));
                STONE_CHECK(RegisterArtHost(env));
            }
            const auto assetType = ArtJni<&JNINativeInterface_::FindClass>(env, "android/content/res/AssetManager");
            const auto assetConstructor =
                assetType ? ArtJni<&JNINativeInterface_::GetMethodID>(env, assetType, "<init>", "()V") : nullptr;
            const auto assets =
                assetConstructor ? ArtJni<&JNINativeInterface_::NewObjectA>(env, assetType, assetConstructor, nullptr)
                                 : nullptr;
            const auto resourcesType = ArtJni<&JNINativeInterface_::FindClass>(
                env, artHost.empty() ? "com/trebledot/stone/probe/Resources" : "com/trebledot/stone/probe/Application");
            if (!artHost.empty()) {
                const auto inject =
                    +[](JNIEnv*, jclass, jint x, jint y, jint width, jint height, jint cancel) -> jboolean {
                    const auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                    struct Restore {
                        DPI_AWARENESS_CONTEXT value;
                        ~Restore()
                        {
                            if (value)
                                SetThreadDpiAwarenessContext(value);
                        }
                    } restore{previous};
                    const auto hwnd = WindowsHostWindow();
                    RECT client{};
                    if (!hwnd || width <= 0 || height <= 0 || !GetClientRect(hwnd, &client))
                        return JNI_FALSE;
                    DWORD_PTR response = 0;
                    if (GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId() ||
                        !SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 250, &response)) {
                        std::cerr << "Win32 window did not respond independently of the ART main thread\n";
                        return JNI_FALSE;
                    }
                    int fittedWidth = client.right, fittedHeight = client.bottom;
                    if (int64_t(fittedWidth) * height > int64_t(fittedHeight) * width)
                        fittedWidth = MulDiv(fittedHeight, width, height);
                    else
                        fittedHeight = MulDiv(fittedWidth, height, width);
                    const int clientX = (client.right - fittedWidth) / 2 + MulDiv(x, fittedWidth, width);
                    const int clientY = (client.bottom - fittedHeight) / 2 + MulDiv(y, fittedHeight, height);
                    const auto position = MAKELPARAM(clientX, clientY);
                    return RunWindowCheck(
                               hwnd,
                               [=] {
                                   SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, position);
                                   if (GetCapture() != hwnd) {
                                       std::cerr << "Synthetic ART mouse down did not capture the window at " << clientX
                                                 << ',' << clientY << '\n';
                                       return false;
                                   }
                                   auto releasePosition = position;
                                   if (cancel == 2) {
                                       for (int step = 1; step <= 4; ++step) {
                                           releasePosition =
                                               MAKELPARAM(clientX, clientY - MulDiv(step * 24, fittedHeight, height));
                                           SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, releasePosition);
                                       }
                                   }
                                   SendMessageW(hwnd, cancel == 1 ? WM_CANCELMODE : WM_LBUTTONUP, 0, releasePosition);
                                   return true;
                               })
                               ? JNI_TRUE
                               : JNI_FALSE;
                };
                const auto close = +[](JNIEnv*, jclass) -> jboolean {
                    const auto hwnd = WindowsHostWindow();
                    return hwnd && PostMessageW(hwnd, WM_CLOSE, 0, 0) ? JNI_TRUE : JNI_FALSE;
                };
                const auto injectText = +[](JNIEnv*, jclass, jint stage) -> jboolean {
                    const auto hwnd = WindowsHostWindow();
                    return RunWindowCheck(hwnd,
                                          [=] {
                                              switch (stage) {
                                              case 0:
                                                  for (const auto value :
                                                       {L'a', wchar_t{0xd83e}, wchar_t{0xdd16}, L'\u4e2d', L'\u6587'})
                                                      SendMessageW(hwnd, WM_CHAR, value, 0);
                                                  break;
                                              case 1:
                                                  SendMessageW(hwnd, WM_CHAR, L'x', 0);
                                                  SendMessageW(hwnd, WM_KEYDOWN, VK_LEFT, 0);
                                                  SendMessageW(hwnd, WM_KEYUP, VK_LEFT, 0);
                                                  SendMessageW(hwnd, WM_CHAR, L'y', 0);
                                                  break;
                                              case 2:
                                                  SendMessageW(hwnd, WM_CHAR, 0xd83e, 0);
                                                  break;
                                              case 3:
                                                  SendMessageW(hwnd, WM_CHAR, 0xdd16, 0);
                                                  break;
                                              case 4:
                                                  SendMessageW(hwnd, WM_KILLFOCUS, 0, 0);
                                                  SendMessageW(hwnd, WM_CHAR, 0xdd16, 0);
                                                  SendMessageW(hwnd, WM_SETFOCUS, 0, 0);
                                                  break;
                                              case 5:
                                                  for (const auto value : {L'1', L'2', L'3'})
                                                      SendMessageW(hwnd, WM_CHAR, value, 0);
                                                  break;
                                              default:
                                                  return false;
                                              }
                                              return true;
                                          })
                               ? JNI_TRUE
                               : JNI_FALSE;
                };
                static const auto checkCaption = +[](HWND hwnd, jboolean releaseOutside) -> jboolean {
                    TITLEBARINFO titleBar{sizeof(titleBar)};
                    POINT origin{};
                    if (!hwnd || !GetTitleBarInfo(hwnd, &titleBar) || !ClientToScreen(hwnd, &origin)) {
                        std::cerr << "Could not read the Win32 title bar bounds\n";
                        return JNI_FALSE;
                    }
                    const auto dpi = GetDpiForWindow(hwnd);
                    const LONG dragX = origin.x + MulDiv(64, dpi, USER_DEFAULT_SCREEN_DPI);
                    const LONG top = titleBar.rcTitleBar.top;
                    const LONG bottom = std::min(titleBar.rcTitleBar.bottom, origin.y) - 1;
                    RECT frame{};
                    LONG visibleTop = top;
                    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
                        !IsRectEmpty(&frame))
                        visibleTop = std::min(top, frame.top + GetSystemMetricsForDpi(SM_CYBORDER, dpi));
                    const auto hitAt = [hwnd](LONG screenX, LONG screenY) {
                        return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(screenX, screenY));
                    };
                    LONG edge = origin.x;
                    while (edge < dragX && hitAt(edge, bottom) == HTSYSMENU)
                        ++edge;
                    const LONG x = edge - 1;
                    if (bottom <= top || edge <= origin.x || edge == dragX || hitAt(x, visibleTop) != HTSYSMENU ||
                        hitAt(edge, bottom) != HTCAPTION || hitAt(x, origin.y) != HTCLIENT ||
                        hitAt(dragX, (top + bottom) / 2) != HTCAPTION) {
                        std::cerr << "Unexpected title bar hit test at DPI " << dpi << ": " << edge - origin.x
                                  << "px wide, top " << hitAt(x, visibleTop) << ", bottom " << hitAt(x, bottom)
                                  << ", boundary " << hitAt(edge, bottom) << ", drag "
                                  << hitAt(dragX, (top + bottom) / 2) << "; client " << origin.x << ',' << origin.y
                                  << "; caption " << titleBar.rcTitleBar.left << ',' << top << ','
                                  << titleBar.rcTitleBar.right << ',' << titleBar.rcTitleBar.bottom << "; visible "
                                  << IsWindowVisible(hwnd) << ", minimized " << IsIconic(hwnd) << '\n';
                        return JNI_FALSE;
                    }
                    const auto screen = MAKELPARAM(x, visibleTop);
                    SendMessageW(hwnd, WM_NCMOUSELEAVE, 0, 0);
                    const auto button = GetWindow(hwnd, GW_ENABLEDPOPUP);
                    RECT buttonBounds{};
                    if (!button || button == hwnd || !IsWindowVisible(button) ||
                        !GetWindowRect(button, &buttonBounds) || buttonBounds.top != visibleTop ||
                        buttonBounds.bottom != bottom + 1 || buttonBounds.left != origin.x || buttonBounds.right <= x) {
                        std::cerr << "Caption overlay bounds did not match the back hit target\n";
                        return JNI_FALSE;
                    }
                    if (SendMessageW(button, WM_NCHITTEST, 0, screen) != HTTRANSPARENT ||
                        SendMessageW(button, WM_NCHITTEST, 0, MAKELPARAM(dragX, bottom)) != HTTRANSPARENT) {
                        std::cerr << "Caption overlay intercepted pointer input\n";
                        return JNI_FALSE;
                    }
                    SendMessageW(hwnd, WM_NCMOUSEMOVE, HTSYSMENU, screen);
                    RECT highlightBounds{};
                    if (!IsWindowVisible(button) || !GetWindowRect(button, &highlightBounds) ||
                        !EqualRect(&highlightBounds, &buttonBounds)) {
                        std::cerr << "Hover changed the caption overlay bounds\n";
                        return JNI_FALSE;
                    }
                    SendMessageW(hwnd, WM_NCMOUSEHOVER, HTSYSMENU, screen);
                    HWND tooltip = nullptr;
                    do {
                        tooltip = FindWindowExW(nullptr, tooltip, TOOLTIPS_CLASSW, nullptr);
                    } while (tooltip && GetWindow(tooltip, GW_OWNER) != hwnd);
                    if (!tooltip || !IsWindowVisible(tooltip)) {
                        std::cerr << "Back tooltip did not become visible after hovering\n";
                        return JNI_FALSE;
                    }
                    wchar_t label[80]{};
                    TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
                    tool.hwnd = hwnd;
                    tool.uId = 1;
                    tool.lpszText = label;
                    SendMessageW(tooltip, TTM_GETTEXTW, std::size(label), reinterpret_cast<LPARAM>(&tool));
                    const auto expected =
                        PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? L"\u8fd4\u56de" : L"Back";
                    if (std::wstring_view(label) != expected) {
                        std::cerr << "Back tooltip did not expose the expected label\n";
                        return JNI_FALSE;
                    }
                    SendMessageW(hwnd, WM_NCMOUSEMOVE, HTCAPTION, MAKELPARAM(dragX, bottom));
                    if (IsWindowVisible(tooltip)) {
                        std::cerr << "Back tooltip stayed visible outside the button\n";
                        return JNI_FALSE;
                    }
                    SendMessageW(hwnd, WM_NCMOUSELEAVE, 0, 0);
                    if (!IsWindowVisible(button))
                        return JNI_FALSE;
                    const auto release = MAKELPARAM((releaseOutside ? edge : x) - origin.x, bottom - origin.y);
                    return PostMessageW(hwnd, WM_NCLBUTTONDOWN, HTSYSMENU, screen) &&
                                   PostMessageW(hwnd, WM_LBUTTONUP, 0, release)
                               ? JNI_TRUE
                               : JNI_FALSE;
                };
                const auto clickBack = +[](JNIEnv*, jclass, jboolean releaseOutside) -> jboolean {
                    const auto hwnd = WindowsHostWindow();
                    return RunWindowCheck(hwnd,
                                          [=] {
                                              return checkCaption(hwnd, releaseOutside) == JNI_TRUE;
                                          })
                               ? JNI_TRUE
                               : JNI_FALSE;
                };
                const auto minimized = +[](JNIEnv*, jclass) -> jboolean {
                    const auto hwnd = WindowsHostWindow();
                    if (!hwnd || !IsIconic(hwnd))
                        return JNI_FALSE;
                    const auto button = GetWindow(hwnd, GW_ENABLEDPOPUP);
                    return !button || button == hwnd || !IsWindowVisible(button) ? JNI_TRUE : JNI_FALSE;
                };
                const JNINativeMethod input[]{
                    {const_cast<char*>("injectTouch"), const_cast<char*>("(IIIII)Z"),
                     reinterpret_cast<void*>(ImportAndroidScalars<decltype(inject)>(
                         "Stone ART probe pointer injection", reinterpret_cast<uintptr_t>(inject)))},
                    {const_cast<char*>("injectText"), const_cast<char*>("(I)Z"),
                     reinterpret_cast<void*>(Import("Stone ART probe text input", injectText))},
                    {const_cast<char*>("requestWindowClose"), const_cast<char*>("()Z"),
                     reinterpret_cast<void*>(Import("Stone ART probe window close", close))},
                    {const_cast<char*>("clickTitleBack"), const_cast<char*>("(Z)Z"),
                     reinterpret_cast<void*>(ImportAndroidScalars<decltype(clickBack)>(
                         "Stone ART probe title back click", reinterpret_cast<uintptr_t>(clickBack)))},
                    {const_cast<char*>("isWindowMinimized"), const_cast<char*>("()Z"),
                     reinterpret_cast<void*>(Import("Stone ART probe minimized window", minimized))},
                    {const_cast<char*>("verifyMotion"), const_cast<char*>("(Landroid/view/MotionEvent;I)Z"),
                     reinterpret_cast<void*>(Import("Stone ART probe NDK MotionEvent", &VerifyMotion))},
                };
                STONE_CHECK(resourcesType &&
                            ArtJni<&JNINativeInterface_::RegisterNatives>(
                                env, resourcesType, input, static_cast<jint>(std::size(input))) == JNI_OK);
            }
            const auto resourceRun =
                resourcesType ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, resourcesType, "run",
                                                                                "(Landroid/content/res/AssetManager;)I")
                              : nullptr;
            const jvalue resourceArguments[]{jvalue{.l = assets}};
            const auto resourceResult = assets && resourceRun ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(
                                                                    env, resourcesType, resourceRun, resourceArguments)
                                                              : -1;
            if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
            STONE_CHECK(resourceResult == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, resourcesType);
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, assets);
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, assetType);
            std::cout
                << (artHost.empty()
                        ? "ART resolved framework and APK strings, Unicode and binary XML.\n"
                        : "ART ran the original APK Application and Activity lifecycle, image fonts and dashboard drawing.\n")
                << std::flush;
        }
        if (graphics) {
            std::cout << "Registering image Android graphics.\n" << std::flush;
            try {
                const bool registered = RegisterArtGraphics(env);
                if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                    ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
                STONE_CHECK(registered && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
            }
            catch (const std::exception& error) {
                std::cerr << "ART graphics registration: " << error.what() << '\n' << std::flush;
                throw;
            }
            const auto drawing = ArtJni<&JNINativeInterface_::FindClass>(env, "com/trebledot/stone/probe/Graphics");
            const JNINativeMethod bitmapNatives[]{
                {const_cast<char*>("verifyBitmap"), const_cast<char*>("(Landroid/graphics/Bitmap;)Z"),
                 reinterpret_cast<void*>(Import("Stone image NDK Bitmap probe", &VerifyBitmap))},
                {const_cast<char*>("unsupportedSurface"), const_cast<char*>("(Z)V"),
                 reinterpret_cast<void*>(Import("Stone unsupported ART Surface probe", &UnsupportedSurface))},
            };
            STONE_CHECK(drawing &&
                        ArtJni<&JNINativeInterface_::RegisterNatives>(env, drawing, bitmapNatives, 2) == JNI_OK);
            const auto draw =
                drawing ? ArtJni<&JNINativeInterface_::GetStaticMethodID>(env, drawing, "run", "()I") : nullptr;
            const auto drawn =
                draw ? ArtJni<&JNINativeInterface_::CallStaticIntMethodA>(env, drawing, draw, nullptr) : -1;
            if (ArtJni<&JNINativeInterface_::ExceptionCheck>(env))
                ArtJni<&JNINativeInterface_::ExceptionDescribe>(env);
            STONE_CHECK(drawn == 42 && !ArtJni<&JNINativeInterface_::ExceptionCheck>(env));
            ArtJni<&JNINativeInterface_::DeleteLocalRef>(env, drawing);
            std::cout << "ART rendered and verified Android Bitmap/Canvas pixels.\n";
        }
    }
    std::cout << "ART DEX and JNI probe returned 42.\n";
    if (options.useJit) {
        const auto stats = GetArtJitStatistics();
        STONE_CHECK(stats.methods);
        if (IsArm64TranslationEnabled()) {
            // ARM64 publication is interpreted directly; x64 GS adapters and
            // Windows x64 fault recovery must never be used for these methods.
            STONE_CHECK(!stats.instructions && !stats.tlsRecoveries && !stats.breakpoints);
        }
        else
            STONE_CHECK(stats.instructions);
        std::cout << "ART JIT publication: methods=" << stats.methods << " GS=" << stats.instructions
                  << " TLS-recoveries=" << stats.tlsRecoveries << " breakpoints=" << stats.breakpoints << '\n';
    }
    if (onlyInstallSources) {
        // As in RunArtAppSample, this isolated Application probe does not wait
        // for APK-owned executors in DestroyJavaVM. VM teardown is not tested.
        STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
        StopClientApplicationLogs();
        std::cout << "ART install-source probe passed without a host JVM.\n" << std::flush;
        std::cerr << std::flush;
        ExitProcess(0);
    }
    STONE_CHECK(runtime.Shutdown() == JNI_OK);
    STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
}
}
