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

void CheckArm64PairwiseAccumulate()
{
    auto bytes = MakeArm64TlsElf();

    struct Case {
        uintptr_t entry;
        unsigned width;
        bool wide, signedInput, accumulate, alias;
    };

    std::vector<Case> cases;
    for (const bool wide : {false, true})
        for (const bool signedInput : {false, true})
            for (unsigned size = 0; size < 3; ++size)
                for (const bool accumulate : {false, true})
                    for (const bool alias : {false, true}) {
                        const auto entry = 0x1000 + cases.size() * 32;
                        const uint32_t code[]{0x3dc00000, 0x3dc00021,
                                              0x0e202800u | (wide ? 0x40000000u : 0u) |
                                                  (signedInput ? 0u : 0x20000000u) | (size << 22) |
                                                  (accumulate ? 0x4000u : 0u) | (alias ? 0u : 0x20u),
                                              0x3d800040, 0xd65f03c0};
                        std::memcpy(bytes.data() + entry, code, sizeof(code));
                        cases.push_back({entry, 8u << size, wide, signedInput, accumulate, alias});
                    }
    const uint32_t actual[]{0x3dc00017, 0x3dc00038, 0x6e206b17, 0x3d800057, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1700, actual, sizeof(actual));
    cases.push_back({0x1700, 8, true, false, true, false});
    const uint32_t reserved[]{0x4ee06820, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1740, reserved, sizeof(reserved));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Relocate(
        [](std::string_view) -> uintptr_t {
            return 0;
        },
        nullptr, {}, {}, Arm64TlsDescriptorResolver());
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(48, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t address;

        ~Free()
        {
            FreeArm64GuestMemory(address);
        }
    } free{memory};

    auto* vectors = reinterpret_cast<uint8_t*>(memory);
    for (unsigned i = 0; i < 16; ++i) {
        vectors[i] = static_cast<uint8_t>(0xff - i * 7);
        vectors[16 + i] = static_cast<uint8_t>(0x80 + i * 31);
    }
    const auto lane = [](const uint8_t* input, unsigned index, unsigned width) {
        uint64_t value = 0;
        std::memcpy(&value, input + index * width / 8, width / 8);
        return value;
    };
    const std::array<Arm64Argument, 3> args{{{'L', memory}, {'L', memory + 16}, {'L', memory + 32}}};
    for (const auto& test : cases) {
        std::array<uint8_t, 16> expected{};
        const auto* source = vectors + (test.alias ? 0 : 16);
        for (unsigned i = 0; i < (test.wide ? 128u : 64u) / (test.width * 2); ++i) {
            auto first = lane(source, 2 * i, test.width), second = lane(source, 2 * i + 1, test.width);
            if (test.signedInput) {
                const auto sign = uint64_t{1} << (test.width - 1);
                first = (first ^ sign) - sign;
                second = (second ^ sign) - sign;
            }
            const auto value = first + second + (test.accumulate ? lane(vectors, i, test.width * 2) : 0);
            std::memcpy(expected.data() + i * test.width / 4, &value, test.width / 4);
        }
        RunArm64Function(image, image.LoadBias() + test.entry, 'V', {}, nullptr, nullptr, args);
        STONE_CHECK(std::memcmp(vectors + 32, expected.data(), expected.size()) == 0);
    }
    bool rejected = false;
    try {
        RunArm64Function(image, image.LoadBias() + 0x1740, 'V', {}, nullptr, nullptr, {});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
}

void CheckArm64RoundNearest(bool away)
{
    auto bytes = MakeArm64TlsElf();
    bytes.resize(0x3000);
    auto* segment =
        reinterpret_cast<elf::Elf64Phdr*>(bytes.data() + sizeof(elf::Elf64Ehdr) + 3 * sizeof(elf::Elf64Phdr));
    segment->p_filesz = segment->p_memsz = 0x2000;
    const auto index = [](unsigned wide, unsigned mode, unsigned alias) {
        return (wide * 3 + mode) * 2 + alias;
    };
    for (unsigned wide = 0; wide < 2; ++wide)
        for (unsigned mode = 0; mode < 3; ++mode)
            for (unsigned alias = 0; alias < 2; ++alias) {
                const unsigned source = away ? 0 : 16, dest = alias ? 18 : source;
                const auto word = (mode == 2 ? (away ? 0x1e264000u : 0x1e244000u)
                                             : (away ? 0x2e218800u : 0x0e218800u) | (mode == 1 ? 0x40000000 : 0)) |
                                  (wide << 22) | (source << 5) | dest;
                // Includes actual libhwui FRINTN 0x0e218a10 and libinput FRINTA 0x2e218800.
                const uint32_t code[]{0x3dc00000u | source, 0x3dc00412, 0xd53b4406, 0xd53b4427,         0xd51b4402,
                                      0xd51b4423,           0xd51b4204, word,       0x3d800020u | dest, 0xd53b4405,
                                      0xf9000825,           0xd53b4425, 0xf9000c25, 0xd53b4205,         0xf9001025,
                                      0xd51b4406,           0xd51b4427, 0xd65f03c0};
                std::memcpy(bytes.data() + 0x1200 + 128 * index(wide, mode, alias), code, sizeof(code));
            }
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(160, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    struct Case {
        uint64_t bits, answer;
        uint32_t flags = 0;
        uint64_t awayAnswer = 0;
    }; // Zero means unchanged.

    const Case singles[]{{0, 0},
                         {0x80000000, 0x80000000},
                         {0x3f000000, 0, 0, 0x3f800000},
                         {0xbf000000, 0x80000000, 0, 0xbf800000},
                         {0x3effffff, 0},
                         {0x3f000001, 0x3f800000},
                         {0xbf000001, 0xbf800000},
                         {0x3fc00000, 0x40000000},
                         {0x40200000, 0x40000000, 0, 0x40400000},
                         {0x40600000, 0x40800000},
                         {0xbfc00000, 0xc0000000},
                         {0xc0200000, 0xc0000000, 0, 0xc0400000},
                         {0x42280000, 0x42280000},
                         {0x4affffff, 0x4b000000},
                         {0x4afffffd, 0x4afffffc, 0, 0x4afffffe},
                         {0x4b000000, 0x4b000000},
                         {0x7f7fffff, 0x7f7fffff},
                         {0x7f800000, 0x7f800000},
                         {0xff800000, 0xff800000},
                         {0x7fc01234, 0x7fc01234},
                         {0xff801234, 0xffc01234, 1},
                         {1, 0},
                         {0x807fffff, 0x80000000},
                         {0x00800000, 0},
                         {0x80800000, 0x80000000}};
    const Case doubles[]{{0, 0},
                         {0x8000000000000000, 0x8000000000000000},
                         {0x3fe0000000000000, 0, 0, 0x3ff0000000000000},
                         {0xbfe0000000000000, 0x8000000000000000, 0, 0xbff0000000000000},
                         {0x3fdfffffffffffff, 0},
                         {0x3fe0000000000001, 0x3ff0000000000000},
                         {0xbfe0000000000001, 0xbff0000000000000},
                         {0x3ff8000000000000, 0x4000000000000000},
                         {0x4004000000000000, 0x4000000000000000, 0, 0x4008000000000000},
                         {0x400c000000000000, 0x4010000000000000},
                         {0xbff8000000000000, 0xc000000000000000},
                         {0xc004000000000000, 0xc000000000000000, 0, 0xc008000000000000},
                         {0x4045000000000000, 0x4045000000000000},
                         {0x432fffffffffffff, 0x4330000000000000},
                         {0x432ffffffffffffd, 0x432ffffffffffffc, 0, 0x432ffffffffffffe},
                         {0x4330000000000000, 0x4330000000000000},
                         {0x7fefffffffffffff, 0x7fefffffffffffff},
                         {0x7ff0000000000000, 0x7ff0000000000000},
                         {0xfff0000000000000, 0xfff0000000000000},
                         {0x7ff8000000001234, 0x7ff8000000001234},
                         {0xfff0000000001234, 0xfff8000000001234, 1},
                         {1, 0},
                         {0x800fffffffffffff, 0x8000000000000000},
                         {0x0010000000000000, 0},
                         {0x8010000000000000, 0x8000000000000000}};
    const auto exercise = [&](unsigned wide, const auto& cases) {
        const unsigned width = wide ? 64 : 32, laneBytes = width / 8;
        const uint64_t exponentMask = wide ? 0x7ff0000000000000ULL : 0x7f800000ULL;
        const uint64_t fractionMask = wide ? 0x000fffffffffffffULL : 0x007fffffULL;
        for (unsigned mode = 0; mode < 3; ++mode) {
            if (wide && mode == 0)
                continue;
            const unsigned lanes = mode == 2 ? 1 : (mode == 1 ? 128 : 64) / width;
            for (unsigned alias = 0; alias < 2; ++alias)
                for (unsigned offset = 0; offset < std::size(cases); ++offset)
                    for (unsigned rounding = 0; rounding < 4; ++rounding)
                        for (unsigned flush = 0; flush < 2; ++flush)
                            for (const auto previous : {0x08000004u, 0x08000014u}) {
                                std::memset(reinterpret_cast<void*>(memory), 0xa5, 160);
                                const uint32_t control = (rounding << 22) | (flush << 24) | ((offset & 1) << 25);
                                std::array<uint8_t, 16> expected{};
                                uint32_t flags = 0;
                                for (unsigned lane = 0; lane < 128 / width; ++lane) {
                                    const auto& sample = cases[(offset + lane) % std::size(cases)];
                                    std::memcpy(reinterpret_cast<void*>(memory + lane * laneBytes), &sample.bits,
                                                laneBytes);
                                    if (lane >= lanes)
                                        continue;
                                    const bool subnormal =
                                        !(sample.bits & exponentMask) && (sample.bits & fractionMask);
                                    const bool nan =
                                        (sample.bits & exponentMask) == exponentMask && (sample.bits & fractionMask);
                                    const auto answer = nan && (control & (1u << 25))
                                                            ? exponentMask | ((fractionMask + 1) >> 1)
                                                        : away && sample.awayAnswer ? sample.awayAnswer
                                                                                    : sample.answer;
                                    std::memcpy(expected.data() + lane * laneBytes, &answer, laneBytes);
                                    flags |= flush && subnormal ? 128 : sample.flags;
                                }
                                std::array<uint8_t, 32> input;
                                std::memcpy(input.data(), reinterpret_cast<void*>(memory), 32);
                                const Arm64Argument args[]{{'J', memory},
                                                           {'J', memory + 80},
                                                           {'J', control},
                                                           {'J', previous},
                                                           {'J', 0xa0000000}};
                                RunArm64Function(image, image.LoadBias() + 0x1200 + 128 * index(wide, mode, alias), 'V',
                                                 {}, nullptr, nullptr, args);
                                STONE_CHECK(!std::memcmp(reinterpret_cast<void*>(memory + 80), expected.data(), 16));
                                STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 96) == control &&
                                            *reinterpret_cast<uint64_t*>(memory + 104) == (previous | flags) &&
                                            *reinterpret_cast<uint64_t*>(memory + 112) == 0xa0000000);
                                STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 72) == 0xa5a5a5a5a5a5a5a5 &&
                                            *reinterpret_cast<uint64_t*>(memory + 120) == 0xa5a5a5a5a5a5a5a5 &&
                                            !std::memcmp(reinterpret_cast<void*>(memory), input.data(), 32));
                            }
        }
    };
    exercise(0, singles);
    exercise(1, doubles);
    bool rejected = false;
    try {
        RunArm64Function(
            image, image.LoadBias() + 0x1200 + 128 * index(1, 0, 0), 'V', {}, nullptr, nullptr,
            std::array<Arm64Argument, 5>{{{'J', memory}, {'J', memory + 80}, {'J', 0}, {'J', 0}, {'J', 0}}});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
}

