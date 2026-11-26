#pragma once

#include "loader/AndroidLibraryState.h"

namespace stone::windows::android_libraries {
template <typename Lookup> auto FindDependency(const Library& lib, Lookup lookup)
{
    using Result = decltype(lookup(lib));
    std::vector<const Library*> queue(lib.dependencies.begin(), lib.dependencies.end());
    std::unordered_set<const Library*> seen;
    for (size_t i = 0; i < queue.size(); ++i) {
        const auto* dependency = queue[i];
        if (!seen.insert(dependency).second)
            continue;
        if (auto result = lookup(*dependency); result)
            return result;
        queue.insert(queue.end(), dependency->dependencies.begin(), dependency->dependencies.end());
    }
    return Result{};
}

uintptr_t LookupSystemSymbol(const SystemLibrary&, std::string_view symbol);
ElfImage::LinkSymbol DependencySymbol(const Library&, std::string_view, const ElfImage* imageUnwind = nullptr,
                                      AndroidElfLinkReport::Binding* binding = nullptr);
ElfImage::LinkSymbol GlobalSymbol(const LibraryNamespace&, std::string_view symbol);
}
