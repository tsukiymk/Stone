#include "../support/test-suite.h"

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

void CheckArm64FunctionBridge()
{
    // Cached host entries require a process-lifetime ELF owner.
    static const auto image = [] {
        auto bytes = MakeArm64TlsElf();
        const auto put = [&](size_t at, const auto& value) {
            std::memcpy(bytes.data() + at, &value, sizeof(value));
        };
        // Android libc++ can publish X-only code. The host interpreter needs
        // readable storage without granting guest data access or host execution.
        put(sizeof(elf::Elf64Ehdr) + 3 * sizeof(elf::Elf64Phdr) + 4, uint32_t{1});
        std::vector<uint32_t> code;
        for (unsigned reg = 1; reg < 8; ++reg)
            code.push_back(0x8b000000 | (reg << 16)); // ADD x0,x0,xN.
        code.push_back(0xf94003e8);
        code.push_back(0x8b080000); // Add spilled ninth integer.
        for (unsigned reg = 1; reg < 8; ++reg)
            code.push_back(0x1e602800 | (reg << 16)); // FADD d0,d0,dN.
        code.insert(code.end(), {0xfd4007e8, 0x1e682800, 0x9e620001, 0x1e612800, 0xd65f03c0});
        std::memcpy(bytes.data() + 0x1100, code.data(), code.size() * sizeof(code[0]));
        put(0x1200, std::array<uint32_t, 3>{0xf9000001, 0xf9400000, 0xd65f03c0}); // *x0=x1; return *x0.
        put(0x1280, std::array<uint32_t, 2>{0x1e202800, 0xd65f03c0});             // Return s0+s0.
        put(0x12c0, uint32_t{0xd65f03c0});                                        // Pointer identity.
        put(0x1340, std::array<uint32_t, 5>{0xd37efc00, 0xf9400022, 0x8b020000, 0x39404822, 0xd65f03c0});
        put(0x1380, std::array<uint32_t, 8>{0xa9bf7bfd, 0xaa0003f0, 0xaa0103e0, 0xaa0203e1, 0xaa0303e2, 0xd63f0200,
                                            0xa8c17bfd, 0xd65f03c0}); // Call x0 with x1..x3.
        auto result = std::make_unique<ElfImage>(bytes, ElfImage::Machine::Arm64, true);
        result->Relocate(
            [](std::string_view) -> uintptr_t {
                return 0;
            },
            nullptr, {}, {}, Arm64TlsDescriptorResolver());
        result->Protect(ElfImage::Execution::Translated);
        return result;
    }();
    const auto code = image->LoadBias() + 0x1100;
    STONE_CHECK(image->ContainsCode(code, 4) && image->CodeAt(code, 4).size() == 4);
    STONE_CHECK(!image->DataAt(code, 4, false) && !image->DataAt(code, 4, true));
    MEMORY_BASIC_INFORMATION region{};
    STONE_CHECK(VirtualQuery(reinterpret_cast<const void*>(code), &region, sizeof(region)));
    STONE_CHECK(region.Protect == PAGE_READONLY);
    using Mixed = double (*)(uint64_t, double, uint64_t, double, uint64_t, double, uint64_t, double, uint64_t, double,
                             uint64_t, double, uint64_t, double, uint64_t, double, uint64_t, double);
    const auto mixed =
        reinterpret_cast<Mixed>(MakeArm64FunctionThunk(*image, image->LoadBias() + 0x1100, "(JDJDJDJDJDJDJDJDJD)D"));
    STONE_CHECK(mixed(1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5, 5, 5.5, 6, 6.5, 7, 7.5, 8, 8.5, 9, 9.5) == 94.5);
    STONE_CHECK(reinterpret_cast<float (*)(float)>(MakeArm64FunctionThunk(*image, image->LoadBias() + 0x1280, "(F)F"))(
                    1.5f) == 3.0f);
    const auto memory = AllocateArm64GuestMemory(256, true);

    struct Free {
        uintptr_t memory;

        ~Free()
        {
            FreeArm64GuestMemory(memory);
        }
    } free{memory};

    STONE_CHECK(memory);
    const auto pointer = reinterpret_cast<void*>(memory);
    const auto identity =
        reinterpret_cast<void* (*)(void*)>(MakeArm64FunctionThunk(*image, image->LoadBias() + 0x12c0, "(L)L"));
    STONE_CHECK(identity(pointer) == pointer);
    STONE_CHECK(RunArm64IfuncResolver(
                    *image, image->LoadBias() + 0x1340,
                    [](uintptr_t) -> std::string_view {
                        return {};
                    },
                    nullptr) == 25);
    const auto write = reinterpret_cast<uint64_t (*)(void*, uint64_t)>(
        MakeArm64FunctionThunk(*image, image->LoadBias() + 0x1200, "(LJ)J"));
    STONE_CHECK(write(pointer, 42) == 42 && *reinterpret_cast<const uint64_t*>(memory) == 42);
    uint64_t host = 7;
    bool rejected = false;
    try {
        write(&host, 42);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected && host == 7);
    const auto invokeImport = [&](uint64_t token, uint64_t first, uint64_t second = 0, uint64_t third = 0) {
        const std::array<Arm64Argument, 4> args{{{'J', token}, {'J', first}, {'J', second}, {'J', third}}};
        return RunArm64Function(
            *image, image->LoadBias() + 0x1380, 'J',
            [](uintptr_t target) -> std::string_view {
                if (target == 1)
                    return "__android_log_is_loggable";
                if (target == 2)
                    return "__android_log_write_log_message";
                if (target == 3)
                    return "mremap";
                return {};
            },
            nullptr, nullptr, args);
    };
    STONE_CHECK(invokeImport(1, 5, 0, 2) == 1 && invokeImport(1, 0, 0, 2) == 0);
    std::memcpy(reinterpret_cast<void*>(memory + 64), "arm64-log-bridge", 17);
    AndroidLogMessage message{sizeof(message), 0, 5, nullptr, nullptr, 23, reinterpret_cast<const char*>(memory + 64)};
    std::memcpy(pointer, &message, sizeof(message));
    invokeImport(2, memory);
    message.text = "forbidden-host-text";
    std::memcpy(pointer, &message, sizeof(message));
    rejected = false;
    try {
        invokeImport(2, memory);
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
    STONE_CHECK(invokeImport(3, 0) == UINT64_MAX);
    for (const auto signature : {"", "()", "(L", "()LL", "(V)I", "(Q)I", "(L;)I"}) {
        rejected = false;
        try {
            MakeArm64FunctionThunk(*image, image->LoadBias() + 0x12c0, signature);
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    }
}

void CheckArm64JniBuffers()
{
    const auto rejected = [](auto operation) {
        try {
            operation();
        }
        catch (const std::exception&) {
            return true;
        }
        return false;
    };
    STONE_CHECK(detail::ArtJniMethodParameterCount("([[Ljava/lang/String;[IZBCSJFD)Ljava/lang/Object;") == 9);
    STONE_CHECK(detail::ArtJniMethodParameterCount("()V") == 0);
    STONE_CHECK(rejected([] {
                    detail::ArtJniMethodParameterCount("([V)V");
                }) &&
                rejected([] {
                    detail::ArtJniMethodParameterCount("(I)VI");
                }) &&
                rejected([] {
                    detail::ArtJniMethodParameterCount(nullptr);
                }));
    uintptr_t allocation = 0;
    jint read[3]{};
    {
        detail::ArtJniCallStorage storage;
        const jint input[]{12, 14, 16};
        allocation = reinterpret_cast<uintptr_t>(storage.CopyBytes(input, storage.ByteCount(3, sizeof(jint))));
        STONE_CHECK(allocation != reinterpret_cast<uintptr_t>(input) &&
                    ReadArm64GuestAllocation(allocation, read, sizeof(read)) &&
                    std::equal(std::begin(input), std::end(input), read));
        STONE_CHECK(!storage.CopyBytes(nullptr, 0));
        STONE_CHECK(rejected([&] {
                        storage.CopyBytes(nullptr, 1);
                    }) &&
                    rejected([&] {
                        storage.ByteCount(-1, 1);
                    }) &&
                    rejected([&] {
                        storage.ByteCount(INT_MAX, 8);
                    }) &&
                    rejected([&] {
                        storage.Buffer(size_t{UINT_MAX} + 1);
                    }));
        const auto method = reinterpret_cast<jmethodID>(0x5e570001);
        const jvalue values[]{jvalue{.d = 42.5}, jvalue{.l = reinterpret_cast<jobject>(0x55)}, jvalue{.j = -17}};
        STONE_CHECK(rejected([&] {
            storage.CopyValues(method, values);
        }));
        detail::RecordArtJniMethod(method, 3);
        const auto* copied = storage.CopyValues(method, values);
        STONE_CHECK(copied != values && copied[0].d == 42.5 && copied[1].l == values[1].l && copied[2].j == -17);
        STONE_CHECK(rejected([&] {
            storage.CopyValues(method, nullptr);
        }));
        detail::RecordArtJniMethod(method, 0);
        STONE_CHECK(!storage.CopyValues(method, nullptr));
        STONE_CHECK(rejected([&] {
            detail::RecordArtJniMethod(method, 256);
        }));
        STONE_CHECK(rejected([&] {
            storage.Prepare<const jint*, true, false>(input);
        }));
    }
    STONE_CHECK(!ReadArm64GuestAllocation(allocation, read, sizeof(read)));
}

void CheckArm64HostJni()
{
    const auto mixed = +[](uint64_t a0, double f0, uint64_t a1, double f1, uint64_t a2, double f2, uint64_t a3,
                           double f3, uint64_t a4, double f4, uint64_t a5, double f5, uint64_t a6, double f6,
                           uint64_t a7, double f7, uint64_t a8, double f8) -> double {
        return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8;
    };
    const auto source =
        ImportAndroidScalars<decltype(mixed)>("ARM64 host JNI test mixed", reinterpret_cast<uintptr_t>(mixed));
    const auto token = PrepareArm64HostJniTarget("mixed", "(JDJDJDJDJDJDJDJDJD)D", source);
    STONE_CHECK(token && token != source && FindArm64HostJniTarget(token) && !FindArm64HostJniTarget(source));
    auto bytes = MakeArm64TlsElf();
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put(0x1200, std::array<uint32_t, 2>{0x58000050, 0xd61f0200}); // ldr x16,literal; br x16.
    put(0x1208, token);
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    std::vector<Arm64Argument> arguments;
    for (uint64_t i = 1; i <= 9; ++i) {
        arguments.push_back({'J', i});
        arguments.push_back({'D', std::bit_cast<uint64_t>(i + 0.5)});
    }
    const auto invoke = [&](std::span<const Arm64Argument> values) {
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'D',
            [](uintptr_t) -> std::string_view {
                return {};
            },
            nullptr, nullptr, values);
    };
    STONE_CHECK(std::bit_cast<double>(invoke(arguments)) == 94.5);
    const auto rejected = [](auto operation) {
        try {
            operation();
        }
        catch (const std::exception&) {
            return true;
        }
        return false;
    };
    STONE_CHECK(rejected([&] {
        PrepareArm64HostJniTarget("mixed", "(J)D", source);
    }));
    STONE_CHECK(rejected([&] {
        PrepareArm64HostJniTarget("mixed", "(JDJDJDJDJDJDJDJDJD)I", source);
    }));
    STONE_CHECK(rejected([&] {
        PrepareArm64HostJniTarget("mixed", "()D", reinterpret_cast<uintptr_t>(mixed));
    }));
    STONE_CHECK(rejected([&] {
        PrepareArm64HostJniTarget("mixed", "()D", Import("ARM64 host JNI untyped", mixed));
    }));
    JNIEnv fake{};
    STONE_CHECK(rejected([&] {
        Arm64ArtJniEntry(&fake, offsetof(JNINativeInterface_, FindClass));
    }));
    const auto memory = AllocateArm64GuestMemory(sizeof(JNIEnv), true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t memory;

        ~Free()
        {
            FreeArm64GuestMemory(memory);
        }
    } free{memory};

    JNINativeInterface_ table{};
    reinterpret_cast<JNIEnv*>(memory)->functions = &table;
    STONE_CHECK(rejected([&] {
        Arm64ArtJniEntry(reinterpret_cast<JNIEnv*>(memory), offsetof(JNINativeInterface_, FindClass));
    }));
    detail::ArtJniCallStorage storage;
    STONE_CHECK(rejected([&] {
                    storage.CopyMethods(nullptr, 1);
                }) &&
                rejected([&] {
                    storage.CopyMethods(nullptr, -1);
                }));
    const JNINativeMethod method{const_cast<char*>("mixed"), const_cast<char*>("(JDJDJDJDJDJDJDJDJD)D"),
                                 reinterpret_cast<void*>(source)};
    const auto* copied = storage.CopyMethods(&method, 1);
    STONE_CHECK(copied != &method && copied->name != method.name && copied->signature != method.signature &&
                std::strcmp(copied->name, method.name) == 0 && std::strcmp(copied->signature, method.signature) == 0 &&
                reinterpret_cast<uintptr_t>(copied->fnPtr) == token);
    STONE_CHECK(!storage.CopyMethods(nullptr, 0));
}

void CheckTypedImportPrototypes()
{
    const auto mixed = +[](double a, int b, float c, int64_t d, double e, float f, int* object, double h) {
        return a + b + c + static_cast<double>(d) + e + f + *object + h;
    };
    const auto token =
        ImportAndroidScalars<decltype(mixed)>("typed-prototype-mixed", reinterpret_cast<uintptr_t>(mixed));
    const auto info = FindAndroidScalarImport(token);
    STONE_CHECK(info && info->address == reinterpret_cast<uintptr_t>(mixed) && info->invoke &&
                std::string_view(info->prototype.parameters.data(), info->prototype.parameters.size()) == "DIFJDFLD" &&
                info->prototype.result == 'D' && !info->prototype.jniEnvironment);
    int object = 9;
    const auto call = Guest<std::remove_cv_t<decltype(mixed)>>(token, "(DIFJDFLD)D");
    STONE_CHECK(call(1.25, -7, 2.5f, -13, 6.75, 3.5f, &object, 41.0) == 44.0);
    const auto many = +[](uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,
                          uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10, uint64_t a11, uint64_t a12, uint64_t a13,
                          uint64_t a14, uint64_t a15, uint64_t a16, uint64_t a17, uint64_t a18, uint64_t a19,
                          uint64_t a20, uint64_t a21, uint64_t a22, uint64_t a23, uint64_t a24, uint64_t a25,
                          uint64_t a26, uint64_t a27, uint64_t a28, uint64_t a29, uint64_t a30) {
        const uint64_t values[]{a0,  a1,  a2,  a3,  a4,  a5,  a6,  a7,  a8,  a9,  a10, a11, a12, a13, a14, a15,
                                a16, a17, a18, a19, a20, a21, a22, a23, a24, a25, a26, a27, a28, a29, a30};
        uint64_t sum = 0;
        for (size_t i = 0; i < std::size(values); ++i)
            sum += (i + 1) * values[i];
        return sum;
    };
    const auto manyToken =
        ImportAndroidScalars<decltype(many)>("typed-prototype-many", reinterpret_cast<uintptr_t>(many));
    const auto manyInfo = FindAndroidScalarImport(manyToken);
    STONE_CHECK(manyInfo && manyInfo->prototype.parameters.size() == 31 && manyInfo->prototype.result == 'J');
    const auto manyCall = Guest<std::remove_cv_t<decltype(many)>>(manyToken, "(" + std::string(31, 'J') + ")J");
    STONE_CHECK(manyCall(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
                         27, 28, 29, 30, 31) == 10416);
    const auto narrow = +[](int8_t b, uint16_t c, int16_t s, bool z) {
        return int64_t(b) * 1000000 + int64_t(c) * 100 + s + z;
    };
    const auto narrowToken =
        ImportAndroidScalars<decltype(narrow)>("typed-prototype-narrow", reinterpret_cast<uintptr_t>(narrow));
    const auto narrowInfo = FindAndroidScalarImport(narrowToken);
    STONE_CHECK(narrowInfo && std::string_view(narrowInfo->prototype.parameters.data(), 4) == "BSSB");
    STONE_CHECK(Guest<std::remove_cv_t<decltype(narrow)>>(narrowToken, "(BCSZ)J")(-7, 0xabcd, -321, true) == -2602220);
    const auto jni = +[](JNIEnv*, jclass, jint value) {
        return value;
    };
    const auto jniToken = ImportAndroidScalars<decltype(jni)>("typed-prototype-jni", reinterpret_cast<uintptr_t>(jni));
    const auto jniInfo = FindAndroidScalarImport(jniToken);
    STONE_CHECK(jniInfo && jniInfo->prototype.jniEnvironment && jniInfo->prototype.result == 'I' &&
                std::string_view(jniInfo->prototype.parameters.data(), jniInfo->prototype.parameters.size()) == "LLI");
    const auto noArgs = +[]() noexcept {
        return 42;
    };
    const auto noArgsToken =
        ImportAndroidScalars<decltype(noArgs)>("typed-prototype-noexcept", reinterpret_cast<uintptr_t>(noArgs));
    const auto noArgsInfo = FindAndroidScalarImport(noArgsToken);
    STONE_CHECK(noArgsInfo && noArgsInfo->prototype.parameters.empty() && !noArgsInfo->prototype.jniEnvironment &&
                Guest<std::remove_cv_t<decltype(noArgs)>>(noArgsToken, "()I")() == 42);
}

void CheckTypedImportIdentityAndLifetime()
{
    const auto function = +[]() -> uint64_t {
        return 42;
    };
    const auto address = reinterpret_cast<uintptr_t>(function);
    const auto ordinary = Import("typed-prototype-ordinary", function);
    STONE_CHECK(!FindAndroidScalarImport(0) && !FindAndroidScalarImport(address) && !FindAndroidScalarImport(ordinary));
    const auto rejected = [](auto operation) {
        try {
            operation();
        }
        catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    STONE_CHECK(rejected([&] {
        ImportAndroidScalars<decltype(function)>("typed-prototype-ordinary", address);
    }));
    STONE_CHECK(!FindAndroidScalarImport(ordinary));
    const auto token = ImportAndroidScalars<decltype(function)>("typed-prototype-lifetime", address);
    const auto info = FindAndroidScalarImport(token);
    STONE_CHECK(info && info->prototype.parameters.empty() && info->prototype.result == 'J');
    STONE_CHECK(ImportAndroidScalars<decltype(function)>("typed-prototype-lifetime", address) == token);
    auto wrong = info->prototype;
    wrong.result = 'D';
    STONE_CHECK(rejected([&] {
        ImportAndroidScalars("typed-prototype-lifetime", address, info->invoke, wrong);
    }));
    wrong.result = 'Q';
    STONE_CHECK(rejected([&] {
        ImportAndroidScalars("typed-prototype-invalid", address, info->invoke, wrong);
    }));
    wrong.result = 'J';
    wrong.jniEnvironment = true;
    STONE_CHECK(rejected([&] {
        ImportAndroidScalars("typed-prototype-invalid-env", address, info->invoke, wrong);
    }));
    const auto source = +[](int value) -> int {
        return value;
    };
    const auto sourceToken =
        ImportAndroidScalars<decltype(source)>("typed-prototype-source", reinterpret_cast<uintptr_t>(source));
    const auto saved = FindAndroidScalarImport(sourceToken);
    STONE_CHECK(saved && saved->prototype.parameters.size() == 1);
    std::jthread publisher([&] {
        for (unsigned i = 0; i < 64; ++i)
            ImportAndroidScalars<decltype(function)>("typed-prototype-growth-" + std::to_string(i), address);
    });
    AndroidScalarArguments arguments{};
    arguments.integers[0] = 17;
    for (unsigned i = 0; i < 64; ++i)
        STONE_CHECK(saved->prototype.parameters[0] == 'I' && saved->invoke(arguments, saved->address) == 17);
    publisher.join();
    const auto throwing = +[](int value) -> int {
        throw std::runtime_error("typed native failure " + std::to_string(value));
    };
    const auto throwingToken =
        ImportAndroidScalars<decltype(throwing)>("typed-prototype-throw", reinterpret_cast<uintptr_t>(throwing));
    const auto throwingInfo = FindAndroidScalarImport(throwingToken);
    STONE_CHECK(throwingInfo);
    bool unwound = false;
    try {
        throwingInfo->invoke(arguments, throwingInfo->address);
    }
    catch (const std::runtime_error& error) {
        unwound = std::string_view(error.what()) == "typed native failure 17";
    }
    STONE_CHECK(unwound && saved->invoke(arguments, saved->address) == 17);
}
}