void CheckArm64NearestIntegerConversions()
{
    auto bytes = MakeArm64TlsElf();
    bytes.resize(0x3000);
    auto* segment =
        reinterpret_cast<elf::Elf64Phdr*>(bytes.data() + sizeof(elf::Elf64Ehdr) + 3 * sizeof(elf::Elf64Phdr));
    segment->p_filesz = segment->p_memsz = 0x2000;
    const auto index = [](unsigned wide, unsigned sign, unsigned mode, unsigned alias) {
        return ((wide * 2 + sign) * 3 + mode) * 2 + alias;
    };
    for (unsigned wide = 0; wide < 2; ++wide)
        for (unsigned sign = 0; sign < 2; ++sign)
            for (unsigned mode = 0; mode < 3; ++mode)
                for (unsigned alias = 0; alias < 2; ++alias) {
                    const unsigned dest = alias ? 16 : 18;
                    const auto word = 0x0e21a800u | (wide << 22) | (sign ? 0 : 0x20000000) |
                                      (mode == 2   ? 0x50000000
                                       : mode == 1 ? 0x40000000
                                                   : 0) |
                                      (18 << 5) | dest;
                    // FCVTNU .4S, source/dest=18 is the actual libhwui fault word 0x6e21aa52.
                    const uint32_t code[]{0x3dc00012, 0x3dc00410, 0xd53b4406, 0xd53b4427,         0xd51b4402,
                                          0xd51b4423, 0xd51b4204, word,       0x3d800020u | dest, 0xd53b4405,
                                          0xf9000825, 0xd53b4425, 0xf9000c25, 0xd53b4205,         0xf9001025,
                                          0xd51b4406, 0xd51b4427, 0xd65f03c0};
                    std::memcpy(bytes.data() + 0x1200 + 128 * index(wide, sign, mode, alias), code, sizeof(code));
                }
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(160, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    struct Case {
        uint64_t bits, signedValue, unsignedValue;
        uint32_t signedFlags, unsignedFlags;
    };

    constexpr uint32_t ioc = 1, ixc = 16, idc = 128;
    const Case singles[]{{0, 0, 0, 0, 0},
                         {0x80000000, 0, 0, 0, 0},
                         {0x3f000000, 0, 0, ixc, ixc},
                         {0xbf000000, 0, 0, ixc, ixc},
                         {0x3effffff, 0, 0, ixc, ixc},
                         {0x3f000001, 1, 1, ixc, ixc},
                         {0xbf000001, 0xffffffff, 0, ixc, ioc},
                         {0x3fc00000, 2, 2, ixc, ixc},
                         {0x40200000, 2, 2, ixc, ixc},
                         {0x40600000, 4, 4, ixc, ixc},
                         {0xbfc00000, 0xfffffffe, 0, ixc, ioc},
                         {0xc0200000, 0xfffffffe, 0, ixc, ioc},
                         {0x42280000, 42, 42, 0, 0},
                         {0x4effffff, 0x7fffff80, 0x7fffff80, 0, 0},
                         {0x4f000000, 0x7fffffff, 0x80000000, ioc, 0},
                         {0xcf000000, 0x80000000, 0, 0, ioc},
                         {0xcf000001, 0x80000000, 0, ioc, ioc},
                         {0x4f7fffff, 0x7fffffff, 0xffffff00, ioc, 0},
                         {0x4f800000, 0x7fffffff, 0xffffffff, ioc, ioc},
                         {0x7f800000, 0x7fffffff, 0xffffffff, ioc, ioc},
                         {0xff800000, 0x80000000, 0, ioc, ioc},
                         {0x7fc01234, 0, 0, ioc, ioc},
                         {0xff801234, 0, 0, ioc, ioc},
                         {1, 0, 0, ixc, ixc},
                         {0x807fffff, 0, 0, ixc, ixc}};
    const Case doubles[]{{0, 0, 0, 0, 0},
                         {0x8000000000000000, 0, 0, 0, 0},
                         {0x3fe0000000000000, 0, 0, ixc, ixc},
                         {0xbfe0000000000000, 0, 0, ixc, ixc},
                         {0x3fdfffffffffffff, 0, 0, ixc, ixc},
                         {0x3fe0000000000001, 1, 1, ixc, ixc},
                         {0xbfe0000000000001, UINT64_MAX, 0, ixc, ioc},
                         {0x3ff8000000000000, 2, 2, ixc, ixc},
                         {0x4004000000000000, 2, 2, ixc, ixc},
                         {0x400c000000000000, 4, 4, ixc, ixc},
                         {0xbff8000000000000, UINT64_MAX - 1, 0, ixc, ioc},
                         {0xc004000000000000, UINT64_MAX - 1, 0, ixc, ioc},
                         {0x4045000000000000, 42, 42, 0, 0},
                         {0x43dfffffffffffff, 0x7ffffffffffffc00, 0x7ffffffffffffc00, 0, 0},
                         {0x43e0000000000000, 0x7fffffffffffffff, 0x8000000000000000, ioc, 0},
                         {0xc3e0000000000000, 0x8000000000000000, 0, 0, ioc},
                         {0xc3e0000000000001, 0x8000000000000000, 0, ioc, ioc},
                         {0x43efffffffffffff, 0x7fffffffffffffff, 0xfffffffffffff800, ioc, 0},
                         {0x43f0000000000000, 0x7fffffffffffffff, UINT64_MAX, ioc, ioc},
                         {0x7ff0000000000000, 0x7fffffffffffffff, UINT64_MAX, ioc, ioc},
                         {0xfff0000000000000, 0x8000000000000000, 0, ioc, ioc},
                         {0x7ff8000000001234, 0, 0, ioc, ioc},
                         {0xfff0000000001234, 0, 0, ioc, ioc},
                         {1, 0, 0, ixc, ixc},
                         {0x800fffffffffffff, 0, 0, ixc, ixc}};
    const auto exercise = [&](unsigned wide, const auto& cases) {
        const unsigned width = wide ? 64 : 32, laneBytes = width / 8;
        for (unsigned sign = 0; sign < 2; ++sign)
            for (unsigned mode = 0; mode < 3; ++mode) {
                if (wide && mode == 0)
                    continue; // Reserved .1D vector.
                const unsigned lanes = mode == 2 ? 1 : (mode == 1 ? 128 : 64) / width;
                for (unsigned alias = 0; alias < 2; ++alias)
                    for (unsigned offset = 0; offset < std::size(cases); ++offset)
                        for (unsigned rounding = 0; rounding < 4; ++rounding)
                            for (unsigned flush = 0; flush < 2; ++flush) {
                                std::memset(reinterpret_cast<void*>(memory), 0xa5, 160);
                                std::array<uint8_t, 16> expected{};
                                uint32_t flags = 0;
                                for (unsigned lane = 0; lane < 128 / width; ++lane) {
                                    const auto& sample = cases[(offset + lane) % std::size(cases)];
                                    std::memcpy(reinterpret_cast<void*>(memory + lane * laneBytes), &sample.bits,
                                                laneBytes);
                                    if (lane >= lanes)
                                        continue;
                                    const auto result = sign ? sample.signedValue : sample.unsignedValue;
                                    std::memcpy(expected.data() + lane * laneBytes, &result, laneBytes);
                                    const bool subnormal =
                                        (sample.bits & (wide ? 0x7ff0000000000000ULL : 0x7f800000ULL)) == 0 &&
                                        (sample.bits & (wide ? 0x000fffffffffffffULL : 0x007fffffULL)) != 0;
                                    flags |= flush && subnormal ? idc
                                             : sign             ? sample.signedFlags
                                                                : sample.unsignedFlags;
                                }
                                std::array<uint8_t, 32> input;
                                std::memcpy(input.data(), reinterpret_cast<void*>(memory), 32);
                                const uint32_t control = (rounding << 22) | (flush << 24) | ((offset & 1) << 25),
                                               previous = 0x08000004;
                                const Arm64Argument args[]{{'J', memory},
                                                           {'J', memory + 80},
                                                           {'J', control},
                                                           {'J', previous},
                                                           {'J', 0xa0000000}};
                                RunArm64Function(image,
                                                 image.LoadBias() + 0x1200 + 128 * index(wide, sign, mode, alias), 'V',
                                                 {}, nullptr, nullptr, args);
                                STONE_CHECK(!std::memcmp(reinterpret_cast<void*>(memory + 80), expected.data(), 16));
                                STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 96) == control &&
                                            *reinterpret_cast<uint64_t*>(memory + 104) == (previous | flags) &&
                                            *reinterpret_cast<uint64_t*>(memory + 112) == 0xa0000000);
                                STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 72) == 0xa5a5a5a5a5a5a5a5 &&
                                            *reinterpret_cast<uint64_t*>(memory + 120) == 0xa5a5a5a5a5a5a5a5 &&
                                            !std::memcmp(reinterpret_cast<void*>(memory), input.data(), 32));
                            }
            }
    };
    exercise(0, singles);
    exercise(1, doubles);
    for (unsigned sign = 0; sign < 2; ++sign) {
        bool rejected = false;
        try {
            RunArm64Function(
                image, image.LoadBias() + 0x1200 + 128 * index(1, sign, 0, 0), 'V', {}, nullptr, nullptr,
                std::array<Arm64Argument, 5>{{{'J', memory}, {'J', memory + 80}, {'J', 0}, {'J', 0}, {'J', 0}}});
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    }
}

