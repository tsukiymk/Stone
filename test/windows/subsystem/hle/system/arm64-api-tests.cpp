#include "../../support/test-suite.h"

#include "loader/ElfImage.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "cpu/arm64/Arm64ExecutionProfile.h"
#include "cpu/arm64/Arm64ToX64.h"
#include "cpu/arm64/Arm64BlockJit.h"
#include "abi/TypedAndroidImport.h"
#include "abi/Arm64HostJni.h"
#include "runtime/art/ArtJni.h"
#include "runtime/art/ArtGs.h"
#include "signals/AndroidSignalContext.h"
#include "abi/AndroidLibraryImports.h"
#include "hle/system/stub-libc.h"
#include "stub-liblog.h"
#include "hle/system/stub-libc-properties.h"
#include "hle/system/stub-libc-users.h"
#include "memory/GuestMappings.h"
#include "hle/system/stub-libc-sockets.h"
#include "hle/external/LibraryJniStubs.h"
#include "hle/external/stub-libclang_rt.ubsan_standalone.h"
#include "hle/system/stub-libnativeloader.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <io.h>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <optional>
#include <xmmintrin.h>

#include "subsystem/support/native-checks.h"

namespace stone::windows::test::checks {

void CheckArm64Properties()
{
    SetAndroidSystemProperties(
        {{"ro.stone.number", "42"}, {"ro.stone.long", std::string(128, 'q')}, {"ro.stone.empty", ""}});
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1,
                            0xaa0303e2, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t callback[]{0xaa0003e4, 0xaa0203e5, 0x384014a6, 0x35ffffe6, 0xcb0200a5, 0xd10004a5,
                              0xf9000085, 0xb9000883, 0x39400026, 0x39003086, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    std::memcpy(bytes.data() + 0x1280, callback, sizeof(callback));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(512, true);
    STONE_CHECK(memory);

    struct Release {
        uintptr_t address;

        ~Release()
        {
            FreeArm64GuestMemory(address);
        }
    } release{memory};

    auto* buffer = reinterpret_cast<char*>(memory);
    const auto invoke = [&](uint64_t token, uint64_t first = 0, uint64_t second = 0, uint64_t third = 0) {
        const std::array<Arm64Argument, 4> args{{{'J', token}, {'J', first}, {'J', second}, {'J', third}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                constexpr std::string_view names[]{"",
                                                   "__system_property_find",
                                                   "__system_property_serial",
                                                   "__system_property_read",
                                                   "__system_property_read_callback",
                                                   "__system_property_area_serial",
                                                   "__system_properties_init",
                                                   "__system_properties_zygote_reload"};
                return target < std::size(names) ? names[target] : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    const auto find = [&](const char* name) {
        strcpy_s(buffer + 64, 64, name);
        return invoke(1, memory + 64);
    };
    const auto number = find("ro.stone.number");
    STONE_CHECK(number && find("ro.stone.number") == number && !find("ro.stone.missing"));
    STONE_CHECK(invoke(2, number) == (2u << 24));
    STONE_CHECK(invoke(3, number, memory + 128, memory + 192) == 2);
    STONE_CHECK(std::strcmp(buffer + 128, "ro.stone.number") == 0 && std::strcmp(buffer + 192, "42") == 0);
    const auto longValue = find("ro.stone.long");
    const auto serial = invoke(2, longValue);
    STONE_CHECK(serial & (1u << 16));
    invoke(3, longValue, memory + 128, memory + 192);
    STONE_CHECK(std::strstr(buffer + 192, "__system_property_read_callback"));
    invoke(4, longValue, image.LoadBias() + 0x1280, memory);
    STONE_CHECK(*reinterpret_cast<const uint64_t*>(buffer) == 128);
    STONE_CHECK(*reinterpret_cast<const uint32_t*>(buffer + 8) == serial && buffer[12] == 'r');
    const auto empty = find("ro.stone.empty");
    STONE_CHECK(empty && invoke(2, empty) == 0 && invoke(3, empty, 0, memory + 192) == 0 && !buffer[192]);
    STONE_CHECK(invoke(5) == 3 && invoke(6) == 0 && invoke(7) == 0 && invoke(2) == UINT32_MAX);
    std::memset(buffer, 0, 96); // libcutils' empty prop_info is guest-owned.
    STONE_CHECK(invoke(2, memory) == 0);
    *reinterpret_cast<uint32_t*>(buffer) = 0x03000002;
    STONE_CHECK(invoke(2, memory) == 0x03000002);
    uint32_t host = 0;
    bool rejected = false;
    try {
        invoke(2, reinterpret_cast<uintptr_t>(&host));
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    rejected = false;
    try {
        invoke(3, memory, 0, memory + 192);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    rejected = false;
    try {
        invoke(4, longValue, 1, memory);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
}

void CheckArm64LogCallbacks()
{
    auto bytes = MakeArm64TlsElf();
    const auto adr = [](unsigned reg, uint32_t instruction, uint32_t target) {
        const auto delta = (target - instruction) & 0x1fffff;
        return 0x10000000u | ((delta & 3) << 29) | ((delta >> 2) << 5) | reg;
    };
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t logger[]{0xa9bf7bfd, adr(9, 0x1284, 0x400),
                            0xb9400c0a, 0xb900012a, // priority
                            0xf940080a, 0x3940014b,
                            0x3900112b, // tag[0]
                            0xf9400c0a, 0x3940014b,
                            0x3900152b,             // file[0]
                            0xb940200a, 0xb900092a, // line
                            0xf940140a, 0x3940014b,
                            0x3900192b,             // text[0]
                            0xf940000a, 0xb9000d2a, // size
                            0xd2800090, 0xd63f0200,
                            0xa8c17bfd, 0xd65f03c0}; // Reenter the stderr logger.
    const uint32_t aborter[]{adr(9, 0x1380, 0x420), 0x3940000a, 0x3900012a, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    std::memcpy(bytes.data() + 0x1280, logger, sizeof(logger));
    std::memcpy(bytes.data() + 0x1380, aborter, sizeof(aborter));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);

    struct Reset {
        ~Reset()
        {
            SetTranslatedAndroidLogger({});
            SetTranslatedAndroidAborter({});
        }
    } reset;

    const auto invoke = [&](uint64_t token, uintptr_t argument) {
        const std::array<Arm64Argument, 2> args{{{'J', token}, {'J', argument}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'V',
            [](uintptr_t target) -> std::string_view {
                constexpr std::string_view names[]{"", "__android_log_set_logger", "__android_log_set_aborter",
                                                   "__android_log_call_aborter", "__android_log_stderr_logger"};
                return target < std::size(names) ? names[target] : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    invoke(1, image.LoadBias() + 0x1280);
    const AndroidLogMessage message{sizeof(message), 0, 5, "callback-tag", "callback-file", 23, "callback-text"};
    WriteAndroidLogMessage(&message);
    const auto* output = image.DataAt(image.LoadBias() + 0x400, 16, false);
    STONE_CHECK(*reinterpret_cast<const uint32_t*>(output) == 5);
    STONE_CHECK(output[4] == 'c' && output[5] == 'c' && output[6] == 'c');
    STONE_CHECK(*reinterpret_cast<const uint32_t*>(output + 8) == 23);
    STONE_CHECK(*reinterpret_cast<const uint32_t*>(output + 12) == sizeof(message));
    bool threadPassed = false;
    std::thread worker([&] {
        try {
            WriteAndroidLogMessage(&message);
            threadPassed = true;
        }
        catch (...) {
        }
    });
    worker.join();
    STONE_CHECK(threadPassed);
    invoke(2, image.LoadBias() + 0x1380);
    CallAndroidLogAborter("abort-test");
    STONE_CHECK(*image.DataAt(image.LoadBias() + 0x420, 1, false) == 'a');
    const auto memory = AllocateArm64GuestMemory(16, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    std::memcpy(reinterpret_cast<void*>(memory), "guest-abort", 12);
    invoke(3, memory);
    STONE_CHECK(*image.DataAt(image.LoadBias() + 0x420, 1, false) == 'g');
    bool rejected = false;
    try {
        invoke(1, 1);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    invoke(1, 4); // Restore the default logger through a valid import token.
    invoke(2, 0);
}

void CheckArm64ThreadStackAttributes()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1,
                            0xaa0303e2, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(256, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    const auto invoke = [&](uintptr_t token, uint64_t first = 0, uint64_t second = 0, uint64_t third = 0) {
        const std::array<Arm64Argument, 4> args{{{'J', token}, {'J', first}, {'J', second}, {'J', third}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                constexpr std::string_view names[]{"",
                                                   "pthread_self",
                                                   "pthread_getattr_np",
                                                   "pthread_attr_getguardsize",
                                                   "pthread_attr_getstacksize",
                                                   "pthread_attr_getstack",
                                                   "pthread_attr_destroy"};
                return target < std::size(names) ? names[target] : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    const auto thread = invoke(1);
    STONE_CHECK(thread == GetCurrentThreadId() && invoke(2, thread, memory) == 0);
    auto* result = reinterpret_cast<uintptr_t*>(memory + 128);
    STONE_CHECK(invoke(3, memory, memory + 128) == 0 && result[0] == 0);
    STONE_CHECK(invoke(4, memory, memory + 136) == 0 && result[1] > 0);
    STONE_CHECK(invoke(5, memory, memory + 144, memory + 152) == 0 && result[2] && result[3] == result[1]);
    reinterpret_cast<AndroidThreadAttr*>(memory)->guardSize = 8192;
    STONE_CHECK(invoke(3, memory, memory + 128) == 0 && result[0] == 8192);
    uintptr_t host = 7;
    bool rejected = false;
    try {
        invoke(3, memory, reinterpret_cast<uintptr_t>(&host));
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && host == 7);
    STONE_CHECK(invoke(6, memory) == 0);
}

void CheckArm64NativeLoader()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xf94003f0, 0xd61f0200}; // Ninth argument is the import token; tail-call with x0..x7.
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(128, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    struct Reset {
        ~Reset()
        {
            reinterpret_cast<void (*)()>(AndroidNativeLoaderImport("ResetNativeLoader", true))();
        }
    } reset;

    auto* output = reinterpret_cast<uint8_t*>(memory);
    auto& error = *reinterpret_cast<uintptr_t*>(memory + 8);
    std::memcpy(output + 32, "libc.so", 8);
    const auto invoke = [&](uintptr_t token, std::array<uint64_t, 8> values = {}) {
        std::vector<Arm64Argument> args;
        for (const auto value : values)
            args.push_back({'J', value});
        args.push_back({'J', token});
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                constexpr std::string_view names[]{"",
                                                   "InitializeNativeLoader",
                                                   "ResetNativeLoader",
                                                   "OpenNativeLibrary",
                                                   "CloseNativeLibrary",
                                                   "NativeLoaderFreeErrorMessage",
                                                   "FindNativeLoaderNamespaceByClassLoader",
                                                   "IsNamespaceNativeBridged",
                                                   "strlen",
                                                   "android_get_application_target_sdk_version"};
                return target < std::size(names) ? names[target] : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    {
        struct RestoreTargetSdk {
            int value = AndroidTargetSdk();

            ~RestoreTargetSdk()
            {
                SetAndroidTargetSdk(value);
            }
        } restore;

        for (const auto sdk : {21, 28, 37}) {
            SetAndroidTargetSdk(sdk);
            STONE_CHECK(invoke(9) == unsigned(sdk) && AndroidTargetSdk() == sdk);
        }
    }
    const auto releaseError = [&] {
        STONE_CHECK(error && invoke(8, {error}) > 0);
        invoke(5, {error});
        bool rejected = false;
        try {
            invoke(8, {error});
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    };
    invoke(2);
    const std::array<uint64_t, 8> open{0, 37, memory + 32, 0, 0, 0, memory, memory + 8};
    STONE_CHECK(!invoke(3, open) && !output[0] && error);
    STONE_CHECK(std::strstr(reinterpret_cast<const char*>(error), "not been initialized"));
    releaseError();
    invoke(1);
    output[0] = 1;
    const auto handle = invoke(3, open);
    STONE_CHECK(handle && !output[0] && !error);
    STONE_CHECK(!invoke(4, {handle, 1, memory + 8}) && error);
    STONE_CHECK(std::strstr(reinterpret_cast<const char*>(error), "native-bridge namespace"));
    releaseError();
    STONE_CHECK(invoke(4, {handle, 0, memory + 8}) == 1 && !error);
    auto unsupported = open;
    unsupported[3] = 1;
    STONE_CHECK(!invoke(3, unsupported) && error);
    STONE_CHECK(std::strstr(reinterpret_cast<const char*>(error), "unregistered ART application native class loader"));
    releaseError();
    STONE_CHECK(invoke(6) == 0 && invoke(7) == 0 && invoke(5) == 0);
    uintptr_t host = 7;
    auto invalid = open;
    invalid[6] = reinterpret_cast<uintptr_t>(&host);
    bool rejected = false;
    try {
        invoke(3, invalid);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && host == 7);
    rejected = false;
    try {
        invoke(5, {reinterpret_cast<uintptr_t>(&host)});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && host == 7);
}

void CheckArm64UserDatabase()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1, 0xaa0303e2,
                            0xaa0403e3, 0xaa0503e4, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(512, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto& result = *reinterpret_cast<uintptr_t*>(memory + 256);
    const auto uid = AndroidApplicationUid();
    const auto invoke = [&](uintptr_t token, uint64_t key, uint64_t storage = 0, uint64_t strings = 0,
                            uint64_t capacity = 0, uint64_t output = 0) {
        const std::array<Arm64Argument, 6> args{
            {{'J', token}, {'J', key}, {'J', storage}, {'J', strings}, {'J', capacity}, {'J', output}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                constexpr std::string_view names[]{"",           "getpwuid_r", "getpwnam_r",
                                                   "getgrgid_r", "getgrnam_r", "strlen"};
                return target < std::size(names) ? names[target] : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    const auto inside = [&](const void* pointer) {
        const auto address = reinterpret_cast<uintptr_t>(pointer);
        return address >= memory + 128 && address < memory + 256;
    };
    STONE_CHECK(invoke(1, uid, memory, memory + 128, 128, memory + 256) == 0 && result == memory);
    const auto user = *reinterpret_cast<const AndroidPasswd*>(memory);
    STONE_CHECK(user.uid == uid && user.gid == uid && inside(user.name) && inside(user.directory) &&
                inside(user.shell));
    STONE_CHECK(!user.password && !user.gecos && std::strcmp(user.directory, "/data") == 0 &&
                std::strcmp(user.shell, "/bin/sh") == 0);
    STONE_CHECK(invoke(5, reinterpret_cast<uintptr_t>(user.name)) == std::strlen(user.name));
    strcpy_s(reinterpret_cast<char*>(memory + 288), 64, user.name);
    STONE_CHECK(invoke(2, memory + 288, memory, memory + 128, 128, memory + 256) == 0 && result == memory);
    STONE_CHECK(invoke(1, uid + 1, memory, memory + 128, 128, memory + 256) == 0 && !result);
    STONE_CHECK(invoke(1, uid, memory, memory + 128, 1, memory + 256) == ERANGE && !result);
    STONE_CHECK(invoke(1, uid, memory, 0, 0, memory + 256) == ERANGE && !result);
    STONE_CHECK(invoke(1, uid, 0, memory + 128, 128, memory + 256) == EINVAL);
    STONE_CHECK(invoke(2, 0, memory, memory + 128, 128, memory + 256) == EINVAL);
    STONE_CHECK(invoke(3, uid, memory + 64, memory + 129, 127, memory + 256) == 0 && result == memory + 64);
    const auto group = *reinterpret_cast<const AndroidGroup*>(memory + 64);
    STONE_CHECK(group.gid == uid && !group.password && inside(group.name) && inside(group.members));
    STONE_CHECK(!(reinterpret_cast<uintptr_t>(group.members) % alignof(char*)) && group.members[0] == group.name &&
                !group.members[1]);
    STONE_CHECK(invoke(5, reinterpret_cast<uintptr_t>(group.members[0])) == std::strlen(group.name));
    STONE_CHECK(invoke(4, memory + 288, memory + 64, memory + 128, 128, memory + 256) == 0 && result == memory + 64);
    STONE_CHECK(invoke(3, uid + 1, memory + 64, memory + 128, 128, memory + 256) == 0 && !result);
    STONE_CHECK(invoke(3, uid, memory + 64, memory + 128, 1, memory + 256) == ERANGE && !result);
    uintptr_t host = 7;
    for (unsigned argument = 1; argument <= 4; ++argument) {
        std::array<uint64_t, 5> args{uid, memory, memory + 128, 128, memory + 256};
        if (argument == 3)
            args[3] = uint64_t{UINT_MAX} + 1;
        else
            args[argument] = reinterpret_cast<uintptr_t>(&host);
        bool rejected = false;
        try {
            invoke(1, args[0], args[1], args[2], args[3], args[4]);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected && host == 7);
    }
    AndroidPasswd native{}, *found = nullptr;
    char strings[128]{};
    const auto nativeLookup =
        Guest<int (*)(uint32_t, AndroidPasswd*, char*, size_t, AndroidPasswd**)>(Libc("getpwuid_r"), "(ILLJL)I");
    STONE_CHECK(nativeLookup(uid, &native, strings, sizeof(strings), &found) == 0 && found == &native &&
                native.uid == uid);
    STONE_CHECK(nativeLookup(uid, nullptr, strings, sizeof(strings), &found) == EINVAL);
}

void CheckArm64LibrarySearchPath()
{
    constexpr char expected[] = "/apex/com.android.art/lib64:/apex/com.android.i18n/lib64:/system/lib64";
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(sizeof(expected) + 2, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto* buffer = reinterpret_cast<uint8_t*>(memory);
    std::memset(buffer, 0xa5, sizeof(expected) + 2);
    const auto invoke = [&](uintptr_t token, uintptr_t address, size_t capacity) {
        const std::array<Arm64Argument, 3> args{{{'J', token}, {'L', address}, {'J', capacity}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                if (target == 1)
                    return "android_get_LD_LIBRARY_PATH";
                if (target == 2)
                    return "strlen";
                return {};
            },
            nullptr, nullptr, args);
    };
    invoke(1, memory + 1, sizeof(expected));
    STONE_CHECK(std::memcmp(buffer + 1, expected, sizeof(expected)) == 0 && buffer[0] == 0xa5 &&
                buffer[sizeof(expected) + 1] == 0xa5);
    STONE_CHECK(invoke(2, memory + 1, 0) == sizeof(expected) - 1);
    std::memset(buffer, 0xa5, sizeof(expected) + 2);
    bool rejected = false;
    try {
        invoke(1, memory + 1, sizeof(expected) - 1);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && buffer[1] == 0xa5);
    rejected = false;
    try {
        invoke(1, 0, sizeof(expected));
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    char native[256]{};
    rejected = false;
    try {
        invoke(1, reinterpret_cast<uintptr_t>(native), sizeof(native));
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && !native[0]);
    const auto get = Guest<void (*)(char*, size_t)>(Libdl("android_get_LD_LIBRARY_PATH"), "(LJ)V");
    std::memset(native, 0xa5, sizeof(native));
    get(native, sizeof(native));
    STONE_CHECK(std::strcmp(native, expected) == 0 && static_cast<uint8_t>(native[sizeof(expected)]) == 0xa5);
}

void CheckArm64FutexRequeue()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t instructions[]{0xaa0603e8, 0xd4000001, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, instructions, sizeof(instructions));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(8, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto& source = *reinterpret_cast<uint32_t*>(memory);
    auto& destination = *reinterpret_cast<uint32_t*>(memory + 4);
    source = destination = 0;
    const auto syscall = [&](uintptr_t address, unsigned operation, uint64_t count, uint64_t move = 0,
                             uintptr_t target = 0, uint32_t compare = 0) {
        const std::array<Arm64Argument, 7> args{
            {{'L', address}, {'I', operation}, {'J', count}, {'J', move}, {'L', target}, {'I', compare}, {'I', 98}}};
        return static_cast<int64_t>(
            RunArm64Function(image, image.LoadBias() + 0x1200, 'J', {}, nullptr, nullptr, args));
    };
    STONE_CHECK(syscall(memory, 132, 0, 2, memory + 4, 1) == -EAGAIN);
    STONE_CHECK(syscall(memory, 131, 0, 2, 0) == -EINVAL);
    STONE_CHECK(syscall(memory, 131, uint64_t{INT_MAX} + 1, 2, memory + 4) == -EINVAL);
    STONE_CHECK(syscall(memory, 132, 0, uint64_t{INT_MAX} + 1, memory + 4) == -EINVAL);
    std::array<int64_t, 2> results{};
    std::array<std::exception_ptr, 2> failures{};
    std::vector<std::jthread> workers;

    struct Wake {
        uint32_t& source;
        uint32_t& destination;

        ~Wake()
        {
            std::atomic_ref<uint32_t>(source).store(1);
            std::atomic_ref<uint32_t>(destination).store(1);
            AndroidFutex(source, 129, INT_MAX, nullptr, 0);
            AndroidFutex(destination, 129, INT_MAX, nullptr, 0);
        }
    } wake{source, destination};

    for (size_t i = 0; i < results.size(); ++i)
        workers.emplace_back([&, i] {
            try {
                results[i] = syscall(memory, 128, 0);
            }
            catch (...) {
                failures[i] = std::current_exception();
            }
        });
    int64_t moved = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (moved < 2 && std::chrono::steady_clock::now() < deadline) {
        const auto result = syscall(memory, 132, 0, 2, memory + 4);
        STONE_CHECK(result >= 0);
        moved += result;
        if (moved < 2)
            std::this_thread::yield();
    }
    STONE_CHECK(moved == 2 && syscall(memory, 129, 2) == 0 && syscall(memory + 4, 129, 2) == 2);
    for (auto& worker : workers)
        worker.join();
    for (size_t i = 0; i < results.size(); ++i)
        STONE_CHECK(!failures[i] && results[i] == 0);
}

void CheckArm64VectorIo()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1,
                            0xaa0303e2, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(256, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto* vectors = reinterpret_cast<AndroidIoVector*>(memory);
    auto* storage = reinterpret_cast<uint8_t*>(memory);
    const auto create = reinterpret_cast<int (*)(const char*, unsigned)>(AndroidLibcImport("memfd_create", true));
    const int fd = create("arm64-vector-io", 0);
    STONE_CHECK(fd >= 0);

    struct Close {
        int fd;

        ~Close()
        {
            reinterpret_cast<int (*)(int)>(AndroidLibcImport("close", true))(fd);
        }
    } close{fd};

    const auto seek = reinterpret_cast<int64_t (*)(int, int64_t, int)>(AndroidLibcImport("lseek", true));
    const auto invoke = [&](unsigned token, uintptr_t table, int count) {
        const std::array<Arm64Argument, 4> args{
            {{'I', token}, {'I', unsigned(fd)}, {'L', table}, {'I', unsigned(count)}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                return target == 1 ? "writev" : target == 2 ? "readv" : "";
            },
            nullptr, nullptr, args);
    };
    std::memcpy(storage + 64, "abcdef", 6);
    vectors[0] = {storage + 64, 2};
    vectors[1] = {reinterpret_cast<void*>(1), 0};
    vectors[2] = {storage + 66, 4};
    STONE_CHECK(invoke(1, memory, 3) == 6 && seek(fd, 0, 0) == 0);
    std::memset(storage + 128, 0xa5, 32);
    vectors[0] = {storage + 128, 1};
    vectors[1] = {storage + 136, 10};
    STONE_CHECK(invoke(2, memory, 2) == 6 && storage[128] == 'a' && storage[129] == 0xa5 &&
                std::memcmp(storage + 136, "bcdef", 5) == 0 && storage[141] == 0xa5);
    STONE_CHECK(invoke(1, 0, 1) == UINT64_MAX && errno == EFAULT);
    STONE_CHECK(invoke(1, memory, -1) == UINT64_MAX && errno == EINVAL);
    STONE_CHECK(invoke(2, memory, 1025) == UINT64_MAX && errno == EINVAL);
    STONE_CHECK(invoke(1, 0, 0) == 0);
    vectors[0] = {nullptr, 1};
    STONE_CHECK(invoke(1, memory, 1) == UINT64_MAX && errno == EFAULT);
    vectors[0] = {storage + 64, uint64_t{UINT_MAX} + 1};
    STONE_CHECK(invoke(1, memory, 1) == UINT64_MAX && errno == EINVAL);
    int host = 7;
    vectors[0] = {storage + 128, 1};
    vectors[1] = {&host, sizeof(host)};
    for (const auto token : {1u, 2u}) {
        STONE_CHECK(seek(fd, 0, 0) == 0);
        bool rejected = false;
        try {
            invoke(token, memory, 2);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected && host == 7 && seek(fd, 0, 1) == 0);
        rejected = false;
        try {
            invoke(token, reinterpret_cast<uintptr_t>(&host), 1);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected && host == 7);
    }
}

void CheckArm64AllocationStatistics()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t statsCode[]{0xa9bf7bfd, 0xaa0003e8, 0x58000090, 0xd63f0200, 0xa8c17bfd, 0xd65f03c0};
    const uint32_t tail[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, statsCode, sizeof(statsCode));
    std::memcpy(bytes.data() + 0x1218, &token, 8);
    std::memcpy(bytes.data() + 0x1280, tail, sizeof(tail));
    std::memcpy(bytes.data() + 0x1288, &token, 8);
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(112, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    std::memset(reinterpret_cast<void*>(memory), 0xa5, 112);
    const auto invoke = [&](std::string_view name, std::initializer_list<uint64_t> values, bool statistics = false) {
        std::vector<Arm64Argument> arguments;
        for (const auto value : values)
            arguments.push_back({'J', value});
        return RunArm64Function(
            image, image.LoadBias() + (statistics ? 0x1200 : 0x1280), 'J',
            [&](uintptr_t) -> std::string_view {
                return name;
            },
            nullptr, nullptr, arguments);
    };
    const auto read = [&](std::string_view name = "mallinfo") {
        invoke(name, {memory + 16}, true);
        std::array<uint64_t, 10> result{};
        std::memcpy(result.data(), reinterpret_cast<const void*>(memory + 16), sizeof(result));
        STONE_CHECK(result[0] == result[7] && result[5] >= result[7] && !result[1] && !result[2] && !result[3] &&
                    !result[4] && !result[6] && !result[8] && !result[9]);
        STONE_CHECK(*reinterpret_cast<const uint64_t*>(memory) == 0xa5a5a5a5a5a5a5a5 &&
                    *reinterpret_cast<const uint64_t*>(memory + 96) == 0xa5a5a5a5a5a5a5a5);
        return result;
    };
    const auto baseline = read()[7];
    std::vector<uint8_t> host(8192);
    STONE_CHECK(read()[7] == baseline);
    const auto first = invoke("malloc", {1024}), second = invoke("calloc", {4, 64});
    STONE_CHECK(first && second && read()[7] == baseline + 1280);
    std::memset(reinterpret_cast<void*>(first), 0xa5, 1024);
    for (const auto option : {-100, -103, -200, -203, INT_MAX})
        STONE_CHECK(invoke("mallopt", {static_cast<uint32_t>(option), 1}) == 0); // Unsupported tuning is reported.
    for (const auto option : {-101, -104, -105}) {
        STONE_CHECK(invoke("mallopt", {static_cast<uint32_t>(option), 0}) <= 1 && read()[7] == baseline + 1280);
        STONE_CHECK(std::all_of(reinterpret_cast<const uint8_t*>(first), reinterpret_cast<const uint8_t*>(first + 1024),
                                [](uint8_t value) {
                                    return value == 0xa5;
                                }));
        STONE_CHECK(std::all_of(reinterpret_cast<const uint8_t*>(second),
                                reinterpret_cast<const uint8_t*>(second + 256), [](uint8_t value) {
                                    return value == 0;
                                }));
    }
    const auto grown = invoke("realloc", {first, 2048});
    STONE_CHECK(grown && read("mallinfo2")[7] == baseline + 2304);
    invoke("free", {grown});
    invoke("free", {second});
    invoke("free", {0});
    const auto released = read();
    STONE_CHECK(released[7] == baseline && released[5] >= baseline + 2304);
    std::array<uint64_t, 10> unowned{};
    bool rejected = false;
    try {
        invoke("mallinfo", {reinterpret_cast<uintptr_t>(unowned.data())}, true);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && unowned == std::array<uint64_t, 10>{});
}

void CheckArm64FloatingStrings()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t code[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, code, sizeof(code));
    std::memcpy(bytes.data() + 0x1208, &token, sizeof(token));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(1024, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    auto* buffer = reinterpret_cast<uint8_t*>(memory);
    const auto input = memory + 32, output = memory + 512;
    const auto text = [&](const char* value) {
        STONE_CHECK(std::strlen(value) < 400);
        std::memcpy(reinterpret_cast<void*>(input), value, std::strlen(value) + 1);
    };
    const auto invoke = [&](std::string_view name, char result, uintptr_t source, uintptr_t end) {
        const Arm64Argument arguments[]{{'L', source}, {'L', end}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, result,
            [&](uintptr_t target) -> std::string_view {
                return target == token ? name : std::string_view{};
            },
            nullptr, nullptr, arguments);
    };

    struct Case {
        const char* text;
        size_t used;
        double value;
    };

    const Case cases[]{{"42.5tail", 4, 42.5},
                       {" \t-1.25e+2px", 10, -125.0},
                       {"0x1.8p+2,", 8, 6.0},
                       {"-0", 2, -0.0},
                       {"junk", 0, 0.0},
                       {"  junk", 0, 0.0},
                       {"", 0, 0.0},
                       {"1e+", 1, 1.0},
                       {"16777217", 8, 16777217.0}};
    for (const auto& sample : cases)
        for (const bool single : {true, false}) {
            text(sample.text);
            std::memset(buffer + 504, 0xa5, 24);
            errno = 7;
            const auto expected = single ? uint64_t{std::bit_cast<uint32_t>(static_cast<float>(sample.value))}
                                         : std::bit_cast<uint64_t>(sample.value);
            const auto name = single ? "strtof" : "strtod";
            STONE_CHECK(invoke(name, single ? 'F' : 'D', input, output) == expected && errno == 7);
            STONE_CHECK(*reinterpret_cast<const uintptr_t*>(output) == input + sample.used);
            STONE_CHECK(*reinterpret_cast<const uint64_t*>(output - 8) == 0xa5a5a5a5a5a5a5a5 &&
                        *reinterpret_cast<const uint64_t*>(output + 8) == 0xa5a5a5a5a5a5a5a5);
            STONE_CHECK(std::strcmp(reinterpret_cast<const char*>(input), sample.text) == 0);
            STONE_CHECK(invoke(name, single ? 'F' : 'D', input, 0) == expected);
            STONE_CHECK(invoke(name, 'J', input, 0) == 0); // The result belongs in v0, not x0.
        }
    for (const bool single : {true, false}) {
        const auto name = single ? "strtof" : "strtod";
        const auto kind = single ? 'F' : 'D';
        const uint64_t infinity = single ? 0x7f800000 : 0x7ff0000000000000;
        text("InFiNiTy!");
        STONE_CHECK(invoke(name, kind, input, output) == infinity &&
                    *reinterpret_cast<uintptr_t*>(output) == input + 8);
        text("NaN");
        const auto nan = invoke(name, kind, input, output);
        STONE_CHECK((nan & infinity) == infinity && (nan & (single ? 0x7fffff : 0xfffffffffffff)) != 0);
        text("1e9999x");
        errno = 0;
        STONE_CHECK(invoke(name, kind, input, output) == infinity && errno == ERANGE &&
                    *reinterpret_cast<uintptr_t*>(output) == input + 6);
        text("1e-9999x");
        errno = 0;
        STONE_CHECK(invoke(name, kind, input, output) == 0 && errno == ERANGE);
    }
    // Bionic's regression cases at the least float subnormal and double normal boundary.
    text("7.0064923216240853546186479164495e-46");
    STONE_CHECK(invoke("strtof", 'F', input, 0) == 0);
    text("7.0064923216240853546186479164496e-46");
    STONE_CHECK(invoke("strtof", 'F', input, 0) == 1);
    text("2.2250738585072012e-308");
    STONE_CHECK(invoke("strtod", 'D', input, 0) == 0x0010000000000000);
    const auto rejected = [](auto operation) {
        try {
            operation();
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    char host[] = "42.5";
    uintptr_t hostEnd = 17;
    text(host);
    STONE_CHECK(rejected([&] {
        invoke("strtof", 'F', reinterpret_cast<uintptr_t>(host), output);
    }));
    STONE_CHECK(rejected([&] {
                    invoke("strtod", 'D', input, reinterpret_cast<uintptr_t>(&hostEnd));
                }) &&
                hostEnd == 17);
    STONE_CHECK(rejected([&] {
        invoke("strtof", 'F', input, memory + 1020);
    }));
    const auto unterminated = AllocateArm64GuestMemory(4, false);
    STONE_CHECK(unterminated);
    Free freeUnterminated{unterminated};
    std::memcpy(reinterpret_cast<void*>(unterminated), "1234", 4);
    STONE_CHECK(rejected([&] {
        invoke("strtof", 'F', unterminated, output);
    }));
    auto* page = AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0);
    STONE_CHECK(page != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* value;

        ~Unmap()
        {
            AndroidUnmapArm64(value, 4096);
        }
    } unmap{page};

    std::memcpy(page, "42.5tail", 9);
    STONE_CHECK(AndroidProtectArm64(page, 4096, 1) == 0);
    const auto readOnly = reinterpret_cast<uintptr_t>(page);
    STONE_CHECK(invoke("strtof", 'F', readOnly, output) == std::bit_cast<uint32_t>(42.5f) &&
                *reinterpret_cast<uintptr_t*>(output) == readOnly + 4);
    STONE_CHECK(rejected([&] {
        invoke("strtod", 'D', input, readOnly);
    }));
}

void CheckArm64NetworkResolver()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t code[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, code, sizeof(code));
    std::memcpy(bytes.data() + 0x1208, &token, sizeof(token));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(512, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    auto* buffer = reinterpret_cast<uint8_t*>(memory);
    const auto invoke = [&](std::string_view name, std::initializer_list<uint64_t> values) {
        std::vector<Arm64Argument> args;
        for (const auto value : values)
            args.push_back({'J', value});
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [&](uintptr_t address) -> std::string_view {
                return address == token ? name : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    const auto resolve = [&](uintptr_t node, uintptr_t hints, uint64_t network, uint64_t mark, uintptr_t output) {
        return invoke("android_getaddrinfofornet", {node, memory + 64, hints, network, mark, output});
    };
    const auto rejected = [](auto action) {
        try {
            action();
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    std::memcpy(buffer + 64, "443", 4);
    auto* hints = reinterpret_cast<AndroidAddrInfo*>(buffer + 96);
    auto** output = reinterpret_cast<AndroidAddrInfo**>(buffer + 192);
    for (const int family : {2, 10})
        for (const unsigned mark : {0u, UINT32_MAX}) {
            const auto* node = family == 2 ? "127.0.0.1" : "::1";
            std::memcpy(buffer, node, std::strlen(node) + 1);
            *hints = {};
            hints->flags = 12;
            hints->family = family;
            hints->type = 1;
            hints->protocol = 6;
            std::memset(buffer + 184, 0xa5, 24);
            STONE_CHECK(resolve(memory, memory + 96, 0, mark, memory + 192) == 0 && *output);
            const auto* address = *output;
            STONE_CHECK(address->family == family && address->type == 1 && address->protocol == 6 &&
                        address->length == (family == 2 ? 16u : 28u) && address->address);
            const auto* socket = static_cast<const uint8_t*>(address->address);
            STONE_CHECK(socket[0] == family && socket[1] == 0 && socket[2] == 1 && socket[3] == 187);
            if (family == 2)
                STONE_CHECK(socket[4] == 127 && socket[5] == 0 && socket[6] == 0 && socket[7] == 1);
            else
                STONE_CHECK(std::all_of(socket + 8, socket + 23,
                                        [](uint8_t b) {
                                            return !b;
                                        }) &&
                            socket[23] == 1);
            invoke("freeaddrinfo", {reinterpret_cast<uintptr_t>(address)});
            STONE_CHECK(std::all_of(buffer + 184, buffer + 192,
                                    [](uint8_t b) {
                                        return b == 0xa5;
                                    }) &&
                        std::all_of(buffer + 200, buffer + 208, [](uint8_t b) {
                            return b == 0xa5;
                        }));
        }
    STONE_CHECK(resolve(memory, memory + 96, 1, 0, memory + 192) == 11 && errno == 95 && !*output);
    STONE_CHECK(resolve(memory, memory + 96, 0, 1, memory + 192) == 11 && errno == 95 && !*output);
    hints->length = 1;
    STONE_CHECK(resolve(memory, memory + 96, 0, 0, memory + 192) == 12 && !*output);
    hints->length = 0;
    STONE_CHECK(invoke("getaddrinfo", {memory, memory + 64, memory + 96, memory + 192}) == 0 && *output);
    invoke("freeaddrinfo", {reinterpret_cast<uintptr_t>(*output)});
    STONE_CHECK(resolve(memory, memory + 96, 0, 0, 0) == 11 && errno == EFAULT);
    uintptr_t host = 0;
    STONE_CHECK(rejected([&] {
                    resolve(memory, memory + 96, 0, 0, reinterpret_cast<uintptr_t>(&host));
                }) &&
                !host);
    STONE_CHECK(rejected([&] {
        resolve(memory, memory + 96, 0, 0, memory + 508);
    }));
    STONE_CHECK(rejected([&] {
        resolve(memory, memory + 500, 0, 0, memory + 192);
    }));
    const AndroidAddrInfo hostHints = *hints;
    STONE_CHECK(rejected([&] {
        resolve(memory, reinterpret_cast<uintptr_t>(&hostHints), 0, 0, memory + 192);
    }));
    STONE_CHECK(rejected([&] {
        resolve(reinterpret_cast<uintptr_t>("host"), memory + 96, 0, 0, memory + 192);
    }));
    auto* page = AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0);
    STONE_CHECK(page != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* value;

        ~Unmap()
        {
            AndroidUnmapArm64(value, 4096);
        }
    } unmap{page};

    std::memset(page, 'x', 4096);
    STONE_CHECK(AndroidProtectArm64(page, 4096, 1) == 0);
    const auto readOnly = reinterpret_cast<uintptr_t>(page);
    STONE_CHECK(rejected([&] {
        resolve(memory, memory + 96, 0, 0, readOnly);
    }));
    STONE_CHECK(rejected([&] {
        resolve(readOnly, memory + 96, 0, 0, memory + 192);
    }));
    STONE_CHECK(AndroidProtectArm64(page, 4096, 3) == 0);
    std::memcpy(page, "::1", 4);
    std::memcpy(static_cast<uint8_t*>(page) + 96, hints, sizeof(*hints));
    STONE_CHECK(AndroidProtectArm64(page, 4096, 1) == 0);
    STONE_CHECK(resolve(readOnly, readOnly + 96, 0, 0, memory + 192) == 0 && *output);
    invoke("freeaddrinfo", {reinterpret_cast<uintptr_t>(*output)});
}

void CheckArm64ThreadPriorities()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t code[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, code, sizeof(code));
    std::memcpy(bytes.data() + 0x1208, &token, sizeof(token));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto invoke = [&](std::string_view name, std::initializer_list<uint64_t> values) {
        std::vector<Arm64Argument> args;
        for (const auto value : values)
            args.push_back({'J', value});
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [&](uintptr_t address) -> std::string_view {
                return address == token ? name : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    const auto query = reinterpret_cast<int (*)(int, unsigned)>(AndroidLibcImport("getpriority", true));
    const auto set = reinterpret_cast<int (*)(int, unsigned, int)>(AndroidLibcImport("setpriority", true));
    STONE_CHECK(query && set);
    const auto tid = GetCurrentThreadId();
    const int windowsPriority = GetThreadPriority(GetCurrentThread());
    errno = 0;
    const int previous = query(0, tid);
    STONE_CHECK(previous != -1 || !errno);

    struct Restore {
        decltype(set) function;
        unsigned tid;
        int nice, windows;

        ~Restore()
        {
            function(0, tid, nice);
            SetThreadPriority(GetCurrentThread(), windows);
        }
    } restore{set, tid, previous, windowsPriority};

    for (const int nice : {-2, -1, 0, 5})
        for (const auto target : std::array<uint64_t, 3>{0, tid, GetCurrentProcessId()}) {
            STONE_CHECK(invoke("setpriority", {0, target, uint32_t(nice)}) == 0);
            errno = 0;
            STONE_CHECK(invoke("getpriority", {0, target}) == uint32_t(nice) && !errno && query(0, tid) == nice);
            const int expected = nice < 0    ? THREAD_PRIORITY_ABOVE_NORMAL
                                 : nice == 0 ? THREAD_PRIORITY_NORMAL
                                             : THREAD_PRIORITY_LOWEST;
            STONE_CHECK(GetThreadPriority(GetCurrentThread()) == expected);
        }
    STONE_CHECK(invoke("setpriority", {1, 0, 0}) == UINT32_MAX && errno == EINVAL);
    STONE_CHECK(invoke("setpriority", {0, MAXDWORD, 0}) == UINT32_MAX && errno == ESRCH);
    STONE_CHECK(invoke("getpriority", {0, MAXDWORD}) == UINT32_MAX && errno == ESRCH);
    STONE_CHECK(query(0, tid) == 5 && GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_LOWEST);
}

void CheckArm64RandomBytes()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t code[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, code, sizeof(code));
    std::memcpy(bytes.data() + 0x1208, &token, sizeof(token));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(320, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    auto* buffer = reinterpret_cast<uint8_t*>(memory);
    const auto invoke = [&](uintptr_t pointer, uint64_t size, uint64_t flags) {
        const Arm64Argument args[]{{'J', pointer}, {'J', size}, {'J', flags}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [&](uintptr_t value) -> std::string_view {
                return value == token ? "getrandom" : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    const auto rejected = [&](uintptr_t pointer, uint64_t size) {
        try {
            invoke(pointer, size, 0);
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    std::array<uint8_t, 256> previous{};
    for (unsigned flags = 0; flags < 4; ++flags) {
        std::memset(buffer, 0xa5, 320);
        errno = EDOM;
        STONE_CHECK(invoke(memory + 32, 256, flags) == 256 && errno == EDOM);
        STONE_CHECK(std::any_of(buffer + 32, buffer + 288,
                                [](uint8_t value) {
                                    return value != 0xa5;
                                }) &&
                    std::memcmp(buffer + 32, previous.data(), 256) != 0);
        STONE_CHECK(std::all_of(buffer, buffer + 32,
                                [](uint8_t value) {
                                    return value == 0xa5;
                                }) &&
                    std::all_of(buffer + 288, buffer + 320, [](uint8_t value) {
                        return value == 0xa5;
                    }));
        std::memcpy(previous.data(), buffer + 32, 256);
        errno = EDOM;
        STONE_CHECK(invoke(0, 0, flags) == 0 && errno == EDOM);
    }
    std::memset(buffer, 0xa5, 320);
    STONE_CHECK(invoke(memory + 32, 256, ~uint64_t{0}) == UINT64_MAX && errno == EINVAL);
    STONE_CHECK(invoke(memory + 32, uint64_t{ULONG_MAX} + 1, 0) == UINT64_MAX && errno == EINVAL);
    STONE_CHECK(invoke(0, 1, 0) == UINT64_MAX && errno == EFAULT);
    STONE_CHECK(rejected(reinterpret_cast<uintptr_t>(previous.data()), 256) && rejected(memory + 319, 2));
    STONE_CHECK(std::all_of(buffer, buffer + 320, [](uint8_t value) {
        return value == 0xa5;
    }));
    auto* page = AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0);
    STONE_CHECK(page != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* value;

        ~Unmap()
        {
            AndroidUnmapArm64(value, 4096);
        }
    } unmap{page};

    std::memset(page, 0xa5, 4096);
    STONE_CHECK(AndroidProtectArm64(page, 4096, 1) == 0);
    const auto readOnly = reinterpret_cast<uintptr_t>(page);
    STONE_CHECK(rejected(readOnly + 32, 256) && invoke(readOnly, 0, 0) == 0);
    STONE_CHECK(
        std::all_of(static_cast<const uint8_t*>(page), static_cast<const uint8_t*>(page) + 4096, [](uint8_t value) {
            return value == 0xa5;
        }));
    STONE_CHECK(std::all_of(buffer, buffer + 320, [](uint8_t value) {
        return value == 0xa5;
    }));
}

void CheckArm64StringStdioResolver()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t code[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, code, sizeof(code));
    std::memcpy(bytes.data() + 0x1208, &token, sizeof(token));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(1024, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    auto* buffer = reinterpret_cast<uint8_t*>(memory);
    const auto text = [&](size_t at, const char* value) {
        const auto count = std::strlen(value) + 1;
        STONE_CHECK(at <= 1024 && count <= 1024 - at);
        std::memcpy(buffer + at, value, count);
        return memory + at;
    };
    const auto invoke = [&](std::string_view name, std::initializer_list<uint64_t> values) {
        std::vector<Arm64Argument> arguments;
        for (const auto value : values)
            arguments.push_back({'J', value});
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [&](uintptr_t address) -> std::string_view {
                return address == token ? name : std::string_view{};
            },
            nullptr, nullptr, arguments);
    };
    const auto rejected = [](auto action) {
        try {
            action();
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    const auto first = text(32, ",alpha,,beta,"), second = text(80, "one|two"), commas = text(64, ","),
               bars = text(112, "|");
    *reinterpret_cast<uint64_t*>(memory) = UINT64_MAX; // Initial save state is ignored for a new string.
    STONE_CHECK(invoke("strtok_r", {first, commas, memory}) == first + 1 &&
                std::strcmp(reinterpret_cast<char*>(first + 1), "alpha") == 0);
    STONE_CHECK(invoke("strtok_r", {second, bars, memory + 8}) == second);
    STONE_CHECK(*reinterpret_cast<const uintptr_t*>(memory + 8) == second + 4);
    STONE_CHECK(invoke("strtok_r", {0, commas, memory}) == first + 8 &&
                std::strcmp(reinterpret_cast<char*>(first + 8), "beta") == 0);
    const auto secondToken = invoke("strtok_r", {0, bars, memory + 8});
    STONE_CHECK(secondToken == second + 4 && std::strcmp(reinterpret_cast<char*>(second + 4), "two") == 0);
    STONE_CHECK(!invoke("strtok_r", {0, commas, memory}) && !invoke("strtok_r", {0, bars, memory + 8}));
    uintptr_t hostState = 0;
    text(32, "unchanged,value");
    STONE_CHECK(rejected([&] {
                    invoke("strtok_r", {first, commas, reinterpret_cast<uintptr_t>(&hostState)});
                }) &&
                std::strcmp(reinterpret_cast<char*>(first), "unchanged,value") == 0);
    const auto input = text(128, "foobar");
    std::memset(buffer + 160, 0xa5, 48);
    STONE_CHECK(invoke("__b64_ntop", {input, 6, memory + 160, 9}) == 8 &&
                std::strcmp(reinterpret_cast<char*>(buffer + 160), "Zm9vYmFy") == 0 && buffer[169] == 0xa5);
    STONE_CHECK(invoke("__b64_ntop", {input, 6, memory + 160, 8}) == UINT32_MAX && errno == ENOSPC && buffer[168] == 0);
    STONE_CHECK(invoke("__b64_ntop", {0, 0, memory + 160, 1}) == 0 && buffer[160] == 0 && buffer[161] == 'm');
    STONE_CHECK(rejected([&] {
        invoke("__b64_ntop", {reinterpret_cast<uintptr_t>("host"), 4, memory + 160, 16});
    }));
    const auto domain = text(224, "www.example.com");
    std::memset(buffer + 288, 0xa5, 520);
    STONE_CHECK(invoke("res_mkquery", {0, domain, 1, 1, 0, 0, 0, memory + 288, 512}) == 33);
    const uint8_t question[]{3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
    STONE_CHECK(buffer[290] == 1 && buffer[292] == 0 && buffer[293] == 1 &&
                std::equal(std::begin(question), std::end(question), buffer + 300) && buffer[321] == 0xa5);
    std::array<uint8_t, 33> saved{};
    std::copy_n(buffer + 288, saved.size(), saved.begin());
    STONE_CHECK(invoke("res_mkquery", {0, domain, 1, 1, 0, 0, 0, memory + 288, 2}) == UINT32_MAX && errno == ENOSPC &&
                std::equal(saved.begin(), saved.end(), buffer + 288));
    STONE_CHECK(invoke("res_mkquery", {1, 0, 0, 0, 0, 0, 0, 0, 0}) == UINT32_MAX && errno == 95);
    STONE_CHECK(rejected([&] {
        invoke("res_mkquery", {0, reinterpret_cast<uintptr_t>("host"), 1, 1, 0, 0, 0, memory + 288, 512});
    }));
    char encoded[16]{};
    const auto encode = Guest<int (*)(const unsigned char*, size_t, char*, size_t)>(Libc("__b64_ntop"), "(LJLJ)I");
    STONE_CHECK(encode(reinterpret_cast<const unsigned char*>("foobar"), 6, encoded, sizeof(encoded)) == 8 &&
                std::strcmp(encoded, "Zm9vYmFy") == 0);
    const auto query = Guest<int (*)(int, const char*, int, int, const void*, int, const void*, unsigned char*, int)>(
        Libc("res_mkquery"), "(ILIILILLI)I");
    std::array<uint8_t, 512> packet{};
    STONE_CHECK(
        query(0, "www.example.com", 1, 1, nullptr, 0, nullptr, packet.data(), static_cast<int>(packet.size())) == 33 &&
        std::equal(std::begin(question), std::end(question), packet.begin() + 12));
    STONE_CHECK(invoke("__gnu_strerror_r", {38, memory + 160, 64}) == memory + 160 &&
                std::strcmp(reinterpret_cast<const char*>(memory + 160), "Function not implemented") == 0);
    buffer[160] = 0xa5;
    buffer[161] = 0xa5;
    errno = 17;
    const auto error = invoke("__gnu_strerror_r", {38, memory + 160, 1});
    STONE_CHECK(error == memory + 160 && buffer[160] == 0 && buffer[161] == 0xa5 && errno == 17);
    buffer[160] = 0xa5;
    STONE_CHECK(invoke("__gnu_strerror_r", {38, memory + 160, 0}) == memory + 160 && buffer[160] == 0xa5 &&
                errno == 17);
    STONE_CHECK(invoke("__gnu_strerror_r", {38, 0, 0}) == 0);
    const auto fixture =
        std::filesystem::current_path() / (L"arm64-fgets-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const auto file = CreateFileW(fixture.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    STONE_CHECK(file != INVALID_HANDLE_VALUE);
    const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDWR | _O_BINARY);
    if (fd < 0)
        CloseHandle(file);
    STONE_CHECK(fd >= 0);
    uintptr_t stream = 0;

    struct Close {
        std::function<void()> close;

        ~Close()
        {
            try {
                close();
            }
            catch (...) {
            }
        }
    } close{[&] {
        if (stream)
            invoke("fclose", {stream});
        else
            _close(fd);
    }};

    stream = invoke("fdopen", {static_cast<uint64_t>(fd), text(900, "w+")});
    STONE_CHECK(stream);
    const auto content = text(32, "first\nsecond\n");
    STONE_CHECK(invoke("fwrite", {content, 1, 13, stream}) == 13 && invoke("fseek", {stream, 0, 0}) == 0);
    std::memset(buffer + 928, 0xa5, 32);
    STONE_CHECK(invoke("__fgets_chk", {memory + 928, 4, stream, 4}) == memory + 928 &&
                std::strcmp(reinterpret_cast<char*>(buffer + 928), "fir") == 0 && buffer[932] == 0xa5);
    STONE_CHECK(rejected([&] {
                    invoke("__fgets_chk", {memory + 928, 8, stream, 4});
                }) &&
                invoke("ftell", {stream}) == 3);
    STONE_CHECK(rejected([&] {
                    invoke("__fgets_chk", {memory + 928, UINT32_MAX, stream, 32});
                }) &&
                invoke("ftell", {stream}) == 3);
    STONE_CHECK(invoke("__fgets_chk", {memory + 928, 8, stream, 8}) == memory + 928 &&
                std::strcmp(reinterpret_cast<char*>(buffer + 928), "st\n") == 0);
}

void CheckArm64ElfProtection()
{
    const auto make = [](bool readOnly, bool relro) {
        auto bytes = MakeArm64TlsElf();
        const auto put = [&](size_t offset, const auto& value) {
            std::memcpy(bytes.data() + offset, &value, sizeof(value));
        };
        put(0x1200, std::array<uint32_t, 2>{0x58000050, 0xd61f0200});
        put(0x1208, uint64_t{1});
        put(0x1280, std::array<uint32_t, 2>{0xf9000001, 0xd65f03c0}); // str x1,[x0]; ret.
        put(0x1dc, uint8_t{0x11});
        put(0x1e0, uint64_t{0x300});
        put(0x568, uint64_t{0});
        if (readOnly)
            put(sizeof(elf::Elf64Ehdr) + 4, uint32_t{4});
        if (relro)
            put(sizeof(elf::Elf64Ehdr) + 2 * sizeof(elf::Elf64Phdr),
                elf::Elf64Phdr{0x6474e552, 4, 0, 0, 0, 4096, 4096, 4096});
        auto image = std::make_unique<ElfImage>(bytes, ElfImage::Machine::Arm64, true);
        image->Protect(ElfImage::Execution::Translated);
        return image;
    };
    const auto invoke = [](const ElfImage& image, uintptr_t address, int flags) {
        const std::array<Arm64Argument, 3> args{{{'L', address}, {'J', 4096}, {'I', static_cast<uint64_t>(flags)}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'I',
            [](uintptr_t) -> std::string_view {
                return "mprotect";
            },
            nullptr, nullptr, args);
    };
    const auto image = make(false, false);
    const auto data = image->LoadBias(), code = data + 0x1000;
    const auto write = [&] {
        const std::array<Arm64Argument, 2> args{{{'L', data + 0x300}, {'J', 42}}};
        RunArm64Function(
            *image, data + 0x1280, 'V',
            [](uintptr_t) -> std::string_view {
                return {};
            },
            nullptr, nullptr, args);
    };
    STONE_CHECK(invoke(*image, data, 1) == 0 && image->DataAt(data + 0x300, 8, false) &&
                !image->DataAt(data + 0x300, 8, true));
    bool rejected = false;
    try {
        write();
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    STONE_CHECK(invoke(*image, data, 3) == 0 && image->DataAt(data + 0x300, 8, true));
    write();
    STONE_CHECK(invoke(*image, data, 1) == 0 && *reinterpret_cast<const uint64_t*>(data + 0x300) == 42);
    STONE_CHECK(invoke(*image, code, 3) == UINT32_MAX && !image->DataAt(code, 4, true) && image->ContainsCode(code, 4));
    for (const auto& protectedImage : {make(true, false), make(false, true)}) {
        STONE_CHECK(invoke(*protectedImage, protectedImage->LoadBias(), 1) == 0);
        STONE_CHECK(invoke(*protectedImage, protectedImage->LoadBias(), 3) == UINT32_MAX &&
                    !protectedImage->DataAt(protectedImage->LoadBias() + 0x300, 8, true));
    }
}

void CheckArm64Syslog()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    std::memcpy(bytes.data() + 0x1208, &token, 8);
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(3072, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    const auto text = [&](size_t offset, const std::string& value) {
        STONE_CHECK(offset + value.size() + 1 <= 3072);
        std::memcpy(reinterpret_cast<void*>(memory + offset), value.c_str(), value.size() + 1);
        return memory + offset;
    };
    const auto tag = text(0, "arm64-syslog"), format = text(64, "value=%d text=%s real=%.1f"),
               word = text(192, "guest");
    const auto invoke = [&](std::string_view name, std::initializer_list<Arm64Argument> args) {
        return RunArm64Function(image, image.LoadBias() + 0x1200, 'J',
                                [&](uintptr_t) -> std::string_view {
                                    return name;
                                },
                                nullptr, nullptr, {args.begin(), args.size()});
    };

    struct Record {
        int priority;
        std::string tag, text;
    };

    std::vector<Record> records;
    SetTranslatedAndroidLogger([&](const AndroidLogMessage* message) {
        STONE_CHECK(message->size == sizeof(*message) && !message->buffer && !message->file && !message->line);
        records.push_back({message->priority, message->tag, message->text});
        STONE_CHECK(invoke("setlogmask", {{'I', 0}})); // The logger can reenter without holding syslog state.
    });

    struct Reset {
        ~Reset()
        {
            SetTranslatedAndroidLogger({});
        }
    } reset;

    const auto previous = invoke("setlogmask", {{'I', 0xff}});
    invoke("openlog", {{'L', tag}, {'I', 0}, {'I', 8}});
    errno = 123;
    invoke("syslog", {{'I', 8 | 3}, {'L', format}, {'I', 42}, {'L', word}, {'D', std::bit_cast<uint64_t>(42.5)}});
    STONE_CHECK(records.size() == 1 && records.back().priority == 6 && records.back().tag == "arm64-syslog" &&
                records.back().text == "value=42 text=guest real=42.5" && errno == 123);
    text(0, "changed");
    for (unsigned priority = 0; priority < 8; ++priority) {
        invoke("syslog", {{'I', priority}, {'L', word}});
        STONE_CHECK(records.back().priority == (priority <= 3   ? 6
                                                : priority == 4 ? 5
                                                : priority <= 6 ? 4
                                                                : 3) &&
                    records.back().tag == "changed");
    }
    STONE_CHECK(invoke("setlogmask", {{'I', 1u << 4}}) == 0xff && invoke("setlogmask", {{'I', 0}}) == 1u << 4);
    const auto count = records.size();
    invoke("syslog", {{'I', 3}, {'L', 1}});
    invoke("vsyslog", {{'I', 7}, {'L', 1}, {'L', 1}});
    STONE_CHECK(records.size() == count);
    invoke("closelog", {});
    STONE_CHECK(invoke("setlogmask", {{'I', 0}}) == 1u << 4);
    invoke("syslog", {{'I', 4}, {'L', word}});
    STONE_CHECK(records.back().tag == "Stone");
    invoke("setlogmask", {{'I', 0xff}});
    const auto list = memory + 1600;
    const uint64_t fields[]{memory + 2048, memory + 1832, memory + 2032};
    const int32_t offsets[]{-16, -128};
    std::memcpy(reinterpret_cast<void*>(list), fields, sizeof(fields));
    std::memcpy(reinterpret_cast<void*>(list + 24), offsets, sizeof(offsets));
    const uint64_t gp[]{42, word}, fp = std::bit_cast<uint64_t>(42.5);
    std::memcpy(reinterpret_cast<void*>(memory + 1816), gp, sizeof(gp));
    std::memcpy(reinterpret_cast<void*>(memory + 1904), &fp, 8);
    invoke("vsyslog", {{'I', 6}, {'L', format}, {'L', list}});
    STONE_CHECK(records.back().text == "value=42 text=guest real=42.5");
    invoke("syslog", {{'I', 6}, {'L', text(256, std::string(1200, 'q'))}});
    STONE_CHECK(records.back().text == std::string(1023, 'q'));
    bool rejected = false;
    try {
        invoke("openlog", {{'L', reinterpret_cast<uintptr_t>("host-tag")}, {'I', 0}, {'I', 0}});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    invoke("openlog", {{'L', tag}, {'I', 32}, {'I', 0}});
    invoke("syslog", {{'I', 4}, {'L', text(64, "perror once\n")}});
    invoke("closelog", {});
    invoke("setlogmask", {{'I', previous}});
}

void CheckArm64VolumeStatistics()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xd2800030, 0xd61f0200};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto fixture =
        std::filesystem::current_path() / (L"arm64-statvfs-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const auto handle = CreateFileW(fixture.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    STONE_CHECK(handle != INVALID_HANDLE_VALUE);
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDWR | _O_BINARY);
    if (fd < 0)
        CloseHandle(handle);
    STONE_CHECK(fd >= 0);

    struct Close {
        int value;

        ~Close()
        {
            CloseAndroidFd(value);
        }
    } close{fd};

    const auto memory = AllocateArm64GuestMemory(2048, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    const auto output = memory + 16, path = memory + 256;
    const auto filename = fixture.string();
    STONE_CHECK(filename.size() < 700);
    std::memcpy(reinterpret_cast<void*>(path), filename.c_str(), filename.size() + 1);
    const auto invoke = [&](std::string_view name, uintptr_t source, uintptr_t destination) {
        const Arm64Argument arguments[]{{'J', source}, {'L', destination}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'I',
            [&](uintptr_t) -> std::string_view {
                return name;
            },
            nullptr, nullptr, arguments);
    };
    wchar_t root[32768]{};
    DWORD sectors = 0, sectorSize = 0, freeClusters = 0, totalClusters = 0, serial = 0, nameLimit = 0;
    ULARGE_INTEGER available{}, total{}, freeBytes{};
    STONE_CHECK(GetVolumePathNameW(fixture.c_str(), root, 32768) &&
                GetDiskFreeSpaceW(root, &sectors, &sectorSize, &freeClusters, &totalClusters) &&
                GetDiskFreeSpaceExW(root, &available, &total, &freeBytes) &&
                GetVolumeInformationW(root, nullptr, 0, &serial, &nameLimit, nullptr, nullptr, 0));
    const auto verify = [&](const AndroidStatVfs& status) {
        STONE_CHECK(status.blockSize == uint64_t{sectors} * sectorSize && status.fragmentSize == status.blockSize &&
                    status.blocks == total.QuadPart / status.fragmentSize && status.freeBlocks <= status.blocks &&
                    status.availableBlocks <= status.freeBlocks && status.fsid == serial &&
                    status.nameLimit == nameLimit);
        for (const auto value : status.reserved)
            STONE_CHECK(value == 0);
    };

    struct Guard {
        uint64_t before;
        AndroidStatVfs status;
        uint64_t after;
    } direct;

    std::memset(&direct, 0xa5, sizeof(direct));
    const auto host = reinterpret_cast<int (*)(const char*, AndroidStatVfs*)>(AndroidLibcImport("statvfs", true));
    STONE_CHECK(host(filename.c_str(), &direct.status) == 0);
    verify(direct.status);
    constexpr uint64_t canary = 0xa5a5a5a5a5a5a5a5;
    STONE_CHECK(direct.before == canary && direct.after == canary);
    for (const auto name : {"statvfs", "statvfs64", "fstatvfs", "fstatvfs64"}) {
        std::memset(reinterpret_cast<void*>(memory), 0xa5, 144);
        STONE_CHECK(invoke(name, std::string_view(name).starts_with('f') ? uintptr_t(fd) : path, output) == 0);
        verify(*reinterpret_cast<const AndroidStatVfs*>(output));
        STONE_CHECK(*reinterpret_cast<uint64_t*>(output - 8) == canary &&
                    *reinterpret_cast<uint64_t*>(output + sizeof(AndroidStatVfs)) == canary);
    }
    const auto missing = path + 768;
    constexpr char absent[] = "arm64-statvfs-nonexistent/path";
    std::memcpy(reinterpret_cast<void*>(missing), absent, sizeof(absent));
    std::memset(reinterpret_cast<void*>(output), 0xa5, sizeof(AndroidStatVfs));
    STONE_CHECK(invoke("statvfs", missing, output) == UINT32_MAX && errno == ENOENT);
    STONE_CHECK(invoke("fstatvfs", UINT32_MAX, output) == UINT32_MAX && errno == EBADF);
    for (size_t i = 0; i < sizeof(AndroidStatVfs); ++i)
        STONE_CHECK(reinterpret_cast<uint8_t*>(output)[i] == 0xa5);
    STONE_CHECK(invoke("statvfs", 0, output) == UINT32_MAX && errno == EFAULT);
    STONE_CHECK(invoke("statvfs", path, 0) == UINT32_MAX && errno == EFAULT);
    const auto rejected = [](auto operation) {
        try {
            operation();
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    STONE_CHECK(rejected([&] {
        invoke("statvfs", reinterpret_cast<uintptr_t>(filename.c_str()), output);
    }));
    STONE_CHECK(rejected([&] {
        invoke("fstatvfs", fd, reinterpret_cast<uintptr_t>(&direct.status));
    }));
    STONE_CHECK(rejected([&] {
        invoke("statvfs", path, memory + 2040);
    }));
    auto* readOnly = AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0);
    STONE_CHECK(readOnly != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* value;

        ~Unmap()
        {
            AndroidUnmapArm64(value, 4096);
        }
    } unmap{readOnly};

    std::memcpy(readOnly, filename.c_str(), filename.size() + 1);
    STONE_CHECK(AndroidProtectArm64(readOnly, 4096, 1) == 0);
    STONE_CHECK(invoke("statvfs64", reinterpret_cast<uintptr_t>(readOnly), output) == 0);
    STONE_CHECK(rejected([&] {
        invoke("fstatvfs64", fd, reinterpret_cast<uintptr_t>(readOnly));
    }));
}

void CheckArm64SetExtendedAttributes()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xd2800030, 0xd61f0200};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto fixture =
        std::filesystem::current_path() / (L"arm64-xattr-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const auto file = CreateFileW(fixture.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    STONE_CHECK(file != INVALID_HANDLE_VALUE);
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDWR | _O_BINARY);
    if (fd < 0)
        CloseHandle(file);
    STONE_CHECK(fd >= 0);

    struct Close {
        int fd;

        ~Close()
        {
            CloseAndroidFd(fd);
        }
    } close{fd};

    const auto memory = AllocateArm64GuestMemory(4096, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t p;

        ~Free()
        {
            FreeArm64GuestMemory(p);
        }
    } free{memory};

    const auto path = memory, name = memory + 1024, value = memory + 2048;
    const auto filename = fixture.string();
    STONE_CHECK(filename.size() < 1024);
    std::memcpy(reinterpret_cast<void*>(path), filename.c_str(), filename.size() + 1);
    std::memcpy(reinterpret_cast<void*>(name), "user.stone-test", 16);
    std::memset(reinterpret_cast<void*>(value), 0xa5, 16);
    const auto invoke = [&](std::string_view symbol, uintptr_t source, uintptr_t attribute, uintptr_t data,
                            uint64_t size, int flags) {
        const Arm64Argument args[]{{'J', source}, {'L', attribute}, {'L', data}, {'J', size}, {'I', uint32_t(flags)}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'I',
            [&](uintptr_t) -> std::string_view {
                return symbol;
            },
            nullptr, nullptr, args);
    };
    const auto rejected = [](auto action) {
        try {
            action();
        }
        catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    for (const auto symbol : {"setxattr", "lsetxattr", "fsetxattr"}) {
        const bool descriptor = std::string_view(symbol) == "fsetxattr";
        const auto source = descriptor ? uintptr_t(fd) : path;
        STONE_CHECK(invoke(symbol, source, name, value, 16, 0) == UINT32_MAX && errno == 95);
        STONE_CHECK(invoke(symbol, source, name, 0, 0, 2) == UINT32_MAX && errno == 95);
        STONE_CHECK(invoke(symbol, source, name, value, 16, 3) == UINT32_MAX && errno == EINVAL);
        STONE_CHECK(invoke(symbol, source, 0, value, 16, 0) == UINT32_MAX && errno == EFAULT);
        STONE_CHECK(invoke(symbol, source, name, 0, 16, 0) == UINT32_MAX && errno == EFAULT);
        STONE_CHECK(rejected([&] {
            invoke(symbol, source, name, reinterpret_cast<uintptr_t>(&file), 1, 0);
        }));
        STONE_CHECK(rejected([&] {
            invoke(symbol, source, name, memory + 4090, 16, 0);
        }));
        STONE_CHECK(rejected([&] {
            invoke(symbol, source, name, value, uint64_t{UINT_MAX} + 1, 0);
        }));
    }
    STONE_CHECK(invoke("fsetxattr", UINT32_MAX, name, value, 16, 0) == UINT32_MAX && errno == EBADF);
    STONE_CHECK(invoke("setxattr", 0, name, value, 16, 0) == UINT32_MAX && errno == EFAULT);
    STONE_CHECK(rejected([&] {
        invoke("setxattr", reinterpret_cast<uintptr_t>(filename.c_str()), name, value, 16, 0);
    }));
    auto* readOnly = AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0);
    STONE_CHECK(readOnly != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* p;

        ~Unmap()
        {
            AndroidUnmapArm64(p, 4096);
        }
    } unmap{readOnly};

    std::memcpy(readOnly, "read-only", 10);
    STONE_CHECK(AndroidProtectArm64(readOnly, 4096, 1) == 0);
    STONE_CHECK(invoke("setxattr", path, name, reinterpret_cast<uintptr_t>(readOnly), 10, 1) == UINT32_MAX &&
                errno == 95);
    for (unsigned i = 0; i < 16; ++i)
        STONE_CHECK(reinterpret_cast<const uint8_t*>(value)[i] == 0xa5);
}

void CheckArm64Fcntl()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xd2800030, 0xd61f0200};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto fixture =
        std::filesystem::current_path() / (L"arm64-fcntl-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    const auto file = CreateFileW(fixture.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    STONE_CHECK(file != INVALID_HANDLE_VALUE);
    const auto fd = _open_osfhandle(reinterpret_cast<intptr_t>(file), _O_RDWR | _O_BINARY);
    if (fd < 0)
        CloseHandle(file);
    STONE_CHECK(fd >= 0);

    struct Close {
        int fd;

        ~Close()
        {
            CloseAndroidFd(fd);
        }
    } close{fd};

    const auto memory = AllocateArm64GuestMemory(96, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    std::memset(reinterpret_cast<void*>(memory), 0xa5, 96);
    AndroidFileRange range{1, 0, 32, 16, 0};
    const auto argument = memory + 16;
    const auto store = [&] {
        std::memcpy(reinterpret_cast<void*>(argument), &range, sizeof(range));
    };
    const auto invoke = [&](int command, uintptr_t value) {
        const Arm64Argument args[]{
            {'I', static_cast<uint32_t>(fd)}, {'I', static_cast<uint32_t>(command)}, {'J', value}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'I',
            [](uintptr_t) -> std::string_view {
                return "fcntl";
            },
            nullptr, nullptr, args);
    };
    store();
    STONE_CHECK(invoke(5, argument) == 0);
    AndroidFileRange query{};
    std::memcpy(&query, reinterpret_cast<void*>(argument), sizeof(query));
    STONE_CHECK(query.type == 2 && query.origin == 0 && query.start == 32 && query.length == 16);
    STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 8) == 0xa5a5a5a5a5a5a5a5 &&
                *reinterpret_cast<uint64_t*>(memory + 48) == 0xa5a5a5a5a5a5a5a5);
    store();
    STONE_CHECK(invoke(6, argument) == 0);
    OVERLAPPED overlap{};
    overlap.Offset = 32;
    STONE_CHECK(!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &overlap) &&
                GetLastError() == ERROR_LOCK_VIOLATION);
    overlap.Offset = 0;
    STONE_CHECK(LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &overlap));
    STONE_CHECK(UnlockFileEx(file, 0, 16, 0, &overlap));
    range.type = 2;
    store();
    STONE_CHECK(invoke(7, argument) == 0);
    overlap.Offset = 32;
    STONE_CHECK(LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 16, 0, &overlap));
    STONE_CHECK(UnlockFileEx(file, 0, 16, 0, &overlap));
    const auto mapping = AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0);
    STONE_CHECK(mapping != reinterpret_cast<void*>(-1));

    struct Unmap {
        void* value;

        ~Unmap()
        {
            AndroidUnmapArm64(value, 4096);
        }
    } unmap{mapping};

    range.type = 1;
    std::memcpy(mapping, &range, sizeof(range));
    STONE_CHECK(AndroidProtectArm64(mapping, 4096, 1) == 0);
    STONE_CHECK(invoke(6, reinterpret_cast<uintptr_t>(mapping)) == 0); // SETLK reads only.
    bool rejected = false;
    try {
        invoke(5, reinterpret_cast<uintptr_t>(mapping));
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    range.type = 2;
    store();
    STONE_CHECK(invoke(6, argument) == 0);
    for (const auto value : {reinterpret_cast<uintptr_t>(&range), memory + 80}) {
        rejected = false;
        try {
            invoke(6, value);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    }
    STONE_CHECK(invoke(5, 0) == UINT32_MAX && errno == EFAULT);
    STONE_CHECK(invoke(2, 1) == 0 && invoke(1, 0) == 1 && invoke(2, 0) == 0 && invoke(1, 0) == 0);
}

void CheckArm64LogAssert()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t caller[]{0xd2800030, 0xd61f0200};
    std::memcpy(bytes.data() + 0x1200, caller, sizeof(caller));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(3072, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    const auto text = [&](size_t offset, const std::string& value) {
        STONE_CHECK(offset + value.size() + 1 <= 3072);
        std::memcpy(reinterpret_cast<void*>(memory + offset), value.c_str(), value.size() + 1);
        return memory + offset;
    };
    const auto condition = text(0, "value == 42"), tag = text(64, "assert-probe"),
               format = text(128, "actual=%d %.1f %s");
    std::vector<std::string> logged, aborted;
    SetTranslatedAndroidLogger([&](const AndroidLogMessage* message) {
        STONE_CHECK(message->priority == 7 && !message->buffer);
        logged.emplace_back(message->text);
    });
    SetTranslatedAndroidAborter([&](const char* message) {
        aborted.emplace_back(message);
    });

    struct Reset {
        ~Reset()
        {
            SetTranslatedAndroidLogger({});
            SetTranslatedAndroidAborter({});
        }
    } reset;

    const auto invoke = [&](std::initializer_list<Arm64Argument> args) {
        try {
            RunArm64Function(image, image.LoadBias() + 0x1200, 'V',
                             [](uintptr_t) -> std::string_view {
                                 return "__android_log_assert";
                             },
                             nullptr, nullptr, {args.begin(), args.size()});
        }
        catch (const std::runtime_error& error) {
            STONE_CHECK(std::string_view{error.what()}.find("guest log assertion:") != std::string_view::npos);
            return;
        }
        STONE_CHECK(false); // A returning aborter must never let the guest continue.
    };
    invoke({{'L', condition},
            {'L', tag},
            {'L', format},
            {'I', 17},
            {'D', std::bit_cast<uint64_t>(42.5)},
            {'L', text(256, "guest")}});
    STONE_CHECK(logged.back() == "actual=17 42.5 guest" && aborted == logged);
    invoke({{'L', condition}, {'L', tag}, {'L', 0}});
    STONE_CHECK(logged.back() == "Assertion failed: value == 42" && aborted == logged);
    invoke({{'L', 0}, {'L', 0}, {'L', 0}});
    STONE_CHECK(logged.back() == "Assertion failed" && aborted == logged);
    invoke({{'L', 0}, {'L', tag}, {'L', text(512, std::string(1200, 'q'))}});
    STONE_CHECK(logged.back() == std::string(1023, 'q') && aborted == logged);
    SetTranslatedAndroidAborter({});
    invoke({{'L', 0}, {'L', tag}, {'L', text(128, "no registered aborter")}});
    STONE_CHECK(logged.back() == "no registered aborter" && aborted.size() + 1 == logged.size());
    const auto previous = logged.size();
    bool rejected = false;
    try {
        const Arm64Argument args[]{{'L', 0}, {'L', reinterpret_cast<uintptr_t>("host-tag")}, {'L', 0}};
        RunArm64Function(
            image, image.LoadBias() + 0x1200, 'V',
            [](uintptr_t) -> std::string_view {
                return "__android_log_assert";
            },
            nullptr, nullptr, args);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && logged.size() == previous);
}

void CheckArm64CfiAndPerror()
{
    auto bytes = MakeArm64TlsElf();
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    constexpr char symbol[] = "\0__cfi_check";
    put(0x1a0, symbol);
    put(0x518, uint64_t{sizeof(symbol)});
    put(0x1dc, uint8_t{0x12});
    put(0x1de, uint16_t{1});
    put(0x1e0, uint64_t{0x1280});
    put(0x1e8, uint64_t{16});
    put(0x568, uint64_t{0}); // No TLS relocation in this fixture.
    const auto delta = uint32_t(0x300 - 0x1280) & 0x1fffff;
    put(0x1280, std::array<uint32_t, 4>{0x10000003 | ((delta & 3) << 29) | ((delta >> 2) << 5), 0xa9000460, 0xf9000862,
                                        0xd65f03c0}); // Record all three __cfi_check arguments.
    put(0x1200, std::array<uint32_t, 2>{0x58000050, 0xd61f0200});
    put(0x1208, uint64_t{1});
    put(0x1240, std::array<uint32_t, 2>{0x58000050, 0xd61f0200});
    put(0x1248, uint64_t{2});
    put(0x12c0, std::array<uint32_t, 2>{0x58000050, 0xd61f0200});
    put(0x12c8, uint64_t{3});
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate([](std::string_view) -> uintptr_t {
        return 0;
    });
    image.Protect(ElfImage::Execution::Translated);
    const auto invoke = [&](size_t entry, std::span<const Arm64Argument> arguments) {
        return RunArm64Function(
            image, image.LoadBias() + entry, 'V',
            [](uintptr_t token) -> std::string_view {
                return token == 1 ? "perror" : token == 2 ? "__cfi_slowpath" : token == 3 ? "__cfi_slowpath_diag" : "";
            },
            nullptr, nullptr, arguments);
    };
    const auto code = image.LoadBias() + 0x1280, data = image.LoadBias() + 0x300;
    const std::array<Arm64Argument, 3> cfi{{{'J', 0x123456789abcdef0}, {'L', code}, {'L', data + 64}}};
    invoke(0x1240, cfi);
    const auto* record = reinterpret_cast<const uint64_t*>(data);
    STONE_CHECK(record[0] == cfi[0].bits && record[1] == code && record[2] == 0);
    invoke(0x12c0, cfi);
    STONE_CHECK(record[0] == cfi[0].bits && record[1] == code && record[2] == data + 64);
    auto dataCfi = cfi;
    dataCfi[1].bits = data + 32;
    invoke(0x1240, dataCfi);
    STONE_CHECK(record[1] == data + 32 && record[2] == 0);
    bool rejected = false;
    auto invalid = cfi;
    invalid[1].bits = reinterpret_cast<uintptr_t>(&bytes);
    try {
        invoke(0x1240, invalid);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    // A loaded, uninstrumented ELF is accepted; unknown targets still fail.
    bytes[0x1a1] = 'x';
    ElfImage plain(bytes, ElfImage::Machine::Arm64, true);
    plain.Protect(ElfImage::Execution::Translated);
    const std::array<Arm64Argument, 2> plainArgs{{{'J', 1}, {'L', plain.LoadBias() + 0x1280}}};
    RunArm64Function(
        plain, plain.LoadBias() + 0x1240, 'V',
        [](uintptr_t) -> std::string_view {
            return "__cfi_slowpath";
        },
        nullptr, nullptr, plainArgs);
    constexpr char prefix[] = "ARM64 perror probe";
    std::memcpy(reinterpret_cast<void*>(data + 64), prefix, sizeof(prefix));
    errno = 38;
    const std::array<Arm64Argument, 1> errorArgs{{{'L', data + 64}}};
    invoke(0x1200, errorArgs);
    STONE_CHECK(errno == 38);
    const std::array<Arm64Argument, 1> nullArgs{{{'L', 0}}};
    invoke(0x1200, nullArgs);
    STONE_CHECK(errno == 38);
    rejected = false;
    const std::array<Arm64Argument, 1> hostArgs{{{'L', reinterpret_cast<uintptr_t>(prefix)}}};
    try {
        invoke(0x1200, hostArgs);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
}
}
