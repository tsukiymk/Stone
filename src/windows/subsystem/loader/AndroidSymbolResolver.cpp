#include "loader/AndroidSymbolResolver.h"

namespace stone::windows::android_libraries {
uintptr_t LookupSystemSymbol(const SystemLibrary& library, std::string_view symbol)
{
    auto& state = Process();
    const auto address = LookupHleSystemSymbol(library, symbol, {CurrentNamespace().artRuntime, state.translate});
    if (address && state.translate)
        state.arm64ImportNames.try_emplace(address, symbol);
    return address;
}

ElfImage::LinkSymbol DependencySymbol(const Library& lib, std::string_view symbol,
                                      [[maybe_unused]] const ElfImage* imageUnwind,
                                      AndroidElfLinkReport::Binding* binding)
{
    const auto record = [&](ElfImage::LinkSymbol result, std::string_view provider, bool host) {
        if (result && binding) {
            binding->provider = provider;
            binding->hostBridge = host;
        }
        return result;
    };
    auto systemDependencies = lib.systemDependencies;
    if (auto result = FindDependency(lib, [&](const Library& dependency) {
            systemDependencies |= dependency.systemDependencies;
            if (dependency.scope->imageLog && dependency.name == "liblog.so") {
                if (const auto address = LookupSystemSymbol(systemLibraries[SystemLibraryIndex("liblog.so")], symbol))
                    return record(address, dependency.name, true);
            }
            return record(dependency.image.FindLinkSymbol(symbol), dependency.name, false);
        }))
        return result;
    for (size_t i = 0; i < SystemLibraryCount; ++i) {
        if (!systemDependencies[i])
            continue;
        const auto& library = systemLibraries[i];
        if (const auto address = LookupSystemSymbol(library, symbol))
            return record(address, library.name, true);
        if (imageUnwind && library.name == "libc.so" &&
            (symbol.starts_with("_Unwind_") || symbol == "__libc_current_sigrtmin" ||
             symbol == "__libc_current_sigrtmax" || symbol == "rand_r"))
            if (const auto result = imageUnwind->FindLinkSymbol(symbol, true))
                return record(result, "libc.so:unwind", false);
    }
    return 0;
}

ElfImage::LinkSymbol GlobalSymbol(const LibraryNamespace& scope, std::string_view symbol)
{
    for (const auto* image : scope.globalImages) {
        if (const auto result = image->image.FindLinkSymbol(symbol))
            return result;
    }
    return 0;
}

}
