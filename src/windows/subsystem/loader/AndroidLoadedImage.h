#pragma once

#include "loader/ElfImage.h"
#include "cpu/arm64/cache/Arm64BlockCache.h"

namespace stone::windows::android_libraries {
// Android loading coordinates the selected execution backend with ELF ownership.
// A failed derived construction unwinds ElfImage; destruction detaches before unmapping.
class AndroidLoadedImage final : public ElfImage {
public:
    explicit AndroidLoadedImage(std::span<const uint8_t> bytes, Machine machine = Machine::X64, bool dynamicTls = false,
                                bool compiler = false)
        : ElfImage(bytes, machine, dynamicTls, compiler)
    {
        if (machine == Machine::Arm64)
            RegisterArm64BlockCacheImage(Identity(), bytes);
    }

    ~AndroidLoadedImage()
    {
        if (Architecture() == Machine::Arm64)
            UnregisterArm64BlockCacheImage(Identity());
    }
};
}
