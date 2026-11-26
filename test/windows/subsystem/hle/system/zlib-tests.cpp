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

void TestArm64ImageZlib(Suite& suite, const std::filesystem::path& directory)
{
    suite.Run("libz-image-adler32-vector-boundaries-and-incremental-input", [&] {
        auto path = directory / L"lib64/libz.so";
        std::ifstream input(path.make_preferred(), std::ios::binary);
        STONE_CHECK(input);
        const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), {});
        ElfImage image(bytes, ElfImage::Machine::Arm64, true);
        std::map<uintptr_t, std::string> imports;
        std::set<std::string> missing;
        image.Relocate(
            [&](std::string_view name) -> uintptr_t {
                const auto address = Libc(name);
                if (address)
                    imports.try_emplace(address, name);
                return address;
            },
            &missing);
        STONE_CHECK(missing.empty() && image.IndirectRelocationCount() == 0);
        image.Protect(ElfImage::Execution::Translated);
        const auto entry = image.Export("adler32", true);
        STONE_CHECK(entry);
        constexpr size_t capacity = 16384;
        const auto memory = AllocateArm64GuestMemory(capacity + 32, true);
        STONE_CHECK(memory);
        struct Free {
            uintptr_t address;
            ~Free()
            {
                FreeArm64GuestMemory(address);
            }
        } free{memory};
        auto* data = reinterpret_cast<uint8_t*>(memory);
        for (size_t i = 0; i < capacity + 32; ++i)
            data[i] = static_cast<uint8_t>((i * 113) ^ (i >> 3));
        const auto expected = [](uint32_t seed, const uint8_t* data, size_t size) {
            uint32_t first = seed & 65535, second = seed >> 16;
            for (size_t i = 0; i < size; ++i) {
                first = (first + data[i]) % 65521;
                second = (second + first) % 65521;
            }
            return (second % 65521) << 16 | (first % 65521);
        };
        const auto call = [&](uint32_t seed, uintptr_t source, size_t length) {
            const std::array<Arm64Argument, 3> args{{{'J', seed}, {'L', source}, {'I', length}}};
            try {
                return static_cast<uint32_t>(RunArm64Function(
                    image, entry, 'J',
                    [&](uintptr_t address) -> std::string_view {
                        const auto found = imports.find(address);
                        return found == imports.end() ? std::string_view{} : found->second;
                    },
                    nullptr, nullptr, args));
            }
            catch (const std::exception& error) {
                throw std::runtime_error("adler32 length " + std::to_string(length) + ": " + error.what());
            }
        };
        STONE_CHECK(call(0, 0, 0) == 1);
        for (const auto seed : {1u, 0xdeadbeefu, UINT32_MAX})
            for (const auto offset : {0u, 1u, 15u})
                for (const auto length : {0u, 1u, 15u, 16u, 31u, 32u, 63u, 64u, 65u, 127u, 128u, 5552u, 5553u, 16384u})
                    STONE_CHECK(call(seed, memory + offset, length) == expected(seed, data + offset, length));
        const auto partial = call(1, memory, 7001);
        STONE_CHECK(call(partial, memory + 7001, capacity - 7001) == expected(1, data, capacity));
        wchar_t benchmark[2]{};
        if (GetEnvironmentVariableW(L"STONE_ARM64_ZLIB_BENCHMARK", benchmark, 2) == 1 && benchmark[0] == L'1') {
            const auto checksum = expected(1, data, capacity);
            LARGE_INTEGER frequency, start, finish;
            QueryPerformanceFrequency(&frequency);
            const auto before = GetArm64BlockStatistics();
            QueryPerformanceCounter(&start);
            constexpr unsigned repetitions = 256;
            for (unsigned i = 0; i < repetitions; ++i)
                STONE_CHECK(call(1, memory, capacity) == checksum);
            QueryPerformanceCounter(&finish);
            const auto after = GetArm64BlockStatistics();
            STONE_CHECK((after.instructions > before.instructions) == Arm64BlockJitEnabled());
            std::printf(
                "arm64-zlib-benchmark enabled=%d bytes=%zu checksum=%u elapsed-us=%llu native-calls=%llu native-instructions=%llu\n",
                Arm64BlockJitEnabled(), capacity * repetitions, checksum,
                (finish.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart, after.executions - before.executions,
                after.instructions - before.instructions);
        }
        STONE_CHECK(GetModuleHandleW(L"jvm.dll") == nullptr);
    });
}
}
