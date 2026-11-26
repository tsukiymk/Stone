#include "abi/AndroidLibraryImports.h"
#include "stub-libc.h"
#include <algorithm>
#include <cstdlib>

namespace stone::windows {
uintptr_t Libstdcxx(std::string_view name)
{
    if (name == "_Znwm" || name == "_Znam")
        return Import(
            name, +[](size_t size) {
                if (void* memory = AndroidMalloc(std::max<size_t>(1, size)))
                    return memory;
                // Windows C++ exceptions cannot unwind through an ELF image.
                std::abort();
            });
    if (name == "_ZdlPv" || name == "_ZdaPv")
        return Import(name, &AndroidFree);
    return 0;
}
}
