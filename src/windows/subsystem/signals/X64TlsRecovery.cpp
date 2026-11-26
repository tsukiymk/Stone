#include "signals/X64TlsRecovery.h"
#include "abi/x64/TlsInstructions.h"
#include "loader/ElfImage.h"
#include <array>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace stone::windows {
using namespace x64_tls;

bool RecoverAndroidFsLoad(CONTEXT& context, uintptr_t faultAddress)
{
    const auto* instruction = reinterpret_cast<const uint8_t*>(context.Rip);
    uint32_t offset;
    std::memcpy(&offset, instruction + 5, sizeof(offset));
    if (instruction[0] != 0x64 || (instruction[1] & ~4) != 0x48 || instruction[2] != 0x8b ||
        (instruction[3] & 0xc7) != 4 || instruction[4] != 0x25 || offset != faultAddress ||
        (offset != 0 && offset != AndroidStackGuardSlot * sizeof(uintptr_t) &&
         offset != AndroidArtThreadSlot * sizeof(uintptr_t)))
        return false;
    const unsigned destination = ((instruction[3] >> 3) & 7) + ((instruction[1] & 4) ? 8 : 0);
    auto* target = ContextRegister(context, destination);
    const auto* tls = ElfImage::CurrentGuestThreadPointer();
    if (!target || !tls)
        return false;
    try {
        // Cache immutable, separately protected pages. Include the instruction's
        // operands so unloading and reusing an ELF address cannot reuse stale code.
        // Published recovery code lives for the process lifetime, like ABI thunks.
        static std::mutex mutex;
        static std::map<std::pair<uintptr_t, uint32_t>, uintptr_t> recoveries;
        const auto key = std::pair{static_cast<uintptr_t>(context.Rip), (offset << 8) | destination};
        const std::lock_guard lock(mutex);
        auto found = recoveries.find(key);
        if (found == recoveries.end()) {
            std::vector<uint8_t> code;
            ZydisEncoderRequest restore{};
            restore.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
            restore.mnemonic = ZYDIS_MNEMONIC_WRFSBASE;
            restore.operand_count = 1;
            restore.operands[0] = Register(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64, static_cast<ZyanU8>(destination)));
            Encode(code, restore);
            // A Windows transition can clear FS even between these two
            // instructions. Read through the known pointer, so recovery itself
            // does not depend on FS remaining set. The next guest read can
            // safely recover again at its original, owned ELF instruction.
            EncodeBinary(code, ZYDIS_MNEMONIC_MOV, restore.operands[0],
                         Memory(restore.operands[0].reg.value, ZYDIS_REGISTER_NONE, 0, offset));
            constexpr uint8_t jump[]{0xff, 0x25, 0, 0, 0, 0};
            code.insert(code.end(), std::begin(jump), std::end(jump));
            const uintptr_t next = context.Rip + 9;
            const auto* address = reinterpret_cast<const uint8_t*>(&next);
            code.insert(code.end(), address, address + sizeof(next));
            const auto free = [](void* memory) {
                if (memory)
                    VirtualFree(memory, 0, MEM_RELEASE);
            };
            std::unique_ptr<void, decltype(free)> memory(
                VirtualAlloc(nullptr, code.size(), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), free);
            if (!memory)
                return false;
            std::memcpy(memory.get(), code.data(), code.size());
            DWORD previous;
            if (!VirtualProtect(memory.get(), code.size(), PAGE_EXECUTE_READ, &previous) ||
                !FlushInstructionCache(GetCurrentProcess(), memory.get(), code.size()))
                return false;
            const auto start = reinterpret_cast<uintptr_t>(memory.get());
            found = recoveries.emplace(key, start).first;
            memory.release();
        }
        // NtContinue may clear FS after returning from this handler. Restore it
        // in guest code, then perform the original load and jump to its next PC.
        // The MOV's destination is the only changed register; flags and stack
        // (including the complete SysV red zone) remain untouched.
        *target = reinterpret_cast<uintptr_t>(tls);
        context.Rip = found->second;
        return true;
    }
    catch (const std::exception&) {
        return false; // The caller can retain its scalar fallback on allocation failure.
    }
}

}
