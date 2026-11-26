#pragma once

#include "test-suite.h"
#include "loader/ElfFormat.h"
#include <array>
#include <cstring>
#include <vector>

namespace stone::windows::test::checks {
inline auto MakeElf(bool weak)
{
    using namespace stone::elf;
    std::array<uint8_t, 0x300> bytes{};
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    Elf64Ehdr header{};
    constexpr uint8_t ident[]{0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(header.e_ident, ident, sizeof(ident));
    header.e_type = 3;
    header.e_machine = 62;
    header.e_version = 1;
    header.e_ehsize = sizeof(header);
    header.e_phoff = sizeof(header);
    header.e_phentsize = sizeof(Elf64Phdr);
    header.e_phnum = 2;
    const Elf64Phdr load{1, 6, 0, 0, 0, bytes.size(), bytes.size(), 4096};
    constexpr char strings[] = "\0stone_fixture_import";
    constexpr uint64_t tags[][2]{
        {5, 0x1a0}, {10, sizeof(strings)}, {6, 0x1c0}, {11, 24}, {4, 0x1f0}, {7, 0x220}, {8, 24}, {9, 24}, {0, 0},
    };
    const Elf64Phdr dynamic{2, 6, 0x100, 0x100, 0, sizeof(tags), sizeof(tags), 8};
    put(0, header);
    put(sizeof(header), load);
    put(sizeof(header) + sizeof(load), dynamic);
    put(0x100, tags);
    put(0x1a0, strings);
    put(0x1c0 + 24, uint32_t{1});                               // st_name
    put(0x1c0 + 28, static_cast<uint8_t>(weak ? 0x21 : 0x11));  // st_info: WEAK/GLOBAL, OBJECT
    put(0x1f0, std::array<uint32_t, 5>{1, 2, 1, 0, 0});         // DT_HASH
    put(0x220, std::array<uint64_t, 3>{0x250, 0x100000006, 0}); // Elf64_Rela, R_X86_64_GLOB_DAT
    return bytes;
}

inline auto MakeArm64TlsElf()
{
    const auto original = MakeElf(false);
    std::vector<uint8_t> bytes(original.begin(), original.end());
    bytes.resize(0x2000);
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    std::memcpy(bytes.data() + 0x500, bytes.data() + 0x100, 9 * 16);
    put(18, uint16_t{183});
    put(56, uint16_t{4});
    put(sizeof(elf::Elf64Ehdr), elf::Elf64Phdr{1, 6, 0, 0, 0, 0x1000, 0x1000, 4096});
    put(sizeof(elf::Elf64Ehdr) + sizeof(elf::Elf64Phdr), elf::Elf64Phdr{2, 6, 0x500, 0x500, 0, 9 * 16, 9 * 16, 8});
    put(sizeof(elf::Elf64Ehdr) + 2 * sizeof(elf::Elf64Phdr), elf::Elf64Phdr{7, 4, 0x400, 0x400, 0, 16, 32, 16});
    put(sizeof(elf::Elf64Ehdr) + 3 * sizeof(elf::Elf64Phdr),
        elf::Elf64Phdr{1, 5, 0x1000, 0x1000, 0, 0x1000, 0x1000, 4096});
    put(0x1dc, uint8_t{0x16});
    put(0x1de, uint16_t{1}); // GLOBAL, TLS, defined.
    put(0x1e0, uint64_t{8});
    put(0x1e8, uint64_t{8});
    put(0x220, elf::Elf64Rela{0x280, (uint64_t{1} << 32) | 1031, 0});
    put(0x408, uint64_t{41});
    const auto delta = uint32_t(0x280 - 0x1108) & 0x1fffff;
    const uint32_t code[]{
        0xa9bf7bfd,
        0x910003fd,                                             // Save frame pointer and return address.
        0x10000000 | ((delta & 3) << 29) | ((delta >> 2) << 5), // ADR x0,descriptor.
        0xf9400001,
        0xd2800aa2,
        0xd63f0020, // LDR x1,[x0]; MOV x2,#85; BLR x1.
        0xd53bd043,
        0x8b000060, // MRS x3,TPIDR_EL0; ADD x0,x3,x0.
        0xf9400004,
        0x91000484,
        0xf9000004, // Increment the current thread's TLS counter.
        0x8b020080,
        0xa8c17bfd,
        0xd65f03c0, // Return counter+x2; restore frame.
    };
    put(0x1100, code);
    return bytes;
}

inline auto MakeArm64StaticTlsElf()
{
    auto bytes = MakeArm64TlsElf();
    const auto put = [&](size_t at, const auto& value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    put(sizeof(elf::Elf64Ehdr) + sizeof(elf::Elf64Phdr), elf::Elf64Phdr{2, 6, 0x500, 0x500, 0, 10 * 16, 10 * 16, 8});
    put(0x580, std::array<uint64_t, 2>{30, 16}); // DF_STATIC_TLS.
    put(0x590, std::array<uint64_t, 2>{0, 0});
    put(0x220, elf::Elf64Rela{0x280, (uint64_t{1} << 32) | 1030, 0}); // TLS_TPREL.
    const auto delta = uint32_t(0x280 - 0x1100) & 0x1fffff;
    const uint32_t code[]{
        0x10000000 | ((delta & 3) << 29) | ((delta >> 2) << 5),
        0xf9400000,
        0xd53bd043,
        0x8b000060, // LDR x0,[x0]; MRS x3,TPIDR_EL0; ADD x0,x3,x0.
        0xf9400004,
        0x91000484,
        0xf9000004,
        0xaa0403e0,
        0xd65f03c0,
    };
    put(0x1100, code);
    const uint32_t slot[]{0xd53bd041, 0xd2822440, 0xf9001c20, 0xf9401c20, 0xd65f03c0}; // slot 7 = 0x1122.
    put(0x1180, slot);
    return bytes;
}
}