void CheckArm64SaturatingNarrow()
{
    auto bytes = MakeArm64TlsElf();
    bytes.resize(0x4000);
    auto* segment =
        reinterpret_cast<elf::Elf64Phdr*>(bytes.data() + sizeof(elf::Elf64Ehdr) + 3 * sizeof(elf::Elf64Phdr));
    segment->p_filesz = segment->p_memsz = 0x3000;
    const uint32_t operations[]{0x0e212800, 0x0e214800, 0x2e214800, 0x2e212800};
    const auto index = [](unsigned size, unsigned op, unsigned mode, unsigned alias) {
        return ((size * 4 + op) * 3 + mode) * 2 + alias;
    };
    for (unsigned size = 0; size < 3; ++size)
        for (unsigned op = 0; op < 4; ++op)
            for (unsigned mode = 0; mode < 3; ++mode)
                for (unsigned alias = 0; alias < 2; ++alias) {
                    const unsigned dest = alias ? 16 : 0;
                    const auto word = operations[op] | (size << 22) |
                                      (mode == 2   ? 0x50000000
                                       : mode == 1 ? 0x40000000
                                                   : 0) |
                                      dest;
                    const uint32_t code[]{0x3dc00000, 0x3dc00410,         0xd53b4427, 0xd51b4422, 0xd51b4203,
                                          word,       0x3d800020u | dest, 0xd53b4424, 0xf9000824, 0xd53b4205,
                                          0xf9000c25, 0xd51b4427,         0xd65f03c0};
                    // UQXTN, size=0, low half, alias=1 contains actual libhwui word 0x2e214810.
                    std::memcpy(bytes.data() + 0x1200 + 64 * index(size, op, mode, alias), code, sizeof(code));
                }
    unsigned invalidCount = 0;
    const auto invalid = [&](uint32_t word) {
        const uint32_t code[]{word, 0xd65f03c0};
        std::memcpy(bytes.data() + 0x3400 + 16 * invalidCount++, code, sizeof(code));
    };
    for (const auto word : operations)
        for (const auto mode : {0u, 0x40000000u, 0x50000000u})
            invalid(word | 0x00c00000 | mode);
    for (unsigned size = 0; size < 3; ++size)
        invalid(0x5e212800 | (size << 22)); // No scalar XTN.
    const uint32_t addSingle[]{0x1e212800, 0xd65f03c0}, addDouble[]{0x1e612800, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x3600, addSingle, sizeof(addSingle));
    std::memcpy(bytes.data() + 0x3610, addDouble, sizeof(addDouble));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(160, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    constexpr uint64_t qc = uint64_t{1} << 27, nzcv = 0xa0000000, canary = 0xa5a5a5a5a5a5a5a5;
    const std::array<uint64_t, 2> seed{0x0123456789abcdef, 0xfedcba9876543210};
    const auto exercise = [&](unsigned size, const auto& input, const auto& golden, std::array<unsigned, 4> saturated) {
        static_assert(sizeof(input) == 16 && sizeof(golden[0]) == 8);
        const unsigned width = 8u << size, laneBytes = width / 8, count = 64 / width;
        for (unsigned op = 0; op < 4; ++op)
            for (unsigned mode = 0; mode < 3; ++mode) {
                if (mode == 2 && op == 0)
                    continue;
                for (unsigned alias = 0; alias < 2; ++alias)
                    for (unsigned lane = 0; lane < (mode == 2 ? count : 1u); ++lane)
                        for (const auto initial : {uint64_t{0x91}, qc | 0x91}) {
                            std::memset(reinterpret_cast<void*>(memory), 0xa5, 160);
                            std::memcpy(reinterpret_cast<void*>(memory), input.data(), 16);
                            if (mode == 2)
                                std::memcpy(reinterpret_cast<void*>(memory),
                                            reinterpret_cast<const uint8_t*>(input.data()) + 2 * laneBytes * lane,
                                            2 * laneBytes);
                            std::memcpy(reinterpret_cast<void*>(memory + 16), seed.data(), 16);
                            std::array<uint8_t, 32> untouched;
                            std::memcpy(untouched.data(), reinterpret_cast<void*>(memory), 32);
                            std::array<uint8_t, 16> expected{};
                            if (mode == 1)
                                std::memcpy(expected.data(), reinterpret_cast<void*>(memory + alias * 16), 8);
                            const auto* answer = reinterpret_cast<const uint8_t*>(golden[op].data());
                            if (mode == 2)
                                std::memcpy(expected.data(), answer + lane * laneBytes, laneBytes);
                            else
                                std::memcpy(expected.data() + (mode == 1 ? 8 : 0), answer, 8);
                            const bool saturates = mode == 2 ? (saturated[op] & (1u << lane)) != 0 : saturated[op] != 0;
                            const Arm64Argument arguments[]{
                                {'J', memory}, {'J', memory + 80}, {'J', initial}, {'J', nzcv}};
                            RunArm64Function(image, image.LoadBias() + 0x1200 + 64 * index(size, op, mode, alias), 'V',
                                             {}, nullptr, nullptr, arguments);
                            STONE_CHECK(!std::memcmp(reinterpret_cast<void*>(memory + 80), expected.data(), 16));
                            STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 96) == (initial | (saturates ? qc : 0)) &&
                                        *reinterpret_cast<uint64_t*>(memory + 104) == nzcv);
                            STONE_CHECK(*reinterpret_cast<uint64_t*>(memory + 72) == canary &&
                                        *reinterpret_cast<uint64_t*>(memory + 112) == canary &&
                                        !std::memcmp(reinterpret_cast<void*>(memory), untouched.data(), 32));
                        }
            }
    };
    exercise(0, std::array<uint16_t, 8>{0, 127, 128, 255, 256, 0xffff, 0xff80, 0xff7f},
             std::array<std::array<uint8_t, 8>, 4>{{{0, 127, 128, 255, 0, 255, 128, 127},
                                                    {0, 127, 127, 127, 127, 255, 128, 128},
                                                    {0, 127, 128, 255, 255, 255, 255, 255},
                                                    {0, 127, 128, 255, 255, 0, 0, 0}}},
             {0, 0x9c, 0xf0, 0xf0});
    const std::array<uint8_t, 8> small{0, 1, 2, 3, 4, 5, 6, 7};
    exercise(0, std::array<uint16_t, 8>{0, 1, 2, 3, 4, 5, 6, 7}, std::array{small, small, small, small}, {0, 0, 0, 0});
    exercise(1, std::array<uint32_t, 4>{0x7fff, 0x8000, 0x10000, 0xffffffff},
             std::array<std::array<uint16_t, 4>, 4>{{{0x7fff, 0x8000, 0, 0xffff},
                                                     {0x7fff, 0x7fff, 0x7fff, 0xffff},
                                                     {0x7fff, 0x8000, 0xffff, 0xffff},
                                                     {0x7fff, 0x8000, 0xffff, 0}}},
             {0, 6, 12, 12});
    exercise(1, std::array<uint32_t, 4>{0xffff8000, 0xffff7fff, 0xffff, 0x80000000},
             std::array<std::array<uint16_t, 4>, 4>{{{0x8000, 0x7fff, 0xffff, 0},
                                                     {0x8000, 0x8000, 0x7fff, 0x8000},
                                                     {0xffff, 0xffff, 0xffff, 0xffff},
                                                     {0, 0, 0xffff, 0}}},
             {0, 14, 11, 11});
    exercise(
        2, std::array<uint64_t, 2>{0x7fffffff, 0x80000000},
        std::array<std::array<uint32_t, 2>, 4>{
            {{0x7fffffff, 0x80000000}, {0x7fffffff, 0x7fffffff}, {0x7fffffff, 0x80000000}, {0x7fffffff, 0x80000000}}},
        {0, 2, 0, 0});
    exercise(2, std::array<uint64_t, 2>{0xffffffff, 0x100000000},
             std::array<std::array<uint32_t, 2>, 4>{
                 {{0xffffffff, 0}, {0x7fffffff, 0x7fffffff}, {0xffffffff, 0xffffffff}, {0xffffffff, 0xffffffff}}},
             {0, 3, 2, 2});
    exercise(2, std::array<uint64_t, 2>{UINT64_MAX, 0xffffffff80000000},
             std::array<std::array<uint32_t, 2>, 4>{
                 {{0xffffffff, 0x80000000}, {0xffffffff, 0x80000000}, {0xffffffff, 0xffffffff}, {0, 0}}},
             {0, 0, 3, 3});
    exercise(2, std::array<uint64_t, 2>{0xffffffff7fffffff, 0x8000000000000000},
             std::array<std::array<uint32_t, 2>, 4>{
                 {{0x7fffffff, 0}, {0x80000000, 0x80000000}, {0xffffffff, 0xffffffff}, {0, 0}}},
             {0, 3, 3, 3});
    // Scalar floating FADD shares low opcode bits but belongs to a different class.
    const Arm64Argument singleArguments[]{{'F', std::bit_cast<uint32_t>(42.0f)}, {'F', std::bit_cast<uint32_t>(0.5f)}};
    const Arm64Argument doubleArguments[]{{'D', std::bit_cast<uint64_t>(42.0)}, {'D', std::bit_cast<uint64_t>(0.5)}};
    STONE_CHECK(RunArm64Function(image, image.LoadBias() + 0x3600, 'F', {}, nullptr, nullptr, singleArguments) ==
                std::bit_cast<uint32_t>(42.5f));
    STONE_CHECK(RunArm64Function(image, image.LoadBias() + 0x3610, 'D', {}, nullptr, nullptr, doubleArguments) ==
                std::bit_cast<uint64_t>(42.5));
    for (unsigned i = 0; i < invalidCount; ++i) {
        bool rejected = false;
        try {
            RunArm64Function(image, image.LoadBias() + 0x3400 + 16 * i, 'V', {}, nullptr, nullptr, {});
        }
        catch (const std::runtime_error&) {
            rejected = true;
        }
        STONE_CHECK(rejected);
    }
}

