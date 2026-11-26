#pragma once

#include <windows.h>
#include <cstdint>

namespace stone::windows {
// Caller must first prove nine readable bytes of owned guest code at RIP.
bool RecoverAndroidFsLoad(CONTEXT& context, uintptr_t faultAddress);
}
