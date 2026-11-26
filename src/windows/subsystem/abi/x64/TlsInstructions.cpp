#include "abi/x64/TlsInstructions.h"
#include "loader/ElfImage.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace stone::windows::x64_tls {
constexpr int64_t ScratchOffset = -136;

DWORD64* ContextRegister(CONTEXT& context, unsigned index)
{
    DWORD64* registers[]{&context.Rax, &context.Rcx, &context.Rdx, &context.Rbx, nullptr,      &context.Rbp,
                         &context.Rsi, &context.Rdi, &context.R8,  &context.R9,  &context.R10, &context.R11,
                         &context.R12, &context.R13, &context.R14, &context.R15};
    return index < std::size(registers) ? registers[index] : nullptr;
}

ZydisEncoderOperand Register(ZydisRegister value)
{
    ZydisEncoderOperand operand{};
    operand.type = ZYDIS_OPERAND_TYPE_REGISTER;
    operand.reg.value = value;
    return operand;
}

ZydisEncoderOperand Memory(ZydisRegister base, ZydisRegister index, uint8_t scale, int64_t displacement)
{
    ZydisEncoderOperand operand{};
    operand.type = ZYDIS_OPERAND_TYPE_MEMORY;
    operand.mem = {base, index, scale, displacement, 8};
    return operand;
}

void Encode(std::vector<uint8_t>& code, const ZydisEncoderRequest& request)
{
    std::array<uint8_t, ZYDIS_MAX_INSTRUCTION_LENGTH> bytes{};
    ZyanUSize length = bytes.size();
    if (ZYAN_FAILED(ZydisEncoderEncodeInstruction(&request, bytes.data(), &length)))
        throw std::runtime_error("could not encode ART GS replacement instruction");
    code.insert(code.end(), bytes.begin(), bytes.begin() + length);
}

void EncodeBinary(std::vector<uint8_t>& code, ZydisMnemonic mnemonic, ZydisEncoderOperand left,
                  ZydisEncoderOperand right, ZydisInstructionAttributes prefixes)
{
    if (mnemonic == ZYDIS_MNEMONIC_LEA && left.type == ZYDIS_OPERAND_TYPE_REGISTER &&
        right.type == ZYDIS_OPERAND_TYPE_MEMORY)
        right.mem.size = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, left.reg.value) / 8;
    ZydisEncoderRequest request{};
    request.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
    request.mnemonic = mnemonic;
    request.prefixes = prefixes;
    request.operand_count = 2;
    request.operands[0] = left;
    request.operands[1] = right;
    Encode(code, request);
}

bool ControlOperation(ZydisMnemonic mnemonic)
{
    return mnemonic == ZYDIS_MNEMONIC_CALL || mnemonic == ZYDIS_MNEMONIC_JMP || mnemonic == ZYDIS_MNEMONIC_PUSH ||
           mnemonic == ZYDIS_MNEMONIC_POP;
}

bool DirectControlOperation(ZydisMnemonic mnemonic)
{
    return mnemonic == ZYDIS_MNEMONIC_CALL || mnemonic == ZYDIS_MNEMONIC_JMP;
}

ZydisRegister ScratchRegister(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands,
                              ZydisRegister exclude)
{
    std::array<bool, 16> used{};
    const auto mark = [&](ZydisRegister reg) {
        reg = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
        if (ZydisRegisterGetClass(reg) != ZYDIS_REGCLASS_GPR64)
            return;
        const auto id = ZydisRegisterGetId(reg);
        if (id >= 0 && id < used.size())
            used[id] = true;
    };
    used[4] = true; // RSP.
    mark(exclude);
    for (unsigned i = 0; i < instruction.operand_count; ++i) {
        const auto& operand = operands[i];
        if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, operand.reg.value) == ZYDIS_REGISTER_RSP &&
                (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) && !DirectControlOperation(instruction.mnemonic))
                throw std::runtime_error("ART GS instruction writes the stack pointer");
            mark(operand.reg.value);
        }
        else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            mark(operand.mem.base);
            mark(operand.mem.index);
        }
    }
    constexpr ZydisRegister candidates[]{ZYDIS_REGISTER_R11, ZYDIS_REGISTER_R10, ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX,
                                         ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_R8,  ZYDIS_REGISTER_R9,  ZYDIS_REGISTER_R12,
                                         ZYDIS_REGISTER_R13, ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15, ZYDIS_REGISTER_RBX,
                                         ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI, ZYDIS_REGISTER_RBP};
    for (const auto reg : candidates)
        if (!used[ZydisRegisterGetId(reg)])
            return reg;
    throw std::runtime_error("ART GS instruction has no available scratch register");
}

