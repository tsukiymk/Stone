#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>

namespace stone::windows {
// Android x86_64 public siginfo_t/ucontext_t layouts. Windows CONTEXT never
// crosses the guest boundary, and Linux segment writes cannot change the TEB.
struct AndroidSignalInfo {
    int32_t number, error, code, padding;
    std::array<uint64_t, 14> detail;
};

struct AndroidSignalContext {
    uint64_t flags;
    void* link;

    struct Stack {
        void* address;
        int32_t flags, padding;
        uint64_t size;
    } stack;

    struct Machine {
        std::array<uint64_t, 23> registers;
        void* floating;
        std::array<uint64_t, 8> reserved;
    } machine;

    uint64_t mask;
    std::array<uint8_t, 512> floating;
};

static_assert(sizeof(AndroidSignalInfo) == 128);
static_assert(offsetof(AndroidSignalContext, machine) == 40 && offsetof(AndroidSignalContext, mask) == 296);
static_assert(offsetof(AndroidSignalContext, floating) == 304 && sizeof(AndroidSignalContext) == 816);
void CaptureAndroidSignalContext(AndroidSignalContext& output, const CONTEXT& source, uint64_t mask);
void RestoreAndroidSignalContext(CONTEXT& output, const AndroidSignalContext& source);
bool DeliverAndroidHardwareSignal(const EXCEPTION_RECORD& fault, CONTEXT& context);
}
