#include "subsystem/support/test-suite.h"
#include "cpu/arm64/Arm64Initializer.h"
#include "loader/ElfImage.h"
#include <array>
#include <cstring>

namespace stone::windows::test {
void CheckArm64ScalarDecode()
{
    using stone::elf::Elf64Ehdr;
    using stone::elf::Elf64Phdr;
    using stone::elf::Elf64Sym;
    constexpr std::array code{
        // FMADD s2,s26,s26,s1 with 3*3+2. Return 1 if the result is wrong.
        0x1e21101au,
        0x1e201001u,
        0x1f1a0742u,
        0x1e260049u,
        0x52a8260au,
        0x52800020u,
        0x6b0a013fu,
        0x54000201u,
        // USHR d0,d2,#16 with both input halves populated. Return 2 if the
        // low half is wrong or if the upper half was not cleared.
        0xd286420au,
        0xf2aeca8au,
        0xf2d7530au,
        0xf2ffdb8au,
        0x9e670142u,
        0x4e181d42u,
        0x7f700440u,
        0x9e660009u,
        0xd350fd4au,
        0x52800040u,
        0xeb0a013fu,
        0x54000081u,
        0x4e183c0bu,
        0xb500004bu,
        0x52800000u,
        0xd65f03c0u,
    };

    struct Fixture {
        Elf64Ehdr header{};
        Elf64Phdr segments[2]{};
        uint64_t dynamic[6][2]{};
        uint32_t hash[4]{1, 1, 0, 0};
        Elf64Sym nullSymbol{};
        char emptyName{};
        std::array<uint32_t, code.size()> instructions{};
    } fixture;

    constexpr uint8_t ident[]{0x7f, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(fixture.header.e_ident, ident, sizeof(ident));
    fixture.header.e_type = 3;
    fixture.header.e_machine = static_cast<uint16_t>(ElfImage::Machine::Arm64);
    fixture.header.e_version = 1;
    fixture.header.e_ehsize = sizeof(Elf64Ehdr);
    fixture.header.e_phoff = offsetof(Fixture, segments);
    fixture.header.e_phentsize = sizeof(Elf64Phdr);
    fixture.header.e_phnum = 2;
    fixture.segments[0] = {1, 5, 0, 0, 0, sizeof(fixture), sizeof(fixture), 4096};
    constexpr auto dynamic = offsetof(Fixture, dynamic);
    fixture.segments[1] = {2, 4, dynamic, dynamic, 0, sizeof(fixture.dynamic), sizeof(fixture.dynamic), 8};
    constexpr uint64_t tags[][2]{
        {5, offsetof(Fixture, emptyName)}, {10, 1}, {6, offsetof(Fixture, nullSymbol)}, {11, sizeof(Elf64Sym)},
        {4, offsetof(Fixture, hash)},      {0, 0},
    };
    std::memcpy(fixture.dynamic, tags, sizeof(tags));
    fixture.instructions = code;

    const auto* bytes = reinterpret_cast<const uint8_t*>(&fixture);
    ElfImage image({bytes, sizeof(fixture)}, ElfImage::Machine::Arm64);
    image.Protect(ElfImage::Execution::Translated);
    STONE_CHECK(RunArm64Initializer(image, image.LoadBias() + offsetof(Fixture, instructions), 32, {}, nullptr) == 0);
}
}
