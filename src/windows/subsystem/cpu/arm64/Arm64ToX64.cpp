#include "Arm64ToX64.h"
#include "Arm64ExecutionProfile.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace stone::windows {
namespace {
// The first four arguments arrive in rcx, rdx, r8 and r9. rax and r10
// carry guest x4 and x5. r11 is reserved for code generation.
constexpr std::array<unsigned, 6> HostRegisters{1, 2, 8, 9, 0, 10};
constexpr unsigned Scratch = 11;
constexpr unsigned Zero = 31;

class Translator {
public:
    explicit Translator(std::span<const uint8_t> bytes) : code(bytes)
    {
    }

    std::vector<uint8_t> Run(std::span<const unsigned> arguments, unsigned result)
    {
        if (arguments.size() > HostRegisters.size() || (result != 0 && result != 32 && result != 64))
            Fail("unsupported scalar signature");
        for (size_t i = 0; i < arguments.size(); ++i) {
            const auto width = arguments[i];
            if (width != 0 && width != 32 && width != 64)
                Fail("unsupported argument width");
            defined[i] = width;
            if (i < 4 || width == 0)
                continue;
            const auto reg = HostRegisters[i];
            Rex(width, reg, 4);
            output.insert(output.end(), {0x8b, ModRM(1, reg, 4), 0x24, static_cast<uint8_t>(8 + i * 8)});
        }

        for (; offset + 4 <= code.size(); offset += 4) {
            uint32_t word;
            std::memcpy(&word, code.data() + offset, sizeof(word));
            if (word == 0xd65f03c0) { // ret x30
                if (result)
                    Move(0, Read(0, result), result);
                output.push_back(0xc3);
                return ProfileArm64Leaf(std::move(output), static_cast<uint32_t>(offset / 4 + 1));
            }
            if (word == 0xd503201f || word == 0xd503245f || word == 0xd50324df)
                continue; // nop, bti c/jc
            const unsigned width = (word >> 31) ? 64 : 32;
            if ((word & 0x1f800000) == 0x12800000)
                WideMove(word, width);
            else if ((word & 0x1f000000) == 0x11000000 && !(word & 0x20800000))
                ImmediateArithmetic(word, width);
            else if ((word & 0x1f200000) == 0x0b000000 && !(word & 0x20000000))
                Shifted(word, width, true);
            else if ((word & 0x1f200000) == 0x0a000000 && ((word >> 29) & 3) != 3)
                Shifted(word, width, false);
            else if ((word & 0xfffffc00) == 0x93407c00) {
                const auto source = Read((word >> 5) & 31, 32);
                const auto dest = Destination(word & 31);
                if (source == Zero)
                    Immediate(dest, 0, 64);
                else
                    RegisterOp(0x63, source, dest, 64);
            }
            else
                Fail("unsupported AArch64 instruction");
        }
        Fail("no return in AArch64 translation window");
    }

private:
    [[noreturn]] void Fail(const char* message) const
    {
        throw Arm64TranslationError(std::string(message) + " at AArch64 byte " + std::to_string(offset));
    }

    unsigned Read(unsigned reg, unsigned width) const
    {
        if (reg == Zero)
            return Zero;
        if (reg >= defined.size() || defined[reg] < width)
            Fail("read of an unavailable AArch64 register");
        return HostRegisters[reg];
    }

    unsigned Destination(unsigned reg)
    {
        if (reg >= defined.size())
            Fail("unsupported AArch64 destination register");
        // Both ISAs zero the upper half of an integer register on 32-bit writes.
        defined[reg] = 64;
        return HostRegisters[reg];
    }