void CheckArm64ScalarOrderedComparisons()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t operations[]{0x5ee03400, 0x5ee03c00, 0x7ee03400, 0x7ee03c00};
    const unsigned destinations[]{4, 0, 16};
    for (unsigned operation = 0; operation < 4; ++operation)
        for (unsigned alias = 0; alias < 3; ++alias) {
            const auto dest = destinations[alias];
            const uint32_t code[]{0x3dc00004,         0x3dc00020, 0xd53b4427,
                                  0xd51b4423,         0xd51b4204, operations[operation] | (4u << 5) | dest,
                                  0x3d800040u | dest, 0xd53b4425, 0xd53b4206,
                                  0xf9000845,         0xf9000c46, 0xd51b4427,
                                  0xd65f03c0};
            // operation=0, alias=2 contains FreeType's exact CMGT word 0x5ee03490.
            std::memcpy(bytes.data() + 0x1000 + 64 * (operation * 3 + alias), code, sizeof(code));
        }
    for (unsigned operation = 0; operation < 4; ++operation)
        for (unsigned size = 0; size < 3; ++size) {
            const uint32_t code[]{(operations[operation] & ~0x00c00000u) | (size << 22), 0xd65f03c0};
            std::memcpy(bytes.data() + 0x1800 + 16 * (operation * 3 + size), code, sizeof(code));
        }
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(160, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    auto* data = reinterpret_cast<uint64_t*>(memory);

    struct Case {
        uint64_t left, right;
        unsigned matches;
    };

    // Bits encode signed >, signed >=, unsigned > and unsigned >= respectively.
    const Case cases[]{{0, 0, 10},
                       {1, 0, 15},
                       {0, 1, 0},
                       {UINT64_MAX, 0, 12},
                       {0, UINT64_MAX, 3},
                       {0x8000000000000000, 0x7fffffffffffffff, 12},
                       {0x7fffffffffffffff, 0x8000000000000000, 3},
                       {UINT64_MAX, UINT64_MAX, 10},
                       {UINT64_MAX, 0x8000000000000000, 15},
                       {0x8000000000000000, UINT64_MAX, 0},
                       {UINT64_MAX - 1, UINT64_MAX, 0}};
    constexpr uint64_t fpsr = 0x08000091, nzcv = 0xa0000000, canary = 0xa5a5a5a5a5a5a5a5;
    const Arm64Argument arguments[]{{'L', memory}, {'L', memory + 32}, {'L', memory + 80}, {'J', fpsr}, {'J', nzcv}};
    for (const auto& sample : cases)
        for (unsigned operation = 0; operation < 4; ++operation)
            for (unsigned alias = 0; alias < 3; ++alias) {
                std::memset(data, 0xa5, 160);
                data[0] = sample.left;
                data[1] = 0x123456789abcdef0;
                data[4] = sample.right;
                data[5] = 0xfedcba9876543210;
                RunArm64Function(image, image.LoadBias() + 0x1000 + 64 * (operation * 3 + alias), 'V', {}, nullptr,
                                 nullptr, arguments);
                STONE_CHECK(data[10] == ((sample.matches & (1u << operation)) ? UINT64_MAX : 0) && data[11] == 0);
                STONE_CHECK(data[12] == fpsr && data[13] == nzcv && data[9] == canary && data[14] == canary);
                STONE_CHECK(data[0] == sample.left && data[1] == 0x123456789abcdef0 && data[4] == sample.right &&
                            data[5] == 0xfedcba9876543210);
            }
    for (unsigned operation = 0; operation < 4; ++operation)
        for (unsigned size = 0; size < 3; ++size) {
            bool rejected = false;
            try {
                RunArm64Function(image, image.LoadBias() + 0x1800 + 16 * (operation * 3 + size), 'V', {}, nullptr,
                                 nullptr, {});
            }
            catch (const std::runtime_error&) {
                rejected = true;
            }
            STONE_CHECK(rejected);
        }
}

