#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace stone::windows {
// LP64 Android's signal frame contains an aligned AArch64 machine context.
struct Arm64SignalContext {
    uint64_t flags = 0, link = 0, altStack = 0;
    uint32_t stackFlags = 2, stackPadding = 0; // SS_DISABLE.
    uint64_t stackSize = 0, mask = 0;
    std::array<uint8_t, 120> maskPadding{};
    uint64_t machineContextPadding = 0, fault = 0;
    std::array<uint64_t, 31> regs{};
    uint64_t sp = 0, pc = 0, pstate = 0, fpContextPadding = 0;

    struct alignas(16) {
        uint32_t magic = 0x46508001, size = 528; // FPSIMD_MAGIC.
        uint32_t fpsr = 0, fpcr = 0;
        std::array<std::array<uint64_t, 2>, 32> regs{};
    } fp;

    std::array<uint8_t, 4096 - 528> reserved{};
};

static_assert(offsetof(Arm64SignalContext, fault) == 176);
static_assert(offsetof(Arm64SignalContext, sp) == 432);
static_assert(offsetof(Arm64SignalContext, fp) == 464);
static_assert(sizeof(Arm64SignalContext) == 4560);

}