void MakeTrampoline(std::vector<uint8_t>& code, const ZydisDecodedInstruction& instruction,
                    const ZydisDecodedOperand* operands, unsigned memoryOperand, uintptr_t next,
                    std::vector<TlsLoadSite>& tlsLoads)
{
    // An absolute MOV overwrites its destination without reading it. Use that
    // register for the thread pointer instead of spilling a second register.
    const bool reuseDestination = instruction.mnemonic == ZYDIS_MNEMONIC_MOV && instruction.address_width == 64 &&
                                  instruction.operand_count_visible == 2 && memoryOperand == 1 &&
                                  operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                                  ZydisRegisterGetClass(operands[0].reg.value) == ZYDIS_REGCLASS_GPR64 &&
                                  operands[0].reg.value != ZYDIS_REGISTER_RSP &&
                                  operands[1].mem.base == ZYDIS_REGISTER_NONE &&
                                  operands[1].mem.index == ZYDIS_REGISTER_NONE;
    const auto scratch = reuseDestination ? operands[0].reg.value : ScratchRegister(instruction, operands);
    const auto offsetRegister =
        instruction.address_width == 32 ? ScratchRegister(instruction, operands, scratch) : ZYDIS_REGISTER_NONE;
    if (!reuseDestination) {
        // Save below the complete SysV red zone. LEA and MOV preserve the guest flags.
        EncodeBinary(code, ZYDIS_MNEMONIC_LEA, Register(ZYDIS_REGISTER_RSP),
                     Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, ScratchOffset));
        EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, 0),
                     Register(scratch));
        if (offsetRegister != ZYDIS_REGISTER_NONE)
            EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, -8),
                         Register(offsetRegister));
        EncodeBinary(code, ZYDIS_MNEMONIC_LEA, Register(ZYDIS_REGISTER_RSP),
                     Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, -ScratchOffset));
    }
    tlsLoads.push_back({code.size(), scratch});
    EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Register(scratch),
                 Memory(ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, AndroidArtThreadSlot * sizeof(uintptr_t)),
                 ZYDIS_ATTRIB_HAS_SEGMENT_FS);
    const auto& memory = operands[memoryOperand].mem;
    if (memory.base == ZYDIS_REGISTER_RIP)
        throw std::runtime_error("ART GS instruction uses RIP-relative addressing");
    if (offsetRegister != ZYDIS_REGISTER_NONE) {
        // Address-size override wraps the offset to 32 bits before adding GS.base.
        const auto narrow = ZydisRegisterEncode(ZYDIS_REGCLASS_GPR32, ZydisRegisterGetId(offsetRegister));
        EncodeBinary(
            code, ZYDIS_MNEMONIC_LEA, Register(narrow),
            Memory(memory.base, memory.index, memory.scale, memory.disp.has_displacement ? memory.disp.value : 0));
        EncodeBinary(code, ZYDIS_MNEMONIC_LEA, Register(scratch), Memory(scratch, offsetRegister, 1, 0));
    }
    else {
        if (memory.base != ZYDIS_REGISTER_NONE)
            EncodeBinary(code, ZYDIS_MNEMONIC_LEA, Register(scratch), Memory(scratch, memory.base, 1, 0));
        if (memory.index != ZYDIS_REGISTER_NONE)
            EncodeBinary(code, ZYDIS_MNEMONIC_LEA, Register(scratch), Memory(scratch, memory.index, memory.scale, 0));
    }
    if (DirectControlOperation(instruction.mnemonic)) {
        // Resolve the destination below the caller's complete SysV red zone,
        // then restore every register before transferring control. A CALL must
        // expose the original return PC to ART's stack walker, not this thunk.
        constexpr int64_t targetOffset = ScratchOffset - 16;
        EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Register(scratch),
                     Memory(scratch, ZYDIS_REGISTER_NONE, 0,
                            offsetRegister != ZYDIS_REGISTER_NONE ? 0
                            : memory.disp.has_displacement        ? memory.disp.value
                                                                  : 0));
        EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, targetOffset),
                     Register(scratch));
        const bool call = instruction.mnemonic == ZYDIS_MNEMONIC_CALL;
        if (call) {
            ZydisEncoderOperand address{};
            address.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            address.imm.u = next;
            EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Register(scratch), address);
            EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, -8),
                         Register(scratch));
        }
        EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Register(scratch),
                     Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, ScratchOffset));
        if (offsetRegister != ZYDIS_REGISTER_NONE)
            EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Register(offsetRegister),
                         Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, ScratchOffset - 8));
        if (call)
            EncodeBinary(code, ZYDIS_MNEMONIC_LEA, Register(ZYDIS_REGISTER_RSP),
                         Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, -8));
        ZydisEncoderRequest jump{};
        jump.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
        jump.mnemonic = ZYDIS_MNEMONIC_JMP;
        jump.operand_count = 1;
        jump.operands[0] = Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, targetOffset + (call ? 8 : 0));
        Encode(code, jump);
        return;
    }
    ZydisEncoderRequest request{};
    if (ZYAN_FAILED(ZydisEncoderDecodedInstructionToEncoderRequest(&instruction, operands,
                                                                   instruction.operand_count_visible, &request)))
        throw std::runtime_error("could not translate ART GS instruction operands");
    request.prefixes &= ~ZYDIS_ATTRIB_HAS_SEGMENT_GS;
    request.address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_64;
    request.operands[memoryOperand].mem.base = scratch;
    request.operands[memoryOperand].mem.index = ZYDIS_REGISTER_NONE;
    request.operands[memoryOperand].mem.scale = 0;
    if (offsetRegister != ZYDIS_REGISTER_NONE)
        request.operands[memoryOperand].mem.displacement = 0;
    Encode(code, request);
    if (!reuseDestination) {
        EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Register(scratch),
                     Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, ScratchOffset));
        if (offsetRegister != ZYDIS_REGISTER_NONE)
            EncodeBinary(code, ZYDIS_MNEMONIC_MOV, Register(offsetRegister),
                         Memory(ZYDIS_REGISTER_RSP, ZYDIS_REGISTER_NONE, 0, ScratchOffset - 8));
    }
    constexpr uint8_t jump[]{0xff, 0x25, 0, 0, 0, 0}; // JMP [RIP], followed by its destination.
    code.insert(code.end(), std::begin(jump), std::end(jump));
    const auto* address = reinterpret_cast<const uint8_t*>(&next);
    code.insert(code.end(), address, address + sizeof(next));
}

uintptr_t RegisterValue(ZydisRegister reg, const CONTEXT& context, uintptr_t next)
{
    if (reg == ZYDIS_REGISTER_NONE)
        return 0;
    if (reg == ZYDIS_REGISTER_RIP)
        return next;
    const DWORD64 registers[]{context.Rax, context.Rcx, context.Rdx, context.Rbx, context.Rsp, context.Rbp,
                              context.Rsi, context.Rdi, context.R8,  context.R9,  context.R10, context.R11,
                              context.R12, context.R13, context.R14, context.R15};
    const auto id = ZydisRegisterGetId(reg);
    return id >= 0 && id < std::size(registers) ? registers[id] : 0;
}
}