void CheckArm64HighNarrowArithmetic()
{
    auto bytes = MakeArm64TlsElf();
    bytes.resize(0x4000);
    auto* segment =
        reinterpret_cast<elf::Elf64Phdr*>(bytes.data() + sizeof(elf::Elf64Ehdr) + 3 * sizeof(elf::Elf64Phdr));
    segment->p_filesz = segment->p_memsz = 0x3000;
    const auto emit = [&](size_t offset, uint32_t operation) {
        const uint32_t code[]{
            0x3dc00000, 0x3dc00401, 0x3dc00802, 0xd53b4423, 0xf9000823, operation, 0x3d800020u | (operation & 31),
            0xd53b4423, 0xf9000c23, 0xd65f03c0};
        std::memcpy(bytes.data() + offset, code, sizeof(code));
    };
    const auto index = [](unsigned size, unsigned operation, unsigned high, unsigned dest) {
        return (size * 4 + operation) * 6 + high * 3 + dest;
    };
    for (unsigned size = 0; size < 3; ++size)
        for (unsigned op = 0; op < 4; ++op)
            for (unsigned high = 0; high < 2; ++high)
                for (unsigned dest = 0; dest < 3; ++dest)
                    emit(0x1200 + 64 * index(size, op, high, dest), 0x0e214000u | (size << 22) | (high << 30) |
                                                                        ((op & 1) << 29) | ((op & 2) ? 0x2000u : 0) |
                                                                        dest);
    emit(0x2500, 0x2e604021); // Exact FreeType word: raddhn v1.4h,v1.4s,v0.4s.
    const uint32_t invalid[]{0x0ee14002, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x2600, invalid, sizeof(invalid));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(160, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    const auto run = [&](size_t entry, const auto& left, const auto& right, const auto& narrow, unsigned high,
                         unsigned dest) {
        static_assert(sizeof(left) == 16 && sizeof(right) == 16 && sizeof(narrow) == 8);
        std::memset(reinterpret_cast<void*>(memory), 0xa5, 160);
        std::memcpy(reinterpret_cast<void*>(memory), left.data(), 16);
        std::memcpy(reinterpret_cast<void*>(memory + 16), right.data(), 16);
        std::array<uint8_t, 16> expected{};
        if (high)
            std::memcpy(expected.data(), reinterpret_cast<const void*>(memory + 16 * dest), 8);
        std::memcpy(expected.data() + 8 * high, narrow.data(), 8);
        const Arm64Argument args[]{{'J', memory}, {'J', memory + 80}};
        RunArm64Function(
            image, image.LoadBias() + entry, 'V',
            [](uintptr_t) -> std::string_view {
                return {};
            },
            nullptr, nullptr, args);
        STONE_CHECK(!std::memcmp(reinterpret_cast<void*>(memory + 80), expected.data(), 16));
        STONE_CHECK(*reinterpret_cast<const uint64_t*>(memory + 96) ==
                    *reinterpret_cast<const uint64_t*>(memory + 104));
        STONE_CHECK(*reinterpret_cast<const uint64_t*>(memory + 72) == 0xa5a5a5a5a5a5a5a5 &&
                    *reinterpret_cast<const uint64_t*>(memory + 112) == 0xa5a5a5a5a5a5a5a5 &&
                    !std::memcmp(reinterpret_cast<void*>(memory), left.data(), 16) &&
                    !std::memcmp(reinterpret_cast<void*>(memory + 16), right.data(), 16));
    };
    const auto exercise = [&](unsigned size, const auto& left, const auto& right, const auto& golden) {
        for (unsigned op = 0; op < 4; ++op)
            for (unsigned high = 0; high < 2; ++high)
                for (unsigned dest = 0; dest < 3; ++dest)
                    run(0x1200 + 64 * index(size, op, high, dest), left, right, golden[op], high, dest);
    };
    exercise(0, std::array<uint16_t, 8>{0, 0x7f, 0x80, 0xffff, 0xff80, 0x100, 0x8000, 0x7fff},
             std::array<uint16_t, 8>{0, 0, 0, 0, 0x80, 0x180, 1, 1},
             std::array<std::array<uint8_t, 8>, 4>{{{0, 0, 0, 0xff, 0, 2, 0x80, 0x80},
                                                    {0, 0, 1, 0, 0, 3, 0x80, 0x80},
                                                    {0, 0, 0, 0xff, 0xff, 0xff, 0x7f, 0x7f},
                                                    {0, 0, 1, 0, 0xff, 0, 0x80, 0x80}}});
    const std::array<uint32_t, 4> left32{0x7fff, 0x8000, 0xfffffff0, 0x80000000}, right32{0, 0, 0x10, 1};
    const std::array<std::array<uint16_t, 4>, 4> golden32{
        {{0, 0, 0, 0x8000}, {0, 1, 0, 0x8000}, {0, 0, 0xffff, 0x7fff}, {0, 1, 0, 0x8000}}};
    exercise(1, left32, right32, golden32);
    run(0x2500, left32, right32, golden32[1], 0, 1);
    exercise(2, std::array<uint64_t, 2>{UINT64_MAX, 0x8000000000000000}, std::array<uint64_t, 2>{0, 1},
             std::array<std::array<uint32_t, 2>, 4>{
                 {{0xffffffff, 0x80000000}, {0, 0x80000000}, {0xffffffff, 0x7fffffff}, {0, 0x80000000}}});
    exercise(2, std::array<uint64_t, 2>{0x7fffffff, 0x80000000}, std::array<uint64_t, 2>{0, 0},
             std::array<std::array<uint32_t, 2>, 4>{{{0, 0}, {0, 1}, {0, 0}, {0, 1}}});
    bool rejected = false;
    try {
        RunArm64Function(image, image.LoadBias() + 0x2600, 'V',
                         [](uintptr_t) -> std::string_view {
                             return {};
                         },
                         nullptr, nullptr, {});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
}

void CheckArm64VectorRoundDown()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t operations[]{0x0e219800, 0x4e219801, 0x4e619801, 0x0e219801};
    for (size_t i = 0; i < std::size(operations); ++i) {
        const uint32_t code[]{0x3dc00000, 0x3dc00401, 0xd53b4404,    0xd53b4425,
                              0xd51b4402, 0xd51b4423, operations[i], 0x3d800020u | (operations[i] & 31),
                              0xd53b4423, 0xf9000823, 0xd51b4404,    0xd51b4425,
                              0xd65f03c0};
        std::memcpy(bytes.data() + 0x1200 + i * 128, code, sizeof(code));
    }
    const uint32_t invalid[]{0x0e619801, 0xd65f03c0}; // Reserved .1D arrangement.
    std::memcpy(bytes.data() + 0x1600, invalid, sizeof(invalid));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(96, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    const auto check = [&](unsigned variant, const auto& input, const auto& expected, uint32_t control = 0,
                           uint32_t raised = 0) {
        static_assert(sizeof(input) == 16 && sizeof(expected) == 16);
        std::memset(reinterpret_cast<void*>(memory), 0xa5, 96);
        std::memcpy(reinterpret_cast<void*>(memory), input.data(), 16);
        constexpr uint32_t previous = 1u << 27;
        const Arm64Argument args[]{{'J', memory}, {'J', memory + 48}, {'J', control}, {'J', previous}};
        RunArm64Function(
            image, image.LoadBias() + 0x1200 + variant * 128, 'V',
            [](uintptr_t) -> std::string_view {
                return {};
            },
            nullptr, nullptr, args);
        STONE_CHECK(!std::memcmp(reinterpret_cast<void*>(memory + 48), expected.data(), 16));
        STONE_CHECK(*reinterpret_cast<const uint64_t*>(memory + 64) == (previous | raised));
        STONE_CHECK(*reinterpret_cast<const uint64_t*>(memory + 40) == 0xa5a5a5a5a5a5a5a5 &&
                    *reinterpret_cast<const uint64_t*>(memory + 72) == 0xa5a5a5a5a5a5a5a5 &&
                    !std::memcmp(reinterpret_cast<void*>(memory), input.data(), 16));
    };
    const std::array<uint32_t, 4> partial{0x3fe00000, 0xbfa00000, 0x7f800042, 0xff800042};
    const std::array<float, 4> partialExpected{1.0f, -2.0f, 0.0f, 0.0f};
    for (uint32_t mode = 0; mode < 4; ++mode)
        check(0, partial, partialExpected, mode << 22); // Exact fault word; aliased registers.
    check(3, partial, partialExpected);                 // Non-aliased .2S also clears the upper half.
    check(1, std::array<float, 4>{1.75f, -1.25f, 8388607.5f, -8388607.5f},
          std::array<float, 4>{1.0f, -2.0f, 8388607.0f, -8388608.0f});
    check(1, std::array<float, 4>{8388608.0f, -8388608.0f, 16777216.0f, -16777216.0f},
          std::array<float, 4>{8388608.0f, -8388608.0f, 16777216.0f, -16777216.0f});
    check(2, std::array<double, 2>{2.5, -2.5}, std::array<double, 2>{2.0, -3.0});
    check(2, std::array<double, 2>{4503599627370495.5, -4503599627370495.5},
          std::array<double, 2>{4503599627370495.0, -4503599627370496.0});
    const std::array<uint32_t, 4> special{0, 0x80000000, 0x7f800000, 0xff800000};
    check(1, special, special);
    const std::array<uint64_t, 2> infinite{0x7ff0000000000000, 0xfff0000000000000};
    check(2, infinite, infinite);
    const std::array<uint32_t, 4> nan{0x7fc01234, 0x7f801234, 0xffc05678, 0xff805678};
    check(1, nan, std::array<uint32_t, 4>{0x7fc01234, 0x7fc01234, 0xffc05678, 0xffc05678}, 0, 1);
    check(1, nan, std::array<uint32_t, 4>{0x7fc00000, 0x7fc00000, 0x7fc00000, 0x7fc00000}, 1u << 25, 1);
    const std::array<uint64_t, 2> nan64{0x7ff0000000000042, 0xfff8000000000567};
    check(2, nan64, std::array<uint64_t, 2>{0x7ff8000000000042, 0xfff8000000000567}, 0, 1);
    check(2, nan64, std::array<uint64_t, 2>{0x7ff8000000000000, 0x7ff8000000000000}, 1u << 25, 1);
    const std::array<uint32_t, 4> tiny{1, 0x80000001, 0x7fffff, 0x807fffff};
    check(1, tiny, std::array<float, 4>{0.0f, -1.0f, 0.0f, -1.0f});
    check(1, tiny, std::array<uint32_t, 4>{0, 0x80000000, 0, 0x80000000}, 1u << 24, 1u << 7);
    const std::array<uint64_t, 2> tiny64{1, 0x8000000000000001};
    check(2, tiny64, std::array<double, 2>{0.0, -1.0});
    check(2, tiny64, std::array<uint64_t, 2>{0, 0x8000000000000000}, 1u << 24, 1u << 7);
    bool rejected = false;
    try {
        RunArm64Function(image, image.LoadBias() + 0x1600, 'V',
                         [](uintptr_t) -> std::string_view {
                             return {};
                         },
                         nullptr, nullptr, {});
    }
    catch (const std::runtime_error&) {
        rejected = true;
    }
    STONE_CHECK(rejected);
}

void CheckArm64PolynomialMultiply()
{
    auto bytes = MakeArm64TlsElf();
    const uint32_t code[]{0x3dc00000, 0x3dc00401, 0x0e21e002, 0x4e21e003, 0x0e21e000, 0x3d800022,
                          0x3d800423, 0x3d800820, 0x3dc00000, 0x4e21e001, 0x3d800c21, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1200, code, sizeof(code));
    const uint32_t actual[]{0x3dc00010, 0x3dc00403, 0x0e23e210, 0x3d800030, 0xd65f03c0};
    std::memcpy(bytes.data() + 0x1280, actual, sizeof(actual));
    ElfImage image(bytes, ElfImage::Machine::Arm64, true);
    image.Protect(ElfImage::Execution::Translated);
    const auto memory = AllocateArm64GuestMemory(160, true);
    STONE_CHECK(memory);

    struct Free {
        uintptr_t value;

        ~Free()
        {
            FreeArm64GuestMemory(value);
        }
    } free{memory};

    constexpr uint8_t left[]{0xff, 0x80, 1, 0, 0x57, 0x83, 0x0e, 0xa5, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
    constexpr uint8_t right[]{0xff, 0x80, 0xff, 0x99, 0x83, 0x57, 0x0b, 0x5a,
                              0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78};
    constexpr uint16_t expected[]{0x5555, 0x4000, 0x00ff, 0,      0x2b79, 0x2b79, 0x0062, 0x2772,
                                  0x0894, 0x1eb0, 0x38a4, 0x2a80, 0x0894, 0x1eb0, 0x38a4, 0x2a80};
    std::memcpy(reinterpret_cast<void*>(memory), left, sizeof(left));
    std::memcpy(reinterpret_cast<void*>(memory + 16), right, sizeof(right));
    std::memset(reinterpret_cast<void*>(memory + 64), 0xa5, 80);
    const std::array<Arm64Argument, 2> args{{{'L', memory}, {'L', memory + 64}}};
    RunArm64Function(
        image, image.LoadBias() + 0x1200, 'V',
        [](uintptr_t) -> std::string_view {
            return {};
        },
        nullptr, nullptr, args);
    const auto* output = reinterpret_cast<const uint16_t*>(memory + 64);
    STONE_CHECK(std::equal(std::begin(expected), std::end(expected), output) &&
                std::equal(std::begin(expected), std::begin(expected) + 8, output + 16) &&
                std::equal(std::begin(expected) + 8, std::end(expected), output + 24) &&
                *reinterpret_cast<const uint64_t*>(memory + 128) == 0xa5a5a5a5a5a5a5a5);
    RunArm64Function(
        image, image.LoadBias() + 0x1280, 'V',
        [](uintptr_t) -> std::string_view {
            return {};
        },
        nullptr, nullptr, args);
    STONE_CHECK(std::equal(std::begin(expected), std::begin(expected) + 8, output));
}
}
