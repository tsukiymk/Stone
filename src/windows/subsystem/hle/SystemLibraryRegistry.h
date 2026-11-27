#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace stone::windows {
struct SystemLibrary {
    std::string_view name;
    uintptr_t (*lookup)(std::string_view);
    bool (*available)() = nullptr;
};

inline constexpr size_t SystemLibraryCount = 20;
extern const std::array<SystemLibrary, SystemLibraryCount> systemLibraries;
size_t SystemLibraryIndex(std::string_view name);
size_t SystemLibraryPathIndex(std::string_view path);

// Context is required: ART and translated special/data exports precede HLE.
struct SystemSymbolContext {
    bool artRuntime;
    bool translate;
};

uintptr_t LookupHleSystemSymbol(const SystemLibrary& library, std::string_view symbol, SystemSymbolContext context);
}
