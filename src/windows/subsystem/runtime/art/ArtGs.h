#pragma once

#include "signals/X64TlsRecovery.h"

#include <windows.h>
#include <Zydis/Zydis.h>
#include <cstdint>
#include <span>
#include <map>
#include <string>
#include <vector>

namespace stone::windows {
class ElfImage;

// Private registration entry points are retained in Android's mini debug data.
// They must be used only with framework classes from the same selected image.
struct ElfDebugSymbol {
    uint64_t address, size;
    bool code;
};

std::map<std::string, ElfDebugSymbol, std::less<>> ElfDebugSymbols(std::span<const uint8_t> file);

class ArtGsCode {
public:
    ArtGsCode(ElfImage& image, std::span<const uint8_t> file, bool oatCode = false);
    // Adapt one unpublished JIT method in place, preserving all original PCs.
    explicit ArtGsCode(std::span<uint8_t> method);
    ~ArtGsCode();
    ArtGsCode(const ArtGsCode&) = delete;
    ArtGsCode& operator=(const ArtGsCode&) = delete;
    bool Dispatch(uintptr_t address, CONTEXT& context) const;
    bool RecoverTlsLoad(CONTEXT& context) const;

    size_t Size() const
    {
        return traps_.size();
    }

    bool MatchesPublishedCode() const;

    uintptr_t ReplacementCodeBegin() const
    {
        return reinterpret_cast<uintptr_t>(code_);
    }

    uintptr_t ReplacementCodeEnd() const
    {
        return ReplacementCodeBegin() + codeSize_;
    }

private:
    ArtGsCode(ElfImage* image, std::span<const uint8_t> file, std::span<uint8_t> method, bool oatCode = false);

    struct Patch {
        uintptr_t address;
        std::vector<uint8_t> bytes;
    };

    std::vector<Patch> patches_;

    struct Trap {
        uintptr_t address;
        uintptr_t next;
        uintptr_t trampoline;
        ZydisMnemonic operation;
        ZydisDecodedOperandMem memory;
        uint8_t addressWidth;
    };

    std::vector<Trap> traps_;

    struct TlsRecovery {
        uintptr_t address;
        uintptr_t resume;
        uint8_t destination;
    };

    std::vector<TlsRecovery> tlsRecoveries_;
    void* code_ = nullptr;
    size_t codeSize_ = 0;
};
}
