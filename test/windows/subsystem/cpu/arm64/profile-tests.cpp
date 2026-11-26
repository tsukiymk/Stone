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

namespace stone::windows::test {
using namespace checks;

void TestArm64ExecutionProfile(Suite& suite)
{
    char setting[16]{};
    const auto settingLength = GetEnvironmentVariableA("STONE_ARM64_PROFILE", setting, sizeof(setting));
    const bool detailed = settingLength == 1 && setting[0] == '1';
    const bool enabled = detailed || (settingLength == 7 && std::string_view(setting, 7) == "imports");
    auto bytes = MakeArm64TlsElf();
    const uint32_t loop[]{0xf1000400, 0x54ffffe1, 0xd65f03c0}; // SUBS x0,#1; B.NE loop; RET.
    std::memcpy(bytes.data() + 0x1100, loop, sizeof(loop));
    const uint32_t imported[]{0x58000050, 0xd61f0200};
    const uint64_t token = 1;
    std::memcpy(bytes.data() + 0x1200, imported, sizeof(imported));
    std::memcpy(bytes.data() + 0x1208, &token, sizeof(token));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    RegisterArm64ProfileImage(image.Identity(), "profile-fixture.so");
    const auto run = [&](uintptr_t entry, uint64_t iterations) {
        const Arm64Argument args[]{{'J', iterations}};
        return RunArm64Function(
            image, entry, 'J',
            [](uintptr_t target) -> std::string_view {
                return target == token ? "getpid" : std::string_view{};
            },
            nullptr, nullptr, args);
    };
    suite.Run("arm64-profile-static-code-instruction-counts-and-sampled-image-identity", [&] {
        const auto before = GetArm64ExecutionStatistics();
        STONE_CHECK(run(image.LoadBias() + 0x1100, 20000) == 0);
        const auto after = GetArm64ExecutionStatistics();
        STONE_CHECK(after.enabled == enabled && before.enabled == enabled && !after.errors &&
                    after.instructionProfiling == detailed && before.instructionProfiling == detailed);
        STONE_CHECK(after.interpretedInstructions - before.interpretedInstructions == (detailed ? 40001 : 0));
        STONE_CHECK(after.interpreterRuns - before.interpreterRuns == (after.enabled ? 1 : 0));
        STONE_CHECK(after.mappedInstructions == before.mappedInstructions);
        if (detailed) {
            STONE_CHECK(after.samples > before.samples && !after.droppedSamples);
            STONE_CHECK(std::any_of(after.hotspots.begin(), after.hotspots.end(), [&](const auto& hot) {
                return hot.image == image.Identity() && hot.name == "profile-fixture.so" && hot.offset >= 0x1100 &&
                       hot.offset <= 0x1108 && hot.samples;
            }));
            uint64_t instructions = 0;
            for (const auto value : after.opcodeGroups)
                instructions += value;
            STONE_CHECK(instructions == after.interpretedInstructions);
        }
        else {
            STONE_CHECK(!after.samples && after.hotspots.empty() && !after.interpretedInstructions);
            for (const auto count : after.opcodeGroups)
                STONE_CHECK(!count);
        }
    });
    suite.Run("arm64-profile-mapped-code-and-host-import-accounting", [&] {
        auto* memory = AndroidMapArm64(nullptr, 4096, 3, 0x22, -1, 0);
        STONE_CHECK(memory != reinterpret_cast<void*>(-1));
        struct Unmap {
            void* value;
            ~Unmap()
            {
                AndroidUnmapArm64(value, 4096);
            }
        } unmap{memory};
        std::memcpy(memory, loop, sizeof(loop));
        STONE_CHECK(AndroidProtectArm64(memory, 4096, 5) == 0);
        const auto before = GetArm64ExecutionStatistics();
        STONE_CHECK(run(reinterpret_cast<uintptr_t>(memory), 20000) == 0);
        STONE_CHECK(run(image.LoadBias() + 0x1200, 0) == GetCurrentProcessId());
        const auto after = GetArm64ExecutionStatistics();
        STONE_CHECK(after.mappedInstructions - before.mappedInstructions == (detailed ? 40001 : 0));
        STONE_CHECK(after.interpretedInstructions - before.interpretedInstructions == (detailed ? 40003 : 0));
        if (after.enabled)
            STONE_CHECK(std::any_of(after.imports.begin(), after.imports.end(), [](const auto& item) {
                return item.first == "getpid" && item.second == 1;
            }));
    });
    suite.Run("arm64-profile-actual-native-leaf-execution-and-six-argument-abi", [&] {
        const uint32_t leaf[]{0x8b050000, 0xd65f03c0}; // ADD x0,x0,x5; RET.
        const unsigned arguments[]{64, 64, 64, 64, 64, 64};
        const auto code = TranslateArm64Leaf({reinterpret_cast<const uint8_t*>(leaf), sizeof(leaf)}, arguments, 64);
        auto* memory = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        STONE_CHECK(memory);
        struct Free {
            void* value;
            ~Free()
            {
                VirtualFree(value, 0, MEM_RELEASE);
            }
        } free{memory};
        std::memcpy(memory, code.data(), code.size());
        DWORD previous = 0;
        STONE_CHECK(VirtualProtect(memory, code.size(), PAGE_EXECUTE_READ, &previous) &&
                    FlushInstructionCache(GetCurrentProcess(), memory, code.size()));
        const auto call =
            reinterpret_cast<uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)>(memory);
        const auto before = GetArm64ExecutionStatistics();
        for (unsigned i = 0; i < 7; ++i)
            STONE_CHECK(call(1, 2, 3, 4, 5, 6) == 7);
        const auto after = GetArm64ExecutionStatistics();
        STONE_CHECK(after.nativeLeafCalls - before.nativeLeafCalls == (after.enabled ? 7 : 0));
        STONE_CHECK(after.nativeLeafInstructions - before.nativeLeafInstructions == (after.enabled ? 14 : 0));
        STONE_CHECK(after.interpretedInstructions == before.interpretedInstructions);
    });
    suite.Run("arm64-profile-thread-exit-flush-and-unambiguous-elf-lifetimes", [&] {
        ElfImage second(bytes, ElfImage::Machine::Arm64, true);
        STONE_CHECK(second.Identity() && second.Identity() != image.Identity());
        const auto before = GetArm64ExecutionStatistics();
        std::array<std::exception_ptr, 4> errors{};
        std::vector<std::thread> threads;
        for (size_t i = 0; i < errors.size(); ++i)
            threads.emplace_back([&, i] {
                try {
                    STONE_CHECK(run(image.LoadBias() + 0x1100, 2000 + i) == 0);
                }
                catch (...) {
                    errors[i] = std::current_exception();
                }
            });
        for (auto& thread : threads)
            thread.join();
        for (const auto& error : errors)
            if (error)
                std::rethrow_exception(error);
        const auto after = GetArm64ExecutionStatistics();
        STONE_CHECK(after.interpretedInstructions - before.interpretedInstructions == (detailed ? 16016 : 0));
        STONE_CHECK(after.interpreterRuns - before.interpreterRuns == (after.enabled ? 4 : 0) && !after.errors);
    });
    suite.Run("arm64-profile-import-counters-own-temporary-names-and-survive-flush", [&] {
        auto* profile = CurrentArm64ExecutionProfile();
        STONE_CHECK(bool(profile) == enabled);
        if (!profile)
            return;
        GetArm64ExecutionStatistics();
        for (unsigned batch = 0; batch < 2; ++batch) {
            std::string temporary;
            for (unsigned i = 0; i < 100; ++i) {
                temporary = "profile-temporary-" + std::to_string(i);
                for (unsigned call = 0; call < 3; ++call) {
                    errno = ERANGE;
                    SetLastError(ERROR_ACCESS_DENIED);
                    RecordArm64ProfileImport(*profile, temporary);
                    STONE_CHECK(errno == ERANGE && GetLastError() == ERROR_ACCESS_DENIED);
                }
            }
            const auto snapshot = GetArm64ExecutionStatistics();
            STONE_CHECK(!snapshot.errors);
            for (unsigned i = 0; i < 100; ++i) {
                const auto name = "profile-temporary-" + std::to_string(i);
                STONE_CHECK(std::any_of(snapshot.imports.begin(), snapshot.imports.end(), [&](const auto& item) {
                    return item.first == name && item.second == 3 * (batch + 1);
                }));
            }
        }
    });
    errno = EDOM;
    SetLastError(ERROR_ACCESS_DENIED);
    DumpArm64ExecutionProfile();
    STONE_CHECK(errno == EDOM && GetLastError() == ERROR_ACCESS_DENIED);
}
}
