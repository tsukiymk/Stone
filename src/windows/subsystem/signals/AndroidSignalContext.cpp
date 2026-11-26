#include "AndroidSignalContext.h"
#include <cstring>

namespace stone::windows {
void CaptureAndroidSignalContext(AndroidSignalContext& output, const CONTEXT& source, uint64_t mask)
{
    output = {};
    output.machine.registers = {source.R8,
                                source.R9,
                                source.R10,
                                source.R11,
                                source.R12,
                                source.R13,
                                source.R14,
                                source.R15,
                                source.Rdi,
                                source.Rsi,
                                source.Rbp,
                                source.Rbx,
                                source.Rdx,
                                source.Rax,
                                source.Rcx,
                                source.Rsp,
                                source.Rip,
                                source.EFlags,
                                uint64_t{source.SegCs} | (uint64_t{source.SegGs} << 16) |
                                    (uint64_t{source.SegFs} << 32),
                                0,
                                0,
                                mask,
                                0};
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    output.stack.address = reinterpret_cast<void*>(low);
    output.stack.size = high - low;
    output.mask = mask;
    static_assert(sizeof(source.FltSave) == 512);
    std::memcpy(output.floating.data(), &source.FltSave, output.floating.size());
    // No Linux extended XSAVE trailer is present in this legacy FXSAVE image.
    std::memset(output.floating.data() + 464, 0, 48);
    output.machine.floating = output.floating.data();
}

void RestoreAndroidSignalContext(CONTEXT& output, const AndroidSignalContext& source)
{
    const auto& registers = source.machine.registers;
    output.R8 = registers[0];
    output.R9 = registers[1];
    output.R10 = registers[2];
    output.R11 = registers[3];
    output.R12 = registers[4];
    output.R13 = registers[5];
    output.R14 = registers[6];
    output.R15 = registers[7];
    output.Rdi = registers[8];
    output.Rsi = registers[9];
    output.Rbp = registers[10];
    output.Rbx = registers[11];
    output.Rdx = registers[12];
    output.Rax = registers[13];
    output.Rcx = registers[14];
    output.Rsp = registers[15];
    output.Rip = registers[16];
    constexpr DWORD writableFlags = 0x00250dd5; // Arithmetic, direction, trace, alignment and ID flags.
    output.EFlags = (output.EFlags & ~writableFlags) | (static_cast<DWORD>(registers[17]) & writableFlags);
    if (source.machine.floating == source.floating.data()) {
        std::memcpy(&output.FltSave, source.floating.data(), source.floating.size());
        output.MxCsr = output.FltSave.MxCsr;
    }
}
}
