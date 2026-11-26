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

void CheckArm64SignalSets()
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
    const auto memory = AllocateArm64GuestMemory(64, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto* masks = reinterpret_cast<uint64_t*>(memory);
    const auto mask =
        reinterpret_cast<int (*)(int, const uint64_t*, uint64_t*)>(AndroidThreadImport("pthread_sigmask", true));
    uint64_t original = 0;
    STONE_CHECK(mask(0, nullptr, &original) == 0);

    struct Restore {
        decltype(mask) call;
        uint64_t value;

        ~Restore()
        {
            call(2, &value, nullptr);
        }
    } restore{mask, original};

    const auto invoke = [&](uintptr_t token, uint64_t first, uint64_t second = 0, uint64_t third = 0) {
        const std::array<Arm64Argument, 4> args{{{'J', token}, {'J', first}, {'J', second}, {'J', third}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                constexpr std::string_view names[]{"",
                                                   "sigemptyset64",
                                                   "sigfillset64",
                                                   "sigaddset64",
                                                   "sigdelset64",
                                                   "sigismember64",
                                                   "pthread_sigmask64",
                                                   "sigprocmask64",
                                                   "sigpending64",
                                                   "sigsuspend64"};
                return target < std::size(names) ? names[target] : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    STONE_CHECK(invoke(2, memory) == 0 && masks[0] == UINT64_MAX);
    STONE_CHECK(invoke(1, memory) == 0 && !masks[0]);
    STONE_CHECK(invoke(3, memory, 64) == 0 && masks[0] == (uint64_t{1} << 63));
    STONE_CHECK(invoke(5, memory, 64) == 1 && invoke(5, memory, 63) == 0);
    STONE_CHECK(invoke(4, memory, 64) == 0 && !masks[0]);
    STONE_CHECK(invoke(3, memory, 0) == UINT32_MAX && !masks[0]);
    STONE_CHECK(invoke(5, memory, 65) == UINT32_MAX);
    STONE_CHECK(invoke(3, memory, 64) == 0);
    STONE_CHECK(invoke(6, 0, memory, memory + 8) == 0 && masks[1] == original);
    STONE_CHECK(invoke(7, 0, 0, memory + 16) == 0 && (masks[2] & masks[0]) == masks[0]);
    STONE_CHECK(invoke(7, 2, memory + 8) == 0);
    STONE_CHECK(invoke(8, memory + 24) == 0 && !masks[3]);
    STONE_CHECK(AndroidThreadImport("sigdelset64", true) && AndroidThreadImport("sigismember64", true) &&
                AndroidThreadImport("sigpending64", true) && AndroidThreadImport("sigsuspend64"));
    uint64_t host = 7;
    for (const auto token : {1u, 9u}) {
        bool rejected = false;
        try {
            invoke(token, reinterpret_cast<uintptr_t>(&host));
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected && host == 7);
    }
}

void CheckArm64SignalWaitInfo()
{
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
    const auto memory = AllocateArm64GuestMemory(160, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    const auto invoke = [&](unsigned token, uintptr_t mask, uintptr_t info) {
        const std::array<Arm64Argument, 3> args{{{'I', token}, {'L', mask}, {'L', info}}};
        return RunArm64Function(
            image, image.LoadBias() + 0x1200, 'J',
            [](uintptr_t target) -> std::string_view {
                return target == 1 ? "sigwaitinfo" : target == 2 ? "sigwaitinfo64" : "";
            },
            nullptr, nullptr, args);
    };
    AndroidSignalAction previous[2]{}, action{};
    STONE_CHECK(AndroidArm64Sigaction(10, &action, &previous[0]) == 0);
    STONE_CHECK(AndroidArm64Sigaction(64, &action, &previous[1]) == 0);

    struct Restore {
        AndroidSignalAction* previous;
        uint64_t mask;

        ~Restore()
        {
            AndroidArm64Sigaction(10, &previous[0], nullptr);
            AndroidArm64Sigaction(64, &previous[1], nullptr);
            ExchangeAndroidSignalMask(mask);
        }
    } restore{previous, ExchangeAndroidSignalMask((uint64_t{1} << 9) | (uint64_t{1} << 63))};

    // Register this guest thread before enqueueing either blocked signal.
    STONE_CHECK(invoke(1, 0, 0) == UINT32_MAX && errno == EFAULT);
    const auto raise = reinterpret_cast<int (*)(int)>(AndroidThreadImport("raise", true));
    for (const auto signal : {10, 64}) {
        *reinterpret_cast<uint64_t*>(memory) = uint64_t{1} << (signal - 1);
        for (unsigned token = 1; token <= 2; ++token) {
            std::memset(reinterpret_cast<void*>(memory + 8), 0xa5, 136);
            STONE_CHECK(raise(signal) == 0 && invoke(token, memory, memory + 16) == unsigned(signal));
            const auto* info = reinterpret_cast<const int32_t*>(memory + 16);
            STONE_CHECK(info[0] == signal && info[1] == 0 && info[2] == -6 &&
                        info[4] == static_cast<int32_t>(GetCurrentProcessId()) &&
                        uint32_t(info[5]) == AndroidApplicationUid());
            STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 8) == 0xa5a5a5a5a5a5a5a5);
        }
        STONE_CHECK(raise(signal) == 0 && invoke(2, memory, 0) == unsigned(signal));
    }
    int host = 7;
    for (const auto input : {true, false}) {
        bool rejected = false;
        try {
            invoke(2, input ? reinterpret_cast<uintptr_t>(&host) : memory,
                   input ? memory + 16 : reinterpret_cast<uintptr_t>(&host));
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected && host == 7);
    }
}

void CheckNativeSignalRecovery()
{
    auto bytes = MakeElf(false);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put(sizeof(stone::elf::Elf64Ehdr) + 4, uint32_t{5}); // Read-only executable test segment.
    put(0x1c0 + 28, uint8_t{0x12});
    put(0x1c0 + 30, uint16_t{1});
    put(0x1c0 + 32, uint64_t{0x280});
    put(0x1c0 + 40, uint64_t{3});
    put(0x150, std::array<uint64_t, 2>{0, 0}); // End DT tags before relocations.
    bytes[0x280] = 0x8b;
    bytes[0x281] = 0x07;
    bytes[0x282] = 0xc3; // mov eax,[rdi]; ret.
    const auto set = Guest<int (*)(int, const AndroidSignalAction*, AndroidSignalAction*)>(Libc("sigaction"), "(ILL)I");
    static bool handled = false;
    handled = false;
    const auto handler = Import(
        "Stone ELF hardware signal recovery", +[](int number, AndroidSignalInfo* info, AndroidSignalContext* context) {
            handled = number == 11 && info->code == 1 && info->detail[0] == 0;
            context->machine.registers[16] += 2;
            context->machine.registers[13] = 42;
        });
    const AndroidSignalAction action{4, handler, 0, 0};
    AndroidSignalAction previous{};
    STONE_CHECK(set(11, &action, &previous) == 0);

    struct Restore {
        decltype(set) call;
        AndroidSignalAction action;

        ~Restore()
        {
            call(11, &action, nullptr);
        }
    } restore{set, previous};

    LoadNativeSystemElf("libstone-signal-fixture.so", bytes);
    const auto call = reinterpret_cast<int (*)(const void*)>(
        NativeSystemElfThunk("libstone-signal-fixture.so", "stone_fixture_import", "(L)I"));
    STONE_CHECK(call && call(nullptr) == 42 && handled);
}
}
