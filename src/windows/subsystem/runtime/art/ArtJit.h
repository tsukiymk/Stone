#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace stone::windows {
class ElfImage;
void ConfigureArtJit(ElfImage& runtime);
bool DispatchArtJitBreakpoint(uintptr_t address, CONTEXT& context);
bool RecoverArtJitTlsLoad(CONTEXT& context);

struct ArtJitStatistics {
    size_t methods, instructions, tlsRecoveries, breakpoints;
};

ArtJitStatistics GetArtJitStatistics();
bool ArtJitMethodCompiled(std::string_view name);
}
