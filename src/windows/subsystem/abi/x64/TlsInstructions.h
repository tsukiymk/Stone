#pragma once

#include <windows.h>
#include <Zydis/Zydis.h>
#include <cstdint>
#include <vector>

namespace stone::windows::x64_tls {
DWORD64* ContextRegister(CONTEXT& context, unsigned index);
ZydisEncoderOperand Register(ZydisRegister value);
ZydisEncoderOperand Memory(ZydisRegister base, ZydisRegister index, uint8_t scale, int64_t displacement);
void Encode(std::vector<uint8_t>& code, const ZydisEncoderRequest& request);
void EncodeBinary(std::vector<uint8_t>& code, ZydisMnemonic mnemonic, ZydisEncoderOperand left,
                  ZydisEncoderOperand right, ZydisInstructionAttributes prefixes = 0);
bool ControlOperation(ZydisMnemonic mnemonic);
bool DirectControlOperation(ZydisMnemonic mnemonic);
ZydisRegister ScratchRegister(const ZydisDecodedInstruction&, const ZydisDecodedOperand*,
                              ZydisRegister exclude = ZYDIS_REGISTER_NONE);

struct TlsLoadSite {
    size_t offset;
    ZydisRegister destination;
};

void MakeTrampoline(std::vector<uint8_t>& code, const ZydisDecodedInstruction& instruction,
                    const ZydisDecodedOperand* operands, unsigned memoryOperand, uintptr_t next,
                    std::vector<TlsLoadSite>& tlsLoads);
uintptr_t RegisterValue(ZydisRegister reg, const CONTEXT& context, uintptr_t next);
}