    static uint8_t ModRM(unsigned mod, unsigned reg, unsigned rm)
    {
        return static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7));
    }

    void Rex(unsigned width, unsigned reg, unsigned rm)
    {
        const auto rex = static_cast<uint8_t>(0x40 | (width == 64 ? 8 : 0) | ((reg & 8) >> 1) | ((rm & 8) >> 3));
        if (rex != 0x40)
            output.push_back(rex);
    }

    void EmitImmediate(uint64_t value, unsigned bytes)
    {
        for (unsigned i = 0; i < bytes; ++i)
            output.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }

    void RegisterOp(uint8_t opcode, unsigned rm, unsigned reg, unsigned width)
    {
        Rex(width, reg, rm);
        output.insert(output.end(), {opcode, ModRM(3, reg, rm)});
    }

    void Immediate(unsigned dest, uint64_t value, unsigned width)
    {
        Rex(width, 0, dest);
        output.push_back(static_cast<uint8_t>(0xb8 + (dest & 7)));
        EmitImmediate(value, width / 8);
    }

    void Move(unsigned dest, unsigned source, unsigned width)
    {
        if (source == Zero)
            Immediate(dest, 0, width);
        else if (source != dest || width == 32)
            RegisterOp(0x8b, source, dest, width);
    }

    void WideMove(uint32_t word, unsigned width)
    {
        const unsigned reg = word & 31, kind = (word >> 29) & 3;
        const unsigned shift = ((word >> 21) & 3) * 16;
        if (kind == 1 || shift >= width)
            Fail("invalid AArch64 wide move");
        const uint64_t value = uint64_t((word >> 5) & 0xffff) << shift;
        if (kind == 3) {
            Read(reg, width);
            const auto dest = Destination(reg);
            Immediate(Scratch, ~(uint64_t{0xffff} << shift), width);
            RegisterOp(0x23, Scratch, dest, width);
            Immediate(Scratch, value, width);
            RegisterOp(0x0b, Scratch, dest, width);
        }
        else {
            Immediate(Destination(reg), kind == 0 ? ~value : value, width);
        }
    }

    void ImmediateArithmetic(uint32_t word, unsigned width)
    {
        const unsigned source = (word >> 5) & 31;
        if (source == Zero)
            Fail("unsupported AArch64 stack pointer operand");
        const auto left = Read(source, width);
        const auto dest = Destination(word & 31);
        Move(dest, left, width);
        Rex(width, 0, dest);
        output.insert(output.end(), {0x81, ModRM(3, (word & 0x40000000) ? 5 : 0, dest)});
        EmitImmediate(((word >> 10) & 0xfff) << ((word & 0x00400000) ? 12 : 0), 4);
    }

    void Shifted(uint32_t word, unsigned width, bool arithmetic)
    {
        const unsigned amount = (word >> 10) & 63, shift = (word >> 22) & 3;
        if (amount >= width || (arithmetic && shift == 3))
            Fail("invalid AArch64 shift");
        const auto left = Read((word >> 5) & 31, width);
        const auto right = Read((word >> 16) & 31, width);
        const auto dest = Destination(word & 31);
        Move(Scratch, right, width);
        if (amount) {
            constexpr std::array<unsigned, 4> ShiftExtensions{4, 5, 7, 1}; // shl, shr, sar, ror
            Rex(width, 0, Scratch);
            output.insert(output.end(),
                          {0xc1, ModRM(3, ShiftExtensions[shift], Scratch), static_cast<uint8_t>(amount)});
        }
        Move(dest, left, width);
        constexpr std::array<uint8_t, 3> Logical{0x23, 0x0b, 0x33}; // and, or, xor
        const uint8_t opcode = arithmetic ? ((word & 0x40000000) ? 0x2b : 0x03) : Logical[(word >> 29) & 3];
        RegisterOp(opcode, Scratch, dest, width);
    }

    std::span<const uint8_t> code;
    size_t offset = 0;
    std::array<unsigned, HostRegisters.size()> defined{};
    std::vector<uint8_t> output;
};

} // namespace

std::vector<uint8_t> TranslateArm64Leaf(std::span<const uint8_t> code, std::span<const unsigned> arguments,
                                        unsigned resultWidth)
{
    return Translator(code).Run(arguments, resultWidth);
}

} // namespace stone::windows
